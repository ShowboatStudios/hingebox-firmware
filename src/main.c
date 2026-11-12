/* DUMBMIDI: a MIDI controller for the M-VAVE SMK-37 (Pro, Elite, MKE-P37) and the FM-1.
 *
 *   keys       notes out (the SMK's with velocity); the big screen names the chord they make
 *   pads       their own notes (SMK: 16 velocity pads, RGB LEDs)
 *   knobs      four assignable CCs; the other encoders navigate
 *   faders     four CCs (SMK)
 *   buttons    CC / note / program, momentary or toggle
 *   wheels     pitch bend and a CC (SMK); the pedal jack a CC
 *   GLO        the MAP page: every control, its message, number, channel and mode, edited on screen
 *   SAVE       store the mappings in flash
 *   USB/BLE MIDI in, channel 16: the pad colours (see README)
 *
 * MIDI goes out over USB and BLE MIDI at once (midi_send does both). */
#include "fm1.h"
#include "chord.h"
#include "mapping.h"
#include "bop.h"

/* ---- the look: near-black, one accent, colour by pitch class */
#define C_BG RGB(5, 5, 6) /* Hingebox: the pad's OLED black */
#define C_CARD RGB(24, 24, 26) /* neutral greys, as the app (Fable pass 2) */
#define C_LINE RGB(44, 44, 46)
#define C_TXT RGB(237, 237, 232)
#define C_MUT RGB(125, 125, 120)
#define C_BLUE RGB(47, 107, 255) /* Hingebox blue: the tempo, as on the deck */
#define C_ACC RGB(255, 92, 31) /* Hingebox orange */
#define C_SEL RGB(64, 26, 10)

#if !BOARD_SMK37
/* the SMK's pads as the FM-1's white keys (F3 G3 A3 B3 C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5) */
static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
#endif

/* ---- the buttons a mapping can send from (the rest have their own jobs: banks, pages, ARP,
 * CHORD, SCALE, PATCH, GLOBE, SAVE) */
#if BOARD_SMK37
#define N_BTN_UI 6
static const uint8_t BTN_ID[N_BTN_UI] = {BTN_ENV, BTN_LFO, BTN_FX, BTN_SEQ, BTN_PLAY, BTN_REC};
static const char* const BTN_NAME[N_BTN_UI] = {"PARA", "NOTE REP", "FX", "SEQ", "PLAY", "REC"};
#else
#define N_BTN_UI 6
static const uint8_t BTN_ID[N_BTN_UI] = {BTN_FX, BTN_ENV, BTN_LFO, BTN_SEQ, BTN_PLAY, BTN_REC};
static const char* const BTN_NAME[N_BTN_UI] = {"FX", "ENV", "LFO", "SEQ", "PLAY", "REC"};
#endif

/* ---- saved settings */
#define CFG_MAGIC 0x31444D42u /* "BMD1" */
#define CFG_VERSION 4u
typedef struct {
  uint32_t magic;
  uint16_t version;
  uint8_t board;
  int8_t transpose;
  uint8_t pad_bright;
  uint8_t knob_bank, fader_bank;
  uint8_t ble_on;
  uint8_t arp_on, arp_rate, arp_pat, arp_oct, arp_bpm;
  uint8_t chord_on, chord_n;
  int8_t chord_iv[8];
  uint8_t scale_on, scale_root, scale_type;
  uint8_t split_on, split_note, split_lo_ch, split_hi_ch;
  uint16_t pad_rgb[16];
  map_t map[N_MAP];
} cfg_t;

static cfg_t cfg;
static uint8_t cfg_dirty;
static uint32_t cfg_dirty_at;
static uint32_t saved_at;
static uint8_t bt_ready;

/* ---- pages */
enum { PG_MAIN,
       PG_ARP,
       PG_CHORD,
       PG_SCALE,
       PG_SPLIT,
       PG_MAP,
       PG_EDIT };
static uint8_t page = PG_MAIN;

static void send_msg(const uint8_t m[3]);
static void send_map(const map_t* m, uint8_t value, int on);

/* ---- musical helpers: scale lock, chord memory, arpeggiator */
enum { SC_MAJOR,
       SC_MINOR,
       SC_DORIAN,
       SC_PHRYGIAN,
       SC_LYDIAN,
       SC_MIXO,
       SC_LOCRIAN,
       SC_PENTA_MAJ,
       SC_PENTA_MIN,
       SC_BLUES,
       SC_COUNT };
static const uint16_t SCALE_MASK[SC_COUNT] = {
    0x0AB5,
    0x05AD,
    0x06AD,
    0x05AB,
    0x0AD5,
    0x06B5,
    0x056B,
    0x0295,
    0x04A9,
    0x04E9,
};
static const char* const SCALE_NAME[SC_COUNT] = {"MAJOR", "MINOR", "DORIAN", "PHRYG", "LYDIAN",
                                                 "MIXO", "LOCRIAN", "PENTA MAJ", "PENTA MIN", "BLUES"};
static const uint8_t ARP_SUB[5] = {1, 2, 4, 3, 6}; /* steps a beat: 1/4 1/8 1/16 1/8T 1/16T */
static const char* const ARP_RATE_NAME[5] = {"1/4", "1/8", "1/16", "1/8T", "1/16T"};
static const char* const ARP_PAT_NAME[4] = {"UP", "DOWN", "UPDOWN", "RANDOM"};

#define ARP_MAX 48
static int16_t arp_src[128];      /* counts of the notes the keys hold (after chord and scale) */
static uint8_t arp_list[ARP_MAX]; /* the same, sorted and octave-expanded */
static int arp_n;
static uint8_t arp_dirty = 1;
static int arp_pos, arp_dir = 1;
static int8_t arp_playing = -1;
static uint32_t arp_next, arp_off_at;
static uint8_t arp_vel = 100;
static uint8_t arp_beat_on;
static uint32_t arp_beat_next, arp_beat_until;
static uint32_t tap_last, tap_sum;
static uint8_t tap_n;

static int8_t key_gen[37][8]; /* notes a key's press generated (chord); -1 = none */
static uint8_t key_gen_n[37];
static uint8_t key_ch[37]; /* the channel that key's notes went out on (split zones) */
static uint8_t arp_ch;

static int scale_note(int n) {
  if (!cfg.scale_on || cfg.scale_type >= SC_COUNT) {
    return n;
  }
  int pc = (n - cfg.scale_root) % 12;
  if (pc < 0) {
    pc += 12;
  }
  for (int d = 0; d <= 6; d++) {
    int up = pc + d;
    if (up >= 12) {
      up -= 12;
    }
    if (up >= 0 && (SCALE_MASK[cfg.scale_type] >> up & 1u) && n + d <= 127) {
      return n + d;
    }
    int dn = pc - d;
    if (dn < 0) {
      dn += 12;
    }
    if (d && dn >= 0 && (SCALE_MASK[cfg.scale_type] >> dn & 1u) && n - d >= 0) {
      return n - d;
    }
  }
  return n;
}

/* the name of the memorized chord shape: the common sets, else CUSTOM */
static const char* chord_shape_name(void) {
  static const struct {
    const char* name;
    int8_t iv[4];
  } NAMES[] = {{"MAJ", {0, 4, 7, -1}},
               {"MIN", {0, 3, 7, -1}},
               {"MAJ7", {0, 4, 7, 11}},
               {"7", {0, 4, 7, 10}},
               {"MIN7", {0, 3, 7, 10}},
               {"SUS2", {0, 2, 7, -1}},
               {"SUS4", {0, 5, 7, -1}},
               {"DIM", {0, 3, 6, -1}},
               {"AUG", {0, 4, 8, -1}}};
  for (unsigned k = 0; k < sizeof NAMES / sizeof NAMES[0]; k++) {
    int n = 0;
    while (n < 4 && NAMES[k].iv[n] >= 0) {
      n++;
    }
    if (n != cfg.chord_n) {
      continue;
    }
    int same = 1;
    for (int i = 0; i < n; i++) {
      same &= NAMES[k].iv[i] == cfg.chord_iv[i];
    }
    if (same) {
      return NAMES[k].name;
    }
  }
  return "CUSTOM";
}

static void arp_collect(void) {
  arp_n = 0;
  for (int n = 0; n < 128; n++) {
    if (!arp_src[n]) {
      continue;
    }
    for (int o = 0; o < cfg.arp_oct; o++) {
      int v = n + 12 * o;
      if (v > 127) {
        break;
      }
      if (arp_n < ARP_MAX) {
        arp_list[arp_n++] = (uint8_t)v;
      }
    }
  }
  if (arp_pos >= arp_n) {
    arp_pos = 0;
  }
  arp_dirty = 0;
}
static void arp_src_add(int n) {
  if (n >= 0 && n <= 127 && arp_src[n] < 255) {
    arp_src[n]++;
    arp_dirty = 1;
  }
}
static void arp_src_del(int n) {
  if (n >= 0 && n <= 127 && arp_src[n]) {
    arp_src[n]--;
    arp_dirty = 1;
  }
}
static void send_note(uint8_t ch, int note, uint8_t vel) {
  uint8_t msg[3] = {(uint8_t)((vel ? 0x90u : 0x80u) | (ch & 0x0Fu)), (uint8_t)(note & 0x7Fu), vel};
  send_msg(msg);
}
static void arp_note_off(void) {
  if (arp_playing >= 0) {
    send_note(arp_ch, arp_playing, 0);
    arp_playing = -1;
  }
}
static void arp_stop(void) {
  arp_note_off();
  arp_next = 0;
}
static uint32_t arp_step_ms(void) {
  uint32_t bpm = cfg.arp_bpm < 40 ? 40 : cfg.arp_bpm > 240 ? 240
                                                           : cfg.arp_bpm;
  uint32_t sub = ARP_SUB[cfg.arp_rate < 5 ? cfg.arp_rate : 0];
  return 60000u / (bpm * sub);
}
static void arp_tick(uint32_t now) {
  if (!cfg.arp_on) {
    arp_stop();
    return;
  }
  if (arp_dirty) {
    arp_collect();
  }
  if (!arp_n) {
    arp_stop();
    return;
  }
  if (arp_playing >= 0 && (int32_t)(now - arp_off_at) >= 0) {
    arp_note_off();
  }
  if (arp_next && (int32_t)(now - arp_next) < 0) {
    return;
  }
  if (arp_n <= 0) {
    return;
  }
  int idx;
  switch (cfg.arp_pat) {
    case 1: /* down */
      arp_pos = (arp_pos - 1 + arp_n) % arp_n;
      idx = arp_pos;
      break;
    case 2: /* up/down */
      if (arp_pos + arp_dir >= arp_n || arp_pos + arp_dir < 0) {
        arp_dir = -arp_dir;
      }
      arp_pos += arp_dir;
      if (arp_pos < 0) {
        arp_pos = 0;
      }
      idx = arp_pos;
      break;
    case 3:
      idx = (int)(random32() % (uint32_t)arp_n);
      break;
    default:
      idx = arp_pos;
      arp_pos = (arp_pos + 1) % arp_n;
      break;
  }
  arp_note_off();
  send_note(arp_ch, arp_list[idx], arp_vel);
  arp_playing = (int8_t)arp_list[idx];
  uint32_t step = arp_step_ms();
  arp_off_at = now + step * 70u / 100u;
  arp_next = now + step;
}

static void ui_note_dirty(void);
static void ui_full(void);

/* ---- what the screen shows */
static uint8_t refs[128];
static uint8_t last_vel;
static uint16_t held_pcs;
static int bass_pc = -1;
static char chord_txt[16];
static char notes_txt[64];
static uint8_t d_chord = 1, d_notes = 1, d_pills = 1, d_foot = 1, d_head = 1, d_page = 1, d_map = 1, d_edit = 1,
               d_set = 1;
/* the knob overlay (screen v3): a control's name, its value and a bar for 1.5 s. Local controls name themselves
 * (ctl_name, set just before they send); with Hingebox connected the app names them (CC 107) instead */
static char ov_name[17], ov_sub[16];
static uint8_t ov_val, ov_pct, d_ov = 1;
static uint16_t ov_col = C_ACC;
static uint32_t ov_at;
static const char* ctl_name;
static uint32_t act_at; /* the last thing played or turned (Bop gets sleepy a minute after) */
static int ov_on(void) { return ov_at && millis() - ov_at < 1500u; }
static int hb_active(void);

static void note_add(int n, int v) {
  if (n < 0 || n > 127) {
    return;
  }
  if (!refs[n]++) {
    last_vel = (uint8_t)(v > 127 ? 127 : v);
  }
  act_at = millis();
  ui_note_dirty();
}

static void note_del(int n) {
  if (n < 0 || n > 127 || !refs[n]) {
    return;
  }
  refs[n]--;
  ui_note_dirty();
}

/* ---- MIDI out */
static void send_msg(const uint8_t m[3]) {
  midi_send(m[0], m[1], m[2]);
  act_at = millis();
  if ((m[0] & 0xF0u) == 0xB0u && ctl_name && !hb_active()) { /* a knob or fader moved: its name, CC and value */
    int i = 0;
    for (; ctl_name[i] && i < 16; i++) {
      ov_name[i] = ctl_name[i];
    }
    ov_name[i] = 0;
    char b[12];
    int k = 0;
    ov_sub[k++] = 'c', ov_sub[k++] = 'c', ov_sub[k++] = ' ';
    format_int(b, m[1]);
    for (int q = 0; b[q] && k < 7; q++) {
      ov_sub[k++] = b[q];
    }
    ov_sub[k++] = ' ', ov_sub[k++] = 'c', ov_sub[k++] = 'h', ov_sub[k++] = ' ';
    format_int(b, (m[0] & 0x0Fu) + 1);
    for (int q = 0; b[q] && k < 15; q++) {
      ov_sub[k++] = b[q];
    }
    ov_sub[k] = 0;
    ov_val = m[2];
    ov_pct = 0;
    ov_col = C_ACC;
    ov_at = millis() ? millis() : 1;
    d_ov = 1;
  }
}

static void send_map(const map_t* m, uint8_t value, int on) {
  uint8_t msg[3];
  if (map_message(m, value, on, msg)) {
    send_msg(msg);
  }
}

/* ---- the keyboards, as the app sees them */
static int8_t pad_note[16];
static uint8_t pad_on[16];
static uint8_t knob_val[N_KNOB];
static uint8_t fader_prev[4];
static int wheel_prev[2] = {-1, -1};
static int pedal_prev = -1; /* -1: not read yet */
static int pedal_cand, pedal_stable;
static uint8_t btn_tog; /* bit i: the button's toggle state */

static int accel(int32_t detents, uint32_t* last) {
  if (!detents) {
    return 0;
  }
  uint32_t now = millis(), dt = now - *last;
  *last = now;
  return (int)(detents * (dt < 25 ? 4 : dt < 60 ? 2
                                                : 1));
}

/* ---- colours */
static inline uint16_t blend565(uint16_t a, uint16_t b, int t) { /* t 0..256 toward b */
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (br - ar) * t / 256, g = ag + (bg - ag) * t / 256, bl = ab + (bb - ab) * t / 256;
  return (uint16_t)(r << 11 | g << 5 | bl);
}

static uint16_t hsv565(uint32_t h, uint32_t s, uint32_t v) { /* h 0..255, s/v 0..255 */
  uint32_t sec = h / 43u, f = (h - sec * 43u) * 6u;
  uint32_t p = v * (255u - s) / 255u, q = v * (255u - s * f / 255u) / 255u, t = v * (255u - s * (255u - f) / 255u) / 255u;
  uint32_t r, g, b;
  switch (sec) {
    case 0:
      r = v, g = t, b = p;
      break;
    case 1:
      r = q, g = v, b = p;
      break;
    case 2:
      r = p, g = v, b = t;
      break;
    case 3:
      r = p, g = q, b = v;
      break;
    case 4:
      r = t, g = p, b = v;
      break;
    default:
      r = v, g = p, b = q;
      break;
  }
  return RGB(r, g, b);
}

static uint16_t bright565(uint16_t c, uint32_t factor) { /* factor 0..255 */
  uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
  r = r * factor / 255u, g = g * factor / 255u, b = b * factor / 255u;
  return (uint16_t)(r << 11 | g << 5 | b);
}

/* ---- Hingebox: pad colours a host streams (a step grid's playhead changes them every step) live in RAM only.
 * dumbmidi saved them into the settings, which wrote flash each time the stream paused. CC17 = 0 hands the pads
 * back to the saved colours. */
static uint16_t host_rgb[16];
static uint8_t host_on;

static uint16_t pad_colour(int p) { return host_on ? host_rgb[p] : cfg.pad_rgb[p]; }

/* ---- Hingebox's screen (2026-11-12): what the app says, on channel 16 CCs 107-119 (README: host protocol).
 * 119 any: hello; we answer CC 119 = protocol 1, 118 = what we can do (1 pad lights, 2 screen, 4 buttons), 117 =
 * version. 113: the playhead step 0-15 (127 stopped). 111/112: tempo high/low 7 bits. 110 slot (0 groove, 1 track,
 * 2 knob label), 109 a character (ASCII 32-126), 108 show it. 107: the knob's value 0-127 (with slot 2) for 1.5 s. */
#define HB_VERSION 2 /* 0.2 */
static char hb_text[3][17];
static char hb_buf[17];
static uint8_t hb_slot, hb_len;
/* additive (capability 8): CC 104 the track's deck colour, CC 105 a moment (1 fill, 2 drop, 3 cheer), CC 106 the
 * knob's deck colour (0-7: kick snare clap hat open tom perc bass; 127 default orange) */
static uint8_t hb_track_col = 255, hb_knob_col = 255, hb_moment;
static uint32_t hb_moment_at;
static uint8_t host_vel[16]; /* the hues the pads were sent, for the screen's grid in the deck's colours */
static int hb_step = -1;
static uint16_t hb_bpm;
static uint32_t hb_seen;
static uint8_t d_host = 1, d_bop = 1, d_step = 1;
/* the six mapped buttons' lights as Hingebox says (CC 41-46 on channel 16: SEQ PLAY lit while playing...) */
static uint8_t host_btn[N_BTN_UI];
static uint32_t splash_until;

/* lowercase, in the 8x16 bitmap mono (the app's labels are lowercase mono) */
static int mono(int x, int y, const char* s, uint16_t c) {
  char b[40];
  int i = 0;
  for (; s[i] && i < 39; i++) {
    b[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
  }
  b[i] = 0;
  return text(x, y, b, c, FONT_SMALL);
}
static void mono_c(int y, const char* s, uint16_t c) {
  mono((SCREEN_W - text_width(s, FONT_SMALL)) / 2, y, s, c);
}

static uint16_t deck_colour(uint8_t i);
static uint8_t knob_colour_of(const char* name);
static int hb_active(void) { return hb_seen && millis() - hb_seen < 4000u; }

static void hb_cc(uint8_t cc, uint8_t v) {
  hb_seen = millis();
  if (!hb_seen) {
    hb_seen = 1;
  }
  switch (cc) {
    case 119:
      midi_send(0xBF, 119, 1);
      midi_send(0xBF, 118, 15); /* 1 pads, 2 screen, 4 buttons, 8 screen extras (CC 104-106) */
      midi_send(0xBF, 117, HB_VERSION);
      d_host = 1;
      break;
    case 113:
      if ((v < 16 ? (int)v : -1) != hb_step) {
        int was = hb_step;
        hb_step = v < 16 ? (int)v : -1;
        d_step = 1;
        if (was < 0 || hb_step < 0 || (hb_step % 4) == 0 || (was % 4) == 0) {
          d_bop = 1;
        }
      }
      break;
    case 111:
      hb_bpm = (uint16_t)((v << 7) | (hb_bpm & 0x7Fu));
      d_host = 1;
      break;
    case 112:
      hb_bpm = (uint16_t)((hb_bpm & ~0x7Fu) | v);
      d_host = 1;
      break;
    case 110:
      hb_slot = v < 3 ? v : 0;
      hb_len = 0;
      hb_buf[0] = 0;
      break;
    case 109:
      if (hb_len < 16 && v >= 32 && v < 127) {
        hb_buf[hb_len++] = (char)v;
        hb_buf[hb_len] = 0;
      }
      break;
    case 108:
      for (int i = 0; i < 17; i++) {
        hb_text[hb_slot][i] = hb_buf[i];
      }
      if (hb_slot == 2) {
        hb_knob_col = 255; /* a new knob: its colour comes next (CC 106) or from its name */
      }
      d_host = 1;
      break;
    case 107: /* the knob in slot 2: its name, value and bar, in its deck colour */
      for (int i = 0; i < 17; i++) {
        ov_name[i] = hb_text[2][i];
      }
      if (!ov_name[0]) {
        ov_name[0] = 'k', ov_name[1] = 'n', ov_name[2] = 'o', ov_name[3] = 'b', ov_name[4] = 0;
      }
      ov_sub[0] = 0;
      ov_val = v;
      ov_pct = 1;
      if (hb_knob_col >= 8) { /* no CC 106 for this knob: its deck colour by name, else orange */
        hb_knob_col = knob_colour_of(ov_name);
      }
      ov_col = deck_colour(hb_knob_col);
      ov_at = millis() ? millis() : 1;
      d_ov = 1;
      break;
    case 106:
      hb_knob_col = v < 8 ? v : 0; /* 127: the default, orange (the kick's) */
      break;
    case 105:
      hb_moment = v < 4 ? v : 0;
      hb_moment_at = millis();
      break;
    case 104:
      hb_track_col = v < 8 ? v : 255;
      d_host = 1;
      break;
    default:
      break;
  }
}

static void refresh_pads(void) {
  if (!pad_count()) {
    return;
  }
  if (page == PG_ARP) { /* only pad 1, as the beat lamp / tap tempo */
    for (int p = 0; p < 16; p++) {
      led_pad(p, 0);
    }
    led_pad(0, arp_beat_on ? C_ACC : RGB(24, 30, 36));
    return;
  }
  for (int p = 0; p < 16; p++) {
    uint16_t c = pad_colour(p) ? bright565(pad_colour(p), cfg.pad_bright) : 0;
    if (pad_held(p)) {
      c = c ? blend565(c, WHITE, 150) : RGB(180, 190, 205);
    }
    led_pad(p, c);
  }
}

static void refresh_key_leds(void) {
#if !BOARD_SMK37
  uint32_t lit = 0;
  for (int p = 0; p < 16; p++) {
    if (pad_colour(p)) {
      lit |= 1u << WHITE_KEY[p];
    }
  }
  for (int k = 0; k < key_total(); k++) {
    if (key_all_held(k)) {
      lit |= 1u << k;
    }
  }
  static uint32_t was;
  if (lit != was) {
    was = lit;
    for (int k = 0; k < key_total(); k++) {
      led_key(k, (int)(lit >> k & 1u));
    }
  }
#endif
  refresh_pads();
}

/* ---- the note/chord display's text */
static void build_note_text(void) {
  held_pcs = 0;
  bass_pc = -1;
  int lo = 128;
  for (int n = 0; n < 128; n++) {
    if (refs[n]) {
      held_pcs |= (uint16_t)(1u << (n % 12));
      if (n < lo) {
        lo = n;
      }
    }
  }
  if (lo < 128) {
    bass_pc = lo % 12;
  }
  chord_name(held_pcs, bass_pc, chord_txt, sizeof chord_txt);
  int i = 0, shown = 0, total = chord_count(held_pcs);
  for (int n = 0; n < 128 && i < (int)sizeof notes_txt - 5; n++) {
    if (!refs[n]) {
      continue;
    }
    if (shown >= 6) {
      break;
    }
    if (shown) {
      notes_txt[i++] = ' ';
    }
    for (int k = 0; NOTE_NAME[n % 12][k] && i < (int)sizeof notes_txt - 4; k++) {
      notes_txt[i++] = NOTE_NAME[n % 12][k];
    }
    int oct = n / 12 - 1;
    char b[12] = {0};
    format_int(b, oct);
    /* the octave: its sign (the big chord line already says the rest) */
    for (int k = 0; b[k] && i < (int)sizeof notes_txt - 2; k++) {
      notes_txt[i++] = b[k];
    }
    shown++;
  }
  if (total > shown) {
    notes_txt[i++] = ' ';
    notes_txt[i++] = '+';
    char b[12] = {0};
    format_int(b, total - shown);
    for (int k = 0; b[k] && i < (int)sizeof notes_txt - 1; k++) {
      notes_txt[i++] = b[k];
    }
  }
  notes_txt[i] = 0;
}

/* ---- the screen (screen v3, 2026-11-12; docs/SCREEN-REVIEW.md). One grid on every page: 12 px side margins, a
 * 24 px status bar over a 1 px hairline, 16 px mono rows. Lowercase 8x16 mono for every label; Inter Tight only for
 * the wordmark and the big chord. One accent (orange) for what's on, tempo in blue, the deck's track colours. */
#define MX 12 /* the side margin */
#define BAR_H 24
#define C_GHOST RGB(58, 58, 60) /* the last chord, after the keys let go */
#define C_KEYW RGB(74, 74, 76) /* the key strip's white keys */
#define C_KEYB RGB(22, 22, 24) /* and black keys */

/* the deck's track colours (Hingebox UI/Theme.swift, aluminium look): kick, snare, clap, hat, open, tom, perc, bass */
static const uint16_t DECK[8] = {RGB(255, 92, 31), RGB(46, 107, 255), RGB(255, 112, 173), RGB(255, 199, 26),
                                 RGB(46, 196, 107), RGB(148, 128, 255), RGB(237, 237, 232), RGB(46, 196, 107)};
/* the hues (pad velocities) Hingebox sends for them (Stage/Keyboards.swift trackHue), and its fill / drop pads */
static const uint8_t DECK_HUE[8] = {10, 80, 115, 22, 48, 95, 64, 48};

/* a pad's colour on the screen: a track's hue as the deck's colour, anything else round the colour wheel */
static uint16_t hue_colour(uint8_t v) {
  for (int i = 0; i < 8; i++) {
    if (DECK_HUE[i] == v) {
      return DECK[i];
    }
  }
  if (v == 20) {
    return RGB(255, 226, 140); /* fill */
  }
  if (v == 3) {
    return RGB(255, 59, 46); /* drop */
  }
  return hsv565((uint32_t)v * 2u, 190, 235);
}
static uint16_t deck_colour(uint8_t i) { return i < 8 ? DECK[i] : C_ACC; }

/* a deck knob's colour by its name, for a host that names a knob without CC 106 (Hingebox Stage/ControlMap.swift
 * `accent`, in the track colours' numbering: blue 1, green 4, orange 0, white 6, lilac 5, pink 2) */
static uint8_t knob_colour_of(const char* name) {
  static const struct {
    const char* name;
    uint8_t col;
  } K[] = {{"tempo", 1}, {"drive", 1},  {"loops", 1},  {"swing", 4},    {"delay", 4},   {"wet", 4},
           {"drums", 0}, {"volume", 0}, {"synth", 0},  {"beat", 0},     {"pump", 6},    {"crush", 6},
           {"reverb", 6}, {"bass", 6},  {"pad", 5},    {"hinge fx", 5}, {"not set", 2}};
  for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
    int k = 0;
    while (K[i].name[k] && (name[k] | 32) == K[i].name[k]) {
      k++;
    }
    if (!K[i].name[k] && !name[k]) {
      return K[i].col;
    }
  }
  return 255;
}

static int str_len(const char* s) {
  int n = 0;
  while (s[n]) {
    n++;
  }
  return n;
}
/* s, lowercase, into b (n bytes) */
static void lower(char* b, const char* s, int n) {
  int i = 0;
  for (; s[i] && i < n - 1; i++) {
    b[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
  }
  b[i] = 0;
}
/* right-aligned at xr; returns the left edge */
static int mono_r(int xr, int y, const char* s, uint16_t c) {
  int x = xr - text_width(s, FONT_SMALL);
  mono(x, y, s, c);
  return x;
}
/* the mono at 2x (16 x 32), crisp: each pixel a 2x2 block */
static int mono2(int x, int y, const char* s, uint16_t c) {
  char b[24];
  lower(b, s, sizeof b);
  return text_scale(x, y, b, c, FONT_SMALL, 2);
}
static int mono2_w(const char* s) { return text_width(s, FONT_SMALL) * 2; }
static void hairline(int y) { rect(MX, y, SCREEN_W - 2 * MX, 1, C_LINE); }

/* Hingebox is connected: two links of a chain, 15 x 7 */
static void draw_link(int x, int y, uint16_t c) {
  frame(x, y, 9, 7, c);
  frame(x + 6, y, 9, 7, c);
  rect(x + 7, y + 3, 1, 1, C_BG);
}

/* a page title: the name, an on/off pill (on < 0: none), and on the playing pages five dots for where you are */
static int page_index(void);
static void draw_title(const char* title, int on, const char* right) {
  rect(0, 0, SCREEN_W, BAR_H, C_BG);
  int x = mono(MX, 4, title, C_TXT);
  if (on >= 0) {
    x += 8;
    if (on) {
      bop_round(x, 4, 32, 16, 7, C_ACC, C_ACC, 1);
      mono(x + 8, 4, "on", C_BG);
    } else {
      bop_round(x, 4, 32, 16, 7, C_BG, C_LINE, 1);
      mono(x + 4, 4, "off", C_MUT);
    }
  }
  int pi = page_index();
  if (pi >= 0) {
    for (int i = 0; i < 5; i++) {
      int dx = SCREEN_W - MX - 3 - (4 - i) * 12;
      bop_disc(dx, 11, i == pi ? 3 : 2, i == pi ? C_ACC : C_LINE);
    }
  } else if (right) {
    mono_r(SCREEN_W - MX, 4, right, C_MUT);
  }
  hairline(BAR_H - 1);
}

/* a knob's row on the settings pages: its keycap (k1..k4), what it sets, the value at 2x */
#define ROW_Y0 28
#define ROW_H 34
static void draw_knob_row(int i, const char* label, const char* value, uint16_t vc) {
  int y = ROW_Y0 + i * ROW_H;
  rect(0, y, SCREEN_W, ROW_H, C_BG);
  char k[3] = {'k', (char)('1' + i), 0};
  bop_round(MX, y + 7, 22, 18, 4, C_BG, C_LINE, 1);
  mono(MX + 3, y + 8, k, C_MUT);
  mono(MX + 30, y + 8, label, C_MUT);
  int vw = mono2_w(value);
  if (vw <= 116) {
    mono2(SCREEN_W - MX - vw, y, value, vc);
  } else {
    mono_r(SCREEN_W - MX, y + 8, value, vc);
  }
  if (i < 3) {
    rect(MX, y + ROW_H - 1, SCREEN_W - 2 * MX, 1, C_CARD);
  }
}

/* the footer: a hint of which button does what, or "saved" for a moment */
static void draw_hint(const char* hint) {
  rect(0, 220, SCREEN_W, 20, C_BG);
  if (saved_at && millis() - saved_at < 1500u) {
    mono_c(222, "saved", C_ACC);
  } else if (hint) {
    mono_c(222, hint, C_MUT);
  }
}

/* ---- a piano strip: notes lo..hi across (x, y, w, h), from white key to white key. A note is lit (in lit_c) when
 * held[] counts it or its pitch class is in lit_pcs; dot_pcs get a dot; split (>= 0) gets an orange edge; middle C a
 * mark below. Notes held outside lo..hi show as an arrow at that end. */
static int is_black(int n) { return (0x54A >> (n % 12)) & 1; }
static void draw_keys(int x, int y, int w, int h, int lo, int hi, const uint8_t* held, uint16_t lit_pcs,
                      uint16_t lit_c, uint16_t dot_pcs, int split) {
  rect(x - 10, y - 2, w + 20, h + 8, C_BG);
  if (is_black(lo)) {
    lo--;
  }
  if (is_black(hi)) {
    hi++;
  }
  int nw = 0;
  for (int n = lo; n <= hi; n++) {
    nw += !is_black(n);
  }
  if (nw <= 0) {
    return;
  }
  int ww = w / nw, x0 = x + (w - ww * nw) / 2;
  int bw = ww * 3 / 5, bh = h * 3 / 5;
  if (bw < 3) {
    bw = 3;
  }
  int dot = ww >= 12 ? 3 : 2;
  /* white keys */
  int wi = 0;
  for (int n = lo; n <= hi; n++) {
    if (is_black(n)) {
      continue;
    }
    int lit = (held && held[n]) || (lit_pcs >> (n % 12) & 1u);
    int kx = x0 + wi * ww;
    rect(kx, y, ww - 1, h, lit ? lit_c : C_KEYW);
    if (dot_pcs >> (n % 12) & 1u) {
      rect(kx + (ww - 1 - dot) / 2, y + h - dot - 3, dot, dot, lit ? C_BG : C_TXT);
    }
    if (n == 60) { /* middle C */
      rect(kx + (ww - 1 - 3) / 2, y + h + 2, 3, 2, C_MUT);
    }
    if (n == split) {
      rect(kx - 1, y - 2, 2, h + 4, C_ACC);
    }
    wi++;
  }
  /* black keys, on the gaps, with a black edge so they read on the white keys */
  wi = 0;
  for (int n = lo; n <= hi; n++) {
    if (!is_black(n)) {
      wi++;
      continue;
    }
    int lit = (held && held[n]) || (lit_pcs >> (n % 12) & 1u);
    int gx = x0 + wi * ww - 1; /* the gap between the white keys */
    int kx = gx - bw / 2;
    rect(kx - 1, y, bw + 2, bh + 1, C_BG);
    rect(kx, y, bw, bh, lit ? lit_c : C_KEYB);
    if (dot_pcs >> (n % 12) & 1u) {
      rect(kx + (bw - 2) / 2, y + bh - 4, 2, 2, lit ? C_BG : C_TXT);
    }
    if (n == split) {
      rect(gx - bw / 2 - 1, y - 2, 2, h + 4, C_ACC);
    }
  }
  /* held notes past either end */
  if (held) {
    int below = 0, above = 0;
    for (int n = 0; n < 128; n++) {
      if (held[n]) {
        below |= n < lo;
        above |= n > hi;
      }
    }
    for (int i = 0; i < 4; i++) {
      if (below) {
        rect(x0 - 4 - i, y + h / 2 - i, 1, 2 * i + 1, C_ACC);
      }
      if (above) {
        rect(x0 + ww * nw + 2 + i, y + h / 2 - i, 1, 2 * i + 1, C_ACC);
      }
    }
  }
}

/* the keys this keyboard covers (with OCT shifts) */
static int keys_lo(void) {
  int lo = cfg.map[map_of(CTL_KEYS, 0)].num + cfg.transpose;
  return lo < 0 ? 0 : lo > 127 - 36 ? 127 - 36 : lo;
}
static int keys_hi(void) { return keys_lo() + key_total() - 1; }

/* the status bar of the playing page: the wordmark; at the right the link to Hingebox, BLE, the modes that are
 * on (spelt out when they fit) and the octave */
static void draw_main_status(void) {
  rect(0, 0, SCREEN_W, BAR_H, C_BG);
  int wx = aa_text(MX, 3, "hinge", C_TXT, AA_SMALL);
  wx = aa_text(wx, 3, "box", C_ACC, AA_SMALL);
  int x = SCREEN_W - MX;
  if (hb_active()) {
    x -= 15;
    draw_link(x, 9, C_ACC);
    x -= 8;
  }
  if (bt_ready) {
    x = mono_r(x, 4, "ble", C_TXT) - 8;
  }
  /* the modes that are on, as words (PASS3 item 8: no "a s p"); when they don't all fit, "oct 0" goes, then the
   * last words become "+n" (the band beside Bop lists them all) */
  static const char* const WORD[4] = {"arp", "chord", "scale", "split"};
  int on[4] = {cfg.arp_on, cfg.chord_on, cfg.scale_on, cfg.split_on};
  const char* words[4];
  int nw = 0, ww = 0;
  for (int i = 0; i < 4; i++) {
    if (on[i]) {
      words[nw++] = WORD[i];
      ww += text_width(WORD[i], FONT_SMALL) + (nw > 1 ? 8 : 0);
    }
  }
  char ov[8];
  int oct = cfg.transpose / 12, o = 0;
  if (oct > 0) {
    ov[o++] = '+';
  }
  format_int(ov + o, oct);
  int room = x - (wx + 8);
  int oct_full = 24 + 4 + text_width(ov, FONT_SMALL), oct_short = oct ? text_width(ov, FONT_SMALL) : 0;
  int full = oct_full + (nw ? 8 : 0) + ww <= room;
  int show = nw;
  char more[4] = "";
  if (!full && oct_short + (nw && oct_short ? 8 : 0) + ww > room) {
    /* keep the first words that fit with "+n" after them */
    int avail = room - oct_short - (oct_short ? 8 : 0) - (8 + 16), used = 0;
    show = 0;
    while (show < nw && used + (show ? 8 : 0) + text_width(words[show], FONT_SMALL) <= avail) {
      used += (show ? 8 : 0) + text_width(words[show], FONT_SMALL);
      show++;
    }
    more[0] = '+';
    more[1] = (char)('0' + (nw - show));
    more[2] = 0;
  }
  if (more[0]) {
    x = mono_r(x, 4, more, C_ACC) - 8;
  }
  for (int i = show - 1; i >= 0; i--) {
    x = mono_r(x, 4, words[i], C_ACC) - 8;
  }
  if (full) {
    x = mono_r(x, 4, ov, oct ? C_TXT : C_MUT) - 4;
    mono_r(x, 4, "oct", C_MUT);
  } else if (oct) {
    mono_r(x, 4, ov, C_TXT);
  }
  hairline(BAR_H - 1);
}

/* the last velocity, labelled */
static void draw_main_meter(void) {
  rect(0, 220, SCREEN_W, 20, C_BG);
  mono(MX, 222, "vel", C_MUT);
  int bx = MX + 32, bw = SCREEN_W - MX - 32 - bx;
  rect(bx, 230, bw, 1, C_LINE);
  if (last_vel) {
    rect(bx, 229, last_vel * bw / 127, 3, C_ACC);
    char v[8];
    format_int(v, last_vel);
    mono_r(SCREEN_W - MX, 222, v, C_TXT);
  }
}

/* ---- the MAP page: every control as a row */
static struct {
  uint8_t kind;
  uint8_t idx;
} row[N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON + N_WHEEL + N_PEDAL];
static int n_rows;
static int map_sel = 0;
static int map_top = 0;

static void rows_build(void) {
  n_rows = 0;
  row[n_rows].kind = CTL_KEYS, row[n_rows++].idx = 0;
  if (pad_count()) {
    for (int i = 0; i < N_PAD; i++) {
      row[n_rows].kind = CTL_PAD, row[n_rows++].idx = (uint8_t)i;
    }
  }
  for (int i = 0; i < N_KNOB; i++) {
    row[n_rows].kind = CTL_KNOB, row[n_rows++].idx = (uint8_t)i;
  }
  if (fader_count()) {
    for (int i = 0; i < N_FADER; i++) {
      row[n_rows].kind = CTL_FADER, row[n_rows++].idx = (uint8_t)i;
    }
  }
  for (int i = 0; i < N_BTN_UI; i++) {
    row[n_rows].kind = CTL_BUTTON, row[n_rows++].idx = (uint8_t)i;
  }
  if (wheel_count()) {
    for (int i = 0; i < N_WHEEL; i++) {
      row[n_rows].kind = CTL_WHEEL, row[n_rows++].idx = (uint8_t)i;
    }
    row[n_rows].kind = CTL_PEDAL, row[n_rows++].idx = 0;
  }
}

static void row_label(int r, char* b) {
  int kind = row[r].kind, i = row[r].idx;
  int p = 0;
  switch (kind) {
    case CTL_KEYS:
      for (const char* s = "KEYS"; *s; s++) b[p++] = *s;
      break;
    case CTL_PAD:
      for (const char* s = "PAD "; *s; s++) b[p++] = *s;
      b[p++] = (char)('0' + (i + 1) / 10); /* 01..16 (was "PAD :" for pad 10) */
      b[p++] = (char)('0' + (i + 1) % 10);
      break;
    case CTL_KNOB:
      for (const char* s = "KNOB "; *s; s++) b[p++] = *s;
      b[p++] = (char)(i < N_KNOB_PHYS ? 'A' : 'B');
      b[p++] = (char)('1' + i % N_KNOB_PHYS);
      break;
    case CTL_FADER:
      for (const char* s = "FADER "; *s; s++) b[p++] = *s;
      b[p++] = (char)(i < N_FADER_PHYS ? 'A' : 'B');
      b[p++] = (char)('1' + i % N_FADER_PHYS);
      break;
    case CTL_BUTTON:
      for (const char* s = BTN_NAME[i]; *s; s++) b[p++] = *s;
      break;
    case CTL_WHEEL:
      for (const char* s = i ? "WHEEL MOD" : "WHEEL PITCH"; *s; s++) b[p++] = *s;
      break;
    default:
      for (const char* s = "PEDAL"; *s; s++) b[p++] = *s;
      break;
  }
  b[p] = 0;
}

static void row_value(int r, char* b) {
  const map_t* m = &cfg.map[map_of(row[r].kind, row[r].idx)];
  static const char* const SHORT[MSG_COUNT] = {"CC", "N", "PC", "PB"};
  int p = 0;
  const char* t = SHORT[m->type];
  for (int i = 0; t[i] && p < 8; i++) b[p++] = t[i];
  if (m->type == MSG_PITCH) {
    b[p++] = '-';
  } else {
    char n[12] = {0};
    format_int(n, m->num);
    for (int i = 0; n[i] && p < 16; i++) b[p++] = n[i];
  }
  for (const char* s = " CH"; *s && p < 20; s++) b[p++] = *s;
  char n[12] = {0};
  format_int(n, m->chan);
  for (int i = 0; n[i] && p < 24; i++) b[p++] = n[i];
  int mode_kind = row[r].kind;
  if (mode_kind == CTL_BUTTON || mode_kind == CTL_KNOB) {
    b[p++] = ' ';
    const char* md = mode_name(m->mode);
    for (int i = 0; md[i] && p < 30; i++) b[p++] = md[i];
  }
  b[p] = 0;
}


/* ---- the EDIT page */
static int edit_slot;

/* which messages a control can send (a knob cannot hold a note: nothing would end it) */
static const uint8_t TYPE_KNOB[] = {MSG_CC, MSG_PITCH};
static const uint8_t TYPE_PAD[] = {MSG_NOTE, MSG_CC, MSG_PC, MSG_PITCH};
static uint8_t type_next(int kind, int slot, uint8_t type, int dir) {
  const uint8_t* list;
  int n;
  if (kind == CTL_KEYS) {
    return MSG_NOTE;
  }
  if (kind == CTL_WHEEL && slot == map_of(CTL_WHEEL, 0)) {
    return MSG_PITCH;
  }
  if (kind == CTL_KNOB || kind == CTL_FADER || (kind == CTL_WHEEL)) {
    list = TYPE_KNOB, n = 2;
  } else {
    list = TYPE_PAD, n = 4;
  }
  int at = 0;
  for (int i = 0; i < n; i++) {
    if (list[i] == type) {
      at = i;
    }
  }
  at = (at + dir) % n;
  if (at < 0) {
    at += n;
  }
  return list[at];
}

static int slot_row(int slot) {
  int kind = map_kind(slot), idx = slot - ctl_base(kind);
  for (int i = 0; i < n_rows; i++) {
    if (row[i].kind == kind && row[i].idx == idx) {
      return i;
    }
  }
  return 0;
}


/* ---- dirty bookkeeping */

static void ui_note_dirty(void) {
  d_chord = d_notes = d_pills = d_foot = 1;
}

static void ui_full(void) {
  d_host = d_bop = 1;
  d_page = d_map = d_edit = d_set = 1;
  d_chord = d_notes = d_pills = d_foot = d_head = 1;
}

/* ---- the playing page. Without Hingebox: status, the chord, the key strip, Bop and what's on, the velocity.
 * With Hingebox: status (with the link), the deck row (groove, track, tempo), the chord, the key strip, Bop and the
 * pad grid with the playhead, the velocity. A knob's overlay takes the top half for 1.5 s. */
static int cnt_notes_held(void) {
  int n = 0;
  for (int i = 0; i < 128; i++) {
    n += refs[i] ? 1 : 0;
  }
  return n;
}

/* the regions (y, height) of the two layouts */
#define HOST_DECK_Y 24  /* groove, track, tempo: 24-61 */
#define HOST_CHORD_Y 64 /* the chord and its notes: 64-117 */
#define HOST_CHORD_H 54
#define HOST_KEYS_Y 122
#define HOST_KEYS_H 22
#define SOLO_CHORD_Y 24 /* 24-107 */
#define SOLO_CHORD_H 84
#define SOLO_KEYS_Y 114
#define SOLO_KEYS_H 30
#define BAND_Y 152 /* Bop and the grid / what's on: 152-219 */

static char last_chord[16];
static uint8_t d_band = 1;

/* the chord: big (Inter Tight, the one place besides the wordmark), its notes in mono under it. With nothing held:
 * the last chord as a ghost (solo), or the beat as four blocks (Hingebox playing) */
static void draw_main_chord(void) {
  int host = hb_active();
  int y0 = host ? HOST_CHORD_Y : SOLO_CHORD_Y, h = host ? HOST_CHORD_H : SOLO_CHORD_H;
  rect(0, y0, SCREEN_W, h, C_BG);
  int named = chord_txt[0] && chord_txt[0] != '?'; /* two notes have no chord name: just their notes */
  if (named) {
    int i = 0;
    for (; chord_txt[i] && i < 15; i++) {
      last_chord[i] = chord_txt[i];
    }
    last_chord[i] = 0;
  }
  const char* s = named ? chord_txt : chord_txt[0] ? "" : last_chord;
  if (host && !chord_txt[0]) {
    /* the beat, readable across the room: four blocks, the one playing in orange, the sixteenths as ticks */
    int bw = 48, gap = (SCREEN_W - 2 * MX - 4 * bw) / 3, by = y0 + 8;
    for (int b = 0; b < 4; b++) {
      int bx = MX + b * (bw + gap);
      int now = hb_step >= 0 && hb_step / 4 % 4 == b;
      rect(bx, by, bw, 14, now ? C_ACC : C_CARD);
      for (int t = 0; t < 4; t++) {
        int tx = bx + t * 12 + 4;
        rect(tx, by + 20, 4, 4, (now && hb_step % 4 >= t) ? C_TXT : C_LINE);
      }
    }
    if (hb_step < 0) {
      mono_c(by + 30, "stopped", C_MUT);
    }
    return;
  }
  if (!s[0]) {
    if (notes_txt[0]) {
      mono_c(host ? y0 + 20 : y0 + 34, notes_txt, C_TXT);
    }
    return;
  }
  int face = host ? AA_MID : AA_BIG;
  if (aa_width(s, face) > SCREEN_W - 2 * MX) {
    face = AA_MID;
  }
  if (aa_width(s, face) > SCREEN_W - 2 * MX) {
    face = AA_SMALL;
  }
  int cy = host ? y0 + 20 : y0 + 34; /* the cap height's middle */
  aa_text_c(0, cy - aa_height(face) / 2, SCREEN_W, s, chord_txt[0] ? C_TXT : C_GHOST, face);
  if (chord_txt[0] && notes_txt[0]) {
    mono_c(host ? y0 + 38 : y0 + 66, notes_txt, C_MUT);
  }
}

static void draw_main_keys(void) {
  int host = hb_active();
  draw_keys(MX, host ? HOST_KEYS_Y : SOLO_KEYS_Y, SCREEN_W - 2 * MX, host ? HOST_KEYS_H : SOLO_KEYS_H, keys_lo(),
            keys_hi(), refs, 0, C_ACC, cfg.scale_on ? (uint16_t)(((SCALE_MASK[cfg.scale_type] << cfg.scale_root) |
                                                                  (SCALE_MASK[cfg.scale_type] >> (12 - cfg.scale_root))) &
                                                                 0xFFFu)
                                                    : 0,
            cfg.split_on ? cfg.split_note : -1);
}

/* the deck row: the groove, the track in its deck colour, the tempo in blue at 2x */
static void draw_deck_row(void) {
  rect(0, HOST_DECK_Y, SCREEN_W, 38, C_BG);
  int limit = SCREEN_W - MX;
  if (hb_bpm) {
    char t[8];
    format_int(t, hb_bpm);
    int bx = SCREEN_W - MX - 24;
    mono(bx, HOST_DECK_Y + 18, "bpm", C_MUT);
    int nx = bx - 4 - mono2_w(t);
    mono2(nx, HOST_DECK_Y + 2, t, C_BLUE);
    limit = nx - 8;
  }
  char g[17];
  int fit = (limit - MX) / 8;
  lower(g, hb_text[0][0] ? hb_text[0] : "hingebox", fit + 1 < 17 ? fit + 1 : 17);
  mono(MX, HOST_DECK_Y + 2, g, C_TXT);
  uint16_t tc = deck_colour(hb_track_col);
  if (hb_text[1][0]) {
    rect(MX, HOST_DECK_Y + 22, 8, 8, tc);
    lower(g, hb_text[1], fit - 1 < 17 && fit > 2 ? fit - 1 : 17);
    mono(MX + 14, HOST_DECK_Y + 18, g, tc);
  }
  hairline(HOST_DECK_Y + 37);
}

/* the pads as Hingebox lights them, in the deck's colours: 2 rows of 8, in groups of four like the steps, the
 * playhead framed in white with an orange bar under its column */
#define GRID_X 66
#define GRID_Y 160
#define PAD_W 18
#define PAD_H 16
static int grid_x(int col) { return GRID_X + col * (PAD_W + 2) + (col >= 4 ? 4 : 0); }
static void draw_grid(void) {
  rect(GRID_X - 4, BAND_Y, SCREEN_W - GRID_X + 4, 66, C_BG);
  for (int p = 0; p < 16; p++) {
    int x = grid_x(p % 8), y = GRID_Y + (p / 8) * (PAD_H + 3);
    uint16_t c = host_on ? (host_vel[p] ? hue_colour(host_vel[p]) : 0) : cfg.pad_rgb[p];
    if (!c) {
      c = C_CARD;
    }
    if (pad_count() && pad_held(p)) {
      c = blend565(c, WHITE, 150);
    }
    rect(x, y, PAD_W, PAD_H, c);
    if (p == hb_step) {
      frame(x, y, PAD_W, PAD_H, C_TXT);
      frame(x + 1, y + 1, PAD_W - 2, PAD_H - 2, C_TXT);
    }
  }
  int ry = GRID_Y + 2 * PAD_H + 3 + 6;
  rect(GRID_X, ry + 1, grid_x(7) + PAD_W - GRID_X, 1, C_LINE);
  for (int col = 0; col < 8; col += 4) { /* the beats: a notch at each group */
    rect(grid_x(col), ry + 1, 1, 3, C_MUT);
  }
  if (hb_step >= 0) {
    rect(grid_x(hb_step % 8), ry, PAD_W, 4, C_ACC);
  }
}

/* ---- Bop: bob on the beat, happy on a chord of three or more, surprised on a fill or a drop, sleepy after a
 * minute of nothing, a blink now and then, the pupils toward the notes. Redrawn only when that changes. */
#define BOP_X 10
#define BOP_Y 156
#define BOP_S 44
static uint32_t hb_hi_at;  /* Hingebox just connected: a happy hello */
static uint16_t bop_drawn = 0xFFFF;

static int bop_mood(void) {
  uint32_t now = millis();
  if (hb_moment && now - hb_moment_at < 1500u) {
    return hb_moment == 3 ? BOP_HAPPY : BOP_WOW;
  }
  if (hb_hi_at && now - hb_hi_at < 2000u) {
    return BOP_HAPPY;
  }
  if (chord_txt[0] && chord_txt[0] != '?' && cnt_notes_held() >= 3) {
    return BOP_HAPPY;
  }
  int playing = hb_active() && hb_step >= 0;
  if (!playing && !cnt_notes_held() && now - act_at > 60000u) {
    return BOP_SLEEPY;
  }
  return BOP_IDLE;
}

static uint16_t bop_state(void) {
  int mood = bop_mood();
  /* up on the beat, down on the off-beat; above 170 bpm on every other beat, so it never moves faster than 3 Hz */
  int lift = (hb_active() && hb_step >= 0 && (hb_bpm > 170 ? (hb_step % 8) < 4 : (hb_step % 4) < 2)) ? 2 : 0;
  int look = 0, n = 0, sum = 0;
  for (int i = 0; i < 128; i++) {
    if (refs[i]) {
      sum += i;
      n++;
    }
  }
  if (n) {
    int mid = (keys_lo() + keys_hi()) / 2, half = key_total() / 2 + 1;
    look = (sum / n - mid) * 3 / half;
    look = look < -2 ? -2 : look > 2 ? 2 : look;
  }
  int blink = mood != BOP_SLEEPY && (millis() / 120u) % 34u == 0;
  return (uint16_t)(mood | lift << 2 | (look + 2) << 5 | blink << 8);
}

static void draw_bop_corner(uint16_t st) {
  int mood = st & 3, lift = st >> 2 & 7, look = (st >> 5 & 7) - 2, blink = st >> 8 & 1;
  rect(0, BAND_Y, GRID_X - 6, 68, C_BG);
  /* the screen's black inside a white outline (PASS3 item 6) */
  bop_draw_ex(BOP_X, BOP_Y, BOP_S, mood, lift, look, blink, C_BG, C_TXT, C_BG, C_ACC);
  if (mood == BOP_SLEEPY) {
    mono(BOP_X + 38, BAND_Y, "z", C_MUT);
    mono(BOP_X + 46, BAND_Y + 4, "z", C_LINE);
  }
  bop_drawn = st;
}

/* without Hingebox, beside Bop: the modes that are on, or Bop's line */
static const char* chord_mood_line(void) {
  const char* c = chord_txt;
  if (*c) {
    c++; /* the root, and its sharp or flat */
  }
  if (*c == '#' || *c == 'b') {
    c++;
  }
  for (int i = 0; c[i] && c[i] != '/'; i++) {
    if (c[i] == 'd' && c[i + 1] == 'i') {
      return "spooky!";
    }
    if (c[i] == 'a' && c[i + 1] == 'u') {
      return "dreamy...";
    }
    if (c[i] == 's' && c[i + 1] == 'u') {
      return "floaty!";
    }
  }
  if (c[0] == 'm' && c[1] != 'a') {
    return "moody...";
  }
  for (int i = 0; c[i] && c[i] != '/'; i++) {
    if (c[i] >= '6' && c[i] <= '9') {
      return "jazzy!";
    }
  }
  return "sunny!";
}

static void draw_band_text(void) {
  int x = 68, lab = 68, val = 116;
  rect(x - 4, BAND_Y, SCREEN_W - x + 4, 66, C_BG);
  char v[3][20];
  const char* l[3];
  int n = 0;
  if (cfg.arp_on && n < 3) {
    char b[20];
    int i = 0;
    for (const char* s = ARP_RATE_NAME[cfg.arp_rate]; *s; s++) b[i++] = *s;
    b[i++] = ' ';
    for (const char* s = ARP_PAT_NAME[cfg.arp_pat]; *s && i < 19; s++) b[i++] = *s;
    b[i] = 0;
    lower(v[n], b, 20);
    l[n++] = "arp";
  }
  if (cfg.chord_on && n < 3) {
    lower(v[n], chord_shape_name(), 20);
    l[n++] = "chord";
  }
  if (cfg.scale_on && n < 3) {
    char b[20];
    int i = 0;
    for (const char* s = NOTE_NAME[cfg.scale_root]; *s; s++) b[i++] = *s;
    b[i++] = ' ';
    for (const char* s = SCALE_NAME[cfg.scale_type]; *s && i < 19; s++) b[i++] = *s;
    b[i] = 0;
    lower(v[n], b, 20);
    l[n++] = "scale";
  }
  if (cfg.split_on && n < 3) {
    char b[20];
    int i = 0;
    for (const char* s = NOTE_NAME[cfg.split_note % 12]; *s; s++) b[i++] = *s;
    b[i++] = (char)('0' + (cfg.split_note / 12 - 1 < 0 ? 0 : cfg.split_note / 12 - 1));
    b[i] = 0;
    lower(v[n], b, 20);
    l[n++] = "split";
  }
  if (n) {
    for (int i = 0; i < n; i++) {
      mono(lab, BAND_Y + 8 + i * 18, l[i], C_MUT);
      mono(val, BAND_Y + 8 + i * 18, v[i], C_TXT);
    }
    return;
  }
  /* Bop's line */
  int held = cnt_notes_held(), mood = bop_mood();
  const char *a, *b;
  if (mood == BOP_SLEEPY) {
    a = "zzz...", b = "play to wake me";
  } else if (mood == BOP_HAPPY && chord_txt[0]) {
    a = chord_mood_line(), b = held >= 4 ? "fancy chord" : "that's a chord";
  } else if (held == 2) {
    a = "two notes...", b = "one more!";
  } else if (held == 1) {
    a = "ooh, a note", b = "add two more";
  } else {
    a = "play a chord", b = "i'll name it";
  }
  mono(x, BAND_Y + 8, a, C_TXT);
  mono(x, BAND_Y + 26, b, C_MUT);
  mono(x, BAND_Y + 44, "or plug in hingebox", RGB(78, 78, 76));
}

/* ---- the knob overlay: what the knob does, a bar, its value; in the knob's deck colour when Hingebox says */
static void draw_overlay(void) {
  int host = hb_active();
  int y0 = BAR_H, h = (host ? HOST_KEYS_Y : SOLO_KEYS_Y) - 4 - y0;
  rect(0, y0, SCREEN_W, h, C_BG);
  char v[8];
  format_int(v, ov_pct ? ov_val * 100 / 127 : ov_val);
  if (ov_pct) {
    int n = str_len(v);
    v[n] = '%', v[n + 1] = 0;
  }
  int vw = mono2_w(v);
  mono2(SCREEN_W - MX - vw, y0 + 8, v, ov_col);
  int room = SCREEN_W - 2 * MX - vw - 12;
  if (mono2_w(ov_name) <= room) {
    mono2(MX, y0 + 8, ov_name, C_TXT);
  } else {
    mono(MX, y0 + 16, ov_name, C_TXT);
  }
  int by = y0 + 48, bw = SCREEN_W - 2 * MX;
  rect(MX, by, bw, 16, C_CARD);
  rect(MX, by, ov_val * bw / 127, 16, ov_col);
  for (int q = 1; q < 4; q++) {
    rect(MX + bw * q / 4, by, 1, 16, C_BG);
  }
  if (ov_sub[0]) {
    mono(MX, by + 22, ov_sub, C_MUT);
  }
}

static void draw_perform(void) {
  int host = hb_active();
  if (d_page) {
    clear(C_BG);
    d_head = d_chord = d_host = d_step = d_band = d_ov = 1;
    bop_drawn = 0xFFFF;
  }
  if (d_chord || d_notes || d_pills) {
    build_note_text();
    d_chord = 1;
  }
  int ov = ov_on();
  if (d_head) {
    draw_main_status();
  }
  if (ov) {
    if (d_ov) {
      draw_overlay();
    }
  } else {
    if (host && d_host) {
      draw_deck_row();
    }
    if (d_chord || d_host || (host && d_step && !chord_txt[0])) {
      draw_main_chord();
    }
  }
  if (d_chord || d_head) {
    draw_main_keys();
  }
  if (host) {
    if (d_host || d_step || d_chord) {
      draw_grid();
    }
  } else if (d_band || d_chord || d_head) {
    draw_band_text();
  }
  uint16_t st = bop_state();
  if (st != bop_drawn) {
    if (!host && (st & 3) != (bop_drawn & 3)) {
      draw_band_text(); /* Bop's line follows his mood */
    }
    draw_bop_corner(st);
  }
  if (d_foot || d_chord) {
    draw_main_meter();
  }
  d_page = d_chord = d_notes = d_pills = d_foot = d_head = d_host = d_bop = d_step = d_band = d_ov = 0;
}

/* ---- the settings pages: a title with on/off and the page dots, the four knobs as rows, a picture of the
 * setting, a hint. ARP, CHORD, SCALE, SPLIT are the < > pages after the playing page. */
static int page_index(void) {
  static const uint8_t P[5] = {PG_MAIN, PG_ARP, PG_CHORD, PG_SCALE, PG_SPLIT};
  for (int k = 0; k < 5; k++) {
    if (P[k] == page) {
      return k;
    }
  }
  return -1;
}

/* the arp's pattern over three held notes and its octaves, as 16 bars; a notch on each beat */
static void draw_arp_picture(int y) {
  int n = 3 * (cfg.arp_oct ? cfg.arp_oct : 1);
  int bw = 11, step = 13, x0 = MX + (SCREEN_W - 2 * MX - 16 * step + 2) / 2, h = 30;
  int pos = 0, dir = 1;
  uint32_t r = 7;
  for (int i = 0; i < 16; i++) {
    int v;
    if (cfg.arp_pat == 1) {
      v = n - 1 - i % n;
    } else if (cfg.arp_pat == 2) {
      v = pos;
      if (pos + dir >= n || pos + dir < 0) {
        dir = -dir;
      }
      pos += dir;
    } else if (cfg.arp_pat == 3) {
      r = r * 1103515245u + 12345u;
      v = (int)(r >> 16) % n;
    } else {
      v = i % n;
    }
    int bh = 4 + v * (h - 4) / (n > 1 ? n - 1 : 1);
    rect(x0 + i * step, y + h - bh, bw, bh, cfg.arp_on ? C_ACC : C_LINE);
    if (i % ARP_SUB[cfg.arp_rate < 5 ? cfg.arp_rate : 0] == 0) {
      rect(x0 + i * step, y + h + 3, bw, 1, C_MUT);
    }
  }
}

static void draw_mode_page(void) {
  char vb[4][20];
  clear(C_BG);
  int pic = ROW_Y0 + 4 * ROW_H + 8;
  if (page == PG_ARP) {
    draw_title("arp", cfg.arp_on, 0);
    lower(vb[0], ARP_RATE_NAME[cfg.arp_rate], 20);
    lower(vb[1], ARP_PAT_NAME[cfg.arp_pat], 20);
    format_int(vb[2], cfg.arp_oct);
    format_int(vb[3], cfg.arp_bpm);
    draw_knob_row(0, "rate", vb[0], C_TXT);
    draw_knob_row(1, "pattern", vb[1], C_TXT);
    draw_knob_row(2, "octaves", vb[2], C_TXT);
    draw_knob_row(3, "bpm", vb[3], C_BLUE);
    draw_arp_picture(pic);
    draw_hint("arp: on/off  pad 1: tap");
  } else if (page == PG_CHORD) {
    draw_title("chord", cfg.chord_on, 0);
    lower(vb[1], chord_shape_name(), 20);
    format_int(vb[2], cfg.chord_n);
    draw_knob_row(0, "memory", cfg.chord_on ? "on" : "off", cfg.chord_on ? C_ACC : C_MUT);
    draw_knob_row(1, "shape", vb[1], C_TXT);
    draw_knob_row(2, "notes", vb[2], C_TXT);
    /* the shape on C4 */
    uint8_t held[128] = {0};
    for (int i = 0; i < cfg.chord_n && i < 8; i++) {
      int nn = 60 + cfg.chord_iv[i];
      if (nn >= 0 && nn < 128) {
        held[nn] = 1;
      }
    }
    draw_keys(MX, ROW_Y0 + 3 * ROW_H + 10, SCREEN_W - 2 * MX, 40, 60, 83, held, 0, cfg.chord_on ? C_ACC : C_MUT,
              1u, -1);
    draw_hint("hold keys + chord: learn");
  } else if (page == PG_SCALE) {
    draw_title("scale", cfg.scale_on, 0);
    lower(vb[1], NOTE_NAME[cfg.scale_root], 20);
    lower(vb[2], SCALE_NAME[cfg.scale_type], 20);
    draw_knob_row(0, "lock", cfg.scale_on ? "on" : "off", cfg.scale_on ? C_ACC : C_MUT);
    draw_knob_row(1, "root", vb[1], C_TXT);
    draw_knob_row(2, "type", vb[2], C_TXT);
    uint16_t m = (uint16_t)(((SCALE_MASK[cfg.scale_type] << cfg.scale_root) |
                             (SCALE_MASK[cfg.scale_type] >> (12 - cfg.scale_root))) &
                            0xFFFu);
    draw_keys(MX, ROW_Y0 + 3 * ROW_H + 10, SCREEN_W - 2 * MX, 40, 48, 71, 0, m, cfg.scale_on ? C_ACC : C_MUT,
              (uint16_t)(1u << cfg.scale_root), -1);
    draw_hint("scale: on/off");
  } else {
    draw_title("split", cfg.split_on, 0);
    int q = 0;
    for (int k = 0; NOTE_NAME[cfg.split_note % 12][k]; k++) {
      vb[1][q++] = NOTE_NAME[cfg.split_note % 12][k];
    }
    int oct = cfg.split_note / 12 - 1;
    vb[1][q++] = (char)('0' + (oct < 0 ? 0 : oct > 9 ? 9 : oct));
    vb[1][q] = 0;
    lower(vb[1], vb[1], 20);
    char lo[12] = "ch ", hi[12] = "ch ";
    format_int(lo + 3, cfg.split_lo_ch);
    format_int(hi + 3, cfg.split_hi_ch);
    draw_knob_row(0, "split", cfg.split_on ? "on" : "off", cfg.split_on ? C_ACC : C_MUT);
    draw_knob_row(1, "at", vb[1], C_TXT);
    draw_knob_row(2, "low", lo, C_TXT);
    draw_knob_row(3, "high", hi, C_TXT);
    draw_keys(MX, pic, SCREEN_W - 2 * MX, 30, keys_lo(), keys_hi(), 0, 0, C_ACC, 0, cfg.split_note);
    draw_hint("k1: on/off  oct-: back");
  }
}

/* ---- MAP: every control as a row, lowercase, with a scrollbar */
#define MAP_ROWS 11
static void draw_map(void) {
  rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
  char b[16], c[8];
  format_int(b, map_sel + 1);
  format_int(c, n_rows);
  int i = str_len(b);
  b[i++] = ' ', b[i++] = 'o', b[i++] = 'f', b[i++] = ' ';
  for (int k = 0; c[k]; k++) {
    b[i++] = c[k];
  }
  b[i] = 0;
  draw_title("map", -1, b);
  if (map_sel < map_top) {
    map_top = map_sel;
  }
  if (map_sel > map_top + MAP_ROWS - 1) {
    map_top = map_sel - (MAP_ROWS - 1);
  }
  for (int r = 0; r < MAP_ROWS && map_top + r < n_rows; r++) {
    int ri = map_top + r, y = 28 + r * 17;
    char label[16], value[32], lv[32];
    row_label(ri, label);
    row_value(ri, value);
    lower(lv, value, sizeof lv);
    if (ri == map_sel) {
      rect(MX - 4, y - 1, SCREEN_W - 2 * MX + 2, 17, C_SEL);
      rect(MX - 4, y - 1, 2, 17, C_ACC);
    }
    mono(MX + 2, y, label, ri == map_sel ? C_TXT : C_MUT);
    mono_r(SCREEN_W - MX - 6, y, lv, ri == map_sel ? C_ACC : C_TXT);
  }
  /* the scrollbar */
  int th = 28 + MAP_ROWS * 17 - 28, kh = th * MAP_ROWS / n_rows, ky = 28 + (th - kh) * map_top / (n_rows - MAP_ROWS);
  rect(SCREEN_W - MX - 1, 28, 1, th, C_CARD);
  rect(SCREEN_W - MX - 2, ky, 3, kh, C_MUT);
  draw_hint("oct+: edit  oct-: back");
}

/* ---- EDIT: the control's message as the four knobs' rows */
static void draw_edit(void) {
  rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
  map_t* m = &cfg.map[edit_slot];
  char label[16], val[4][12];
  row_label(slot_row(edit_slot), label);
  draw_title("edit", -1, label);
  lower(val[0], msg_name(m->type), 12);
  if (m->type == MSG_PITCH) {
    val[1][0] = '-', val[1][1] = 0;
  } else {
    format_int(val[1], m->num);
  }
  format_int(val[2], m->chan);
  int kind = map_kind(edit_slot);
  int modes = kind == CTL_BUTTON || kind == CTL_KNOB;
  lower(val[3], modes ? mode_name(m->mode) : "-", 12);
  draw_knob_row(0, "type", val[0], C_TXT);
  draw_knob_row(1, m->type == MSG_NOTE ? "note" : "number", val[1], C_TXT);
  draw_knob_row(2, "channel", val[2], C_TXT);
  draw_knob_row(3, "mode", val[3], modes ? C_TXT : C_LINE);
  draw_hint("chord: reset  oct-: back");
}

static void redraw(void) {
  if (splash_until) {
    if (millis() < splash_until) {
      if (d_chord || d_notes || d_pills || d_foot) {
        build_note_text(); /* the chord is still worked out under the splash; it's drawn once it goes */
      }
      return;
    }
    splash_until = 0;
    ui_full();
  }
  if (page == PG_MAP) {
    if (d_map) {
      draw_map();
      d_map = 0;
    }
  } else if (page == PG_EDIT) {
    if (d_edit) {
      draw_edit();
      d_edit = 0;
    }
  } else if (page == PG_ARP || page == PG_CHORD || page == PG_SCALE || page == PG_SPLIT) {
    if (d_set) {
      draw_mode_page();
      d_set = 0;
    }
  } else {
    draw_perform();
  }
}

/* ---- settings */
static void cfg_defaults(void) {
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.board = BOARD_SMK37 ? 1 : 0;
  cfg.transpose = 0;
  cfg.pad_bright = 150;
  cfg.knob_bank = cfg.fader_bank = 0;
  cfg.ble_on = 1;
  cfg.arp_on = 0;
  cfg.arp_rate = 2;
  cfg.arp_pat = 0;
  cfg.arp_oct = 2;
  cfg.arp_bpm = 120;
  cfg.chord_on = 0;
  cfg.chord_n = 3;
  cfg.chord_iv[0] = 0, cfg.chord_iv[1] = 4, cfg.chord_iv[2] = 7;
  for (int i = 3; i < 8; i++) {
    cfg.chord_iv[i] = 0;
  }
  cfg.scale_on = 0;
  cfg.scale_root = 0;
  cfg.scale_type = SC_MAJOR;
  cfg.split_on = 0;
  cfg.split_note = 60;
  cfg.split_lo_ch = 1;
  cfg.split_hi_ch = 1;
  for (int p = 0; p < 16; p++) {
    cfg.pad_rgb[p] = 0;
  }
  for (int k = 0; k < CTL_KIND_COUNT; k++) {
    int n = k == CTL_KEYS ? N_KEY : k == CTL_PAD  ? N_PAD
                                : k == CTL_KNOB   ? N_KNOB
                                : k == CTL_FADER  ? N_FADER
                                : k == CTL_BUTTON ? N_BUTTON
                                : k == CTL_WHEEL  ? N_WHEEL
                                                  : N_PEDAL;
    for (int i = 0; i < n; i++) {
      map_default(k, i, BOARD_SMK37, &cfg.map[map_of(k, i)]);
    }
  }
}

static void cfg_changed(void) {
  cfg_dirty = 1;
  cfg_dirty_at = millis();
}

static void cfg_save(void) {
  if (settings_save(&cfg, sizeof cfg) == 0) {
    cfg_dirty = 0;
    saved_at = millis();
    d_foot = d_edit = d_map = d_set = 1;
  } else {
    cfg_dirty_at = millis(); /* no flash: try again later, not every pass */
  }
}

static void cfg_load(void) {
  cfg_defaults();
  if (settings_load(&cfg, sizeof cfg) || cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION ||
      cfg.board != (BOARD_SMK37 ? 1 : 0)) {
    cfg_defaults();
    cfg_dirty = 1;
    cfg_dirty_at = millis();
  }
  if (cfg.transpose < -24 || cfg.transpose > 24) {
    cfg.transpose = 0;
  }
  if (cfg.pad_bright < 8) {
    cfg.pad_bright = 8;
  }
  if (cfg.knob_bank > 1) {
    cfg.knob_bank = 0;
  }
  if (cfg.fader_bank > 1) {
    cfg.fader_bank = 0;
  }
  if (cfg.ble_on > 1) {
    cfg.ble_on = 1;
  }
  if (cfg.arp_rate >= 5) {
    cfg.arp_rate = 2;
  }
  if (cfg.arp_pat >= 4) {
    cfg.arp_pat = 0;
  }
  if (cfg.arp_oct < 1 || cfg.arp_oct > 4) {
    cfg.arp_oct = 2;
  }
  if (cfg.arp_bpm < 40 || cfg.arp_bpm > 240) {
    cfg.arp_bpm = 120;
  }
  if (cfg.chord_n < 1 || cfg.chord_n > 8) {
    cfg.chord_n = 3;
    cfg.chord_iv[0] = 0, cfg.chord_iv[1] = 4, cfg.chord_iv[2] = 7;
  }
  if (cfg.scale_root > 11) {
    cfg.scale_root = 0;
  }
  if (cfg.scale_type >= SC_COUNT) {
    cfg.scale_type = SC_MAJOR;
  }
  if (cfg.split_note > 127) {
    cfg.split_note = 60;
  }
  if (cfg.split_lo_ch < 1 || cfg.split_lo_ch > 16) {
    cfg.split_lo_ch = 1;
  }
  if (cfg.split_hi_ch < 1 || cfg.split_hi_ch > 16) {
    cfg.split_hi_ch = 1;
  }
  for (int i = 0; i < N_MAP; i++) {
    if (cfg.map[i].chan < 1 || cfg.map[i].chan > 16) {
      cfg.map[i].chan = 1;
    }
    if (cfg.map[i].type >= MSG_COUNT) {
      cfg.map[i].type = MSG_CC;
    }
    if (cfg.map[i].mode >= MODE_COUNT) {
      cfg.map[i].mode = MODE_ABS;
    }
  }
}

/* ---- MIDI in: notes for the display, and the colour protocol on channel 16 */
static void midi_in(void) {
  uint8_t st, d1, d2;
  while (midi_read(&st, &d1, &d2)) {
    uint8_t ch = (uint8_t)((st & 0x0Fu) + 1u);
    uint8_t hi = st & 0xF0u;
    if (ch == 16) { /* the pad colours (README: the protocol) */
      if (hi == 0x90 || hi == 0x80) {
        int pad = -1;
        if (d1 >= 36 && d1 < 52) {
          pad = d1 - 36;
        } else if (d1 < 16) {
          pad = d1;
        }
        if (pad >= 0) {
          host_rgb[pad] = (hi == 0x90 && d2) ? hsv565((uint32_t)d2 * 2u, 190, 235) : 0;
          host_vel[pad] = (hi == 0x90) ? d2 : 0;
          host_on = 1;
          d_step = 1;
        }
      } else if (hi == 0xB0) {
        if (d1 >= 104 && d1 <= 119) {
          hb_cc(d1, d2);
        } else if (d1 >= 41 && d1 < 41 + N_BTN_UI) {
          host_btn[d1 - 41] = d2 > 0;
          hb_seen = millis() ? millis() : 1;
        } else if (d1 >= 20 && d1 < 36) {
          host_rgb[d1 - 20] = d2 ? hsv565((uint32_t)d2 * 2u, 190, 235) : 0;
          host_vel[d1 - 20] = d2;
          host_on = 1;
          d_step = 1;
        } else if (d1 == 16) {
          cfg.pad_bright = d2 ? (uint8_t)(d2 * 2u) : 8;
          cfg_changed();
        } else if (d1 == 17) {
          for (int p = 0; p < 16; p++) {
            host_rgb[p] = 0;
            host_vel[p] = 0;
          }
          host_on = 0;
        }
      }
      continue;
    }
    if (hi == 0x90 && d2) {
      note_add(d1, d2);
    } else if (hi == 0x80 || hi == 0x90) {
      note_del(d1);
    }
  }
}

/* ---- the physical knobs, in the order the panel has them */
#if BOARD_SMK37
static const uint8_t ENC_PHYS[N_KNOB_PHYS] = {ENC_MASTER, ENC_SELECT, ENC_ALGORITHM, ENC_PRESETS,
                                              ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4};
#else
static const uint8_t ENC_PHYS[N_KNOB_PHYS] = {ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4};
#endif
/* K1..K4: the submenus' and EDIT's knobs (K5..K8 are the extra performance knobs on the SMK) */
#if BOARD_SMK37
static const uint8_t ENC_TOP[4] = {ENC_MASTER, ENC_SELECT, ENC_ALGORITHM, ENC_PRESETS};
#else
static const uint8_t ENC_TOP[4] = {ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4};
#endif

static int main_group(uint8_t p) { return p == PG_MAIN || p == PG_ARP || p == PG_CHORD || p == PG_SCALE || p == PG_SPLIT; }

/* every note this firmware started, off (a page change never leaves one hanging) */
static void stop_all(void) {
  arp_stop();
  for (int k = 0; k < key_total(); k++) {
    for (int j = 0; j < key_gen_n[k]; j++) {
      if (!cfg.arp_on) {
        send_note(key_ch[k], key_gen[k][j], 0);
      }
      note_del(key_gen[k][j]);
    }
    key_gen_n[k] = 0;
  }
  for (int p = 0; p < pad_count(); p++) {
    if (pad_on[p]) {
      send_map(&cfg.map[map_of(CTL_PAD, p)], 0, 0);
      if (pad_note[p] >= 0) {
        note_del(pad_note[p]);
      }
      pad_note[p] = -1;
      pad_on[p] = 0;
    }
  }
  for (int n = 0; n < 128; n++) {
    arp_src[n] = 0;
  }
  arp_dirty = 1;
  arp_pos = 0;
}

static void page_set(uint8_t p) {
  if (p == page) {
    return;
  }
  /* moving between the playing pages keeps held notes (they still play and release there); MAP/EDIT do not
   * see the keys, so those transitions end them */
  if (!(main_group(page) && main_group(p))) {
    stop_all();
  }
  page = p;
  ui_full();
}

static void page_cycle(int dir) {
  static const uint8_t P[5] = {PG_MAIN, PG_ARP, PG_CHORD, PG_SCALE, PG_SPLIT};
  int i = 0;
  for (int k = 0; k < 5; k++) {
    if (P[k] == page) {
      i = k;
    }
  }
  page_set(P[(i + dir + 5) % 5]);
}

/* a key plays: chord memory expands it, scale lock snaps it; the arp takes over the timing when it is on */
static void key_press(int k) {
  map_t* km = &cfg.map[map_of(CTL_KEYS, 0)];
  int vel = key_velocity(k);
  if (!vel) {
    vel = 100;
  }
  arp_vel = (uint8_t)vel;
  int root = km->num + k + cfg.transpose;
  root = root < 0 ? 0 : root > 127 ? 127
                                   : root;
  uint8_t ch;
  if (cfg.split_on) { /* lower zone / upper zone */
    ch = (uint8_t)((root < cfg.split_note ? cfg.split_lo_ch : cfg.split_hi_ch) - 1u);
  } else {
    ch = (uint8_t)(km->chan - 1u);
  }
  key_ch[k] = ch;
  arp_ch = ch;
  root = scale_note(root);
  int n = 0;
  if (cfg.chord_on && cfg.chord_n) {
    for (int j = 0; j < cfg.chord_n && n < 8; j++) {
      int v = scale_note(root + cfg.chord_iv[j]);
      if (v < 0 || v > 127) {
        continue;
      }
      int dup = 0;
      for (int q = 0; q < n; q++) {
        dup |= key_gen[k][q] == v;
      }
      if (!dup) {
        key_gen[k][n++] = (int8_t)v;
      }
    }
  }
  if (!n) {
    key_gen[k][n++] = (int8_t)root;
  }
  key_gen_n[k] = (uint8_t)n;
  for (int j = 0; j < n; j++) {
    int v = key_gen[k][j];
    note_add(v, vel);
    arp_src_add(v);
    if (!cfg.arp_on) {
      send_note(ch, v, (uint8_t)vel);
    }
  }
}

static void key_release(int k) {
  uint8_t ch = key_ch[k];
  for (int j = 0; j < key_gen_n[k]; j++) {
    int v = key_gen[k][j];
    if (!cfg.arp_on) {
      send_note(ch, v, 0);
    }
    note_del(v);
    arp_src_del(v);
  }
  key_gen_n[k] = 0;
}

/* the ARP page's beat lamp: a quarter note at the set BPM, free-running */
static void arp_beat_tick(uint32_t now) {
  if (page != PG_ARP) {
    arp_beat_on = 0;
    arp_beat_next = 0;
    return;
  }
  if (!arp_beat_next || (int32_t)(now - arp_beat_next) >= 0) {
    arp_beat_on = 1;
    arp_beat_until = now + 70u;
    arp_beat_next = now + 60000u / cfg.arp_bpm;
  }
  if (arp_beat_on && (int32_t)(now - arp_beat_until) >= 0) {
    arp_beat_on = 0;
  }
}

/* tap tempo on pad 1 (ARP page): the average of up to four intervals */
static void arp_tap(void) {
  uint32_t t = millis();
  if (tap_last && t - tap_last > 150u && t - tap_last < 2000u) {
    tap_sum += t - tap_last;
    if (tap_n < 4) {
      tap_n++;
    }
  } else {
    tap_sum = 0;
    tap_n = 0;
  }
  tap_last = t;
  if (tap_n) {
    int bpm = (int)(60000u / (tap_sum / tap_n));
    cfg.arp_bpm = (uint8_t)(bpm < 40 ? 40 : bpm > 240 ? 240
                                                      : bpm);
    cfg_changed();
    arp_beat_next = 0;
    d_set = 1;
  }
}

static void arp_toggle(void) {
  if (cfg.arp_on) {
    cfg.arp_on = 0;
    arp_stop();
  } else {
    cfg.arp_on = 1;
    for (int k = 0; k < key_total(); k++) { /* the direct notes go; the arp restarts the held ones */
      for (int j = 0; j < key_gen_n[k]; j++) {
        send_note(key_ch[k], key_gen[k][j], 0);
      }
    }
    arp_dirty = 1;
    arp_pos = 0;
    arp_next = 0;
  }
  cfg_changed();
  d_head = d_foot = d_set = 1;
}

/* CHORD: with keys held, learn their shape; with none, just switch the memory on or off */
static int keys_held_any(void) {
  for (int k = 0; k < key_total(); k++) {
    if (key_all_held(k)) {
      return 1;
    }
  }
  return 0;
}

static void chord_toggle(void) {
  int bass = 128, any = 0;
  for (int k = 0; k < key_total(); k++) {
    if (key_all_held(k)) {
      int b = cfg.map[map_of(CTL_KEYS, 0)].num + k + cfg.transpose;
      if (b < bass) {
        bass = b;
      }
      any = 1;
    }
  }
  if (any) {
    int8_t iv[16];
    int n = 0;
    for (int k = 0; k < key_total(); k++) {
      if (!key_all_held(k)) {
        continue;
      }
      int d = cfg.map[map_of(CTL_KEYS, 0)].num + k + cfg.transpose - bass;
      int dup = 0;
      for (int j = 0; j < n; j++) {
        dup |= iv[j] == d;
      }
      if (!dup && n < 16) {
        iv[n++] = (int8_t)d;
      }
    }
    for (int a = 1; a < n; a++) { /* ascending */
      int8_t v = iv[a], b = a;
      while (b > 0 && iv[b - 1] > v) {
        iv[b] = iv[b - 1];
        b--;
      }
      iv[b] = v;
    }
    cfg.chord_n = (uint8_t)(n > 8 ? 8 : n);
    for (int i = 0; i < cfg.chord_n; i++) {
      cfg.chord_iv[i] = iv[i];
    }
    cfg.chord_on = 1;
  } else {
    cfg.chord_on = !cfg.chord_on;
  }
  cfg_changed();
  d_head = d_foot = d_set = 1;
}

static void scale_toggle(void) {
  cfg.scale_on = !cfg.scale_on;
  cfg_changed();
  d_head = d_set = 1;
}

/* the buttons that do something on this page glow dimly (the solid ones are the state) */
static void refresh_dim(void) {
  /* the six mapped buttons: dim = usable; lit while held, or while Hingebox says (playing, recording) */
  for (int i = 0; i < N_BTN_UI; i++) {
    led_button_dim(BTN_ID[i], 1);
    if (hb_active()) {
      led_button(BTN_ID[i], host_btn[i] || button_held(BTN_ID[i]));
    }
  }
  led_button_dim(BTN_GLO, 1);
  led_button(BTN_GLO, page == PG_MAP);
#if BOARD_SMK37
  led_button_dim(BTN_SAVE, !cfg.ble_on); /* BT: solid = BLE on */
  led_button(BTN_SAVE, cfg.ble_on);
#else
  led_button_dim(BTN_SAVE, 1);
#endif
  led_button_dim(BTN_HOME, page != PG_MAIN);
  if (main_group(page)) {
    led_button_dim(BTN_ARP, !cfg.arp_on);
    led_button(BTN_ARP, cfg.arp_on);
    led_button_dim(BTN_SEL, !cfg.scale_on);
    led_button(BTN_SEL, cfg.scale_on);
    led_button_dim(BTN_EDIT, !cfg.chord_on);
    led_button(BTN_EDIT, cfg.chord_on);
  } else {
    led_button_dim(BTN_ARP, 1);
    led_button_dim(BTN_SEL, 1);
    led_button_dim(BTN_EDIT, 1);
    led_button(BTN_ARP, 0);
    led_button(BTN_SEL, 0);
    led_button(BTN_EDIT, 0);
  }
  if (!main_group(page)) {
    led_button_dim(BTN_OCT_UP, 1);   /* OK */
    led_button_dim(BTN_OCT_DOWN, 1); /* back */
  } else {
    led_button_dim(BTN_OCT_UP, 0);
    led_button_dim(BTN_OCT_DOWN, 0);
  }
#if BOARD_SMK37
  led_button_dim(BTN_KNOB_BANK, !cfg.knob_bank);
  led_button(BTN_KNOB_BANK, cfg.knob_bank);
  led_button_dim(BTN_FADER_BANK, !cfg.fader_bank);
  led_button(BTN_FADER_BANK, cfg.fader_bank);
  led_button_dim(BTN_LEFT, 1);
  led_button_dim(BTN_RIGHT, 1);
#endif
}

/* ---- the panel */
void setup(void) {
  cfg_load();
  rows_build();
  for (int i = 0; i < N_KNOB; i++) {
    knob_val[i] = 64;
  }
  for (int i = 0; i < 4; i++) {
    fader_prev[i] = 255;
  }
  for (int k = 0; k < 37; k++) {
    key_gen_n[k] = 0;
    for (int j = 0; j < 8; j++) {
      key_gen[k][j] = -1;
    }
  }
  for (int p = 0; p < 16; p++) {
    pad_note[p] = -1;
    pad_on[p] = 0;
  }
  arp_n = 0;
  arp_dirty = 1;
  build_note_text();
  last_vel = 0;
  act_at = millis();
  ui_full();
  /* the boot splash: Bop and the wordmark, for a moment */
  clear(C_BG);
  bop_draw(75, 40, 90, BOP_HAPPY, 0, C_BG, C_TXT, C_BG, C_ACC); /* the screen's black in a white outline */
  /* lowercase in the bitmap mono, like the app's wordmark (the anti-aliased faces have capitals only) */
  int w = text_width("hingebox", FONT_SMALL) * 3;
  int x = text_scale((SCREEN_W - w) / 2, 160, "hinge", C_TXT, FONT_SMALL, 3);
  text_scale(x, 160, "box", C_ACC, FONT_SMALL, 3);
  splash_until = millis() + 1400u;
  if (!splash_until) {
    splash_until = 1;
  }
}

void loop(void) {
  static uint32_t last_enc[ENC_COUNT];

#if FOMA_BLUETOOTH
  if (cfg.ble_on) {
    if (!bt_ready && bt_enable(BT_MIDI) == 0) {
      bt_ready = 1;
      d_head = 1;
    }
  } else if (bt_ready && bt_enable(0) == 0) { /* BT off: stop advertising */
    bt_ready = 0;
    d_head = 1;
  }
#endif
#if BOARD_SMK37
  if (button_pressed(BTN_SAVE)) { /* BT: the BLE MIDI radio */
    cfg.ble_on = !cfg.ble_on;
    cfg_changed();
    d_head = 1;
  }
#endif

  /* PATCH is home, GLOBE the map; < > turn the main pages; ARP/SCALE/CHORD open their submenu
   * (pressing them again, inside it, switches the mode) */
  if (button_pressed(BTN_HOME)) {
    page_set(PG_MAIN);
  }
  if (button_pressed(BTN_GLO)) {
    page_set(page == PG_MAP ? PG_MAIN : PG_MAP);
  }
  if (button_pressed(BTN_ARP)) {
    if (page == PG_ARP) {
      arp_toggle();
    } else {
      page_set(PG_ARP);
    }
  }
  if (page != PG_MAP && page != PG_EDIT) {
    if (button_pressed(BTN_SEL)) {
      if (page == PG_SCALE) {
        scale_toggle();
      } else {
        page_set(PG_SCALE);
      }
    }
    if (button_pressed(BTN_EDIT)) {
      if (page == PG_CHORD) {
        chord_toggle();
      } else {
        if (keys_held_any()) {
          chord_toggle();
        }
        page_set(PG_CHORD);
      }
    }
  }
  if (main_group(page)) {
#if BOARD_SMK37
    if (button_pressed(BTN_LEFT)) {
      page_cycle(-1);
    }
    if (button_pressed(BTN_RIGHT)) {
      page_cycle(1);
    }
#endif
  }

  /* the banks (SMK-37's labelled buttons) */
#if BOARD_SMK37
  if (button_pressed(BTN_KNOB_BANK)) {
    cfg.knob_bank ^= 1;
    cfg_changed();
    d_head = 1;
  }
  if (button_pressed(BTN_FADER_BANK)) {
    cfg.fader_bank ^= 1;
    for (int i = 0; i < N_FADER_PHYS; i++) {
      fader_prev[i] = (uint8_t)fader(i); /* no jump on the new bank */
    }
    cfg_changed();
    d_head = 1;
  }
#endif

  /* transposition (OCT- / OCT+) */
  int oct = button_pressed(BTN_OCT_UP) - button_pressed(BTN_OCT_DOWN);
  if (oct && page == PG_MAIN) {
    int t = cfg.transpose + oct * 12;
    cfg.transpose = (int8_t)(t < -24 ? -24 : t > 24 ? 24
                                                    : t);
    cfg_changed();
    d_head = d_foot = 1;
  }
  if (page == PG_MAIN) {
    led_button(BTN_OCT_DOWN, cfg.transpose < 0);
    led_button(BTN_OCT_UP, cfg.transpose > 0);
  } else {
    led_button(BTN_OCT_DOWN, 0);
    led_button(BTN_OCT_UP, 0);
  }

  /* in a menu OCT+ is OK and OCT- is back (both glow dimly) */
  if (!main_group(page)) {
    if (button_pressed(BTN_OCT_UP)) {
      if (page == PG_MAP) {
        edit_slot = map_of(row[map_sel].kind, row[map_sel].idx);
        page_set(PG_EDIT);
      } else if (page == PG_EDIT) {
        page_set(PG_MAP);
      } else if (page == PG_ARP) {
        arp_toggle();
      } else if (page == PG_CHORD) {
        chord_toggle();
      } else if (page == PG_SCALE) {
        scale_toggle();
      }
    }
    if (button_pressed(BTN_OCT_DOWN)) {
      page_set(page == PG_EDIT ? PG_MAP : PG_MAIN);
    }
  }

  if (main_group(page)) {
    for (int k = 0; k < key_total(); k++) {
      if (key_all_pressed(k)) {
        key_press(k);
      }
      if (key_all_released(k)) {
        key_release(k);
      }
    }
  }

  if (page == PG_MAP) {
    int d = accel(encoder(ENC_SELECT), &last_enc[ENC_SELECT]);
    int jump = accel(encoder(ENC_PRESETS), &last_enc[ENC_PRESETS]);
#if BOARD_SMK37
    d += accel(encoder(ENC_MASTER), &last_enc[ENC_MASTER]); /* K1 too: the menu knob */
#endif
    if (d) {
      map_sel = (map_sel + d) % n_rows;
      if (map_sel < 0) {
        map_sel += n_rows;
      }
      d_map = 1;
    }
    if (jump) {
      map_sel = (map_sel + jump * 8) % n_rows;
      if (map_sel < 0) {
        map_sel += n_rows;
      }
      d_map = 1;
    }
#if BOARD_SMK37
    if (button_pressed(BTN_LEFT) || button_pressed(BTN_RIGHT)) {
      map_sel = (map_sel + (button_pressed(BTN_RIGHT) ? 1 : -1) + n_rows) % n_rows;
      d_map = 1;
    }
#endif
    if (button_pressed(BTN_SEL) || button_pressed(BTN_EDIT)) {
      edit_slot = map_of(row[map_sel].kind, row[map_sel].idx);
      page_set(PG_EDIT);
    }
  } else if (page == PG_EDIT) {
    map_t* m = &cfg.map[edit_slot];
    int kind = map_kind(edit_slot);
    int knob[4];
    for (int i = 0; i < 4; i++) {
      knob[i] = accel(encoder(ENC_TOP[i]), &last_enc[ENC_TOP[i]]);
    }
    int changed = 0;
    if (knob[0]) {
      m->type = type_next(kind, edit_slot, m->type, knob[0]);
      changed = 1;
    }
    if (knob[1] && m->type != MSG_PITCH) {
      int v = m->num + knob[1];
      m->num = (uint8_t)(v < 0 ? 0 : v > 127 ? 127
                                             : v);
      changed = 1;
    }
    if (knob[2]) {
      int v = m->chan + knob[2];
      m->chan = (uint8_t)(v < 1 ? 1 : v > 16 ? 16
                                             : v);
      changed = 1;
    }
    if (knob[3]) {
      int modes = kind == CTL_BUTTON || kind == CTL_KNOB;
      if (modes) {
        int md = m->mode;
        if (kind == CTL_KNOB) {
          md = md == MODE_ABS ? MODE_REL : MODE_ABS;
        } else {
          md = md == MODE_MOM ? MODE_TOG : MODE_MOM;
        }
        m->mode = (uint8_t)md;
        changed = 1;
      }
    }
    if (changed) {
      cfg_changed();
      d_edit = d_map = 1;
    }
    if (button_pressed(BTN_EDIT)) {
      int k = map_kind(edit_slot), i = edit_slot - ctl_base(k);
      map_default(k, i, BOARD_SMK37, m);
      cfg_changed();
      d_edit = d_map = 1;
    }
    if (button_pressed(BTN_SEL)) {
      edit_slot = (edit_slot + 1) % N_MAP;
      map_sel = slot_row(edit_slot);
      d_edit = d_map = 1;
    }
#if BOARD_SMK37
    if (button_pressed(BTN_LEFT) || button_pressed(BTN_RIGHT)) {
      edit_slot = (edit_slot + (button_pressed(BTN_RIGHT) ? 1 : -1) + N_MAP) % N_MAP;
      map_sel = slot_row(edit_slot);
      d_edit = d_map = 1;
    }
#endif
  } else if (page == PG_ARP) {
    if (pad_pressed(0)) {
      arp_tap();
    }
    int d0 = accel(encoder(ENC_TOP[0]), &last_enc[ENC_TOP[0]]);
    int d1 = accel(encoder(ENC_TOP[1]), &last_enc[ENC_TOP[1]]);
    int d2 = accel(encoder(ENC_TOP[2]), &last_enc[ENC_TOP[2]]);
    int d3 = accel(encoder(ENC_TOP[3]), &last_enc[ENC_TOP[3]]);
    int ch = 0;
    if (d0) {
      cfg.arp_rate = (uint8_t)((cfg.arp_rate + d0 + 5) % 5);
      ch = 1;
    }
    if (d1) {
      cfg.arp_pat = (uint8_t)((cfg.arp_pat + d1 + 4) % 4);
      ch = 1;
    }
    if (d2) {
      int o = cfg.arp_oct + d2;
      cfg.arp_oct = (uint8_t)(o < 1 ? 1 : o > 4 ? 4
                                                : o);
      arp_dirty = 1;
      ch = 1;
    }
    if (d3) {
      int b = cfg.arp_bpm + d3 * 5;
      cfg.arp_bpm = (uint8_t)(b < 40 ? 40 : b > 240 ? 240
                                                    : b);
      ch = 1;
    }
    if (ch) {
      cfg_changed();
      d_set = d_head = 1;
    }
  } else if (page == PG_CHORD) {
    int d0 = accel(encoder(ENC_TOP[0]), &last_enc[ENC_TOP[0]]);
    if (d0) {
      cfg.chord_on = !cfg.chord_on;
      cfg_changed();
      d_set = d_head = 1;
    }
  } else if (page == PG_SCALE) {
    int d0 = accel(encoder(ENC_TOP[0]), &last_enc[ENC_TOP[0]]);
    int d1 = accel(encoder(ENC_TOP[1]), &last_enc[ENC_TOP[1]]);
    int d2 = accel(encoder(ENC_TOP[2]), &last_enc[ENC_TOP[2]]);
    int ch = 0;
    if (d0) {
      cfg.scale_on = !cfg.scale_on;
      ch = 1;
    }
    if (d1) {
      cfg.scale_root = (uint8_t)((cfg.scale_root + d1 + 12) % 12);
      ch = 1;
    }
    if (d2) {
      cfg.scale_type = (uint8_t)((cfg.scale_type + d2 + SC_COUNT) % SC_COUNT);
      ch = 1;
    }
    if (ch) {
      cfg_changed();
      d_set = d_head = 1;
    }
  } else if (page == PG_SPLIT) {
    int d0 = accel(encoder(ENC_TOP[0]), &last_enc[ENC_TOP[0]]);
    int d1 = accel(encoder(ENC_TOP[1]), &last_enc[ENC_TOP[1]]);
    int d2 = accel(encoder(ENC_TOP[2]), &last_enc[ENC_TOP[2]]);
    int d3 = accel(encoder(ENC_TOP[3]), &last_enc[ENC_TOP[3]]);
    int ch = 0;
    if (d0) {
      cfg.split_on = !cfg.split_on;
      ch = 1;
    }
    if (d1) {
      int n = cfg.split_note + d1;
      cfg.split_note = (uint8_t)(n < 0 ? 0 : n > 127 ? 127
                                                     : n);
      ch = 1;
    }
    if (d2) {
      int v = cfg.split_lo_ch + d2;
      cfg.split_lo_ch = (uint8_t)(v < 1 ? 1 : v > 16 ? 16
                                                     : v);
      ch = 1;
    }
    if (d3) {
      int v = cfg.split_hi_ch + d3;
      cfg.split_hi_ch = (uint8_t)(v < 1 ? 1 : v > 16 ? 16
                                                     : v);
      ch = 1;
    }
    if (ch) {
      cfg_changed();
      d_set = d_head = 1;
    }
  } else {
    /* MAIN: the controls send */
    for (int p = 0; p < pad_count(); p++) {
      map_t* m = &cfg.map[map_of(CTL_PAD, p)];
      if (pad_pressed(p)) {
        int vel = pad_velocity(p) ? pad_velocity(p) : 100;
        uint8_t msg[3];
        if (m->type == MSG_NOTE) {
          if (map_message(m, (uint8_t)vel, 1, msg)) {
            send_msg(msg);
          }
          pad_note[p] = (int8_t)m->num;
          note_add(m->num, vel);
        } else if (map_message(m, 127, 1, msg)) {
          send_msg(msg);
        }
        pad_on[p] = 1;
        refresh_pads();
      }
      if (!pad_held(p) && pad_on[p]) {
        uint8_t msg[3];
        if (m->type == MSG_NOTE && map_message(m, 0, 0, msg)) {
          send_msg(msg);
        } else if (m->type != MSG_NOTE) {
          send_map(m, 0, 0);
        }
        if (pad_note[p] >= 0) {
          note_del(pad_note[p]);
        }
        pad_note[p] = -1;
        pad_on[p] = 0;
        refresh_pads();
      }
    }
    for (int i = 0; i < N_KNOB_PHYS; i++) {
      int d = accel(encoder(ENC_PHYS[i]), &last_enc[ENC_PHYS[i]]);
      if (!d) {
        continue;
      }
      int slot = cfg.knob_bank * N_KNOB_PHYS + i;
      map_t* m = &cfg.map[map_of(CTL_KNOB, slot)];
      char kn[8] = "knob a1";
      kn[5] = (char)(slot < N_KNOB_PHYS ? 'a' : 'b');
      kn[6] = (char)('1' + slot % N_KNOB_PHYS);
      ctl_name = kn;
      if (m->mode == MODE_REL) {
        int v = 64 + (d > 0 ? (d > 63 ? 63 : d) : (d < -63 ? -63 : d));
        send_map(m, (uint8_t)v, 1);
        ctl_name = 0;
        continue;
      }
      int v = knob_val[slot] + d;
      v = v < 0 ? 0 : v > 127 ? 127
                              : v;
      if (v != knob_val[slot]) {
        knob_val[slot] = (uint8_t)v;
        send_map(m, knob_val[slot], 1);
      }
      ctl_name = 0;
    }
    for (int i = 0; i < fader_count(); i++) {
      int v = fader(i);
      if (v != fader_prev[i]) {
        fader_prev[i] = (uint8_t)v;
        int slot = cfg.fader_bank * N_FADER_PHYS + i;
        char fn[9] = "fader a1";
        fn[6] = (char)(slot < N_FADER_PHYS ? 'a' : 'b');
        fn[7] = (char)('1' + slot % N_FADER_PHYS);
        ctl_name = fn;
        send_map(&cfg.map[map_of(CTL_FADER, slot)], (uint8_t)v, 1);
        ctl_name = 0;
      }
    }
    for (int i = 0; i < N_BTN_UI; i++) {
      int b = BTN_ID[i];
      map_t* m = &cfg.map[map_of(CTL_BUTTON, i)];
      if (button_pressed(b)) {
        int on = 1, val = 127;
        if (m->mode == MODE_TOG) {
          btn_tog ^= (uint8_t)(1u << i);
          val = (btn_tog >> i & 1u) ? 127 : 0;
        }
        send_map(m, (uint8_t)val, on);
        led_button(b, m->mode == MODE_TOG ? (int)(btn_tog >> i & 1u) : 1);
      }
      if (button_released(b)) {
        if (m->mode == MODE_TOG) {
          continue;
        }
        send_map(m, 0, 0);
        led_button(b, 0);
      }
    }
    for (int i = 0; i < wheel_count(); i++) {
      int raw = wheel(i);
      if (raw == wheel_prev[i]) {
        continue;
      }
      int d = raw - wheel_prev[i];
      if (wheel_prev[i] >= 0 && d < 8 && d > -8) {
        continue;
      }
      wheel_prev[i] = raw;
      map_t* m = &cfg.map[map_of(CTL_WHEEL, i)];
      if (m->type == MSG_PITCH) {
        uint8_t bend[3] = {(uint8_t)(0xE0u | ((m->chan - 1u) & 0x0Fu)), (uint8_t)((raw << 4) & 0x7Fu),
                           (uint8_t)((raw << 4) >> 7)};
        send_msg(bend);
      } else {
        ctl_name = i ? "mod wheel" : "pitch wheel";
        send_map(m, (uint8_t)(raw >> 3), 1);
        ctl_name = 0;
      }
    }
    {
      int raw = pedal_raw();
      if (raw >= 0) {
        int now_class = raw > 700 ? 1 : raw < 300 ? 0
                                                  : -1;
        if (now_class >= 0) {
          if (now_class != pedal_cand) {
            pedal_cand = now_class;
            pedal_stable = 0;
          } else if (pedal_stable < 3) {
            pedal_stable++;
            if (pedal_stable == 3 && pedal_prev >= 0 && now_class != pedal_prev) {
              pedal_prev = now_class;
              send_map(&cfg.map[map_of(CTL_PEDAL, 0)], (uint8_t)(now_class ? 127 : 0), 1);
            } else if (pedal_stable == 3) {
              pedal_prev = now_class;
            }
          }
        }
      }
    }
  }

  arp_tick(millis());
  arp_beat_tick(millis());

  /* SAVE stores from any page; HOME + REC is the panic */
#if !BOARD_SMK37
  if (button_pressed(BTN_SAVE)) {
    cfg_save();
  }
#endif
  if (button_held(BTN_HOME) && button_pressed(BTN_REC)) {
    uint8_t msg[3] = {0xB0u, 123u, 0u};
    stop_all();
    send_msg(msg);
    for (int n = 0; n < 128; n++) {
      refs[n] = 0;
    }
    d_chord = d_notes = d_pills = 1;
    led_all_off();
  }

  midi_in();

  if (cfg_dirty && millis() - cfg_dirty_at > 2500u && !button_held(BTN_SAVE)) {
    cfg_save();
  }
  if (ov_at && millis() - ov_at >= 1500u) { /* the overlay goes: the top half comes back */
    ov_at = 0;
    d_host = d_chord = 1;
  }
  {
    static int was_active;
    int a = hb_active();
    if (a != was_active) {
      was_active = a;
      if (a) {
        hb_hi_at = millis() ? millis() : 1; /* Bop says hi */
      } else {
        ov_at = 0;
      }
      ui_full(); /* the other layout */
    }
  }
  {
    static uint8_t toast_shown;
    if (saved_at && millis() - saved_at < 1500u) {
      if (!toast_shown) {
        toast_shown = 1;
        d_foot = d_edit = d_set = 1;
      }
    } else if (saved_at || toast_shown) {
      saved_at = 0;
      toast_shown = 0;
      d_foot = d_edit = d_set = 1;
    }
  }
  refresh_dim();
  refresh_key_leds();
  redraw();
}

/* a controller that is also a sound card (usb_audio, 0.3): whatever the phone or computer plays over USB comes
 * out of the keyboard's own output. Without usb_audio: silence. */
void audio(int32_t* out, uint32_t frames) {
#if FOMA_USB_AUDIO
  const int32_t* in = audio_in();
  if (in) {
    for (uint32_t i = 0; i < 2u * frames; i++) {
      out[i] = in[i];
    }
  }
#else
  (void)out, (void)frames;
#endif
}

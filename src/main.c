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

/* ---- the look: near-black, one accent, colour by pitch class */
#define C_BG RGB(8, 10, 14)
#define C_CARD RGB(19, 23, 30)
#define C_LINE RGB(38, 45, 56)
#define C_TXT RGB(232, 238, 246)
#define C_MUT RGB(112, 124, 142)
#define C_ACC RGB(64, 224, 196)
#define C_SEL RGB(14, 46, 48)

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
static char cc_txt[16];
static uint32_t cc_at;

static void note_add(int n, int v) {
  if (n < 0 || n > 127) {
    return;
  }
  if (!refs[n]++) {
    last_vel = (uint8_t)(v > 127 ? 127 : v);
  }
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
  if ((m[0] & 0xF0u) == 0xB0u) { /* the knob or fader just moved: say which CC and value */
    char b[12];
    int i = 0;
    cc_txt[i++] = 'C', cc_txt[i++] = 'C';
    format_int(b, m[1]);
    for (int k = 0; b[k] && i < 9; k++) {
      cc_txt[i++] = b[k];
    }
    cc_txt[i++] = ' ';
    format_int(b, m[2]);
    for (int k = 0; b[k] && i < 15; k++) {
      cc_txt[i++] = b[k];
    }
    cc_txt[i] = 0;
    cc_at = millis();
    d_foot = 1;
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
    uint16_t c = cfg.pad_rgb[p] ? bright565(cfg.pad_rgb[p], cfg.pad_bright) : 0;
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
    if (cfg.pad_rgb[p]) {
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

/* ---- the screen: menus get a small title; the performance page gets only what is needed */
static void draw_header(void) {
  rect(0, 0, SCREEN_W, 32, C_BG);
  const char* sub = page == PG_MAP ? "MAP" : page == PG_EDIT ? "EDIT"
                                         : page == PG_ARP    ? "ARP"
                                         : page == PG_CHORD  ? "CHORD"
                                         : page == PG_SPLIT  ? "SPLIT"
                                                             : "SCALE";
  aa_text(12, 2, sub, C_MUT, AA_SMALL);
  if (bt_ready) {
    aa_text_r(SCREEN_W - 12, 2, "BLE", C_ACC, AA_SMALL);
  }
  rect(0, 30, SCREEN_W, 1, C_LINE);
}

/* the performance page: octave, the modes that are on, the chord, its notes, the last velocity as a thin rule */
static void draw_main_status(void) {
  rect(0, 0, SCREEN_W, 26, C_BG);
  char b[12];
  int oct = cfg.transpose / 12, i = 0;
  b[i++] = 'O', b[i++] = 'C', b[i++] = 'T', b[i++] = ' ';
  if (oct < 0) {
    b[i++] = '-';
    if (oct <= -10) {
      b[i++] = (char)('0' + (-oct) / 10);
    }
    b[i++] = (char)('0' + (-oct) % 10);
  } else {
    if (oct) {
      b[i++] = '+';
    }
    b[i++] = (char)('0' + oct);
  }
  b[i] = 0;
  aa_text(14, 1, b, C_MUT, AA_SMALL);
  int x = 150;
  aa_text(x, 1, "A", cfg.arp_on ? C_ACC : C_LINE, AA_SMALL);
  aa_text(x + 14, 1, "C", cfg.chord_on ? C_ACC : C_LINE, AA_SMALL);
  aa_text(x + 28, 1, "S", cfg.scale_on ? C_ACC : C_LINE, AA_SMALL);
  aa_text(x + 42, 1, "P", cfg.split_on ? C_ACC : C_LINE, AA_SMALL);
  if (bt_ready) {
    aa_text_r(SCREEN_W - 14, 1, "BLE", C_ACC, AA_SMALL);
  }
}

static void draw_main_chord(void) {
  rect(0, 26, SCREEN_W, 74, C_BG);
  if (chord_txt[0]) {
    int face = AA_BIG;
    if (aa_width(chord_txt, face) > SCREEN_W - 16) {
      face = AA_MID;
    }
    if (aa_width(chord_txt, face) > SCREEN_W - 16) {
      face = AA_SMALL;
    }
    aa_text_c(0, 68 - aa_height(face) / 2, SCREEN_W, chord_txt, C_TXT, face);
  }
  rect(16, 100, SCREEN_W - 32, 1, C_LINE);
}

static void draw_main_notes(void) {
  rect(0, 104, SCREEN_W, 30, C_BG);
  if (notes_txt[0]) {
    aa_text_c(0, 110, SCREEN_W, notes_txt, C_MUT, AA_SMALL);
  }
}

static void draw_main_cc(void) {
  rect(0, 140, SCREEN_W, 24, C_BG);
  if (cc_at && millis() - cc_at < 1500u) {
    aa_text_r(SCREEN_W - 16, 144, cc_txt, C_MUT, AA_SMALL);
  }
}

static void draw_main_meter(void) {
  rect(0, 222, SCREEN_W, 18, C_BG);
  rect(16, 232, SCREEN_W - 32, 1, C_LINE);
  if (last_vel) {
    rect(16, 231, last_vel * (SCREEN_W - 32) / 127, 2, C_ACC);
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
      if (i < 9)
        b[p++] = '0';
      b[p++] = (char)('1' + i);
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

static void draw_map(void) {
  rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
  draw_header();
  char b[12] = {0};
  format_int(b, map_sel + 1);
  text(120, 5, b, C_MUT, FONT_SMALL);
  text(136, 5, "/", C_MUT, FONT_SMALL);
  format_int(b, n_rows);
  text(144, 5, b, C_MUT, FONT_SMALL);
  if (map_sel < map_top) {
    map_top = map_sel;
  }
  if (map_sel > map_top + 10) {
    map_top = map_sel - 10;
  }
  for (int i = 0; i < 11 && map_top + i < n_rows; i++) {
    int r = map_top + i, y = 36 + i * 16;
    char label[16], value[32];
    row_label(r, label);
    row_value(r, value);
    if (r == map_sel) {
      rect(4, y - 1, SCREEN_W - 8, 15, C_SEL);
      rect(4, y - 1, 2, 15, C_ACC);
    }
    text(10, y, label, r == map_sel ? C_TXT : C_MUT, FONT_SMALL);
    text(SCREEN_W - 8 - text_width(value, FONT_SMALL), y, value, r == map_sel ? C_ACC : C_TXT, FONT_SMALL);
  }
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

static void draw_edit(void) {
  rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
  draw_header();
  map_t* m = &cfg.map[edit_slot];
  char label[16];
  row_label(slot_row(edit_slot), label);
  text(12, 36, label, C_TXT, FONT_SMALL);
  static const char* const FIELD[4] = {"TYPE", "NUM", "CHAN", "MODE"};
  char val[4][12] = {{0}};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 58, y = 58;
    rect(x, y, 54, 86, C_CARD);
    frame(x, y, 54, 86, C_LINE);
    text(x + (54 - text_width(FIELD[i], FONT_SMALL)) / 2, y + 6, FIELD[i], C_MUT, FONT_SMALL);
    const char* v = "";
    if (i == 0) {
      v = msg_name(m->type);
    } else if (i == 1) {
      if (m->type == MSG_PITCH) {
        v = "-";
      } else {
        format_int(val[i], m->num);
        v = val[i];
      }
    } else if (i == 2) {
      format_int(val[i], m->chan);
      v = val[i];
    } else {
      v = mode_name(m->mode);
    }
    int sc = text_width(v, FONT_BIG) <= 50 ? FONT_BIG : FONT_SMALL;
    int w = text_width(v, sc);
    text(x + (54 - w) / 2, y + 34 + (sc == FONT_BIG ? 0 : 8), v, C_ACC, sc);
    char k[12] = "K1";
    k[1] = (char)('1' + i);
    text(x + (54 - text_width(k, FONT_SMALL)) / 2, y + 70, k, C_LINE, FONT_SMALL);
  }
  if (saved_at && millis() - saved_at < 1500u) {
    text(12, 216, "SAVED TO FLASH", C_ACC, FONT_SMALL);
  }
}

/* ---- dirty bookkeeping */

static void ui_note_dirty(void) {
  d_chord = d_notes = d_pills = d_foot = 1;
}

static void ui_full(void) {
  d_page = d_map = d_edit = d_set = 1;
  d_chord = d_notes = d_pills = d_foot = d_head = 1;
}

static void draw_perform(void) {
  if (d_page) {
    clear(C_BG);
  }
  if (d_page || d_chord || d_notes || d_pills || d_foot) {
    build_note_text();
  }
  if (d_page || d_head || d_chord || d_notes || d_foot) {
    draw_main_status();
    draw_main_chord();
    draw_main_notes();
    draw_main_cc();
    draw_main_meter();
  }
  d_page = d_chord = d_notes = d_pills = d_foot = d_head = 0;
}

static void draw_mode_page(void) {
  const char* lab[4];
  char vbuf[4][20];
  const char* val[4];
  clear(C_BG);
  draw_header();
  for (int i = 0; i < 4; i++) {
    vbuf[i][0] = 0;
    val[i] = "";
  }
  if (page == PG_ARP) {
    lab[0] = "RATE", lab[1] = "PATTERN", lab[2] = "OCT", lab[3] = "BPM";
    val[0] = ARP_RATE_NAME[cfg.arp_rate];
    val[1] = ARP_PAT_NAME[cfg.arp_pat];
    format_int(vbuf[2], cfg.arp_oct);
    val[2] = vbuf[2];
    format_int(vbuf[3], cfg.arp_bpm);
    val[3] = vbuf[3];
    text(12, 36, cfg.arp_on ? "ARPEGGIATOR ON" : "ARPEGGIATOR OFF", cfg.arp_on ? C_ACC : C_MUT, FONT_SMALL);
  } else if (page == PG_CHORD) {
    lab[0] = "ON", lab[1] = "SHAPE", lab[2] = "NOTES", lab[3] = "";
    val[0] = cfg.chord_on ? "ON" : "OFF";
    val[1] = chord_shape_name();
    format_int(vbuf[2], cfg.chord_n);
    val[2] = vbuf[2];
    text(12, 36, "CHORD MEMORY", C_TXT, FONT_SMALL);
  } else if (page == PG_SCALE) {
    lab[0] = "ON", lab[1] = "ROOT", lab[2] = "TYPE", lab[3] = "";
    val[0] = cfg.scale_on ? "ON" : "OFF";
    val[1] = NOTE_NAME[cfg.scale_root];
    val[2] = SCALE_NAME[cfg.scale_type];
    text(12, 36, "SCALE LOCK", C_TXT, FONT_SMALL);
  } else {
    lab[0] = "ON", lab[1] = "SPLIT", lab[2] = "LO CH", lab[3] = "HI CH";
    val[0] = cfg.split_on ? "ON" : "OFF";
    int oct = cfg.split_note / 12 - 1;
    vbuf[1][0] = 0;
    int q = 0;
    for (int k = 0; NOTE_NAME[cfg.split_note % 12][k]; k++) {
      vbuf[1][q++] = NOTE_NAME[cfg.split_note % 12][k];
    }
    vbuf[1][q++] = (char)('0' + (oct < 0 ? 0 : oct > 9 ? 9
                                                       : oct));
    vbuf[1][q] = 0;
    val[1] = vbuf[1];
    format_int(vbuf[2], cfg.split_lo_ch);
    val[2] = vbuf[2];
    format_int(vbuf[3], cfg.split_hi_ch);
    val[3] = vbuf[3];
    text(12, 36, "KEYBOARD SPLIT", C_TXT, FONT_SMALL);
  }
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 58, y = 58;
    rect(x, y, 54, 86, C_CARD);
    frame(x, y, 54, 86, C_LINE);
    text(x + (54 - text_width(lab[i], FONT_SMALL)) / 2, y + 6, lab[i], C_MUT, FONT_SMALL);
    int sc = text_width(val[i], FONT_BIG) <= 50 ? FONT_BIG : FONT_SMALL;
    int w = text_width(val[i], sc);
    text(x + (54 - w) / 2, y + 34 + (sc == FONT_BIG ? 0 : 8), val[i], C_ACC, sc);
    char k[8] = "K1";
    if (i < 3 || page != PG_CHORD) {
      k[1] = (char)('1' + i);
      text(x + (54 - text_width(k, FONT_SMALL)) / 2, y + 70, k, C_LINE, FONT_SMALL);
    }
  }
  if (saved_at && millis() - saved_at < 1500u) {
    text(12, 216, "SAVED TO FLASH", C_ACC, FONT_SMALL);
  }
}

static void redraw(void) {
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
          if (hi == 0x90 && d2) {
            cfg.pad_rgb[pad] = hsv565((uint32_t)d2 * 2u, 190, 235);
          } else {
            cfg.pad_rgb[pad] = 0;
          }
          cfg_changed();
        }
      } else if (hi == 0xB0) {
        if (d1 >= 20 && d1 < 36) {
          cfg.pad_rgb[d1 - 20] = d2 ? hsv565((uint32_t)d2 * 2u, 190, 235) : 0;
          cfg_changed();
        } else if (d1 == 16) {
          cfg.pad_bright = d2 ? (uint8_t)(d2 * 2u) : 8;
          cfg_changed();
        } else if (d1 == 17) {
          for (int p = 0; p < 16; p++) {
            cfg.pad_rgb[p] = 0;
          }
          cfg_changed();
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
  ui_full();
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
      if (m->mode == MODE_REL) {
        int v = 64 + (d > 0 ? (d > 63 ? 63 : d) : (d < -63 ? -63 : d));
        send_map(m, (uint8_t)v, 1);
        continue;
      }
      int v = knob_val[slot] + d;
      v = v < 0 ? 0 : v > 127 ? 127
                              : v;
      if (v != knob_val[slot]) {
        knob_val[slot] = (uint8_t)v;
        send_map(m, knob_val[slot], 1);
      }
    }
    for (int i = 0; i < fader_count(); i++) {
      int v = fader(i);
      if (v != fader_prev[i]) {
        fader_prev[i] = (uint8_t)v;
        int slot = cfg.fader_bank * N_FADER_PHYS + i;
        send_map(&cfg.map[map_of(CTL_FADER, slot)], (uint8_t)v, 1);
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
        send_map(m, (uint8_t)(raw >> 3), 1);
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
  if (cc_at && millis() - cc_at >= 1500u) {
    cc_at = 0;
    d_foot = 1;
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

void audio(int32_t* out, uint32_t frames) { /* a controller: silence */
  (void)out, (void)frames;
}

/* The firmware's screen and logic on the computer: build src/main.c against stubs, drive it, write PPMs.
 * Not an emulator: no chip, no sound, no MIDI device; just enough to see and check the UI. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fm1.h"

typedef struct {
  uint8_t h, pad, first, last;
  const uint8_t *adv, *bw;
  const uint16_t* off;
  const uint8_t* data;
} felucca_font_t;
#include "../platform/gen/felucca_font.h"

static uint16_t fb[SCREEN_W * SCREEN_H];
static uint16_t fb_swap(uint16_t c) { return c; }
static void fb_dirty(int y0, int y1) {
  (void)y0;
  (void)y1;
}

static uint16_t blend(uint16_t bg, uint16_t fg, uint32_t a) {
  uint32_t r = ((bg >> 11) * (15u - a) + (fg >> 11) * a) / 15u;
  uint32_t g = (((bg >> 5) & 63u) * (15u - a) + ((fg >> 5) & 63u) * a) / 15u;
  uint32_t b = ((bg & 31u) * (15u - a) + (fg & 31u) * a) / 15u;
  return (uint16_t)(r << 11 | g << 5 | b);
}
static const felucca_font_t* font_of(int font) { return font == FONT_BIG ? &FONT_L : &FONT_S; }
static uint32_t font_glyph(const felucca_font_t* f, uint32_t ch) {
  if (ch >= 'a' && ch <= 'z' && f->last < 'a')
    ch -= 32u;
  if (ch < f->first || ch > f->last)
    ch = '?';
  return ch - f->first;
}
void clear(uint16_t c) {
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = c;
}
void pixel(int x, int y, uint16_t c) {
  if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
    fb[y * SCREEN_W + x] = c;
}
void rect(int x, int y, int w, int h, uint16_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) pixel(i, j, c);
}
void frame(int x, int y, int w, int h, uint16_t c) {
  rect(x, y, w, 1, c);
  rect(x, y + h - 1, w, 1, c);
  rect(x, y, 1, h, c);
  rect(x + w - 1, y, 1, h, c);
}
void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 >= dy)
      err += dy, x0 += sx;
    if (e2 <= dx)
      err += dx, y0 += sy;
  }
}
static int glyph_run(int x, int y, const char* s, uint16_t color, int font, int scale) {
  const felucca_font_t* f = font_of(font);
  for (; *s; s++) {
    uint32_t gi = font_glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u) / 2u, gx, gy;
    const uint8_t* gd = f->data + f->off[gi];
    for (gy = 0; gy < f->h; gy++)
      for (gx = 0; gx < w; gx++) {
        int px = x - f->pad + (int)gx * scale, py = y + (int)gy * scale;
        uint32_t v = gd[gy * bpr + gx / 2u];
        v = (gx & 1u) ? (v & 15u) : (v >> 4);
        if (!v || (unsigned)px >= SCREEN_W || (unsigned)py >= SCREEN_H)
          continue;
        uint16_t c = v == 15u ? color : blend(fb[py * SCREEN_W + px], color, v);
        rect(px, py, scale, scale, c);
      }
    x += f->adv[gi] * scale;
  }
  return x;
}
int text(int x, int y, const char* s, uint16_t c, int font) { return glyph_run(x, y, s, c, font, 1); }
int text_scale(int x, int y, const char* s, uint16_t c, int font, int scale) {
  if (scale < 1)
    scale = 1;
  if (scale > 4)
    scale = 4;
  return glyph_run(x, y, s, c, font, scale);
}
int text_width(const char* s, int font) {
  const felucca_font_t* f = font_of(font);
  int w = 0;
  for (; *s; s++) w += f->adv[font_glyph(f, (uint8_t)*s)];
  return w;
}
int text_scale_width(const char* s, int font, int scale) {
  if (scale < 1)
    scale = 1;
  if (scale > 4)
    scale = 4;
  return text_width(s, font) * scale;
}
int text_height(int font) { return font_of(font)->h; }

/* ---- the clock, settings, MIDI */
static uint32_t now_ms;
uint32_t millis(void) { return now_ms; }
uint32_t micros(void) { return now_ms * 1000u; }
static int saved_ok;
int settings_load(void* p, uint32_t n) {
  (void)p;
  (void)n;
  return -1;
}
int settings_save(const void* p, uint32_t n) {
  (void)p;
  (void)n;
  saved_ok++;
  return 0;
}
static uint32_t rnd = 1;
uint32_t random32(void) {
  rnd ^= rnd << 13;
  rnd ^= rnd >> 17;
  rnd ^= rnd << 5;
  return rnd;
}
void format_int(char* b, int32_t v) {
  char t[12];
  int i = 0, neg = v < 0;
  uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
  do {
    t[i++] = (char)('0' + u % 10u);
    u /= 10u;
  } while (u);
  int o = 0;
  if (neg)
    b[o++] = '-';
  while (i) b[o++] = t[--i];
  b[o] = 0;
}
static uint8_t inq[64][3];
static int inq_w, inq_r;
static int sent_count;
static uint8_t sent[64][3];
void midi_send(uint8_t st, uint8_t d1, uint8_t d2) {
  if (sent_count < 64) {
    sent[sent_count][0] = st;
    sent[sent_count][1] = d1;
    sent[sent_count][2] = d2;
  }
  sent_count++;
}
int midi_read(uint8_t* st, uint8_t* d1, uint8_t* d2) {
  if (inq_r == inq_w)
    return 0;
  *st = inq[inq_r][0], *d1 = inq[inq_r][1], *d2 = inq[inq_r][2];
  inq_r = (inq_r + 1) % 64;
  return MIDI_USB;
}
static void midi_push(uint8_t a, uint8_t b, uint8_t c) {
  inq[inq_w][0] = a, inq[inq_w][1] = b, inq[inq_w][2] = c;
  inq_w = (inq_w + 1) % 64;
}

/* ---- the panel */
static uint64_t keys_held, keys_press, keys_rel;
static uint32_t btns_held, btns_press, btns_rel;
static int32_t encs[ENC_COUNT];
static uint16_t pad_rgb[16];
static uint8_t pads_held, pads_press;
static uint8_t pad_vel[16];

int key_total(void) { return BOARD_SMK37 ? 37 : 27; }
int key_base_note(void) { return BOARD_SMK37 ? 48 : 53; }
int key_all_held(int k) { return keys_held >> k & 1u; }
int key_all_pressed(int k) { return keys_press >> k & 1u; }
int key_all_released(int k) { return keys_rel >> k & 1u; }
int key_velocity(int k) {
  (void)k;
  return BOARD_SMK37 ? 100 : 0;
}
int pad_count(void) { return BOARD_SMK37 ? 16 : 0; }
int pad_held(int p) { return pads_held >> p & 1u; }
int pad_pressed(int p) { return pads_press >> p & 1u; }
int pad_velocity(int p) { return pad_vel[p]; }
int pad_pressure(int p) {
  (void)p;
  return 0;
}
void led_pad(int p, uint16_t c) {
  if ((unsigned)p < 16)
    pad_rgb[p] = c;
}
void led_all_off(void) { memset(pad_rgb, 0, sizeof pad_rgb); }
int fader_count(void) { return BOARD_SMK37 ? 4 : 0; }
int fader(int i) {
  (void)i;
  return 0;
}
int wheel_count(void) { return BOARD_SMK37 ? 2 : 0; }
int wheel(int i) {
  (void)i;
  return 512;
}
int pedal_raw(void) { return BOARD_SMK37 ? 0 : -1; }
#if FOMA_USB_AUDIO
static int32_t t_ain[64];
static int t_ain_on;
const int32_t* audio_in(void) { return t_ain_on ? t_ain : 0; }
#endif
static uint32_t ledkeys, ledbtns, ledbtns_dim;
void led_button_dim_stub(int b, int on) {
  if (on)
    ledbtns_dim |= 1u << b;
  else
    ledbtns_dim &= ~(1u << b);
}
void led_key(int k, int on) {
  if (on)
    ledkeys |= 1u << k;
  else
    ledkeys &= ~(1u << k);
}
void led_button_dim(int b, int on) { led_button_dim_stub(b, on); }
void led_key_dim(int k, int on) {
  (void)k;
  (void)on;
}
void led_button(int b, int on) {
  if (on)
    ledbtns |= 1u << b;
  else
    ledbtns &= ~(1u << b);
}
void led_backlight(int level) { (void)level; }
int button_held(int b) { return btns_held >> b & 1u; }
int button_pressed(int b) { return btns_press >> b & 1u; }
int button_released(int b) { return btns_rel >> b & 1u; }
int32_t encoder(int e) { return (unsigned)e < ENC_COUNT ? encs[e] : 0; }
uint32_t master_knob(void) { return 512; }

#include "../src/main.c"
#include "../platform/src/aatext.c"

static int ink(void) {
  int n = 0;
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
    if (fb[i] != 0x0000)
      n++;
  return n;
}
static int sent_note(int note) {
  for (int i = 0; i < sent_count && i < 64; i++)
    if (sent[i][0] == 0x90 && sent[i][1] == note && sent[i][2])
      return 1;
  return 0;
}
static int sent_after(int from, int note) {
  for (int i = from; i < sent_count && i < 64; i++)
    if (sent[i][0] == 0x90 && sent[i][1] == note && sent[i][2])
      return 1;
  return 0;
}
static int sent_status(int from, int note) {
  for (int i = from; i < sent_count && i < 64; i++)
    if ((sent[i][0] & 0xF0) == 0x90 && sent[i][1] == note && sent[i][2])
      return sent[i][0];
  return -1;
}
static int sent_cc_after(int from, int cc, int* value) {
  for (int i = from; i < sent_count && i < 64; i++)
    if (sent[i][0] == 0xB0 && sent[i][1] == cc) {
      if (value)
        *value = sent[i][2];
      return 1;
    }
  return 0;
}
static void ppm(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
    uint16_t c = fb[i];
    uint8_t r = (uint8_t)(((c >> 11) & 31u) * 255u / 31u);
    uint8_t g = (uint8_t)(((c >> 5) & 63u) * 255u / 63u);
    uint8_t b = (uint8_t)((c & 31u) * 255u / 31u);
    fputc(r, f), fputc(g, f), fputc(b, f);
  }
  fclose(f);
}
/* SMK-37 shots are build/host-NAME.ppm; the FM-1's build/fm1-host-NAME.ppm (it used to overwrite them) */
static void shot(const char* name) {
  char path[96];
  snprintf(path, sizeof path, "build/%shost-%s.ppm", BOARD_SMK37 ? "" : "fm1-", name);
  ppm(path);
}
static void tick(void) {
  now_ms += 10;
  loop();
  keys_press = keys_rel = 0;
  btns_press = btns_rel = 0;
  for (int i = 0; i < ENC_COUNT; i++) encs[i] = 0;
  pads_press = 0;
}
static void press_key(int k) {
  keys_held |= (uint64_t)1u << k;
  keys_press |= (uint64_t)1u << k;
}
static void release_key(int k) {
  keys_held &= ~((uint64_t)1u << k);
  keys_rel |= (uint64_t)1u << k;
}
static void press_btn(int b) {
  btns_held |= 1u << b;
  btns_press |= 1u << b;
}
static void release_btn(int b) {
  btns_held &= ~(1u << b);
  btns_rel |= 1u << b;
}

int main(void) {
  setup();
  tick();
  shot("splash");
  now_ms += 1500; /* past the boot splash: the shots below are the pages, not the splash again */
  tick();
#if FOMA_USB_AUDIO
  { /* the keyboard as a sound card: silence until the host plays, then exactly what it plays */
    int32_t o[64] = {0};
    audio(o, 32);
    for (int i = 0; i < 64; i++) {
      if (o[i]) {
        printf("audio: not silent without the host\n");
        return 1;
      }
    }
    for (int i = 0; i < 64; i++) {
      t_ain[i] = (i * 997) % 65535 - 32767;
    }
    t_ain_on = 1;
    audio(o, 32);
    t_ain_on = 0;
    for (int i = 0; i < 64; i++) {
      if (o[i] != t_ain[i]) {
        printf("audio: sample %d is %d, want %d\n", i, (int)o[i], (int)t_ain[i]);
        return 1;
      }
    }
  }
#endif
  shot("perform");

  /* C4 E4 G4 B4: a maj7. The SMK's key 12 is C4 (key 0 = C3); the FM-1's key 12 is C4 too (key 0 = F3) */
  int base = BOARD_SMK37 ? 12 : 7; /* key 0 = C3 / F3; both reach C4 here */
  press_key(base + 0);             /* C */
  press_key(base + 4);             /* E */
  press_key(base + 7);             /* G */
  press_key(base + 11);            /* B */
  tick();
  shot("chord");
  if (strcmp(chord_txt, "Cmaj7")) {
    printf("chord: %s, want Cmaj7\n", chord_txt);
    return 1;
  }
  if (notes_txt[0] != 'C') {
    printf("notes: %s\n", notes_txt);
    return 1;
  }
  int base_note = BOARD_SMK37 ? 60 : 53 + (12 - 7);
  (void)base_note;
  for (int i = 0; i < 4; i++) {
    int note = (BOARD_SMK37 ? 48 : 53) + base + (i == 0 ? 0 : i == 1 ? 4
                                                          : i == 2   ? 7
                                                                     : 11);
    if (!sent_note(note)) {
      printf("no note on %d in the MIDI out\n", note);
      return 1;
    }
  }
  if (ink() < 3000) {
    printf("the chord screen is nearly blank (%d)\n", ink());
    return 1;
  }

  release_key(base + 0);
  release_key(base + 4);
  release_key(base + 7);
  release_key(base + 11);
  tick();

  /* the colour protocol: pad 1 green-ish, pad 2 red-ish */
  midi_push(0x9F, 36, 40);
  midi_push(0x9F, 37, 100);
  tick();
  if (pad_count()) {
    if (!pad_rgb[0] || !pad_rgb[1]) {
      printf("colour: a pad stayed dark\n");
      return 1;
    }
    if (pad_rgb[0] == pad_rgb[1]) {
      printf("colour: both pads the same\n");
      return 1;
    }
  }
#if !BOARD_SMK37
  else if (!(ledkeys >> WHITE_KEY[0] & 1u) || !(ledkeys >> WHITE_KEY[1] & 1u)) {
    printf("colour: the FM-1's key LEDs did not take the colours\n");
    return 1;
  }
#endif

  /* the MAP page and an edit */
  press_btn(BTN_GLO);
  tick();
  release_btn(BTN_GLO);
  tick();
  shot("map");
  if (ink() < 2000) {
    printf("the map screen is nearly blank (%d)\n", ink());
    return 1;
  }
  press_btn(BTN_SEL);
  tick();
  release_btn(BTN_SEL);
  tick();
  shot("edit");
  if (ink() < 2000) {
    printf("the edit screen is nearly blank (%d)\n", ink());
    return 1;
  }

  /* SAVE stores; go home and play a chord again for the screenshot */
  press_btn(BTN_SAVE);
  tick();
  release_btn(BTN_SAVE);
  press_btn(BTN_HOME);
  tick();
  release_btn(BTN_HOME);
  press_key(base + 2); /* D */
  press_key(base + 6); /* F# */
  press_key(base + 9); /* A */
  tick();
  shot("dmaj");

  /* scale lock: C# snaps up to D (C major) */
  cfg.scale_on = 1;
  cfg.scale_root = 0;
  cfg.scale_type = SC_MAJOR;
  if (scale_note(61) != 62) {
    printf("scale: %d -> %d, want 62\n", 61, scale_note(61));
    return 1;
  }
  cfg.scale_on = 0;

  /* chord memory: hold C E G, press CHORD, release, play C -> C E G */
  release_key(base + 2);
  release_key(base + 6);
  release_key(base + 9);
  tick();
  press_key(base + 0);
  press_key(base + 4);
  press_key(base + 7);
  tick();
  press_btn(BTN_EDIT);
  tick();
  release_btn(BTN_EDIT);
  tick();
  if (!cfg.chord_on) {
    printf("chord memory did not turn on\n");
    return 1;
  }
  release_key(base + 0);
  release_key(base + 4);
  release_key(base + 7);
  tick();
  int mark = sent_count;
  press_key(base + 0);
  tick();
  int c0 = (BOARD_SMK37 ? 48 : 53) + base + 0;
  if (!sent_after(mark, c0) || !sent_after(mark, c0 + 4) || !sent_after(mark, c0 + 7)) {
    printf("chord memory did not send the shape\n");
    return 1;
  }
  release_key(base + 0);
  tick();
  press_btn(BTN_EDIT); /* off (no keys held) */
  tick();
  release_btn(BTN_EDIT);
  tick();

  /* arpeggiator: open its page, switch it on there, three keys, let it step */
  cfg.arp_rate = 2; /* 1/16 */
  cfg.arp_bpm = 120;
  cfg.arp_pat = 0;
  press_btn(BTN_ARP);
  tick();
  release_btn(BTN_ARP);
  tick(); /* opens ARP */
  press_btn(BTN_ARP);
  tick();
  release_btn(BTN_ARP);
  tick(); /* toggles on */
  if (!cfg.arp_on) {
    printf("arp did not switch on\n");
    return 1;
  }
  press_key(base + 0);
  press_key(base + 4);
  press_key(base + 7);
  tick();
  mark = sent_count;
  for (int i = 0; i < 90; i++) tick();
  {
    int ons = 0;
    for (int i = mark; i < sent_count && i < 64; i++)
      if (sent[i][0] == 0x90 && sent[i][2])
        ons++;
    if (ons < 3) {
      printf("arp: only %d notes in 900 ms\n", ons);
      return 1;
    }
  }
  release_key(base + 0);
  release_key(base + 4);
  release_key(base + 7);
  tick();
  press_btn(BTN_ARP);
  tick();
  release_btn(BTN_ARP);
  tick(); /* off */
  page_set(PG_MAIN);
  tick();

  /* knob bank: KNOB BANK, then K1 -> CC31 */
#if BOARD_SMK37
  {
    press_btn(BTN_KNOB_BANK);
    tick();
    release_btn(BTN_KNOB_BANK);
    tick();
    if (cfg.knob_bank != 1) {
      printf("knob bank did not switch\n");
      return 1;
    }
    mark = sent_count;
    encs[ENC_MASTER] = 1;
    tick();
    int v = -1;
    if (!sent_cc_after(mark, 31, &v)) {
      printf("knob bank B did not send CC31\n");
      return 1;
    }
    shot("cc");
  }
#endif

  /* the mode pages render */
  page_set(PG_ARP);
  tick();
  shot("arp");
  if (ink() < 2000) {
    printf("the arp page is nearly blank\n");
    return 1;
  }
  page_set(PG_CHORD);
  tick();
  shot("chord-mode");
  page_set(PG_SCALE);
  tick();
  shot("scale");
  page_set(PG_MAIN);
  tick();

  /* split: B3 on low channel 2, C4 on high channel 3 (default channels all 1 now) */
  page_set(PG_MAIN);
  tick();
  cfg.split_on = 1;
  cfg.split_note = 60;
  cfg.split_lo_ch = 2;
  cfg.split_hi_ch = 3;
  int kc = BOARD_SMK37 ? 12 : 7; /* C4 */
  int kb = kc - 1;               /* B3 */
  keys_held = 0;
  mark = sent_count;
  press_key(kb);
  tick();
  press_key(kc);
  tick();
  if (sent_status(mark, 59) != 0x91 || sent_status(mark, 60) != 0x92) {
    printf("split channels: B3 %02x, C4 %02x\n", sent_status(mark, 59), sent_status(mark, 60));
    return 1;
  }
  release_key(kb);
  release_key(kc);
  tick();
  cfg.split_on = 0;

  /* ARP page: K1 is rate; pad 1 is beat/tap */
  page_set(PG_ARP);
  tick();
  int rate0 = cfg.arp_rate;
  encs[ENC_TOP[0]] = 1;
  tick();
  if (cfg.arp_rate == rate0) {
    printf("arp page K1 did not change the rate\n");
    return 1;
  }
  now_ms = 10000;
  arp_tap();
  now_ms = 10500;
  arp_tap();
  if (cfg.arp_bpm < 115 || cfg.arp_bpm > 125) {
    printf("tap tempo: %d BPM\n", cfg.arp_bpm);
    return 1;
  }
  arp_beat_next = 0;
  arp_beat_tick(now_ms);
  refresh_pads();
  if (pad_count() && !pad_rgb[0]) {
    printf("arp page pad 1 beat lamp stayed dark\n");
    return 1;
  }
  page_set(PG_SPLIT);
  tick();
  shot("split");
  if (ink() < 2000) {
    printf("the split page is nearly blank\n");
    return 1;
  }
  page_set(PG_MAIN);
  tick();

  /* a long chord uses the medium face */
  {
    uint16_t pcs = (1u << 1) | (1u << 4) | (1u << 7) | (1u << 11);
    chord_name(pcs, 6, chord_txt, sizeof chord_txt);
    page_set(PG_MAIN);
    tick();
    draw_main_chord();
    shot("chord-long");
    printf("long chord: %s (big %d, mid %d)\n", chord_txt, aa_width(chord_txt, AA_BIG), aa_width(chord_txt, AA_MID));
  }

  /* Hingebox (the host) drives the screen and the pads (2026-11-12): hello answered, groove, tempo, track,
   * playhead, pads; then a knob's label and value; and it all goes when the host goes quiet */
  {
    page_set(PG_MAIN);
    now_ms += 2000;
    int before = sent_count;
    midi_push(0xBF, 119, 1);
    tick();
    if (sent_count - before < 3) {
      printf("hello: %d replies, want 3\n", sent_count - before);
      return 1;
    }
    const char* g = "sunday keys";
    midi_push(0xBF, 110, 0);
    for (int i = 0; g[i]; i++) midi_push(0xBF, 109, (uint8_t)g[i]);
    midi_push(0xBF, 108, 0);
    tick();
    const char* t = "kick";
    midi_push(0xBF, 110, 1);
    for (int i = 0; t[i]; i++) midi_push(0xBF, 109, (uint8_t)t[i]);
    midi_push(0xBF, 108, 0);
    midi_push(0xBF, 111, 0);
    midi_push(0xBF, 112, 96);
    tick();
    for (int p = 0; p < 16; p++) midi_push(0x9F, (uint8_t)(36 + p), (p % 4 == 0) ? 10 : 0);
    midi_push(0xBF, 113, 4);
    tick();
    if (strcmp(hb_text[0], "sunday keys") || strcmp(hb_text[1], "kick") || hb_bpm != 96 || hb_step != 4) {
      printf("host: groove \"%s\" track \"%s\" bpm %d step %d\n", hb_text[0], hb_text[1], hb_bpm, hb_step);
      return 1;
    }
    press_key(base + 0);
    press_key(base + 4);
    press_key(base + 7);
    tick();
    shot("hingebox");
    release_key(base + 0);
    release_key(base + 4);
    release_key(base + 7);
    tick();
    const char* k = "reverb";
    midi_push(0xBF, 110, 2);
    for (int i = 0; k[i]; i++) midi_push(0xBF, 109, (uint8_t)k[i]);
    midi_push(0xBF, 108, 0);
    midi_push(0xBF, 107, 80);
    tick();
    shot("knob");
    /* screen v3: the knob in its deck colour (CC 106), the track's colour (CC 104), a fill (CC 105), the beat */
    if (strcmp(ov_name, "reverb") || !ov_pct) {
      printf("overlay: \"%s\"\n", ov_name);
      return 1;
    }
    if (ov_col != DECK[6]) { /* no CC 106 sent: the deck's reverb knob is white (PASS3 item 8) */
      printf("overlay colour: reverb is not the deck's white\n");
      return 1;
    }
    {
      int caps = -1;
      for (int i = 0; i < sent_count && i < 64; i++)
        if (sent[i][0] == 0xBF && sent[i][1] == 118)
          caps = sent[i][2];
      if (caps != 15) {
        printf("hello: capabilities %d, want 15\n", caps);
        return 1;
      }
    }
    if (hue_colour(10) != DECK[0] || hue_colour(80) != DECK[1]) {
      printf("hue_colour: the deck's kick / snare hues are not the deck's colours\n");
      return 1;
    }
    {
      const char* k3 = "drive";
      midi_push(0xBF, 110, 2);
      for (int i = 0; k3[i]; i++) midi_push(0xBF, 109, (uint8_t)k3[i]);
      midi_push(0xBF, 108, 0);
      midi_push(0xBF, 107, 100);
      tick();
      shot("knob-drive");
      if (ov_col != DECK[1]) {
        printf("overlay colour: drive is not the deck's blue\n");
        return 1;
      }
    }
    const char* k2 = "swing";
    midi_push(0xBF, 110, 2);
    for (int i = 0; k2[i]; i++) midi_push(0xBF, 109, (uint8_t)k2[i]);
    midi_push(0xBF, 108, 0);
    midi_push(0xBF, 106, 2); /* clap pink */
    midi_push(0xBF, 107, 40);
    tick();
    shot("knob-colour");
    if (ov_col != DECK[2]) {
      printf("overlay colour: not the clap's pink\n");
      return 1;
    }
    now_ms += 1600; /* the overlay goes */
    const char* t2 = "snare";
    midi_push(0xBF, 110, 1);
    for (int i = 0; t2[i]; i++) midi_push(0xBF, 109, (uint8_t)t2[i]);
    midi_push(0xBF, 108, 0);
    midi_push(0xBF, 104, 1); /* snare blue */
    for (int p = 0; p < 16; p++) midi_push(0x9F, (uint8_t)(36 + p), (p == 4 || p == 12 || p == 14) ? 80 : 0);
    midi_push(0xBF, 113, 9);
    tick();
    press_key(base + 2); /* D F A C: dm7 */
    press_key(base + 5);
    press_key(base + 9);
    press_key(base + 12);
    tick();
    shot("hb-chord");
    release_key(base + 2);
    release_key(base + 5);
    release_key(base + 9);
    release_key(base + 12);
    tick();
    midi_push(0xBF, 113, 6);
    tick();
    shot("hb-beat");
    midi_push(0xBF, 105, 1); /* a fill */
    midi_push(0xBF, 113, 8);
    tick();
    shot("hb-fill");
    if (bop_mood() != BOP_WOW) {
      printf("bop: not surprised by the fill (%d)\n", bop_mood());
      return 1;
    }
    midi_push(0xBF, 113, 127);
    tick();
    shot("hb-stopped");
    now_ms += 5000;
    tick();
    tick();
    if (hb_active()) {
      printf("host: still active after 5 s of quiet\n");
      return 1;
    }
  }

  /* screen v3: the modes in the status bar and beside Bop, scale dots and the split on the strip, BLE */
  {
    page_set(PG_MAIN);
    cfg.arp_on = 1;
    cfg.scale_on = 1;
    cfg.scale_root = 2;
    cfg.scale_type = SC_DORIAN;
    cfg.split_on = 1;
    cfg.split_note = 60;
    bt_ready = 1;
    ui_full();
    press_key(base + 2);
    press_key(base + 5);
    tick();
    shot("modes");
    cfg.chord_on = 1; /* all four on: the words that fit, then "+n" */
    ui_full();
    tick();
    shot("modes-all");
    cfg.chord_on = 0;
    release_key(base + 2);
    release_key(base + 5);
    cfg.arp_on = cfg.scale_on = cfg.split_on = 0;
    bt_ready = 0;
    cfg.transpose = 12;
    ui_full();
    tick();
    now_ms += 61000; /* a minute of nothing: Bop sleeps */
    tick();
    shot("sleepy");
    if (bop_mood() != BOP_SLEEPY) {
      printf("bop: not sleepy after a minute (%d)\n", bop_mood());
      return 1;
    }
    press_key(base + 0);
    tick();
    if (bop_mood() == BOP_SLEEPY) {
      printf("bop: a key did not wake him\n");
      return 1;
    }
    release_key(base + 0);
    cfg.transpose = 0;
    tick();
  }

  printf("%s: ok, save called %d, sent %d, chord \"%s\", arp/banks/scale/chord ok\n",
         BOARD_SMK37 ? "smk37" : "fm1", saved_ok, sent_count, chord_txt);
  return 0;
}

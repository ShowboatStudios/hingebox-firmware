/* SPDX-License-Identifier: GPL-3.0-only */
/* Anti-aliased sans text for the performance screen: Inter Tight (SIL OFL 1.1, LICENSES/OFL-InterTight.txt),
 * pre-rasterised into 4-bit alpha glyphs by tools/gen_aa_font.py. Two faces: AA_SMALL 13 px (notes, status) and
 * AA_BIG 44 px (the chord). Drawn straight into the framebuffer, alpha-blended. */
typedef struct {
  uint16_t off, adv;
  int8_t bx;
  uint8_t by, bw, bh;
} aag_t;
typedef struct {
  uint8_t h, asc, first, last, nex;
  uint16_t nk;
  const aag_t* g;
  const uint8_t* data;
  const uint16_t* kkey;
  const int8_t* kd;
  const uint8_t* ex;
  uint8_t psh;
  const uint8_t* hc;
  const uint8_t* map;
} aafont_t;
#include "inter_font.h"

static uint16_t aa_lut[3][256];
static uint8_t aa_lut_ok[3];

/* the canonical Huffman code -> a 256-entry prefix table (aatext.c's) */
static const uint16_t* aa_hc_lut(const uint8_t* hc, int slot) {
  if (aa_lut_ok[slot]) {
    return aa_lut[slot];
  }
  uint32_t len, code = 0, sym = 15, i;
  for (i = 0; i < 256u; i++) {
    aa_lut[slot][i] = 0;
  }
  for (len = 1; len <= 8u; len++, code <<= 1) {
    for (i = 0; i < hc[len - 1u]; i++, code++, sym++) {
      uint32_t a = code << (8u - len), n = 1u << (8u - len);
      while (n--) {
        aa_lut[slot][a + n] = (uint16_t)(hc[sym] | len << 8);
      }
    }
  }
  aa_lut_ok[slot] = 1;
  return aa_lut[slot];
}

static inline uint32_t aa_fold(const aafont_t* f, uint32_t c) {
  return c >= 'a' && c <= 'z' && f->last < 'a' ? c - 32u : c;
}
static int32_t aa_glyph_at(const aafont_t* f, uint32_t ch) {
  uint32_t i;
  if (ch >= f->first && ch <= f->last) {
    return !f->map ? (int32_t)(ch - f->first) : f->map[ch - f->first] == 255u ? -1 : (int32_t)f->map[ch - f->first];
  }
  for (i = 0; i < f->nex; i++) {
    if (f->ex[i] == ch) {
      return (int32_t)(f->last - f->first + 1u + i);
    }
  }
  return -1;
}
static uint32_t aa_glyph(const aafont_t* f, uint32_t ch) {
  int32_t k;
  if (ch >= 'a' && ch <= 'z' && f->last < 'a') {
    ch -= 32u;
  }
  if ((k = aa_glyph_at(f, ch)) < 0 && (k = aa_glyph_at(f, '?')) < 0) {
    k = 0;
  }
  return (uint32_t)k << f->psh;
}
static int32_t aa_kern(const aafont_t* f, uint32_t a, uint32_t b) {
  uint32_t key = (a << 8) | b, lo = 0, hi = f->nk;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2u;
    if (f->kkey[m] == key) {
      return f->kd[m];
    }
    if (f->kkey[m] < key) {
      lo = m + 1u;
    } else {
      hi = m;
    }
  }
  return 0;
}

/* one glyph's Huffman bit stream: runs of (value, count) across the ink box */
static void aa_blit(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint8_t* d, const uint8_t* hc, uint16_t fg,
                    int slot) {
  const uint16_t* lut = aa_hc_lut(hc, slot);
  uint32_t total = w * h, i = 0, run = 0, v = 0, acc = 0, nb = 0;
  if (y + (int32_t)h < 0 || y >= SCREEN_H) {
    return;
  }
  while (i < total) {
    if (!run) {
      uint32_t e, s;
      while (nb < 16u) {
        acc = (acc << 8) | *d++;
        nb += 8u;
      }
      e = lut[(acc >> (nb - 8u)) & 255u];
      if (e) {
        nb -= e >> 8;
        s = e & 255u;
      } else {
        uint32_t code = 0, first = 0, idx = 0, len = 0;
        for (;;) {
          code |= (acc >> --nb) & 1u;
          if (code - first < hc[len]) {
            break;
          }
          idx += hc[len];
          first = (first + hc[len]) << 1;
          code <<= 1;
          len++;
        }
        s = hc[15u + idx + code - first];
      }
      v = s & 15u;
      run = (s >> 4) + 1u;
    }
    uint32_t n = run < total - i ? run : total - i;
    if (v) {
      for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = i + k, gx = idx % w, gy = idx / w;
        int32_t px = x + (int32_t)gx, py = y + (int32_t)gy;
        if ((uint32_t)px < SCREEN_W && (uint32_t)py < SCREEN_H) {
          uint16_t* p = &fb[(uint32_t)py * SCREEN_W + (uint32_t)px];
          *p = fb_swap(blend(fb_swap(*p), fg, v));
        }
      }
      fb_dirty(y < 0 ? 0 : y, y + (int32_t)h > SCREEN_H ? SCREEN_H : y + (int32_t)h);
    }
    i += n;
    run -= n;
  }
}

static const aafont_t* aa_font(int face) { return face == AA_BIG ? &AF_L : face == AA_MID ? &AF_M : &AF_S; }

int aa_text(int x, int y, const char* s, uint16_t color, int face) {
  const aafont_t* f = aa_font(face);
  int32_t pen = 0;
  uint32_t prev = 0;
  int slot = face == AA_BIG ? 2 : face == AA_MID ? 1 : 0;
  for (; *s; s++) {
    uint32_t c = aa_fold(f, (uint8_t)*s);
    if (prev) {
      pen += aa_kern(f, prev, c);
    }
    const aag_t* g = &f->g[aa_glyph(f, c)];
    if (g->bw) {
      aa_blit(x + ((pen + 8) >> 4) + g->bx, y + g->by, g->bw, g->bh, f->data + g->off, f->hc, color, slot);
    }
    pen += g->adv;
    prev = c;
  }
  return x + ((pen + 8) >> 4);
}

int aa_width(const char* s, int face) {
  const aafont_t* f = aa_font(face);
  int32_t pen = 0;
  uint32_t prev = 0;
  for (; *s; s++) {
    uint32_t c = aa_fold(f, (uint8_t)*s);
    if (prev) {
      pen += aa_kern(f, prev, c);
    }
    pen += f->g[aa_glyph(f, c)].adv;
    prev = c;
  }
  return (pen + 8) >> 4;
}

int aa_height(int face) { return aa_font(face)->h; }

int aa_text_c(int x, int y, int w, const char* s, uint16_t color, int face) {
  int tw = aa_width(s, face);
  return aa_text(x + (w - tw) / 2, y, s, color, face);
}

int aa_text_r(int xr, int y, const char* s, uint16_t color, int face) {
  int tw = aa_width(s, face);
  return aa_text(xr - tw, y, s, color, face);
}

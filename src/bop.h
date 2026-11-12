/* Bop, Hingebox's robot (the cover show's pictogram), on the keyboard's 240x240 screen (Hingebox fork, 2026-11-12).
 * Built from rects, rings and lines so it costs a few rows to redraw: a rounded body with ring eyes, keycap feet,
 * an antenna whose tip is the one spot of orange. Moods: idle, wow (notes held), happy (a chord), and a beat bob. */
#ifndef BOP_H
#define BOP_H

#include "fm1.h"

enum { BOP_IDLE, BOP_WOW, BOP_HAPPY, BOP_SLEEPY };

static void bop_disc(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) {
      dx++;
    }
    rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
  }
}

static void bop_ring(int cx, int cy, int r, int w, uint16_t c, uint16_t bg) {
  bop_disc(cx, cy, r, c);
  bop_disc(cx, cy, r - w, bg);
}

/* a rounded rect: corners cut by radius r (filled), then an outline of w px in c on top of fill f */
static void bop_round(int x, int y, int wd, int ht, int r, uint16_t f, uint16_t c, int w) {
  rect(x + r, y, wd - 2 * r, ht, c);
  rect(x, y + r, wd, ht - 2 * r, c);
  bop_disc(x + r, y + r, r, c);
  bop_disc(x + wd - 1 - r, y + r, r, c);
  bop_disc(x + r, y + ht - 1 - r, r, c);
  bop_disc(x + wd - 1 - r, y + ht - 1 - r, r, c);
  int ri = r > w ? r - w : 0;
  int xi = x + w, yi = y + w, wi = wd - 2 * w, hi = ht - 2 * w;
  rect(xi + ri, yi, wi - 2 * ri, hi, f);
  rect(xi, yi + ri, wi, hi - 2 * ri, f);
  if (ri) {
    bop_disc(xi + ri, yi + ri, ri, f);
    bop_disc(xi + wi - 1 - ri, yi + ri, ri, f);
    bop_disc(xi + ri, yi + hi - 1 - ri, ri, f);
    bop_disc(xi + wi - 1 - ri, yi + hi - 1 - ri, ri, f);
  }
}

/* Bop at (x, y) = top-left of a box s wide and about 1.2 s tall, on background bg. lift: px up (the beat bob).
 * ink: the lines; body: the fill; tip: the antenna's spot of colour. */
static void bop_draw(int x, int y, int s, int mood, int lift, uint16_t bg, uint16_t ink, uint16_t body, uint16_t tip) {
  int w = s / 22 + 1; /* line width */
  rect(x, y, s, s * 6 / 5 + 4, bg);
  int bx = x + s / 12, by = y + s / 4 - lift, bw = s - s / 6, bh = s * 7 / 10;
  /* antenna: a stem leaning right, and the tip */
  int sx = x + s / 2, sy = by;
  for (int i = 0; i < s / 5; i++) {
    rect(sx + i / 3, sy - i, w, 1, ink);
  }
  bop_ring(sx + s / 15, sy - s / 5, s / 14 + w, w, ink, ink);
  bop_disc(sx + s / 15, sy - s / 5, s / 14, tip);
  /* body */
  bop_round(bx, by, bw, bh, s / 7, body, ink, w);
  /* eyes */
  int ey = by + bh * 2 / 5, ex0 = bx + bw * 3 / 10, ex1 = bx + bw * 7 / 10;
  int er = mood == BOP_WOW ? s / 9 : s / 12;
  for (int k = 0; k < 2; k++) {
    int ex = k ? ex1 : ex0;
    if (mood == BOP_SLEEPY) {
      rect(ex - er, ey, 2 * er, w, ink);
    } else {
      bop_ring(ex, ey, er, w, ink, body);
      bop_disc(ex, ey, s / 30 + 1, ink);
    }
  }
  /* mouth: a line, an "o" (wow), a smile (happy) */
  int my = by + bh * 3 / 4, mx = bx + bw / 2;
  if (mood == BOP_WOW) {
    bop_ring(mx, my, s / 16 + w, w, ink, body);
  } else if (mood == BOP_HAPPY) {
    int r = s / 9;
    for (int a = 0; a <= r; a++) { /* a shallow U */
      int dy = (a * a) / (r + 1) / 2;
      rect(mx - a, my - dy, w, w, ink);
      rect(mx + a, my - dy, w, w, ink);
    }
  } else {
    rect(mx - s / 10, my, s / 5, w, ink);
  }
  /* feet: two keycaps */
  int fy = by + bh + 1 + lift, fw = s / 5, fh = s / 10 > 3 ? s / 10 : 3;
  rect(x + s / 2 - s / 4 - fw / 2, fy, fw, fh, ink);
  rect(x + s / 2 + s / 4 - fw / 2, fy, fw, fh, ink);
}

#endif

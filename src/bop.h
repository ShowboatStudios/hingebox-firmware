/* Bop, Hingebox's robot (the cover show's pictogram), on the keyboard's 240x240 screen (Hingebox fork, 2026-11-12).
 * Built from rects, rings and lines so it costs a few rows to redraw. One Bop everywhere (PASS3 item 6, the app's
 * UI/Bop.swift): a 4:3 rounded head, ring eyes with pupils, pill feet 2.5 times as wide as tall and fully round,
 * the antenna ball in the current accent. On the black screen: the screen's black inside a white outline.
 * Moods: idle, wow (surprised: a fill or a drop), happy (a chord), sleepy (nothing played for a minute).
 * lift: px up (the beat bob); look: the pupils' x offset in px (toward the notes); blink: eyes shut a moment. */
#ifndef BOP_H
#define BOP_H

#include "fm1.h"

enum { BOP_IDLE, BOP_WOW, BOP_HAPPY, BOP_SLEEPY };

static void bop_disc(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) { /* r + 1/2: round, without a nub at each end */
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
  if (wi <= 0 || hi <= 0) {
    return;
  }
  rect(xi + ri, yi, wi - 2 * ri, hi, f);
  rect(xi, yi + ri, wi, hi - 2 * ri, f);
  if (ri) {
    bop_disc(xi + ri, yi + ri, ri, f);
    bop_disc(xi + wi - 1 - ri, yi + ri, ri, f);
    bop_disc(xi + ri, yi + hi - 1 - ri, ri, f);
    bop_disc(xi + wi - 1 - ri, yi + hi - 1 - ri, ri, f);
  }
}

/* a pill: a rect with fully round ends (an odd height, so the ends are whole discs), a w px outline in c around f */
static void bop_pill(int x, int y, int wd, int ht, uint16_t f, uint16_t c, int w) {
  bop_round(x, y, wd, ht, (ht - 1) / 2, f, c, w);
}

/* Bop at (x, y) = top-left of a box s wide and about 1.2 s tall, on background bg. lift: px up (the beat bob).
 * ink: the outline and face; body: the fill (the screen's black on the black screen); tip: the antenna ball. */
static void bop_draw_ex(int x, int y, int s, int mood, int lift, int look, int blink, uint16_t bg, uint16_t ink,
                        uint16_t body, uint16_t tip) {
  int w = s / 22 + 1; /* line width */
  rect(x, y, s, s * 6 / 5 + 4, bg);
  /* the head: 4:3, low enough that the ball stays inside the box at the top of a bob */
  int bw = s - s / 6, bh = bw * 3 / 4, bx = x + (s - bw) / 2, by = y + s / 4 + 4 - lift;
  /* antenna: a stem leaning right, and the ball (bigger when surprised) */
  int sx = x + s / 2, sy = by;
  for (int i = 0; i < s / 5; i++) {
    rect(sx + i / 3, sy - i, w, 1, ink);
  }
  int tr = s / 16 + (mood == BOP_WOW ? 1 : 0), tw = w > 2 ? 2 : w; /* the ball, on top of the stem */
  int ty = sy - s / 5 - tr - tw + 1;
  bop_disc(sx + s / 15, ty, tr + tw, ink);
  bop_disc(sx + s / 15, ty, tr, tip);
  /* head */
  bop_round(bx, by, bw, bh, s / 7, body, ink, w);
  /* eyes: a ring with a pupil at every size, wider when surprised */
  int ey = by + bh / 3, ex0 = bx + bw * 3 / 10, ex1 = bx + bw - 1 - bw * 3 / 10;
  int er = s / 10;
  if (er < 5) {
    er = 5;
  }
  if (mood == BOP_WOW && s >= 70) {
    er += er / 4; /* wider when surprised (at small sizes the "o" mouth says it) */
  }
  int ew = s >= 70 ? w : 2;
  int pp = s >= 70 ? 4 : 2; /* the pupil, square */
  int lk = look < -(er - ew - pp / 2) ? -(er - ew - pp / 2) : look > er - ew - pp / 2 ? er - ew - pp / 2 : look;
  for (int k = 0; k < 2; k++) {
    int ex = k ? ex1 : ex0;
    if (mood == BOP_SLEEPY || blink) {
      rect(ex - er, ey, 2 * er + 1, w, ink); /* shut */
    } else if (mood == BOP_HAPPY) {
      /* happy: ^ ^ eyes (a ring's top half) */
      bop_ring(ex, ey + er / 2, er, ew, ink, body);
      rect(ex - er, ey + er / 2, 2 * er + 1, er + 1, body);
    } else {
      bop_ring(ex, ey, er, ew, ink, body);
      rect(ex - pp / 2 + lk, ey - pp / 2, pp, pp, ink);
    }
  }
  /* mouth: a line, an "o" (wow), a smile (happy), a small line (sleepy) */
  int my = by + bh * 4 / 5 - w / 2, mx = bx + bw / 2;
  if (mood == BOP_WOW) {
    int ow = w > 2 ? 2 : w;
    bop_ring(mx, my - 1, s / 20 + ow, ow, ink, body);
  } else if (mood == BOP_HAPPY) {
    int r = s / 9;
    for (int a = 0; a <= r; a++) { /* a shallow U */
      int dy = (a * a) / (r + 1) / 2;
      rect(mx - a, my - dy, w, w, ink);
      rect(mx + a, my - dy, w, w, ink);
    }
  } else if (mood == BOP_SLEEPY) {
    rect(mx - s / 20, my, s / 10 + 1, w, ink);
  } else {
    rect(mx - s / 10, my, s / 5, w, ink);
  }
  /* feet: two pills, 2.5 times as wide as tall, under the head's quarters */
  int fh = (s / 9) | 1, fy = by + bh + 1 + lift;
  if (fh < 5) {
    fh = 5;
  }
  int fw = (fh * 5 + 1) / 2, fl = (fh - 1) / 2 - 1;
  fl = fl < 2 ? 2 : fl > w ? w : fl;
  int fx = bx + bw / 4 - fw / 2;
  bop_pill(fx, fy, fw, fh, body, ink, fl);
  bop_pill(2 * bx + bw - fx - fw, fy, fw, fh, body, ink, fl); /* its mirror */
}

static void bop_draw(int x, int y, int s, int mood, int lift, uint16_t bg, uint16_t ink, uint16_t body, uint16_t tip) {
  bop_draw_ex(x, y, s, mood, lift, 0, 0, bg, ink, body, tip);
}

#endif

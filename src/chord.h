/* Chord naming: a set of sounding pitch classes -> "Cmaj7", "C#m7/F", ... (pure C, no platform: the host
 * test includes it too). Spelt as Hingebox spells the chord the keys make (Stage/LiveKeys.swift ChordName): its
 * note names and suffixes, with b and # for the app's flat and sharp signs (the big font has no music signs; it
 * carries the suffixes' lowercase letters as extras, tools/aa_extend.py). */
#pragma once
#include <stdint.h>

static const char* const NOTE_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

/* an exact pitch-class set (rooted at 0) -> suffix; first match wins */
typedef struct {
  uint16_t mask;
  const char* suffix;
} chord_tpl_t;

#define PC(i) (1u << (i))
static const chord_tpl_t CHORDS[] = {
    {PC(0) | PC(7), "5"},                          /* power */
    {PC(0) | PC(4) | PC(7), ""},                   /* major */
    {PC(0) | PC(3) | PC(7), "m"},                  /* minor */
    {PC(0) | PC(3) | PC(6), "dim"},                /* diminished */
    {PC(0) | PC(4) | PC(8), "aug"},                /* augmented */
    {PC(0) | PC(2) | PC(7), "sus2"},               /* sus2 */
    {PC(0) | PC(5) | PC(7), "sus4"},               /* sus4 */
    {PC(0) | PC(4) | PC(7) | PC(9), "6"},          /* major 6 */
    {PC(0) | PC(3) | PC(7) | PC(9), "m6"},         /* minor 6 */
    {PC(0) | PC(4) | PC(7) | PC(11), "maj7"},      /* major 7 */
    {PC(0) | PC(4) | PC(7) | PC(10), "7"},         /* dominant 7 */
    {PC(0) | PC(3) | PC(7) | PC(10), "m7"},        /* minor 7 */
    {PC(0) | PC(3) | PC(6) | PC(10), "m7b5"},      /* half-diminished */
    {PC(0) | PC(3) | PC(6) | PC(9), "dim7"},       /* diminished 7 */
    {PC(0) | PC(2) | PC(4) | PC(7), "add9"},       /* add9 */
    {PC(0) | PC(2) | PC(3) | PC(7), "madd9"},      /* minor add9 */
    {PC(0) | PC(2) | PC(4) | PC(7) | PC(10), "9"}, /* dominant 9 */
    {PC(0) | PC(2) | PC(4) | PC(7) | PC(11), "maj9"},
    {PC(0) | PC(2) | PC(3) | PC(7) | PC(10), "m9"},
    {PC(0) | PC(5) | PC(7) | PC(10), "7sus4"},
    {PC(0) | PC(4) | PC(8) | PC(11), "maj7#5"},
    {PC(0) | PC(2) | PC(4) | PC(7) | PC(9), "6/9"},
};

static inline uint16_t pc_rot(uint16_t pcs, int root) {
  return (uint16_t)(((pcs >> root) | (pcs << (12 - root))) & 0x0FFFu);
}

static inline int chord_count(uint16_t pcs) {
  int n = 0;
  for (; pcs; pcs &= (uint16_t)(pcs - 1u)) {
    n++;
  }
  return n;
}

/* pcs: the pitch classes held (bit n = n); bass: the lowest note's pitch class, or -1.
 * out gets "Cmaj7", "C#m7/F" or a single "C4"-style note name; returns the length. No
 * match: "?" (the notes are still shown separately). */
static inline int chord_name(uint16_t pcs, int bass, char* out, int cap) {
  const chord_tpl_t* hit = 0;
  int root = -1, n = chord_count(pcs), len = 0, slash = 0, r;
  out[0] = 0;
  if (!n || cap < 2) {
    return 0;
  }
  if (n == 1) {
    r = 0;
    while (!(pcs >> r & 1u)) {
      r++;
    }
    while (NOTE_NAME[r][len]) {
      out[len] = NOTE_NAME[r][len];
      len++;
    }
    out[len] = 0;
    return len;
  }
  /* a root inside the chord, the bass first (an inversion gets its slash); the first template
   * that matches the whole set exactly names it */
  for (int pass = 0; pass < 2 && !hit; pass++) {
    for (int k = 0; k < 12 && !hit; k++) {
      int cand = pass == 0 ? bass : k;
      uint16_t rel;
      if (cand < 0 || !(pcs >> cand & 1u) || (pass == 0 && cand != bass)) {
        continue;
      }
      rel = pc_rot(pcs, cand);
      for (unsigned t = 0; t < sizeof CHORDS / sizeof CHORDS[0]; t++) {
        if (CHORDS[t].mask == rel) {
          hit = &CHORDS[t];
          root = cand;
          break;
        }
      }
    }
  }
  if (!hit) {
    out[0] = '?';
    out[1] = 0;
    return 1;
  }
  while (NOTE_NAME[root][len]) {
    out[len] = NOTE_NAME[root][len];
    len++;
  }
  if (bass >= 0 && bass != root) {
    slash = 1;
  }
  for (const char* s = hit->suffix; *s && len < cap - 4; s++) {
    out[len++] = *s;
  }
  if (slash && len < cap - 3) {
    out[len++] = '/';
    for (int i = 0; NOTE_NAME[bass][i] && len < cap - 1; i++) {
      out[len++] = NOTE_NAME[bass][i];
    }
  }
  out[len] = 0;
  return len;
}

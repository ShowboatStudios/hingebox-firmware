/* SPDX-License-Identifier: GPL-3.0-only */
/* When the audio engine (ALNK0 DMA + its interrupt) runs, with FOMA_AUDIO_GATE: only while the host streams to us.
 *
 * Why: on the SMK-37 Elite (2026-11-12) the usb_audio build enumerated only in the OCT- rescue, where the audio
 * engine never starts. A controller's audio() is only the host's audio passed through (src/main.c), so there is
 * nothing to render until the host picks the streaming interface's alt setting 1. Gated, the device enumerates
 * exactly as the rescue does (no ALNK0 interrupt, every TIMER5 USB poll on time), and the engine starts once the
 * host has held alt 1 for AG_SETTLE_MS (macOS and iOS flip alt 1 / alt 0 while they probe a new device), and stops
 * again on alt 0, a bus reset, unconfigure or suspend.
 *
 * Pure logic (no registers): fm1.c steps it from the main loop, tests/logic_test.c checks it on the computer. */
#ifndef AUDIO_GATE_H
#define AUDIO_GATE_H
#include <stdint.h>

#ifndef AG_SETTLE_MS
#define AG_SETTLE_MS 100u
#endif
enum { AG_NONE = 0, AG_START = 1, AG_STOP = 2 };
typedef struct {
  uint8_t on;     /* the engine runs */
  uint8_t held;   /* the host wants it; since `since` */
  uint32_t since; /* ms */
} audio_gate_t;

/* want: the host streams (configured, alt 1, not suspended). Returns what to do with the engine now. */
static inline int audio_gate_step(audio_gate_t* g, int want, uint32_t now_ms) {
  if (!want) {
    g->held = 0;
    if (g->on) {
      g->on = 0;
      return AG_STOP;
    }
    return AG_NONE;
  }
  if (g->on) {
    return AG_NONE;
  }
  if (!g->held) {
    g->held = 1;
    g->since = now_ms;
    return AG_NONE;
  }
  if ((uint32_t)(now_ms - g->since) >= AG_SETTLE_MS) {
    g->on = 1;
    return AG_START;
  }
  return AG_NONE;
}
#endif

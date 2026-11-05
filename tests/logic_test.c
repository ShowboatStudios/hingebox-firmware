/* The app's pure half on the computer: chord names and the MIDI encoding of a mapping. */
#include <stdio.h>
#include <string.h>
#include "chord.h"
#include "mapping.h"

static int fails, checks;
static void eq(const char* what, const char* got, const char* want) {
  checks++;
  if (strcmp(got, want)) {
    printf("FAIL %s: \"%s\" (want \"%s\")\n", what, got, want);
    fails++;
  }
}
static void eqi(const char* what, int got, int want) {
  checks++;
  if (got != want) {
    printf("FAIL %s: %d (want %d)\n", what, got, want);
    fails++;
  }
}
static void chord(int a, int b, int c, int d, int e, int bass, const char* want) {
  uint16_t m = 0;
  int v[5] = {a, b, c, d, e};
  char name[24] = {0};
  for (int i = 0; i < 5; i++) {
    if (v[i] >= 0)
      m |= (uint16_t)(1u << v[i]);
  }
  chord_name(m, bass, name, sizeof name);
  eq("chord", name, want);
}

int main(void) {
  chord(0, 7, -1, -1, -1, 0, "C5");
  chord(0, 4, 7, -1, -1, 0, "C");
  chord(0, 3, 7, -1, -1, 0, "CMIN");
  chord(2, 5, 9, -1, -1, 2, "DMIN");
  chord(0, 4, 7, 11, -1, 0, "CMAJ7");
  chord(0, 4, 7, 10, -1, 0, "C7");
  chord(0, 3, 7, 10, -1, 0, "CMIN7");
  chord(4, 7, 0, -1, -1, 4, "C/E");
  chord(7, 11, 2, 5, -1, 7, "G7");     /* G B D F: the bass is the root */
  chord(0, 4, 7, 11, 2, 4, "CMAJ9/E"); /* the same chord over E: a slash */
  chord(0, 2, 7, -1, -1, 0, "CSUS2");
  chord(0, 5, 7, -1, -1, 0, "CSUS4");
  chord(0, 4, 8, -1, -1, 0, "CAUG");
  chord(0, 3, 6, 10, -1, 0, "CMIN7B5");
  chord(0, 3, 6, 9, -1, 0, "CDIM7");
  chord(9, 0, 4, 7, -1, 9, "AMIN7");
  chord(9, -1, -1, -1, -1, 9, "A");
  chord(0, 1, 4, -1, -1, 0, "?");
  {
    char b[8] = "x";
    eqi("empty chord", chord_name(0, -1, b, sizeof b), 0);
    eq("empty text", b, "");
  }

  map_t m = {MSG_CC, 21, 2, MODE_ABS};
  uint8_t msg[3];
  eqi("cc sends", map_message(&m, 100, 1, msg), 1);
  eqi("cc status", msg[0], 0xB1);
  eqi("cc num", msg[1], 21);
  eqi("cc value", msg[2], 100);
  m.type = MSG_NOTE, m.num = 60;
  map_message(&m, 91, 1, msg);
  eqi("note on", msg[0], 0x91);
  eqi("note vel", msg[2], 91);
  map_message(&m, 0, 0, msg);
  eqi("note off status", msg[0], 0x81);
  eqi("note off vel", msg[2], 0);
  m.type = MSG_PC, m.num = 12;
  eqi("pc release silent", map_message(&m, 0, 0, msg), 0);
  eqi("pc press", map_message(&m, 0, 1, msg), 1);
  eqi("pc d1", msg[1], 12);
  m.type = MSG_PITCH;
  map_message(&m, 127, 1, msg);
  eqi("bend status", msg[0], 0xE1);
  eqi("bend top", msg[1] | (msg[2] << 7), 16383);
  map_message(&m, 0, 1, msg);
  eqi("bend bottom", msg[1] | (msg[2] << 7), 0);

  map_t km = {MSG_NOTE, 48, 1, MODE_MOM};
  eqi("key message", key_message(&km, 60, 101, 1, msg), 1);
  eqi("key note", msg[1], 60);
  eqi("key vel", msg[2], 101);
  eqi("key out of range", key_message(&km, 200, 100, 1, msg), 0);
  map_default(CTL_PAD, 3, 1, &m);
  eqi("pad default type", m.type, MSG_NOTE);
  eqi("pad default note", m.num, 39);
  eqi("pad default channel", m.chan, 1);
  map_default(CTL_KNOB, 1, 0, &m);
  eqi("knob default cc", m.num, 22);
  map_default(CTL_KEYS, 0, 1, &m);
  eqi("smk key base", m.num, 48);
  map_default(CTL_KEYS, 0, 0, &m);
  eqi("fm1 key base", m.num, 53);
  map_default(CTL_WHEEL, 0, 1, &m);
  eqi("wheel 0 is pitch", m.type, MSG_PITCH);
  eqi("slot of pad 2", map_of(CTL_PAD, 2), ctl_slot(CTL_PAD, 2));
  eqi("kind of pad 2", map_kind(map_of(CTL_PAD, 2)), CTL_PAD);
  eqi("kind of pedal", map_kind(N_MAP - 1), CTL_PEDAL);

  printf(fails ? "%d of %d checks FAILED\n" : "%d checks, all good\n", fails ? fails : checks, checks);
  return fails != 0;
}

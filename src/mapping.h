/* What a control sends, and how: the map table the screen edits (pure C, no platform). */
#pragma once
#include <stdint.h>

enum { MSG_CC,
       MSG_NOTE,
       MSG_PC,
       MSG_PITCH,
       MSG_COUNT };
enum { MODE_MOM,
       MODE_TOG,
       MODE_ABS,
       MODE_REL,
       MODE_COUNT };
enum { CTL_KEYS,
       CTL_PAD,
       CTL_KNOB,
       CTL_FADER,
       CTL_BUTTON,
       CTL_WHEEL,
       CTL_PEDAL,
       CTL_KIND_COUNT };

#define N_KEY 1
#define N_PAD 16
#if BOARD_SMK37
#define N_KNOB_PHYS 8
#else
#define N_KNOB_PHYS 4
#endif
#define N_KNOB_BANKS 2
#define N_KNOB (N_KNOB_PHYS * N_KNOB_BANKS)
#define N_FADER_PHYS 4
#define N_FADER_BANKS 2
#define N_FADER (N_FADER_PHYS * N_FADER_BANKS)
#define N_BUTTON 6
#define N_WHEEL 2
#define N_PEDAL 1
#define N_MAP (N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON + N_WHEEL + N_PEDAL)

typedef struct {
  uint8_t type; /* MSG_* */
  uint8_t num;  /* CC number / note / program */
  uint8_t chan; /* 1..16 */
  uint8_t mode; /* MODE_* */
} map_t;

static inline int ctl_base(int kind) {
  switch (kind) {
    case CTL_KEYS:
      return 0;
    case CTL_PAD:
      return N_KEY;
    case CTL_KNOB:
      return N_KEY + N_PAD;
    case CTL_FADER:
      return N_KEY + N_PAD + N_KNOB;
    case CTL_BUTTON:
      return N_KEY + N_PAD + N_KNOB + N_FADER;
    case CTL_WHEEL:
      return N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON;
    default:
      return N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON + N_WHEEL;
  }
}

static inline int ctl_slot(int kind, int i) { return ctl_base(kind) + i; }

static inline int map_kind(int slot) {
  if (slot < N_KEY)
    return CTL_KEYS;
  if (slot < N_KEY + N_PAD)
    return CTL_PAD;
  if (slot < N_KEY + N_PAD + N_KNOB)
    return CTL_KNOB;
  if (slot < N_KEY + N_PAD + N_KNOB + N_FADER)
    return CTL_FADER;
  if (slot < N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON)
    return CTL_BUTTON;
  if (slot < N_KEY + N_PAD + N_KNOB + N_FADER + N_BUTTON + N_WHEEL)
    return CTL_WHEEL;
  return CTL_PEDAL;
}

/* mapping i of that kind (an index inside the kind) */
static inline int map_of(int kind, int i) { return ctl_slot(kind, i); }

static inline void map_default(int kind, int i, int smk, map_t* m) {
  static const uint8_t FADER_CC[N_FADER] = {7, 11, 91, 93, 71, 74, 72, 73};
  m->type = MSG_CC;
  m->num = 0;
  m->chan = 1;
  m->mode = MODE_ABS;
  switch (kind) {
    case CTL_KEYS:
      m->type = MSG_NOTE;
      m->num = (uint8_t)(smk ? 48 : 53);
      m->mode = MODE_MOM;
      break;
    case CTL_PAD:
      m->type = MSG_NOTE;
      m->num = (uint8_t)(36 + i);
      m->chan = 1;
      m->mode = MODE_MOM;
      break;
    case CTL_KNOB: { /* the bank's CCs: A 21..28, B 31..38 */
      int bank = i / N_KNOB_PHYS, phys = i % N_KNOB_PHYS;
      m->num = (uint8_t)((bank ? 31 : 21) + phys);
    } break;
    case CTL_FADER:
      m->num = FADER_CC[i < N_FADER ? i : 0];
      break;
    case CTL_BUTTON:
      m->num = (uint8_t)(41 + i);
      m->mode = MODE_MOM;
      break;
    case CTL_WHEEL:
      if (i == 0) {
        m->type = MSG_PITCH;
      } else {
        m->num = 1;
      }
      break;
    default:
      m->num = 64;
      m->mode = MODE_MOM;
      break;
  }
}

/* one message from a control's value (0..127). Returns 1 (send), 0 (nothing: a program change on release) */
static inline int map_message(const map_t* m, uint8_t value, int on, uint8_t msg[3]) {
  uint8_t ch = (uint8_t)(m->chan ? (m->chan > 16u ? 16u : m->chan - 1u) : 0u);
  value = value > 127u ? 127u : value;
  switch (m->type) {
    case MSG_NOTE:
      msg[0] = (uint8_t)((on ? 0x90u : 0x80u) | ch);
      msg[1] = (uint8_t)(m->num & 0x7Fu);
      msg[2] = on ? value : 0u;
      return 1;
    case MSG_PC:
      if (!on) {
        return 0;
      }
      msg[0] = (uint8_t)(0xC0u | ch);
      msg[1] = (uint8_t)(m->num & 0x7Fu);
      msg[2] = 0;
      return 1;
    case MSG_PITCH: {
      uint32_t v = (uint32_t)value * 16383u / 127u;
      msg[0] = (uint8_t)(0xE0u | ch);
      msg[1] = (uint8_t)(v & 0x7Fu);
      msg[2] = (uint8_t)(v >> 7);
      return 1;
    }
    default:
      msg[0] = (uint8_t)(0xB0u | ch);
      msg[1] = (uint8_t)(m->num & 0x7Fu);
      msg[2] = value;
      return 1;
  }
}

/* a keybed note from the keys mapping: its num is the lowest key's note */
static inline int key_message(const map_t* m, int note, uint8_t vel, int on, uint8_t msg[3]) {
  uint8_t ch = (uint8_t)(m->chan ? (m->chan > 16u ? 16u : m->chan - 1u) : 0u);
  msg[0] = (uint8_t)((on ? 0x90u : 0x80u) | ch);
  msg[1] = (uint8_t)(note & 0x7Fu);
  msg[2] = on ? (vel ? vel : 1u) : 0u;
  return note >= 0 && note <= 127;
}

static inline const char* msg_name(int type) {
  static const char* const N[MSG_COUNT] = {"CC", "NOTE", "PC", "PITCH"};
  return N[type < MSG_COUNT ? type : 0];
}

static inline const char* mode_name(int mode) {
  static const char* const N[MODE_COUNT] = {"MOM", "TOG", "ABS", "REL"};
  return N[mode < MODE_COUNT ? mode : 0];
}

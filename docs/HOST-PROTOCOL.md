# Host feedback protocol (HINGEBOX firmware 0.2)

How an app (Hingebox, or any host) drives the M-VAVE SMK-37 / FM-1 running this firmware: the pads' colours, the
screen, the buttons' lights. Everything is **MIDI channel 16, plain channel messages**. No SysEx: on this platform
SysEx belongs to the firmware updater, and keeping hosts off it keeps updates safe.

Messages the keyboard doesn't understand are ignored, so a host can send these to any keyboard and only units
that answer the hello (below) will react.

## Hello

| Host sends | Keyboard answers |
|---|---|
| CC 119, any value | CC 119 = protocol (1), CC 118 = capabilities, CC 117 = version (2 = 0.2) |

Capabilities (CC 118 bits): 1 pad lights, 2 screen, 4 button lights.

The keyboard treats the host as **connected** for 4 seconds after any message below; send the playhead (CC 113) at
least every 2 seconds to stay connected. When the host goes quiet the screen goes back to its own page.

## Pads (capability 1)

| Message | Meaning |
|---|---|
| Note on 36-51, velocity 1-127 | pad 0-15 (pad 0 top-left, row by row) to that hue on the colour wheel |
| Note on 36-51, velocity 0 / note off | pad dark |
| CC 20-35, value | the same, by CC |
| CC 16, value | brightness (saved) |
| CC 17 = 0 | all pads back to their saved colours |

Host colours live in RAM only: streaming a playhead never writes flash.

## Screen (capability 2)

| Message | Meaning |
|---|---|
| CC 110, slot | start a text: 0 groove (title), 1 track, 2 knob label |
| CC 109, character | one printable ASCII character (32-126), up to 16 |
| CC 108, any | show the text in that slot |
| CC 111 / CC 112 | tempo, high / low 7 bits (BPM) |
| CC 113, step | playhead 0-15, or 127 for stopped |
| CC 107, value | the knob in slot 2 moved: show its name and this value (0-127) for 1.5 s |

With a host connected, the bottom of the screen shows the groove, tempo and track, the pads as a 2x8 grid with the
playhead framed, and Bop (Hingebox's robot) bobbing on the beat.

## Buttons (capability 4)

| Message | Meaning |
|---|---|
| CC 41-46, value > 0 / 0 | the six mapped buttons' lights on / off (SMK-37: PARA, NOTE REPEAT, FX, SEQ, SEQ PLAY, SEQ REC) |

The same buttons send CC 41-46 on channel 1 when pressed (127) and released (0).

## What the keyboard sends (defaults)

Keys channel 1 from C3; pads channel 10, notes 36-51; knobs CC 21-28 (bank B 31-38); faders CC 7, 11, 91, 93
(bank B 71, 74, 72, 73); buttons CC 41-46; mod wheel CC 1; pedal CC 64; pitch bend.

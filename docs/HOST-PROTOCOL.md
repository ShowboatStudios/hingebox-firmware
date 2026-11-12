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

Capabilities (CC 118 bits): 1 pad lights, 2 screen, 4 button lights, 8 screen extras (CC 104-106 below; screen
v3, 2026-11-12). A keyboard that answers 7 ignores CC 104-106, so a host can send them to any unit.

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

### Screen extras (capability 8, additive)

| Message | Meaning |
|---|---|
| CC 104, index | the track's colour (slot 1's swatch and name): 0 kick orange, 1 snare blue, 2 clap pink, 3 hat yellow, 4 open green, 5 tom lilac, 6 perc white, 7 bass green; 127 = default (orange) |
| CC 106, index | the knob's colour, same indices; send it before CC 107. It stays until changed (127 = orange) |
| CC 105, moment | a moment for Bop: 1 fill, 2 drop (surprised), 3 cheer (happy), for 1.5 s; 0 nothing |

Pads sent in the deck's track hues (velocity 10, 80, 115, 22, 48, 95, 64; 20 fill, 3 drop: Hingebox's
`Keyboards.trackHue`) are drawn on the screen's grid in the deck's exact colours; other hues round the colour wheel.

With a host connected the screen shows a deck row (groove, the track in its colour, the tempo), the chord, the
keys held, Bop, and the pads as a 2x8 grid with the playhead framed and marked under its column; a link mark
in the status bar says the host is there. With no chord held the chord's place shows the beat as four blocks.
The knob overlay (CC 107) shows the name at 2x, the value as a percentage and a bar, in the CC 106 colour.

## Buttons (capability 4)

| Message | Meaning |
|---|---|
| CC 41-46, value > 0 / 0 | the six mapped buttons' lights on / off (SMK-37: PARA, NOTE REPEAT, FX, SEQ, SEQ PLAY, SEQ REC) |

The same buttons send CC 41-46 on channel 1 when pressed (127) and released (0).

## What the keyboard sends (defaults)

Keys channel 1 from C3; pads channel 10, notes 36-51; knobs CC 21-28 (bank B 31-38); faders CC 7, 11, 91, 93
(bank B 71, 74, 72, 73); buttons CC 41-46; mod wheel CC 1; pedal CC 64; pitch bend.

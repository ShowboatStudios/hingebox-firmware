# The keyboard's screen: review and redesign (screen v3, 2026-11-12)

What the SMK-37 / FM-1 screen showed on firmware 0.2 (branch `hingebox`, c253d3a), measured against
Hingebox's brief (`Hingebox/docs/interfaces/BRIEF.md` section 6 and "The shared look") and the last review
(`Hingebox/docs/ux-review-2026-11-12/PASS2.md` section 3). Screenshots come from `sh tests/run.sh`
(host render, 240x240 RGB565): `build/host-*.ppm` for the SMK-37, `build/fm1-host-*.ppm` for the FM-1.

The brief, in one line: a glance at arm's length while both hands play; mirror the deck; lowercase mono; OLED
black; never a number without a name, never scroll, never flash; Bop reacting to chords.

## Audit of 0.2, page by page

### The test harness itself
- Every screenshot after boot but the long chord was **the boot splash**: the splash lasts 1.4 s and the test
  only advanced 10 ms a tick, so `host-perform`, `host-chord`, `host-map`, `host-edit`, `host-arp`, the mode pages
  and `host-dmaj` all showed Bop and the wordmark. Nobody had seen those pages rendered since the splash went in.
- The FM-1 run wrote the same file names as the SMK-37 run, so its shots were overwritten.
- Fixed first: the test shoots the splash, then skips past it; the FM-1 shots are `build/fm1-host-*.ppm`.

### Boot splash
- Good: Bop and the wordmark in mono 3x, lowercase. Bop's feet are flat bars, not the app's pill feet.

### Perform page (no host)
- Top line: the wordmark in Inter Tight next to `oct 0` and `a c s p` in mono: the single letters are cryptic and
  the unlit ones are `C_LINE` grey, invisible at arm's length.
- With nothing held: 90% black. A chord rule at y 100, a small Bop at the bottom left and an unlabelled orange bar
  at the bottom (the last velocity). Nothing says what to do or what is on.
- Chord held: the big chord (Inter Tight, capitals: allowed) and its notes in mono are the best part of the
  screen. But nothing shows **which keys** are down, and the middle of the screen (y 130-160) is empty.
- Knob or fader moved: `CC31 65` in sans capitals at the right, no name: breaks "never a number without a name".
- The orange velocity bar has no label (PASS2 asked to label it or drop it).

### Host page (Hingebox connected)
- Groove, tempo (blue) and track (orange) in mono, Bop, the pads as a 2x8 grid: right content, too small. The
  grid's cells are 9x11 px: unreadable at arm's length; the playhead is a 1 px white frame on a 9 px cell.
- The pads take the host's hue as an arbitrary colour-wheel colour: the deck's kick orange arrives as a muddy
  orange, the snare blue as a cyan. The deck's track colours are not used.
- The track name is always orange, whatever the track's colour on the deck.
- No sign that the keyboard **is** connected to Hingebox other than the groove name appearing.
- Bop bobs on the beat (good) but never sleeps, never reacts to a fill or drop.

### Knob overlay (host)
- `reverb`, a 6 px orange bar and `62%` in the bottom band, for 1.5 s. Right idea, small: the name is 1x mono
  in the corner, the bar is thin, the colour is always orange (not the knob's colour on the deck), and the
  groove/tempo disappear underneath it.

### Mode pages (ARP, CHORD, SCALE, SPLIT)
- Header in Inter Tight capitals (`ARP`), titles in mono capitals (`ARPEGGIATOR OFF`), four 54x86 cards with
  `RATE` / `PATTERN` labels in capitals and `K1`..`K4` in `C_LINE` (invisible). `PATTERN` overflows its card.
  Values are all orange, so nothing stands out as "on".
- No hint of which button switches the mode or goes back, no sign of where you are among the five pages
  (< > walk them).
- SCALE says `MAJOR` and `C` but not which notes those are; SPLIT says `C4` but not where on the keys.
- CHORD's fourth card is an empty box.

### MAP and EDIT
- MAP: a 16 px list in capitals (`PAD 01  N36 CH10`) with a `1 /50` counter crammed next to the title; `PAD :`
  for pad 10 (a `'1' + 9` bug in the label). No scrollbar.
- EDIT: the same four-card layout as the mode pages, capitals, invisible `K1`..`K4`.

### Rescue / updater
- `platform/src/recovery.c` draws "USB RESCUE" in capitals. It is loader-side code: left alone on purpose
  (calibration and update safety come first; the rescue screen must never depend on app code).

## The redesign (screen v3)

One grid on every page: 12 px side margins, a 24 px status bar with a 1 px hairline, 16 px mono rows, regions
that start on 4 px boundaries. Labels lowercase mono; Inter Tight only for the wordmark and the chord.
Black panel, one accent (orange: what's on / what's happening now), tempo in blue, the deck's track colours.

- **Status bar** (every playing page): the wordmark, `oct` with its value, then only the modes that are on as
  short tags (`arp`, `chord`, `scale`, `split`), BLE, and a **link** mark in orange while Hingebox is connected.
- **Perform page**: the big chord with its notes underneath, a **keyboard strip** of the keys the keyboard
  covers (held keys orange, middle C marked, scale notes dotted when scale lock is on, the split point marked),
  Bop in the bottom band with a line of what's on (or a friendly prompt), and a labelled `vel` meter.
- **Host page**: a deck row (groove, track in its deck colour, tempo `96 bpm` in blue at 2x), the chord and the
  strip, and the **pad grid** in the deck's track colours: 18x16 px pads in two groups of four per row, the
  playhead as a 2 px white frame plus an orange bar under its column: readable at arm's length.
- **Knob overlay** (host or local): a card over the top half: the name at 2x, a 16 px bar with quarter ticks
  and the value, in the knob's deck colour when the host sends it (CC 106). Local controls name themselves
  (`knob a1`, `fader b2`, `mod wheel`) with their CC.
- **Bop** (`bop.h`): pill feet, ring eyes with a pupil that looks toward the notes you play, a blink every few
  seconds, bob on the beat, happy on 3+ notes, sleepy after 60 s with nothing played, surprised on a fill or
  drop (CC 105 "moment"), a happy hop when Hingebox connects.
- **Mode pages**: a title with an on/off pill and five page dots (where you are among the < > pages), four knob
  rows (`k1 rate 1/16`) with the value at 2x, and a picture of the setting: the scale's notes, the chord shape,
  the split point on a one- or three-octave strip. A footer says which button does what.
- **MAP / EDIT**: lowercase rows, a scrollbar, the selected row in orange; EDIT uses the mode pages' rows.

Flashing: nothing on the screen changes faster than the beat bob (two states a beat) and a 120 ms blink every
four seconds; no element blinks at more than 3 Hz.

See the protocol additions in `docs/HOST-PROTOCOL.md` (CC 104-106, additive).

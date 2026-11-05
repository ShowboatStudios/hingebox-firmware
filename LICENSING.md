# Licensing

GPL-3.0-only (`LICENSE`), as Felucca, SLOOP and Dinghy. Whoever gets a firmware built from this gets its source too.

| What                                                                                                                                                                                                        | Who                                                                                                                                                                                                                                                                                                                                         |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `platform/hal/`, `platform/loader/`, `platform/src/`, `platform/crt0.S`, `platform/app.ld`, `tools/fm1pkg_make.py`, `tools/lz4blk.py`, the FM-1 parts of `tools/build.py` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89), as [Dinghy](https://github.com/zvenson/dinghy) (zvenson) and [X0X](https://github.com/charlesvestal/fm1-x0x) (Charles Vestal) carry them |
| `platform/fm1.c`                                                                                                                                                                                            | Dinghy's `platform.c` (zvenson), rewired to `fm1.h`                                                                                                                                                                                                                                                                                         |
| `platform/fm1.h`, `platform/common.c`, `platform/clock.c`, `fm1`, `Dockerfile`, `src/`, `tests/`                                                                                                             | foma; DUMBMIDI's app by its authors                                                                                                                                                                                                                                                                                                        |
| `platform/gen/inter_font.h`, `platform/src/aatext.c`                                                                                                                    | Inter Tight (SIL OFL 1.1, Copyright 2022 The Inter Project Authors; `LICENSES/OFL-InterTight.txt`), rasterised with Felucca/Frodhes' `gen_aa_font.py`; the renderer is adapted from their `aatext.c` (GPL-3.0-only, Hügelton Instruments) |
| `platform/hal/smk_input.h`, `platform/hal/fm1_flash_map.h`, the SMK-37 parts of `platform/hal/fm1_lcd_hw.h`, `platform/src/lcd.c`, `src/ota.c`, `platform/loader/`, `tools/fm1pkg_make.py` and `tools/build.py` | SMK-37 Pro port: Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit and [sloop-fm1](https://github.com/genkigenki/sloop-fm1), as [Dinghy](https://github.com/zvenson/dinghy) and [Frodhes](https://github.com/leonardoMirabal/frodhes) carry it; the pad event queue (zvenson); the stock v016 flash table is M-VAVE's |

The USB audio from a computer in `platform/src/usb.c` (`FOMA_USB_AUDIO`) is ported from
[felucca-bsp](https://github.com/hugelton/felucca-bsp) (commit 5758857, MPL-2.0, by Leo Kuroshita), whose license lets
it be used under GPL-3.0; foma's version uses integer math. The TRS MIDI input (`platform/src/midi_uart.c`,
`platform/hal/fm1_uart.h`) is Felucca's.

Bluetooth MIDI on Felucca's platform (the `bluetooth` option):

- `platform/ble/`: Henri David's (hdavid) BLE stack from [Optimist](https://github.com/w0ts/optimist), as
  [fm1-base](https://github.com/charlesvestal/fm1-base) (Charles Vestal) carries it in `bt/ble` (commit 4039a8b, with
  its one fix in `ble_att.c`). Its files are `GPL-3.0-only OR MPL-2.0`
  ([w0ts/optimist#55](https://github.com/w0ts/optimist/issues/55)), used here under GPL-3.0-only.
- `platform/hal/fm1_ble.h`, `fm1_ble_rf.h`: the radio drivers, Optimist's, from
  [felucca-bsp](https://github.com/hugelton/felucca-bsp) (commit b0b2428), `GPL-3.0-only OR MPL-2.0`. The flash's
  unique-ID read (`fl_uid_ram` in `platform/hal/fm1_flash.h`) is felucca-bsp's too (MPL-2.0).
- `platform/hal/ble_rf_tables_v15.h` (the radio's start-up writes, recorded from M-VAVE's stock V15 firmware: JieLi's
  and M-VAVE's) and `platform/hal/fm1_rf_cal.h` (JieLi's radio calibration, rewritten in C by Charles Vestal), from
  felucca-bsp (b0b2428): Apache-2.0 (`LICENSES/Apache-2.0.txt`, which a firmware built with them ships). Their
  headers say where they come from and what changed.
- `platform/bt.c` (the glue) is foma's; the radio's constant trims in it are fm1-base's.

DUMBMIDI: `src/`, `tests/`, `firmware.json`, and its changes to the rest, by its authors, on [foma](https://github.com/konsumer/foma).
The SMK-37 board layer is the fm1-to-smk37 kit's, as Dinghy (`firmware/hal/smk_input.h`, the ST7789V table, the pad
colours, the flash map and the SMK package regions) and Frodhes carry it; the pad LED driver is Dinghy's `lcd.c`.

## Third-party material

| What                                                                                      | Licence     | Where                                           |
| ----------------------------------------------------------------------------------------- | ----------- | ----------------------------------------------- |
| Terminus font (ter-u16n), rendered into `platform/gen/felucca_font.h`                     | SIL OFL 1.1 | `platform/gen/Terminus-LICENSE.txt`             |
| Inter Tight, rendered into `platform/gen/inter_font.h`                                    | SIL OFL 1.1 | `LICENSES/OFL-InterTight.txt`                   |
| `platform/sdk/uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` from JieLi's AC79NN_SDK_V1.2.1 | Apache-2.0  | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |
| JieLi's toolchain, downloaded into the Docker image                                       | JieLi's     | <https://pkgman.jieliapp.com/s/linux-toolchain> |

"M-VAVE" and "FM-1" belong to their owners; "Felucca" and "Hügelton Instruments" to Hügelton Instruments. This project is not made or endorsed by them.

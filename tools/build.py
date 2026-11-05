#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the firmware for every target board (run it in the toolchain container: ./fm1 build).

  tools/build.py             FM-1 and the three SMK-37 models into build/*.fwsc (nothing else)

Each package is complete: the app, the update loader and the flash image. Install one with
mutant.cloud (or the stock updater / the family installer); there is no web emulator here.

The FM-1 and SMK-37 parts are Dinghy's / Felucca's build (Leo Kuroshita, Hügelton Instruments;
zvenson; the SMK-37 port: M. Geisler): the app, the update loader, the checks that keep a build
from bricking the unit, the package.
"""

import json
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLAT = ROOT / "platform"
SRC = ROOT / "src"
OUT = ROOT / "build"
sys.path.insert(0, str(ROOT / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

TC = Path(os.environ.get("JIELI_TOOLCHAIN", "/opt/jieli/toolchain"))
APP_XIP = 0x02000120  # app.bin offset 0 in the XIP map; the SPL jumps here
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"  # the file name the SPL looks for
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
INCLUDES = ["-Iplatform", "-Iplatform/hal", "-Iplatform/src", "-Iplatform/gen"]
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# one keyboard, three names: M-VAVE's updater only takes a unit's own factory package back
MODELS = {"pro": "SMK-37 Pro", "elite": "SMK-37 Elite", "mke": "MKE-P37"}
MODEL_PID = {"pro": 0x0007, "elite": 0x0008, "mke": 0x0009}  # (the family's ids; the update loader is 0x0002)
FM1_PID = 0x0006
# firmware.json "options": the platform's parts you can leave out (all in unless set false), as FOMA_* in fm1.h
OPTIONS = {"usb_audio": "FOMA_USB_AUDIO", "trs_midi": "FOMA_TRS_MIDI", "midi_clock": "FOMA_MIDI_CLOCK",
           "bluetooth": "FOMA_BLUETOOTH"}


class BuildError(Exception):
    pass


def run(cmd):
  r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
  if r.returncode:
    raise BuildError((r.stdout + r.stderr).strip() or f"{cmd[0]} failed")
  if r.stderr.strip():
    print(r.stderr.rstrip(), file=sys.stderr)
  return r.stdout


def rel(a):
  return str(Path(a).resolve().relative_to(ROOT)) if isinstance(a, Path) else a


def tc(tool, *args):
  """a JieLi toolchain binary: cc, ld, objcopy, objdump"""
  args = [rel(a) for a in args]
  if tool == "cc":
    return run([str(TC / "pi32v2/bin/clang"), "-target", "pi32v2", *args])
  if tool == "ld":
    return run([str(TC / "pi32v2/bin/ld"), *args])
  return run([str(TC / "common/bin" / tool), *args])


def tc_all(*cmds):
  with ThreadPoolExecutor(len(cmds)) as ex:
    return list(ex.map(lambda c: tc(*c), cmds))


def sources():
  return sorted(SRC.glob("**/*.c"))


def config():
  c = json.loads((ROOT / "firmware.json").read_text())
  m = re.fullmatch(r"(\d+)\.(\d+)(\.\d+)?(-[A-Za-z0-9]+)?", c.get("version", ""))
  if not m:
    raise BuildError('firmware.json: "version" must look like 0.1, 1.2.3 or 0.1-beta')
  if not re.fullmatch(r"[A-Z0-9 _.+-]{1,16}", c.get("name", "")):
    raise BuildError('firmware.json: "name" must be 1..16 capitals, digits, spaces or _.+-')
  if not re.fullmatch(r"FM-1_\d{2}", c.get("identity", "")):
    raise BuildError('firmware.json: "identity" must be FM-1_ and two digits')
  opts = c.get("options", {})
  bad = sorted(set(opts) - set(OPTIONS))
  if bad:
    raise BuildError(f"firmware.json: unknown option {', '.join(bad)} (there are: {', '.join(OPTIONS)})")
  on = {k: bool(opts.get(k, True)) for k in OPTIONS}
  if not on["bluetooth"]:
    print("warning: bluetooth is off: this firmware's BLE MIDI output will not be in the build", file=sys.stderr)
  minor = int(m[2]) % 10
  c["version"] = m[1] + "." + m[2] + (m[3] or "") + (m[4] or "")
  c["minor"] = minor
  # a fixed FM-1 device id (firmware.json "product"), so a listing keeps one identity across releases
  c["fm1_product"] = c.get("product") or f'{c["identity"]}{minor}'
  c["on"] = on
  c["file"] = re.sub(r"[^a-z0-9.-]+", "-", c["name"].lower()).strip("-") or "firmware"
  return c


# ---- the update loader


def crc16(d, c=0):
  for b in d:
    c ^= b << 8
    for _ in range(8):
      c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
    c &= 0xFFFF
  return c


def ldr_head(dcrc, a, b, attr, name):
  body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
  return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
  """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
  blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
  blocks = bytearray()
  for i in range(0, len(image), 4096):
    raw = image[i : i + 4096]
    c = lz4blk.compress(raw)
    if len(c) > 4096 or lz4blk.decompress(c) != raw:
      raise BuildError(f"loader: block {i // 4096} does not compress below 4096 B")
    blocks += struct.pack("<II", len(c), len(raw)) + c
  inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
  body = inner + bytes(blocks)
  return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader(out, board_defs):
  ldr = out / "loader"
  ldr.mkdir(parents=True, exist_ok=True)
  src = PLAT / "loader"
  tc_all(
    ("cc", "-c", src / "crt0_ldr.S", "-o", ldr / "crt0_ldr.o"),
    ("cc", *CFLAGS, *INCLUDES, *board_defs, "-c", src / "loader.c", "-o", ldr / "loader.o"),
  )
  elf = ldr / "loader.elf"
  tc("ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld", ldr / "crt0_ldr.o", ldr / "loader.o", "-o", elf)
  _, dis, hdr = tc_all(("objcopy", "-O", "binary", "-j", ".text", elf, ldr / "loader.bin"), ("objdump", "-d", elf),
                       ("objdump", "-h", elf))
  (ldr / "loader.dis").write_text(dis)
  for ln in hdr.splitlines():  # the loader rewrites the flash: nothing may run from XIP
    p = ln.split()
    if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
      raise BuildError(f"loader: section {p[1]} at {p[3]} is outside RAM")
  image = (ldr / "loader.bin").read_bytes()
  if not image or len(image) > 0x14000:
    raise BuildError("loader: image empty or too big")
  ota = ldr_wrap(image)
  (ldr / "ota.bin").write_bytes(ota)
  return ota


# ---- the app


def build_app(cfg, out, board_defs, product):
  app_slot = fm1pkg_make.APP_SLOT
  flags = [*CFLAGS, *INCLUDES, f'-DAPP_NAME="{cfg["name"]}"', f'-DAPP_VER="{cfg["version"]}"',
           f'-DFELUCCA_ID="{product}"', *cfg["defines"], *board_defs]
  objs, cmds = [], []
  for s in [PLAT / "crt0.S", PLAT / "hal/fm1_vec.S", PLAT / "hal/fm1_isr.S"]:
    objs.append(out / (s.stem + ".o"))
    cmds.append(("cc", "-c", s, "-o", objs[-1]))
  for s in [PLAT / "fm1.c", *sources()]:
    objs.append(out / (("platform_" if s.parent == PLAT else "app_") + s.stem + ".o"))
    cmds.append(("cc", *flags, "-c", s, "-o", objs[-1]))
  tc_all(*cmds)
  elf = out / "firmware.elf"
  ld = PLAT / "app.ld"
  if app_slot != 0x8DFBC:  # another board's app slot (SMK-37): the same script, its XIP length
    txt = ld.read_text()
    if txt.count("LENGTH = 0x8DFBC") != 1:
      raise BuildError("app.ld: the XIP LENGTH line was not found")
    ld = out / "app.ld"
    ld.write_text(txt.replace("LENGTH = 0x8DFBC", f"LENGTH = {app_slot:#X}".replace("0X", "0x")))
  try:
    tc("ld", "--gc-sections", "-e", "_start", "-T", ld, *objs, "-o", elf)
  except BuildError as e:
    raise BuildError(explain_link(str(e))) from None
  for sect in ("text.bin", "data.bin", "ramtext.bin"):
    (out / sect).unlink(missing_ok=True)
  *_, syms, dis, rt = tc_all(
    ("objcopy", "-O", "binary", "-j", ".text", elf, out / "text.bin"),
    ("objcopy", "-O", "binary", "-j", ".data", elf, out / "data.bin"),
    ("objcopy", "-O", "binary", "-j", ".ram_text", elf, out / "ramtext.bin"),
    ("objdump", "-t", elf),
    ("objdump", "-d", elf),
    ("objdump", "-d", "-j", ".ram_text", elf),
  )
  (out / "firmware.dis").write_text(dis)

  def symv(name):
    return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)

  img = bytearray((out / "text.bin").read_bytes())
  # .ram_text and .data follow .text at their load addresses; crt0 copies them by words
  for sect, lname in (("ramtext.bin", "_rt_load"), ("data.bin", "_data_load")):
    load = symv(lname)
    if load % 4:
      raise BuildError(f"{lname} {load:#x} is not word aligned")
    blob = (out / sect).read_bytes() if (out / sect).exists() else b""
    if blob:
      if load - APP_XIP < len(img):
        raise BuildError(f"{lname} overlaps the image")
      img += b"\xff" * (load - APP_XIP - len(img))
      img += blob
  img += b"\xff" * (-len(img) % 4)
  (out / "firmware.bin").write_bytes(img)
  return bytes(img), syms, dis, rt


LINK_HINTS = [
  (
    r"__(?:float|fix|extend|trunc)\w*|__(?:add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|un)[sd]f[23]\b",
    "float or double: the chip has no FPU and no float library. Use integers (fixed point, e.g. Q15)",
  ),
  (r"__u?(?:div|mod)di3\b", "a 64-bit division or modulo: there is no libgcc. Shift, or divide 32-bit values"),
  (
    r"\b(?:printf|sprintf|snprintf|malloc|free|strlen|strcpy|sin|cos|pow|sqrt)\b",
    "there is no C library: see fm1.h for what there is (format_int, sine, ...)",
  ),
]


def explain_link(err):
  """the linker's 'undefined reference' errors, in words, with where they come from"""
  out = []
  for ln in err.splitlines():
    m = re.search(r"^(\S+?):\(\.text\.(\w+)[^)]*\): undefined reference to `(\w+)'", ln)
    if not m:
      continue
    why = next((h for rx, h in LINK_HINTS if re.search(rx, m[3])), f"{m[3]} does not exist")
    line = f"  in {m[2]}(): {why} ({m[3]})"
    if line not in out:
      out.append(line)
  return "link failed:\n" + "\n".join(out) if out else err


def check(img, syms, dis, rt, app_slot):
  errors, notes = [], []
  m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
  if not m or int(m.group(1), 16) != APP_XIP:
    errors.append(f"_start is not at {APP_XIP:#x}")
  if img[:4] != bytes.fromhex("04818000"):
    errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
  rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
  if rt_calls:  # RAM code runs with the flash off: no calls into XIP
    errors.append(f".ram_text contains calls: {rt_calls[:3]}")
  fn = "?"
  for ln in dis.splitlines():  # nothing may call or load an address in the chip ROM
    h = re.match(r"^[0-9a-f]+ <([^>]+)>:", ln)
    if h:
      fn = h.group(1)
      continue
    mm = LINE.match(ln)
    if not mm:
      continue
    for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
      val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
      if 0xFFC00000 <= val < 0xFFD00000:
        errors.append(f"{fn}: a constant {val:#010x} lies in the chip ROM (a call there bricks the unit)")
  if len(img) > app_slot:
    errors.append(f"image {len(img)} B exceeds the app slot ({app_slot} B)")

  def sym(name):
    mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
    return int(mm.group(1), 16) if mm else 0

  ram = sym("_bss_end") - 0x01C08000
  pool = sym("_pool_end") - 0x01C20000
  notes.append(f"code {len(img) // 1024} of {app_slot // 1024} KiB, RAM {ram // 1024} of 96 KiB, pool {pool // 1024} of 336 KiB")
  if ram > 96 * 1024:
    errors.append(f"RAM (.data + .bss) {ram} B > 96 KiB: make big arrays POOL or const")
  if pool > 0x54000:
    errors.append(f"pool {pool} B > 336 KiB")
  return errors, notes


# Every hardware register access lives in platform/hal/. Elsewhere (comments stripped):
#  - no volatile pointer cast, except of a C object's address;
#  - no literal in a register or reserved window;
#  - no inline asm, except the empty compiler barrier RING_PUBLISH();
#  - no HAL register macro.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
  def strip(s):
    s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)

  defs = {}
  for h in sorted((PLAT / "hal").glob("*.h")):
    for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
      defs[m.group(1)] = m.group(2)
  regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
  while True:
    more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
    if not more:
      break
    regs |= more
  errors = []
  files = [*SRC.glob("**/*.[ch]"), *(PLAT / "src").glob("*.[ch]"), *(PLAT / "loader").glob("*.c"), PLAT / "fm1.c",
           PLAT / "common.c"]
  for f in sorted(files):
    for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
      where = f"{f.relative_to(ROOT)}:{no}"
      for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"), (MMIO_ASM, "inline asm")):
        m = rx.search(ln)
        if m:
          errors.append(f"{where}: {what} outside platform/hal ({m.group(0).strip()})")
      used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
      if used:
        errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside platform/hal")
  return errors


# ---- targets


def target_defs(cfg, board, model):
  """the compile defines of one package: the board, the model, its USB id and its package identity"""
  model_name = MODELS[model] if board == "smk37" else None
  minor = cfg["minor"]
  if board == "smk37":
    product = f"{model_name}_99{minor}"
    pid = MODEL_PID[model]
    defs = ["-DBOARD_SMK37=1", f'-DSMK_MODEL="{model_name}"']
    app_defs = [*defs, f"-DFELUCCA_USB_PID=0x{pid:04X}"]
    suffix = "-smk37" + ("" if model == "pro" else "-" + model)
  else:
    product = cfg["fm1_product"]
    defs = []
    app_defs = [f"-DFELUCCA_USB_PID=0x{FM1_PID:04X}"]
    suffix = ""
  return product, defs, app_defs, suffix


def build_target(cfg, board, model="pro"):
  product, board_defs, app_defs, suffix = target_defs(cfg, board, model)
  fm1pkg_make.set_board(board)
  if board == "smk37" and cfg["on"]["trs_midi"]:
    raise BuildError("smk37: trs_midi must be off (its UART pin is a key-matrix row on this keyboard)")
  out = OUT / ("smk37-" + model if board == "smk37" else "fm1")
  out.mkdir(parents=True, exist_ok=True)
  ota = build_loader(out, board_defs)
  img, syms, dis, rt = build_app(cfg, out, app_defs, product)
  errors, notes = check(img, syms, dis, rt, fm1pkg_make.APP_SLOT)
  errors += mmio_check()
  if errors:
    raise BuildError("the build is refused (it could crash or brick the unit):\n" + "\n".join("  " + e for e in errors))
  pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, product)
  name = f'{cfg["file"]}{suffix}-{cfg["version"]}.fwsc'
  (OUT / name).write_bytes(pkg)
  return name, product, board, notes


def build():
  cfg = config()
  cfg["defines"] = [f"-D{OPTIONS[k]}={int(cfg['on'][k])}" for k in OPTIONS]
  targets = [("fm1", "pro")] + [("smk37", m) for m in MODELS]
  print(f'{cfg["name"]} {cfg["version"]}: {len(targets)} packages')
  failed = 0
  for board, model in targets:
    try:
      name, product, board, notes = build_target(cfg, board, model)
      print(f"  ok    {name}  identity {product}  ({'; '.join(notes)})")
    except BuildError as e:
      failed += 1
      print(f"  FAILED  {board} {model}: {e}")
  print(f"{'ok' if not failed else f'{failed} failed'}")
  return failed == 0


def main():
  if not (TC / "pi32v2/bin/clang").exists():
    raise SystemExit("no JieLi toolchain here: run this through ./fm1 (it uses the Docker image)")
  return 0 if build() else 1


if __name__ == "__main__":
  sys.exit(main())

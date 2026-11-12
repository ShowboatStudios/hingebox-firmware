"""Add extra glyphs to a face of platform/gen/inter_font.h (the AA_MID / AA_BIG faces carry only space to Z).

The original generator (gen_aa_font.py) is not in this repo, so this appends glyphs through the font's "ex" list
(aatext.c looks them up before folding lowercase to capitals): rasterised by tools/aa_raster.swift (CoreText, the
same metrics as the existing glyphs), then Huffman-coded with the face's own table. Kerning for them is zero.

  python3 tools/aa_extend.py <InterTight[wght].ttf> M L   (adds "abdgijmsu": the chord suffixes, maj7 m7b5 sus4 ...)
"""

import json
import re
import subprocess
import sys

HDR = "platform/gen/inter_font.h"
CHARS = "abdgijmsu"
FACES = {"M": (32, 600), "L": (52, 600)}


def array(src, name):
    m = re.search(r"static const (\w+) " + name + r"\[(\d+)\] = \{(.*?)\n\};", src, re.S)
    return m


def nums(body):
    return [int(x, 0) for x in re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", body)]


def codes(hc):
    """canonical Huffman: symbol -> (code, length), as aatext.c's aa_hc_lut builds it"""
    out, code, k = {}, 0, 15
    for ln in range(1, 16):
        for _ in range(hc[ln - 1]):
            out[hc[k]] = (code, ln)
            code += 1
            k += 1
        code <<= 1
    return out


def encode(pix, table):
    """runs of (value, count) over the ink box, each split into runs the table has"""
    runs = {}
    for s in table:
        runs.setdefault(s & 15, []).append((s >> 4) + 1)
    for v in runs:
        runs[v].sort(reverse=True)
    bits, nb, out = 0, 0, bytearray()

    def put(s):
        nonlocal bits, nb
        c, ln = table[s]
        bits = (bits << ln) | c
        nb += ln
        while nb >= 8:
            out.append((bits >> (nb - 8)) & 255)
            nb -= 8
        bits &= (1 << nb) - 1

    i = 0
    while i < len(pix):
        v, n = pix[i], 1
        while i + n < len(pix) and pix[i + n] == v:
            n += 1
        i += n
        while n:
            r = next(r for r in runs[v] if r <= n)
            put(((r - 1) << 4) | v)
            n -= r
    if nb:
        out.append((bits << (8 - nb)) & 255)
    return bytes(out)


def fmt(vals, per=24):
    lines = []
    for k in range(0, len(vals), per):
        lines.append("    " + ", ".join(f"0x{v:02x}" for v in vals[k : k + per]) + ",")
    return "\n".join(lines)


def extend(src, face, ttf):
    px, wght = FACES[face]
    fm = re.search(r"static const aafont_t AF_" + face + r" = \{(.*?)\};", src)
    f = [x.strip() for x in fm.group(1).split(",")]
    h, asc, first, last, nex = (int(x) for x in f[:5])
    psh = int(f[11])
    if nex:
        sys.exit(f"AF_{face} already has extra glyphs")
    hc = nums(array(src, f"AF_{face}_HC").group(3))
    table = codes(hc)
    dm = array(src, f"AF_{face}_DATA")
    data = nums(dm.group(3))
    gm = array(src, f"AF_{face}_G")
    glyphs = re.findall(r"\{(-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+)\}", gm.group(3))
    glyphs = [tuple(int(x) for x in g) for g in glyphs]
    assert len(glyphs) == (last - first + 1) << psh
    out = subprocess.run(
        ["swift", "tools/aa_raster.swift", ttf, str(px), str(wght), CHARS], capture_output=True, text=True, check=True
    ).stdout
    for line in out.splitlines():
        o = json.loads(line)
        rows = o["rows"]
        bw, bh = (len(rows[0]), len(rows)) if rows else (0, 0)
        pix = [(a * 15 + 127) // 255 for r in rows for a in r]
        off = len(data)
        data += list(encode(pix, table))
        e = (off, round(o["adv"] * 16), o["bx"], asc - o["top"], bw, bh)
        glyphs += [e] * (1 << psh)  # the same glyph at every phase (aatext draws phase 0)
    data += [0, 0]  # the decoder reads ahead
    assert len(data) < 65536
    n = len(CHARS)
    src = src.replace(dm.group(0), f"static const uint8_t AF_{face}_DATA[{len(data)}] = {{\n{fmt(data)}\n}};")
    gl = "\n".join("    {" + ", ".join(str(v) for v in g) + "}," for g in glyphs)
    src = src.replace(gm.group(0), f"static const aag_t AF_{face}_G[{len(glyphs)}] = {{\n{gl}\n}};")
    em = re.search(r"static const uint8_t AF_" + face + r"_EX\[\d+\] = \{.*?\};", src)
    ex = ", ".join(str(ord(c)) for c in CHARS)
    src = src.replace(em.group(0), f"static const uint8_t AF_{face}_EX[{n}] = {{{ex}}}; /* tools/aa_extend.py */")
    f[4] = str(n)
    src = src.replace(fm.group(0), f"static const aafont_t AF_{face} = {{{', '.join(f)}}};")
    return src


def main():
    ttf, faces = sys.argv[1], sys.argv[2:]
    src = open(HDR).read()
    for face in faces:
        src = extend(src, face, ttf)
    open(HDR, "w").write(src)


if __name__ == "__main__":
    main()

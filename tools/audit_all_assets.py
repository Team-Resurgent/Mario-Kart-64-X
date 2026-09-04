#!/usr/bin/env python3
"""Audit EVERY json-described asset against baserom.us.z64.

fix_course_sprites.py only covered assets/courses/*.json entries that have an
.inc.c. This sweeps every assets/**/*.json entry, both .inc.c and .mio0/.bin
payloads, and reports three defect classes:

  SIZE   the .inc.c holds a different number of bytes than width*height*bpp
         -- the PVR "padded to square and twiddled" signature that scrambled
         the moles/crabs/bats/thwomps (see fix_course_sprites.py)
  DIFF   right size, wrong bytes (byte-order or re-quantized export)
  MIO0   a .mio0 blob that is not the ROM's own compressed block

Read-only: prints findings, changes nothing.
"""
import json, re, struct, glob, os, sys, io

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
rom = open("baserom.us.z64", "rb").read()
_blocks = {}

BPP = {"ci8": 1, "i8": 1, "ia8": 1, "rgba16": 2, "ia16": 2,
       "ci4": 0.5, "i4": 0.5, "ia4": 0.5, "rgba32": 4, "ia32": 4}


def mio0_len(d):
    us, co, ro = struct.unpack(">III", d[4:16])
    n = 0; lb = 16; lp = 0x10; ct = 0; cp = co; rp = ro
    while n < us:
        if lb == 16:
            ct = struct.unpack(">H", d[lp:lp + 2])[0]; lp += 2; lb = 0
        if ct & 0x8000:
            rp += 1; n += 1
        else:
            v = struct.unpack(">H", d[cp:cp + 2])[0]; cp += 2; n += (v >> 12) + 3
        ct = (ct << 1) & 0xFFFF; lb += 1
    return max(lp, cp, rp)


def decompress(off):
    if off in _blocks:
        return _blocks[off]
    d = rom[off:off + 0x200000]
    if d[:4] != b"MIO0":
        _blocks[off] = None
        return None
    us, co, ro = struct.unpack(">III", d[4:16])
    out = bytearray(); lp = 0x10; cp = co; rp = ro; lb = 16; ct = 0
    while len(out) < us:
        if lb == 16:
            ct = struct.unpack(">H", d[lp:lp + 2])[0]; lp += 2; lb = 0
        if ct & 0x8000:
            out.append(d[rp]); rp += 1
        else:
            v = struct.unpack(">H", d[cp:cp + 2])[0]; cp += 2
            n = (v >> 12) + 3; dist = (v & 0xFFF) + 1
            for _ in range(n):
                out.append(out[-dist])
        ct = (ct << 1) & 0xFFFF; lb += 1
    _blocks[off] = bytes(out)
    return _blocks[off]


def inc_bytes(path):
    t = io.open(path, encoding="utf-8", errors="replace").read()
    if re.search(r"0x[0-9a-fA-F]{4}\b", t):
        return None, "u16"      # value-literal file: different convention
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", t)), "u8"


findings = {"SIZE": [], "DIFF": [], "MIO0": [], "SKIP": []}
checked = 0

for jp in sorted(glob.glob("assets/**/*.json", recursive=True)):
    try:
        j = json.load(open(jp))
    except Exception as e:
        print("unreadable json:", jp, e)
        continue
    base = os.path.dirname(jp)
    for name, e in j.items():
        if not isinstance(e, dict) or "rom_offset" not in e:
            continue
        outdir = e.get("output_dir", "")
        stem = os.path.join(base, outdir, name)
        typ = e.get("type", "")
        rom_off = int(str(e["rom_offset"]), 16)

        incp = stem + ".inc.c"
        miop = stem + ".mio0"

        if os.path.exists(incp) and typ in BPP and "block_offset" in e:
            blk = decompress(rom_off)
            if blk is None:
                findings["SKIP"].append((incp, "rom_offset is not an MIO0 block"))
                continue
            exp = int(e["width"] * e["height"] * BPP[typ])
            rb = blk[int(str(e["block_offset"]), 16):][:exp]
            ib, kind = inc_bytes(incp)
            if ib is None:
                findings["SKIP"].append((incp, "u16 value-literal file (boot-swap convention)"))
                continue
            checked += 1
            if len(ib) != exp:
                findings["SIZE"].append((incp, len(ib), exp, typ))
            elif ib != rb:
                nd = sum(1 for a, b in zip(ib, rb) if a != b)
                findings["DIFF"].append((incp, nd, exp, typ))

        if os.path.exists(miop):
            d = rom[rom_off:rom_off + 0x200000]
            if d[:4] != b"MIO0":
                findings["SKIP"].append((miop, "rom_offset is not MIO0"))
                continue
            want = d[:mio0_len(d)]
            got = io.open(miop, "rb").read()
            checked += 1
            if got != want:
                findings["MIO0"].append((miop, len(got), len(want)))

print("checked %d assets\n" % checked)
for k in ("SIZE", "DIFF", "MIO0"):
    rows = findings[k]
    print("== %s: %d" % (k, len(rows)))
    for r in rows:
        print("   ", *r)
    print()
if findings["SKIP"]:
    print("== not comparable: %d (u16 convention / non-MIO0 source)" % len(findings["SKIP"]))

#!/usr/bin/env python3
"""Rewrite course sprite .inc.c files with ROM-verbatim bytes.

The DC port re-exported the animated course sprites (moles, crabs, bats,
thwomp faces, snowman, rainbow-road neon signs, hedgehog, sherbet ice)
padded to square and PVR-TWIDDLED for the Dreamcast GPU -- most are 2x the
N64 size, and the C arrays' fixed dims silently drop the overflow, leaving
half a twiddled image behind: scrambled indices through a correct palette
(the Moo Moo Farm mole glitch). The Xbox importer wants N64-linear layout,
which is exactly the ROM's own bytes.

Compares every course json texture entry that has an .inc.c against the
MIO0-decompressed ROM block and rewrites any mismatch verbatim. Idempotent.
"""
import json, re, struct, glob, os, sys

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
rom = open("baserom.us.z64", "rb").read()
blocks = {}

def mio0(off):
    if off in blocks:
        return blocks[off]
    d = rom[off:off + 0x100000]
    if d[:4] != b"MIO0":
        blocks[off] = None
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
    blocks[off] = bytes(out)
    return blocks[off]

BPP = {"ci8": 1, "i8": 1, "ia8": 1, "rgba16": 2, "ia16": 2,
       "ci4": 0.5, "i4": 0.5, "ia4": 0.5}

fixed = ok = 0
for jp in sorted(glob.glob("assets/courses/*.json")):
    j = json.load(open(jp))
    for name, e in j.items():
        if not isinstance(e, dict) or "block_offset" not in e or e.get("type") not in BPP:
            continue
        p = "assets/courses/%s/%s.inc.c" % (e.get("output_dir", ""), name)
        if not os.path.exists(p):
            continue
        exp = int(e["width"] * e["height"] * BPP[e["type"]])
        blk = mio0(int(e["rom_offset"], 16))
        if blk is None:
            print("no MIO0 at", e["rom_offset"], "for", name); continue
        bo = int(e["block_offset"], 16)
        rb = blk[bo:bo + exp]
        txt = open(p).read()
        ib = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", txt))
        if ib == rb:
            ok += 1
            continue
        lines = []
        for i in range(0, len(rb), 32):
            lines.append(",".join("0x%02x" % b for b in rb[i:i + 32]) + ",")
        with open(p, "w", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print("fixed %-60s %5d -> %5d bytes" % (p, len(ib), len(rb)))
        fixed += 1
print("ok %d, fixed %d" % (ok, fixed))

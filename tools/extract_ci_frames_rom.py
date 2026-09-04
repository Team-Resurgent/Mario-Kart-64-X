#!/usr/bin/env python3
"""Extract CI frame .mio0 blobs VERBATIM from baserom.us.z64.

The PNG->n64graphics regeneration produced index data ordered against each
PNG's own palette rather than the runtime TLUT (shell mottling), and
re-indexing loses exactness for palette-swapped reuse (red shells draw the
GREEN shell's indices with a red TLUT -- nearest-match speckles). The ROM
holds the original compressed frames; copy their bytes untouched.
"""
import json, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "baserom.us.z64")
rom = open(ROM, "rb").read()

FAMILIES = ["greenshell", "blueshell", "trees", "finish_line_banner"]

total = 0
for fam in FAMILIES:
    j = json.load(open(os.path.join(ROOT, "assets", fam + ".json")))
    frames = [(k, int(v["rom_offset"], 16), v) for k, v in j.items()
              if isinstance(v, dict) and v.get("type") == "ci8" and "rom_offset" in v]
    frames.sort(key=lambda t: t[1])
    for i, (name, off, v) in enumerate(frames):
        end = frames[i+1][1] if i+1 < len(frames) else off + 0x600
        blob = rom[off:end]
        if blob[:4] != b"MIO0":
            print("BAD MAGIC", fam, name, hex(off)); continue
        (usize,) = struct.unpack(">I", blob[4:8])
        expect = v["width"] * v["height"]
        if usize != expect:
            print("SIZE MISMATCH", fam, name, usize, "!=", expect); continue
        out = os.path.join(ROOT, "assets", fam, name + ".mio0")
        open(out, "wb").write(blob)
        print("ok %-14s %-28s off=%s usize=%d len=%d" % (fam, name, hex(off), usize, len(blob)))
        total += 1
print("extracted", total)

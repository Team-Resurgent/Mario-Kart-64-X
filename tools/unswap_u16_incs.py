#!/usr/bin/env python3
"""Revert the u16 byteswap for files it broke.

Console-verified: the inc.c files were generated with TWO conventions. The
startup/ceremony reflection maps store texel VALUES (broken before the swap,
fixed by it); the common_data / rainbow_road / podium files store the raw BE
byte stream packed as u16 (correct before, broken by the swap -- HUD, portraits
and trees rendered as noise). The swap is an involution, so reverting = running
it again and dropping the marker. KEEP holds the value-convention files that
must stay swapped.
"""
import re, io, os

MARKER = "/* xbox-u16-byteswapped */"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KEEP = {
    "assets/code/startup_logo/reflection_map_gold.rgba16.inc.c",
    "assets/code/ceremony_data/reflection_map_gold.rgba16.inc.c",
    "assets/code/ceremony_data/reflection_map_brass.rgba16.inc.c",
    "assets/code/ceremony_data/reflection_map_silver.rgba16.inc.c",
}

lit_re = re.compile(r'0[xX]([0-9A-Fa-f]{1,4})\b')
def swap(m):
    v = int(m.group(1), 16)
    return "0x%04X" % (((v & 0xFF) << 8) | (v >> 8))

reverted = kept = 0
for dirpath, _, files in os.walk(os.path.join(ROOT, "assets", "code")):
    for fn in files:
        if not fn.endswith(".inc.c"):
            continue
        p = os.path.join(dirpath, fn)
        rel = os.path.relpath(p, ROOT).replace("\\", "/")
        s = io.open(p, encoding="utf-8").read()
        if not s.startswith(MARKER):
            continue
        if rel in KEEP:
            kept += 1
            continue
        body = s[len(MARKER):].lstrip("\n")
        io.open(p, "w", encoding="utf-8").write(lit_re.sub(swap, body))
        reverted += 1
        print("reverted", rel)
print("reverted=%d kept=%d" % (reverted, kept))

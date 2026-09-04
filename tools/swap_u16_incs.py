#!/usr/bin/env python3
"""Byte-swap the u16 texture literals in asset .inc.c files.

These arrays are declared `u16 name[] = { 0x0841, ... }`. Compiled on the
little-endian Xbox, 0x0841 lands in memory as 41 08 -- but the values are the
N64's big-endian texel VALUES, and the RGBA16/IA16 importers byteswap what they
read (correct for raw BE byte streams like the u8 course arrays). The result is
a double swap: the spinning-logo gold ramp rendered as rainbow noise with
alpha=0, and the same corruption applies to every u16-declared texture
(common_data HUD/item textures, ceremony reflection maps, Rainbow Road TLUTs).

Swapping each literal (0xABCD -> 0xCDAB) makes the compiled memory identical to
the raw ROM byte stream, which is exactly what the importers are written for.
Idempotent: transformed files are marked with a header comment and skipped.
"""
import re, io, os, sys

MARKER = "/* xbox-u16-byteswapped */"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC_FILES = [
    "assets/code/ceremony_data/ceremony_data.c",
    "assets/code/common_data/common_data.c",
    "assets/code/rainbow_road_tluts/rainbow_road_tluts.c",
    "assets/code/startup_logo/startup_logo.c",
]

inc_re = re.compile(r'^(u16)\s+\w+\[\]\s*=\s*\{\s*$')
incl_re = re.compile(r'#include\s+"([^"]+\.inc\.c)"')
lit_re = re.compile(r'0[xX]([0-9A-Fa-f]{1,4})\b')

def swap(m):
    v = int(m.group(1), 16)
    return "0x%04X" % (((v & 0xFF) << 8) | (v >> 8))

targets = []
for src in SRC_FILES:
    lines = io.open(os.path.join(ROOT, src), encoding="utf-8").read().splitlines()
    for i, ln in enumerate(lines):
        if inc_re.match(ln.strip().replace("ALIGNED8 ", "")) or ln.strip().startswith("u16 "):
            if "[] = {" not in ln:
                continue
            for j in range(i + 1, min(i + 4, len(lines))):
                m = incl_re.search(lines[j])
                if m:
                    targets.append(m.group(1))
                    break

done = skipped = 0
for rel in sorted(set(targets)):
    p = os.path.join(ROOT, rel)
    if not os.path.exists(p):
        print("MISSING", rel); continue
    s = io.open(p, encoding="utf-8").read()
    if s.startswith(MARKER):
        skipped += 1; continue
    io.open(p, "w", encoding="utf-8").write(MARKER + "\n" + lit_re.sub(swap, s))
    done += 1
    print("swapped", rel)
print("done=%d skipped=%d total=%d" % (done, skipped, len(set(targets))))

#!/usr/bin/env python3
"""Replace EVERY repo .mio0 asset that has a json rom_offset with the ROM's
verbatim compressed blob.

v2 of extract_ci_frames_rom: (1) covers all types, not just ci8 -- the tree
and banner families dma their TLUTs as separate mio0 blobs, which stayed
PNG-pipeline while their frames became ROM-true, scrambling trees; (2) sizes
each blob to the next known mio0 offset (or +0x2000), fixing the truncated
last-of-family frames the +0x600 cap produced (Trees7, Banner8, Shell7).
Only files that already exist as .mio0 in the repo are replaced.
"""
import json, os, glob, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rom = open(os.path.join(ROOT, "baserom.us.z64"), "rb").read()

entries = []
for jp in glob.glob(os.path.join(ROOT, "assets", "*.json")):
    try: j = json.load(open(jp))
    except Exception: continue
    for name, v in j.items():
        if not isinstance(v, dict) or "rom_offset" not in v: continue
        off = int(v["rom_offset"], 16)
        if rom[off:off+4] != b"MIO0": continue
        out = os.path.join(ROOT, "assets", v.get("output_dir", ""), name + ".mio0")
        if not os.path.exists(out): continue     # only replace real mio0 assets
        entries.append((off, name, out))

def mio0_len(d):
    """Exact compressed blob length: walk the stream, track furthest read."""
    usize, coff, roff = struct.unpack(">III", d[4:16])
    n = 0; lbits = 16; lpos = 0x10; ctrl = 0; cpos = coff; rpos = roff
    while n < usize:
        if lbits == 16:
            ctrl = struct.unpack(">H", d[lpos:lpos+2])[0]; lpos += 2; lbits = 0
        if ctrl & 0x8000:
            rpos += 1; n += 1
        else:
            v = struct.unpack(">H", d[cpos:cpos+2])[0]; cpos += 2
            n += (v >> 12) + 3
        ctrl = (ctrl << 1) & 0xFFFF; lbits += 1
    return max(lpos, cpos, rpos)

done = 0
for off, name, out in sorted(entries):
    end = off + mio0_len(rom[off:off + 0x20000])
    blob = rom[off:end]
    (usize,) = struct.unpack(">I", blob[4:8])
    open(out, "wb").write(blob)
    print("ok %-45s off=%-9s usize=%-6d len=%d" % (os.path.relpath(out, ROOT), hex(off), usize, len(blob)))
    done += 1
print("replaced", done)

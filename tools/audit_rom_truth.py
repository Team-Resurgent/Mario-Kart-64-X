#!/usr/bin/env python3
"""Make every json-described asset array ROM-true (companion to
audit_common_arrays.py, which handles the 16-bit palettes/textures).

The PNG pipeline was internally self-consistent: scrambled palettes paired
with identically-scrambled CI indices, cancelling out -- except where indices
are reused across palettes (red shells over green indices). Flipping only the
palettes to ROM order (audit_common_arrays) broke the pairing for trees,
portraits and item icons. This tool completes the job: 1-byte (ci8/i8/ia8) and
4-bit (ci4/i4/ia4) index/texture arrays are rewritten to the ROM's exact
bytes, so both halves of every pairing match reality.
"""
import json, struct, re, io, os, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
rom = open(os.path.join(ROOT, "baserom.us.z64"), "rb").read()

def mio0_dec(d):
    usize, coff, roff = struct.unpack(">III", d[4:16])
    out = bytearray(); lbits = 16; lpos = 0x10; ctrl = 0; cpos = coff; rpos = roff
    while len(out) < usize:
        if lbits == 16:
            ctrl = struct.unpack(">H", d[lpos:lpos+2])[0]; lpos += 2; lbits = 0
        if ctrl & 0x8000:
            out.append(d[rpos]); rpos += 1
        else:
            v = struct.unpack(">H", d[cpos:cpos+2])[0]; cpos += 2
            ln = (v >> 12) + 3; dist = (v & 0xFFF) + 1
            for _ in range(ln): out.append(out[-dist])
        ctrl = (ctrl << 1) & 0xFFFF; lbits += 1
    return bytes(out)

blocks = {}
def block(base):
    if base not in blocks:
        blocks[base] = mio0_dec(rom[base:base + 0x40000])
    return blocks[base]

BPP = {"ci8": 1.0, "i8": 1.0, "ia8": 1.0, "ci4": 0.5, "i4": 0.5, "ia4": 0.5}
lit_re = re.compile(r'0[xX]([0-9A-Fa-f]{1,2})\b')

fixed = ok = nofile = mism = 0
for jp in glob.glob(os.path.join(ROOT, "assets", "*.json")):
    try: j = json.load(open(jp))
    except Exception: continue
    for name, v in j.items():
        if not isinstance(v, dict) or v.get("type") not in BPP: continue
        if "rom_offset" not in v: continue
        n = int(v.get("width", 0) * v.get("height", 0) * BPP[v["type"]])
        if not n: continue
        if "block_offset" in v:
            data = block(int(v["rom_offset"], 16))
            o = int(v["block_offset"], 16)
            raw = data[o:o + n]
        else:
            # raw offsets for byte types are usually MIO0 frames (handled by
            # extract_ci_frames_rom); skip unless the bytes are uncompressed.
            o = int(v["rom_offset"], 16)
            if rom[o:o+4] == b"MIO0": continue
            raw = rom[o:o + n]
        if len(raw) < n: continue
        cands = glob.glob(os.path.join(ROOT, "assets", "code", "*", name + ".*.inc.c"))
        if not cands: nofile += 1; continue
        p = cands[0]
        s = io.open(p, encoding="utf-8").read()
        lits = [int(x, 16) for x in lit_re.findall(s)]
        if len(lits) != n:
            print("LEN %-45s file=%d rom=%d" % (name, len(lits), n)); mism += 1; continue
        want = list(raw)
        if lits == want:
            ok += 1; continue
        it = iter(want)
        body = lit_re.sub(lambda m: "0x%02X" % next(it), s)
        io.open(p, "w", encoding="utf-8").write(body)
        diff = sum(1 for a, b in zip(lits, want) if a != b)
        print("FIXED %-45s bytes-changed=%d/%d" % (name, diff, n))
        fixed += 1
print("ok=%d fixed=%d len-mismatch=%d no-file=%d" % (ok, fixed, mism, nofile))

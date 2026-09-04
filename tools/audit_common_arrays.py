#!/usr/bin/env python3
"""Audit every 16-bit asset array against ROM ground truth.

The repo's u16 .inc.c files were generated with two different conventions:
  B) literal = bswap(ROM value)  -> compiled LE memory == raw BE byte stream
     (what every importer/loader expects; portraits, HUD, etc.)
  A) literal = ROM value verbatim -> loader's bswap scrambles it
     (the shell TLUTs -- mottled shells since day one)

For each assets/*.json entry carrying rom_offset [+ block_offset into a MIO0
block], compare the repo file to the ROM bytes and rewrite any convention-A
file into convention B. Files already matching are left untouched.
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

BPP2 = {"rgba16", "tlut", "ia16"}
lit_re = re.compile(r'0[xX]([0-9A-Fa-f]{1,4})\b')

fixed = ok_b = skipped = mism = 0
for jp in glob.glob(os.path.join(ROOT, "assets", "*.json")):
    try: j = json.load(open(jp))
    except Exception: continue
    for name, v in j.items():
        if not isinstance(v, dict) or v.get("type") not in BPP2: continue
        if "rom_offset" not in v: continue
        n = v.get("width", 0) * v.get("height", 0)
        if not n: continue
        if "block_offset" in v:
            data = block(int(v["rom_offset"], 16))
            o = int(v["block_offset"], 16)
            raw = data[o:o + n * 2]
        else:
            o = int(v["rom_offset"], 16)
            raw = rom[o:o + n * 2]
        if len(raw) < n * 2: continue
        rom_be = [struct.unpack(">H", raw[i*2:i*2+2])[0] for i in range(n)]
        # find the repo inc file by name
        cands = glob.glob(os.path.join(ROOT, "assets", "code", "*", name + ".*.inc.c"))
        if not cands:
            skipped += 1; continue
        p = cands[0]
        s = io.open(p, encoding="utf-8").read()
        lits = [int(x, 16) for x in lit_re.findall(s)]
        if len(lits) != n:
            print("LEN MISMATCH %-40s file=%d rom=%d" % (name, len(lits), n)); mism += 1; continue
        # TARGET = convention A: literals hold the ROM VALUES verbatim. The
        # DC port byteswaps these arrays IN PLACE at boot (main.c ~1590), so
        # the file must hold values for memory to end up as the byte stream
        # load_tlut expects. (The earlier A->B rewrite double-swapped them --
        # portraits/trees/items/shell palettes all scrambled at once.)
        want_b = [((x >> 8) | ((x & 0xFF) << 8)) for x in rom_be]
        if lits == rom_be:
            ok_b += 1; continue
        if lits == want_b:
            it = iter(rom_be)
            body = lit_re.sub(lambda m: "0x%04X" % next(it), s)
            io.open(p, "w", encoding="utf-8").write(body)
            print("FIXED B->A %-45s (%s)" % (name, os.path.relpath(p, ROOT)))
            fixed += 1; continue
        d_b = sum(1 for a, b in zip(lits, want_b) if a == b)
        d_a = sum(1 for a, b in zip(lits, rom_be) if a == b)
        print("NEITHER    %-45s matchB=%d matchA=%d n=%d" % (name, d_b, d_a, n)); mism += 1
print("ok(B)=%d fixed(A->B)=%d neither=%d no-file=%d" % (ok_b, fixed, mism, skipped))

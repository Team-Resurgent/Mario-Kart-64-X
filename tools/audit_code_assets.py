#!/usr/bin/env python3
"""Audit assets/code/** -- the .inc.c files that are actually COMPILED.

audit_all_assets.py walks the json files, which name assets under
assets/<group>/<name>.inc.c. But several segments are built from a parallel
set under assets/code/<segment>/<other_name>.inc.c, and those were never
compared to anything. This maps them onto their json entries by normalised
name and checks them against the ROM.

Reports, per file, which byte convention matches:
    RAW   = the ROM's own big-endian byte stream (what the importers want)
    SWAP  = 16-bit byteswapped (value order; some arrays are swapped at boot
            or at load time -- see load_ceremony_data / the main.c boot loop)
    NONE  = matches neither: genuinely wrong data
"""
import json, re, struct, glob, os, io

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
rom = io.open("baserom.us.z64", "rb").read()
_blocks = {}

BPP = {"ci8": 1, "i8": 1, "ia8": 1, "rgba16": 2, "ia16": 2,
       "ci4": 0.5, "i4": 0.5, "ia4": 0.5}


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


def norm(s):
    """gTexturePodium1 / texture_podium1.rgba16 -> texturepodium1

    The compiled files under assets/code drop the decomp's leading 'g' and
    use snake_case, so both sides are folded to bare alphanumerics with any
    leading 'g' removed.
    """
    s = os.path.basename(s)
    for suf in (".inc.c", ".rgba16", ".ia16", ".ci8", ".i8", ".ia8", ".i4", ".ci4"):
        s = s.replace(suf, "")
    s = re.sub(r"[^a-z0-9]", "", s.lower())
    return s[1:] if s.startswith("g") else s


# Every json entry, keyed by normalised name.
entries = {}
for jp in glob.glob("assets/**/*.json", recursive=True):
    try:
        j = json.load(open(jp))
    except Exception:
        continue
    for name, e in j.items():
        if isinstance(e, dict) and "rom_offset" in e and "block_offset" in e:
            entries.setdefault(norm(name), (name, e))


def file_bytes(path):
    t = io.open(path, encoding="utf-8", errors="replace").read()
    w = re.findall(r"0x([0-9a-fA-F]{4})\b", t)
    if w:
        return b"".join(struct.pack(">H", int(x, 16)) for x in w), "u16"
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", t)), "u8"


def swap16(b):
    return b"".join(b[i + 1:i + 2] + b[i:i + 1] for i in range(0, len(b) - 1, 2))


rows, unmatched = [], []
for p in sorted(glob.glob("assets/code/**/*.inc.c", recursive=True)):
    key = norm(p)
    if key not in entries:
        unmatched.append(p)
        continue
    name, e = entries[key]
    if e.get("type") not in BPP:
        unmatched.append(p)
        continue
    blk = decompress(int(str(e["rom_offset"]), 16))
    if blk is None:
        unmatched.append(p)
        continue
    exp = int(e["width"] * e["height"] * BPP[e["type"]])
    rb = blk[int(str(e["block_offset"]), 16):][:exp]
    fb, kind = file_bytes(p)
    if len(fb) != exp:
        verdict = "SIZE %d != %d" % (len(fb), exp)
    elif fb == rb:
        verdict = "RAW"
    elif swap16(fb) == rb:
        verdict = "SWAP"
    else:
        nd = sum(1 for a, b in zip(fb, rb) if a != b)
        verdict = "NONE (%d/%d bytes differ)" % (nd, exp)
    rows.append((verdict, p, name, kind))

for v, p, name, kind in sorted(rows):
    print("%-28s %-56s [%s as %s]" % (v, p, name, kind))
print("\n%d matched to a json entry, %d had none" % (len(rows), len(unmatched)))
from collections import Counter
print(Counter(r[0].split()[0] for r in rows))
if unmatched:
    print("\nno ROM description (cannot be verified):")
    for p in unmatched:
        print("   ", p)

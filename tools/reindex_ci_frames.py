#!/usr/bin/env python3
"""Re-index CI8 frame textures against their RUNTIME palettes.

The shell/tree/banner frame .bins were generated with n64graphics using each
PNG's own palette ordering, but at runtime the game loads the palette from the
u16 arrays in assets/code/common_data (via gDPLoadTLUT). Only ~15/256 entries
line up, so the indices select scrambled colours: mottled shells, off trees.

This decodes each frame PNG, maps every pixel to the runtime palette
(exact match on RGB553+alpha first, else nearest by weighted distance),
writes the .bin and re-compresses the .mio0 in place.
"""
import struct, zlib, re, io, os, glob, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MIO0 = os.path.join(ROOT, "tools", "mio0.exe")

DIRS = ["greenshell", "blueshell", "trees", "finish_line_banner"]

def png_decode(path):
    d = open(path, "rb").read()
    i = 8; idat = b""; ihdr = None; plte = None; trns = b""
    while i < len(d):
        ln, typ = struct.unpack(">I4s", d[i:i+8]); c = d[i+8:i+8+ln]
        if typ == b"IHDR": ihdr = struct.unpack(">IIBBBBB", c)
        elif typ == b"IDAT": idat += c
        elif typ == b"PLTE": plte = c
        elif typ == b"tRNS": trns = c
        i += 12 + ln
    w, h, depth, ctype = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
    raw = zlib.decompress(idat)
    ch = {0:1, 2:3, 3:1, 4:2, 6:4}[ctype]
    stride = w * ch
    out = bytearray(h * stride)
    pos = 0
    for y in range(h):
        f = raw[pos]; row = bytearray(raw[pos+1:pos+1+stride]); pos += 1 + stride
        prev = out[(y-1)*stride:y*stride] if y else bytearray(stride)
        for x in range(stride):
            a = row[x - ch] if x >= ch else 0
            b = prev[x]
            cD = prev[x - ch] if x >= ch else 0
            if f == 1: row[x] = (row[x] + a) & 0xFF
            elif f == 2: row[x] = (row[x] + b) & 0xFF
            elif f == 3: row[x] = (row[x] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                p = a + b - cD
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-cD)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else cD)
                row[x] = (row[x] + pred) & 0xFF
        out[y*stride:(y+1)*stride] = row
    px = []
    for y in range(h):
        for x in range(w):
            o = y*stride + x*ch
            if ctype == 6: r, g, b2, a2 = out[o], out[o+1], out[o+2], out[o+3]
            elif ctype == 2: r, g, b2, a2 = out[o], out[o+1], out[o+2], 255
            elif ctype == 3:
                idx = out[o]; r, g, b2 = plte[idx*3:idx*3+3]
                a2 = trns[idx] if idx < len(trns) else 255
            else: raise SystemExit("unsupported ctype %d in %s" % (ctype, path))
            px.append((r, g, b2, a2))
    return w, h, px

def runtime_pal(stem):
    for suf in (".rgba16.inc.c", ".tlut.inc.c"):
        p = os.path.join(ROOT, "assets", "code", "common_data", stem + suf)
        if os.path.exists(p): break
    else:
        return None
    s = io.open(p, encoding="utf-8").read()
    lits = [int(x, 16) for x in re.findall(r'0[xX]([0-9A-Fa-f]{1,4})\b', s)]
    pal = []
    for v in lits[:256]:
        be = ((v >> 8) | ((v & 0xFF) << 8))       # convention B: memory = BE stream
        pal.append((((be >> 11) & 31), ((be >> 6) & 31), ((be >> 1) & 31), be & 1))
    return pal

def best_index(pal, r, g, b, a):
    r5, g5, b5, a1 = r >> 3, g >> 3, b >> 3, (1 if a >= 128 else 0)
    # exact first
    for i, e in enumerate(pal):
        if e == (r5, g5, b5, a1): return i
    # nearest among same-alpha entries; fall back to any
    bi, bd = 0, 1 << 30
    for i, e in enumerate(pal):
        if e[3] != a1: continue
        d = 2*(e[0]-r5)**2 + 4*(e[1]-g5)**2 + 3*(e[2]-b5)**2
        if d < bd: bd, bi = d, i
    if bd == 1 << 30:
        for i, e in enumerate(pal):
            d = 2*(e[0]-r5)**2 + 4*(e[1]-g5)**2 + 3*(e[2]-b5)**2
            if d < bd: bd, bi = d, i
    return bi

total = 0
for dname in DIRS:
    d = os.path.join(ROOT, "assets", dname)
    tluts = glob.glob(os.path.join(d, "*tlut*.png"))
    if not tluts:
        print("SKIP", dname, "(no tlut png)"); continue
    stem = os.path.basename(tluts[0])[:-4]
    pal = runtime_pal(stem)
    if pal is None:
        print("SKIP", dname, "(no runtime inc for %s)" % stem); continue
    for m in sorted(glob.glob(os.path.join(d, "*.mio0"))):
        base = m[:-5]
        png = base + ".png"
        if not os.path.exists(png):
            print("  MISSING PNG", png); continue
        w, h, px = png_decode(png)
        cache = {}
        out = bytearray(w * h)
        for i, (r, g, b, a) in enumerate(px):
            k = (r >> 3, g >> 3, b >> 3, 1 if a >= 128 else 0)
            if k not in cache: cache[k] = best_index(pal, r, g, b, a)
            out[i] = cache[k]
        open(base + ".bin", "wb").write(bytes(out))
        if os.path.exists(m): os.remove(m)
        rr = subprocess.run([MIO0, "-c", base + ".bin", m], capture_output=True)
        ok = rr.returncode == 0 and os.path.exists(m)
        print("  %-60s %dx%d %s" % (os.path.relpath(m, ROOT), w, h, "OK" if ok else "MIO0 FAIL"))
        total += ok
print("reindexed", total)

import os
#!/usr/bin/env python3
"""Generate the .bin / .mio0 blobs the MIPS data files .incbin.

The Makefile does this with pattern rules (PNG -> raw via n64graphics ->
MIO0 via the mio0 tool). There is no make here, so the same two steps run
directly, driven by the list of incbin targets the .s files actually name.

Texture format comes from the per-asset JSON under assets/ where there is one,
otherwise from the filename suffix, exactly as the Makefile's
    -f $(lastword $(subst ., ,$@))
rule does.
"""
import io, json, os, re, subprocess, sys, glob

N64G = os.path.abspath("tools/n64graphics.exe")
MIO0 = os.path.abspath("tools/mio0.exe")
FMTS = ("rgba16","rgba32","ia16","ia8","ia4","ia1","i8","i4","ci8","ci4")

# name -> format, and dir -> palette, from the per-asset JSONs
fmt_by_path = {}
pal_by_dir = {}
for j in glob.glob("assets/*.json") + glob.glob("assets/*/*.json"):
    try: d = json.load(io.open(j, encoding="utf-8"))
    except Exception: continue
    base = os.path.dirname(os.path.realpath(j))
    for name, a in d.items():
        if not isinstance(a, dict): continue
        od = a.get("output_dir"); t = a.get("type")
        if not od or not t: continue
        p = os.path.relpath(os.path.join(base, od, name), ".").replace("\\","/")
        fmt_by_path[p] = t
        # CI textures need their palette passed alongside; the .mk files pair
        # them per directory, and the palette is the tlut-named entry.
        if re.search(r"tlut|palette", name, re.I):
            pal_by_dir[os.path.dirname(p)] = p + ".png"

def fmt_for(stem):
    if stem in fmt_by_path: return fmt_by_path[stem]
    tail = os.path.basename(stem).rsplit(".", 1)
    if len(tail) == 2 and tail[1] in FMTS: return tail[1]
    return None

def make_bin(binpath):
    if os.path.exists(binpath): return True
    stem = binpath[:-4] if binpath.endswith(".bin") else binpath
    png = stem + ".png"
    if not os.path.exists(png):
        # some .s reference the stem without the format suffix
        alt = glob.glob(stem + ".*.png")
        png = alt[0] if alt else None
    if not png: return False
    f = fmt_for(stem) or fmt_for(png[:-4])
    if not f: return False
    os.makedirs(os.path.dirname(binpath) or ".", exist_ok=True)
    d, b = os.path.dirname(binpath), os.path.basename(binpath)[:-4]
    # Kart frames get a per-frame stitched palette and a wheel mask, per
    # assets/include/karts/*.mk:
    #   -Z out -g in.png -s raw -f ci8 -c rgba16
    #      -p <dir>/stitched_palettes/<name>_stitched_palette.png
    #      -M <dir>/wheel_masks/<name>_wheel_mask.raw
    sp = os.path.join(d, "stitched_palettes", b + "_stitched_palette.png")
    wm = os.path.join(d, "wheel_masks", b + "_wheel_mask.raw")
    if os.path.exists(sp):
        cmd = [N64G, "-Z", binpath, "-g", png, "-s", "raw", "-f", "ci8", "-c", "rgba16", "-p", sp]
        if os.path.exists(wm): cmd += ["-M", wm]
        r = subprocess.run(cmd, capture_output=True)
        return r.returncode == 0 and os.path.exists(binpath)
    if f.startswith("ci"):
        # Matches the .mk recipe exactly:
        #   n64graphics -Z out.bin -g in.png -s raw -f ci8 -c rgba16 -p palette.png
        pal = pal_by_dir.get(os.path.dirname(binpath))
        if not pal or not os.path.exists(pal): return False
        cmd = [N64G, "-Z", binpath, "-g", png, "-s", "raw", "-f", f, "-c", "rgba16", "-p", pal]
    else:
        cmd = [N64G, "-i", binpath, "-g", png, "-f", f]
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode == 0 and os.path.exists(binpath)

def make_mio0(mio0path):
    if os.path.exists(mio0path): return True
    raw = mio0path[:-5]                    # strip .mio0
    if not os.path.exists(raw):
        if not make_bin(raw if raw.endswith(".bin") else raw + ".bin"):
            if not make_bin(raw): return False
        if not os.path.exists(raw) and os.path.exists(raw + ".bin"):
            raw = raw + ".bin"
    if not os.path.exists(raw): return False
    os.makedirs(os.path.dirname(mio0path) or ".", exist_ok=True)
    r = subprocess.run([MIO0, "-c", raw, mio0path], capture_output=True)
    return r.returncode == 0 and os.path.exists(mio0path)

_objdir = os.environ.get("RXDK_OBJ_DIR") or ("out/Release" if os.path.isdir("out/Release") else "out")
targets = [l.strip() for l in io.open(_objdir + "/incbins.txt", encoding="utf-8") if l.strip()]
ok = skip = fail = 0
missing_examples = []
for t in targets:
    if os.path.exists(t): skip += 1; continue
    if t.endswith(".mio0"): good = make_mio0(t)
    elif t.endswith(".bin"): good = make_bin(t)
    else: good = make_bin(t)
    if good: ok += 1
    else:
        fail += 1
        if len(missing_examples) < 15: missing_examples.append(t)
print("targets:", len(targets), " already present:", skip, " generated:", ok, " failed:", fail)
for e in missing_examples: print("  fail:", e)

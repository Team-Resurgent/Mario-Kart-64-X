#!/usr/bin/env python3
"""Find generated asm labels that lost their .incbin.

gen_binassets.py can only build a .bin/.mio0 when it knows how (a single PNG,
a raw .bin, ...). When it cannot, the generator emits the label but no
.incbin -- so the symbol silently ALIASES whatever data follows it and the
game reads a completely different texture. gTextureGhosts (Banshee's Boos)
did exactly this: it resolved to gTextureExhaust0.

Compares each generated Platform/xbox/gen_asm/*.c against the original
data/*.s it was produced from, and lists labels whose .incbin went missing.
"""
import os, re, glob, io

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

QUOTE = chr(34)
BACKSLASH = chr(92)


def gen_lines(path):
    """Unescape the generated file's C string lines back to plain asm."""
    out = []
    for raw in io.open(path, encoding="utf-8", errors="replace"):
        s = raw.strip()
        if not s.startswith(QUOTE):
            continue
        s = s.strip(",").strip()
        if s.startswith(QUOTE):
            s = s[1:]
        if s.endswith(QUOTE):
            s = s[:-1]
        if s.endswith(BACKSLASH + "n"):
            s = s[:-2]
        out.append(s.replace(BACKSLASH + QUOTE, QUOTE))
    return out


def labels_with_incbin(lines, label_re):
    """Map label -> incbin target (or None when the label has no data)."""
    res = {}
    cur = None
    for l in lines:
        m = label_re.match(l.strip())
        if m:
            cur = m.group(1)
            res.setdefault(cur, None)
            continue
        if cur and ".incbin" in l:
            m2 = re.search(r'\.incbin\s+"([^"]+)"', l)
            if m2 and res.get(cur) is None:
                res[cur] = m2.group(1)
    return res


def find_sources():
    """Every .s anywhere that could have produced these labels."""
    pats = ("data/*.s", "karts/**/*.s", "assets/**/*.s", "courses/**/*.s", "*.s")
    out = []
    for p in pats:
        out += glob.glob(p, recursive=True)
    return out


ORIG = {}
for sp in find_sources():
    lines = [l.rstrip("\n") for l in io.open(sp, encoding="utf-8", errors="replace")]
    for k, v in labels_with_incbin(lines, re.compile(r"^glabel\s+_?(\w+)$")).items():
        if v and k not in ORIG:
            ORIG[k] = (v, sp)

gen_label = re.compile(r"^_(\w+):$")
src_label = re.compile(r"^(?:glabel|\.global)?\s*_?(\w+):?$")

total = 0
for gpath in sorted(glob.glob("Platform/xbox/gen_asm/*.c")):
    base = os.path.basename(gpath)
    gl = labels_with_incbin(gen_lines(gpath), gen_label)
    dropped = [k for k, v in gl.items() if v is None]
    if not dropped:
        continue
    # Only a label the ORIGINAL source gave data to is genuinely dropped;
    # *_end / *SegmentStart markers are meant to carry none.
    real = [(d, ORIG[d]) for d in dropped if d in ORIG]
    if not real:
        continue
    print("%s: %d dropped" % (base, len(real)))
    for d, (want, src) in real:
        exists = "" if os.path.exists(want) else "   [asset file MISSING]"
        print("    %-30s <- %s%s" % (d, want, exists))
        total += 1
print("\n%d genuinely dropped asset(s)" % total)

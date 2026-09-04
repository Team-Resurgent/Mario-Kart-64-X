#!/usr/bin/env python3
"""Turn the decomp's data-only MIPS .s files into C files RXDK can build.

RXDK's manifest only takes .c/.cpp sources and compiles everything with -x c,
so a .s cannot be listed directly. File-scope __asm__ in a .c file reaches the
same integrated assembler, so each .s becomes one generated .c wrapping its
contents in an asm block.

Three transformations are needed:
  glabel   expanded inline (no .include, so the MIPS macros.inc is not needed)
  .word    is 32-bit on MIPS but 16-bit on x86 -> .long
  .hword   -> .short
  .incbin  paths made absolute, since the assembler resolves them against the
           compiler's working directory, which RXDK controls, not us.

The labels already carry the leading underscore that x86 Windows uses for C
symbols, so they need no renaming.
"""
import io, os, re, sys, glob

ROOT = os.path.abspath(".")
OUT  = "Platform/xbox/gen_asm"
os.makedirs(OUT, exist_ok=True)
for f in glob.glob(OUT + "/*.c"): os.remove(f)

BS = chr(92)
def esc(x):
    return x.replace(BS, BS + BS).replace(chr(34), BS + chr(34))
NL = BS + "n"

# Not every .s belongs in the image:
#   data/sound_data/*  main.c declares _audio_banksSegmentRomStart and friends
#                      as POINTERS and fills them from files loaded off the
#                      disc at runtime -- the Dreamcast port's design. Linking
#                      the .s data in would collide with those definitions.
#   data/textures_0a.s covers the same ROM range (segment 0x0A,
#                      0x729A30-0x7E684F) as course_player_selection.s; they
#                      are alternate splits of one region, not complements.
EXCLUDE = ("data/sound_data/", "data/textures_0a.s")
srcs = sorted(p.replace("\\", "/") for p in glob.glob("data/**/*.s", recursive=True))
srcs = [p for p in srcs if not any(p.startswith(e) or p == e for e in EXCLUDE)]
srcs += sorted(p.replace("\\", "/") for p in glob.glob("src/**/*.s", recursive=True))

made, skipped_missing = 0, 0
for sp in srcs:
    lines, missing = [], 0
    cur_label = "<none>"
    for raw in io.open(sp, encoding="utf-8", errors="surrogateescape"):
        # n64split emits '!' comments in some files and '#' in others; strip
        # both, but only outside quoted strings so .incbin paths survive.
        t, q = [], False
        for ch in raw.rstrip(chr(13) + chr(10)):
            if ch == chr(34): q = not q
            if not q and ch in "#!": break
            t.append(ch)
        t = "".join(t).rstrip()
        if not t.strip(): continue
        s = t.strip()
        if s.startswith(".include"): continue          # macros.inc: expanded below
        if s.startswith(".set "): continue             # MIPS assembler settings
        m = re.match(r'^glabel\s+(\S+)$', s) or re.match(r'^export\s+(\S+)$', s)
        if m:
            cur_label = m.group(1)
            lines += [".global %s" % cur_label, ".balign 4", "%s:" % cur_label]
            continue
        m = re.match(r'^\.incbin\s+"([^"]+)"$', s)
        if m:
            p = m.group(1)
            ap = os.path.join(ROOT, p).replace("\\", "/")
            if not os.path.exists(ap):
                # Emitting the label with no data is NOT harmless: the symbol
                # then resolves to whatever data follows it. gTextureGhosts did
                # this and Banshee's Boos decoded gTextureExhaust0's bytes.
                # Name the symbol so the aliasing is never silent again --
                # tools/audit_dropped_incbins.py finds any that slip through.
                missing += 1
                print("  DROPPED %-28s (%s missing) -- symbol will ALIAS the "
                      "next asset" % (cur_label, p))
                continue
            lines.append('.incbin "%s"' % ap)
            continue
        s = re.sub(r'^\.word\b',  '.long',  s)
        s = re.sub(r'^\.hword\b', '.short', s)
        lines.append(s)
    name = sp.replace("/", "_").replace(".s", "")
    out = os.path.join(OUT, name + ".c")
    with io.open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write('/* Generated from %s -- see tools/gen_asmwrappers.py.\n'
                ' * Data-only assembly, reached through file-scope __asm__ because\n'
                ' * RXDK builds only .c/.cpp sources. */\n' % sp)
        f.write('__asm__(\n')
        for l in lines:
            f.write(chr(32)*4 + chr(34) + esc(l) + NL + chr(34) + chr(10))
        f.write(');\n')
    made += 1
    skipped_missing += missing
    if missing: print("  %-42s %d incbin targets still missing" % (sp, missing))
print("wrappers written:", made, " incbin targets skipped:", skipped_missing)

#!/usr/bin/env python3
"""Find 16-bit common_data arrays that are drawn BY SYMBOL but never swapped.

The common_data .inc.c files hold ROM values verbatim (convention A), so the
compiled u16 array's little-endian image in x86 RAM is the ROM's byte stream
REVERSED. Every consumer that passes the array symbol straight to a texture or
TLUT loader therefore needs main.c's boot-time swap. Consumers that instead use
a 0x0Dxxxxxx segment address are already fine -- the segment copy is ROM-true.

D_0D02AA58 (the bomb kart's wheels) lost its swap and rendered blue/purple.
This lists any other array in the same position.
"""
import io, os, re, glob

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# 1. Arrays main.c swaps at boot.
main_c = io.open("src/main.c", encoding="utf-8", errors="replace").read()
swapped = set(re.findall(r"segmented_to_virtual\(\s*([A-Za-z_]\w*)", main_c))

# 2. 16-bit arrays defined in common_data.c (u16 => needs byte-order care).
cd = io.open("assets/code/common_data/common_data.c", encoding="utf-8",
             errors="replace").read()
u16_arrays = set(re.findall(r"^\s*u16\s+(\w+)\s*\[", cd, re.M))

# 3. Where each is used as a texture/palette source, by symbol.
LOADERS = ("gDPLoadTLUT_pal256", "rsp_load_texture", "rsp_load_texture_mask",
           "load_texture_block_rgba16_mirror", "load_texture_and_tlut",
           "draw_2d_texture_at", "draw_rectangle_texture_overlap",
           "init_texture_object", "func_80046F60", "func_8004747C",
           "func_80047068", "draw_2d_texture_at_no_inval")
loader_re = re.compile(r"\b(?:%s)\s*\(" % "|".join(LOADERS))

uses = {}
for src in glob.glob("src/**/*.c", recursive=True):
    if src.replace("\\", "/").endswith("src/main.c"):
        continue
    txt = io.open(src, encoding="utf-8", errors="replace").read()
    for m in loader_re.finditer(txt):
        # Grab the call's argument list (single line is enough here).
        end = txt.find(";", m.end())
        args = txt[m.end():end if end > 0 else m.end() + 300]
        for name in u16_arrays:
            if re.search(r"\b%s\b" % re.escape(name), args):
                uses.setdefault(name, set()).add(os.path.basename(src))

missing = sorted(n for n in uses if n not in swapped)
ok = sorted(n for n in uses if n in swapped)

print("16-bit common_data arrays drawn by symbol: %d" % len(uses))
print("  swapped at boot : %d" % len(ok))
print("  NOT swapped     : %d" % len(missing))
for n in missing:
    print("     %-46s used in %s" % (n, ", ".join(sorted(uses[n]))))

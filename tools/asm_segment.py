#!/usr/bin/env python3
"""Assemble one data-only .s and emit its raw .data as a binary.

Used for the sound segments, which are not pure .incbin -- instrument_sets.s
is a table of label-difference .hwords -- so they have to be genuinely
assembled rather than concatenated. Same transformations as
gen_asmwrappers.py; see that file for why each is needed.
"""
import io, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coff_section

ZIG = os.path.expandvars(r"%LOCALAPPDATA%\RXDK\zig\0.16.0\zig-x86_64-windows-0.16.0\zig.exe")
BS  = chr(92)
ROOT = os.path.abspath(".")

def esc(x): return x.replace(BS, BS + BS).replace(chr(34), BS + chr(34))

def build(sp, dst):
    lines = []
    for raw in io.open(sp, encoding="utf-8", errors="surrogateescape"):
        t, q = [], False
        for ch in raw.rstrip(chr(13) + chr(10)):
            if ch == chr(34): q = not q
            if not q and ch in "#!": break
            t.append(ch)
        s = "".join(t).strip()
        if not s or s.startswith(".include") or s.startswith(".set "): continue
        m = re.match(r'^glabel\s+(\S+)$', s) or re.match(r'^export\s+(\S+)$', s)
        if m:
            lines += [".global %s" % m.group(1), ".balign 4", "%s:" % m.group(1)]
            continue
        m = re.match(r'^\.incbin\s+"([^"]+)"$', s)
        if m:
            ap = os.path.join(ROOT, m.group(1)).replace(BS, "/")
            if os.path.exists(ap): lines.append('.incbin "%s"' % ap)
            continue
        s = re.sub(r'^\.word' + chr(92) + 'b',  '.long',  s)
        s = re.sub(r'^\.hword' + chr(92) + 'b', '.short', s)
        lines.append(s)

    c = "__asm__(\n" + "".join('    "%s' % esc(l) + BS + 'n"\n' for l in lines) + ");\n"
    tmp_c = dst + ".gen.c"; tmp_o = dst + ".gen.obj"
    io.open(tmp_c, "w", encoding="utf-8", newline="\n").write(c)
    r = subprocess.run([ZIG, "cc", "-std=c23", "-target", "x86-windows-gnu", "-O1",
                        "-ffreestanding", "-nostdinc", "-march=pentium3",
                        "-c", tmp_c, "-o", tmp_o], capture_output=True)
    if r.returncode != 0:
        print("  assemble failed:", sp, r.stderr.decode(errors="replace")[:180]); return False
    # See coff_section.py: llvm-objcopy -O binary emits the whole object here.
    b = coff_section.extract(tmp_o, ".data")
    ok = bool(b)
    if ok: io.open(dst, "wb").write(b)
    for f in (tmp_c, tmp_o):
        if os.path.exists(f): os.remove(f)
    return ok

for sp, dst in [(sys.argv[i], sys.argv[i+1]) for i in range(1, len(sys.argv), 2)]:
    ok = build(sp, dst)
    print(("  ok  " if ok else "  FAIL") , sp, "->", dst,
          ("(%d bytes)" % os.path.getsize(dst)) if ok else "")

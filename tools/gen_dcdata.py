#!/usr/bin/env python3
"""Build the dc_data/ set the game loads off the disc at runtime.

The Dreamcast port streams course and audio data from the disc rather than
linking it all in -- the right design for 16MB, and still the right one for
64MB. generate_dc_data.sh assembles that set from the MIPS build's artifacts;
this does the same from the x86 build's, plus the raw binaries already
extracted from the ROM.

Segment binaries are the .data section of the corresponding object, which is
exactly what the Dreamcast build does (compile the same .c, objcopy -O binary).
"""
import io, os, re, subprocess, glob, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MIO0 = os.path.abspath("tools/mio0.exe")
OUT = "dc_data"
os.makedirs(OUT, exist_ok=True)

import coff_section
import coff_reloc

def objcopy(obj, dst):
    """Extract the object's .data. NOT llvm-objcopy -O binary: on these COFF
    objects that emits the whole object file, header and symbol table
    included, which silently produced oversized segment binaries that overran
    their runtime buffers."""
    if not os.path.exists(obj): return False
    b = coff_section.extract(obj, ".data")
    if not b: return False
    io.open(dst, "wb").write(b)
    return True

def concat_incbins(s_path, dst):
    """Reproduce an assembled data segment by concatenating what it .incbins,
    honouring .balign padding so offsets match."""
    if not os.path.exists(s_path): return False
    buf = bytearray()
    align = 1
    for raw in io.open(s_path, encoding="utf-8", errors="surrogateescape"):
        t, q = [], False
        for ch in raw.rstrip(chr(13) + chr(10)):
            if ch == chr(34): q = not q
            if not q and ch in "#!": break
            t.append(ch)
        line = "".join(t).strip()
        m = re.match(r'^\.balign\s+(\d+)', line) or re.match(r'^\.align\s+(\d+)', line)
        if m:
            a = int(m.group(1))
            if line.startswith(".align"): a = 1 << a
            while len(buf) % a: buf.append(0)
            continue
        m = re.match(r'^\.incbin\s+"([^"]+)"$', line)
        if m and os.path.exists(m.group(1)):
            buf += io.open(m.group(1), "rb").read()
    if not buf: return False
    io.open(dst, "wb").write(bytes(buf))
    return True

# The game divides _geography.bin at a hardcoded offset per course; if our
# compressor's output size differs from the one those constants were built
# against, the split lands in the wrong place. Check rather than assume.
PACKOFFS = {}
try:
    import re as _re
    _m = _re.search(r"u32 packoffs\[20\]\s*=\s*\{(.*?)\};",
                    io.open("src/racing/memory.c", encoding="utf-8", errors="surrogateescape").read(), _re.S)
    _v = _re.findall(r"0x([0-9A-Fa-f]{8})", _m.group(1))
    _n = _re.findall(r"d_course_([a-z_0-9]+)_packed", _m.group(1))
    PACKOFFS = {n: int(v, 16) for n, v in zip(_n, _v)}
except Exception:
    pass

made, missed = [], []

# --- audio: raw ROM rips already extracted, plus two assembled segments ---
for src, dst in (("bin/audiobanks.us.bin", "audiobanks.bin"),
                 ("bin/audiotables.bin",   "audiotables.bin")):
    if os.path.exists(src):
        shutil.copyfile(src, os.path.join(OUT, dst)); made.append(dst)
    else: missed.append(dst)

for s, dst in (("data/sound_data/instrument_sets.s", "instrument_sets.bin"),
               ("data/sound_data/sequences.s",       "sequences.bin")):
    (made if concat_incbins(s, os.path.join(OUT, dst)) else missed).append(dst)

# The AICA ADPCM pool is Dreamcast sound-hardware specific and is only read by
# the mixer, which is stubbed on Xbox. An empty file keeps the loader happy.
io.open(os.path.join(OUT, "adpcm_pool.bin"), "wb").write(b"\0" * 32)
made.append("adpcm_pool.bin")

OBJDIR = os.environ.get("RXDK_OBJ_DIR") or ("out/Release" if os.path.isdir("out/Release") else "out")

# --- segments compiled into objects by this build ---
for obj, dst in ((OBJDIR + "/common_data.obj",   "common_data.bin"),
                 (OBJDIR + "/ceremony_data.obj", "ceremony_data.bin")):
    (made if objcopy(obj, os.path.join(OUT, dst)) else missed).append(dst)

# --- per-course ---
# The game streams three files per course (see racing/memory.c):
#   <course>_data.bin        course_data           the course structure
#   <course>_tex.bin         course_textures.linkonly  its texture blob
#   <course>_geography.bin   course_vertices.inc   its geometry
# The Makefile builds the latter two under its "Course Geography Generation"
# section from exactly these objects.
# generate_dc_data.sh is authoritative on which of these are compressed:
#   _data.bin       course_data.bin                        raw
#   _tex.bin        .data of course_textures.linkonly.o    raw
#   _geography.bin  .data of course_geography.MIO0.o       MIO0-COMPRESSED
# decompress_vtx MIO0-decodes the geography, so handing it raw vertices makes
# it read a garbage header and write past its target.
COURSE_PARTS = (("course_data",              "_data.bin",      False),
                ("course_textures.linkonly", "_tex.bin",       False),
                ("course_vertices.inc",      "_geography.bin", True))

for d in sorted(glob.glob("courses/*/")):
    c = os.path.basename(d.rstrip("/\\"))
    # Relocate the whole course group together at the segment bases the
    # Makefile links them at, so cross-references between data, displaylists
    # and textures resolve exactly as the linker would resolve them.
    group = [(OBJDIR + "/courses_%s_course_textures.linkonly.obj" % c, 0x05000000),
             (OBJDIR + "/courses_%s_course_data.obj"              % c, 0x06000000),
             (OBJDIR + "/courses_%s_course_displaylists.inc.obj"  % c, 0x07000000),
             (OBJDIR + "/courses_%s_course_vertices.inc.obj"      % c, 0x0F000000)]
    relocated, unres = coff_reloc.build(group)
    if unres:
        print("  %s: %d unresolved symbols" % (c, len(unres)))

    for objbase, suffix, compress in COURSE_PARTS:
        obj = OBJDIR + "/courses_%s_%s.obj" % (c, objbase)
        dst = os.path.join(OUT, c + suffix)
        blob = relocated.get(obj)
        if blob is None:
            missed.append(c + suffix); continue
        if not compress:
            io.open(dst, "wb").write(blob)
            made.append(c + suffix)
            continue

        # _geography.bin is NOT just the compressed vertices. Per the
        # course_geography.mio0.s rule it is:
        #     mio0(course_vertices.inc)  +  4-byte align  +  course_displaylists.inc
        # and load_course splits it at the hardcoded packoffs[] offset, which
        # is where d_course_<name>_packed lands. Writing only the first half
        # left the packed display lists empty.
        tmp = dst + ".raw"
        io.open(tmp, "wb").write(blob)
        r = subprocess.run([MIO0, "-c", tmp, dst + ".mio0"], capture_output=True)
        os.remove(tmp)
        if r.returncode != 0 or not os.path.exists(dst + ".mio0"):
            missed.append(c + suffix); continue
        vtx = io.open(dst + ".mio0", "rb").read()
        os.remove(dst + ".mio0")
        pad = (-len(vtx)) % 4
        packed = relocated.get(OBJDIR + "/courses_%s_course_displaylists.inc.obj" % c, b"")
        io.open(dst, "wb").write(vtx + bytes(pad) + packed)
        split = len(vtx) + pad
        want = PACKOFFS.get(c)
        if want is not None and split != want:
            print("  %-20s packoffs MISMATCH: split=%d expected=%d" % (c, split, want))
        made.append(c + suffix)

print("written:", len(made), " missing:", len(missed))
if missed: print("  missing:", ", ".join(missed[:12]))
total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
print("dc_data total: %.1f MB" % (total / 1048576.0))

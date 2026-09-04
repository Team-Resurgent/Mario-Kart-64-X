#!/usr/bin/env python3
"""Extract a course/segment binary with its relocations APPLIED.

The Makefile does not dump these from the object file. It LINKS each one at
its N64 segment base and dumps the result:

    course_textures.linkonly.elf   -Ttext=05000000
    course_data.elf                -Ttext=06000000  -R course_displaylists.inc.elf
    course_displaylists.inc.elf    -Ttext=07000000  -R course_textures.linkonly.elf
    course_vertices.inc.elf        -Ttext=0F000000

That link is what turns each internal reference into a segmented address the
game can resolve at runtime. Dumping the unlinked object instead leaves every
one of them unrelocated -- 2259 of them in mario_raceway's course_data alone --
so the course loads and then walks into nonsense.

Rather than stand up a second ELF toolchain just to relink, this reads the COFF
symbol and relocation tables directly and applies them, which is the same
arithmetic the linker would do: each symbol resolves to its object's segment
base plus its offset.
"""
import io, os, struct, sys

IMAGE_REL_I386_DIR32   = 0x0006
IMAGE_REL_I386_DIR32NB = 0x0007
IMAGE_REL_I386_REL32   = 0x0014

class Obj:
    def __init__(self, path, base):
        self.path, self.base = path, base
        self.d = io.open(path, "rb").read()
        _, nsec, _, symptr, nsym, opthdr, _ = struct.unpack_from("<HHIIIHH", self.d, 0)
        self.nsec, self.symptr, self.nsym = nsec, symptr, nsym
        self.secs = []                      # (name, size, rawptr, relptr, nrel)
        off = 20 + opthdr
        for i in range(nsec):
            b = off + i * 40
            name = self.d[b:b+8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, size, ptr = struct.unpack_from("<IIII", self.d, b + 8)
            relptr, lineptr, nrel, nline = struct.unpack_from("<IIHH", self.d, b + 24)
            self.secs.append((name, size, ptr, relptr, nrel))
        self.strtab = symptr + nsym * 18

    def sym_name(self, i):
        b = self.symptr + i * 18
        raw = self.d[b:b+8]
        if raw[:4] == b"\0\0\0\0":
            so = self.strtab + struct.unpack_from("<I", raw, 4)[0]
            end = self.d.index(b"\0", so)
            return self.d[so:end].decode("ascii", "replace")
        return raw.rstrip(b"\0").decode("ascii", "replace")

    def sym(self, i):
        # A COFF symbol entry is 18 bytes: an 8-byte name FIRST, then the
        # fields. Reading the fields from offset 0 parses the name as a value.
        b = self.symptr + i * 18 + 8
        value, secnum, typ, cls, naux = struct.unpack_from("<IhHBB", self.d, b)
        return value, secnum, cls, naux

    def data_index(self):
        for i, (name, size, ptr, relptr, nrel) in enumerate(self.secs):
            if name == ".data":
                return i
        return -1

    def defined_symbols(self):
        """name -> absolute address, for symbols defined in this object's .data."""
        out, i, di = {}, 0, self.data_index()
        while i < self.nsym:
            value, secnum, cls, naux = self.sym(i)
            if secnum - 1 == di and secnum > 0:
                out[self.sym_name(i)] = self.base + value
            i += 1 + naux
        return out

def build(objs):
    """objs: list of (path, base). Returns {path: relocated .data bytes}."""
    loaded = [Obj(p, b) for p, b in objs if os.path.exists(p)]
    table = {}
    for o in loaded:
        table.update(o.defined_symbols())

    out, unresolved = {}, set()
    for o in loaded:
        di = o.data_index()
        if di < 0: continue
        name, size, ptr, relptr, nrel = o.secs[di]
        buf = bytearray(o.d[ptr:ptr + size])
        for r in range(nrel):
            rb = relptr + r * 10
            va, symidx, rtype = struct.unpack_from("<IIH", o.d, rb)
            sname = o.sym_name(symidx)
            target = table.get(sname)
            if target is None:
                unresolved.add(sname); continue
            if va + 4 > len(buf): continue
            addend = struct.unpack_from("<I", buf, va)[0]
            if rtype in (IMAGE_REL_I386_DIR32, IMAGE_REL_I386_DIR32NB):
                struct.pack_into("<I", buf, va, (target + addend) & 0xFFFFFFFF)
            elif rtype == IMAGE_REL_I386_REL32:
                struct.pack_into("<I", buf, va, (target + addend - (o.base + va + 4)) & 0xFFFFFFFF)
        out[o.path] = bytes(buf)
    return out, unresolved

if __name__ == "__main__":
    course = sys.argv[1]
    objdir = os.environ.get("RXDK_OBJ_DIR") or ("out/Release" if os.path.isdir("out/Release") else "out")
    objs = [(objdir + "/courses_%s_course_textures.linkonly.obj" % course, 0x05000000),
            (objdir + "/courses_%s_course_data.obj"              % course, 0x06000000),
            (objdir + "/courses_%s_course_displaylists.inc.obj"  % course, 0x07000000),
            (objdir + "/courses_%s_course_vertices.inc.obj"      % course, 0x0F000000)]
    res, unres = build(objs)
    for p, b in res.items():
        print("  %-52s %8d bytes" % (os.path.basename(p), len(b)))
    if unres:
        print("  unresolved: %d e.g. %s" % (len(unres), sorted(unres)[:4]))

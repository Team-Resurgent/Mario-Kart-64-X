#!/usr/bin/env python3
"""Extract one section's raw bytes from a COFF object.

llvm-objcopy -O binary on these COFF objects emits the whole object file, not
the section contents -- the generated .bin files were object files with the
COFF header and symbol table still in them, which is how a 136-byte segment
became 1648 bytes and overran a 256-byte buffer at runtime.

The COFF layout is small enough to read directly and leaves no doubt:
  20-byte file header, then one 40-byte section header per section, each
  giving the section's raw size and file offset.
"""
import io, struct, sys

def extract(obj_path, section=".data"):
    d = io.open(obj_path, "rb").read()
    if len(d) < 20: return None
    machine, nsec, _ts, symptr, nsym, opthdr, _ch = struct.unpack_from("<HHIIIHH", d, 0)
    off = 20 + opthdr
    # Long section names are "/<offset>" into the string table, which follows
    # the symbol table (18 bytes per symbol).
    strtab = symptr + nsym * 18
    for i in range(nsec):
        base = off + i * 40
        raw_name = d[base:base + 8]
        name = raw_name.rstrip(b"\0").decode("ascii", "replace")
        if name.startswith("/"):
            try:
                so = strtab + int(name[1:])
                end = d.index(b"\0", so)
                name = d[so:end].decode("ascii", "replace")
            except Exception:
                pass
        vsize, vaddr, size, ptr = struct.unpack_from("<IIII", d, base + 8)
        if name == section:
            if ptr == 0 or size == 0: return b""
            return d[ptr:ptr + size]
    return None

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    sec = sys.argv[3] if len(sys.argv) > 3 else ".data"
    b = extract(src, sec)
    if b is None:
        print("  no %s section in %s" % (sec, src)); sys.exit(1)
    io.open(dst, "wb").write(b)
    print("  %-46s %8d bytes" % (dst, len(b)))

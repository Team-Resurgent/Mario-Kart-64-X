#!/usr/bin/env python3
"""Resolve a crash EIP from xbWatson to a function name, via the PDB.

xbWatson prints e.g.
    Exception: 0x0000001C 0xC0000005 0x000E5B14 0x00000000 0x00000000 0x00000000
                          |          |          |          |
                          |          EIP        |          faulting address
                          access violation      0 = read, 1 = write

Usage:
    python tools/symbolize.py 0x000E5B14 [more addrs...]
    python tools/symbolize.py --pdb out/Debug/mk64x.pdb 0x...

Reads the PDB's public symbols directly (MSF container -> DBI -> symbol
records + section headers), so it needs no external tools; RXDK ships no
symbolizer and zig cc refuses -Wl,-Map.
"""
import io, os, struct, sys, bisect

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

S_PUB32 = 0x110E
S_LPROC32 = 0x110F
S_GPROC32 = 0x1110


class MSF:
    def __init__(self, path):
        self.d = io.open(path, "rb").read()
        if self.d[:26] != b"Microsoft C/C++ MSF 7.00\r\n":
            raise SystemExit("not an MSF 7.00 PDB: " + path)
        (self.bs, _free, self.nblocks, self.dirbytes,
         _unk, self.dirmap) = struct.unpack_from("<6I", self.d, 32)
        ndirblocks = (self.dirbytes + self.bs - 1) // self.bs
        idx = struct.unpack_from("<%dI" % ndirblocks, self.d, self.dirmap * self.bs)
        directory = self._read(idx, self.dirbytes)
        nstreams = struct.unpack_from("<I", directory, 0)[0]
        sizes = struct.unpack_from("<%dI" % nstreams, directory, 4)
        pos = 4 + 4 * nstreams
        self.streams = []
        for sz in sizes:
            n = 0 if sz in (0, 0xFFFFFFFF) else (sz + self.bs - 1) // self.bs
            blocks = struct.unpack_from("<%dI" % n, directory, pos)
            pos += 4 * n
            self.streams.append((blocks, 0 if sz == 0xFFFFFFFF else sz))

    def _read(self, blocks, size):
        out = bytearray()
        for b in blocks:
            out += self.d[b * self.bs:(b + 1) * self.bs]
        return bytes(out[:size])

    def stream(self, i):
        blocks, size = self.streams[i]
        return self._read(blocks, size)


def load_symbols(pdb_path):
    msf = MSF(pdb_path)
    dbi = msf.stream(3)
    (_sig, _ver, _age, _gs, _bld, _ps, _dllver, symrec, _rbld,
     modinfo_sz, seccontrib_sz, secmap_sz, srcinfo_sz, tsmap_sz,
     _mfc, dbghdr_sz, ec_sz, _flags, _machine, _pad) = struct.unpack_from(
        "<iIIHHHHHHiiiiiIiiHHI", dbi, 0)

    # The optional debug header is the LAST substream; entry 5 is the
    # section-headers stream, which maps a symbol's segment to an RVA.
    off = 64 + modinfo_sz + seccontrib_sz + secmap_sz + srcinfo_sz + tsmap_sz + ec_sz
    dbghdr = dbi[off:off + dbghdr_sz]
    sec_stream = struct.unpack_from("<H", dbghdr, 5 * 2)[0]
    sections = []
    if sec_stream != 0xFFFF:
        raw = msf.stream(sec_stream)
        for i in range(0, len(raw) - 39, 40):
            va, = struct.unpack_from("<I", raw, i + 12)
            sections.append(va)

    syms = []   # (rva, name, size)  size 0 = unknown extent

    rec = msf.stream(symrec)
    p = 0
    while p + 4 <= len(rec):
        reclen, rectype = struct.unpack_from("<HH", rec, p)
        if reclen < 2:
            break
        body = p + 4
        if rectype == S_PUB32:
            _flags, offset, seg = struct.unpack_from("<IIH", rec, body)
            nm = rec[body + 10:p + 2 + reclen].split(b"\0")[0].decode("utf-8", "replace")
            if 0 < seg <= len(sections):
                syms.append((sections[seg - 1] + offset, nm, 0))
        p += reclen + 2

    # Public symbols omit file-static functions, so an address inside one
    # resolves to whatever public symbol precedes it (with a bogus offset).
    # The per-module streams carry S_LPROC32/S_GPROC32 with exact extents.
    off = 64
    mods = dbi[off:off + modinfo_sz]
    q = 0
    while q + 64 <= len(mods):
        modstream, = struct.unpack_from("<H", mods, q + 34)
        end = mods.find(b"\0", q + 64)
        end2 = mods.find(b"\0", end + 1)
        q = (end2 + 1 + 3) & ~3
        if modstream == 0xFFFF or modstream >= len(msf.streams):
            continue
        ms = msf.stream(modstream)
        r = 4
        while r + 4 <= len(ms):
            rl, rt = struct.unpack_from("<HH", ms, r)
            if rl < 2:
                break
            b = r + 4
            if rt in (S_LPROC32, S_GPROC32) and b + 35 <= len(ms):
                codesize, = struct.unpack_from("<I", ms, b + 12)
                poff, pseg = struct.unpack_from("<IH", ms, b + 28)
                nm = ms[b + 35:r + 2 + rl].split(b"\0")[0].decode("utf-8", "replace")
                if 0 < pseg <= len(sections):
                    syms.append((sections[pseg - 1] + poff, nm, codesize))
            r += rl + 2

    # Prefer sized (procedure) records where both describe the same address.
    best = {}
    for rva, nm, sz in syms:
        cur = best.get(rva)
        if cur is None or (sz and not cur[1]):
            best[rva] = (nm, sz)
    syms = sorted((rva, nm, sz) for rva, (nm, sz) in best.items())
    return syms, sections


if __name__ == "__main__":
    args = sys.argv[1:]
    pdb = "out/Release/mk64x.pdb"
    if args and args[0] == "--pdb":
        pdb = args[1]; args = args[2:]
    if not args:
        raise SystemExit(__doc__)

    syms, sections = load_symbols(pdb)
    if not syms:
        raise SystemExit("no public symbols found in " + pdb)
    keys = [s[0] for s in syms]
    print("%s: %d symbols, RVA %#x..%#x" % (pdb, len(syms), keys[0], keys[-1]))

    # xbWatson prints a virtual address and both the XBE and the linked EXE are
    # based at 0x10000, so the RVA is simply EIP - 0x10000.
    for a in [int(x, 0) for x in args]:
        rva = a - 0x10000
        i = bisect.bisect_right(keys, rva) - 1
        if i < 0:
            print("%#010x -> below the first symbol" % a)
            continue
        base, name, size = syms[i]
        delta = rva - base
        if size and delta >= size:
            print("%#010x -> after %s (ends +%#x); no symbol covers it" % (a, name, size))
        else:
            extent = (" of %#x" % size) if size else ""
            print("%#010x -> %s + %#x%s" % (a, name, delta, extent))

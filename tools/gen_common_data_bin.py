#!/usr/bin/env python3
"""Rebuild dc_data/common_data.bin from the ROM, not from the compiled object.

    python tools/gen_common_data_bin.py

gen_dcdata builds every segment blob by objcopying the corresponding object's
.data. That is correct for segments the game byteswaps at boot -- but WRONG for
this one, and the difference is not cosmetic.

main.c's tlut_ptr loop byteswaps ~57 common_data u16 arrays IN PLACE at boot,
so the compiled arrays hold texel VALUES and objcopy yields value-order bytes.
The shell draw lists, though, do not read those arrays: they load their palette
through a SEGMENT address (0x0D004E38 green, 0x0D005038 blue) straight out of
this file, and expect the ROM's big-endian byte stream. Feed them value order
and the alpha bit lands in the wrong half of each entry -- shells render as
rainbow noise with alpha 0, along with anything else reading the segment.

The ROM is the authority: the MIO0 block at 0x132B50 decompresses to exactly
the 184,664 bytes this file should contain, and matches the known-good copy
byte for byte (md5 ac6a21feeffc, 0x4E38 = `00 00 03 07`).

Must run AFTER gen_dcdata, which would otherwise overwrite it.
"""
import io, os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = "baserom.us.z64"
MIO0_OFFSET = 0x132B50
DST = "dc_data/common_data.bin"
EXPECT = 184664


def main():
    os.chdir(ROOT)
    mio0 = os.path.abspath("tools/mio0" + (".exe" if os.name == "nt" else ""))
    if not os.path.exists(mio0):
        print("gen_common_data_bin: tools/mio0 not built", file=sys.stderr)
        return 1
    if not os.path.exists(ROM):
        print("gen_common_data_bin: %s not found" % ROM, file=sys.stderr)
        return 1

    rom = io.open(ROM, "rb").read()
    if rom[MIO0_OFFSET:MIO0_OFFSET + 4] != b"MIO0":
        print("gen_common_data_bin: no MIO0 header at 0x%X -- wrong ROM?"
              % MIO0_OFFSET, file=sys.stderr)
        return 1
    usize = struct.unpack(">I", rom[MIO0_OFFSET + 4:MIO0_OFFSET + 8])[0]
    if usize != EXPECT:
        print("gen_common_data_bin: block at 0x%X decompresses to %d, expected %d"
              % (MIO0_OFFSET, usize, EXPECT), file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp(prefix="cdseg")
    src = os.path.join(tmp, "common_data.mio0")
    out = os.path.join(tmp, "common_data.bin")
    # The header does not carry the compressed length; hand the decompressor a
    # generous slice and let it stop at usize.
    io.open(src, "wb").write(rom[MIO0_OFFSET:MIO0_OFFSET + 0x80000])
    r = subprocess.run([mio0, "-d", src, out], capture_output=True)
    if r.returncode != 0 or not os.path.exists(out):
        print("gen_common_data_bin: decompression failed: %s"
              % r.stderr.decode("utf-8", "replace")[-300:], file=sys.stderr)
        return 1

    blob = io.open(out, "rb").read()
    if len(blob) != EXPECT:
        print("gen_common_data_bin: got %d bytes, expected %d"
              % (len(blob), EXPECT), file=sys.stderr)
        return 1
    # Cheap sanity check on the very thing this exists to protect: the green
    # shell TLUT must be in ROM byte order.
    if blob[0x4E38:0x4E3C] != b"\x00\x00\x03\x07":
        print("gen_common_data_bin: shell TLUT at 0x4E38 is %s, expected "
              "00 00 03 07 -- byte order is wrong"
              % blob[0x4E38:0x4E3C].hex(" "), file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    io.open(DST, "wb").write(blob)
    print("gen_common_data_bin: wrote %s (%s bytes, ROM 0x%X)"
          % (DST, format(len(blob), ","), MIO0_OFFSET))
    return 0


if __name__ == "__main__":
    sys.exit(main())

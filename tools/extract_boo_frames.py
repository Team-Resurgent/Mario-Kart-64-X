#!/usr/bin/env python3
"""Rebuild assets/courses/banshee_boardwalk/boo_frames.mio0 from the ROM.

gTextureGhosts (Banshee Boardwalk's Boos) is .incbin'd from this blob, but the
asset generator could not produce it -- the Boos ship as 29 separate PNGs, and
there is no rule that packs them back into one MIO0 stream. The generator then
emitted the label with NO data, so gTextureGhosts silently aliased the texture
that follows it (gTextureExhaust0) and dma_textures MIO0-decoded the wrong
bytes: Banshee's ghosts rendered as garbage.

The ROM's own compressed block is the ground truth: all 29 gTextureBoo*
entries in assets/courses/banshee_boardwalk.json share rom_offset 0x712DC0
with block_offsets 0x000..0xD200 step 0x780 (48*40 ci8), i.e. that one block
IS the frame set (29 * 0x780 = 0xD980, the size dma_textures asks for).
"""
import io, os, struct

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

ROM_OFFSET = 0x712DC0
DST = "assets/courses/banshee_boardwalk/boo_frames.mio0"
EXPECT_DECOMPRESSED = 0xD980


def mio0_len(d):
    """Exact byte length of the MIO0 stream at d[0], by walking its controls."""
    us, co, ro = struct.unpack(">III", d[4:16])
    n = 0; lb = 16; lp = 0x10; ct = 0; cp = co; rp = ro
    while n < us:
        if lb == 16:
            ct = struct.unpack(">H", d[lp:lp + 2])[0]; lp += 2; lb = 0
        if ct & 0x8000:
            rp += 1; n += 1
        else:
            v = struct.unpack(">H", d[cp:cp + 2])[0]; cp += 2; n += (v >> 12) + 3
        ct = (ct << 1) & 0xFFFF; lb += 1
    return max(lp, cp, rp)


rom = io.open("baserom.us.z64", "rb").read()
d = rom[ROM_OFFSET:ROM_OFFSET + 0x200000]
if d[:4] != b"MIO0":
    raise SystemExit("no MIO0 block at %#x" % ROM_OFFSET)

decompressed = struct.unpack(">I", d[4:8])[0]
if decompressed != EXPECT_DECOMPRESSED:
    raise SystemExit("block decompresses to %#x, expected %#x"
                     % (decompressed, EXPECT_DECOMPRESSED))

blob = d[:mio0_len(d)]
io.open(DST, "wb").write(blob)
print("%s: %d bytes compressed -> %#x decompressed (%d frames of 48x40 ci8)"
      % (DST, len(blob), decompressed, decompressed // 0x780))

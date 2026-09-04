#!/usr/bin/env python3
"""Generate dc_data/saveimage.xbx -- the dashboard thumbnail for MK64 saves.

An .xbx is an XPR0 package: a 2048-byte header holding an Xbox D3D texture
descriptor, followed by the texture data. RXDK's own sample image decodes as
64x64 DXT1, one mip level (format word 0x06610c2a: dimension 2, format 0x0C =
DXT1, USIZE/VSIZE 6 -> 2^6 = 64), giving 64*64/2 = 2048 bytes of payload for
4096 bytes total. The header is generic, so it is copied verbatim from the
sample and only the payload is replaced.

The artwork is drawn here rather than lifted from the game: a chequered racing
flag with a red roundel and a blocky "64". Original, and it reads clearly at
64px where anything detailed would turn to mush.
"""
import io, os, struct

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

TEMPLATE = r"C:\ProgramData\RXDK\samples\RxdkSamples\Common\rxdk-saveimage.xbx"
DST = "dc_data/saveimage.xbx"
N = 64

# ---------------------------------------------------------------- artwork ---
BLACK = (0x14, 0x14, 0x18)
WHITE = (0xEE, 0xEE, 0xEA)
RED = (0xC8, 0x20, 0x28)
DARK = (0x60, 0x0C, 0x14)

px = [[BLACK] * N for _ in range(N)]

# Chequered flag, 8px squares.
for y in range(N):
    for x in range(N):
        px[y][x] = WHITE if ((x >> 3) + (y >> 3)) & 1 else BLACK

# Red roundel, with a darker rim so it stays distinct against white squares.
cx = cy = 31.5
for y in range(N):
    for x in range(N):
        d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
        if d <= 21.0:
            px[y][x] = RED
        elif d <= 23.0:
            px[y][x] = DARK

# "64" in a 5x7 blocky face, scaled 3x.
GLYPH = {
    "6": ["01110", "10000", "10000", "11110", "10001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
}
SCALE = 3
text = "64"
gw, gh = 5 * SCALE, 7 * SCALE
total_w = len(text) * gw + (len(text) - 1) * SCALE
ox = int(cx - total_w / 2) + 1
oy = int(cy - gh / 2) + 1
for gi, ch in enumerate(text):
    gx = ox + gi * (gw + SCALE)
    for ry, row in enumerate(GLYPH[ch]):
        for rx, bit in enumerate(row):
            if bit != "1":
                continue
            for sy in range(SCALE):
                for sx in range(SCALE):
                    X, Y = gx + rx * SCALE + sx, oy + ry * SCALE + sy
                    if 0 <= X < N and 0 <= Y < N:
                        px[Y][X] = WHITE

# ------------------------------------------------------------ DXT1 encode ---
def rgb565(c):
    return ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)


def unpack565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 3))


def encode_block(block):
    """block: 16 RGB tuples in row-major 4x4 order."""
    # Endpoints: the pair furthest apart, which is exact for the flat-colour
    # art here and near-exact for the roundel's edge blocks.
    best = (0, 0, -1)
    for i in range(16):
        for j in range(i + 1, 16):
            a, b = block[i], block[j]
            d = (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2
            if d > best[2]:
                best = (i, j, d)
    c0v, c1v = rgb565(block[best[0]]), rgb565(block[best[1]])
    if c0v == c1v:
        # Uniform block: c0 > c1 is required for 4-colour mode; index 0 = c0.
        return struct.pack("<HHI", c0v, c1v, 0)
    if c0v < c1v:
        c0v, c1v = c1v, c0v
    c0, c1 = unpack565(c0v), unpack565(c1v)
    palette = [
        c0,
        c1,
        tuple((2 * c0[k] + c1[k]) // 3 for k in range(3)),
        tuple((c0[k] + 2 * c1[k]) // 3 for k in range(3)),
    ]
    bits = 0
    for n, p in enumerate(block):
        bi, bd = 0, None
        for idx, q in enumerate(palette):
            d = (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2
            if bd is None or d < bd:
                bi, bd = idx, d
        bits |= bi << (2 * n)
    return struct.pack("<HHI", c0v, c1v, bits)


payload = bytearray()
for by in range(0, N, 4):
    for bx in range(0, N, 4):
        blk = [px[by + y][bx + x] for y in range(4) for x in range(4)]
        payload += encode_block(blk)

# ------------------------------------------------------------------ write ---
tpl = io.open(TEMPLATE, "rb").read()
total, hdr = struct.unpack_from("<II", tpl, 4)
assert len(payload) == total - hdr, "payload %d != expected %d" % (len(payload), total - hdr)

os.makedirs(os.path.dirname(DST), exist_ok=True)
io.open(DST, "wb").write(tpl[:hdr] + bytes(payload))
print("%s: %d bytes (%d header + %d DXT1, 64x64)" % (DST, hdr + len(payload), hdr, len(payload)))

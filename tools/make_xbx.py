#!/usr/bin/env python3
"""Convert source artwork into the dashboard's .xbx images.

    python tools/make_xbx.py                      # rebuild both from the defaults
    python tools/make_xbx.py in.png out.xbx 128   # one-off

An .xbx is an XPR0 package: a 2048-byte header holding an Xbox D3D texture
descriptor, then the texture. The two images are NOT the same size, which is
easy to get wrong -- every sample in RXDK agrees:

    saveimage.xbx    64x64   DXT1   4096 bytes total
    titleimage.xbx   128x128 DXT1  10240 bytes total

so the header is taken from whichever sample matches the target size and only
the payload is replaced. That keeps the descriptor (dimensions, format, mip
count) correct by construction instead of hand-packing the format word.

Quality notes, because these are tiny and mistakes are very visible:
  * DXT1 compression is done by Pillow, not by hand. A naive encoder that picks
    the two furthest-apart colours per block loses noticeably more detail.
  * Downscaling a large source to 64px with a pure box filter looks soft, so a
    mild unsharp mask goes on afterwards. Overdoing it produces crunchy edge
    halos that DXT1 then smears, hence the restrained amount.
  * DXT1 here carries no usable alpha, so transparency is composited onto a
    solid background first. Left alone, transparent pixels decode as whatever
    happens to be in the colour channels.
"""
import io, os, struct, sys
from PIL import Image, ImageFilter

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

SAMPLES = r"C:\ProgramData\RXDK\samples\RxdkSamples\Common"
TEMPLATE = {64:  os.path.join(SAMPLES, "rxdk-saveimage.xbx"),
            128: os.path.join(SAMPLES, "rxdk-titleimage.xbx")}


def dxt1(im):
    """Compress an RGB image to raw DXT1 via Pillow's DDS writer."""
    buf = io.BytesIO()
    im.save(buf, format="DDS", pixel_format="DXT1")
    d = buf.getvalue()
    expect = im.width * im.height // 2
    return d[len(d) - expect:]          # strip the 128-byte DDS header


# Xbox texture format codes (d3d8types.h) and the linear-size field (d3d8.h).
D3DFMT_DXT1           = 0x0C
D3DFMT_LIN_A8R8G8B8   = 0x12
D3DSIZE_HEIGHT_SHIFT  = 12
D3DSIZE_PITCH_SHIFT   = 24


def build_xpr_argb(im):
    """Uncompressed 32-bit XPR: header, format word and size field built here.

    DXT1 gets two endpoint colours per 4x4 block, which is what produces the
    blockiness on detailed art. D3DFMT_LIN_A8R8G8B8 stores every pixel exactly,
    at 4 bytes each instead of half a byte -- 64KB for 128x128 rather than 8KB,
    which is nothing for an icon.

    LINEAR is chosen over the swizzled format deliberately: swizzled textures
    need pixels reordered into Morton order, and getting that interleave
    backwards transposes the image. A linear texture is stored row by row, and
    instead carries its geometry in the descriptor's Size field --
    width-1, height-1 and pitch/64-1, packed per d3d8.h.

    Alpha is preserved rather than flattened. If the dashboard honours it, the
    artwork's own transparency shows through its circular mask; if it ignores
    it, the RGB underneath has already been composited onto the background, so
    the result is no worse than the DXT1 version.
    """
    w, h = im.size
    tpl = io.open(TEMPLATE[128], "rb").read()      # any template: header is generic
    total_old, hdr = struct.unpack_from("<II", tpl, 4)
    head = bytearray(tpl[:hdr])

    fmt = struct.unpack_from("<I", head, 24)[0]
    fmt &= ~0x0000FF00                              # clear format field
    fmt |= D3DFMT_LIN_A8R8G8B8 << 8
    fmt &= ~0x0FF00000                              # linear: log2 U/V unused
    struct.pack_into("<I", head, 24, fmt)

    pitch = w * 4
    size = ((w - 1) |
            ((h - 1) << D3DSIZE_HEIGHT_SHIFT) |
            ((pitch // 64 - 1) << D3DSIZE_PITCH_SHIFT))
    struct.pack_into("<I", head, 28, size)

    payload = im.convert("RGBA").tobytes("raw", "BGRA")   # A8R8G8B8 in memory
    struct.pack_into("<I", head, 4, hdr + len(payload))
    return bytes(head) + payload


def convert(src, dst, size, bg=(0, 0, 0), crop=None, sharpen=True, trim=False,
            fit=False, argb=False):
    im = Image.open(src)
    if crop:
        # Crops are FRACTIONS of the image (0..1), not pixels, so they keep
        # meaning when the source art is re-exported at a different size --
        # a pixel crop silently goes out of bounds the moment that happens.
        w, h = im.size
        im = im.crop((int(crop[0] * w), int(crop[1] * h),
                      int(crop[2] * w), int(crop[3] * h)))
    im = im.convert("RGBA")

    if trim:
        # Drop fully transparent margin before scaling. Sources here carry a lot
        # of it -- the roundel ~16%, the logo far more -- and at 64-128px every
        # wasted row costs real detail.
        bb = im.split()[-1].getbbox()
        if bb:
            if fit:
                # Keep the artwork's own aspect: crop tight, scale to fit the
                # square, and letterbox. Squashing a 2:1 logo into 1:1 would
                # distort the lettering, which is the whole point of using it.
                im = im.crop(bb)
            else:
                # Square the crop about the centre so a circular source does
                # not come out as an oval.
                cx, cy = (bb[0] + bb[2]) / 2.0, (bb[1] + bb[3]) / 2.0
                half = max(bb[2] - bb[0], bb[3] - bb[1]) / 2.0
                im = im.crop((int(cx - half), int(cy - half),
                              int(cx + half), int(cy + half)))

    # Composite onto the background but KEEP the original alpha alongside it.
    # DXT1 discards alpha anyway; the uncompressed path can carry it, and if
    # the dashboard honours it the artwork masks cleanly instead of showing a
    # background it never asked for.
    alpha = im.split()[-1]
    flat = Image.new("RGBA", im.size, bg + (255,))
    flat.alpha_composite(im)
    im = flat.convert("RGB")
    im.putalpha(alpha)

    if fit:
        # Scale to fit inside the square, preserving aspect, and centre it.
        w, h = im.size
        s = min(size / float(w), size / float(h))
        im = im.resize((max(1, int(round(w * s))), max(1, int(round(h * s)))),
                       Image.LANCZOS)
        canvas = Image.new("RGB", (size, size), bg)
        canvas.paste(im, ((size - im.width) // 2, (size - im.height) // 2))
        im = canvas
    else:
        # Downscale in one LANCZOS step.
        im = im.resize((size, size), Image.LANCZOS)
    if sharpen:
        # NOT an unsharp mask. Sharpening before DXT1 is counterproductive: it
        # manufactures high-frequency detail, and DXT1 only has two endpoint
        # colours per 4x4 block to spend, so the extra edges come back as block
        # artefacts. Measured on the box art, round-trip fidelity went
        # 18.7 dB sharpened / 21.2 dB untouched / 22.2 dB very slightly
        # softened. A touch of blur gives the compressor less to fight and
        # reads cleaner at icon size.
        im = im.filter(ImageFilter.GaussianBlur(0.4))

    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if argb:
        blob = build_xpr_argb(im)
        kind = "A8R8G8B8"
    else:
        payload = dxt1(im.convert("RGB"))
        tpl = io.open(TEMPLATE[size], "rb").read()
        total, hdr = struct.unpack_from("<II", tpl, 4)
        assert len(payload) == total - hdr, \
            "%dx%d payload %d != template body %d" % (size, size, len(payload), total - hdr)
        blob = tpl[:hdr] + payload
        kind = "DXT1"
    io.open(dst, "wb").write(blob)
    print("%-26s <- %-20s %3dx%-3d %-8s %d bytes" %
          (dst, os.path.basename(src), size, size, kind, len(blob)))


if __name__ == "__main__":
    if len(sys.argv) >= 4:
        convert(sys.argv[1], sys.argv[2], int(sys.argv[3]))
    else:
        # The roundel is transparent outside the ring; black keeps it reading as
        # a disc rather than bleeding into whatever the dashboard paints behind.
        # THE DASHBOARD MASKS EVERY ICON INTO A CIRCLE. That single fact drives
        # both choices here, and it is why the logo-on-black version looked so
        # much worse than other games' entries: the mask kept the black and
        # threw away the corners, leaving a small logo adrift in a dark disc.
        # Art that bleeds to all four edges is what fills the circle -- which
        # is exactly what Mega Man X and the rest are doing.
        #
        # So: full-bleed box art, no padding, no letterboxing, nothing that
        # introduces background the mask will put on show.
        #
        # The save image gets a tighter crop rather than the same picture. At
        # 64px the full scene -- five karts, motion blur, the logo -- collapses
        # into mush, while Mario's head alone stays instantly readable at that
        # size. Same source, framed for the size it will actually be seen at.
        # DXT1, not the uncompressed path. argb=True was tried on hardware and
        # rendered BLACK: this dashboard assumes DXT1 rather than reading the
        # format out of the descriptor, so a valid A8R8G8B8 XPR is no use here.
        # Keep argb= for reference, but do not ship it.
        #
        # Sizes are the convention every RXDK sample follows -- save 64, title
        # 128 -- and deviating is what the black icons taught us not to do. The
        # title is 1:1 with its 128x128 source (no resampling); the save image
        # is the only place a downscale happens, and it is unavoidable.
        convert("MK64Kart128x128.png", "dc_data/saveimage.xbx", 64,
                sharpen=False)
        convert("MK64128x128.png", "dc_data/titleimage.xbx", 128,
                sharpen=False)

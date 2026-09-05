#!/usr/bin/env python3
"""Prepare the dashboard icon artwork: source PNG -> sized 24-bit BMP.

    python tools/make_xbx.py                      # regenerate both from the defaults
    python tools/make_xbx.py in.png out.bmp 128   # one-off

This is an ART tool, not a build step. It runs only when the source art
changes, and its output -- Platform/xbox/titleimage.bmp (128x128) and
Platform/xbox/saveimage.bmp (64x64) -- is committed.

The conversion into the dashboard's XPR0/DXT1 .xbx is NOT done here: RXDK
already ships the tool for that. Platform/xbox/titleimage.rdf and
saveimage.rdf describe each icon to the bundler (Format D3DFMT_DXT1, one
level, out_version XPR0), the build engine runs the bundler on them before
linking, and imagebld injects the result into the XBE. The bundler's DXT1
encoder is the XDK's own, byte-exact, so a hand-written compressor and a
hand-packed header here would only be a worse copy of it. Needing Pillow is
the reason this stays out of setup.py: a build should not require an imaging
library for two icons whose source can simply be committed at final size.

Why BMP: the bundler reads BMP and TGA. PNG is accepted by extension but the
loader does not decode it, so the source it is handed has to be one of those.

Quality notes, because these are tiny and mistakes are very visible:
  * Downscaling a large source to 64px with a pure box filter looks soft.
    A LANCZOS resample handles the size change in one step.
  * Sharpening before DXT1 is counterproductive: it manufactures
    high-frequency detail that a two-endpoint block format smears into
    artefacts. Measured on the box art, round-trip fidelity went 18.7 dB
    sharpened / 21.2 dB untouched / 22.2 dB very slightly softened, so the
    default is a touch of blur, not an unsharp mask.
  * DXT1 carries no usable alpha, so transparency is composited onto a solid
    background first. Left alone, transparent pixels decode as whatever
    happens to be in the colour channels.
"""
import os, sys
from PIL import Image, ImageFilter

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def convert(src, dst, size, bg=(0, 0, 0), crop=None, sharpen=True, trim=False,
            fit=False):
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

    # Composite onto the background: DXT1 discards alpha, and a BMP has none.
    flat = Image.new("RGBA", im.size, bg + (255,))
    flat.alpha_composite(im)
    im = flat.convert("RGB")

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
        # NOT an unsharp mask -- see the module docstring for the numbers.
        im = im.filter(ImageFilter.GaussianBlur(0.4))

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    im.save(dst, "BMP")
    print("%-32s <- %-20s %3dx%-3d %d bytes" %
          (dst, os.path.basename(src), size, size, os.path.getsize(dst)))


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
        #
        # DXT1 is what the .rdf files ask the bundler for. An uncompressed
        # A8R8G8B8 XPR was tried on hardware and rendered BLACK: this dashboard
        # assumes DXT1 rather than reading the format out of the descriptor.
        #
        # Sizes are the convention every RXDK sample follows -- save 64, title
        # 128 -- and deviating is what the black icons taught us not to do. The
        # title is 1:1 with its 128x128 source (no resampling); the save image
        # is the only place a downscale happens, and it is unavoidable.
        convert("MK64Kart128x128.png", "Platform/xbox/saveimage.bmp", 64,
                sharpen=False)
        convert("MK64128x128.png", "Platform/xbox/titleimage.bmp", 128,
                sharpen=False)

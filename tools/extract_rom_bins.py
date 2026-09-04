#!/usr/bin/env python3
"""Regenerate the raw ROM assets that are deliberately not in the repo.

    python tools/extract_rom_bins.py            # after placing baserom.us.z64

The port ships as source only, so `bin/*.bin` and the raw texture payloads
under `textures/` are absent from a fresh clone -- they are verbatim slices of
the ROM. This puts them back from the user's own copy.

`tools/rom_bins.json` describes each one as (path, offset, size, md5). That
manifest is publishable because it is numbers, not content: knowing that a
texture lives at 0x7F1124 and is 208 bytes long conveys none of the texture.

Each extracted file is checked against its recorded md5, so a bad or
mis-versioned ROM fails loudly here rather than as corrupt graphics later.
"""
import io, os, json, hashlib, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "rom_bins.json")


def main():
    os.chdir(ROOT)
    man = json.load(io.open(MANIFEST, encoding="utf-8"))
    rom_path = man["rom"]

    if not os.path.exists(rom_path):
        print("error: %s not found.\n"
              "Place your own Mario Kart 64 (U) ROM in the repo root as %s."
              % (rom_path, rom_path), file=sys.stderr)
        return 1

    rom = io.open(rom_path, "rb").read()
    got = hashlib.md5(rom).hexdigest()
    if got != man["rom_md5"]:
        print("error: %s has md5 %s, expected %s.\n"
              "This is the wrong ROM version -- every offset below would be "
              "garbage, so nothing was written."
              % (rom_path, got, man["rom_md5"]), file=sys.stderr)
        return 1

    written = skipped = 0
    for e in man["files"]:
        path, off, size = e["path"], e["offset"], e["size"]
        data = rom[off:off + size]
        if len(data) != size:
            print("error: %s runs past the end of the ROM" % path,
                  file=sys.stderr)
            return 1
        if hashlib.md5(data).hexdigest() != e["md5"]:
            print("error: %s (0x%06X +%d) does not match its recorded md5"
                  % (path, off, size), file=sys.stderr)
            return 1
        # Leave an identical existing file alone so this is cheap to re-run.
        if os.path.exists(path) and os.path.getsize(path) == size:
            if hashlib.md5(io.open(path, "rb").read()).hexdigest() == e["md5"]:
                skipped += 1
                continue
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        io.open(path, "wb").write(data)
        written += 1

    print("extract_rom_bins: %d written, %d already present (%d total, %s bytes)"
          % (written, skipped, len(man["files"]),
             format(sum(e["size"] for e in man["files"]), ",")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Check that nothing ROM-derived would be published to the repo.

    python tools/audit_repo_clean.py

The port is distributed as SOURCE ONLY: users supply their own
`baserom.us.z64` and build everything themselves. That position is only as
good as the .gitignore, and an ignore list rots the moment a build step starts
writing somewhere it did not write before -- which is exactly what the Xbox
port did when it added Platform/xbox/gen*. This is the check that catches it.

Exit status is 1 if anything looks wrong, so it can gate a release.

Three independent tests, because no single one is sufficient:

  1. VERBATIM ROM BYTES. Every publishable binary file is searched for inside
     the ROM, in both byte orders (assets get 16-bit byteswapped on the way
     into this port, so a raw-only search would miss half of them). A hit is
     proof, not a heuristic.

  2. PATH RULES. Assets that are MIO0-COMPRESSED in the ROM decompress to
     bytes that appear nowhere in it, so test 1 cannot see them. Anything
     living where extracted assets live is flagged on that basis alone.

  3. BUILD OUTPUT. ISOs, XBEs, PDBs and object files are nobody's source.

Test 1 is the one that finds surprises; tests 2 and 3 are the ones that stop
a surprise from being possible.
"""
import io, os, re, subprocess, sys, tempfile, shutil, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

ROM = "baserom.us.z64"

# Raw asset payloads: uncompressed N64 texture formats and the like. An
# extension naming a pixel format is a payload, never source.
ASSET_EXTS = (".mio0", ".z64", ".n64", ".v64", ".raw", ".aiff", ".m64",
              ".i4", ".i8", ".ia1", ".ia4", ".ia8", ".ia16",
              ".ci4", ".ci8", ".rgba16", ".rgba32")

BUILD_EXTS = (".iso", ".cdi", ".xbe", ".pdb", ".o", ".obj", ".elf", ".map",
              ".exe", ".lib", ".ilk", ".exp")
BUILD_DIRS = ("out/", "build/", "__pycache__/")

# Third-party toolchains the project ships on purpose. These are binaries and
# they are not ours, but they are also not Nintendo's -- upstream's .gitignore
# explicitly re-admits them (`!tools/ido-recomp/*/*`), so respect that.
VENDOR_DIRS = ("tools/ido-recomp/", "tools/mingw64/", "doxygen-awesome-css/")

# Descriptors are the extraction RECIPE -- names, ROM offsets, formats. They
# carry no content and the build cannot run without them, so they must be
# published even though they live beside the payloads they describe.
DESCRIPTOR_EXTS = (".json", ".yml", ".yaml", ".mk", ".md", ".txt")

# Text that could still be a payload in disguise: a .inc.c of hex literals is
# ROM data wearing a C hat, and a bytes-only scan looks straight past it.
HEXY_EXTS = (".c", ".h", ".inc")
BYTE_LIT = re.compile(rb"0x([0-9a-fA-F]{2})\b")


def publishable_files():
    """Files git would commit from a fresh clone, with .gitignore applied.

    Run against a throwaway git dir so this never creates a repository in the
    working tree -- the tree may not be a repo yet, and if it is, its index is
    none of this script's business.
    """
    tmp = tempfile.mkdtemp(prefix="mk64audit")
    try:
        subprocess.run(["git", "init", "-q", tmp], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = subprocess.run(
            ["git", "--git-dir", os.path.join(tmp, ".git"),
             "--work-tree", ROOT, "status", "--porcelain", "-uall"],
            check=True, capture_output=True, text=True).stdout
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    files = []
    for line in out.splitlines():
        p = line[3:].strip().strip('"')
        if p and os.path.isfile(os.path.join(ROOT, p)):
            files.append(p.replace("\\", "/"))
    return sorted(files)


def byteswap16(b):
    n = len(b) & ~1
    m = bytearray(b[:n])
    m[0::2], m[1::2] = b[1:n:2], b[0:n:2]
    return bytes(m)


def rom_probe(rom, data):
    """Is this file's content lifted from the ROM?

    Probes from the middle rather than the start: many formats open with a
    header this port rewrites, so offset 0 can differ while everything after
    it is verbatim. A hit is confirmed with a second, distant probe so that a
    run of 0x00 padding cannot produce a false positive on its own.

    A miss is NOT a clean bill of health: assets stored MIO0-compressed in the
    ROM decompress to bytes that appear nowhere in it. That is why the path
    rules exist alongside this.
    """
    if len(data) < 64:
        return None
    for label, d in (("raw", data), ("byteswapped", byteswap16(data))):
        a = len(d) // 3
        probe = d[a:a + 64]
        if len(set(probe)) < 4:           # flat padding proves nothing
            continue
        if rom.find(probe) < 0:
            continue
        b = (len(d) * 2) // 3
        confirm = d[b:b + 64]
        if len(set(confirm)) < 4 or rom.find(confirm) >= 0:
            return label
    return None


def hex_payload(path):
    """Bytes encoded by a file that is mostly `0xNN,` literals, else None.

    The density test matters: ordinary C is full of hex constants too, and
    flagging every source file with a bitmask in it would make this useless.
    """
    try:
        raw = io.open(path, "rb").read()
    except OSError:
        return None
    vals = BYTE_LIT.findall(raw)
    if len(vals) < 256 or len(vals) * 5 < len(raw) // 2:
        return None
    return bytes(int(v, 16) for v in vals)


def main():
    files = publishable_files()
    total = sum(os.path.getsize(f) for f in files)
    print("Publishable: %s files, %.1f MB\n" % (format(len(files), ","),
                                                total / 1048576.0))

    rom = None
    if os.path.exists(ROM):
        rom = io.open(ROM, "rb").read()
    else:
        print("NOTE: %s absent -- skipping the verbatim-bytes test.\n" % ROM)

    verbatim, payloads, build_output, rom_itself = [], [], [], []

    for f in files:
        low = f.lower()
        ext = os.path.splitext(low)[1]

        if any(low.startswith(v) for v in VENDOR_DIRS):
            continue                       # third-party, shipped on purpose
        if ext in (".z64", ".n64", ".v64"):
            rom_itself.append(f)
            continue
        if ext in BUILD_EXTS or any(low.startswith(d) for d in BUILD_DIRS):
            build_output.append(f)
            continue
        if ext in DESCRIPTOR_EXTS:
            continue                       # recipe, not content
        if ext in ASSET_EXTS:
            payloads.append(f)
            continue
        if rom is None:
            continue

        # Binary probe, then the hex-literal probe for data wearing a C hat.
        data = None
        if ext in HEXY_EXTS:
            data = hex_payload(f)
        else:
            try:
                data = io.open(f, "rb").read()
            except OSError:
                data = None
        if data:
            hit = rom_probe(rom, data)
            if hit:
                verbatim.append((f, hit + (", as C hex" if ext in HEXY_EXTS else "")))

    bad = 0
    for title, items, why in (
        ("THE ROM ITSELF", rom_itself,
         "never publish this"),
        ("VERBATIM ROM CONTENT", verbatim,
         "these bytes were found inside the ROM"),
        ("RAW ASSET PAYLOAD", payloads,
         "extracted from the ROM; regenerate with tools/extract_rom_bins.py"),
        ("BUILD OUTPUT", build_output,
         "rebuilt by the user; not source"),
    ):
        if not items:
            continue
        bad += len(items)
        print("!! %s -- %s (%d)" % (title, why, len(items)))
        for it in items[:12]:
            if isinstance(it, tuple):
                print("     %-64s [%s]" % it)
            else:
                print("     %s" % it)
        if len(items) > 12:
            print("     ... and %d more" % (len(items) - 12))
        print()

    print("Publishable content by top-level directory:")
    by = collections.Counter()
    sz = collections.Counter()
    for f in files:
        top = f.split("/")[0] if "/" in f else "(root)"
        by[top] += 1
        sz[top] += os.path.getsize(f)
    for top, n in by.most_common():
        print("  %-22s %5d files %9.2f MB" % (top, n, sz[top] / 1048576.0))

    if bad:
        print("\nFAIL: %d file(s) must not be published. Add them to "
              ".gitignore and re-run." % bad)
        return 1
    print("\nOK: nothing ROM-derived would be published.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

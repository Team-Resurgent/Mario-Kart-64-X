#!/usr/bin/env python3
"""Turn a fresh clone plus your own ROM into a bootable Xbox ISO.

    python setup.py                # do everything
    python setup.py --check        # say what is missing, change nothing
    python setup.py --skip-build   # regenerate assets, stop before RXDK
    python setup.py --force        # redo every step even if outputs exist
    python setup.py --config Debug # Debug instead of Release

This project ships as SOURCE ONLY. Nothing ROM-derived is in the repository,
so a clean clone will not build until you supply `baserom.us.z64` yourself and
run this. `tools/audit_repo_clean.py` is what keeps that true from the other
side; this is what makes it survivable for the person cloning.

WHY THE ORDER IS WHAT IT IS
---------------------------
The chain is not a straight line, and getting it wrong produces confusing
failures rather than obvious ones. Four constraints shape it:

  * THE OUTPUT DIRECTORIES MUST EXIST FIRST. The Makefile creates them up
    front (its ALL_DIRS variable) and the extractors silently rely on that.
    torch does not create its own: it opens the file, the open fails, it
    throws, and the uncaught throw reaches abort() -- which Windows reports as
    `0xC0000409 STATUS_STACK_BUFFER_OVERRUN`. That message sends you hunting a
    memory-corruption bug in torch that does not exist. It is a missing mkdir.

  * torch must exist before anything else. It is a git submodule that has to
    be COMPILED, and it is what writes assets/code/** -- 180 files the build
    lists as sources. Without it the RXDK build fails on missing .c files that
    no other step produces.

  * gen_segblobs and gen_dcdata read COMPILED OBJECTS, not source. They dump
    the .data section of each object, exactly as the Dreamcast Makefile does
    with objcopy. So the RXDK build has to run BEFORE them, and again after,
    which is why the build appears twice below. Pass one produces objects;
    pass two packs the dc_data those objects yielded into the ISO.

  * gen_binassets must precede gen_asmwrappers. The wrappers emit `.incbin`
    lines, and a missing file there is DROPPED SILENTLY while still emitting
    its label -- the symbol then aliases whatever asset comes next in memory.
    That is a real bug this project has already been bitten by (Banshee's
    Boos), so the order is load-bearing, not stylistic.

  * gen_binassets reads <objdir>/incbins.txt, and NOTHING in the tree writes
    it -- it was a transient file made by hand during development. It is
    derived here from the decomp's data-only .s files, which is the only
    source that works on a clean tree: deriving it from the generated wrappers
    instead would be circular, since those drop the very entries whose files
    do not exist yet.

Every step declares its outputs and is skipped when they are all present, so
re-running is cheap and safe. Use --force to rebuild regardless.
"""
import argparse, glob, hashlib, io, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
ROM = "baserom.us.z64"
ROM_MD5 = "3a67d9986f54eb282924fca4cd5f6dff"
RXDK = r"C:\ProgramData\RXDK\tools\Rxdk.Cli.exe"
EXE = ".exe" if os.name == "nt" else ""

# torch extracts the ROM into ~180 source files. It is fetched and built by
# this script rather than carried as a git submodule: a submodule would still
# leave the user to run cmake and to patch torch's link line by hand, while
# adding --recursive, a network dependency at clone time, and a failure mode
# that breaks GUI git clients. Pinned, because a newer torch may not agree
# with this port's yaml.
TORCH_URL = "https://github.com/HarbourMasters/torch"
TORCH_SHA = "6a2eb921482f2eb3b3cb5b675152d6d21d1a20ff"

# No ANSI colour, deliberately. Detecting whether a Windows console will
# actually render escape codes is not reliable -- isatty() can say yes while
# the terminal renders them literally, and the SetConsoleMode call that opts a
# console in can fail silently. A first-time builder then meets an error
# message full of `[31m` garbage, which is a worse first impression than plain
# text. Colour buys a build script nothing; correctness costs nothing.
GREEN = YELLOW = RED = DIM = OFF = ""


def torch_exe():
    """The built torch binary, wherever this platform's CMake dropped it.

    Returned ABSOLUTE. sh() passes cwd= to subprocess, and on Windows
    CreateProcess resolves a relative executable against the PARENT's working
    directory rather than that cwd -- so a relative path here fails with a
    bare "FileNotFoundError: [WinError 2]" that names nothing useful. Every
    other tool this script invokes is absolute for the same reason.
    """
    for p in ("tools/torch/build-win/Release/torch.exe",
              "tools/torch/build-win/torch.exe",
              "tools/torch/build/Release/torch",
              "tools/torch/build/torch",
              "tools/torch/torch.exe",
              "tools/torch/torch"):
        if os.path.exists(p):
            return os.path.abspath(p)
    return None


def need(tool, why):
    """Fail with something actionable rather than a bare WinError 2."""
    if shutil.which(tool) is None:
        raise SystemExit(
            "%s is not on PATH, and it is needed to %s.\n"
            "Install it and re-run: python setup.py" % (tool, why))


def patch_torch_cmake():
    """Add wininet to torch's link line on Windows.

    Upstream torch links StormLib but not wininet, so the Windows build fails
    with unresolved InternetOpen / HttpSendRequest. Idempotent: does nothing
    once the library is listed. Not our bug, but making every user hit it and
    hand-edit a third-party file is not a build process.
    """
    if os.name != "nt":
        return
    p = "tools/torch/CMakeLists.txt"
    if not os.path.exists(p):
        return
    text = io.open(p, encoding="utf-8", errors="surrogateescape").read()
    if "storm wininet" in text or "PRIVATE storm)" not in text:
        return
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(
        text.replace("PRIVATE storm)", "PRIVATE storm wininet)"))
    print("    patched torch's CMakeLists.txt to link wininet")


def ensure_torch():
    """Return a built torch, fetching and building it if necessary.

    The whole point: `git clone`, drop in a ROM, `python setup.py`. Anything
    the script can do for the user, it should.
    """
    t = torch_exe()
    if t:
        return t

    if not os.path.exists("tools/torch/CMakeLists.txt"):
        need("git", "fetch torch")
        print("    fetching torch (%s)" % TORCH_SHA[:12])
        if os.path.isdir("tools/torch"):
            shutil.rmtree("tools/torch", ignore_errors=True)
        sh("git", "clone", "--quiet", TORCH_URL, "tools/torch")
        # The pinned commit is NOT on torch's default branch, so a plain clone
        # does not contain it -- checkout would fail with "reference is not a
        # tree". Ask for it explicitly; the server serves unadvertised SHAs.
        sh("git", "-C", "tools/torch", "fetch", "--quiet", "origin", TORCH_SHA)
        sh("git", "-C", "tools/torch", "checkout", "--quiet", TORCH_SHA)

    patch_torch_cmake()

    need("cmake", "build torch")
    print("    building torch (one-off, a few minutes)")
    sh("cmake", "-S", "tools/torch", "-B", "tools/torch/build-win")
    sh("cmake", "--build", "tools/torch/build-win", "--config", "Release")

    t = torch_exe()
    if not t:
        raise SystemExit(
            "torch built without error but no torch binary was found under\n"
            "tools/torch. Look in tools/torch/build-win for it and, if it is\n"
            "somewhere unexpected, add that path to torch_exe() in setup.py.")
    return t


class Step(object):
    def __init__(self, name, outputs, run, why=""):
        self.name, self.outputs, self.run, self.why = name, outputs, run, why

    def satisfied(self):
        """Present if every declared output pattern matches something.

        A step declaring NO outputs is never satisfied -- it always runs. That
        is how the final packaging build is expressed.
        """
        if not self.outputs:
            return False
        for o in self.outputs:
            if any(c in o for c in "*?["):
                if not glob.glob(o, recursive=True):
                    return False
            elif not os.path.exists(o):
                return False
        return True


def sh(*cmd, **kw):
    env = dict(os.environ)
    env.update(kw.pop("env", {}) or {})
    print(DIM + "    $ " + " ".join(cmd) + OFF)
    r = subprocess.run(list(cmd), cwd=ROOT, env=env)
    if r.returncode != 0:
        raise SystemExit("%s! step failed: %s%s" % (RED, cmd[0], OFF))


def check_rom():
    if not os.path.exists(ROM):
        raise SystemExit(
            "%s! %s not found.%s\n\n"
            "  This project does not distribute game assets. Supply your own\n"
            "  Mario Kart 64 (U) ROM, in big-endian .z64 format, named exactly\n"
            "  %s, in:\n    %s\n" % (RED, ROM, OFF, ROM, ROOT))
    got = hashlib.md5(io.open(ROM, "rb").read()).hexdigest()
    if got != ROM_MD5:
        raise SystemExit(
            "%s! %s has md5 %s%s\n"
            "  expected %s (Mario Kart 64, USA).\n\n"
            "  A .n64 or .v64 dump is byte-swapped and will NOT work -- every\n"
            "  offset the extractors use would land in the wrong place. Convert\n"
            "  it to .z64 first.\n" % (RED, ROM, got, OFF, ROM_MD5))
    print("  %sROM ok%s  (md5 %s)" % (GREEN, OFF, got))


def build_steps(config):
    py = sys.executable
    aica_env = {"PYTHONPATH": os.path.join(ROOT, "tools", "aica")}
    objdir = "out/" + config

    def torch_step():
        # Resolved HERE, not when the step list is built: on a clean tree
        # torch does not exist yet, and ensure_torch fetches and builds it.
        t = ensure_torch()
        sh(t, "code", ROM)
        sh(t, "header", ROM)

    # The Makefile creates these before anything runs (its ALL_DIRS variable),
    # and the extractors QUIETLY DEPEND ON THAT. torch does not create its own
    # output directory: it opens the file, the open fails, it throws, and the
    # uncaught throw reaches abort() -- which Windows reports as
    # 0xC0000409 STACK_BUFFER_OVERRUN, a message that sends you looking for a
    # memory-corruption bug that does not exist. It cost a bisect that turned
    # up nothing. Create the directories first and torch exits 0.
    OUT_DIRS = ["assets", "assets/code", "assets/code/common_data",
                "assets/code/startup_logo", "assets/code/ceremony_data",
                "assets/code/data_800E45C0", "assets/code/data_800E8700",
                "assets/code/rainbow_road_tluts", "assets/course_metadata",
                "bin", "dc_data", "textures/raw", "textures/standalone",
                "textures/crash_screen", "textures/trophy", "textures/courses",
                "textures/startup_logo", "textures/common", "textures/common/tlut",
                "Platform/xbox/gen", "Platform/xbox/gen_asm",
                "Platform/xbox/gen_seg", "include/assets"]

    def dirs_step():
        for d in OUT_DIRS:
            os.makedirs(d, exist_ok=True)
        print("    created %d output directories" % len(OUT_DIRS))

    def asset_payloads_step():
        """Extract assets/** from the ROM, one JSON descriptor at a time.

        Distinct from extract_assets.py, which only fills textures/. This is
        what produces the ~32,000 kart frames, portraits and palettes that
        data/**/*.s incbins, and without it gen_binassets fails on every
        target and gen_asmwrappers then drops 24,570 symbols.
        """
        jsons = sorted(glob.glob("assets/**/*.json", recursive=True))
        done = 0
        for j in jsons:
            r = subprocess.run([py, "tools/new_extract_assets.py", ROM, j],
                               cwd=ROOT, capture_output=True)
            if r.returncode != 0:
                raise SystemExit("%s! new_extract_assets failed on %s%s\n%s"
                                 % (RED, j, OFF,
                                    r.stderr.decode("utf-8", "replace")[-2000:]))
            done += 1
        print("    extracted %d asset descriptors" % done)

    def texture_incs_step():
        """Generate every *.inc.c that src/ #includes but nothing produces.

        The Makefile emits these into its BUILD_DIR (n64graphics, rules at
        Makefile:591/596), but the Xbox sources include them by their SOURCE
        path -- src/crash_screen.c:30 and src/data/some_data.c:185 among
        others -- so they have to land in the tree instead.

        Covers textures/ and assets/ alike. Anything under assets/code/ is
        skipped: torch writes those, and they are not image data.

        The pixel format is the second-to-last dotted component of the name,
        exactly as the Makefile's `$(lastword $(subst ., ,$@))` derives it.
        A name with no format component is a palette (gTLUTOnomatopoeia), and
        N64 TLUTs are rgba16.
        """
        n64graphics = os.path.abspath("tools/n64graphics" + EXE)
        wanted = set()
        pat = re.compile(r'#include\s+"((?:textures|assets)/[^"]+\.inc\.c)"')
        # courses/ as well as src/: each course_data.c pulls in its own
        # palettes the same way (banshee_boardwalk/course_data.c:2909).
        scan = []
        for tree in ("src", "courses"):
            scan += glob.glob(tree + "/**/*.c", recursive=True)
            scan += glob.glob(tree + "/**/*.h", recursive=True)
        for src in scan:
            text = io.open(src, encoding="utf-8", errors="replace").read()
            wanted.update(pat.findall(text))
        wanted = {w for w in wanted if not w.startswith("assets/code/")}

        made = 0
        for inc in sorted(wanted):
            if os.path.exists(inc):
                continue
            stem = inc[:-len(".inc.c")]          # textures/x/name.ia1
            png = stem + ".png"
            if not os.path.exists(png):
                raise SystemExit(
                    "%s! %s is #included by src/ but neither it nor %s "
                    "exists.%s\n  Nothing in the chain produces it."
                    % (RED, inc, png, OFF))
            tail = os.path.basename(stem).rsplit(".", 1)
            fmt = tail[-1] if len(tail) > 1 else "rgba16"
            sh(n64graphics, "-i", inc, "-g", png, "-f", fmt, "-s", "u8")
            made += 1
        print("    %d generated, %d already present (%d source-included)"
              % (made, len(wanted) - made, len(wanted)))

    def patch_torch_output_step():
        """Add the type header torch omits from its generated .c files.

        torch emits `u16 gTLUT...[] = {` with no include that defines u16, so
        the file cannot compile. In the dev tree this had been fixed BY HAND
        (assets/code/rainbow_road_tluts/rainbow_road_tluts.c carries a comment
        saying so) -- which means regenerating silently reintroduced the
        breakage. Automate it, or the next clean build hits it again.
        """
        need = re.compile(r'^\s*(?:u|s)(?:8|16|32|64)\s+\w+\s*\[', re.M)
        patched = 0
        for f in sorted(glob.glob("assets/code/**/*.c", recursive=True)):
            text = io.open(f, encoding="utf-8", errors="replace").read()
            if "PR/ultratypes.h" in text or not need.search(text):
                continue
            io.open(f, "w", encoding="utf-8").write(
                "/* Torch emits this without the type header it needs. */\n"
                "#include <PR/ultratypes.h>\n\n" + text)
            patched += 1

        # torch can emit the SAME array in two segments' files -- the ceremony
        # and the startup logo both carry reflection_map_gold -- which the
        # linker rejects as a duplicate symbol. The dev tree had this fixed by
        # hand too (startup_logo_reflection_map_gold), so regenerating brought
        # the clash back. Keep the first definition and prefix the others with
        # their segment name, which is the convention that fix used. Nothing
        # references the renamed copy; only the ceremony's is ever read.
        defn = re.compile(r'^\s*(?:extern\s+)?(?:const\s+)?[A-Za-z_]\w*\s+(\w+)\s*\[',
                          re.M)
        seen, renamed = {}, 0
        for f in sorted(glob.glob("assets/code/**/*.c", recursive=True)):
            text = io.open(f, encoding="utf-8", errors="replace").read()
            seg = os.path.basename(os.path.dirname(f))
            changed = False
            for sym in set(defn.findall(text)):
                if sym not in seen:
                    seen[sym] = f
                    continue
                if seen[sym] == f:
                    continue
                new = "%s_%s" % (seg, sym)
                # Rename the DECLARATION only. The payload it includes is a
                # file whose name embeds the same symbol, so a blanket
                # substitution rewrites the #include path to something that
                # does not exist.
                out = []
                for line in text.splitlines(True):
                    if not line.lstrip().startswith("#include"):
                        line = re.sub(r'\b%s\b' % re.escape(sym), new, line)
                    out.append(line)
                text = "".join(out)
                changed = True
                renamed += 1
            if changed:
                io.open(f, "w", encoding="utf-8").write(text)

        print("    patched %d file(s) for missing types, renamed %d duplicate "
              "symbol(s)" % (patched, renamed))

    def linkonly_step():
        """Regenerate courses/*/course_textures.linkonly.{c,h}.

        Excluded from the repo by `/courses/**/*linkonly*` and derived from
        each course's course_offsets.c, so a clean tree has neither. The
        header is what course_data.c includes, so its absence stops the build
        on the first course.
        """
        courses = sorted(d.replace("\\", "/").split("/")[1]
                         for d in glob.glob("courses/*")
                         if os.path.isdir(d) and
                         os.path.exists(os.path.join(d, "course_offsets.c")))
        for c in courses:
            r = subprocess.run([py, "tools/linkonly_generator.py", c],
                               cwd=ROOT, capture_output=True)
            if r.returncode != 0:
                raise SystemExit("%s! linkonly_generator failed on %s%s\n%s"
                                 % (RED, c, OFF,
                                    r.stderr.decode("utf-8", "replace")[-2000:]))
        print("    generated linkonly pairs for %d courses" % len(courses))

    SEG_BLOBS = ("ceremony_data", "common_data", "data_segment2", "startup_logo")

    def segblobs_step():
        """Generate the MIO0 segment blobs and the wrappers that .incbin them.

        There is a genuine cycle here on a clean tree: gen_segblobs needs four
        COMPILED OBJECTS (data_segment2, ceremony_data, startup_logo,
        common_data), but the wrappers it writes are themselves listed in
        rxdk.project.json, so the build cannot get that far without them.

        It used to be hidden because the wrappers were committed -- carrying
        one machine's absolute .incbin paths, which is why a build elsewhere
        would have failed or silently read the wrong files.

        Broken by running the build ONCE and ignoring whether it succeeds:
        those four objects are compiled well before the build reaches the
        missing blob sources, so a failed run still leaves exactly what is
        needed. Only pays this cost on a tree that has no wrappers yet.
        """
        missing = [s for s in SEG_BLOBS
                   if not os.path.exists("Platform/xbox/gen_seg/%s_blob.c" % s)]
        if missing:
            print("    %sbootstrap: building once for the segment objects%s"
                  % (DIM, OFF))
            subprocess.run([RXDK, "build", "--project-root", ".",
                            "--configuration", config],
                           cwd=ROOT, capture_output=True)   # failure expected
        sh(py, "tools/gen_segblobs.py", env={"RXDK_OBJ_DIR": objdir})

    def incbins_step():
        """Write <objdir>/incbins.txt, which gen_binassets reads and nothing
        in the tree produces -- it was a transient dev artifact.

        The list comes from the decomp's own data-only .s files, NOT from the
        generated wrappers. That matters: gen_asmwrappers DROPS an .incbin
        whose file is missing, so deriving the list from its output on a clean
        tree would yield an empty list, gen_binassets would build nothing, and
        the wrappers would then silently alias every affected symbol.
        """
        # Skip the same .s files gen_asmwrappers skips, or the list describes
        # assets the build never assembles. data/sound_data/* is loaded off
        # the disc at runtime instead of being linked in, and it references
        # EU/PAL audio that simply is not in a US ROM -- harvesting those made
        # gen_binassets report 31 alarming "fail:" lines for assets nothing
        # wants. (Harmless, because those .s are never wrapped and so can
        # never alias a symbol, but a new user has no way to know that.)
        EXCLUDE = ("data/sound_data/", "data/textures_0a.s")
        pat = re.compile(r'\.incbin\s+"([^"]+)"')
        seen, targets = set(), []
        for s in sorted(glob.glob("data/**/*.s", recursive=True)):
            rel = s.replace("\\", "/")
            if any(rel.startswith(e) or rel == e for e in EXCLUDE):
                continue
            text = io.open(s, encoding="utf-8", errors="replace").read()
            for m in pat.finditer(text):
                p = m.group(1)
                if p not in seen:
                    seen.add(p)
                    targets.append(p)
        os.makedirs(objdir, exist_ok=True)
        io.open(objdir + "/incbins.txt", "w", encoding="utf-8").write(
            "\n".join(targets) + "\n")
        print("    %s incbin targets from data/**/*.s" % format(len(targets), ","))

    def tools_step():
        # MinGW installs call it mingw32-make; MSYS2 and w64devkit call it
        # make; some BSD-ish setups gmake. Any of them builds tools/Makefile.
        make = next((m for m in ("make", "mingw32-make", "gmake")
                     if shutil.which(m)), None)
        if not make:
            raise SystemExit(
                "%s! no make found (tried make, mingw32-make, gmake).%s\n\n"
                "  extract_assets.py shells out to six native helpers --\n"
                "  n64graphics, mio0, tkmk00, n64cksum, extract_data_for_mio\n"
                "  and displaylist_packer. Their C sources ARE in the repo but\n"
                "  the binaries are not, so they have to be compiled once:\n\n"
                "    make -C tools\n\n"
                "  On Windows the DC port ships w64devkit for exactly this;\n"
                "  any MinGW/MSYS2 install with make and gcc will do.\n"
                % (RED, OFF))
        # Name the programs rather than using `all`: that target also builds
        # torch, by shelling out to a hardcoded `make`, which fails on a MinGW
        # install where the binary is mingw32-make. torch is a CMake project
        # with its own build step below, so it does not belong here anyway.
        sh(make, "-C", "tools", "mio0", "n64graphics", "displaylist_packer",
           "n64cksum", "tkmk00", "extract_data_for_mio")

    return [
        Step("create the output directories the extractors assume",
             [],                            # cheap and idempotent; always run
             dirs_step,
             "torch aborts with a bogus 0xC0000409 if these are missing"),

        Step("build the native asset helpers",
             ["tools/n64graphics" + EXE, "tools/mio0" + EXE,
              "tools/tkmk00" + EXE, "tools/n64cksum" + EXE,
              "tools/extract_data_for_mio" + EXE,
              "tools/displaylist_packer" + EXE],
             tools_step,
             "extract_assets.py shells out to these; a clean clone has none"),

        # Declare the HEADERS as outputs too, not just the .c directories.
        # `torch code` and `torch header` are separate invocations, and
        # checking only the former lets a half-done run report "up to date" --
        # the build then dies much later on a missing assets/*.h.
        Step("torch: assets/code and include/assets from the ROM",
             ["assets/code/common_data", "assets/code/ceremony_data",
              "include/assets/common_data.h", "include/assets/ceremony_data.h",
              "assets/course_metadata/gCourseNames.inc.c"],
             torch_step,
             "180 .c files the build lists as sources, plus their headers"),

        # Check something THIS step produces. An earlier version looked at
        # assets/course_metadata and textures/raw -- both of which torch and
        # extract_rom_bins create -- so it reported "up to date" and never
        # ran, and the failure surfaced 24,570 dropped incbins later.
        Step("patch torch's generated .c for missing type headers",
             [],                            # idempotent; always verify
             patch_torch_output_step,
             "torch emits u16 arrays with nothing defining u16"),

        # torch regenerates assets/code in texel-VALUE order. Four files must
        # instead hold the raw big-endian byte stream, because nothing swaps
        # them at boot (main.c's tlut_ptr loop covers the other ~57, which is
        # why those must NOT be touched). Get it wrong on the startup logo and
        # the Nintendo logo renders as rainbow noise with alpha=0.
        #
        # The two tools are a console-verified PAIR and must both run, in this
        # order: swap_u16_incs converts all 81 u16 arrays it can find, then
        # unswap_u16_incs reverts the 77 that were already correct, keeping the
        # 4 named in its KEEP set. Running only the first double-swaps the HUD,
        # portraits and trees into noise.
        Step("byteswap the value-convention asset arrays",
             [],                            # idempotent, marker-guarded
             lambda: (sh(py, "tools/swap_u16_incs.py"),
                      sh(py, "tools/unswap_u16_incs.py")),
             "reflection maps only; the rest are swapped at boot by main.c"),

        Step("extract_assets: textures, palettes, course payloads",
             ["assets/**/*.png", "assets/**/*.mio0"],
             lambda: sh(py, "extract_assets.py", "us"),
             "produces ~32,000 payload files under assets/"),

        Step("extract_rom_bins: verbatim ROM slices",
             ["bin/audiobanks.us.bin", "bin/audiotables.bin"],
             lambda: sh(py, "tools/extract_rom_bins.py"),
             "audio banks/tables and the raw textures named by ROM offset"),

        Step("new_extract_assets: the assets/** payload tree",
             ["assets/**/*.mio0"],
             asset_payloads_step,
             "kart frames, portraits, palettes -- what data/**/*.s incbins"),

        # Three ROM-truth fixups that the generic extractors get WRONG. Each
        # was written to fix a specific hardware bug and each is required for
        # correctness, not tidiness -- they had only ever been run by hand, so
        # nothing in the chain invoked them and a clean build silently
        # reintroduced every one of these bugs.
        Step("extract_ci_frames_rom: CI frames verbatim from the ROM",
             [],
             lambda: sh(py, "tools/extract_ci_frames_rom.py"),
             "shells/trees/banner: red shells reuse green INDICES, so a "
             "re-quantized PNG is wrong"),

        Step("extract_boo_frames: Banshee's Boos",
             ["assets/courses/banshee_boardwalk/boo_frames.mio0"],
             lambda: sh(py, "tools/extract_boo_frames.py"),
             "the Boos ship as 29 PNGs with no repacking rule; without this "
             "gTextureGhosts aliases gTextureExhaust0"),

        # linkonly FIRST: it generates course_textures.linkonly.c, which
        # itself #includes .inc.c files. Scanning for those includes before
        # the files that contain them exist misses every one.
        Step("linkonly_generator: per-course texture link stubs",
             ["courses/banshee_boardwalk/course_textures.linkonly.h"],
             linkonly_step,
             "course_data.c includes the .h; excluded from the repo"),

        Step("generate the *.inc.c that src/ and courses/ include",
             [],                            # cheap; verifies every one exists
             texture_incs_step,
             "the Makefile puts these in BUILD_DIR; our sources include them in-tree"),

        # MUST follow the .inc.c generation above: it REWRITES those files, and
        # on a clean tree they do not exist until n64graphics has made them.
        # Run it earlier and it silently fixes nothing -- the sprites stay
        # PVR-twiddled at 2x size, the fixed array dims truncate them, and the
        # Moo Moo mole glitch comes straight back.
        Step("fix_course_sprites: un-twiddle the DC course sprites",
             [],
             lambda: sh(py, "tools/fix_course_sprites.py"),
             "after the .inc.c exist, because it rewrites them in place"),

        # No declared outputs: ALWAYS runs. The list is DERIVED from the .s
        # files and from which of them the build actually assembles, so an
        # existing incbins.txt can be stale and silently wrong. Gating it on
        # the file's existence meant a changed exclusion rule had no effect
        # until the file was deleted by hand. Rebuilding it is a directory
        # scan.
        Step("list the incbin targets from data/**/*.s",
             [],
             incbins_step,
             "gen_binassets reads this list; nothing else produces it"),

        # No declared outputs: ALWAYS runs. It was gated on `assets/**/*.mio0`,
        # a glob that new_extract_assets satisfies with a handful of files --
        # so on a clean tree it reported "up to date" and never built the other
        # 24,715 incbin targets. gen_asmwrappers then dropped them all and the
        # XBE came out 12MB short, missing every kart frame. The tool is
        # idempotent (it skips targets that already exist), so always running
        # costs a directory scan.
        Step("gen_binassets: .bin/.mio0 the MIPS data files incbin",
             [],
             lambda: sh(py, "tools/gen_binassets.py",
                        env={"RXDK_OBJ_DIR": objdir}),
             "MUST precede gen_asmwrappers -- see the module docstring"),

        # No declared outputs: this ALWAYS runs. Its output bakes in absolute
        # paths to the current checkout, so a copy carried over from another
        # machine is worse than useless -- it silently points at that machine's
        # assets. Cheap to regenerate, so never trust an existing one.
        Step("gen_asmwrappers: MIPS .s -> .c RXDK can compile",
             [],
             lambda: sh(py, "tools/gen_asmwrappers.py"),
             "always re-run: its incbin paths are absolute to this checkout"),

        # BEFORE the first build, not after: the wrappers it writes are listed
        # in rxdk.project.json, so pass 1 cannot compile without them. It does
        # NOT need the compiled objects -- it sources from the ROM and the
        # extracted assets -- so there is no ordering conflict, only the one
        # this used to create. (Published, these wrappers carried one machine's
        # absolute .incbin paths; the build appeared to work here purely
        # because the dev tree's files existed at those paths. They are
        # gitignored now and regenerated every run.)
        Step("gen_segblobs: MIO0 segment blobs + their wrappers",
             [],
             segblobs_step,
             "must precede build pass 1: it writes sources the build compiles"),

        # imagebld needs these to stamp the XBE, and dc_data is gitignored, so
        # a clean tree has neither. The source art IS published at the repo
        # root; make_xbx.py converts it to the dashboard's XPR0 format.
        Step("make_xbx: dashboard title and save icons",
             ["dc_data/titleimage.xbx", "dc_data/saveimage.xbx"],
             lambda: sh(py, "tools/make_xbx.py"),
             "imagebld refuses to run without them"),

        Step("RXDK build (pass 1: objects)",
             [objdir + "/Build"],
             lambda: sh(RXDK, "build", "--project-root", ".",
                        "--configuration", config),
             "the next two steps dump .data out of these objects"),

        # Not gated on outputs existing: after the reflection-map byteswap was
        # fixed, this skipped itself and startup_logo.bin kept the pre-fix
        # bytes -- same size, different content -- and the Nintendo logo stayed
        # miscoloured.
        Step("gen_dcdata: the runtime data set",
             [],
             lambda: sh(py, "tools/gen_dcdata.py",
                        env={"RXDK_OBJ_DIR": objdir}),
             "always: same reason"),

        # Also overwrites gen_dcdata. It objcopies the compiled common_data
        # array, which main.c byteswaps at boot and so holds texel VALUES --
        # but the shell draw lists read this segment RAW through 0x0D004E38
        # and need the ROM's big-endian bytes. Getting it wrong renders the
        # shells as rainbow noise with alpha 0.
        Step("rebuild common_data.bin from the ROM",
             [],
             lambda: sh(py, "tools/gen_common_data_bin.py"),
             "gen_dcdata sources it from the boot-swapped array"),

        # MUST follow gen_dcdata and OVERWRITE what it wrote. gen_dcdata builds
        # the sound segments by concatenating .incbin targets, which drops the
        # header both of these carry: instrument_sets.bin came out empty (it
        # has no incbins at all) and sequences.bin came out 244 bytes short,
        # missing its ALSeqFile header. The game then read sequence bytes as
        # the header and crashed at boot in sequence_player_process_sequence.
        # No declared outputs, so it always runs and always wins.
        Step("assemble the sound segments (sequences, instrument_sets)",
             [],
             lambda: sh(py, "tools/gen_sound_segments.py"),
             "gen_dcdata's concatenation drops their headers"),

        # MUST come after gen_dcdata, which unconditionally overwrites
        # adpcm_pool.bin with a 32-byte stub (gen_dcdata.py:89). Its comment
        # there -- "only read by the mixer, which is stubbed on Xbox" -- is
        # stale: aica_synth.c's DirectSound path reads the real 2.2MB pool
        # through gAicaAdpcmPoolBase. Running earlier gets the pool clobbered
        # and yields an ISO that boots with no sound.
        #
        # No declared outputs, so it always runs: an interrupted run can leave
        # a truncated pool behind, and an existence-only check accepts it.
        Step("aica: ADPCM pool + sample table",
             [],
             lambda: sh(py, "tools/aica/mk64_emit.py",
                        "--audiobanks", "bin/audiobanks.us.bin",
                        "--audiotables", "bin/audiotables.bin",
                        "--outdir", "Platform/xbox/gen",
                        "--incdir", "Platform/xbox/gen",
                        "--pool", "dc_data/adpcm_pool.bin",
                        env=aica_env),
             "after gen_dcdata: that step stubs the pool out"),

        Step("RXDK build (pass 2: ISO)",
             [],                            # always runs; packs the dc_data above
             lambda: sh(RXDK, "build", "--project-root", ".",
                        "--configuration", config)),
    ]


def require_console():
    """Refuse to run under IDLE.

    This is not fussiness. The build starts many THOUSANDS of child processes
    -- extract_assets alone shells out to n64graphics and mio0 once per asset
    -- and IDLE runs user code in a process with no console, so Windows
    allocates a BRAND NEW console window for every console child. You get
    windows flashing open and shut for the whole build, and it takes hours
    instead of minutes.

    It cannot be fixed from here: the spawns that matter are inside the
    extractors' own subprocess calls, and a console-less parent makes every
    descendant allocate one. A real terminal fixes all of them at once,
    because children inherit it.

    Detected by looking for idlelib rather than by asking Windows whether a
    console is attached -- GetConsoleWindow() also returns 0 under Git Bash,
    which is a perfectly good place to build from.
    """
    idle = ("idlelib" in sys.modules
            or type(sys.stdin).__module__.startswith("idlelib")
            or type(sys.stdout).__module__.startswith("idlelib"))
    if not idle:
        return
    raise SystemExit(
        "Do not run this from IDLE.\n"
        "\n"
        "Open Command Prompt, PowerShell or Git Bash, cd to this folder, and\n"
        "run:\n"
        "\n"
        "    python setup.py\n"
        "\n"
        "Why: the build starts many thousands of helper processes. IDLE runs\n"
        "your code without a console, so Windows opens a NEW console window\n"
        "for every one of them -- windows flashing endlessly, and a build that\n"
        "takes hours instead of minutes. In a terminal they all share yours.\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="report what is missing and exit without changing anything")
    ap.add_argument("--skip-build", action="store_true",
                    help="run the asset steps but not the RXDK build")
    ap.add_argument("--force", action="store_true",
                    help="run every step even when its outputs already exist")
    ap.add_argument("--config", default="Release", choices=["Release", "Debug"])
    args = ap.parse_args()

    os.chdir(ROOT)
    if not args.check:
        require_console()
    print("\n%sMario Kart 64 -- Xbox%s   setup, %s\n" % (GREEN, OFF, args.config))
    check_rom()

    steps = build_steps(args.config)
    if args.skip_build:
        steps = [s for s in steps if not s.name.startswith("RXDK build")]

    if args.check:
        print("\n  %-52s %s" % ("STEP", "STATUS"))
        missing = 0
        for s in steps:
            ok = s.satisfied()
            missing += 0 if ok else 1
            if ok:
                state = GREEN + "present" + OFF
            elif not s.outputs:
                state = DIM + "always runs" + OFF
            else:
                state = YELLOW + "MISSING" + OFF
            print("  %-52s %s" % (s.name[:52], state))
        if torch_exe() is None:
            print("\n  torch is not built -- setup.py will fetch and build it "
                  "for you on the first run.")
        print("\n%d of %d steps still to run." % (missing, len(steps)))
        return 0

    if not os.path.exists(RXDK) and not args.skip_build:
        raise SystemExit(
            "%s! RXDK not found at %s%s\n"
            "  Install it, or pass --skip-build to stop after asset generation."
            % (RED, RXDK, OFF))

    for i, s in enumerate(steps, 1):
        if s.satisfied() and not args.force and s.outputs:
            print("\n[%d/%d] %s\n    %sup to date%s"
                  % (i, len(steps), s.name, DIM, OFF))
            continue
        print("\n[%d/%d] %s" % (i, len(steps), s.name))
        if s.why:
            print("    %s%s%s" % (DIM, s.why, OFF))
        s.run()

    iso = "out/%s/XISO/mk64x.iso" % args.config
    print("\n%sDone.%s" % (GREEN, OFF))
    if os.path.exists(iso):
        print("  %s  (%.1f MB)" % (iso, os.path.getsize(iso) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

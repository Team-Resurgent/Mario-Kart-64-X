"""Where RXDK put things on this machine, and which C compiler to use.

One place for every host-side path the build scripts need, so they stop
carrying `C:\\ProgramData\\...` and `%LOCALAPPDATA%\\...\\zig.exe` literals
that only resolve on one Windows install.

The layouts mirror what the RXDK extensions themselves use (RXDK-VSCode
hostTools.ts / zigRuntime.ts and RXDK-Tools RxdkPaths.cs), including their
environment overrides, so a machine where RXDK works is a machine where these
resolve:

    tools   Windows  %ProgramData%\\RXDK\\tools
            macOS    ~/Library/Application Support/RXDK/tools
            Linux    $XDG_DATA_HOME/rxdk/tools   (default ~/.local/share)
            override RXDK_STAGED_TOOLS
    zig     Windows  %LOCALAPPDATA%\\RXDK\\zig\\<ver>\\zig-<arch>-windows-<ver>\\zig.exe
            macOS    ~/Library/Application Support/RXDK/zig/<ver>/zig-*/zig
            Linux    $XDG_DATA_HOME/rxdk/zig/<ver>/zig-*/zig
            override RXDK_ZIG

Import as `import hostenv` -- every script under tools/ is run with tools/
as sys.path[0], and setup.py adds it explicitly.
"""
import glob
import os
import shlex
import shutil
import subprocess
import sys

WIN = os.name == "nt"
MAC = sys.platform == "darwin"
EXE = ".exe" if WIN else ""

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _home():
    return os.path.expanduser("~")


def _xdg_data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.join(_home(), ".local", "share")


def _env_dir(name):
    v = os.environ.get(name, "").strip()
    return os.path.abspath(v) if v else None


def rxdk_data_root():
    """The machine-wide RXDK root that tools/, sdk/, samples/ live under."""
    if WIN:
        return os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "RXDK")
    if MAC:
        return os.path.join(_home(), "Library", "Application Support", "RXDK")
    return os.path.join(_xdg_data_home(), "rxdk")


def staged_tools_root():
    return _env_dir("RXDK_STAGED_TOOLS") or os.path.join(rxdk_data_root(), "tools")


def staged_samples_root():
    return _env_dir("RXDK_STAGED_SAMPLES") or os.path.join(rxdk_data_root(), "samples")


def host_tool(name):
    """Absolute path of an RXDK host tool (Rxdk.Cli, imagebld, bundler...)."""
    return os.path.join(staged_tools_root(), name + EXE)


def rxdk_cli():
    return host_tool("Rxdk.Cli")


def zig_install_root():
    if WIN:
        local = os.environ.get("LOCALAPPDATA") or os.path.join(_home(), "AppData", "Local")
        return os.path.join(local, "RXDK", "zig")
    if MAC:
        return os.path.join(_home(), "Library", "Application Support", "RXDK", "zig")
    return os.path.join(_xdg_data_home(), "rxdk", "zig")


def zig():
    """The zig binary, RXDK's managed copy preferred. None if there is none.

    Order matches the RXDK engine: RXDK_ZIG, then the managed install (newest
    version first), then whatever `zig` is on PATH. For compiling host tools
    any zig will do -- the version pin only matters for the Xbox target.
    """
    env = os.environ.get("RXDK_ZIG", "").strip()
    if env:
        if os.path.isfile(env):
            return os.path.abspath(env)
        raise SystemExit("RXDK_ZIG points to a missing file: %s" % env)
    root = zig_install_root()
    found = []
    for pat in (os.path.join(root, "*", "zig-*", "zig" + EXE),
                os.path.join(root, "*", "zig" + EXE)):
        found += [p for p in glob.glob(pat) if os.path.isfile(p)]
    if found:
        # Sort by the version directory so 0.16.0 beats 0.14.0.
        def ver(p):
            v = p[len(root):].strip("\\/").split(os.sep)[0]
            return tuple(int(x) if x.isdigit() else 0 for x in v.split("."))
        return max(found, key=ver)
    return shutil.which("zig")


def cc_command():
    """The command prefix that compiles C on this host, as a list.

    `CC` in the environment wins, then RXDK's zig (`zig cc` -- a complete
    Clang + libc that RXDK already guarantees on every machine it runs on),
    then the usual system names. None if nothing is found.
    """
    env = os.environ.get("CC", "").strip()
    if env:
        return shlex.split(env, posix=not WIN)
    z = zig()
    if z:
        return [z, "cc"]
    for name in ("cc", "gcc", "clang"):
        p = shutil.which(name)
        if p:
            return [p]
    return None


def shared_lib_suffix():
    return ".dll" if WIN else (".dylib" if MAC else ".so")


def compile_exe(sources, out, flags=(), cwd=None):
    """Compile `sources` into the executable `out`. Raises on failure."""
    cc = cc_command()
    if cc is None:
        raise SystemExit(
            "no C compiler found. Install RXDK (which brings zig), or put\n"
            "cc/gcc/clang on PATH, or set CC.")
    cmd = cc + list(flags) + list(sources) + ["-o", out]
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        raise SystemExit("compile failed: %s" % " ".join(cmd))
    return out

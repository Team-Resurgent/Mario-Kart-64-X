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
    llvm    Windows  %ProgramData%\\RXDK\\llvm\\xboxog-windows-<arch>\\bin\\clang.exe
            macOS    ~/Library/Application Support/RXDK/llvm/xboxog-macos-<arch>/bin/clang
            Linux    $XDG_DATA_HOME/rxdk/llvm/xboxog-linux-<arch>/bin/clang
            override RXDK_LLVM (a root, or a parent of <name>-<os>-<arch>)

The RXDK clang is host-capable: on Windows it links host tools through the
mingw sysroot bundled in the package (<llvm-root>/host-sysroot, override
RXDK_HOST_SYSROOT); on Linux/macOS it uses the system sysroot. This replaces
the old `zig cc` host-compiler path -- no zig/MinGW/MSYS2 install is needed.

Import as `import hostenv` -- every script under tools/ is run with tools/
as sys.path[0], and setup.py adds it explicitly.
"""
import glob
import os
import platform
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


def llvm_install_root():
    """Root that holds the unpacked RXDK LLVM toolchain(s), <root>/<name>-<os>-<arch>."""
    return os.path.join(rxdk_data_root(), "llvm")


def _host_arch():
    m = platform.machine().lower()
    return "arm64" if ("arm64" in m or "aarch64" in m) else "x64"


def _os_tag():
    return "windows" if WIN else ("macos" if MAC else "linux")


def llvm_root():
    """The RXDK clang toolchain dir (the one with bin/clang) for this host.

    RXDK_LLVM wins (a root, or a parent holding <name>-<os>-<arch>); otherwise
    the managed install under llvm_install_root(). Mirrors RXDK-VSCode
    llvmRuntime.ts / RXDK-Tools LlvmRuntime. The future merged `xbox` package
    name is accepted alongside today's `xboxog`. None if not found.
    """
    tag = "%s-%s" % (_os_tag(), _host_arch())
    cands = []
    env = _env_dir("RXDK_LLVM")
    if env:
        cands += [env, os.path.join(env, "xboxog-" + tag), os.path.join(env, "xbox-" + tag)]
    base = llvm_install_root()
    cands += [os.path.join(base, "xboxog-" + tag), os.path.join(base, "xbox-" + tag)]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "bin", "clang" + EXE)):
            return c
    return None


def clang():
    r = llvm_root()
    if r:
        return os.path.join(r, "bin", "clang" + EXE)
    return shutil.which("clang")


def clangxx():
    r = llvm_root()
    if r:
        cxx = os.path.join(r, "bin", "clang++" + EXE)
        return cxx if os.path.isfile(cxx) else os.path.join(r, "bin", "clang" + EXE)
    return shutil.which("clang++") or shutil.which("clang")


def host_sysroot():
    """The bundled mingw host sysroot (Windows only; None on Unix, where clang
    uses the system sysroot). RXDK_HOST_SYSROOT overrides; else the package's
    own <llvm-root>/host-sysroot.
    """
    if not WIN:
        return None
    env = _env_dir("RXDK_HOST_SYSROOT")
    if env and os.path.isdir(env):
        return env
    r = llvm_root()
    if r:
        s = os.path.join(r, "host-sysroot")
        if os.path.isdir(s):
            return s
    return None


def _host_flags(sysroot):
    """clang flags that target the native host through the bundled mingw sysroot,
    using compiler-rt + libunwind (llvm-mingw ships no libgcc)."""
    triple = ("aarch64" if _host_arch() == "arm64" else "x86_64") + "-w64-mingw32"
    flags = ["--target=" + triple, "-fuse-ld=lld", "--sysroot=" + sysroot,
             "--rtlib=compiler-rt", "--unwindlib=libunwind"]
    rd = sorted(glob.glob(os.path.join(sysroot, "lib", "clang", "*")))
    if rd:
        flags.append("-resource-dir=" + rd[-1])
    return flags


def cc_command():
    """Command prefix (list) that compiles + links C host tools on this machine.

    `CC` wins; then RXDK's clang -- on Windows through the bundled mingw
    host-sysroot, on Unix with the system sysroot -- then a system cc/gcc/clang.
    None if nothing is found. Replaces the old `zig cc` path: RXDK now ships a
    host-capable clang, so no zig/MinGW/MSYS2 install is needed.
    """
    env = os.environ.get("CC", "").strip()
    if env:
        return shlex.split(env, posix=not WIN)
    c = clang()
    if c:
        if not WIN:
            return [c]
        sr = host_sysroot()
        if sr:
            return [c] + _host_flags(sr)
        # Windows RXDK clang with no bundled sysroot yet: fall back to a system compiler.
    for name in ("clang", "cc", "gcc"):
        p = shutil.which(name)
        if p:
            return [p]
    return None


def cxx_command():
    """Like cc_command() but for C++ -- adds libc++ and static linking on Windows
    so the host tool has no runtime DLL dependencies."""
    env = os.environ.get("CXX", "").strip()
    if env:
        return shlex.split(env, posix=not WIN)
    c = clangxx()
    if c:
        if not WIN:
            return [c]
        sr = host_sysroot()
        if sr:
            return [c] + _host_flags(sr) + ["-stdlib=libc++", "-static"]
    for name in ("clang++", "c++", "g++"):
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
            "no C compiler found. Install RXDK (which brings a host-capable\n"
            "clang), or put clang/cc/gcc on PATH, or set CC.")
    cmd = cc + list(flags) + list(sources) + ["-o", out]
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        raise SystemExit("compile failed: %s" % " ".join(cmd))
    return out

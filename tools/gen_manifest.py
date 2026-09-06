#!/usr/bin/env python3
"""Regenerate the committed rxdk.project.json from mk64x.vcxproj.

    python tools/gen_manifest.py            # regenerate, then review `git diff`
    python tools/gen_manifest.py --check    # exit 1 if the committed json is stale

Same convention as RXDK-Samples (scripts/Generate-Manifests.ps1 there): the
.vcxproj is the authoritative project file, edited in Visual Studio's
Solution Explorer and property pages, and the multi-config rxdk.project.json
is DERIVED from it -- it is what setup.py and the VS Code extension read,
including on Linux and macOS where there is no MSBuild. Run this after any
change to the .vcxproj and commit the result.

How: the RXDK "Xbox" MSBuild platform defines an RxdkGenerateManifest target
that writes a flat, single-configuration manifest from the project's Rxdk*
properties and ClCompile items. It is run here once for Debug and once for
Release into out/ (gitignored), and the pair is merged into the committed
file. Deterministic output (json.dump indent=2, LF), so an unchanged .vcxproj
reproduces a byte-identical json; --check relies on that.

Needs Windows with Visual Studio (or Build Tools) plus the RXDK Xbox platform
installed (VS: "RXDK: Install Xbox Platform"). Everyone else just consumes
the committed json.
"""
import glob, io, json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
PROJECT = "mk64x.vcxproj"
OUT = "rxdk.project.json"


def find_msbuild():
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if os.path.exists(vswhere):
        r = subprocess.run([vswhere, "-latest", "-prerelease", "-products", "*",
                            "-requires", "Microsoft.Component.MSBuild",
                            "-find", r"MSBuild\**\Bin\MSBuild.exe"],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.strip():
                return line.strip()
    import shutil
    return shutil.which("msbuild")


def generate(msbuild, config):
    out = os.path.abspath(os.path.join("out", "rxdk.%s.json" % config))
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([msbuild, PROJECT, "-t:RxdkGenerateManifest", "-nologo", "-v:m",
                        "-p:Configuration=%s" % config, "-p:Platform=Xbox",
                        "-p:RxdkManifestFile=%s" % out],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(
            "RxdkGenerateManifest failed for %s. Is the RXDK 'Xbox' MSBuild platform "
            "installed? (Visual Studio: RXDK: Install Xbox Platform)" % config)
    return json.load(io.open(out, encoding="utf-8"))


def main():
    check = "--check" in sys.argv
    msbuild = find_msbuild()
    if not msbuild:
        raise SystemExit("MSBuild not found: install Visual Studio or Build Tools with the C++ workload.")

    default = "Release"
    if os.path.exists(OUT):
        try:
            default = json.load(io.open(OUT, encoding="utf-8")).get("defaultConfiguration", default)
        except ValueError:
            pass

    D = generate(msbuild, "Debug")
    R = generate(msbuild, "Release")
    name = D.get("name") or os.path.splitext(PROJECT)[0]
    for body in (D, R):
        body.pop("name", None)              # shared at the top level
    manifest = {
        "name": name,
        "defaultConfiguration": default,
        "configurations": {"Debug": D, "Release": R},
    }
    text = json.dumps(manifest, indent=2) + "\n"

    old = io.open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else None
    if check:
        if old == text:
            print("OK: %s is up to date with %s" % (OUT, PROJECT))
            return 0
        import difflib
        sys.stdout.writelines(difflib.unified_diff(
            (old or "").splitlines(True), text.splitlines(True),
            "committed " + OUT, "regenerated from " + PROJECT, n=2))
        print("\n%s is STALE: run python tools/gen_manifest.py and commit it." % OUT)
        return 1

    io.open(OUT, "w", encoding="utf-8", newline="\n").write(text)
    print("%s %s (%d Debug / %d Release sources). Review git diff and commit."
          % ("unchanged" if old == text else "wrote", OUT,
             len(D.get("sources", [])), len(R.get("sources", []))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build step: put Nintendall's bundled emulator cores next to the program.

Run automatically by CMake (target "bundle_cores"). Downloads each core once
from the libretro buildbot into a cache folder, then copies it to --dest.

  python3 tools/bundle_cores.py --dest build/cores [--cache cores] [--update]

Keep CORES in sync with src/systems.cpp.
"""
import argparse
import io
import os
import platform
import shutil
import sys
import urllib.request
import zipfile

BUILDBOT = "https://buildbot.libretro.com/nightly"

# One core per system. All licenses allow commercial distribution.
CORES = [
    "mesen",             # NES, Famicom Disk System  (GPL-3.0)
    "bsnes",             # SNES                      (GPL-3.0)
    "mupen64plus_next",  # N64                       (GPL-2.0)
    "sameboy",           # Game Boy / Color          (MIT)
    "mgba",              # Game Boy Advance          (MPL-2.0)
    "melondsds",         # Nintendo DS               (GPL-3.0+)
    "dolphin",           # GameCube / Wii            (GPL-2.0+)
    "azahar",            # 3DS                       (GPL-3.0+)
    "cemu",              # Wii U                     (MPL-2.0)
]


PLATFORMS = {
    "windows": ("windows/x86_64", ".dll"),
    "linux-x64": ("linux/x86_64", ".so"),
    "linux-arm64": ("linux/aarch64", ".so"),
    "macos-x64": ("apple/osx/x86_64", ".dylib"),
    "macos-arm64": ("apple/osx/arm64", ".dylib"),
}


def platform_info(name="auto"):
    if name != "auto":
        return PLATFORMS[name]
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Windows":
        return "windows/x86_64", ".dll"
    if system == "Darwin":
        return ("apple/osx/arm64" if machine in ("arm64", "aarch64") else "apple/osx/x86_64"), ".dylib"
    if machine in ("aarch64", "arm64"):
        return "linux/aarch64", ".so"
    return "linux/x86_64", ".so"


def download(core, filename, plat, cache):
    url = f"{BUILDBOT}/{plat}/latest/{filename}.zip"
    print(f"  downloading {url}")
    with urllib.request.urlopen(url, timeout=300) as r:
        data = r.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        # Only take the one library we expect; ignore any other paths in the zip.
        member = next((n for n in z.namelist() if os.path.basename(n) == filename), None)
        if member is None:
            raise RuntimeError(f"{filename} not found in download")
        os.makedirs(cache, exist_ok=True)
        tmp = os.path.join(cache, filename + ".part")
        with open(tmp, "wb") as f:
            f.write(z.read(member))
        os.replace(tmp, os.path.join(cache, filename))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", required=True, help="cores folder next to the program")
    ap.add_argument("--cache", default=os.path.join(root, "cores"), help="download cache")
    ap.add_argument("--update", action="store_true", help="re-download the latest cores")
    ap.add_argument("--platform", default="auto", choices=["auto", *PLATFORMS],
                    help="which OS to fetch cores for (default: this one)")
    args = ap.parse_args()

    plat, ext = platform_info(args.platform)
    failed = []
    for core in CORES:
        filename = f"{core}_libretro{ext}"
        cached = os.path.join(args.cache, filename)
        try:
            if args.update or not os.path.isfile(cached):
                download(core, filename, plat, args.cache)
            dest = os.path.join(args.dest, filename)
            if os.path.abspath(dest) != os.path.abspath(cached) and (
                    not os.path.isfile(dest) or os.path.getmtime(dest) < os.path.getmtime(cached)):
                os.makedirs(args.dest, exist_ok=True)
                shutil.copy2(cached, dest)
        except Exception as e:  # noqa: BLE001 - report every failure at the end
            failed.append(f"{core}: {e}")
    if failed:
        print("Failed to bundle cores:\n  " + "\n  ".join(failed), file=sys.stderr)
        return 1
    print(f"Bundled {len(CORES)} emulator cores in {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build step: put Nintendall's bundled emulator cores next to the program.

Run automatically by CMake (target "bundle_cores"). Downloads each core once
from the libretro buildbot into a cache folder, then copies it to --dest.
Cores we had to fix (PATCHED_CORES) come from our own builds instead; fetching
those needs the GitHub CLI (`gh`) logged in to an account that can see the repo.

  python3 tools/bundle_cores.py --dest build/cores [--cache cores] [--update]

Keep CORES in sync with src/systems.cpp.
"""
import argparse
import io
import os
import platform
import shutil
import subprocess
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


# Cores Nintendall ships with fixes of its own, built by GitHub Actions in the repo below
# and attached to a release there. The changed source travels with Nintendall (see
# third_party/), as the MPL-2.0 requires.
#   cemu: VPADRead didn't report fresh button presses (trig), so most game menus ignored buttons.
#   dolphin: the widescreen hack only stretched the picture (see third_party/dolphin/).
PATCHED_CORES = {
    "cemu": {"repo": "exultantliving0/cemu-libretro-nintendall", "tag": "nintendall-1",
             "platforms": {"linux/x86_64", "windows/x86_64"}},
    "dolphin": {"repo": "exultantliving0/dolphin-libretro-nintendall", "tag": "nintendall-1",
                "platforms": {"linux/x86_64", "windows/x86_64"}},
}


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


# Data files some cores need next to them (read-only), fetched from the core's source repo.
# Dolphin (GameCube/Wii) needs its "Sys" folder: game fixes, fonts, DSP code.
SYSTEM_DATA = [
    {"name": "dolphin-emu/Sys", "repo": "https://github.com/libretro/dolphin.git", "path": "Data/Sys"},
]


def fetch_system_data(cache, dest, update):
    for item in SYSTEM_DATA:
        cached = os.path.join(cache, "system", item["name"])
        if update or not os.path.isdir(cached):
            print(f"  fetching {item['path']} from {item['repo']}")
            work = os.path.join(cache, "system", "_checkout")
            shutil.rmtree(work, ignore_errors=True)
            run = lambda *a: subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            run("git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", item["repo"], work)
            run("git", "-C", work, "sparse-checkout", "set", item["path"])
            shutil.rmtree(cached, ignore_errors=True)
            os.makedirs(os.path.dirname(cached), exist_ok=True)
            shutil.move(os.path.join(work, item["path"]), cached)
            shutil.rmtree(work, ignore_errors=True)
        target = os.path.join(dest, item["name"])
        if not os.path.isdir(target) or update:
            shutil.rmtree(target, ignore_errors=True)
            shutil.copytree(cached, target)


def download_patched(core, filename, plat, cache):
    info = PATCHED_CORES[core]
    folder = os.path.join(cache, "patched", info["tag"])
    os.makedirs(folder, exist_ok=True)
    print(f"  downloading {filename} from {info['repo']} release {info['tag']}")
    subprocess.run(["gh", "release", "download", info["tag"], "-R", info["repo"], "-p", filename, "-D", folder,
                    "--clobber"], check=True, stdout=subprocess.DEVNULL)
    return os.path.join(folder, filename)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", required=True, help="cores folder next to the program")
    ap.add_argument("--cache", default=os.path.join(root, "cores"), help="download cache")
    ap.add_argument("--update", action="store_true", help="re-download the latest cores")
    ap.add_argument("--system-dest", help="where to put the cores' bundled data files (e.g. build/system)")
    ap.add_argument("--platform", default="auto", choices=["auto", *PLATFORMS],
                    help="which OS to fetch cores for (default: this one)")
    args = ap.parse_args()

    plat, ext = platform_info(args.platform)
    failed = []
    for core in CORES:
        filename = f"{core}_libretro{ext}"
        cached = os.path.join(args.cache, filename)
        try:
            patched = PATCHED_CORES.get(core)
            if patched and plat in patched["platforms"]:
                cached = os.path.join(args.cache, "patched", patched["tag"], filename)
                if args.update or not os.path.isfile(cached):
                    cached = download_patched(core, filename, plat, args.cache)
            elif args.update or not os.path.isfile(cached):
                download(core, filename, plat, args.cache)
            dest = os.path.join(args.dest, filename)
            if os.path.abspath(dest) != os.path.abspath(cached) and (
                    not os.path.isfile(dest) or os.path.getmtime(dest) < os.path.getmtime(cached)):
                os.makedirs(args.dest, exist_ok=True)
                shutil.copy2(cached, dest)
        except Exception as e:  # noqa: BLE001 - report every failure at the end
            failed.append(f"{core}: {e}")
    if args.system_dest:
        try:
            fetch_system_data(args.cache, args.system_dest, args.update)
        except Exception as e:  # noqa: BLE001
            failed.append(f"system data: {e}")
    if failed:
        print("Failed to bundle cores:\n  " + "\n  ".join(failed), file=sys.stderr)
        return 1
    print(f"Bundled {len(CORES)} emulator cores in {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

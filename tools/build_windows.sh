#!/usr/bin/env bash
# Build the Windows version of Nintendall and its setup.exe, from Linux.
#
# Needs (Debian/Ubuntu): sudo apt install mingw-w64 libz-mingw-w64-dev nsis cmake python3 curl 7zip
# Output: dist/Nintendall-<version>-setup.exe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDL_VERSION=2.32.10
SDL_DIR="$ROOT/third_party/win/SDL2-$SDL_VERSION"
TTF_VERSION=2.24.0
TTF_DIR="$ROOT/third_party/win/SDL2_ttf-$TTF_VERSION"
BUILD="$ROOT/build-windows"
STAGE="$ROOT/dist/windows/Nintendall"
VERSION="$(sed -n 's/^project(Nintendall VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

# 1. SDL2 and SDL2_ttf for MinGW (official releases from libsdl.org's GitHub)
if [ ! -d "$SDL_DIR" ]; then
  echo "Downloading SDL2 $SDL_VERSION for Windows..."
  mkdir -p "$ROOT/third_party/win"
  curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz" \
    | tar xz -C "$ROOT/third_party/win"
fi

if [ ! -d "$TTF_DIR" ]; then
  echo "Downloading SDL2_ttf $TTF_VERSION for Windows..."
  curl -fsSL "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VERSION/SDL2_ttf-devel-$TTF_VERSION-mingw.tar.gz" \
    | tar xz -C "$ROOT/third_party/win"
fi

# 1b. SDL2.dll with Nintendall's fix (third_party/sdl/): built once from SDL's source release and
#     put in place of the official DLL (same version, so the headers and import library match).
SDL_PATCH="$ROOT/third_party/sdl/switch-bluetooth-idle.patch"
SDL_STAMP="$SDL_DIR/.nintendall-patched-$(sha256sum "$SDL_PATCH" | cut -c1-12)"
if [ ! -f "$SDL_STAMP" ]; then
  echo "Building SDL2 $SDL_VERSION with Nintendall's fix..."
  SDL_SRC="$ROOT/third_party/win/SDL2-src-$SDL_VERSION"
  rm -rf "$SDL_SRC" && mkdir -p "$SDL_SRC"
  curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" \
    | tar xz -C "$SDL_SRC" --strip-components=1
  patch -d "$SDL_SRC" -p1 < "$SDL_PATCH"
  cmake -S "$SDL_SRC" -B "$SDL_SRC/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw64.cmake" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > /dev/null
  cmake --build "$SDL_SRC/build" -j"$(nproc)" > /dev/null
  x86_64-w64-mingw32-strip "$SDL_SRC/build/SDL2.dll"
  cp "$SDL_SRC/build/SDL2.dll" "$SDL_DIR/x86_64-w64-mingw32/bin/SDL2.dll"
  rm -f "$SDL_DIR"/.nintendall-patched-*
  touch "$SDL_STAMP"
fi

# 2. Compile and bundle the Windows emulator cores
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw64.cmake"
cmake --build "$BUILD" -j"$(nproc)"

# 3. Stage the files that go into the installer
rm -rf "$STAGE"
cmake --install "$BUILD" --prefix "$STAGE"
x86_64-w64-mingw32-strip "$STAGE/nintendall.exe"

# 4. Make the installer
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/Nintendall-$VERSION-setup.exe"
makensis -V2 -DVERSION="$VERSION" -DSRCDIR="$STAGE" -DOUTFILE="$OUT" "$ROOT/installer/nintendall.nsi"
echo "Built $OUT"

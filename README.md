# Nintendall

One app that plays games from many Nintendo systems. Pick (or drag in) any game
file, and Nintendall works out which system it's for and starts the right
emulator automatically. Everything comes in one package: there is nothing to
install or choose.

Under the hood it's a **libretro frontend**. The emulation is done by one proven,
open-source emulator "core" per system, bundled in `cores/` next to the program.
Nintendall provides the app around them: game detection, the library screen,
controller support (via SDL), saves and save states.

## Systems

| System | Bundled emulator | Status |
|---|---|---|
| NES / Famicom | Mesen | **plays** |
| Famicom Disk System | Mesen | plays (needs `disksys.rom` in the system folder) |
| SNES | bsnes | runs (tested with test files only) |
| Game Boy / Game Boy Color | SameBoy | runs (tested with test files only) |
| Game Boy Advance | mGBA | runs (tested with test files only) |
| Nintendo DS | melonDS DS | included, not yet tested with a real game |
| Nintendo 64 | Mupen64Plus-Next | coming soon: needs GPU rendering |
| GameCube / Wii | Dolphin | coming soon: needs GPU rendering |
| 3DS | Azahar | coming soon: needs GPU rendering |
| Wii U | Cemu | coming soon: needs GPU rendering |

## How auto-detection works

`src/detect.cpp` looks at the **contents** of the file first, and only falls back
to the file extension when the contents aren't conclusive. That means renamed or
misnamed games still work.

- NES: `NES\x1A` header; FDS: `FDS\x1A` / `*NINTENDO-HVC*`
- SNES: scores the internal header at the LoROM / HiROM / ExHiROM spots (checksum, map mode, reset vector)
- N64: the 3 byte orders (`.z64`, `.v64`, `.n64`)
- Game Boy / Color: Nintendo logo and header checksum; the CGB flag separates GB from GBC
- GBA: logo and fixed byte at `0xB2`; DS: logo CRC `0xCF56`
- GameCube / Wii: disc magic numbers, plus RVZ, WIA, GCZ, WBFS, CISO, TGC, WAD containers
- 3DS: NCSD, NCCH, CIA, 3DSX; Wii U: WUD, WUX, RPX, WUHB, and unpacked game folders
- `.zip` files are opened and the game inside is detected

Try it from the command line:

```
./build/nintendall --detect path/to/games/*
```

## Building

Needs CMake, a C++17 compiler, Python 3, SDL2 and zlib.

```
# Debian/Ubuntu
sudo apt install build-essential cmake python3 libsdl2-dev zlib1g-dev
# macOS
brew install cmake sdl2
```

```
cmake -S . -B build
cmake --build build -j                          # also bundles all emulators into build/cores
./build/nintendall                              # open the library
./build/nintendall "My Game.nes"                # or play a game directly
cmake --install build --prefix dist/Nintendall  # make the shippable folder
```

The build downloads each emulator once (from the official libretro buildbot)
into `cores/` in the project, then copies them next to the program. To pull the
newest versions: `python3 tools/bundle_cores.py --dest build/cores --update`.

## Controls

Any controller SDL supports works (Xbox, PlayStation, Switch Pro, 8BitDo, most
USB pads), and you can plug controllers in or out at any time. Buttons are mapped
by **position**, so the bottom face button is always the bottom button.
Player 1 is the keyboard plus the first controller, player 2 the second
controller, and so on. Extra mappings can go in `gamecontrollerdb.txt` next to
the program.

| | Keyboard | Controller |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick (retro systems) |
| A / B / X / Y | X / Z / S / A | Right / bottom / top / left face buttons |
| L / R, L2 / R2 | Q / W, 1 / 3 | Shoulders / triggers |
| Start / Select | Enter / Right Shift | Start / Back |
| Menu | Esc | Guide button, or Select + Start |
| Save / load state | F5 / F9 (F6 / F7 change slot) | from the menu |
| Fast forward | hold Tab (Ctrl+Space toggles) | |
| Screenshot | F12 | |
| Fullscreen | F11 or Alt+Enter | |

You can also drag a game file onto the window from anywhere.

## Where files go

User data lives in `~/.local/share/Nintendall/` (Linux),
`~/Library/Application Support/Nintendall/` (macOS) or `%APPDATA%\Nintendall\`
(Windows). Put a file called `portable.txt` next to the program to keep everything
beside it instead.

- `saves/`: in-game battery saves (`.srm`)
- `states/`: save states
- `system/`: BIOS / firmware files that some cores need
- `config/`: per-core settings (`<core>.opt`, editable)
- `screenshots/`
- `nintendall.cfg`: app settings

## Code layout

| File | What it does |
|---|---|
| `src/detect.*` | works out the system from a file / zip / folder |
| `src/systems.*` | the list of systems, extensions and the emulator for each |
| `src/core.*` | loads a libretro core and implements the libretro API |
| `src/app.*` | library screen, pause menu, game loop, saves |
| `src/video.*`, `src/audio.*`, `src/input.*` | SDL window, sound (with rate control) and controllers |
| `src/zip.*`, `src/png.*`, `src/font.*` | zip reading, screenshots, built-in UI font |
| `tools/bundle_cores.py` | build step that bundles the emulator cores |
| `tests/resampler_test.cpp` | checks the audio resampler at every core's sample rate |
| `tests/make_test_files.py` | builds a tiny NES test program plus fake headers for every system |

## Licensing (important if you sell this)

Not legal advice. Have a lawyer review this before selling.

- All 9 bundled emulators have licenses that **allow commercial use** (GPL, MPL
  or MIT). See `THIRD_PARTY_LICENSES.md`, which ships in the package.
- Most of them are **GPL**. You can sell GPL software, but because the GPL cores
  ship inside Nintendall you must make their exact source code available
  (keep a copy of each core's source for every release). The safest reading is
  that Nintendall itself then needs a GPL-compatible license.
- `third_party/libretro/libretro.h` is MIT licensed.
- Never bundle games, BIOS files or console keys. Users must provide their own.
- "Nintendo" is a trademark, so a commercial product shouldn't use a name or logo
  that suggests it's affiliated with Nintendo.

## Testing

```
python3 tests/make_test_files.py /tmp/nt
./build/nintendall --detect /tmp/nt/*
./build/nintendall /tmp/nt/test.nes --frames 120 --screenshot shot.png            # no window
./build/nintendall /tmp/nt/test.nes --frames 120 --hold a --screenshot shot_a.png # A held: red background
g++ -O2 -std=c++17 -Isrc tests/resampler_test.cpp src/audio.cpp $(pkg-config --cflags --libs sdl2) -o rt && ./rt
```

# Nintendall

One app that plays games from many Nintendo systems. Drag a game file onto the
window (or use File > Open) and Nintendall works out which system it's for and
starts the right emulator automatically. Everything comes in one package: there
is nothing to install or choose.

The window is deliberately simple: a menu bar (File, Emulation, State, Options,
Help), in the style of classic emulators, over the game.

## Home screen

When no game is running, Nintendall shows your games. Across the top are the
consoles in release order (NES, Game Boy, SNES, N64, Game Boy Color, Game Boy
Advance, GameCube, DS, Wii, 3DS, Wii U); click one and its games appear below
with their box covers. Click a game to play it. With the keyboard or a
controller: arrow keys / D-pad choose a game, Enter / A plays it, Tab /
Shift+Tab or L / R switch consoles. **Ctrl +** and **Ctrl -** make the covers
bigger or smaller (remembered), **Ctrl 0** puts them back to normal.

The games come from the **Games folder**, which has one folder per console:

```
Documents/Nintendall/Games/
  NES/  Game Boy/  SNES/  Nintendo 64/  Game Boy Color/  Game Boy Advance/
  GameCube/  Nintendo DS/  Wii/  Nintendo 3DS/  Wii U/
```

The installer makes it (and can open it at the end); Nintendall also makes any
missing folders when it starts. **File > Open Games Folder** or the **Open
Folder** button go there. Games can be in subfolders, zipped (`.zip`), and
Famicom Disk System games go in `NES/`. New games show up by themselves.
To use another folder, set `games.folder = "D:/My Games"` in `nintendall.cfg`
(and `covers.folder` for the Box Covers folder; portable mode uses `Games/` and
`Box Covers/` next to the program).

**Box covers** live in the **Box Covers** folder next to the Games folder
(`Documents/Nintendall/Box Covers/<console>/`), named like the game file:
`Box Covers/NES/Super Mario Bros. (World).png` is the cover of
`Games/NES/Super Mario Bros. (World).nes`. To use your own cover, put a `.png`
or `.jpg` with the game's name there (replacing the downloaded one if there is
one); it shows up within a few seconds. Delete a cover to get the downloaded
one back. **File > Open Box Covers Folder** goes there.

Covers are drawn at exactly the size they're shown at, so they stay sharp at
every cover size (Ctrl + / Ctrl -).

When a game has no picture there yet, Nintendall downloads its **USA** cover
from the [libretro thumbnail library](https://thumbnails.libretro.com) and
saves it in that folder. A picture that's already there is never replaced.
Covers are found by matching the game's name (its file name, or the name inside
GameCube / Wii discs and Wii U games) to the library's list of game names:
No-Intro names like `Super Mario Bros. 3 (USA).nes` match exactly, and close
names like `Pokemon Red.gb` usually match too. Only USA releases are used
(`(USA)`, `(USA, Europe)`, `(Japan, USA)`...); a game with no USA cover there
gets a plain box with its title (Nintendall looks again a week later). Only the
game's name is sent, to that one site.

Under the hood it's a **libretro frontend**. The emulation is done by one proven,
open-source emulator "core" per system, bundled in `cores/` next to the program.
Nintendall provides the app around them: game detection, the window and menus,
controller support (via SDL), saves and save states.

## Systems

| System | Bundled emulator | Status |
|---|---|---|
| NES / Famicom | Mesen | **plays** |
| Famicom Disk System | Mesen | plays (needs `disksys.rom` in the system folder) |
| SNES | bsnes | runs (tested with test files only) |
| Game Boy / Game Boy Color | SameBoy | runs (tested with test files only) |
| Game Boy Advance | mGBA | runs (tested with test files only) |
| Nintendo DS | melonDS DS | runs (tested with homebrew) |
| Nintendo 64 | Mupen64Plus-Next | runs, drawn on the GPU (tested with homebrew test ROMs) |
| GameCube / Wii | Dolphin | runs, drawn on the GPU (tested with homebrew) |
| 3DS | Azahar | runs, drawn on the GPU (tested with homebrew; needs decrypted games) |
| Wii U | Cemu (with a GamePad input fix, see `third_party/cemu/`) | runs, drawn on the GPU (tested with homebrew; disc images need your own keys) |

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

Needs CMake, a C++17 compiler, Python 3, SDL2, SDL2_ttf and zlib.

```
# Debian/Ubuntu
sudo apt install build-essential cmake python3 libsdl2-dev libsdl2-ttf-dev zlib1g-dev
# macOS
brew install cmake sdl2 sdl2_ttf
```

```
cmake -S . -B build
cmake --build build -j                          # also bundles all emulators into build/cores
./build/nintendall                              # open the window
./build/nintendall "My Game.nes"                # or play a game directly
cmake --install build --prefix dist/Nintendall  # make the shippable folder
```

The build downloads each emulator once (from the official libretro buildbot)
into `cores/` in the project, then copies them next to the program. To pull the
newest versions: `python3 tools/bundle_cores.py --dest build/cores --update`.

## Windows installer

`tools/build_windows.sh` cross-compiles Nintendall for 64-bit Windows from
Linux and builds `dist/Nintendall-<version>-setup.exe` (NSIS). The installer
puts Nintendall in Program Files with all 9 emulators, adds Start Menu (and
optionally desktop) shortcuts and an Apps & features entry. Uninstalling keeps
the user's saves in `%APPDATA%\Nintendall`.

```
sudo apt install mingw-w64 libz-mingw-w64-dev nsis
./tools/build_windows.sh
```

The program only needs `SDL2.dll` and `SDL2_ttf.dll` (shipped beside it); the compiler runtime and
zlib are built in. The setup.exe isn't code-signed yet, so Windows SmartScreen
will warn on first run. Sign it before selling.

## Controls

Any controller SDL supports works (Xbox, PlayStation, Switch Pro, 8BitDo, most
USB pads), and you can plug controllers in or out at any time. Controllers SDL
doesn't know (e.g. some USB NES pads) still work as **Detected Controller**,
with a guessed button layout you can fix in Button Config. Player 1 is the
first controller, player 2 the second, and so on, unless you pick a controller
for a player from the **Controller** drop-down in **Options > Button Config**
(remembered, and found again when it's plugged back in; "None" makes a player
keyboard-only). All keys and controller buttons can be changed there too (per
player), including the analog stick directions: map them to another stick, a
trigger or a button (by default they're the real sticks). Only the player's own
controller can set its buttons. Extra
controller mappings can go in `gamecontrollerdb.txt` next to the program.

Default keyboard (player 1): arrows = D-pad, X / Z = A / B, S / A = X / Y,
Q / W = L / R, 1 / 3 = L2 / R2, Enter = Start, Right Shift = Select.

| Shortcut | Does |
|---|---|
| Ctrl+O | Open a game |
| Ctrl+W | Close the game |
| Ctrl+P (or Pause key) | Pause / resume |
| Ctrl+R | Reset |
| F5 / F9 | Save / load state |
| F6 / F7 | Previous / next state slot |
| Hold Tab | Fast forward |
| F11 or Alt+Enter | Fullscreen (Esc leaves fullscreen) |
| F12 | Screenshot |
| F10 | Open the menu bar with the keyboard |
| Controller Home, or Select + Start | Pause / resume |

The game pauses automatically when the window loses focus or a menu is open.

### Console Settings

**Options > Console Settings** shows hand-picked settings from the emulator of
the game that's running, with friendly names. Settings marked * apply after
**Restart Game** (in the same window); the rest change right away. Highlights:

| System | Settings |
|---|---|
| N64 | Resolution upscaling (to 4K), widescreen hack, MSAA / FXAA, texture filters and HD texture enhancement (xBRZ, HQ4x), Expansion Pak, Rumble / Controller Pak, stick sensitivity |
| GameCube / Wii | Resolution upscaling (to 8x), widescreen hack, aspect ratio, anti-aliasing, texture filtering, CPU speed, language, rumble |
| Wii U | Graphics API (Vulkan / OpenGL), Resolution (from the game's own resolution graphic pack, like standalone Cemu, so shadows and effects stay right; shown for the ~50 games that have one), smoother frame rate, accurate shader math, Clear Shader Cache, and more. Without a pack, games draw at their native resolution (Cemu's generic resize broke shadows) |
| 3DS | Resolution upscaling (to 10x), texture filters (Anime4K, ScaleForce, xBRZ), stereoscopic 3D, New / Old 3DS, region, language, CPU speed |
| DS | OpenGL renderer with 3D upscaling (to 8x), gap between screens, stylus cursor, sound smoothing |
| SNES | HD Mode 7, run-ahead (less input lag), CPU and Super FX overclock, TV filters, no sprite limit |
| NES / FDS | Palettes, TV filters, no sprite limit, hide screen edges, overclock, region, fast disk loading |
| Game Boy / Color | Palettes, Super Game Boy mode with borders, color correction, rumble |
| GBA | LCD ghosting, color correction, solar sensor, skip BIOS intro |

The list lives in `src/consolesettings.cpp`; values are saved per emulator in
the `config/` folder.

### Nintendo DS screens

The **Layout** menu arranges the two DS screens: Vertical (like a real DS),
Side by Side, Large + Small Screen, Single Screen, **Separate Windows**, and
two **Sideways** layouts for games played with the DS held like a book (e.g.
Ninja Gaiden: Dragon Sword). **Swap Screens** switches which screen comes
first. The window resizes to fit, and the layout is remembered for each game.

**Separate Windows** puts the top screen in the main window and the bottom
screen in a second window next to it (Swap Screens trades them), so each can
be moved, resized, or put on its own monitor. Closing the second window goes
back to Vertical. Wii U has it too: the TV picture in the main window and the
GamePad in the second one, each at 16:9.

The mouse is the stylus: click and drag on the touch screen, in any layout
(and in either window).

### Nintendo 3DS

3DS games are drawn on the graphics card with OpenGL 4.3 (most PCs from 2013
on). If a PC can't provide that, Nintendall automatically falls back to the
much slower software renderer. The Layout menu has Vertical, Side by Side,
Large + Small Screen, Single Screen, Separate Windows and Swap Screens for 3DS
(no sideways layouts). Controls: the Circle Pad is a controller's left stick or I / J / K / L
on the keyboard, the C-Stick is the right stick, ZL / ZR are the triggers (1 / 3),
and the mouse is the stylus. Games must be decrypted dumps (.3ds / .cci / .cxi / .3dsx).

### Nintendo 64

N64 games are drawn on the graphics card (GLideN64, OpenGL 3.3 or newer). On
PCs without it, Nintendall falls back to the software renderer (Angrylion).
Controls: N64 A / B are the bottom / left face buttons (keyboard Z / A), Z is the
left trigger (keyboard 1), the Control Stick is the left stick or I / J / K / L,
and the C buttons are the right stick or T / F / G / H. Up to 4 players.

### GameCube and Wii

Both run on Dolphin (OpenGL 3.3+). Nintendall ships Dolphin's `Sys` data folder
in `system/dolphin-emu/Sys` (fetched from Dolphin's source by the build).
GameCube A is the bottom face button (B left, X right, Y top), Z the right bumper,
L / R the triggers, plus Control Stick and C-Stick. Wii defaults to
Wiimote + Nunchuk; Console Settings switches to sideways Wiimote, Wiimote alone or
Classic Controller, and Button Config follows. The Wiimote pointer is the right
stick, or the mouse (Console Settings > Wiimote Pointer). Only players who have a
controller (or keys) are plugged in, so Wii games don't show extra pointers.

**Widescreen:** Console Settings > **Widescreen** shows more of the 3D world at the
sides: **16:9**, or **Ultrawide 21:9** for ultrawide monitors (Dolphin's widescreen
hack; menus and 2D screens may still stretch, and some games glitch). Most Wii
games are already 16:9 on their own. Nintendall ships Dolphin with a fix so the
hack truly widens the view instead of stretching the picture (see
`third_party/dolphin/`).

**Real Wii Remotes:** set Console Settings > Wii Controller to **Real Wii Remote**
(the game restarts), then press a button on each remote to connect it. The remotes
must first be connected to the PC:

- **Mayflash DolphinBar** (easiest): plug it in, set it to mode 4, and sync each
  remote with its red button. It's also the sensor bar.
- **Bluetooth:** choose **Options > Pair Wii Remotes** and press the red sync
  button under each remote's battery cover (like Dolphin's Pair button). Paired
  remotes stay paired: later, any button reconnects them. On Windows, pressing
  1 + 2 during a game also connects a remote for that session. (On Linux, pairing
  uses BlueZ's `bluetoothctl`.)

Games' Wii Remote speaker sounds play on the real remotes (Nintendall switches on
Dolphin's speaker setting). Pointing needs a powered sensor bar (the DolphinBar's, or a USB / battery one; a
Wii's own bar only works plugged into a Wii). On Linux, Dolphin also needs
permission to use the remotes; add this as `/etc/udev/rules.d/51-wiimote.rules`
and reconnect them:

```
SUBSYSTEM=="hidraw*", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="0306", TAG+="uaccess"
SUBSYSTEM=="hidraw*", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="0330", TAG+="uaccess"
```

### Wii U

Wii U games run on Cemu (OpenGL 4.5). The Layout menu shows the TV picture
(Single Screen, the default), the GamePad screen (Single Screen + Swap Screens),
or both: Vertical, Side by Side, or Large + Small (GamePad in a corner).
The GamePad buttons sit where Nintendo puts them (A right, B bottom); ZL / ZR are
the triggers, + / - are Start / Select, and the mouse is the GamePad touch screen.
Console Settings has resolution (up to 4K), CPU mode, smoother frame rate,
shader cache and more. Save states aren't possible (Cemu can't save them), so use
the game's own saves.

Disc images (.wud / .wux) are encrypted with keys from your own console: add your
`keys.txt` with **Options > Wii U Keys...** (or put `game.key` next to `game.wux`).
Decrypted games (.wua, unpacked folders with `code/`, `content/`, `meta/`) and
homebrew (.rpx, .wuhb) don't need keys.

Cemu is shipped with one fix: in libretro mode it never told games which GamePad
buttons were *newly* pressed, so most game menus ignored buttons. The patch and
where it applies are in `third_party/cemu/`; `tools/bundle_cores.py` fetches the
patched core from `exultantliving0/cemu-libretro-nintendall` (built by GitHub
Actions there) instead of the libretro buildbot.

### GPU rendering

Cores that draw with OpenGL (3DS, N64, GameCube/Wii, Wii U, and DS with its OpenGL renderer) get
their own OpenGL context on a hidden window and render into an offscreen
framebuffer. Nintendall copies each finished frame into the normal window, so
the window itself can use any renderer (Direct3D on Windows). See `src/hwrender.*`.

**Vulkan:** Wii U games draw with Vulkan by default (Cemu with OpenGL draws parts
of some games black on some drivers, e.g. Intel's); **Graphics API** in the Wii U
Console Settings switches to OpenGL. The other consoles use OpenGL. Nintendall creates the Vulkan device through libretro's
negotiation interface (the core asks for what it needs), gives the core a hidden
window's surface, and copies each frame it hands over (`set_image`) into the
window, shrunk on the GPU like the OpenGL path. If Vulkan can't start on a PC,
the game starts with OpenGL and says so. See `src/vkrender.*`.

## New versions

When Nintendall starts, it asks GitHub (in the background) whether a newer
version has been released. If so, a popup shows what's new, with **Update Now**
(**Download** on Linux / Mac, which opens the release page), **Skip This
Version** (not asked again until an even newer one) and **Later**.

**Update Now** (Windows, installed with the setup): Nintendall downloads the new
setup with a progress bar (Esc cancels), checks it against the SHA-256
fingerprint GitHub publishes for it (a damaged or different file is never run),
then starts it (Windows asks for permission once, as for any install in Program
Files) and closes. The setup waits for Nintendall to close, installs silently
into the same folder and starts the new version, which says
"Nintendall was updated to ...". Silent updates keep your choice of desktop shortcut.

The popup only appears on the home screen right after
launch: if a game is started first (or Nintendall was opened with a game), it
isn't shown that time. Nothing is sent except the request for the release list.
To turn the check off, set `update.check = "false"` in `nintendall.cfg`.

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
- `covers/`: lists of game names for finding covers, and notes of covers that don't exist (the covers themselves are in the Box Covers folder)
- `nintendall.cfg`: settings, button mappings and recent games

Games go in the Games folder (`Documents/Nintendall/Games`, see Home screen).

## Code layout

| File | What it does |
|---|---|
| `src/detect.*` | works out the system from a file / zip / folder |
| `src/systems.*` | the list of systems, extensions and the emulator for each |
| `src/core.*` | loads a libretro core and implements the libretro API |
| `src/hwrender.*` | OpenGL context + framebuffer for GPU-drawn cores (N64, GameCube/Wii, 3DS, Wii U) |
| `src/vkrender.*` | Vulkan device and frame copy for Wii U |
| `src/app.*` | main window, menu actions, game loop, saves |
| `src/home.*`, `src/library.*`, `src/covers.*`, `src/http.*` | home screen, Games folder scanning (and game IDs), box cover downloads |
| `src/update.*`, `src/selfupdate.*`, `src/sha256.*` | checks GitHub for a new version at startup; Update Now (download, check, install, restart) |
| `src/menubar.*`, `src/buttonconfig.*`, `src/text.*`, `src/filedialog.*` | menu bar, Button Config window, font drawing, system Open dialog |
| `src/consolesettings.*`, `src/settingsdialog.*` | the Console Settings list and window |
| `src/video.*`, `src/audio.*`, `src/input.*` | SDL window, sound (with rate control) and controllers |
| `src/zip.*`, `src/png.*` | zip reading, screenshots |
| `tools/bundle_cores.py` | build step that bundles the emulator cores |
| `tools/build_windows.sh`, `installer/nintendall.nsi`, `cmake/toolchain-mingw64.cmake` | Windows build and setup.exe |
| `tools/make_icon.py`, `assets/` | app icon and Windows resources |
| `tests/resampler_test.cpp` | checks the audio resampler at every core's sample rate |
| `tests/make_test_files.py` | builds a tiny NES test program plus fake headers for every system |


## Testing

```
python3 tests/make_test_files.py /tmp/nt
./build/nintendall --detect /tmp/nt/*
./build/nintendall /tmp/nt/test.nes --frames 120 --screenshot shot.png            # no window
./build/nintendall /tmp/nt/test.nes --frames 120 --hold a --screenshot shot_a.png # A held: red background
g++ -O2 -std=c++17 -Isrc tests/resampler_test.cpp src/audio.cpp $(pkg-config --cflags --libs sdl2) -o rt && ./rt
```

# Third-party software in Nintendall

Nintendall ships with the following open-source emulator cores. All are
unmodified, as built by the libretro buildbot (https://buildbot.libretro.com),
except Dolphin and Cemu (see below). Each one
stays under its own license. The full license texts and source code are
available from the projects below.

| System | Core (file) | License | Source code |
|---|---|---|---|
| NES, Famicom Disk System | Mesen (`mesen_libretro`) | GPL-3.0 | https://github.com/libretro/Mesen |
| SNES | bsnes (`bsnes_libretro`) | GPL-3.0 | https://github.com/libretro/bsnes |
| Nintendo 64 | Mupen64Plus-Next (`mupen64plus_next_libretro`) | GPL-2.0 | https://github.com/libretro/mupen64plus-libretro-nx |
| Game Boy / Game Boy Color | SameBoy (`sameboy_libretro`) | MIT | https://github.com/libretro/SameBoy |
| Game Boy Advance | mGBA (`mgba_libretro`) | MPL-2.0 | https://github.com/libretro/mgba |
| Game Boy / Game Boy Color LAN games (link cable) | Gambatte (`gambatte_libretro`) | GPL-2.0 | https://github.com/libretro/gambatte-libretro |
| Game Boy Advance LAN games (link cable) | gpSP (`gpsp_libretro`) | GPL-2.0 | https://github.com/libretro/gpsp |
| Nintendo DS | melonDS DS (`melondsds_libretro`) | GPL-3.0-or-later | https://github.com/JesseTG/melonds-ds |
| GameCube / Wii | Dolphin (`dolphin_libretro`), with changes by Nintendall | GPL-2.0-or-later | https://github.com/libretro/dolphin + `third_party/dolphin/` |
| Nintendo 3DS | Azahar (`azahar_libretro`) | GPL-3.0-or-later | https://github.com/azahar-emu/azahar |
| Wii U | Cemu (`cemu_libretro`), with a fix by Nintendall | MPL-2.0 | https://github.com/WizzardSK/cemu-libretro + `third_party/cemu/` |

Dolphin is shipped with two changes: a fix so its widescreen hack widens the
picture, and LAN games (Nintendall's Host / Join windows use Dolphin's own
netplay). The changes are in `third_party/dolphin/` (installed next to
Nintendall). It holds the patches and the exact upstream commit they apply to,
which together are the full source of the shipped Dolphin core. LAN games get
host codes from Dolphin's public netplay server (stun.dolphin-emu.org), as the
Dolphin emulator itself does.

Cemu is shipped with one small fix to its GamePad input, so that games see
button presses. The change is in `third_party/cemu/` (installed next to
Nintendall). It holds the patch and the exact upstream commit it applies to,
which together are the full source of the shipped Cemu core under the MPL-2.0.

Dolphin also needs its data folder, shipped as `system/dolphin-emu/Sys`
(GPL-2.0-or-later, from https://github.com/libretro/dolphin, `Data/Sys`).

Also used:

| Component | License | Source |
|---|---|---|
| libretro API headers (`libretro.h`, `libretro_vulkan.h`) | MIT | https://github.com/libretro/libretro-common |
| Vulkan headers (`third_party/vulkan`, Khronos Group) | Apache-2.0 or MIT, see `third_party/vulkan/LICENSE.md` | https://github.com/KhronosGroup/Vulkan-Headers |
| SDL2 (Windows: with a fix for Nintendo controllers over Bluetooth, see `third_party/sdl/`) | zlib | https://libsdl.org |
| SDL2_ttf (font drawing) | zlib | https://github.com/libsdl-org/SDL_ttf |
| FreeType (inside SDL2_ttf) | FreeType License (FTL) | https://freetype.org. Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. |
| HarfBuzz (inside SDL2_ttf) | MIT ("Old MIT") | https://github.com/harfbuzz/harfbuzz |
| DejaVu Sans font (`fonts/DejaVuSans.ttf`) | Bitstream Vera / public domain, see `fonts/DejaVu-LICENSE.txt` | https://dejavu-fonts.github.io |
| zlib | zlib | https://zlib.net |
| stb_image (PNG / JPEG decoding for box covers, `third_party/stb/stb_image.h`) | MIT or public domain (your choice) | https://github.com/nothings/stb |

Box covers are not part of Nintendall: the home screen downloads them on each
user's PC from the libretro thumbnail library (https://thumbnails.libretro.com).

## Source code offer

For the GPL-licensed cores above, the exact source code for the shipped
binaries can be requested from the Nintendall distributor for at least three
years after you received this copy. (Before selling Nintendall, the
distributor should keep a copy of each core's source matching the shipped
build so this offer can be honoured.)

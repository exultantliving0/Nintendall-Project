# Dolphin (GameCube / Wii) - Nintendall's change

Nintendall's GameCube / Wii emulator is the libretro port of Dolphin, licensed
under the GNU General Public License v2 or later (GPL-2.0-or-later). Nintendall
ships it with two changes.

- Original source: https://github.com/libretro/dolphin
  (branch `master`, commit `32c61f9d0e933331bb7087ba57e96883241b4960`)
- The changes: `widescreen-hack.patch` and `lan.patch` in this folder. Apply
  them in that order to the commit above (`git apply widescreen-hack.patch`,
  then `git apply lan.patch`) to get the exact source of the core Nintendall
  ships. Every other file is unchanged.

**LAN games** (`lan.patch`: new `Source/Core/DolphinLibretro/Lan.cpp` / `Lan.h`,
hooks in `Boot.cpp`, `Main.cpp`, `Input.cpp` and `CMakeLists.txt`): lets Nintendall host or
join a game with Dolphin's own netplay code, which the libretro core already
contains but had no way to start. After `nintendall_lan_setup`, loading a game
opens a netplay session instead of booting it, and the game boots with the
session's settings and save data when the host starts it, as in Dolphin's own
netplay window. Netplay only sends emulated Wii Remotes, so with Real Wii Remote
chosen, a LAN game hands the real remotes to Dolphin's controller interface
(Dolphin's "Connect Wii Remotes for Emulated Controllers") and maps the player's
emulated Wii Remote to the real one (`UseRealWiimoteInLan` in `Input.cpp`).

**Widescreen hack** (`widescreen-hack.patch`, `Source/Core/DolphinLibretro/Video.cpp`).
What it fixes: the widescreen hack didn't widen anything. The hack widens
the 3D view to the shape of the picture buffer, and in the libretro core that
buffer was always 640 pixels wide (about 4:3), while the frontend was told the
picture is 16:9. With the hack on, the buffer is now as wide as the shape the
hack draws for: 16:9 (or the custom aspect ratio from Dolphin's `GFX.ini` when
Aspect Ratio is set to Custom, which Nintendall doesn't offer).

The GPL text: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

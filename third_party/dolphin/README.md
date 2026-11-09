# Dolphin (GameCube / Wii) - Nintendall's change

Nintendall's GameCube / Wii emulator is the libretro port of Dolphin, licensed
under the GNU General Public License v2 or later (GPL-2.0-or-later). Nintendall
ships it with one small fix.

- Original source: https://github.com/libretro/dolphin
  (branch `master`, commit `32c61f9d0e933331bb7087ba57e96883241b4960`)
- Changed file: `Source/Core/DolphinLibretro/Video.cpp`
- The change: `widescreen-hack.patch` in this folder. Apply it to the commit
  above with `git apply widescreen-hack.patch` to get the exact source of the
  core Nintendall ships. Every other file is unchanged.

What it fixes: the widescreen hack didn't widen anything. The hack widens
the 3D view to the shape of the picture buffer, and in the libretro core that
buffer was always 640 pixels wide (about 4:3), while the frontend was told the
picture is 16:9. With the hack on, the buffer is now as wide as the shape the
hack draws for: 16:9 (or the custom aspect ratio from Dolphin's `GFX.ini` when
Aspect Ratio is set to Custom, which Nintendall doesn't offer).

The GPL text: https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

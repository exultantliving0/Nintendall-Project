# Cemu (Wii U) - Nintendall's changes

Nintendall's Wii U emulator is the libretro port of Cemu, licensed under the
Mozilla Public License 2.0 (MPL-2.0). Nintendall ships it with two small changes.

- Original source: https://github.com/WizzardSK/cemu-libretro
  (branch `libretro`, commit `6d38ee8d`)
- Changed files: `src/Cafe/OS/libs/vpad/vpad.cpp` and
  `src/libretro/CemuLibretro.cpp`
- The changes: `vpad-fresh-presses.patch` and `updates-dlc-folders.patch` in
  this folder. Apply them in that order to the commit above (`git apply
  vpad-fresh-presses.patch`, then `git apply updates-dlc-folders.patch`) to get
  the exact source of the core Nintendall ships. Every other file is unchanged.

What it fixes: in libretro mode, `VPADRead` only reported which GamePad buttons
were held. It never reported which were newly pressed or released
(`trig` / `release`), or stick directions as buttons. Games that act on a fresh
press, as most menus do, ignored every button. The patch fills these in the
same way Cemu itself does outside libretro mode.

**Updates and DLC** (`updates-dlc-folders.patch`): the core looks for game
updates and DLC only in its own folders. It now also scans the folders listed
in `system/Cemu/game_paths.txt` (one per line), the way Cemu scans its game
paths. Nintendall lists its Wii U games folder there, so an update or DLC put
next to the game is used with it. Without its update, Breath of the Wild stops
with "A system update is required".

The MPL-2.0 text: https://www.mozilla.org/MPL/2.0/

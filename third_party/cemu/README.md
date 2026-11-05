# Cemu (Wii U) - Nintendall's changes

Nintendall's Wii U emulator is the libretro port of Cemu, licensed under the
Mozilla Public License 2.0 (MPL-2.0). Nintendall ships it with one small fix.

- Original source: https://github.com/WizzardSK/cemu-libretro
  (branch `libretro`, commit `6d38ee8d`)
- Changed file: `src/Cafe/OS/libs/vpad/vpad.cpp`
- The change: `vpad-fresh-presses.patch` in this folder. Apply it to the
  commit above with `git apply vpad-fresh-presses.patch` to get the exact
  source of the core Nintendall ships. Every other file is unchanged.

What it fixes: in libretro mode, `VPADRead` only reported which GamePad buttons
were held. It never reported which were newly pressed or released
(`trig` / `release`), or stick directions as buttons. Games that act on a fresh
press, as most menus do, ignored every button. The patch fills these in the
same way Cemu itself does outside libretro mode.

The MPL-2.0 text: https://www.mozilla.org/MPL/2.0/

# SDL2 - Nintendall's change

Nintendall's Windows version ships SDL2 2.32.10 (zlib license) built from SDL's
source release with one small change: `switch-bluetooth-idle.patch` in this
folder (`patch -p1 < switch-bluetooth-idle.patch` in SDL2-2.32.10). The build
does this by itself (`tools/build_windows.sh`).

What it fixes: SDL reads Nintendo controllers (Switch Pro, Joy-Con, and the
Nintendo Switch Online NES / SNES / N64 controllers) with its own driver. Over
Bluetooth they send their state only when a button changes, but SDL took 3
seconds without any input as a lost connection: holding a direction or not
touching the controller for 3 seconds made it disconnect and reconnect a moment
later. With the change, that only happens in the mode where the controller keeps
sending (with motion controls on); a controller that's switched off or out of
range still disconnects, as Windows removes it.

Source: https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10

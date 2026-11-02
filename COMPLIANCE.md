# Compliance

Per-component provenance for `fujinet-go-o2-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-studio2-desktop/COMPLIANCE.md`,
`fujinet-go-sms-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **The console core** (`core/o2/machine.c`, `carts.c`) | transposed to C from MAME: `src/mame/philips/odyssey2.cpp` (the Odyssey 2, Videopac G7000 and Videopac+ G7400 driver) and the cartridge bus `src/devices/bus/odyssey2/{slot,rom,4in1,rally,ktaa}.cpp` | BSD-3-Clause; copyright-holders Peter Trauner, Wilbert Pol, hap (driver) and Fabio Priuli (cart bus), kept in each file's header | compiled into `o2_core` |
| **The 8048** (`core/o2/mcs48.c`, `dasm48.c`) | transposed from MAME `src/devices/cpu/mcs48/mcs48.cpp/.h` and `mcs48dsm.cpp`: the I8048 variant only | BSD-3-Clause; Dan Boris, Mirko Buffoni, Aaron Giles, Couriersud | compiled into `o2_core` |
| **The 8244/8245 video and sound** (`core/o2/i8244.c`) | transposed from MAME `src/devices/video/i8244.cpp/.h` | BSD-3-Clause; Peter Trauner, Wilbert Pol, hap | compiled into `o2_core` |
| **The Videopac+ G7400's text chip and expander** (`core/o2/ef9340.c`, `i8243.c`) | transposed from MAME `src/devices/video/ef9340_1.cpp/.h` and `src/devices/machine/i8243.cpp/.h` | BSD-3-Clause; the MAME copyright-holders named in each file | compiled into `o2_core` |
| **The Voice's speech chip** (`core/o2/sp0256.c`) | transposed from MAME `src/devices/sound/sp0256.cpp/.h` (itself Joseph Zbiciak's jzIntv implementation) and `src/devices/bus/odyssey2/voice.cpp` | BSD-3-Clause; Joseph Zbiciak, Tim Lindner | compiled into `o2_core` |
| **The no-BIOS stand-in** (`core/o2/stub.a48`, `stub.c`) | this repository: about 120 bytes of 8048 code of our own, written from the hardware and the two routine contracts the FujiNet apps use; it contains nothing from the Magnavox or Philips firmware | GPL-3.0-or-later | served at `$000-$3FF` when no BIOS is imported |
| **The FujiNet Odyssey 2 cartridge device** (`core/o2/fujinet_cart.c`) | transposed from the firmware's MAME device, `fujinet-firmware` `pico/o2/emu/fujinet.cpp` | BSD-3-Clause, © Thomas Cherryhomes (the transposition GPL-3.0-or-later) | compiled into `o2_core` |
| **The cartridge firmware's own sources** (`fujimail.c`, `o2map.c`, `o2app.h`, `fuji_mailbox.h`, `fujiconfigrom.h` from `pico/o2/firmware`; `fujibus.c` and `fujitcp.h` from `pico/common`) | `fujinet-firmware`, staged verbatim into `core/o2/fuji-generated/` by `cmake/StageFujiProto.cmake` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled into `o2_core`, never patched |
| **The CONFIG client** (`fujiconfigrom.h`, 25,600 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `o2/`, assembled with the Macroassembler AS and baked into the firmware (PR #2237) | GPL-3.0 (the FujiNet project's) | the image the cartridge serves at power-on, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| The session, debugger and frontends (`core/src`, `core/debugger`, `frontends/{gnome,kde,windows,macos}`) | ported from the Studio II and SMS siblings' (this project's family, © Thomas Cherryhomes) | GPL-3.0-or-later | one executable per platform |
| GTK4, libadwaita (GNOME) / Qt6 (KDE) | the GNOME and Qt projects | LGPL-2.1-or-later / LGPL-3.0 | system libraries, dynamically linked |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it: the combined work
is distributed under GPL-3.0.

## What is not shipped, and why

The **Odyssey 2 / Videopac BIOS** (1 KB of Magnavox/Philips code), the
**Videopac+ G7400 BIOS**, the **8244's and the EF9340's character sets**
(mask ROMs on the video chips), and **The Voice's speech ROMs** are
copyrighted and not freely licensed. This repository does not redistribute
them, and every artifact it publishes is built with `-DWITH_O2_ROMS=OFF`,
which embeds nothing; `core/tests/no_embedded_roms.py` checks the shipped
binaries for slices of each of them.

They are needed less than one might expect: CONFIG and every FujiNet app boot on
this repository's own stand-in BIOS, and **Import BIOS** (a file, a zip such as
MAME's `videopac.zip` / `i8244.zip` / `videopacp.zip`, or a folder) copies the
user's own dumps into the ROM directory, each identified by size and CRC-32
against MAME's tables. Games need the real BIOS; every machine needs its
video chip's character set to show text. `WITH_O2_ROMS=ON` embeds whatever
recognised images are in `tools/roms/` for a developer's own build; such a
build is not for redistribution.

The Chess Module (an NSC800) and the Home Computer Module (a Z80) carry a
second CPU of their own and are not emulated; the cartridge loader names them
and refuses them politely.

## What is changed from MAME

The core is a transposition, not a copy: MAME's C++ device classes become C
structs and functions under MAME's own names (minus `m_`), so the two can be
read side by side. What is deliberately different:

- **The clockwork.** MAME runs the 8048 and the 8244's timers in its
  scheduler; here the machine runs the CPU one instruction at a time and
  first brings the VDC's events up to the CPU's clock, which is what MAME's
  timeslicing comes to with both clocks derived from one crystal. Absolute
  time is counted in pixel clocks (20 per NTSC machine cycle, 18 per PAL
  one); the beam's origin is power-on, as in MAME. The VDC draws lazily, the
  way `screen().update_now()` does, so mid-frame writes land on the right
  pixel; nothing runs per clock.
- **The sound** is kept as a list of level changes with their times and
  box-averaged to 48 kHz once a frame, not streamed at the chip's clock.
- **Memory** is the console's own: 1 KB BIOS (or the stand-in), 128 bytes of
  external RAM, 64 bytes inside the 8048, and a cartridge slot; the cartridge
  is a mapper (standard, 4-in-1, rally, KTAA, The Voice in front) or the
  FujiNet cartridge.
- **The software list** is replaced by a CRC table of the special mappers
  (`core/o2/carts.c`).

`tools/ab/` runs the core against MAME frame for frame.

## What is changed from the firmware's MAME device

`fujinet_cart.c` keeps `pico/o2/emu/fujinet.cpp`'s structure line for line,
with: the console's reads, writes and port writes handed over by the machine
(rather than an installed MAME handler); the debugger's peeks in place of
`side_effects_disabled()`; the mailbox service on a worker thread whenever
the host runs paced (MAME decides this from `video().throttled()`); and the
reconnect MAME's device lacks, which dials the link again before a
transaction when it has dropped.

## Trademarks and the icon

"Magnavox", "Odyssey" and "Philips Videopac" are trademarks of their
owners; they are used here to say which machine this emulates and nothing
more. This project is not affiliated with or endorsed by them. "FujiNet" is
the FujiNet project's. The icon is the family mark on the console's case
silver (`#C3C0BF`); the UI accent is the power-button red (`#C11218`), both
sampled from a photograph of the console kept in `fujinet-ads`.

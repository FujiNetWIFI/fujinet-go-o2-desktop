# Compliance

Per-component provenance for `fujinet-go-o2-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-astrocade-desktop/COMPLIANCE.md`,
`fujinet-go-sms-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **The Odyssey 2 core** (`core/o2/machine.c`, `cpu1802.c`, `cdp1861.c`, `dasm1802.c`) | transposed to C from MAME: `src/mame/rca/o2.cpp`, `src/devices/cpu/cosmac/cosmac.cpp/.h`, `src/devices/cpu/cosmac/cosdasm.cpp`, `src/devices/video/cdp1861.cpp/.h`, `src/devices/sound/beep.cpp` | BSD-3-Clause; copyright-holders Curt Coder (o2, cosmac, cosdasm, cdp1861) and Kevin Thacker (beep), kept in each file's header | compiled into `o2_core` |
| **The no-BIOS stand-in** (`core/o2/stub.asm`, `stub.c`) | this repository: 92 bytes of 1802 code of our own | GPL-3.0-or-later | served at `$0000` when no BIOS is imported |
| **The FujiNet Odyssey 2 cartridge device** (`core/o2/fujinet_cart.c`) | transposed from the firmware's MAME device, `fujinet-firmware` `pico/o2/emu/o2_fujinet.cpp` | BSD-3-Clause, © Thomas Cherryhomes (the transposition GPL-3.0-or-later) | compiled into `o2_core` |
| **The cartridge firmware's own sources** (`s2map.c`, `fuji_load.c`, `s2_text.c`, `s2_font.c`, `s2_cart.h`, `fuji_mailbox.h` and headers from `pico/o2/firmware`; `fujimail.c`, `fujibus.c`, the shared mailbox headers and `fujitcp.h` from `pico/common`) | `fujinet-firmware` master, staged verbatim into `core/o2/fuji-generated/` by `cmake/StageFujiProto.cmake` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled into `o2_core`, never patched |
| **The CONFIG client** (`fujiconfigrom.h`, 6 656 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `o2/`, assembled with the Macroassembler AS, converted by `pico/o2/tools/mkromh.py`, staged from `pico/o2/firmware/baked/` | GPL-3.0 (the FujiNet project's) | the image the cartridge serves at power-on, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| The session, debugger and frontends (`core/src`, `core/debugger`, `frontends/{gnome,kde,windows,macos}`) | ported from the SMS sibling's (this project's family, © Thomas Cherryhomes) | GPL-3.0-or-later | one executable per platform |
| The tests' 1802 programs (`core/tests/roms/*.asm`) | this repository | GPL-3.0-or-later | assembled into `core/tests/test_rom.h`; tests only |
| GTK4, libadwaita (GNOME) / Qt6 (KDE) | the GNOME and Qt projects | LGPL-2.1-or-later / LGPL-3.0 | system libraries, dynamically linked |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it: the combined work
is distributed under GPL-3.0.

## What is changed from MAME

The core is a transposition, not a copy: MAME's C++ device classes become C
structs and functions under MAME's own names (minus `m_`), so the two can be
read side by side. What is deliberately different:

- **The clockwork.** MAME runs the CPU and the 1861's three emu_timers in its
  scheduler; here the machine runs the CPU one state at a time (8 clocks)
  and first fires every 1861 timer due at or before that state's start,
  which is exactly what MAME's timeslicing comes to with both chips on the
  same 1.76 MHz clock. `screen().time_until_pos()` and the beam's origin
  (line 0, the start of VBLANK, at power-on) are transposed exactly.
- **Power-on.** MAME's CPU never runs its CLEAR reset at power-on (its first
  pass is INIT straight from the constructor, so IE stays 0); the console's
  reset circuit holds CLEAR low, giving IE = 1 and Q = 0, which is what this
  core does. `tools/ab/mame_ab.py --mame-power-on` (`o2_headless -mame`)
  reproduces MAME's way for comparisons; the firmware's own Tier B reference
  pulses CLEAR the same way this core does.
- **The CPU** is the CDP1802 subset only: no 1801 opcode map, no 1804/5/6
  counter or extended opcodes.
- **Left out**: the Visicom, the MPT-02 family and the other clones, the
  plain ROM cartridge slot (the FujiNet cartridge is always in the slot) and
  the software list.

`tools/ab/mame_ab.py` runs the core against MAME frame for frame.

## What is changed from the firmware's MAME device

`fujinet_cart.c` keeps `pico/o2/emu/o2_fujinet.cpp`'s structure
line for line, with: the console's reads handed over by the machine with the
bus time (rather than an installed MAME read handler); the debugger's peek
and poke in place of `side_effects_disabled()`; the mailbox service and the
text engine on a worker thread whenever the machine runs paced (MAME only
when throttled); `FN_R_LINK` and a busy status published after each service
pass, and a reconnecting `wait_link_ms`, as the SMS sibling's cart has them.
The transport is this app's own portable `core/o2/fujitcp_host.c`
(Winsock and POSIX, abortable, a closed peer is a dead link, and the
debugger's trace tap); the staged `fujitcp.c` is staged but not compiled.

## System ROMs — not redistributed, only games need them

The Magnavox Odyssey 2 BIOS (MAME's `84932.ic11`, `84933.ic12`, `85456.ic13`,
`85457.ic14`) is copyrighted firmware that is **not** freely licensed. This
repository does not contain or redistribute it. CONFIG and every FujiNet app
boot without it, on the app's own stand-in; commercial games are BIOS
byte-code and need it.

- **Import BIOS…** (every frontend) copies a user's own image into the ROM
  directory, identified by size and CRC-32 against MAME's set (the 2K set,
  the 1K system ROM, or the four ROMs one by one); a 2K or 1K image with an
  unknown CRC is accepted with a warning.
- `-DWITH_O2_ROMS=ON` embeds whatever recognised images a developer put
  in `tools/roms/`, for local development only. Every artifact this project
  publishes is built with it **OFF** (every CI, release and Flatpak
  configure passes it explicitly), and the `no_embedded_roms_*` tests
  (`core/tests/no_embedded_roms.py`) check the shipped binaries for the
  ROMs' bytes.
- The tests never use RCA code either. `stub_boot` and `session` run a BIOS
  the tests assemble themselves (`core/tests/roms/testbios.asm`: display on,
  enter the cartridge), and `media` checks identification with filler images
  whose last four bytes steer the CRC-32 to a table entry. Comparisons with
  the real BIOS (`tools/ab/mame_ab.py --bios`) are run locally against a
  developer's own MAME ROM set and are not part of the repository.

## Trademarks and names

"RCA" and "Odyssey 2" are trademarks of their owners; they are used here to
name the machine being emulated, and the project is not affiliated with or
endorsed by them. "COSMAC" names RCA's CPU family. "FujiNet" is the FujiNet
project's name. "MAME" is a registered trademark of Gregory Ember.

## Icon and name

The icon is the FujiNet Go family mark (the same artwork as the other
desktops), inverted to black, on the Odyssey 2's case tan, `#C11218`
(`tools/icons/make-icons.py`).

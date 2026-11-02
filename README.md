# FujiNet Go Odyssey 2 Desktop

A native desktop emulator of the **Magnavox Odyssey 2 / Philips Videopac**
with FujiNet built in: boot the FujiNet CONFIG, browse a network host with the
joystick, load games and FujiNet apps over the network, play them. Part of the
FujiNet Go desktop family (Linux GNOME and KDE, Windows, macOS).

| | |
|---|---|
| **Emulator** | MAME's Odyssey 2 driver, its 8048, its 8244/8245 video and sound chip, the Videopac+ G7400's EF9340/41 text chip and 8243, and The Voice's SP0256, transposed to C (the Studio II and SMS siblings' way). The core does no per-clock work: the video chip draws lazily, the sound is kept as level changes, and the 8048 runs about 85 times faster than real time on one core. |
| **Machines** | Odyssey 2 (NTSC, 59.83 Hz), Videopac G7000 (PAL, 49.70 Hz), Videopac+ G7400 (PAL, EF9340 text layer mixed into the 8245's picture). Two joysticks and the real membrane keyboard. |
| **No BIOS needed for FujiNet** | CONFIG and every FujiNet app boot on this app's own stand-in BIOS (about 120 bytes of 8048 code written from the hardware). Games need the real BIOS: **Machine ▸ Import BIOS…**. |
| **Cartridge** | The FujiNet Odyssey 2 cartridge (`fujinet-firmware` `pico/o2`): the 3K program window, up to 63K of paged app image (APPBANK), the mailbox at `$E0-$E3` / `$F20`, and CONFIG baked in. Its mailbox service (`fujimail`), image planner (`o2map`), APPBANK serving (`o2app`) and wire codec (`fujibus`) are the cartridge firmware's own sources, staged verbatim. A game opened from disk gets its own mapper cartridge instead (standard, 4-in-1, rally, KTAA), as a real console would have it. |
| **The Voice** | The SP0256 speech module in front of the cartridge (**Preferences ▸ Machine**); its speech ROMs are imported like the BIOS. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/D3D11) — each with the display, a Controllers window, Preferences, a live FujiNet console log, and a debugger. |
| **Packages** | Linux: Flatpak (GNOME and KDE runtimes), DEB, RPM, tar.gz. Windows: ZIP and an NSIS installer. macOS: signed/notarised zips (unsigned without secrets). |

The icon is the family mark in black on the console's case silver
(`#C3C0BF`); the UI accent is the console's power-button red (`#C11218`),
marking held keys, the Map target, the current line in the disassembly and the
status dot.

## What you need to import (and why it is not shipped)

The Odyssey 2 / Videopac BIOS, the Videopac+ G7400 BIOS, the **character sets
of the video chips** (mask ROMs on the 8244/8245 and the EF9340) and The
Voice's speech ROMs are copyrighted; nothing here embeds them. **Import BIOS**
takes a file, a zip (MAME's `videopac.zip`, `i8244.zip`, `videopacp.zip`,
`ef9340_1.zip` all work) or a folder, identifies every dump by size and CRC-32
whatever it is called, and stores it under MAME's name. Without the 8244's
character set the picture is there but text is blank (the app says so);
without the BIOS only FujiNet apps run. See `COMPLIANCE.md`.

The Chess Module (an NSC800) and the Home Computer Module (a Z80) carry a CPU
of their own and are not emulated yet.

## Controllers window (F9)

A fixed-size floating window drawn like the console: the **real membrane
keyboard** (numeric row, + − × ÷ =, YES NO CLEAR ENTER, QWERTYUIOP, ASDFGHJKL,
ZXCVBNM . ?, SPACE, the red RESET key) and the two joysticks. Click a key to
press it; held keys light up in the accent colour whoever holds them. **Map**
rebinds any key or stick control to a host key or a gamepad button (a key moves
to its new target and leaves its old one), **Defaults** puts them back.

| | Default |
|---|---|
| Keyboard | each host key to the O2 key of the same label (**Enter** = ENTER, **Backspace** = CLEAR, `*` `/` `+` `-` `=` the operators) |
| Stick 1 | arrow keys, **Right Ctrl** = fire |
| Stick 2 | keypad **8 6 2 4**, keypad **0** = fire |
| Gamepads | D-pad / left stick = the stick, South = fire |
| RESET / Reset to CONFIG | **F1** / **Escape** |
| Controllers / Fullscreen / Debugger | F9 / F11 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

RESET resets the 8048 only, as on the console, so a running FujiNet app
restarts from the cartridge's current image; **Reset to CONFIG** is the power
cycle.

## Debugger (F12)

Prompt · CPU & RAM · Disassembly · **Video (the 8244)** · EF9340 · Sound & I/O ·
Breakpoints · Cart · FujiNet. The Video tab shows the rendered display (live
frame, re-rendered from the registers, or the frame in progress up to the
beam) with every layer switchable and objects boxed and indexed; every sprite,
single character and quad with its position, glyph and colour and a rendered
bitmap; the grid; every register decoded and editable; collisions; the sound
shift register; and the font, shape and palette sheets. The FujiNet tab
decodes the cartridge's FujiBus traffic from the moment the app starts.

## External FujiNet

A switch in Preferences makes the app connect to another FujiNet instance at a
host:port you enter, e.g. `127.0.0.1:11520`, instead of starting the built-in
one.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

fujinet-firmware is fetched at its pinned commit by the configure step — a
plain `git clone` with no `--recurse-submodules` is enough. To develop against
a working checkout of the cartridge firmware:

```sh
cmake -B build -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core and its tests; `-DWITH_FUJINET=OFF`
skips the firmware build (the machine boots CONFIG reporting the link down).
`-DWITH_O2_ROMS=ON` embeds whatever recognised ROMs are in `tools/roms/` for
local development; it is off by default and in every published build.

The core is C11. Qt >= 6.7 and GTK >= 4.16 are needed for the Linux
frontends (an older toolkit makes CMake skip that frontend). The macOS bundle
needs macOS 13.3 or later.

The tests' own 8048 program (`core/tests/roms/*.a48`, assembled into
`core/tests/test_rom.h` by `core/tests/roms/build-roms.sh`) and the stand-in
(`core/o2/stub.a48`, `tools/stub/build-stub.sh`) are committed assembled, so
building needs no assembler.

### Against MAME

`tools/ab/` runs the core against MAME's own Odyssey 2 (the MAME build with the
firmware's FujiNet graft, `pico/o2/emu/apply-mame.sh`, for CONFIG):

- `cart_ab.py` — whole carts, frame for frame, on the o2, g7000 and g7400
  machines, with your own BIOS and charset from MAME's rompath.
- `config_golden.py` — regenerates `core/tests/config_golden.h`, MAME's frame
  hashes of CONFIG on the stand-in with a generated font (no ROM needed to run
  the test that holds the core to them: `config_frames`).
- `mcs48_ab.py` — the 8048 against MAME's, instruction by instruction on 15
  probe programs; `gen_goldens.py` / `real_carts.py` — the 8244 against MAME's
  register traffic and on real carts.
- `o2_headless` (`core/tests/`) — the machine with no host thread, writing
  frames, hashes, audio and a time-stamped VDC access log for comparing.

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
wine build-win/frontends/windows/fujinet-go-o2-windows.exe
```

This is the desk-side check. `fujinet.dll` does not cross-build — the
firmware's PC target needs mingw builds of its dependencies — so the release
build is native MSYS2/UCRT64, which builds it.

## Ports

FujiNet's BoIP listener is on **11520** and its web admin UI on **11521** —
high ports of this app's own, continuing the family's table, so a standalone
`fujinet-pc` or a sibling app never collides:

| App | BoIP | Web UI |
|---|---|---|
| ADAM | 65216 | 65214 |
| Apple II | 1985 | 64001 |
| CoCo | 65504 | 64002 |
| Intellivision | 65503 | 64003 |
| MSX | 65505 | 64003 |
| Astrocade | 11500 | 11501 |
| ColecoVision | 11502 | 11503 |
| Atari 2600 | 11504 | 11505 |
| NES | 11506 | 11507 |
| Master System | 11508 | 11509 |
| Atari 7800 | 11510 | 11511 |
| H89 | 11512 | 11513 |
| Atari 800 | 11514 | 11515 |
| MS-DOS | 11516 | 11517 |
| Studio II | 11518 | 11519 |
| **Odyssey 2** | **11520** | **11521** |

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. The cartridge's mailbox
service runs on its own thread, as it runs on the cartridge's second core, so a
slow network transaction never stalls the picture.

## How this was verified

- **The core against MAME** (all run on this machine, locally, with MAME's own
  ROMs; none of them are in the repo):
  - the 8048: 15 probe programs, 147,053 instructions, traces identical, and
    the disassembler's text for all 256 opcodes;
  - the 8244/8245: 94 recorded MAME scenes (static, beam registers, raster
    effects, sound) replayed into the C chip, every read value and frame
    pixel-identical, and 157 real cartridges' register traffic replayed;
  - the EF9340/8243 against MAME's G7400 on 12 scenarios, pixel for pixel;
  - the SP0256 sample for sample against MAME's, also run against MAME's
    Intellivision with a real Intellivoice ROM;
  - the whole machine: CONFIG on the stand-in matches MAME's frames
    (`config_frames`, no ROM needed), and with the real BIOS two carts
    (Baseball, Alpine Skiing) match all but the first two power-on frames on
    all three machines. **Known gap:** Atlantis' sprite multiplexing differs
    from MAME in about 0.3% of its pixels; not explained. CONFIG itself runs
    one frame ahead of MAME after boot (its splash ends a frame early, the
    transition frames identical); `config_frames` samples steady pictures
    only.
- **The stand-in BIOS** runs CONFIG's own MAME scenarios (hosts, edit, boot,
  browse) with the same results as the real BIOS, and `stub_boot` presses
  every key of the matrix and every stick direction through it.
- **`ctest`** (26 tests): the core alone with no SDL, the chips above, the
  stand-in, the cartridge against a loopback FujiNet (a pushed image staged,
  swapped in at the next fetch of `$400`, the trace tap byte for byte with the
  wire), the paced host (59.83 Hz measured), the session, ROM import (CRC-forged
  fillers, our own zip reader, folders), bindings, the keyboard layout, media
  routing, gamepads, and the debugger contract.
- **GNOME, KDE, Windows, macOS** — see `TODO` for each frontend's state.

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-o2-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-o2-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `fujinet-go-o2-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `fujinet-go-o2-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `fujinet-go-o2-<version>-macos-{arm64,x86_64}.zip` | the `.app` bundle for Apple Silicon and Intel, FujiNet inside |
| `online.fujinet.go.o2.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both metainfo
files, which template it), not derived from the tag; `check-version` stops the
release if they disagree. To release 0.1.1: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.1.1 && git push origin v0.1.1`. Running the workflow by hand
(Actions → Release → Run workflow) is a dry run: everything is built and
tested and the artifacts are attached to the run, but no release is made.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the exe's and `fujinet.dll`'s import tables against a system-DLL
whitelist. The macOS jobs sign and notarise when the `MACOS_*` secrets are
set, and check the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance. "Magnavox",
"Odyssey" and "Philips Videopac" are trademarks of their owners, used to name
the machine emulated; this project is not affiliated with them.

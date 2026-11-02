# FujiNet Go — Magnavox Odyssey 2

A self-contained Magnavox Odyssey 2 with a built-in
[FujiNet](https://fujinet.online/): power on into the FujiNet CONFIG client,
browse a network host from the keypad, boot `.st2` images over the network,
and let a FujiNet app keep talking to the network — all in one desktop app.
A member of the FujiNet Go desktop family (`fujinet-go-adam-desktop`,
`-apple2-`, `-coco-`, `-msx-`, `-intv-`, `-astrocade-`, `-coleco-`,
`-atari2600-`, `-nes-`, `-sms-`, `-atari7800-`, `-h89-`, `-atari800-`,
`-msdos-`).

| | |
|---|---|
| **Emulator** | MAME's Magnavox Odyssey 2 (`o2.cpp`), its CDP1802 COSMAC (`cosmac.cpp`), its CDP1861 Pixie (`cdp1861.cpp`) and its beeper, transposed to C — the astrocade and SMS siblings' way. Frame for frame pixel-identical to MAME, keys included (`tools/ab/`). |
| **Machine** | The Magnavox Odyssey 2 (NTSC, 1.76 MHz, 59.98 Hz), two 10-key keypads and CLEAR. No BIOS needed for CONFIG or FujiNet apps. |
| **Cartridge** | The FujiNet Odyssey 2 cartridge (`fujinet-firmware` `pico/o2` on `pico/common`): it sees every console read — display DMA included — and claims whole pages outside the BIOS, serving the 64×128 raster the 1861 fetches and a 16×10 text engine drawn into it, the mailbox at `$E400`, and up to 48K of image (an ST2 with a bank window at `$C400`, or raw), CONFIG baked in. It speaks protocol 2: pages the program can write (a poke stream, COPY) and a graphics layer — pictures stamped at any pixel, rectangles, 16 sprites, the view shaken or inverted — composed into a double-buffered display, and drawn even while a FujiNet transaction is out. M.U.L.E. runs on it. Its protocol (`fujimail`), wire codec (`fujibus`), image planner (`s2map`), staging and swap (`fuji_load`) and text engine (`s2_text`) are the cartridge firmware's own sources, staged verbatim. |
| **FujiNet** | The firmware's `RS232` PC target, built in-process as `libfujinet` and dialled by the cartridge over loopback (BoIP on **11520**, web admin on **11521**). |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/GDI) — each with the display, a Keypad window, Preferences, a live FujiNet console log, and a debugger. |
| **Packaging** | Per-frontend DEB/RPM/TGZ, two Flatpaks, a Windows (x86-64) zip and NSIS installer, and macOS bundles for Apple Silicon (arm64) and Intel (x86_64), all through GitHub Actions. |

## What it is

- **CONFIG is the power-on program.** The CONFIG client (`fujinet-config`'s
  `o2/` build) is the image the cartridge serves at power-on, exactly as
  on the hardware. From CONFIG, pick a host and an image; FujiNet pushes the
  file to the cartridge, the cartridge plans and stages it beside CONFIG,
  CONFIG arms the swap and starts the console over, and the cartridge swaps
  the image in on the first fetch of `$0000`.
- **The cartridge, faithfully.** Every image — CONFIG, a network-booted
  game, a local file — is planned by the cartridge's own `s2map`, so what
  works here works on the cartridge: an ST2 of up to 192 blocks, or a raw
  image of up to 1K at `$0400`. An image it cannot plan is refused with the
  reason. A program that carries the `FUJI` claim at `$07FC` keeps the
  mailbox and the text engine; a game runs alone.
- **No BIOS needed for FujiNet.** The console runs the RCA BIOS at `$0000`,
  but CONFIG and every FujiNet app boot on this app's own 92-byte stand-in
  (`core/o2/stub.asm`): it clears RAM, turns the display on and enters
  the app at `$0402` as the BIOS would. Commercial games are BIOS byte-code
  and need the real thing: **Import BIOS…** takes your own dump (MAME's 2K
  set, the 1K system ROM, or the four 512-byte ROMs, identified by size and
  CRC). A game opened, or booted from CONFIG, with no BIOS says so and
  offers Import BIOS. Nothing copyrighted ships with the app
  (`COMPLIANCE.md`).
- **Open Cartridge…** serves a local `.st2`/`.bin` from power-on in place of
  CONFIG (and remembers it for the next start). **Import Cartridge to SD…**
  copies one into FujiNet's SD root, where CONFIG lists it. Dropping a file
  on the window opens a cartridge, imports a BIOS, or puts anything else on
  the SD card.
- **The console's keys.** Two keypads, A and B (1 2 3 / 4 5 6 / 7 8 9 / 0),
  and **CLEAR** (F3), which holds the CPU in reset and resets the 1861; the
  cartridge restarts whatever it serves. **Reset to CONFIG** (Escape) is a
  power cycle — on the hardware only losing power brings CONFIG back — and
  ejects an opened cartridge.
- **The debugger, in every frontend.** F12 opens it and stops the machine:
  Run/Stop (F5), Step (F7), Step Over (F8: over a `SEP` call or an SCRT
  call), Step Out (Shift+F8), Scanline+1, Frame+1. Tabs: a Prompt (`help`,
  `break`, `bpr`/`bpw`/`bpin`/`bpout` with conditions over registers, flags,
  memory and the beam, `print`, `mem`, `poke`, `video`, `io`, `keys`,
  `cart`, `mailbox`, `text`, `label`, `runto`, `disasm`, `trace`, `save` …
  with Tab completion), CPU & RAM (R0-RF, D, DF, P, X, T, B, IE, Q, EF1-4,
  the 512 bytes of RAM, editable), Disassembly (MAME's mnemonics; click to
  toggle a breakpoint, follow PC, jump to a label), Video (the 1861's
  INT/EF/DMA and the beam; the whole raster, the cart's raster and the BIOS
  display page), Sound & I/O (Q, the key latch, both keypads, the ports),
  Breakpoints, Cart (the 256-page map of what the cart serves, staging and
  the swap, the text engine's screen) and FujiNet (every FujiBus packet on
  the cartridge's link, from power-on). It loads Macroassembler AS `.map`
  and listing symbols, WLA-DX `.sym` and plain lists; the mailbox, the text
  engine, the raster and the clients' entry are labelled out of the box.
- **Gamepads that come and go.** SDL3 hot-plug: a pad steers with the keys
  the games use — 2 4 6 8, the diagonals on 1 3 7 9 — fires with 0 and 5,
  and Start is CLEAR. The **Keypad** window shows both keypads live,
  presses keys with the mouse, and rebinds any key to a keyboard key or a
  gamepad button (Map).

The icon is the family mark in black on the Odyssey 2's case tan
(`#C11218`). The UI accent is the same tan, marking held keys, the Map
target, the current line in the disassembly and the status dot.

## External FujiNet

A switch in Preferences makes the app connect to another FujiNet instance at a host:port you enter, e.g. `127.0.0.1:11520`, instead of starting the built-in one.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

fujinet-firmware is cloned at its pinned commit (on master) by the configure
step — a plain `git clone` with no `--recurse-submodules` is enough. To
develop against a working checkout of the cartridge firmware:

```sh
cmake -B build -DFUJINET_SRC=~/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core and its tests; `-DWITH_FUJINET=OFF`
skips the firmware build (the machine boots CONFIG reporting the link down).
`-DWITH_O2_ROMS=ON` embeds whatever BIOS images are in `tools/roms/` for
local development; it is off by default and in every published build.

The core is C11. The macOS bundle needs macOS 13.3 or later.

The `netboot` test runs the cartridge bring-up's own clients (`fujitest.st2`,
and `fujiboot.st2` built to boot `/S2COUNTER.BIN`, which the test puts on the
SD host); build them with the Macroassembler AS and point the test at them:

```sh
BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN third_party/fujinet-firmware/pico/o2/build.sh fujiboot fujitest
O2_TESTROM_DIR=$PWD/third_party/fujinet-firmware/pico/o2/build \
    ctest --test-dir build -R netboot
```

The tests' own 1802 programs (`core/tests/roms/*.asm`, assembled into
`core/tests/test_rom.h` by `roms/build-roms.sh`) and the stand-in
(`core/o2/stub.asm`, `tools/stub/build-stub.sh`) are committed
assembled, so building needs no assembler.

### Against MAME

`tools/ab/mame_ab.py` runs the same image on MAME's own Odyssey 2 and on
this core, frame for frame, with the same scripted keys, and demands
identical pixels: the console alone (the BIOS's built-in games), a cart
image on stock MAME's cart or (`--direct`) through MAME's FujiNet device,
or CONFIG. It wants a o2 MAME build carrying the firmware's FujiNet
graft (`pico/o2/emu/apply.sh`) in `$MAME` and your own BIOS
(`--bios`); use `--mame-power-on`, since MAME's 1802 skips the power-on
CLEAR reset the console does (`core/o2/machine.c`), and press Clear
first, or MAME's BIOS never takes its interrupt. `--wav` compares the
beeper too. `tools/ab/abtrace.lua` and `o2_headless -trace` give bus-read traces of
both sides with cycle stamps, for finding where two runs part.

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
| Master System | 11508 | 11521 |
| Atari 7800 | 11510 | 11511 |
| H89 | 11512 | 11513 |
| Atari 800 | 11514 | 11515 |
| MS-DOS | 11516 | 11517 |
| **Odyssey 2** | **11520** | **11521** |

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. The cartridge's mailbox
service and text engine run on their own thread, as they run on the
cartridge's second core, so a slow network transaction never stalls the
picture.

## Keys

| | |
|---|---|
| Keypad A | **Q W E** / **A S D** / **Z X C** = 1-9, **Left Alt** = 0 (MAME's) |
| Keypad B | the numeric keypad, laid out alike: 7 8 9 = 1 2 3 … 0 = 0 |
| Gamepads | D-pad / left stick = 2 4 6 8 (diagonals 1 3 7 9); South = 0, East = 5, Start = CLEAR |
| CLEAR / Reset to CONFIG | **F3** / **Escape** |
| Keypad / Fullscreen / Debugger | F9 / F11 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

Every key is remappable in the Keypad window.

## How this was verified

- **The core against MAME** — `tools/ab/mame_ab.py`, with a local BIOS,
  Clear pressed first and keys on both pads: the console alone (the
  built-in games), and eight cart images (*Spacewar*, *Tennis-Squash*,
  *Baseball*, *Speedway-Tag*, *Blackjack*, and Paul Robson's *Combat*,
  *Hockey* and *Space Invaders*) served through the FujiNet cart against
  MAME's FujiNet device: 1200 of 1200 frames pixel-identical each, and the
  beeper sample for sample (every beep's start and end within two samples
  of MAME's); CONFIG with the link down 400 of 400. Every bus read of the
  tests' own BIOS and game, and of CONFIG on the stand-in, matches MAME's
  trace (`mame_trace`, in the suite). The disassembler matches MAME's for
  all 256 opcodes (`dasm_golden.h`).
- **The core on Linux** — `ctest`: the core alone (`no_sdl_link`); the
  stand-in (`stub_boot`: CONFIG paints, a FujiNet app runs and takes its
  interrupt once a frame, keypad A reaches it, CLEAR restarts it, a game
  parks it and blanks the picture, a BIOS of the tests' own runs the game);
  the paced host (`boot_smoke`: 59.98 Hz measured); the cart and its
  transport against a loopback FujiNet (`cart`: a pushed image staged,
  armed and swapped in, the trace tap byte-for-byte with the wire); the
  seam (`cart_seam`: a game's pages and an app's, peeks that fire nothing,
  the hand-over only on a qualified read of `$0000`, CLEAR bringing an app
  that armed CONFIG back up in CONFIG); the
  session; media and Import BIOS (identified by CRC with filler images whose
  CRC was steered to MAME's table — no RCA code involved); bindings;
  gamepads (an SDL virtual pad); the debugger contract. With the in-process
  FujiNet: the link (`fujibus_smoke`), CONFIG driven like a user — the SD
  host, the file, boot — booting an app onto the stand-in and a game that
  then asks for the BIOS (`config_boot`), and the bring-up's fujitest and
  fujiboot clients with the mailbox threaded and inline (`netboot`).
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

The version is declared in the tree (`project(… VERSION …)` and both
metainfo files, which template it), not derived from the tag; `check-version`
stops the release if they disagree. To release 0.2.1: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.2.1 && git push origin v0.2.1`. Running the workflow by hand
(Actions → Release → Run workflow) is a dry run: everything is built and
tested and the artifacts are attached to the run, but no release is made.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the exe's and `fujinet.dll`'s import tables against a system-DLL
whitelist. The macOS jobs sign and notarise when the `MACOS_*` secrets are
set, and check the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance. "RCA"
and "Odyssey 2" are trademarks of their owners, used to name the machine
emulated; this project is not affiliated with them.

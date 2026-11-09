This is the text of FujiNet Go Odyssey 2's built-in Help (Help ▸ FujiNet Go
Odyssey 2 Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Odyssey 2 is a Magnavox Odyssey 2 (or Philips Videopac) with a
FujiNet cartridge in its slot. The console is MAME's: the 8048 processor, the
8244/8245 video and sound chip, the Videopac+ G7400's EF9340 text chip, The
Voice's speech chip, and the two joysticks and the membrane keyboard. The
FujiNet runs inside the app, so CONFIG comes up at once and can mount games
and programs from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. It
   needs no BIOS: the app has a small stand-in of its own that runs CONFIG
   and every FujiNet app. See [BIOS and ROMs](#bios-and-roms).
2. **Pick something in CONFIG.** Choose a host (the SD folder or a server on
   the network) and a program on it. FujiNet sends it to the cartridge and
   it starts. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes a cartridge image: a `.bin`, a `.rom` or an
   `.o2`, or a zip of one. Games need the console's own BIOS, which you
   import (**Import BIOS…**). See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the two joysticks and the keyboard. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the machine,
   the BIOS, The Voice, the picture, the sound, the controls and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window, the FujiNet
console log and the Needs BIOS message opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(In the main window <kbd>F1</kbd> is the console's RESET.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…**:
  see [Cartridges](#cartridges).
- **Import BIOS…** and **Import BIOS from a Folder…**: see
  [BIOS and ROMs](#bios-and-roms).
- **Pause**, **Reset** and **Reset to CONFIG**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**
  (<kbd>Ctrl+Q</kbd>).

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, View, FujiNet and Help. The Machine menu has the
cartridge and BIOS items, Pause, Reset, Reset to CONFIG, Preferences and Quit
(Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and
the cartridge and BIOS items are in the Machine menu. Controllers and the
Debugger are also ⌘J and ⌘D.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

A banner over the picture says when a ROM the running machine lacks matters:
the 8244's character set (without it the picture is there but the text is
blank) and The Voice's speech ROMs. A **Needs BIOS** message (with an
**Import BIOS…** button) appears when a game needs the console's own BIOS and
has none. See [Needing the BIOS](#needing-the-bios).

Messages (a cartridge opened, a gamepad plugged in, FujiNet restarted) show
for a few seconds over the picture.

## Reset and power

- **Reset** presses the console's RESET key (<kbd>F1</kbd>). It resets the
  8048 only, as on the console, so a running FujiNet app starts again from
  the cartridge's current image.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is a power cycle:
  it ejects an opened cartridge and boots CONFIG, exactly as at power-on.
  The machine, BIOS and The Voice you chose in Preferences take effect here.
- **Eject Cartridge** does the same; a FujiNet cartridge is always in the
  slot, and CONFIG is its own program.
- **Pause** stops the console and its sound; the Debugger's Stop is
  separate.

## The picture

The console draws 360 × 243 pixels. A television shows them 4:3; **TV
Aspect** switches between that and square pixels. The Odyssey 2 runs at 59.83
frames a second and the PAL Videopacs at 49.70.

**Smooth Scaling** blends the pixels when the picture is stretched, and
leaves them sharp when it is off.

# Controllers and keys

The Odyssey 2 has two joysticks, each with an eight-way stick and a fire
button, and a membrane keyboard: the numeric row, + − × ÷ and =, YES, NO,
CLEAR and ENTER, the letters, SPACE, and the red RESET key. The Videopac+
G7400 has a bigger keyboard. Any number of keys can be held at once.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Stick 1 | Stick 2 |
|---|---|---|
| Up, down, left, right | The arrow keys | <kbd>Keypad 8</kbd>, <kbd>Keypad 2</kbd>, <kbd>Keypad 4</kbd>, <kbd>Keypad 6</kbd> |
| Fire | <kbd>Right Ctrl</kbd> | <kbd>Keypad 0</kbd> |

The console's keyboard is each host key to the key of the same label:
letters, digits and `. ? ,` are themselves, <kbd>Return</kbd> is ENTER and
<kbd>Backspace</kbd> is CLEAR, and `*`, `/`, `+`, `-` and `=` are the
operators.

| Console | Key |
|---|---|
| RESET | <kbd>F1</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Q</kbd> | Quit |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The first is Stick 1, the second Stick 2; Preferences ▸ Input can fix
which is which. A pad's stick is remembered between runs: plug it back in,
or start the app again, and it gets its stick back.

| Gamepad | Does |
|---|---|
| D-pad, or left stick | The stick |
| South (A) | Fire |

With **Analog sticks steer too** on, the left stick presses the stick's
directions as the D-pad does. A gamepad with no standard layout (a plain USB
adapter) uses its first stick or hat and its buttons, which can be given
jobs in the same way.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, then click a control on the picture (77 in all: both sticks' four
directions and fire, the 64 keys, RESET and the app's own actions), then
press the key or the gamepad button to give it. The control's old key or
button is taken from it. **Defaults** puts every control back, and
<kbd>Escape</kbd> leaves Map. The map is kept in the settings, and only what
differs from the defaults is written.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the console as a player sees
it: the real membrane keyboard (the numeric row, the operators, YES NO CLEAR
ENTER, the letters, SPACE and the red RESET key) and the two joysticks. It
floats on its own at a fixed size. The controls that are held light up in the
accent colour, whoever holds them: the keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Click a key | Presses it |
| Drag a stick | Moves it |
| Click a fire button | Presses it |

**Map** and **Defaults** change the controls (see
[Changing the controls](#changing-the-controls)).

Switching to another window lets go of everything the mouse holds. Keys
typed in the Controllers window go to the console.

# Machines

Preferences ▸ Machine picks the machine:

- **Magnavox Odyssey 2 (NTSC)**: the American console, with the 8244 video
  chip, at 59.83 frames a second.
- **Philips Videopac G7000 (PAL)**: the European console, with the 8245 and
  the same BIOS, at 49.70 frames a second.
- **Philips Videopac+ G7400 (PAL)**: adds the EF9340/41 text chip mixed into
  the 8245's picture, its own BIOS and a bigger keyboard.

A new machine takes effect at the next power cycle (Reset to CONFIG). The
Chess Module (an NSC800) and the Home Computer Module (a Z80) carry a CPU of
their own and are not emulated yet.

# BIOS and ROMs

The console's BIOS, the character sets of the video chips (mask ROMs on the
8244/8245 and the EF9340) and The Voice's speech ROMs are copyrighted, so
none comes with the app. You import your own:

| ROM | Is |
|---|---|
| `o2bios.rom` | The BIOS of the Odyssey 2 and the Videopac G7000 |
| `g7400.bin` | The BIOS of the Videopac+ G7400 |
| `charset_i8244.bin` | The 8244 and 8245's character set |
| `charset_ef9340_1.rom` | The G7400's EF9340 character set |
| `sp0256b-019.bin` | The Voice's SP0256 mask ROM |
| `spr128-003.bin` | The Voice's speech ROM |
| `spr128-004.bin` | Sid the Spellbinder's speech ROM |

## The stand-in

Without the BIOS the app runs a small program of its own in its place, about
120 bytes of 8048 code written from the hardware. It is enough for CONFIG and
every FujiNet app. A game calls the real BIOS's routines, so it needs the
real BIOS (see [Needing the BIOS](#needing-the-bios)). Without the 8244's
character set the picture is there but the text is blank, and the app says
so in a banner.

## Importing them

- **Import BIOS…** (in the Machine menu, on the ROMs page of Preferences, and
  in the Needs BIOS message) takes a file or a zip. MAME's `videopac.zip`,
  `i8244.zip`, `videopacp.zip` and `ef9340_1.zip` all work.
- **Import BIOS from a Folder…** takes a whole folder, searched eight
  levels down, and the zips in it.
- Dropping a ROM file or zip on the window imports it too.

Files are recognised by their size and checksum (CRC-32), whatever they are
called; anything else in a folder or zip is left alone. The message says what
was imported and what is still missing. Imported files are copied into the
app's `roms` folder (see [Files and folders](#files-and-folders)), under
MAME's names, so the originals can go. A new ROM takes effect at the next
power cycle (Reset to CONFIG).

<!-- context: nobios -->
## Needing the BIOS

A cartridge that is not a FujiNet app is a game, and a game calls the real
BIOS. If the BIOS is not imported, or Preferences ▸ Machine ▸ **Use the
imported BIOS** is off, **Open Cartridge** refuses it and says why, and a
**Needs BIOS** message appears. Import the BIOS, then open the cartridge
again. The Videopac+ G7400 runs only on its own BIOS, and The Voice needs its
speech ROMs.

<!-- context: prefs/ROMs -->
## The ROMs page

Preferences ▸ ROMs lists every ROM above, *Imported* or *Not imported*, in
three groups, with **Import BIOS…** and **Import from a Folder…** buttons,
and says where they are kept.

- **Console:** the Odyssey 2 / Videopac G7000 BIOS (o2bios.rom) and the
  Videopac+ G7400 BIOS (g7400.bin).
- **Video chips:** the 8244/8245 character set and the EF9340 character set.
- **The Voice:** the SP0256 ROM, the speech ROM, and Sid the Spellbinder's
  speech ROM.

# Cartridges

A cartridge is a file: a `.bin`, a `.rom` or an `.o2`, or a `.zip` that holds
one. The app works out what it is from its contents.

- **A FujiNet app** is an image that claims the mailbox. It runs in the
  FujiNet cartridge: it can talk to the network, and it needs no BIOS beyond
  the stand-in.
- **A game** is any other image. It gets the mapper a real cartridge would
  have (standard, 4-in-1, rally or KTAA), runs on the real BIOS, and FujiNet
  is not reachable until the next power cycle.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) powers the console off and on with
the cartridge in the slot. The cartridge is remembered and starts again
next time; **Eject Cartridge** or **Reset to CONFIG** forgets it. Dropping a
cartridge file on the window opens it. A file the console cannot take is
refused with the reason, and what was running goes on.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge (a zip of one is
unpacked to `<name>.bin`). The FujiNet cartridge refuses what it cannot
serve, and the message says why. Without the FujiNet runtime there is no SD
folder to copy to. A game that boots from the SD folder still needs the
console's BIOS.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. On the real console
the FujiNet cartridge is a board on the cartridge connector; here it is
emulated, and talks to FujiNet over a local network socket. FujiNet
listens; the cartridge dials in, and keeps trying again if the link drops.

The cartridge has a 3 KB program window, up to 63 KB of paged app image
(APPBANK) and the mailbox at $E0 to $E3 and $F20, with CONFIG baked in.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where programs come from: the SD folder, and servers on the
  network.
- Pick a host, a program on it, and start it. FujiNet sends the image to the
  cartridge, which swaps it in and restarts the console on it. A FujiNet
  app can then ask FujiNet for more (pages, files, the network).
- With the BIOS imported, CONFIG runs on the real BIOS; without it, on the
  stand-in.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11521/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **Enable FujiNet** turns the FujiNet off. The
cartridge then reports *link down*, CONFIG runs without hosts, and nothing
is dialled. Games are not affected.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet
and dials another FujiNet instance at the address you give, for example
`192.168.1.20:11520`. Leave the field empty for `127.0.0.1:11520`. Set that
instance's `[BOIP]` section to listen there (`enabled=1`, `host`, `port`).
The cartridge dials out, so the other FujiNet can start before or after it.
FujiNet Web UI and Console Log belong to the built-in FujiNet, so they say
the FujiNet is external; use the other instance's own. The Debugger's
FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11520 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11521. Nothing from outside can
  reach either.
- The cartridge's mailbox service runs on its own thread, as it runs on the
  cartridge's second core, so a slow network transaction never stalls the
  picture.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.
- A game that does not claim the mailbox runs with FujiNet unreachable until
  the next power cycle.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. The machine, BIOS, Voice,
audio, gamepad and FujiNet settings apply with a power cycle: the app does it
when Preferences closes. The picture, the volume and the sticks apply at
once. The settings are one file, shared by the GNOME and KDE apps on the
computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu), and its
pages are tabs.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Machine**: which console (see [Machines](#machines)).
- **BIOS**: whether the imported BIOS is there.
- **Use the imported BIOS**: off, the app's own stand-in runs instead. It
  boots CONFIG and FujiNet apps, not games (setting `use_bios`).
- **The Voice in front of the cartridge**: the speech module, as plugged
  into the console's slot. It needs its two speech ROMs (setting `voice`).

<!-- context: prefs/Display -->
## Display and audio settings

- **Aspect**: *Television (4:3)* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched
  (setting `smooth`).
- **Enable audio**: open the system audio device.
- **Volume**: the output level, 0 to 100.

<!-- only: kde windows macos -->

On KDE, Windows and the Mac these are two pages, Display and Audio.

<!-- end -->

<!-- context: prefs/Input -->
## Input settings

- **Enable gamepads**: poll USB and Bluetooth gamepads.
- **Analog sticks steer too**: the left stick presses the joystick's
  directions as the D-pad does (setting `analog_joystick`).
- **Gamepads**: each one plugged in, and which stick it drives. A new
  gamepad drives Stick 1, the next Stick 2; choose *Stick 1* or *Stick 2*
  to fix it, or *Automatic*. The choice is remembered between runs (setting
  `pad_assign`), by the gamepad's id and name; two identical gamepads cannot
  be told apart, so the first to connect gets it.

The keys and buttons are changed in the Controllers window (see
[Changing the controls](#changing-the-controls)).

<!-- context: prefs/FujiNet -->
## FujiNet settings

- **Enable FujiNet**: run the built-in FujiNet.
- **Use external FujiNet**: dial another instance instead.
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet)).
- **Web UI**: the address of the built-in FujiNet's settings page.

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
The keys the Preferences pages set:

| Key | Is |
|---|---|
| `machine` | 0 the Odyssey 2, 1 the Videopac G7000, 2 the Videopac+ G7400 |
| `use_bios` | 1 (the default) or 0 |
| `voice` | 1 or 0 (the default) |
| `analog_joystick` | 1 (the default) or 0 |
| `enable_fujinet` | 1 or 0 |
| `fujinet_external` | 1 or 0 |
| `fujinet_external_addr` | `host:port` |
| `enable_audio` | 1 or 0 |
| `enable_gamepad` | 1 or 0 |
| `pad_assign` | the gamepads fixed to a stick, remembered between runs; written by the app |
| `volume` | 0 to 100 |
| `aspect` | 0 the television's 4:3, 1 square pixels |
| `smooth` | 1 blends the pixels when stretched, 0 keeps them sharp |
| `cart` | the opened cartridge, empty for CONFIG |

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has nine tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a CALL (or a DJNZ loop), to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns (RET or RETR) |
| Scanline+1 | On to the next line of the picture |
| Frame+1 | On to the next frame |
| Symbols | Loads labels (see [Disassembly](#disassembly)) |
| Save | Saves the disassembly, RAM, the mailbox, the VDC, the font, the screen or the registers to a file |
| Help | Opens the topic of the tab that shows |

Run to an address with the Prompt's `runto`.

Beside them, the Debugger says why the console stopped: a step, a
breakpoint and its address, a data read or write.

<!-- context: debugger/prompt -->
## Prompt

The Prompt takes commands, with completion on <kbd>Tab</kbd> for commands,
registers and labels. Numbers are `$0400`, `0x400`, `400h`, `1024` or
`%1010`. `help` lists the commands.

| Command | Does |
|---|---|
| `step` | Runs `step n` instructions |
| `over`, `out` | Step over, step out |
| `frame`, `scanline` | Run `n` frames or lines |
| `run`, `stop` | Run on, or stop |
| `runto` | Runs to an address |
| `break` | An execute breakpoint, `break addr[-end] [cond]` |
| `bpr`, `bpw` | Program-data reads or writes of an address range |
| `bpxr`, `bpxw` | External data (MOVX) reads or writes |
| `bpir`, `bpiw` | Internal RAM reads or writes |
| `bpvr`, `bpvw` | VDC register reads or writes |
| `bpin`, `bpout` | IN or OUT on a port: `bpin p1\|p2\|bus [cond]` |
| `delete`, `enable`, `disable` | A breakpoint by id |
| `clear`, `breaks` | Clear all, or list them |
| `print` | Evaluates an expression |
| `regs`, `set` | The registers; `set reg expr` |
| `mem` | Memory in a space: `mem [space] addr [len]` |
| `poke` | Writes bytes to a space |
| `vdc`, `objects` | The 8244/8245's registers; its sprites, characters and quads |
| `ef`, `voice` | The G7400's EF9340; The Voice's SP0256 |
| `io`, `keys` | The ports decoded; the keyboard matrix and the sticks |
| `cart`, `mailbox` | The cartridge, and FujiNet's mailbox page |
| `label`, `labels` | Name an address; load a symbol file |
| `disasm` | Disassembles `disasm [addr] [n]` |
| `trace` | The instruction trace: `trace [on\|off\|clear\|n]` |
| `save` | Saves `dis`, `ram`, `mailbox`, `vdc`, `font`, `screen` or `regs` to a file |

The spaces are `prog` (the default), `xdata`, `iram` and `vdc`. An expression
has the registers (`a pc psw sp r0` to `r7` for the bank in use, `r0'` to
`r7'` for bank 1, `t cnt p1 p2 bus`, the flags `c ac f0 f1 bs mb`), the byte
at `[addr]`, `x[addr]`, `i[addr]` and `v[addr]`, labels, and `vpos`, `hpos`,
`frame` and `cycles`. In a breakpoint condition `value` and `addr` are the
access. The operators are C's.

<!-- context: debugger/cpu -->
## CPU and RAM

The 8048's registers: the accumulator, the program counter, PSW and the
stack pointer, the eight registers of each bank, the timer and the flags.
Click a value to edit it while stopped. The hex view reads program memory,
external data, internal RAM and the VDC, and can edit them.

<!-- context: debugger/disassembly -->
## Disassembly

The 8048 disassembled at the PC, with the opcode bytes, labels and the
breakpoints in the margin. Double-click a line for an execute breakpoint.
Follow PC keeps the current line in view. **Symbols** loads the labels of a
listing, or a plain list of addresses and names.

<!-- context: debugger/video -->
## Video

The 8244 (the 8245 on the PAL machines) draws sprites, single characters and
quads over a grid. The Video tab shows:

- The rendered display: the live frame, re-rendered from the registers, or
  the frame in progress up to the beam, with every layer switchable and the
  objects boxed and indexed.
- Every sprite, single character and quad with its position, glyph and colour,
  and a rendered bitmap, and the grid.
- Every register decoded and editable, and the collisions.
- The sound shift register.
- The font, shape and palette sheets.

<!-- context: debugger/ef9340 -->
## EF9340

The Videopac+ G7400's text chip, mixed into the 8245's picture: its page of
characters, the registers and the font. It is blank on the other machines.

<!-- context: debugger/io -->
## Sound and I/O

The console's ports as the CPU last wrote them and decoded, the keyboard
matrix and the sticks as the CPU sees them, the sound, and The Voice.

<!-- context: debugger/breakpoints -->
## Breakpoints

The list of breakpoints with their ids, kinds, ranges, conditions and hit
counts. A breakpoint can be an execute, a data read or write, an external or
internal RAM access, a VDC register access, or a port IN or OUT, each with an
optional condition. **Add**, **Remove**, and the check box to turn one off.

<!-- context: debugger/cart -->
## Cart

The cartridge in the slot, and the FujiNet mailbox: the registers, the live
image and the one staged for the next swap, and the events.

<!-- context: debugger/fujinet -->
## FujiNet packets

Everything between the cartridge and FujiNet as FujiBus packets, from the
moment the app starts: the direction, the device, the command and the
length. A repeat of the same exchange shows once with a count. **Clear**
empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-o2/settings.ini` and
the data is in `~/.local/share/fujinet-go-o2` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-o2\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-o2`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-o2/settings.ini` and
the data is in `~/.local/share/fujinet-go-o2`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported ROMs |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **This cartridge needs the Odyssey 2 BIOS**: it is a game, and no BIOS is
  imported (or Use the imported BIOS is off). See
  [Needing the BIOS](#needing-the-bios).
- **The text is blank**: the 8244's character set is not imported. See
  [BIOS and ROMs](#bios-and-roms).
- **The Videopac+ does not start**: it runs only on its own BIOS, g7400.bin,
  and needs the EF9340 character set for its text.
- **No speech**: The Voice needs its speech ROMs, and the switch in
  Preferences ▸ Machine.
- **An imported file was not taken**: files are recognised by size and
  checksum; get the ROMs from MAME's `videopac.zip`, `i8244.zip`,
  `videopacp.zip` and `ef9340_1.zip`.
- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ FujiNet.
- **The cartridge was refused**: the message says why: too big, empty, or
  not a cartridge image. A zip must hold exactly one image.
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the stick you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No sound**: the audio device could not be opened, or Pause is on.
- **A port is taken**: another program is using 11520 or 11521. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Odyssey 2 is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the Odyssey 2 driver, the 8048, the 8244/8245 and EF9340 video
  chips and The Voice's SP0256) is what the console is modelled on.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No BIOS, character set, speech ROM or other copyrighted ROM is part of the
  app; you import your own. "Magnavox", "Odyssey 2", "Philips" and
  "Videopac" are trademarks of their owners, used to name the machines
  emulated; this project is not affiliated with them.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues)

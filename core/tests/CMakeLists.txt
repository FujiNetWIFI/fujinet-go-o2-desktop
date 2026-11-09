# core tests (see each file's header). Network and ROM-dependent tests exit
# 77, which ctest reports as skipped.

function(o2_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE o2_core)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    O2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# The core links and runs alone: no SDL, no session (a session symbol would
# fail the link).
o2_test(no_sdl_link_test no_sdl_link_test.c)
add_test(NAME no_sdl_link COMMAND no_sdl_link_test)

# The remembered pad -> port table (the "pad_assign" setting's text form):
# encode, decode, lookup by GUID then name, the cap, malformed input. Pure.
o2_test(pad_assign_test pad_assign_test.c)
target_include_directories(pad_assign_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src")
add_test(NAME pad_assign COMMAND pad_assign_test)

# Module tests live in <name>.test.cmake fragments beside their sources, each
# registering its own tests with o2_test() / add_test(); they are picked up
# here so a module can land with its tests without touching this file.
file(GLOB _o2_test_fragments "${CMAKE_CURRENT_SOURCE_DIR}/*.test.cmake")
list(SORT _o2_test_fragments)
foreach(_frag IN LISTS _o2_test_fragments)
  include("${_frag}")
endforeach()

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) only the core libraries build.
if(TARGET o2_session)

function(o2_session_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE o2_session)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    O2_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# The whole app headless, like a user: a session with the in-process
# FujiNet, a script of presses and waits, screenshots. Built, not registered.
o2_session_test(o2_drive o2_drive.c)

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots on the stand-in and reports the missing character
# set, input reaches the machine, a FujiNet app opens and runs, RESET restarts
# it, Reset to CONFIG ejects it, a game is refused until a BIOS is imported,
# an image the console cannot take is refused with a reason, the Videopac G7000
# and the Voice, and a Videopac+ G7400 with no BIOS does not start.
o2_session_test(session_test session_test.c)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 120)

# The remappable binding table and the keyboard translator, pure: MAME's key
# map, two sticks, F1 for RESET, steal-on-rebind, persistence, names.
o2_session_test(bindings_test bindings_test.c)
add_test(NAME bindings COMMAND bindings_test)

# The on-screen keyboard's layout tables for the Odyssey 2 and the Videopac+
# G7400: no overlapping caps, every matrix position reachable, labels unique
# and agreeing with the matrix, the caps where the console's photograph has
# them. No machine is started.
o2_session_test(keyboard_test keyboard_test.c)
add_test(NAME keyboard COMMAND keyboard_test)

# Import BIOS end to end, without a machine: every ROM item recognised by size
# and CRC whatever it is called and stored under MAME's name; a file, a zip
# (stored; deflated with fixed and dynamic Huffman blocks, by the reader's own
# INFLATE), a folder eight levels deep with zips in it; other consoles' dumps
# recognised and not stored; an unknown 1 KB dump taken only when it is the one
# file handed over; the G7400's two extra ROMs; the EF9340 bad-dump warning.
# Generated fillers with the right CRC (no firmware).
o2_session_test(roms_import_test roms_import_test.c)
add_test(NAME roms_import COMMAND roms_import_test)

# Where a dropped file goes (a ROM to the ROM directory, a cartridge to the
# cartridge directory, the rest to the SD folder), what makes a cartridge
# image acceptable, and Import Cartridge to SD.
o2_session_test(media_test media_test.c)
add_test(NAME media COMMAND media_test)

# The built-in Help covers what the app has: settings, machines, keys, ports,
# the debugger's tabs and prompt commands, and every context a Help button
# opens.
o2_session_test(help_test help_test.c)
add_test(NAME help COMMAND help_test)

# The external-FujiNet address field's parser.
o2_session_test(hostport_test hostport_test.c)
add_test(NAME hostport COMMAND hostport_test)

# External FujiNet: the cartridge dials a stand-in listener and no built-in
# runtime is started. SKIPs (77) on Windows (POSIX sockets).
o2_session_test(external_test external_test.c)
add_test(NAME external COMMAND external_test)
set_tests_properties(external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
o2_session_test(gamepad_test gamepad_test.c)
add_test(NAME gamepad COMMAND gamepad_test)

# The gamepad layer end to end with an SDL3 virtual gamepad: hot-plug, the
# default mapping onto the stick (D-pad, left analog stick, South), buttons
# rebound to a key and to RESET, the sources kept apart, unplug releasing what
# was held, and a pad's port assignment written to the settings, restored by a
# new session and dropped by Automatic. SKIPs (77) where SDL has no virtual
# joysticks.
o2_session_test(gamepad_map_test gamepad_map_test.c)
add_test(NAME gamepad_map COMMAND gamepad_map_test)
set_tests_properties(gamepad_map PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up (and the debugger's
# FujiNet trace saw both ways), and comes up again after a power cycle. SKIPs
# (77) when the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns
# the port.
o2_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# These two need the FujiNet runtime and the firmware's own test clients; they
# join once their sources are ported (see TODO).
if(FALSE)
# CONFIG, driven like a user, boots the suite's own app from the SD host
# through FujiNet (screens recognised by their text in the cart's text
# engine). SKIPs (77) without the FujiNet runtime. Serial: it owns the port.
o2_session_test(config_boot_test config_boot_test.c)
add_test(NAME config_boot COMMAND config_boot_test)
set_tests_properties(config_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujitest client makes a
# real transaction and fujiboot mounts S2COUNTER.BIN (put there by Import
# Cartridge to SD) from the SD host, FujiNet pushes it, the cart stages and
# swaps it in, and the stand-in runs it; twice, with the mailbox on its worker
# thread and inline. Needs O2_TESTROM_DIR (fujinet-firmware
# pico/o2/build, built with BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN); SKIPs
# without.
o2_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "O2_CART_SYNC=1")

endif()
endif()

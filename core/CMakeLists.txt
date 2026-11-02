# o2_core: the toolkit-agnostic, SDL-free Magnavox Odyssey 2 / Philips
# Videopac core.
#
#   o2/                the emulator: MAME's Odyssey 2 driver, its MCS-48
#                      CPU, the 8244/8245 VDC (and the G7400's EF9340/41 and
#                      8243), The Voice's SP0256 and the cart mappers
#                      transposed to C; the no-BIOS stand-in (stub.a48 /
#                      stub.c), the paced host thread and the FujiNet
#                      cartridge device.
#
#   o2/fuji-generated/ the cartridge firmware's OWN sources -- fujimail.c
#                      (the mailbox), o2map.c (the image planner), o2app.h
#                      (APPBANK serving), fujibus.c (the SLIP + FujiBus wire
#                      codec) and the baked CONFIG client -- staged by
#                      cmake/StageFujiProto.cmake. Compiling these rather
#                      than reimplementing them is what makes the emulated
#                      cartridge and the real RP2354B one identical by
#                      construction.
#
#   debugger/          the debugger engine (core/include/o2debug.h).
#
# The one port-specific cartridge file is o2/fujinet_cart.c, transposed from
# the firmware's MAME device (pico/o2/emu/fujinet.cpp). The transport is
# o2/fujitcp_host.c, this app's own portable twin of the staged fujitcp.c.
#
# Nothing here links SDL. The session layer (SDL3 audio + gamepad) is a
# separate target so that stays true; no_sdl_link guards it.

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/o2/fuji-generated")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet sources are missing. cmake/StageFujiProto.cmake should "
    "have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# The staged cartridge sources (the staged fujitcp.c is not compiled; see
# o2/fujitcp_host.c).
set(O2_FUJI_SOURCES
  ${FUJI_GEN}/fujimail.c
  ${FUJI_GEN}/fujibus.c
  ${FUJI_GEN}/o2map.c)

# The emulator's own modules. A module that has not landed yet is skipped, so
# the library builds at every step of the bring-up.
set(O2_EMU_SOURCES)
foreach(_f mcs48 dasm48 i8244 i8243 ef9340 sp0256 carts machine stub
           fujinet_cart fujitcp_host host)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/o2/${_f}.c")
    list(APPEND O2_EMU_SOURCES o2/${_f}.c)
  endif()
endforeach()

# The debugger drives the machine, so it joins once the machine and the
# debugger's own O2 port (vdcview.c is its marker) exist.
set(O2_DEBUGGER_SOURCES)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/o2/machine.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/vdcview.c")
  foreach(_f debugger.c expr.c fujitrace.c prompt.c symbols.c symbols_builtin.c vdcview.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/${_f}")
      list(APPEND O2_DEBUGGER_SOURCES debugger/${_f})
    endif()
  endforeach()
endif()

add_library(o2_core STATIC
  ${O2_EMU_SOURCES} ${O2_FUJI_SOURCES} ${O2_DEBUGGER_SOURCES})
set_target_properties(o2_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(o2_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/o2"
  # public because the debugger and the tests read the cart's state through
  # the staged fuji_mailbox.h, o2map.h and o2app.h
  "${FUJI_GEN}")
target_include_directories(o2_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/debugger")

# getaddrinfo/clock_gettime/nanosleep under a strict -std=c11.
target_compile_definitions(o2_core PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)

target_link_libraries(o2_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(o2_core PUBLIC ws2_32)
else()
  target_link_libraries(o2_core PUBLIC m)
endif()

# Our own modules are warning-clean; the staged tree is built with whatever
# warnings upstream is comfortable with.
set(O2_WARN_SOURCES ${O2_EMU_SOURCES} ${O2_DEBUGGER_SOURCES})
list(REMOVE_ITEM O2_WARN_SOURCES debugger/symbols_builtin.c)
set_property(SOURCE ${O2_WARN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>")

# ---- o2_session: the frontend contract ----------------------------------
#
# A separate target from o2_core so the core stays SDL-free: audio_sdl.c and
# gamepad_sdl.c live here. Found-or-skipped, exactly as the frontends are:
# `-DFRONTEND=none` on a machine (or a cross-toolchain) with no SDL3 must
# still configure and build the emulator and its tests. Not built until the
# machine and the host it drives exist.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/session.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/o2/host.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/o2/machine.c")
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    message(STATUS "o2_session: SDL3 not found -- building the core and its "
                   "tests only (no audio, no gamepads, no frontends)")
  else()
    include(session.cmake)
  endif()
endif()

add_subdirectory(tests)

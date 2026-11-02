# o2_session: the frontend contract (core/include/o2session.h), included
# from core/CMakeLists.txt once SDL3 is found.

set(O2_SESSION_SOURCES
  src/session.c
  src/settings.c
  src/paths.c
  src/media.c
  src/roms.c
  src/zipread.c
  src/keyboard.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c
  src/gamepad_sdl.c
  src/debug.c)

# The embedded ROM table: empty unless WITH_O2_ROMS (see COMPLIANCE.md). With
# it ON the developer's own images in tools/roms/ (the console BIOS, the
# character sets, The Voice's speech ROMs, loose or in MAME's zips) are
# compiled in for local development only; the shipping builds are OFF and
# core/tests/no_embedded_roms.py checks the binaries.
set(O2_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(WITH_O2_ROMS)
  file(GLOB O2_ROM_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/*")
  add_custom_command(
    OUTPUT "${O2_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${O2_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${O2_ROM_INPUTS}
    COMMENT "Embedding the ROM images in tools/roms (WITH_O2_ROMS=ON: NOT redistributable)"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${O2_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${O2_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
    VERBATIM)
endif()

add_library(o2_session STATIC ${O2_SESSION_SOURCES} "${O2_ROMS_C}")
set_target_properties(o2_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(o2_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(o2_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger")
target_link_libraries(o2_session PUBLIC o2_core SDL3::SDL3)
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(o2_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_property(SOURCE ${O2_SESSION_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>"
  # paths are bounded by O2_PATH_MAX and joined with snprintf on purpose;
  # GCC's truncation analysis flags every such join
  "$<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(o2_session PRIVATE
  O2_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  O2_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-o2"
  O2_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-o2")
if(WIN32)
  target_link_libraries(o2_session PUBLIC ws2_32)
else()
  target_link_libraries(o2_session PUBLIC ${CMAKE_DL_LIBS})
endif()

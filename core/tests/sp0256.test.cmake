# SP0256 speech chip (core/o2/sp0256.c). Included from core/tests/CMakeLists.txt,
# which defines o2_test(). See the headers of the test sources for what each
# one proves and how its goldens were made.

# No ROM needed: fuzz scenarios and hand-assembled microcode against goldens
# made with MAME's own sp0256.cpp, the ALD/LRQ/SBY protocol, the Voice's
# address decode, the clock arithmetic, the ring and the 48 kHz resampler.
o2_test(sp0256_test sp0256_test.c)
add_test(NAME sp0256 COMMAND sp0256_test)

# The Intellivoice's mask ROM (never in the repository; see
# sp0256_romfind.h for where it is looked for): "hello" and all 64 allophones
# against the samples the real MAME binary produced. SKIPs (77) without it.
add_test(NAME sp0256_rom COMMAND sp0256_test rom)
set_tests_properties(sp0256_rom PROPERTIES SKIP_RETURN_CODE 77)

# A second opinion on the LPC decode: jzIntv's own ivoice.c compiled into the
# test. The jzIntv tree is not part of this repository; it is found from
# -DSP0256_JZINTV_SRC=<dir>, or ~/Workspace/jzintv-20200712-src, or the
# Intellivision desktop's third_party/jzintv. Without it (or on a compiler or
# platform it was not tried on) the test is still built, as a skip (77).
set(SP0256_JZINTV_SRC "" CACHE PATH
    "jzIntv source tree for the optional sp0256_jz cross-check (needs src/ivoice/ivoice.c)")
set(_sp0256_jz "")
foreach(_c "${SP0256_JZINTV_SRC}"
           "$ENV{HOME}/Workspace/jzintv-20200712-src"
           "$ENV{HOME}/Workspace/fujinet-go-intv-desktop/third_party/jzintv")
  if(NOT _sp0256_jz AND _c AND EXISTS "${_c}/src/ivoice/ivoice.c")
    set(_sp0256_jz "${_c}")
  endif()
endforeach()

o2_test(sp0256_jz_test sp0256_jz_test.c)
if(_sp0256_jz AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  message(STATUS "sp0256_jz: cross-checking against ${_sp0256_jz}/src/ivoice/ivoice.c")
  target_compile_definitions(sp0256_jz_test PRIVATE
    SP0256_JZ_IVOICE_C="${_sp0256_jz}/src/ivoice/ivoice.c")
  target_include_directories(sp0256_jz_test PRIVATE
    "${_sp0256_jz}/src" "${_sp0256_jz}/src/ivoice")
  # jzIntv's ivoice.c references services of its host (display, files) from
  # functions this test never calls: drop them at link time rather than stub them
  target_compile_options(sp0256_jz_test PRIVATE -w -ffunction-sections)
  target_link_options(sp0256_jz_test PRIVATE -Wl,--gc-sections)
endif()
add_test(NAME sp0256_jz COMMAND sp0256_jz_test)
set_tests_properties(sp0256_jz PROPERTIES SKIP_RETURN_CODE 77)

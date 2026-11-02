# The 8048 core's tests (included by core/tests/CMakeLists.txt, which defines
# o2_test()). They link o2_core, which compiles core/o2/mcs48.c and dasm48.c.
#
#   dasm          the disassembler against MAME's own (dasm_golden.h, from
#                 mcs48dsm.cpp compiled standalone: tools/ab/gen_dasm_golden.sh):
#                 all 256 opcodes' text, length and step flags at four pc /
#                 operand pairs, branch targets, and the disassembler against
#                 the core (the same 26 illegal opcodes, the same lengths).
#   cpu_ops       the core against MAME's CPU, instruction for instruction:
#                 fifteen probe programs (tools/ab/probes/*.a48, every legal
#                 opcode and the timer, counter, interrupt, bank and port
#                 cases) whose traces from MAME's odyssey2 are committed as
#                 hashes in mcs48_golden.h (tools/ab/mcs48_ab.py --gen); needs
#                 neither MAME nor AS. Also the core's own API checks.
#   cpu_ops_mame  the same probes assembled with AS and run in a live MAME,
#                 the whole traces diffed against the core's. SKIPs (77)
#                 without AS ($ASL, ~/asl/asl) or MAME ($MAME,
#                 ~/Workspace/mame/mame).

o2_test(dasm_test dasm_test.c)
add_test(NAME dasm COMMAND dasm_test)

o2_test(mcs48_ops_test mcs48_ops_test.c)
add_test(NAME cpu_ops COMMAND mcs48_ops_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME cpu_ops_mame
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/mcs48_ab.py"
            --check --exe $<TARGET_FILE:mcs48_ops_test>)
  set_tests_properties(cpu_ops_mame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

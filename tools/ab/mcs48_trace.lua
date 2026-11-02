-- mcs48_trace.lua -- MAME side of the MCS-48 conformance test: logs the 8048's
-- state before every instruction of a probe program, then exits when the probe
-- reaches its end marker.
--
-- Run it as an -autoboot_script of `mame odyssey2 -debug -debugger none ...`
-- (tools/ab/mcs48_ab.py does). The debugger front end "none" never stops the
-- CPU, so the trace is set up from here with the debugger's own `trace`
-- command and its `tracelog` action, which MAME runs before each instruction.
--
-- Environment:
--   MCS_TRACE   the trace file to write
--   MCS_DONE    the probe's end marker address, hex (default 3FE): a
--               breakpoint there ends the run, and a frame limit backs it up
--   MCS_FRAMES  the frame limit (default 20)
--
-- One line per instruction, the state BEFORE it executes, all hex but the last:
--   pc a psw tc tpre p1 p2 r0 r1 r2 r3 r4 r5 r6 r7 totalcycles
-- (psw is MAME's `curflags`, the whole PSW with the stack pointer; r0-r7 are
-- the active bank; totalcycles is the machine cycles since power-on.)
--
-- SPDX-License-Identifier: GPL-3.0-or-later

local trace_file = os.getenv("MCS_TRACE") or "mcs48.trace"
local done = os.getenv("MCS_DONE") or "3FE"
local limit = tonumber(os.getenv("MCS_FRAMES") or "20")

local dbg = manager.machine.debugger
local fmt = "%03X %02X %02X %02X %02X %02X %02X " ..
            "%02X %02X %02X %02X %02X %02X %02X %02X %d\\n"
dbg:command('trace ' .. trace_file ..
            ',maincpu,noloop,{tracelog "' .. fmt ..
            '",pc,a,curflags,tc,tpre,p1,p2,r0,r1,r2,r3,r4,r5,r6,r7,totalcycles}')
-- the breakpoint's action quits MAME; the trace flushes after every line
dbg:command('bpset ' .. done .. ',1,{quit}')

local frames = 0
emu.add_machine_frame_notifier(function()
  frames = frames + 1
  if frames >= limit then
    manager.machine:exit()
  end
end)

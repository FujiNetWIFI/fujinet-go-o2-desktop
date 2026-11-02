-- vdc_log.lua -- log every 8244/8245 access (and optionally dump frames) from
-- MAME's odyssey2 / videopac driver, with the time of the access in PIXEL
-- CLOCKS, so the C chip (core/o2/i8244.c) can replay the same accesses.
--
-- Environment:
--   VDC_LOG     output text file (required)
--   VDC_FRAMES  binary file: every frame's 360x243 pixels, 32-bit LE ARGB, in
--               order (optional); VDC_FRAME_FROM skips the frames before it
--   VDC_PAL=1   PAL machine (pixel clock 7093790 Hz instead of 7159090)
--   VDC_FRAMES_N  stop after this many frames (default 60)
--   VDC_IRAM    "a:n" print n bytes of the 8048's internal RAM at the end
--   VDC_P1TRACE=1  also log `P <t> <p1>` at every change of P1 (slow)
--   VDC_PRESS   "frame:port:mask:hold,..." input presses (see below)
--
-- Log lines:  W <t> <off> <data> <p1>   a write to the I/O space ($00-$FF)
--             R <t> <off> <data> <p1>   a read (data = what the bus returned)
--             F <n> <t> <p1>            frame n ended
--             P <t> <p1>                P1 changed (VDC_P1TRACE only)
-- The tap sees every I/O access whatever P1 selects; the C replay applies the
-- chip-select gating itself from <p1>.
-- MAME runs this chunk on a coroutine that the collector frees once the chunk
-- returns; the taps and the notifier keep using its lua_State, so one GC cycle
-- crashes MAME.  Never collect (attotime objects leak a little: ~100 bytes an
-- access), and write numbers without making strings.
collectgarbage("stop")

local out = assert(io.open(os.getenv("VDC_LOG"), "w"))
local framesf = os.getenv("VDC_FRAMES") and assert(io.open(os.getenv("VDC_FRAMES"), "wb")) or nil
local from = tonumber(os.getenv("VDC_FRAME_FROM") or "0")
local nmax = tonumber(os.getenv("VDC_FRAMES_N") or "60")
local pixhz = os.getenv("VDC_PAL") == "1" and 7093790.0 or 7159090.0
local cpu = manager.machine.devices[":maincpu"]
local io_space = cpu.spaces["io"]
local p1item = emu.item(manager.machine.devices[":"].items["0/m_p1"])
local scr = manager.machine.screens[":screen"]

-- The time in PIXEL CLOCKS the way MAME's own screen turns it into a beam
-- position: floor((t + pixeltime / 2) / pixeltime) with pixeltime the
-- attoseconds of one pixel clock, truncated (HZ_TO_ATTOSECONDS).  Integer
-- arithmetic: good up to 9 emulated seconds.
local PIX = 1000000000000000000 // math.tointeger(pixhz)
local function now_px()
  local tm = manager.machine.time
  return (tm.seconds * 1000000000000000000 + tm.attoseconds + PIX // 2) // PIX
end

local tw = io_space:install_write_tap(0, 0xff, "vdc_w", function(off, data, mask)
  out:write("W ", now_px(), " ", off, " ", data, " ", p1item:read(0), "\n")
end)
local tr = io_space:install_read_tap(0, 0xff, "vdc_r", function(off, data, mask)
  out:write("R ", now_px(), " ", off, " ", data, " ", p1item:read(0), "\n")
end)

-- VDC_P1TRACE=1: log every change of P1 with its exact time (a tap on the
-- program space sees the instruction fetch that follows the P1 write, which
-- is when the write happened).  Slow (a Lua call per opcode fetch).
local lastp1 = p1item:read(0)
local tp = nil
if os.getenv("VDC_P1TRACE") == "1" then
  tp = cpu.spaces["program"]:install_read_tap(0, 0xfff, "p1trace", function(off, data, mask)
    local v = p1item:read(0)
    if v ~= lastp1 then
      out:write("P ", now_px(), " ", v, "\n")
      lastp1 = v
    end
  end)
end

-- VDC_PRESS: "frame:port:mask:hold,..." presses the bits `mask` of an input
-- port (":KEY.0" ... ":KEY.7", ":JOY.0", ":JOY.1") for `hold` frames from `frame`
local presses = {}
for spec in string.gmatch(os.getenv("VDC_PRESS") or "", "[^,]+") do
  local f, p, m, h = string.match(spec, "(%d+):([^:]+):(%d+):(%d+)")
  if f then presses[#presses + 1] = { f = tonumber(f), port = p, mask = tonumber(m), hold = tonumber(h) } end
end
local function setport(portname, mask)
  if string.sub(portname, 1, 1) ~= ":" then portname = ":" .. portname end
  local port = manager.machine.ioport.ports[portname]
  if not port then return end
  for _, fld in pairs(port.fields) do
    fld:set_value((fld.mask & mask) ~= 0 and 1 or 0)
  end
end

local n = 0
emu.add_machine_frame_notifier(function()
  n = n + 1
  for _, pr in ipairs(presses) do
    if n == pr.f then setport(pr.port, pr.mask) end
    if n == pr.f + pr.hold then setport(pr.port, 0) end
  end
  out:write("F ", n, " ", now_px(), " ", p1item:read(0), "\n")
  if framesf and n >= from then
    -- pixels() returns (string, width, height): keep only the string
    local px = scr:pixels()
    framesf:write(px)
  end
  if n >= nmax then
    local spec = os.getenv("VDC_IRAM")
    if spec then
      local a, c = spec:match("(%d+):(%d+)")
      local t = {}
      local d = cpu.spaces["data"]
      for k = 0, tonumber(c) - 1 do t[#t + 1] = string.format("%02X", d:read_u8(tonumber(a) + k)) end
      out:write("IRAM " .. table.concat(t, " ") .. "\n")
    end
    out:flush()
    out:close()
    if framesf then framesf:close() end
    manager.machine:exit()
  end
end)

#!/usr/bin/env python3
"""o2cart.py -- build tiny 8048 test cartridges whose VDC accesses happen at
EXACT, planned times.

A cart is a straight line of operations (no interrupts, no loops except the
delay loops), so the time of every access is known to the cycle.  The BIOS
hands over to the cart at 0x400; the first thing the cart does is write a
MARKER (0xA5 to the unused register $A6) which the logger sees, which pins the
absolute time of everything after it.  Build a cart once with a placeholder
(`marker_cycle=None`) to measure the marker's time, then again with it, and
operations can be placed at absolute pixel-clock times (`at()`).

Costs (8048 machine cycles): mov r,#n 2; mov a,#n 2; movx @r0,a 2; movx a,@r0 2;
outl p1,a 2; orl/anl p1,#n 2; nop 1; djnz 2; jmp 2.  A VDC access happens at
the END of its MOVX (MAME burns an instruction's cycles first), so
   w(addr, val)  = mov r0,#addr; mov a,#val; movx @r0,a   -> access at +6
   r(addr)       = mov r0,#addr; movx a,@r0               -> access at +4
"""
import os, sys, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import o2ab

# P1: bit7 LUM (1 = off), bit6 copy mode (0 = VDC reads and cart writes on),
# bit5 EF9341 (1 = off), bit4 RAM (1 = off), bit3 VDC select (0 = on),
# bit2 keyboard (1 = off), bits1-0 bank
P1_VDC = 0x37          # LUM bright (P17 = 0): VDC selected, reads enabled
P1_VDC_DIM = 0xB7      # LUM off (P17 = 1)

MARKER = (0xA6, 0xA5)

# MAME runs the 8048 slightly FASTER than the exact 20 (18) pixel clocks per
# machine cycle: its XTAL class rounds 7.15909 MHz to 7.1591 MHz for the CPU
# clock ((7.1591 MHz * 3) / 4 = 5,369,325 Hz; PAL 17.7345 MHz / 3 = 5,911,500
# Hz) while the screen keeps 7,159,090 Hz (7,093,790.4 Hz).  So a cycle is
# 19.999972 (17.999975) pixels, 1.4 ppm short: ~1 pixel every 36k cycles.  The
# access log shows it; the cart model and the C replay use the same ratio.
CPU_HZ = (5369325.0, 5911500.0)
PIXEL_HZ = (7159090.0, 7093790.0)


def cycle_px(pal):
    return 15.0 * PIXEL_HZ[1 if pal else 0] / CPU_HZ[1 if pal else 0]


def cycles_to_px(c, pal):
    return int(c * cycle_px(pal) + 0.5)


class Cart:
    def __init__(self, pal=False, p1=P1_VDC_DIM):
        self.pal = pal
        self.cyc_px = cycle_px(pal)
        self.lines = []
        self.cum = 0                  # cycles since the marker's access
        self.marker_cycle = None      # absolute cycle of the marker access
        self.label = 0
        self.p1 = p1
        self.events = []              # model: (kind, cycle_after_marker, off, data)
        self.addr = 0x400
        self._header(p1)

    def ensure_room(self, n):
        """Sequential execution wraps inside a 2K bank (0x7FF -> 0x000), so
        code has to JMP across 0x800: `sel mb1; jmp` (3 cycles, AS adds the sel) when `n` more
        bytes would not fit below it."""
        if self.addr < 0x800 and self.addr + n > 0x7fb:
            # (AS puts the `sel mb1` in front of a jmp into the other bank itself)
            self.lines += ["        jmp bk1", "        org 800h", "bk1:"]
            self.cum += 3
            self.addr = 0x800

    def _emit(self, text, cycles, size=0):
        self.ensure_room(size)
        self.lines.append(text)
        self.cum += cycles
        self.addr += size

    def _header(self, p1):
        self.lines += [
            "        cpu 8048",
            "        org 400h",
            "        jmp start",         # 400 reset
            "        jmp spin",          # 402 irq
            "        jmp spin",          # 404 timer
            "        jmp spin",          # 406
            "        jmp spin",          # 408
            "        jmp spin",          # 40a
            "spin:   jmp spin",
            "start:  dis i",
            "        dis tcnti",
            "        mov a,#%d" % p1,
            "        outl p1,a",
            "        mov r0,#%d" % MARKER[0],
            "        mov a,#%d" % MARKER[1],
            "        movx @r0,a",
        ]
        self.addr = 0x400 + 14 + 10    # header + start: dis i, dis tcnti, mov, outl, mov, mov, movx
        # cum is counted from the marker's access (its last cycle)

    # ---- operations -----------------------------------------------------

    def w(self, addr, val):
        self._emit("        mov r0,#%d\n        mov a,#%d\n        movx @r0,a" % (addr, val & 0xff), 6, 5)
        self.events.append(('W', self.cum, addr, val & 0xff))

    def r(self, addr):
        self._emit("        mov r0,#%d\n        movx a,@r0" % addr, 4, 3)
        self.events.append(('R', self.cum, addr, None))

    def p1_set(self, val):
        """outl p1 (the P1 write happens at the end of the instruction)."""
        self._emit("        mov a,#%d\n        outl p1,a" % val, 4, 3)
        self.events.append(('P1', self.cum, 0, val))
        self.p1 = val

    def nop(self, n=1):
        self._emit(("        nop\n" * n).rstrip("\n"), n, n)

    def _plan(self, cycles):
        """Pieces ('loop2', a), ('loop1', n), ('nop', k) burning exactly
        `cycles` cycles."""
        pieces = []
        rem = cycles
        if rem >= 2 + 504 + 2:
            a = min((rem - 2) // 504, 255)
            pieces.append(('loop2', a))
            rem -= 2 + a * 504
        if rem >= 4:
            n = min((rem - 2) // 2, 255)
            pieces.append(('loop1', n))
            rem -= 2 + 2 * n
        if rem:
            pieces.append(('nop', rem))
        return pieces

    def delay(self, cycles):
        """Burn exactly `cycles` cycles.  The loops are single-page (the 8048's
        conditional jumps are page-relative), so nops are put in front when a
        loop would straddle a 256-byte page."""
        if cycles < 0:
            raise ValueError("negative delay %d" % cycles)
        if cycles == 0:
            return
        self.ensure_room(40)
        for pad in range(0, 24):
            if cycles - pad < 0:
                break
            pieces = self._plan(cycles - pad)
            addr = self.addr + pad
            ok = True
            for kind, v in pieces:
                size = {'loop2': 8, 'loop1': 4, 'nop': v}[kind]
                if kind != 'nop' and (addr & 0xff) + size > 0xfe:
                    ok = False
                    break
                addr += size
            if ok:
                break
        else:
            raise ValueError("no page-safe delay of %d cycles at %x" % (cycles, self.addr))
        out = ["        nop"] * pad
        for kind, v in pieces:
            if kind == 'loop2':
                lab, lab2 = self._lab(), self._lab()
                out += ["        mov r2,#%d" % v, "%s: mov r3,#250" % lab,
                        "%s: djnz r3,%s" % (lab2, lab2), "        djnz r2,%s" % lab]
            elif kind == 'loop1':
                lab = self._lab()
                out += ["        mov r4,#%d" % v, "%s: djnz r4,%s" % (lab, lab)]
            else:
                out += ["        nop"] * v
        total_size = pad + sum({'loop2': 8, 'loop1': 4, 'nop': v}[k] for k, v in pieces)
        self._emit("\n".join(out), cycles, total_size)

    def loop_begin(self, n):
        """Run the body n times.  The body must stay below 0x7F0 and be
        straight-line; loop_end() puts the tail at 0x7F0 (page-safe) and the
        code after the loop in bank 1 (0x800)."""
        self.ensure_room(40)
        if self.addr >= 0x400 + 0x3e0:
            raise ValueError("loop starts too high")
        self._emit("        mov r5,#%d" % n, 2, 2)
        self.lines.append("top%d:" % self.label)
        self._loop = dict(n=n, top=self.label, cum0=self.cum, ev0=len(self.events))
        self.label += 1

    def loop_end(self, period):
        """Pad the body to `period` cycles per iteration (including the 6 of
        the tail), then repeat the model's events for iterations 2..n."""
        lp = self._loop
        body = self.cum - lp['cum0']
        pad = period - body - 6
        if pad < 0:
            raise ValueError("loop body is %d cycles, period %d" % (body, period))
        if self.addr >= 0x800:
            raise ValueError("loop body crossed into bank 1")
        self.delay(pad)
        if self.addr > 0x7ee:
            raise ValueError("loop body too long: %x" % self.addr)
        self.lines += ["        jmp tail%d" % lp['top'], "        org 7F0h", "tail%d:" % lp['top'],
                       "        djnz r5,again%d" % lp['top'], "        jmp after%d" % lp['top'],
                       "again%d: jmp top%d" % (lp['top'], lp['top']), "        org 800h", "after%d:" % lp['top']]
        self.cum += 6                       # jmp tail, djnz, jmp top
        iter_cycles = self.cum - lp['cum0']
        assert iter_cycles == period
        evs = self.events[lp['ev0']:]
        for k in range(1, lp['n']):
            for kind, c, off, data in evs:
                self.events.append((kind, c + k * period, off, data))
        self.cum = lp['cum0'] + lp['n'] * period + 1    # the last pass leaves through sel mb1 + jmp
        self.addr = 0x800

    def _lab(self):
        self.label += 1
        return "L%d" % self.label

    def at(self, target_px, op_cycles=6):
        """Delay so that an op of `op_cycles` cycles issued next accesses at
        about absolute pixel time target_px (rounded to a whole cycle)."""
        if self.marker_cycle is None:
            return
        self.ensure_room(48)
        now_cyc = self.marker_cycle + self.cum
        want = int(target_px / self.cyc_px + 0.5) - op_cycles
        if want < now_cyc:
            raise ValueError("at(%d): already at cycle %d, need %d" % (target_px, now_cyc, want))
        self.delay(want - now_cyc)

    def now_px(self):
        return cycles_to_px(self.marker_cycle + self.cum, self.pal)

    def size_check(self):
        if self.addr > 0xbf0:
            raise ValueError("cart too big: %x" % self.addr)

    def source(self):
        self.size_check()
        return "\n".join(self.lines + ["idle:   jmp idle", "        end", ""])

    def build(self, workdir, name):
        src = os.path.join(workdir, name + ".a48")
        binf = os.path.join(workdir, name + ".bin")
        open(src, "w").write(self.source())
        o2ab.assemble(src, binf)
        return binf


def find_marker(events):
    """The absolute time (pixel clocks) of the marker write in a parsed log."""
    for k, e in enumerate(events):
        if e[0] == 'W' and e[2] == MARKER[0] and e[3] == MARKER[1]:
            return k, e[1]
    raise SystemExit("marker not found in log")

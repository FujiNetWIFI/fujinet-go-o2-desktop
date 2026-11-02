#!/usr/bin/env python3
"""scenes.py -- the 8244 test scenes: register programs for the A/B carts.

A scene is a function building a `Scene`: an ordered list of register writes
(display off), the final control write, a settle time and probes (reads) of
the status registers.  gen_goldens.py turns each into a cart (o2cart.py), runs
it in MAME, and records what MAME did.
"""
import random

CTL_FILL, CTL_DOTS, CTL_FG, CTL_EXT, CTL_GRID, CTL_SND, CTL_STROBE, CTL_B0 = \
    0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01


class Scene:
    def __init__(self, name, pal=False, frames=10):
        self.name = name
        self.pal = pal
        self.frames = frames
        self.writes = []          # (addr, value) in order, before the enable
        self.control = 0          # final $A0
        self.mask = 0             # $A2 collision mask
        self.color = 0            # $A3
        self.lum = 1              # P17 (1 = off)
        self.probes = []          # addresses read after the settle
        self.settle_frames = 6    # the probes run this many frames in
        self.readback_addrs = []  # registers read back before the enable

    # -- register helpers (offsets as in vdc_t) ---------------------------
    def reg(self, a, v):
        self.writes.append((a, v))

    def sprite(self, i, y, x, ctrl, shape):
        self.reg(i * 4, y)
        self.reg(i * 4 + 1, x)
        self.reg(i * 4 + 2, ctrl)
        for k, b in enumerate(shape):
            self.reg(0x80 + i * 8 + k, b)

    def char(self, i, y, x, ptr, color):
        self.reg(0x10 + i * 4, y)
        self.reg(0x10 + i * 4 + 1, x)
        self.reg(0x10 + i * 4 + 2, ptr)
        self.reg(0x10 + i * 4 + 3, color)

    def quad(self, i, y, x, chars):
        """chars: four (ptr, color); x/y written to the first (and, like the
        BIOS does, to the mirrored three)."""
        b = 0x40 + i * 16
        for j, (ptr, color) in enumerate(chars):
            self.reg(b + j * 4, y)
            self.reg(b + j * 4 + 1, x)
            self.reg(b + j * 4 + 2, ptr)
            self.reg(b + j * 4 + 3, color)

    def hgrid(self, col, bits9):
        self.reg(0xc0 + col, bits9 & 0xff)
        self.reg(0xd0 + col, (bits9 >> 8) & 1)

    def vgrid(self, col, bits8):
        self.reg(0xe0 + col, bits8 & 0xff)


def sprite_shape(rng, density=0.5):
    return [sum((1 << b) for b in range(8) if rng.random() < density) for _ in range(8)]


def fixed_shape(k):
    shapes = [
        [0x18, 0x3c, 0x7e, 0xff, 0xff, 0x7e, 0x3c, 0x18],   # diamond
        [0xff, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xff],   # box
        [0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff],   # ramp
        [0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55],   # checker
    ]
    return shapes[k % 4]


def build_scenes():
    sc = []

    # ---- background and luminance ---------------------------------------
    for bg in range(8):
        s = Scene("bg%d" % bg, frames=8)
        s.color = ((bg >> 2 & 1) << 3) | ((bg >> 1 & 1) << 4) | ((bg & 1) << 5)   # bitswap<3>(color,3,4,5)
        s.settle_frames = 4
        s.probes = [0xa1, 0xa2]
        sc.append(s)
    s = Scene("bg_lum", frames=8)
    s.color = 0x2b
    s.lum = 0
    s.settle_frames = 4
    s.probes = [0xa1]
    sc.append(s)

    # ---- grid ----------------------------------------------------------------
    def grid_all(name, control, color, hmask=0x1ff, vmask=0xff, lum=1):
        s = Scene(name, frames=9)
        for c in range(9):
            s.hgrid(c, hmask if c % 2 == 0 else (hmask ^ 0x0aa) & 0x1ff)
        for c in range(10):
            s.vgrid(c, vmask if c % 3 else (vmask ^ 0x55) & 0xff)
        s.color = color
        s.control = control
        s.lum = lum
        s.probes = [0xa1, 0xa2]
        return s
    sc.append(grid_all("grid_lines", CTL_GRID, 0x0e | 0x08))
    sc.append(grid_all("grid_fill", CTL_GRID | CTL_FILL, 0x41 | 0x10))
    sc.append(grid_all("grid_dots", CTL_GRID | CTL_DOTS, 0x45 | 0x20, hmask=0x0f0, vmask=0x0f))
    sc.append(grid_all("grid_dotsfill", CTL_GRID | CTL_DOTS | CTL_FILL, 0x03 | 0x18, hmask=0x18c, vmask=0x81))
    sc.append(grid_all("grid_off_regs_set", 0, 0x07))        # regs written, grid disabled
    sc.append(grid_all("grid_lum", CTL_GRID, 0x49 | 0x28, lum=0))

    # ---- sprites -------------------------------------------------------------
    s = Scene("sprites_basic")
    for i in range(4):
        s.sprite(i, 40 + i * 30, 20 + i * 40, (i + 1) << 3, fixed_shape(i))
    s.control = CTL_FG
    s.color = 0x00
    s.probes = [0xa1, 0xa2]
    sc.append(s)
    s = Scene("sprites_zoom_s_x9")
    for i in range(4):
        # control bits: bit2 zoom, bit1 S, bit0 X9
        s.sprite(i, 30 + i * 45, 30 + i * 33, ((i + 2) << 3) | (i & 1) << 2 | ((i >> 1) & 1) << 1 | 1 if i % 2 else ((i + 2) << 3) | (i << 0 & 7), fixed_shape(i + 1))
    s.control = CTL_FG
    s.probes = [0xa2]
    sc.append(s)
    rng = random.Random(0x5115)
    for nm, ctl_lo in (("sprites_attr0", 0), ("sprites_attr1", 1), ("sprites_attr2", 2), ("sprites_attr3", 3),
                       ("sprites_attr4", 4), ("sprites_attr5", 5), ("sprites_attr6", 6), ("sprites_attr7", 7)):
        s = Scene(nm, frames=8)
        for i in range(4):
            s.sprite(i, 24 + i * 50, 24 + i * 11, (((rng.randrange(8)) << 3) | ctl_lo), sprite_shape(rng, 0.55))
        s.control = CTL_FG
        s.color = rng.randrange(0x80)
        s.probes = [0xa2]
        s.settle_frames = 4
        sc.append(s)
    # overlapping sprites with every collision mask bit
    for m in (0x01, 0x04, 0x10, 0x40, 0x80, 0xff):
        s = Scene("sprite_coll_%02x" % m, frames=8)
        s.sprite(0, 80, 60, 0x30, fixed_shape(1))
        s.sprite(1, 84, 66, 0x08 | 4, fixed_shape(3))
        s.sprite(2, 90, 130, 0x28, fixed_shape(0))
        s.sprite(3, 100, 132, 0x10 | 2, fixed_shape(2))
        s.hgrid(3, 0x1ff)
        s.vgrid(3, 0xff)
        s.char(0, 90, 70, 0, 0x04)
        s.control = CTL_FG | CTL_GRID
        s.mask = m
        s.probes = [0xa2, 0xa2, 0xa1]
        s.settle_frames = 4
        sc.append(s)
    # sprite at the screen edges / y wrap
    s = Scene("sprites_edges")
    s.sprite(0, 0, 0, 0x30, fixed_shape(0))
    s.sprite(1, 236, 170, 0x08 | 4, fixed_shape(1))
    s.sprite(2, 200, 250, 0x28, fixed_shape(2))
    s.sprite(3, 255, 100, 0x10, fixed_shape(3))
    s.control = CTL_FG
    s.probes = [0xa2]
    sc.append(s)

    # ---- single characters --------------------------------------------------------
    s = Scene("chars_basic")
    for i in range(12):
        s.char(i, 40 + (i // 4) * 30, 8 + (i % 4) * 36, (i * 19 + 3) & 0xff, (i % 8) << 1 | (i & 1))
    s.control = CTL_FG
    s.probes = [0xa1, 0xa2]
    sc.append(s)
    s = Scene("chars_y_ntsc_rule")
    for i in range(12):
        s.char(i, i * 2, 10 + i * 12, 8 * (i + 1), (i % 8) << 1)
    s.control = CTL_FG
    s.probes = [0xa1]
    sc.append(s)
    s = Scene("chars_overlap")
    # overlapping singles: priority, self-overlap flag ($A1 bit 7), collisions
    for i in range(12):
        s.char(i, 60 + (i % 3) * 4, 40 + i * 5, (i * 37) & 0xff, (i * 3) & 0xf)
    s.control = CTL_FG
    s.mask = 0x80
    s.probes = [0xa1, 0xa2]
    sc.append(s)
    s = Scene("chars_heights")
    for i in range(12):
        # the pointer sets where in the glyph (and so how many rows) it starts
        s.char(i, 30 + i * 2 + (i // 6) * 60, 8 + (i % 6) * 28, i, (i % 7) << 1)
    s.control = CTL_FG
    s.probes = [0xa1]
    sc.append(s)

    # ---- quads ------------------------------------------------------------------------
    s = Scene("quads_basic")
    for i in range(4):
        s.quad(i, 30 + i * 40, 10 + i * 6, [((i * 16 + j * 8) & 0xff, (j * 2 + i) & 0xf) for j in range(4)])
    s.control = CTL_FG
    s.probes = [0xa1, 0xa2]
    sc.append(s)
    s = Scene("quads_overlap_singles")
    for i in range(4):
        s.quad(i, 50 + i * 3, 20 + i * 8, [((i * 50 + j * 13) & 0xff, (j * 2 + 1) & 0xf) for j in range(4)])
    for i in range(12):
        s.char(i, 54 + (i % 3) * 2, 18 + i * 10, (i * 11) & 0xff, (i % 8) << 1)
    s.control = CTL_FG
    s.mask = 0x80
    s.probes = [0xa1, 0xa2]
    sc.append(s)
    s = Scene("quads_top_ntsc")
    for i in range(4):
        s.quad(i, i * 4, 30, [(j * 20, j * 2) for j in range(4)])
    s.control = CTL_FG
    sc.append(s)

    # ---- everything together, various controls -----------------------------------
    for ctl in (CTL_FG | CTL_GRID, CTL_FG | CTL_GRID | CTL_DOTS, CTL_FG | CTL_GRID | CTL_FILL | CTL_DOTS,
                CTL_FG | CTL_EXT | CTL_SND, CTL_FG | CTL_B0, CTL_GRID | CTL_B0 | CTL_DOTS):
        rng = random.Random(0xA000 + ctl)
        s = Scene("combo_%02x" % ctl, frames=8)
        for i in range(4):
            s.sprite(i, rng.randrange(20, 220), rng.randrange(0, 170), (rng.randrange(8) << 3) | rng.randrange(8),
                     sprite_shape(rng, 0.5))
        for i in range(12):
            s.char(i, rng.randrange(14, 230), rng.randrange(0, 170), rng.randrange(256), rng.randrange(16))
        for i in range(4):
            s.quad(i, rng.randrange(14, 220), rng.randrange(0, 130),
                   [(rng.randrange(256), rng.randrange(16)) for _ in range(4)])
        for c in range(9):
            s.hgrid(c, rng.randrange(512))
        for c in range(10):
            s.vgrid(c, rng.randrange(256))
        s.color = rng.randrange(0x80)
        s.mask = rng.randrange(256)
        s.control = ctl
        s.probes = [0xa1, 0xa2, 0xa1]
        s.settle_frames = 4
        sc.append(s)

    # ---- random scenes ------------------------------------------------------------------
    for n in range(16):
        rng = random.Random(0xC0DE0000 + n)
        s = Scene("rand%02d" % n, frames=7)
        dens = rng.choice((0.2, 0.5, 0.8))
        for i in range(4):
            s.sprite(i, rng.randrange(0, 236), rng.randrange(0, 180), rng.randrange(256), sprite_shape(rng, dens))
        for i in range(12):
            s.char(i, rng.randrange(0, 240), rng.randrange(0, 200), rng.randrange(256), rng.randrange(256))
        for i in range(4):
            s.quad(i, rng.randrange(0, 240), rng.randrange(0, 200),
                   [(rng.randrange(256), rng.randrange(256)) for _ in range(4)])
        for c in range(9):
            s.hgrid(c, rng.randrange(512))
        for c in range(10):
            s.vgrid(c, rng.randrange(256))
        s.color = rng.randrange(256)
        s.mask = rng.randrange(256)
        s.control = rng.randrange(256) | (CTL_FG if n % 3 else 0)
        s.control &= ~CTL_SND
        s.lum = rng.randrange(2)
        s.probes = [0xa1, 0xa2, 0xa2]
        s.settle_frames = 4
        sc.append(s)

    # ---- register semantics: mirrors, masks, lockouts, read back -------------------
    for part, rngs in (("lo", range(0x00, 0xa0)), ("hi", range(0xa0, 0x100))):
        s = Scene("regsem_" + part, frames=6)
        rng = random.Random(0x5E4A + len(part) + rngs[0])
        for a in rngs:
            if a not in (0xa0, 0xb0, 0xa2, 0xb2, 0xa3, 0xb3):
                s.reg(a, rng.randrange(256))              # junk into everything
        # the quads' mirrored X/Y, $B0-$BF mirrors, grid high bytes: more junk
        for a in (0x44, 0x45, 0x48, 0x49, 0x4c, 0x4d, 0x54, 0x55, 0x58, 0x59, 0x41, 0x61, 0xb1, 0xb4, 0xb5, 0xb7, 0xb8, 0xba):
            if part == "lo" and a >= 0xa0:
                continue
            s.reg(a, rng.randrange(256))
        s.readback_addrs = list(range(0x00, 0xa0)) if part == "lo" else list(range(0xa0, 0x100)) + [0xb0, 0xb1, 0xb4, 0xb5, 0xb7]
        s.control = CTL_FG | CTL_GRID | CTL_DOTS
        s.color = rng.randrange(256)
        s.mask = rng.randrange(256)
        s.probes = [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xb0, 0xb1, 0xb2, 0xb3, 0x40, 0x44, 0x20, 0x82, 0xc0, 0xe3, 0xd0, 0x00, 0x13]
        s.settle_frames = 3
        sc.append(s)

    # ---- PAL ------------------------------------------------------------------------------
    for n in range(6):
        rng = random.Random(0x7A100000 + n)
        s = Scene("pal_rand%02d" % n, pal=True, frames=7)
        for i in range(4):
            s.sprite(i, rng.randrange(0, 236), rng.randrange(0, 180), rng.randrange(256), sprite_shape(rng, 0.5))
        for i in range(12):
            s.char(i, rng.randrange(0, 240), rng.randrange(0, 200), rng.randrange(256), rng.randrange(256))
        for i in range(4):
            s.quad(i, rng.randrange(0, 240), rng.randrange(0, 200),
                   [(rng.randrange(256), rng.randrange(256)) for _ in range(4)])
        for c in range(9):
            s.hgrid(c, rng.randrange(512))
        for c in range(10):
            s.vgrid(c, rng.randrange(256))
        s.color = rng.randrange(256)
        s.mask = rng.randrange(256)
        s.control = (rng.randrange(256) | CTL_FG) & ~CTL_SND
        s.probes = [0xa1, 0xa2, 0xa2]
        s.settle_frames = 4
        sc.append(s)
    s = Scene("pal_chars_y", pal=True)
    for i in range(12):
        s.char(i, i * 2, 10 + i * 12, 8 * (i + 1), (i % 8) << 1)
    s.control = CTL_FG
    s.probes = [0xa1]
    sc.append(s)
    return sc


if __name__ == "__main__":
    sc = build_scenes()
    print(len(sc), "scenes;", sum(len(s.writes) for s in sc), "register writes")

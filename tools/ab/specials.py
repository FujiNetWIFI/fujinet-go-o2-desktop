"""specials.py -- hand-written timing carts: the beam registers (live and
strobe-latched), raster effects (writes landing mid-line and mid-frame:
background colour splits, objects moved while the beam is on screen, grid and
control toggles, collision reads mid-frame) and the P17 LUM line.

Each case has .name and .run(work) -> result dict like gen_goldens.do_scene().
"""
import os, sys, random

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import o2cart, scenes
from abrun import *
from scenes import CTL_FG, CTL_GRID, CTL_DOTS, CTL_FILL, CTL_STROBE, Scene


def geom(pal):
    return (456, 313) if pal else (455, 263)


class Case:
    def __init__(self, name, pal, frames, build, sound=False):
        self.name, self.pal, self.frames, self.build, self.sound = name, pal, frames, build, sound

    def run(self, work):
        cart = o2cart.Cart(pal=self.pal)
        cart.marker_cycle = MARKER_CYCLE
        cart.name = self.name
        self.build(cart, self.pal)
        r = result_for(self.name, cart, self.pal, self.frames, work, wav=self.sound)
        if self.sound:
            r["sound"] = analyse_wav(r["wav"])
        return r


def analyse_wav(path):
    """The edges (crossings of half the peak) and some plateau samples of
    MAME's 48 kHz output, for the C side to compare against."""
    import wave
    import numpy as np
    w = wave.open(path)
    a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.int32)
    n = len(a)
    peak = int(a.max())
    thr = peak // 2 if peak > 200 else 100
    hi = a >= thr
    edges = [int(i) for i in np.nonzero(hi[1:] != hi[:-1])[0] + 1][:160]
    d = np.abs(np.diff(a, prepend=a[0]))
    probes = []
    k = 0
    big = max(peak, 1) * 0.08
    while len(probes) < 48:
        i = 300 + 211 * k
        k += 1
        if i + 8 >= n:
            break
        if d[i - 7:i + 8].max() < big:
            probes.append((i, int(a[i])))
    return dict(nsamples=n, thr=int(thr), peak=peak, edges=edges, probes=probes)


def hv_to_t(pal, frame, vpos, hpos):
    """Time of the beam at (vpos, hpos) during the frame that is *completed*
    at frame*F (so vpos 0..242 are in the picture, 243.. the vblank AFTER it)."""
    H, V = geom(pal)
    F = H * V
    # t = 0 is (243, 0); positions advance 243 -> ... -> 262/312 -> 0 -> 242
    d = (vpos - 243) % V
    return (frame - 1) * F + d * H + hpos


# ---------------------------------------------------------------------------
# beam registers
# ---------------------------------------------------------------------------

def beam_live(pal, part):
    H, V = geom(pal)
    keys = ([0, 120] if part == 0 else [236, 252]) if not pal else ([0, 120] if part == 0 else [236, 302])
    hs = [0, 100, 224, 226, 364, 366, 412, 414, 440, 452, 454]
    # one point a line (a group of three reads takes 240 pixels), the edges
    # of hstatus / blanking / the Y increment at 11 different lines
    pts = [((kv + j) % V, h) for kv in keys for j, h in enumerate(hs)]
    pts.sort(key=lambda p: hv_to_t(pal, 3, p[0], p[1]))

    def build(c, pal_):
        c.at(int(geom(pal_)[0] * geom(pal_)[1] * 1.1))
        c.w(0xa0, CTL_STROBE)               # live beam position
        for (v, h) in pts:
            c.at(hv_to_t(pal_, 3, v, h), 4)
            c.r(0xa4)
            c.r(0xa5)
            c.r(0xa1)
    return Case("beam_live%d%s" % (part, "_pal" if pal else ""), pal, 5, build)


def beam_latch(pal):
    H, V = geom(pal)
    pts = [(0, 10), (1, 0), (50, 224), (100, 366), (120, 413), (121, 412), (200, 440), (241, 452), (242, 365), (242, 366),
           (243, 0), (255, 300), (V - 1, 454), (V - 1, 100)]
    pts.sort(key=lambda p: hv_to_t(pal, 3, p[0], p[1]))
    keep, last = [], -10**9
    for p in pts:                       # an iteration takes ~850 px
        t = hv_to_t(pal, 3, p[0], p[1])
        if t - last >= 1000:
            keep.append(p)
            last = t
    pts = keep

    def build(c, pal_):
        c.at(int(H * V * 1.1))
        for k, (v, h) in enumerate(pts):
            c.at(hv_to_t(pal_, 3, v, h) - 600, 6)
            c.w(0xa0, CTL_STROBE)            # strobe on
            c.at(hv_to_t(pal_, 3, v, h), 6)
            c.w(0xa0, 0)                     # strobe off: tucks the position away
            c.r(0xa4)
            c.r(0xa5)
            c.r(0xa1)
        c.at(hv_to_t(pal_, 4, 100, 100), 4)
        c.r(0xa4)
        c.r(0xa5)
    return Case("beam_latch%s" % ("_pal" if pal else ""), pal, 5, build)


# ---------------------------------------------------------------------------
# raster effects
# ---------------------------------------------------------------------------

def base_scene_writes(c, rng, with_quad=True):
    s = Scene("x")
    for i in range(4):
        s.sprite(i, 30 + i * 48, 20 + i * 30, (rng.randrange(8) << 3) | (i & 3), scenes.sprite_shape(rng, 0.6))
    for i in range(6):
        s.char(i, 24 + i * 30, 10 + i * 20, rng.randrange(256), rng.randrange(16))
    if with_quad:
        s.quad(0, 100, 30, [(rng.randrange(256), rng.randrange(16)) for _ in range(4)])
    for col in range(9):
        s.hgrid(col, rng.randrange(512))
    for col in range(10):
        s.vgrid(col, rng.randrange(256))
    for a, v in s.writes:
        c.w(a, v)


def raster(name, pal, seed, period, iters, variant):
    def build(c, pal_):
        rng = random.Random(seed)
        H, V = geom(pal_)
        F = H * V
        c.at(int(F * 1.1))
        base_scene_writes(c, rng)
        c.w(0xa3, 0x50)
        c.w(0xa2, 0x3f)
        c.w(0xa0, CTL_FG | CTL_GRID)
        c.at(hv_to_t(pal_, 3, 4, 0) if variant != 2 else hv_to_t(pal_, 3, 120, 0), 8)
        c.loop_begin(iters)
        # ---- one frame's worth of effects ----
        c.w(0xa3, 0x08)
        c.delay(600)
        c.w(0xa3, 0x2c)                  # background splits the line
        c.delay(37)
        c.w(0xa3, 0x31)
        c.delay(400)
        c.w(0xa0, CTL_GRID)              # objects off
        c.w(0x04, 90)
        c.w(0x05, 66)                    # a sprite moves while the beam is below its old place
        c.w(0x10 + 4 * 2, 160)
        c.w(0x10 + 4 * 2 + 1, 100)
        c.w(0xa0, CTL_FG | CTL_GRID)
        c.delay(500)
        c.w(0x80, 0xa5)                  # a sprite shape row (writable with the display on)
        c.w(0x81, 0x5a)
        c.delay(53)
        c.w(0xa2, 0xff)
        c.r(0xa2)
        c.r(0xa1)
        if variant == 1:
            c.delay(800)
            c.p1_set(o2cart.P1_VDC)      # LUM on for a while
            c.delay(300)
            c.p1_set(o2cart.P1_VDC_DIM)
            c.delay(700)
            c.w(0xa0, CTL_GRID | CTL_DOTS)
            c.w(0x40, 140)
            c.w(0x41, 20)
            c.w(0xa0, CTL_FG | CTL_GRID | CTL_DOTS | CTL_FILL)
        else:
            c.delay(900)
            c.w(0xa0, CTL_FG)            # grid off: its registers may be written
            c.w(0xc0, 0x55)
            c.w(0xe3, 0xf0)
            c.w(0xa0, CTL_FG | CTL_GRID)
        c.delay(1200)
        c.r(0xa2)
        c.r(0xa1)
        c.delay(900)
        c.w(0xa3, 0x1c)
        c.loop_end(period)
        c.r(0xa2)
    return Case(name, pal, iters + 4, build)


# ---------------------------------------------------------------------------
# sound
# ---------------------------------------------------------------------------

def sound(name, pal, frames, program):
    def build(c, pal_):
        H, V = geom(pal_)
        F = H * V
        c.at(int(F * 1.5))
        program(c, F)
    return Case(name, pal, frames, build, sound=True)


def prog_slow(c, F):
    c.w(0xa7, 0xf0)
    c.w(0xa8, 0xf0)
    c.w(0xa9, 0xf0)
    c.w(0xaa, 0x8f)                       # on, slow clock, loop, volume 15
    c.at(F * 6, 4)
    c.r(0xa1)


def prog_fast(c, F):
    c.w(0xa7, 0xcc)
    c.w(0xa8, 0xcc)
    c.w(0xa9, 0xcc)
    c.w(0xaa, 0xaf)                       # on, fast clock, loop, volume 15


def prog_pattern(c, F):
    c.w(0xa7, 0x0f)
    c.w(0xa8, 0xff)
    c.w(0xa9, 0x30)
    c.w(0xaa, 0xaf)
    c.at(int(F * 4.2), 6)
    c.w(0xa8, 0x0f)                       # rewriting a shift register: the count restarts
    c.at(int(F * 6.1), 6)
    c.w(0xaa, 0x2f)                       # sound off, clock still going
    c.at(int(F * 6.7), 6)
    c.w(0xaa, 0xaf)


def prog_noise(c, F):
    c.w(0xa7, 0x00)
    c.w(0xa8, 0x01)
    c.w(0xa9, 0x01)
    c.w(0xaa, 0xbf)                       # on, fast, NOISE, volume 15
    c.at(int(F * 5), 4)
    c.r(0xa7)                             # (write-only registers read 0)


def prog_volume(c, F):
    c.w(0xa7, 0xff)
    c.w(0xa8, 0x00)
    c.w(0xa9, 0xff)
    c.w(0xaa, 0x8f)
    for k, v in enumerate((0x8b, 0x85, 0x8f, 0x80, 0x83, 0x8c)):     # (not 8: half-peak is the edge threshold)
        c.at(int(F * (3.2 + k * 1.37)), 6)
        c.w(0xaa, v)


def prog_irq(c, F):
    c.w(0xa0, 0x04)                       # sound interrupt enable
    c.w(0xa7, 0xaa)
    c.w(0xa8, 0xaa)
    c.w(0xa9, 0xaa)
    c.w(0xaa, 0xaf)                       # fast clock: 24 shifts every 24 * 4 lines
    for k in range(10):
        c.at(int(F * (3.0 + 0.13 * k * k / 4)), 4)
        c.r(0xa1)


def prog_noise_slow(c, F):
    c.w(0xa7, 0x81)
    c.w(0xa8, 0x00)
    c.w(0xa9, 0x81)
    c.w(0xaa, 0x9f)                       # on, SLOW, noise
    c.at(int(F * 8), 6)
    c.w(0xaa, 0xaf)                       # then fast, loop


def special_cases():
    cs = [beam_live(False, 0), beam_live(False, 1), beam_latch(False),
          beam_live(True, 0), beam_live(True, 1), beam_latch(True)]
    for pal in (False, True):
        sfx = "_pal" if pal else ""
        cs += [sound("snd_slow" + sfx, pal, 30, prog_slow), sound("snd_fast" + sfx, pal, 14, prog_fast),
               sound("snd_pattern" + sfx, pal, 30, prog_pattern), sound("snd_noise" + sfx, pal, 14, prog_noise),
               sound("snd_volume" + sfx, pal, 30, prog_volume), sound("snd_irq" + sfx, pal, 14, prog_irq),
               sound("snd_noise_slow" + sfx, pal, 30, prog_noise_slow)]
    cs += [raster("raster_a", False, 0xAA01, 5983, 10, 0),
           raster("raster_b", False, 0xAA02, 5991, 10, 1),
           raster("raster_c", False, 0xAA03, 5987, 10, 2),
           raster("raster_pal", True, 0xAA04, 7933, 10, 1)]
    return cs

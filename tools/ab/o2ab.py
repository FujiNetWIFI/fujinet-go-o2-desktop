#!/usr/bin/env python3
"""o2ab.py -- shared helpers for the 8244 A/B tools (MAME is the reference).

    from o2ab import *

MAME is run with `-video none -sound none` and killed hard if it outlives its
budget (it ignores SIGTERM in that mode).  Test ROM sets are built in a
scratch directory: the real BIOS from MAME's rompath, and an `i8244.zip`
carrying either the generated TEST FONT (what the committed goldens use, so
no ROM is needed to run them) or the real charset (local checks only).
"""
import os, struct, subprocess, sys, zipfile, hashlib, signal, shutil

MAME_DIR = os.path.expanduser(os.environ.get("MAME_DIR", "~/Workspace/mame"))
MAME = os.path.join(MAME_DIR, "mame")
HERE = os.path.dirname(os.path.abspath(__file__))
ASL = os.path.expanduser(os.environ.get("ASL", "~/asl/asl"))
P2BIN = os.path.expanduser(os.environ.get("P2BIN", "~/asl/p2bin"))
W, H = 360, 243

# MAME's calibrated palette (odyssey2.cpp odyssey2_colors) -> index
PAL_O2 = [(0x00,0x00,0x00),(0x79,0x00,0x00),(0x00,0x6d,0x07),(0x77,0x67,0x0b),
          (0x1a,0x37,0xbe),(0x94,0x30,0x9f),(0x2a,0xaa,0xbe),(0xce,0xce,0xce),
          (0x67,0x67,0x67),(0xc7,0x51,0x51),(0x56,0xc4,0x69),(0xc6,0xb8,0x6a),
          (0x5c,0x80,0xf6),(0xdc,0x84,0xe8),(0x77,0xe6,0xeb),(0xff,0xff,0xff)]


def test_font():
    """The generated 512-byte test font (same formula in i8244_test.c)."""
    return bytes((((i * 2654435761) & 0xffffffff) >> 24) for i in range(512))


def make_romdir(path, font=None):
    """A rompath directory holding an i8244.zip with `font` (default: the
    test font).  MAME warns about the checksum and runs."""
    os.makedirs(path, exist_ok=True)
    z = os.path.join(path, "i8244.zip")
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("charset_i8244.bin", font if font is not None else test_font())
    # the real BIOS by link; the rompath is this directory ONLY, or MAME finds
    # the real charset for the PAL machine (it searches on past a bad CRC)
    for n in ("videopac.zip",):
        d = os.path.join(path, n)
        if not os.path.lexists(d):
            os.symlink(os.path.join(MAME_DIR, "roms", n), d)
    return path


def run_mame(cart, lua, env, seconds, romdir, machine="odyssey2", wav=None,
             extra=(), timeout=None, log=None):
    """Run MAME headless for `seconds` of emulated time, hard-killed after
    `timeout` wall seconds.  Returns the exit status."""
    cmd = [MAME, "-rompath", romdir,
           machine, "-cart", cart, "-video", "none", "-sound", "none",
           "-seconds_to_run", str(seconds), "-nothrottle", "-skip_gameinfo",
           "-autoboot_script", lua]
    if wav:
        cmd[cmd.index("-sound") + 1] = "dummy"
        cmd += ["-wavwrite", wav]
    cmd += list(extra)
    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    logf = open(log, "wb") if log else subprocess.DEVNULL
    p = subprocess.Popen(cmd, env=e, stdout=logf, stderr=subprocess.STDOUT,
                         start_new_session=True)
    try:
        p.wait(timeout=timeout or (20 + seconds * 3))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    if log:
        logf.close()
    return p.returncode


def assemble(src, outbin, includes=()):
    """asl + p2bin a .a48 into outbin (a 2K cart image, $400-$BFF); returns
    the list file path."""
    d = os.path.dirname(os.path.abspath(src))
    name = os.path.splitext(os.path.basename(src))[0]
    args = [ASL, "-q", "-L", "-U"]
    for i in includes:
        args += ["-i", i]
    args.append(name + ".a48")
    r = subprocess.run(args, cwd=d, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("asl failed for %s" % src)
    r = subprocess.run([P2BIN, name + ".p", os.path.abspath(outbin), "-r", "0x400-0xBFF", "-l", "0"],
                       cwd=d, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("p2bin failed for %s" % src)
    return os.path.join(d, name + ".lst")


_inv = {((r << 16) | (g << 8) | b): i for i, (r, g, b) in enumerate(PAL_O2)}


def frame_indices(raw):
    """One frame of MAME pixels() (u32 LE ARGB) -> bytes of palette indices."""
    out = bytearray(W * H)
    for i in range(W * H):
        b, g, r, a = raw[i * 4:i * 4 + 4]
        out[i] = _inv[(r << 16) | (g << 8) | b]
    return bytes(out)


def read_frames(path, start=0):
    """Yield (frame_number, indices) from a VDC_FRAMES dump."""
    sz = W * H * 4
    with open(path, "rb") as f:
        n = start
        while True:
            raw = f.read(sz)
            if len(raw) < sz:
                return
            yield n, frame_indices(raw)
            n += 1


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def xrgb_hash(idx, palette=PAL_O2):
    """FNV-1a 64 over the frame as little-endian 0x00RRGGBB words, the hash
    i8244_test.c computes on the chip's frame."""
    h = 0xcbf29ce484222325
    for v in idx:
        r, g, b = palette[v]
        x = (r << 16) | (g << 8) | b
        for sh in (0, 8, 16, 24):
            h = ((h ^ ((x >> sh) & 0xff)) * 0x100000001b3) & 0xffffffffffffffff
    return h


def parse_log(path):
    """-> (events, frames, iram): events are (kind, t, off, data, p1)."""
    ev, fr, iram = [], [], None
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] in "RW":
                ev.append((p[0], int(p[1]), int(p[2]), int(p[3]), int(p[4])))
            elif p[0] == "F":
                fr.append((int(p[1]), int(p[2]), int(p[3])))
            elif p[0] == "IRAM":
                iram = [int(x, 16) for x in p[1:]]
    return ev, fr, iram

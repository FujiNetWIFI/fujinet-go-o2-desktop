#!/usr/bin/env python3
"""abrun.py -- run a cart model in MAME and encode what happened (shared by
gen_goldens.py and specials.py).  See gen_goldens.py for the stream encoding."""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import o2ab, o2cart

LOGLUA = os.path.join(HERE, "vdc_log.lua")
MARKER_CYCLE = 16


def frame_px(pal):
    return 456 * 313 if pal else 455 * 263


def run_cart(cart, name, pal, nframes, work, font=None, wav=False):
    wd = os.path.join(work, name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    roms = os.path.join(work, "roms_test")
    binf = cart.build(wd, name)
    env = {"VDC_LOG": os.path.join(wd, "vdc.log"), "VDC_FRAMES": os.path.join(wd, "vdc.frames"),
           "VDC_FRAMES_N": nframes, "VDC_FRAME_FROM": 1}
    if pal:
        env["VDC_PAL"] = 1
    rc = o2ab.run_mame(binf, LOGLUA, env, 5 + nframes // 20, roms,
                       machine="videopac" if pal else "odyssey2", log=os.path.join(wd, "mame.out"),
                       wav=os.path.join(wd, "out.wav") if wav else None,
                       extra=["-cfg_directory", os.path.join(wd, "cfg"), "-nvram_directory", os.path.join(wd, "nvram"),
                              "-keyboardprovider", "none", "-joystickprovider", "none",
                              "-mouseprovider", "none", "-lightgunprovider", "none"])
    ev, fr, iram = o2ab.parse_log(env["VDC_LOG"])
    if len(fr) < nframes:
        raise SystemExit("%s: MAME ended early (rc %s, %d frames)" % (name, rc, len(fr)))
    return wd, ev, fr


def check_and_encode(cart, ev, pal):
    """Match the MAME log against the cart model; return (stream, reads)."""
    # the model's events with absolute cycles; the marker first
    model = [('W', MARKER_CYCLE, o2cart.MARKER[0], o2cart.MARKER[1])]
    for kind, c, off, val in cart.events:
        model.append((kind, MARKER_CYCLE + c, off, val))
    vdc = [e for e in ev]
    mi = 0
    stream = []
    t = 0           # cycles
    pending_w = []
    reads = 0
    adjusts = 0
    p1_cur = None

    def flush():
        nonlocal pending_w
        if pending_w:
            stream.append(0x1000 | len(pending_w))
            stream.extend(pending_w)
            pending_w = []

    # VDC accesses in the log, in order, matched with the model's W/R events
    accesses = [e for e in model if e[0] in 'WR']
    logv = [e for e in vdc if e[0] in 'WR']
    if len(accesses) != len(logv):
        raise SystemExit("%s: cart model has %d accesses, MAME logged %d" % (cart.name if hasattr(cart, 'name') else '?', len(accesses), len(logv)))
    li = 0
    for kind, c, off, val in model:
        if kind == 'P1':
            gap = c - t - 4
            if gap < 0:
                raise SystemExit("negative gap at P1")
            if gap:
                flush()
                stream.append(gap)
            flush()
            stream.append(0x3000 | (val & 0xff))
            t = c
            continue
        lk, lt, loff, ldata, lp1 = logv[li]
        li += 1
        dpx = lt - o2cart.cycles_to_px(c, pal)
        if lk != kind or loff != off or abs(dpx) > 1:
            raise SystemExit("model/log mismatch at access %d: model %s c=%d off=%02x, log %s t=%d off=%02x"
                             % (li, kind, c, off, lk, lt, loff))
        if dpx:
            # a tie MAME's time falls on the other side of: a pixel's difference
            flush()
            stream.append(0x4000 | (1 if dpx > 0 else 2))
            adjusts += 1
        if kind == 'W' and ldata != val:
            raise SystemExit("write data mismatch at access %d" % li)
        cost = 6 if kind == 'W' else 4
        gap = c - t - cost
        if gap < 0:
            raise SystemExit("negative gap at access %d" % li)
        if kind == 'W':
            if gap:
                flush()
                while gap > 0xfff:
                    stream.append(0xfff)
                    gap -= 0xfff
                stream.append(gap)
            pending_w.append((off << 8) | val)
        else:
            flush()
            while gap > 0xfff:
                stream.append(0xfff)
                gap -= 0xfff
            if gap:
                stream.append(gap)
            stream.append(0x2000 | off)
            stream.append(ldata)
            reads += 1
        t = c
    flush()
    return stream, reads


def frame_hashes(wd, nframes, palette=o2ab.PAL_O2):
    """Hash of C frame n (1..nframes-1) = MAME's dump n+1 (the dump at
    notifier n shows the frame completed one notifier before)."""
    hs = []
    for n, idx in o2ab.read_frames(os.path.join(wd, "vdc.frames"), 1):
        hs.append(o2ab.xrgb_hash(idx, palette))
    # hs[k] is the dump at notifier k+1; C frame n <-> dump n+1 <-> hs[n]
    return hs[1:nframes]



def result_for(name, cart, pal, nframes, work, wav=False):
    wd, ev, fr = run_cart(cart, name, pal, nframes, work, wav=wav)
    stream, reads = check_and_encode(cart, ev, pal)
    hs = frame_hashes(wd, nframes)
    r = dict(name=name, pal=pal, nframes=nframes, stream=stream, hashes=hs, reads=reads, wd=wd)
    if wav:
        r["wav"] = os.path.join(wd, "out.wav")
    return r

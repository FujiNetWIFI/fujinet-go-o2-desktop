#!/usr/bin/env python3
"""real_carts.py -- replay real cartridges' VDC traffic into the C chip.

    real_carts.py [--frames N] [--press SPEC] [--pal] [--font real|test] CART...

For each cart (a LOCAL file; nothing here is stored): run MAME headless on the
real BIOS and real charset, log every VDC access and dump the last frames,
then run `i8244_test --replay` on the log, comparing the C chip with MAME pixel
for pixel and every value the program read.  A developer tool: it needs the
carts, the BIOS and the charset (MAME's roms/i8244.zip), none of which are in
the repo.  `--test-bin` is the built i8244_test (default: build it with cc).
"""
import argparse, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import o2ab

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LOGLUA = os.path.join(HERE, "vdc_log.lua")


def build_test(work):
    exe = os.path.join(work, "i8244_replay")
    subprocess.check_call(["cc", "-std=c11", "-O2", "-w", "-I" + os.path.join(REPO, "core", "o2"),
                           "-I" + os.path.join(REPO, "core", "tests"),
                           os.path.join(REPO, "core", "tests", "i8244_test.c"),
                           os.path.join(REPO, "core", "o2", "i8244.c"), "-o", exe])
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("carts", nargs="+")
    ap.add_argument("--frames", type=int, default=420)
    ap.add_argument("--tail", type=int, default=24, help="frames compared at the end (all accesses are replayed)")
    ap.add_argument("--press", default="150:KEY.0:2:10,300:JOY.0:16:20",
                    help="VDC_PRESS spec: frame:port:mask:hold,...")
    ap.add_argument("--pal", action="store_true")
    ap.add_argument("--p1trace", action="store_true", help="log exact P1 write times (slow, needed for LUM changes mid-frame)")
    ap.add_argument("--work")
    a = ap.parse_args()
    work = a.work or tempfile.mkdtemp(prefix="i8244-real-")
    os.makedirs(work, exist_ok=True)
    roms = os.path.join(work, "roms_real")
    os.makedirs(roms, exist_ok=True)
    for n in ("i8244.zip", "videopac.zip"):
        d = os.path.join(roms, n)
        if not os.path.lexists(d):
            os.symlink(os.path.join(o2ab.MAME_DIR, "roms", n), d)
    exe = build_test(work)
    font = os.path.join(work, "charset_i8244.bin")
    import zipfile
    with zipfile.ZipFile(os.path.join(o2ab.MAME_DIR, "roms", "i8244.zip")) as z:
        open(font, "wb").write(z.read("charset_i8244.bin"))
    bad = 0
    for cart in a.carts:
        name = os.path.splitext(os.path.basename(cart))[0].replace(" ", "_")[:40]
        log = os.path.join(work, name + ".log")
        frames = os.path.join(work, name + ".frames")
        env = {"VDC_LOG": log, "VDC_FRAMES": frames, "VDC_FRAMES_N": a.frames,
               "VDC_FRAME_FROM": a.frames - a.tail, "VDC_PRESS": a.press}
        if a.pal:
            env["VDC_PAL"] = 1
        if a.p1trace:
            env["VDC_P1TRACE"] = 1
        o2ab.run_mame(cart, LOGLUA, env, 3 + a.frames // 55, roms, machine="videopac" if a.pal else "odyssey2",
                      log=os.path.join(work, name + ".out"),
                      extra=["-cfg_directory", os.path.join(work, "cfg"), "-nvram_directory", os.path.join(work, "nvram"),
                             "-keyboardprovider", "none", "-joystickprovider", "none", "-mouseprovider", "none",
                             "-lightgunprovider", "none"])
        if not os.path.exists(frames):
            print("%-40s MAME produced nothing" % name)
            bad += 1
            continue
        # MAME runs timers only after the instruction that overshoots them: an
        # access sees events up to one machine cycle (20 px NTSC, 18 PAL) before it
        cmd = [exe, "--replay", log, "--frames", frames, "--from", str(a.frames - a.tail), "--font", font,
               "--access-lag", "18" if a.pal else "20"]
        if a.pal:
            cmd.append("--pal")
        r = subprocess.run(cmd, capture_output=True, text=True)
        last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "?"
        print("%-40s %s %s" % (name, "ok " if r.returncode == 0 else "BAD", last))
        if r.returncode:
            bad += 1
            print("\n".join(r.stdout.strip().splitlines()[:4]))
        os.remove(frames)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

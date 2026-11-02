#!/usr/bin/env python3
"""cart_ab.py -- the whole machine against MAME, frame for frame, on cartridges.

    cart_ab.py [--frames N] [--keys SPEC] [--machine o2|g7000] [--font F]
               [--bios F] [--report] CART...

For each cartridge image (a standard .bin: 2 KB banks as MAME's plain-file
rule picks them) it runs MAME (odyssey2 or videopac, the REAL BIOS from MAME's
rompath) with tools/ab/vdc_log.lua dumping every frame as 360x243 32-bit ARGB,
and runs build/.../o2_headless on the same image with the same BIOS and font,
-raw dumping the same way, then compares the two frame by frame and prints how
many of the first N frames are identical and where the first difference is.

The BIOS (o2bios.rom) and the 8244 character set come from MAME's rompath
(~/Workspace/mame/roms/{videopac,i8244}.zip) and are only ever read here; the
repository holds no ROM. Key presses go to both sides: SPEC is
"frame:row:col:down" (row/col as the keyboard matrix, MAME's KEY.row bit col),
comma separated, e.g. 120:5:7:1,126:5:7:0 presses ENTER at frame 120.

Environment: MAME_DIR (default ~/Workspace/mame), O2_HEADLESS (the runner;
default: found under the scratch build dirs), O2AB_WORK (scratch directory).
Exit status 1 if any cartridge differs.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import o2ab  # noqa: E402  (the VDC A/B helpers: run_mame, make_romdir, ...)

W, H = 360, 243
FRAME = W * H * 4


def find_runner():
    env = os.environ.get("O2_HEADLESS")
    if env:
        return env
    for cand in (
        os.path.join(HERE, "../../build/core/tests/o2_headless"),
        os.path.join(HERE, "../../build-core/core/tests/o2_headless"),
    ):
        if os.path.exists(cand):
            return os.path.abspath(cand)
    raise SystemExit("cart_ab: set O2_HEADLESS to the o2_headless binary")


def mame_file(zipname, member):
    with zipfile.ZipFile(os.path.join(o2ab.MAME_DIR, "roms", zipname)) as z:
        return z.read(member)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("carts", nargs="+")
    ap.add_argument("--frames", type=int, default=240)
    ap.add_argument("--keys", default="")
    ap.add_argument("--machine", default="o2", choices=["o2", "g7000"])
    ap.add_argument("--report", action="store_true", help="print every cartridge, not only the differing ones")
    args = ap.parse_args()

    runner = find_runner()
    work = os.environ.get("O2AB_WORK") or tempfile.mkdtemp(prefix="o2ab-")
    os.makedirs(work, exist_ok=True)
    bios = os.path.join(work, "o2bios.rom")
    font = os.path.join(work, "charset_i8244.bin")
    open(bios, "wb").write(mame_file("videopac.zip", "o2bios.rom"))
    open(font, "wb").write(mame_file("i8244.zip", "charset_i8244.bin"))
    # MAME reads both from its own rompath (the real set), no override needed
    romdir = os.path.join(o2ab.MAME_DIR, "roms")
    machine = "odyssey2" if args.machine == "o2" else "videopac"

    keys = []
    for item in [k for k in args.keys.split(",") if k]:
        f, r, c, d = (int(x) for x in item.split(":"))
        keys.append((f, r, c, d))

    bad = 0
    for cart in args.carts:
        name = os.path.basename(cart)
        mraw = os.path.join(work, "mame.raw")
        oraw = os.path.join(work, "ours.raw")
        mlog = os.path.join(work, "mame.log")
        for p in (mraw, oraw):
            if os.path.exists(p):
                os.unlink(p)
        env = {"VDC_LOG": mlog, "VDC_FRAMES": mraw, "VDC_FRAMES_N": args.frames}
        if args.machine == "g7000":
            env["VDC_PAL"] = "1"
        if keys:
            env["VDC_KEYS"] = ",".join("%d:%d:%d:%d" % k for k in keys)
        o2ab.run_mame(os.path.abspath(cart), os.path.join(HERE, "vdc_log.lua"), env,
                      args.frames / 50 + 2, romdir, machine=machine,
                      timeout=120)
        cmd = [runner, "-machine", "o2" if args.machine == "o2" else "g7000",
               "-bios", bios, "-font", font, "-game", cart,
               "-frames", str(args.frames), "-raw", oraw]
        for f, r, c, d in keys:
            cmd += ["-key", "%d:%d:%d:%d" % (f, r, c, d)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)

        a = open(mraw, "rb").read() if os.path.exists(mraw) else b""
        b = open(oraw, "rb").read()
        n = min(len(a), len(b)) // FRAME
        same = 0
        first = None
        for i in range(n):
            fa = a[i * FRAME:(i + 1) * FRAME]
            fb = b[i * FRAME:(i + 1) * FRAME]
            # MAME's alpha is 0xFF too; compare the colour only
            if fa == fb:
                same += 1
            elif first is None:
                first = i
        status = "OK " if n and same == n else "DIFF"
        if status == "DIFF":
            bad += 1
        if args.report or status == "DIFF":
            print("%s %-50s %d/%d frames identical%s" % (
                status, name[:50], same, n,
                "" if first is None else ", first difference at frame %d" % first))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

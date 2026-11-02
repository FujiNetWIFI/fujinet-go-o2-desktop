#!/usr/bin/env python3
"""mcs48_ab.py -- the MCS-48 core against MAME, on assembled probe programs.

A probe (tools/ab/probes/NAME.a48) is assembled with AS into a 1K image (the
console's internal ROM, which MAME's odyssey2 loads as o2bios.rom) and a 2K
cartridge. MAME runs it from reset with the debugger trace of mcs48_trace.lua
and logs the CPU state before every instruction until the probe reaches its
end marker ($3FE). core/tests/mcs48_ops_test runs the same images on
core/o2/mcs48.c in a model of the console and prints the same lines.

    mcs48_ab.py --gen [PROBE ...]     write core/tests/mcs48_golden.h: the probe
                                      images plus, from MAME, each trace's length,
                                      FNV-1a hash and sample lines (committed, so
                                      the cpu_ops test needs neither MAME nor AS)
    mcs48_ab.py --check [PROBE ...]   run MAME and the core live and diff the
                                      whole traces (the cpu_ops_mame test);
                                      --exe gives the mcs48_ops_test binary
    mcs48_ab.py --mame PROBE          print MAME's trace for one probe
    mcs48_ab.py --coverage [PROBE ...]  which opcodes the probes execute (--exe)
    mcs48_ab.py --list                the probes

Exit status: 0 equal, 1 a difference or a failure, 77 when AS or MAME is
missing (ctest reads that as skipped).

Environment: ASL, P2BIN (default ~/asl/asl, ~/asl/p2bin or on PATH), MAME
(default ~/Workspace/mame/mame), MAME_ROMS (its rompath for the 8244's own
ROM; default ~/Workspace/mame/roms). MAME is always run under
`timeout -s KILL`: it ignores SIGTERM with -video none.

SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PROBES = os.path.join(HERE, "probes")
GOLDEN = os.path.join(ROOT, "core", "tests", "mcs48_golden.h")
LUA = os.path.join(HERE, "mcs48_trace.lua")

ROM_SIZE = 0x400
CART_SIZE = 0x800
DONE_PC = 0x3FE
LINE_RE = re.compile(r"^[0-9A-F]{3}( [0-9A-F]{2}){14} \d+$")
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


class Missing(Exception):
    """A tool this needs is not installed: the test is skipped."""


def find_tool(env, default, name):
    path = os.environ.get(env)
    if path and os.access(path, os.X_OK):
        return path
    path = os.path.expanduser(default)
    if os.access(path, os.X_OK):
        return path
    found = shutil.which(name)
    if found:
        return found
    raise Missing("%s not found (set $%s)" % (name, env))


# The CPU probes, by name (other programs may live in probes/ as well): each is
# probes/NAME.a48, with common.inc / common_end.inc around it.
PROBE_NAMES = ("alu", "branch", "counter", "extint", "fuzz", "illegal", "jnihack", "mb",
               "mem", "ports", "regops", "regs", "smoke", "timer", "vblank")


def probe_names(wanted):
    for name in PROBE_NAMES:
        if not os.path.exists(os.path.join(PROBES, name + ".a48")):
            sys.exit("mcs48_ab.py: probes/%s.a48 is missing" % name)
    if not wanted:
        return list(PROBE_NAMES)
    for w in wanted:
        if w not in PROBE_NAMES:
            sys.exit("mcs48_ab.py: no probe %s (have %s)" % (w, ", ".join(PROBE_NAMES)))
    return wanted


def probe_frames(name):
    with open(os.path.join(PROBES, name + ".a48")) as f:
        for line in f:
            m = re.match(r";\s*FRAMES:\s*(\d+)", line)
            if m:
                return int(m.group(1))
    return 6


def assemble(name, work):
    """AS + p2bin: (rom 1024 bytes, cart 2048 bytes)."""
    asl = find_tool("ASL", "~/asl/asl", "asl")
    p2bin = find_tool("P2BIN", "~/asl/p2bin", "p2bin")
    src = os.path.join(work, name)
    os.makedirs(src, exist_ok=True)
    for f in os.listdir(PROBES):
        if f.endswith((".a48", ".inc")):
            shutil.copy(os.path.join(PROBES, f), src)
    r = subprocess.run([asl, "-q", "-L", "-U", name + ".a48"], cwd=src,
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(os.path.join(src, name + ".p")):
        sys.exit("mcs48_ab.py: assembling %s failed:\n%s%s" % (name, r.stdout, r.stderr))
    images = []
    for out, lo, size in (("rom", 0, ROM_SIZE), ("cart", ROM_SIZE, CART_SIZE)):
        path = os.path.join(src, "%s.%s" % (name, out))
        subprocess.run([p2bin, name + ".p", path, "-r", "0x%X-0x%X" % (lo, lo + size - 1),
                        "-l", "0xFF"], cwd=src, capture_output=True, text=True)
        data = open(path, "rb").read() if os.path.exists(path) else b""
        if len(data) != size:      # nothing assembled in the range
            data = (data + b"\xff" * size)[:size]
        images.append(data)
        with open(path, "wb") as f:
            f.write(data)
    return images[0], images[1]


def parse_trace(text):
    """The state lines of a trace, up to and including the first at the end marker."""
    lines = []
    for raw in text.splitlines():
        raw = raw.strip()
        if not LINE_RE.match(raw):
            continue
        lines.append(raw)
        if int(raw[:3], 16) == DONE_PC:
            return lines, True
    return lines, False


def mame_trace(name, rom, cart, work):
    mame = find_tool("MAME", "~/Workspace/mame/mame", "mame")
    mame_roms = os.path.expanduser(os.environ.get("MAME_ROMS", "~/Workspace/mame/roms"))
    run = os.path.join(work, name + "-mame")
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(os.path.join(run, "roms", "odyssey2"))
    with open(os.path.join(run, "roms", "odyssey2", "o2bios.rom"), "wb") as f:
        f.write(rom)
    cart_path = os.path.join(run, "cart.bin")
    with open(cart_path, "wb") as f:
        f.write(cart)
    trace = os.path.join(run, "trace.txt")
    env = dict(os.environ, MCS_TRACE=trace, MCS_DONE="%X" % DONE_PC,
               MCS_FRAMES=str(probe_frames(name)))
    cmd = ["timeout", "-s", "KILL", "120", mame, "odyssey2",
           "-rompath", "%s;%s" % (os.path.join(run, "roms"), mame_roms),
           "-cart", cart_path, "-video", "none", "-sound", "none", "-nothrottle",
           "-seconds_to_run", "30", "-debug", "-debugger", "none",
           "-autoboot_script", LUA, "-skip_gameinfo",
           "-cfg_directory", os.path.join(run, "cfg"),
           "-snapshot_directory", os.path.join(run, "snap"),
           "-nvram_directory", os.path.join(run, "nv")]
    r = subprocess.run(cmd, env=env, cwd=run, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not os.path.exists(trace):
        sys.exit("mcs48_ab.py: MAME wrote no trace for %s:\n%s" % (name, r.stdout[-2000:]))
    with open(trace) as f:
        lines, done = parse_trace(f.read())
    if not done:
        sys.exit("mcs48_ab.py: %s never reached the end marker in MAME (%d lines; raise "
                 "FRAMES or check the probe)" % (name, len(lines)))
    return lines


class CoreFailed(Exception):
    pass


def core_trace(exe, name, rom, cart, work, cap=0):
    rom_path = os.path.join(work, name + ".rom")
    cart_path = os.path.join(work, name + ".cart")
    with open(rom_path, "wb") as f:
        f.write(rom)
    with open(cart_path, "wb") as f:
        f.write(cart)
    cmd = [exe, "--run", rom_path, cart_path] + ([str(cap)] if cap else [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    lines, done = parse_trace(r.stdout)
    if r.returncode != 0 or not done:
        raise CoreFailed("rc %d, %d lines: %s" % (r.returncode, len(lines), r.stderr.strip()))
    return lines


def fnv(lines):
    h = FNV_OFFSET
    for line in lines:
        for b in (line + "\n").encode():
            h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def segments(image):
    """Runs of non-$FF bytes (gaps under 6 bytes joined): [(addr, bytes)]."""
    segs = []
    i = 0
    n = len(image)
    while i < n:
        if image[i] == 0xFF:
            i += 1
            continue
        start = i
        end = i
        while i < n and i - end < 6:
            if image[i] != 0xFF:
                end = i
            i += 1
        segs.append((start, image[start:end + 1]))
        i = end + 1
    return segs


def sample_indexes(count):
    step = max(1, count // 14)
    idx = set(range(min(3, count)))
    idx.update(range(0, count, step))
    idx.add(count - 1)
    return sorted(idx)


def c_bytes(data):
    rows = []
    for i in range(0, len(data), 16):
        rows.append("    " + ", ".join("0x%02X" % b for b in data[i:i + 16]) + ",")
    return "\n".join(rows)


def emit_header(probes):
    out = []
    out.append("""/* mcs48_golden.h -- the MCS-48 conformance probes and MAME's traces of them.
 * GENERATED by tools/ab/mcs48_ab.py --gen; mcs48_ops_test.c holds mcs48.c to it.
 *
 * Per probe (tools/ab/probes/NAME.a48): the assembled bytes of the console ROM
 * ($000-$3FF) and the 2K cartridge as runs of non-$FF bytes (the rest is $FF),
 * then from MAME's odyssey2 (tools/ab/mcs48_trace.lua): the number of
 * instructions up to the end marker, the FNV-1a hash of the trace lines (state
 * before each instruction, "pc a psw tc tpre p1 p2 r0..r7 cycles") and sample
 * lines. No ROM of anyone's: the probes are this project's own programs. */
""")
    for p in probes:
        name = p["name"]
        for part, segs in (("rom", p["rom_segs"]), ("cart", p["cart_segs"])):
            for i, (addr, data) in enumerate(segs):
                out.append("static const uint8_t %s_%s%d[] = {\n%s\n};" % (name, part, i, c_bytes(data)))
            if segs:
                out.append("static const mcs48_seg %s_%s[] = {" % (name, part))
                for i, (addr, data) in enumerate(segs):
                    out.append("    { 0x%04X, %d, %s_%s%d }," % (addr, len(data), name, part, i))
                out.append("};")
            else:
                out.append("static const mcs48_seg %s_%s[1] = { { 0, 0, NULL } };" % (name, part))
        out.append("static const mcs48_sample %s_sample[] = {" % name)
        for idx in p["samples"]:
            out.append('    { %d, "%s\\n" },' % (idx, p["lines"][idx]))
        out.append("};")
    out.append("static const mcs48_probe mcs48_probes[] = {")
    for p in probes:
        name = p["name"]
        out.append('    { "%s", %s_rom, %d, %s_cart, %d, %d, 0x%016XULL, %s_sample, %d },' % (
            name, name, len(p["rom_segs"]), name, len(p["cart_segs"]), len(p["lines"]),
            fnv(p["lines"]), name, len(p["samples"])))
    out.append("};")
    out.append("static const unsigned mcs48_nprobes = %d;" % len(probes))
    return "\n".join(out) + "\n"


def cmd_gen(names):
    work = tempfile.mkdtemp(prefix="mcs48_ab_")
    try:
        probes = []
        for name in names:
            rom, cart = assemble(name, work)
            lines = mame_trace(name, rom, cart, work)
            print("%-10s %7d instructions, hash %016X" % (name, len(lines), fnv(lines)))
            probes.append({"name": name, "rom_segs": segments(rom), "cart_segs": segments(cart),
                           "lines": lines, "samples": sample_indexes(len(lines))})
        with open(GOLDEN, "w") as f:
            f.write(emit_header(probes))
        print("wrote %s (%d probes, %d bytes)" % (GOLDEN, len(probes), os.path.getsize(GOLDEN)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


def cmd_check(names, exe):
    if not exe or not os.access(exe, os.X_OK):
        sys.exit("mcs48_ab.py: --check needs --exe PATH to mcs48_ops_test")
    work = tempfile.mkdtemp(prefix="mcs48_ab_")
    bad = 0
    total = 0
    try:
        for name in names:
            rom, cart = assemble(name, work)
            theirs = mame_trace(name, rom, cart, work)
            total += len(theirs)
            try:
                ours = core_trace(exe, name, rom, cart, work, 2 * len(theirs) + 1000)
            except CoreFailed as e:
                bad += 1
                print("FAIL %-10s the core did not finish: %s" % (name, e))
                continue
            if ours == theirs:
                print("ok   %-10s %7d instructions" % (name, len(theirs)))
                continue
            bad += 1
            for i, (a, b) in enumerate(zip(ours, theirs)):
                if a != b:
                    print("FAIL %-10s line %d (of MAME's %d, ours %d):" % (name, i, len(theirs), len(ours)))
                    print("  pc  a  psw tc tpre p1 p2 r0..r7 cycles")
                    for j in range(max(0, i - 2), i):
                        print("   ", theirs[j])
                    print("  MAME", b)
                    print("  ours", a)
                    break
            else:
                print("FAIL %-10s MAME ran %d instructions, ours %d" % (name, len(theirs), len(ours)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("mcs48_ab: %d probes, %d instructions against live MAME, %d differ" % (len(names), total, bad))
    return 1 if bad else 0


# the I8048 opcodes MAME's table has no handler for (they run as `illegal`)
ILLEGAL = {0x01, 0x06, 0x0B, 0x22, 0x33, 0x38, 0x3B, 0x63, 0x66, 0x73, 0x82, 0x87, 0x8B,
           0x9B, 0xA2, 0xA6, 0xB7, 0xC0, 0xC1, 0xC2, 0xC3, 0xD6, 0xE0, 0xE1, 0xE2, 0xF3}


def cmd_coverage(names, exe):
    if not exe or not os.access(exe, os.X_OK):
        sys.exit("mcs48_ab.py: --coverage needs --exe PATH to mcs48_ops_test")
    work = tempfile.mkdtemp(prefix="mcs48_ab_")
    seen = {}
    try:
        for name in names:
            rom, cart = assemble(name, work)
            lines = core_trace(exe, name, rom, cart, work)
            ops = set()
            for line in lines:
                pc = int(line[:3], 16)
                if pc < ROM_SIZE:
                    ops.add(rom[pc])
                else:
                    ops.add(cart[(pc - 0x400) if pc < 0xC00 else (pc - 0x800)])
            for op in ops:
                seen.setdefault(op, []).append(name)
            print("%-10s %7d instructions, %3d different opcodes" % (name, len(lines), len(ops)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    legal = [op for op in range(256) if op not in ILLEGAL]
    missing = [op for op in legal if op not in seen]
    ill_seen = [op for op in sorted(ILLEGAL) if op in seen]
    print("legal opcodes executed: %d of %d; illegal opcodes executed: %d of %d" % (
        len(legal) - len(missing), len(legal), len(ill_seen), len(ILLEGAL)))
    if missing:
        print("never executed: " + " ".join("%02X" % op for op in missing))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--gen", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--mame", action="store_true")
    mode.add_argument("--coverage", action="store_true")
    mode.add_argument("--list", action="store_true")
    ap.add_argument("--exe", help="the mcs48_ops_test binary (--check)")
    ap.add_argument("probes", nargs="*")
    args = ap.parse_args()

    if args.list:
        print("\n".join(probe_names([])))
        return 0
    names = probe_names(args.probes)
    try:
        if args.gen:
            return cmd_gen(names)
        if args.check:
            return cmd_check(names, args.exe)
        if args.coverage:
            return cmd_coverage(names, args.exe)
        work = tempfile.mkdtemp(prefix="mcs48_ab_")
        try:
            rom, cart = assemble(names[0], work)
            print("\n".join(mame_trace(names[0], rom, cart, work)))
        finally:
            shutil.rmtree(work, ignore_errors=True)
        return 0
    except Missing as e:
        print("mcs48_ab.py: skipped: %s" % e)
        return 77


if __name__ == "__main__":
    sys.exit(main())

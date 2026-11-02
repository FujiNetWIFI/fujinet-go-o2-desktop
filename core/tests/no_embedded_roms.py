#!/usr/bin/env python3
"""Assert that a WITH_O2_ROMS=OFF binary really carries none of the Odyssey 2
/ Videopac ROMs.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each ROM image in
tools/roms and greps the built binary for it. Anything found is a ROM
that got compiled in despite the option, which would make the artifact
non-redistributable.

The images are found the way the importer finds them: by size and CRC-32,
whatever the file is called, and inside zips (MAME's videopac.zip and
friends). The list is every ROM the app imports (the console BIOS and the
G7400's, the 8244 and EF9340 character sets, The Voice's three ROMs) plus the
dumps of consoles it recognises and does not use, none of which may be in a
binary either.

There is no freely-redistributable class of ROM here: all of it is
copyrighted firmware, so a WITH_O2_ROMS=OFF binary is expected to contain none
of it -- see COMPLIANCE.md. (The app's own no-BIOS stand-in, core/o2/stub.a48,
is not a ROM in this sense: it is ours.)

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
import zipfile
import zlib
from pathlib import Path

# (size, CRC-32) of every ROM to look for, from MAME
FINGERPRINTS = {
    (0x400, 0x8016A315): "o2bios.rom",
    (0x400, 0xE20A9F41): "g7400.bin",
    (0x200, 0xB46A3F31): "charset_i8244.bin",
    (0xA00, 0x8DE85988): "charset_ef9340_1.rom",
    (0x800, 0x4BB43724): "sp0256b-019.bin",
    (0x4000, 0x509367B5): "spr128-003.bin",
    (0x4000, 0xE79DFB75): "spr128-004.bin",
    (0x400, 0xA318E8D6): "c52.rom",
    (0x400, 0x11647CA5): "jopac.bin",
    (0x400, 0xE2B23324): "odyssey3.bin",
}

PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def images(romdir):
    """(name, bytes) of every known ROM in the directory or in a zip in it."""
    for f in sorted(romdir.iterdir()):
        if not f.is_file():
            continue
        if zipfile.is_zipfile(f):
            with zipfile.ZipFile(f) as z:
                blobs = [z.read(i) for i in z.infolist()
                         if not i.is_dir() and i.file_size <= 0x4000]
        elif f.stat().st_size <= 0x4000:
            blobs = [f.read_bytes()]
        else:
            continue
        for data in blobs:
            name = FINGERPRINTS.get((len(data), zlib.crc32(data) & 0xFFFFFFFF))
            if name:
                yield name, data


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])
    if not romdir.is_dir():
        print(f"no_embedded_roms: SKIP: {romdir} does not exist -- nothing to check against")
        return 77

    checked = 0
    leaked = []
    unprobeable = []

    for name, data in images(romdir):
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(name)
            continue
        checked += 1
        if chunk in binary:
            leaked.append(name)

    if checked == 0:
        # No real ROMs to check against -- true of every CI and release build,
        # since the copyrighted ROMs (see COMPLIANCE.md) are gitignored
        # and never present on a checkout that isn't a developer's own machine.
        # SKIP (ctest exit code 77), not fail: there is nothing wrong with the
        # binary, just nothing here to probe it with.
        print("no_embedded_roms: SKIP: no ROM images found in "
              f"{romdir} -- nothing to check against")
        return 77

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_O2_ROMS=OFF build")

    if leaked:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} ROM image(s) checked; none embedded, "
          "as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())

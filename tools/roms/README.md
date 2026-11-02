# tools/roms

Developer-only. Drop your own Magnavox Odyssey 2 BIOS here (MAME's `o2.zip`
ROMs `84932.ic11` … `85457.ic14`, or the joined 2K dump) to:

- build with `-DWITH_O2_ROMS=ON`, which embeds the recognised images so a
  development build has a BIOS from the first start (NOT for redistribution);
- give the `no_embedded_roms_*` tests something to probe for, so they check
  rather than skip.

Everything here except `embed-roms.py` and this file is git-ignored. See
COMPLIANCE.md.

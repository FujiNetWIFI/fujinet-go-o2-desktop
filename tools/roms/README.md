# tools/roms

Developer-only. Drop your own Odyssey 2 / Videopac ROMs here, loose or in
MAME's zips (`videopac.zip`, `videopacp.zip`, `i8244.zip`, `ef9340_1.zip`):

| file (MAME's name)      | what                                          |
|-------------------------|-----------------------------------------------|
| `o2bios.rom`            | Odyssey 2 / Videopac G7000 BIOS (1 KB)        |
| `g7400.bin`             | Videopac+ G7400 BIOS (1 KB)                   |
| `charset_i8244.bin`     | 8244/8245 character set (512 B)               |
| `charset_ef9340_1.rom`  | EF9340 character set (2,560 B)                |
| `sp0256b-019.bin`       | The Voice: SP0256 mask ROM (2 KB)             |
| `spr128-003.bin`        | The Voice: speech ROM (16 KB)                 |
| `spr128-004.bin`        | Sid the Spellbinder's speech ROM (16 KB)      |

They are recognised by size and CRC-32, whatever they are called. Then:

- build with `-DWITH_O2_ROMS=ON`, which embeds what is here so a development
  build has its ROMs from the first start (NOT for redistribution);
- the `no_embedded_roms_*` tests have something to probe for, so they check
  rather than skip.

Everything here except `embed-roms.py` and this file is git-ignored. See
COMPLIANCE.md.

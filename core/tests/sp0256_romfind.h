/*
 * sp0256_romfind.h -- find the Intellivoice's SP0256-012 mask ROM for the
 * tests. The ROM is never in the repository. Looked for, in order:
 *   $SP0256_ROM            the 2048-byte file itself
 *   <dir>/sp0256-012.bin   for <dir> in $SP0256_ROM_DIR, $O2_ROM_DIR, $MAME_ROMS
 *   <dir>/intv.zip         (member intvecs/sp0256-012.bin) and
 *   <dir>/intv_voice.zip   (member sp0256-012.bin), via `unzip -p`
 * and finally ~/Workspace/mame/roms as <dir>. Anything found must have CRC32
 * 0DE7579D (SP0256_INTV_ROM_CRC) or it is ignored.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SP0256_ROMFIND_H
#define SP0256_ROMFIND_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP0256_INTV_ROM_CRC 0x0DE7579Du

static inline uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;

    while (n--)
    {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static inline int read_file(const char *path, uint8_t *dst, size_t want)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return 0;
    n = fread(dst, 1, want, f);
    fclose(f);
    return n == want;
}

#ifndef _WIN32
static inline int read_zip_member(const char *zip, const char *member, uint8_t *dst, size_t want)
{
    char cmd[2600];
    FILE *p;
    size_t n;

    if (strchr(zip, '\'') || strchr(member, '\''))
        return 0;
    snprintf(cmd, sizeof cmd, "unzip -p '%s' '%s' 2>/dev/null", zip, member);
    p = popen(cmd, "r");
    if (!p)
        return 0;
    n = fread(dst, 1, want, p);
    pclose(p);
    return n == want;
}
#endif

static inline int find_rom(uint8_t rom[2048], char *how, size_t how_size)
{
    const char *dirs[4];
    char home[600] = "";
    const char *e;

    if ((e = getenv("SP0256_ROM")) && read_file(e, rom, 2048) && crc32_of(rom, 2048) == SP0256_INTV_ROM_CRC)
    {
        snprintf(how, how_size, "%s", e);
        return 1;
    }
    dirs[0] = getenv("SP0256_ROM_DIR");
    dirs[1] = getenv("O2_ROM_DIR");
    dirs[2] = getenv("MAME_ROMS");
    if ((e = getenv("HOME")))
        snprintf(home, sizeof home, "%s/Workspace/mame/roms", e);
    dirs[3] = home[0] ? home : NULL;
    for (int i = 0; i < 4; i++)
    {
        char path[1200];

        if (!dirs[i])
            continue;
        snprintf(path, sizeof path, "%s/sp0256-012.bin", dirs[i]);
        if (read_file(path, rom, 2048) && crc32_of(rom, 2048) == SP0256_INTV_ROM_CRC)
        {
            snprintf(how, how_size, "%s", path);
            return 1;
        }
#ifndef _WIN32
        snprintf(path, sizeof path, "%s/intv.zip", dirs[i]);
        if (read_zip_member(path, "intvecs/sp0256-012.bin", rom, 2048) && crc32_of(rom, 2048) == SP0256_INTV_ROM_CRC)
        {
            snprintf(how, how_size, "%s (intvecs/sp0256-012.bin)", path);
            return 1;
        }
        snprintf(path, sizeof path, "%s/intv_voice.zip", dirs[i]);
        if (read_zip_member(path, "sp0256-012.bin", rom, 2048) && crc32_of(rom, 2048) == SP0256_INTV_ROM_CRC)
        {
            snprintf(how, how_size, "%s (sp0256-012.bin)", path);
            return 1;
        }
#endif
    }
    return 0;
}

#endif /* SP0256_ROMFIND_H */

/*
 * roms -- the Magnavox Odyssey 2 BIOS: what MAME knows, what the user imported,
 * and what a developer build embedded.
 *
 * Only games need it. CONFIG and every FujiNet app boot on this app's own
 * stand-in (core/o2/stub.asm); a commercial game is BIOS byte-code and
 * runs only on the real thing. The BIOS is copyrighted RCA firmware and this
 * project does not redistribute it (see COMPLIANCE.md): "Import BIOS..."
 * copies a user's own image into the ROM directory, recognised by size and
 * CRC-32 against MAME's ROM_START( o2 ) (src/mame/rca/o2.cpp) and
 * stored under MAME's file names.
 *
 * MAME's set is four 512-byte ROMs: ic11 and ic12 are the system ROM the
 * console runs at $0000, ic13 and ic14 the built-in games at $0400, which
 * the FujiNet cart always hides. A dump of all four (2K, TOSEC's "RCA Studio
 * II BIOS"), of the system ROM alone (1K), or the ROMs one by one all work.
 *
 * WITH_O2_ROMS=ON embeds the recognised images a developer put in
 * tools/roms/; published builds are built with it OFF and
 * core/tests/no_embedded_roms.py checks that claim against the shipped
 * binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "roms_embedded.h"
#include "session_internal.h"

enum { B_SET = 0, B_SYSTEM, B_IC11, B_IC12, B_IC13, B_IC14, BIOS_COUNT };

/* MAME's ROMs and the two dumps of them people have. */
static const o2_bios_info s_bios[BIOS_COUNT] = {
    { "o2",    "o2.bin", "Magnavox Odyssey 2 BIOS (2K: system ROM and built-in games)",
      0x800, 0xa494b339u },
    { "system",     "o2-system.bin", "Magnavox Odyssey 2 system ROM (1K)",
      0x400, 0xd3de048cu },
    { "84932.ic11", "84932.ic11", "Magnavox Odyssey 2 ROM ic11 ($0000)", 0x200, 0x283b7e65u },
    { "84933.ic12", "84933.ic12", "Magnavox Odyssey 2 ROM ic12 ($0200)", 0x200, 0xa396b77cu },
    { "85456.ic13", "85456.ic13", "Magnavox Odyssey 2 ROM ic13 ($0400, built-in games)",
      0x200, 0xd25cf97fu },
    { "85457.ic14", "85457.ic14", "Magnavox Odyssey 2 ROM ic14 ($0600, built-in games)",
      0x200, 0x74aa724fu },
};

/* A 2K or 1K image whose CRC is not in the table (a modified BIOS) is
 * imported as the set anyway, with a warning. */
#define MAX_IMAGE 0x800

int o2session_bios_count(void)
{
    return BIOS_COUNT;
}

const o2_bios_info *o2session_bios_info(int i)
{
    return i >= 0 && i < BIOS_COUNT ? &s_bios[i] : NULL;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int built;
    uint32_t c = 0xFFFFFFFFu;

    if (!built) {
        for (uint32_t k = 0; k < 256; k++) {
            uint32_t v = k;
            for (int j = 0; j < 8; j++)
                v = (v & 1) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            table[k] = v;
        }
        built = 1;
    }
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* The whole file, if it is no bigger than `max`; NULL otherwise. */
static uint8_t *read_file(const char *path, uint32_t max, uint32_t *size)
{
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    uint8_t *buf;
    size_t got;

    if (!f)
        return NULL;
    buf = malloc((size_t)max + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)max + 1, f);
    fclose(f);
    if (got == 0 || got > max) {
        free(buf);
        return NULL;
    }
    *size = (uint32_t)got;
    return buf;
}

static int identify(const uint8_t *data, uint32_t size)
{
    const uint32_t crc = crc32_of(data, size);
    for (int i = 0; i < BIOS_COUNT; i++)
        if (s_bios[i].size == size && s_bios[i].crc == crc)
            return i;
    return -1;
}

static void rom_path(const struct o2session *s, const o2_bios_info *b, char *out, int n)
{
    snprintf(out, (size_t)n, "%s/%s", s->roms_dir, b->file);
}

static const o2_embedded_rom *embedded(const o2_bios_info *b)
{
    for (unsigned i = 0; i < o2_embedded_rom_count; i++)
        if (strcmp(o2_embedded_roms[i].file, b->file) == 0)
            return &o2_embedded_roms[i];
    return NULL;
}

/* Image i's bytes from the ROM directory or the embedded table, at its size
 * exactly (a stored custom set is any 2K or 1K). Caller frees. */
static uint8_t *load_image(struct o2session *s, int i, uint32_t *size)
{
    const o2_bios_info *b = &s_bios[i];
    char path[O2_PATH_MAX];
    uint8_t *buf;

    rom_path(s, b, path, sizeof path);
    buf = read_file(path, b->size, size);
    if (buf && *size == b->size)
        return buf;
    free(buf);
    {
        const o2_embedded_rom *e = embedded(b);
        if (e && e->size == b->size) {
            buf = malloc(e->size);
            if (!buf)
                return NULL;
            memcpy(buf, e->data, e->size);
            *size = e->size;
            return buf;
        }
    }
    return NULL;
}

int o2session_bios_available(o2session *s, int i)
{
    const o2_bios_info *b = o2session_bios_info(i);
    char path[O2_PATH_MAX];
    struct stat st;

    if (!b)
        return 0;
    if (embedded(b))
        return 1;
    rom_path(s, b, path, sizeof path);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (uint32_t)st.st_size == b->size;
}

int o2session_have_bios(o2session *s)
{
    return o2session_bios_available(s, B_SET) || o2session_bios_available(s, B_SYSTEM)
        || (o2session_bios_available(s, B_IC11) && o2session_bios_available(s, B_IC12));
}

uint8_t *roms_load_bios(struct o2session *s, uint32_t *size)
{
    uint8_t *bios, *part;
    uint32_t n = 0;

    if ((bios = load_image(s, B_SET, size)) != NULL)
        return bios;
    bios = calloc(1, 0x800);
    if (!bios)
        return NULL;
    if ((part = load_image(s, B_SYSTEM, &n)) != NULL) {
        memcpy(bios, part, 0x400);
        free(part);
    } else {
        uint8_t *lo = load_image(s, B_IC11, &n);
        uint8_t *hi = load_image(s, B_IC12, &n);
        if (!lo || !hi) {
            free(lo);
            free(hi);
            free(bios);
            return NULL;
        }
        memcpy(bios, lo, 0x200);
        memcpy(bios + 0x200, hi, 0x200);
        free(lo);
        free(hi);
    }
    /* the built-in games, if they are here too (only a console with no
     * cartridge would ever show them) */
    if ((part = load_image(s, B_IC13, &n)) != NULL) {
        memcpy(bios + 0x400, part, 0x200);
        free(part);
    }
    if ((part = load_image(s, B_IC14, &n)) != NULL) {
        memcpy(bios + 0x600, part, 0x200);
        free(part);
    }
    *size = 0x800;
    return bios;
}

/* Materialise the embedded images into the ROM directory on first run, so a
 * developer build behaves like a configured install. */
void roms_provision_embedded(struct o2session *s)
{
    for (unsigned i = 0; i < o2_embedded_rom_count; i++) {
        char path[O2_PATH_MAX];
        FILE *f;

        snprintf(path, sizeof path, "%s/%s", s->roms_dir, o2_embedded_roms[i].file);
        f = fopen(path, "rb");
        if (f) {
            fclose(f);   /* the user's own copy wins */
            continue;
        }
        f = fopen(path, "wb");
        if (!f)
            continue;
        fwrite(o2_embedded_roms[i].data, 1, o2_embedded_roms[i].size, f);
        fclose(f);
    }
}

int o2session_media_is_bios(const char *path)
{
    uint32_t size = 0;
    char abs[O2_PATH_MAX];
    uint8_t *buf = read_file(paths_resolve(path, abs, sizeof abs), MAX_IMAGE, &size);
    int known;

    if (!buf)
        return 0;
    known = identify(buf, size) >= 0;
    free(buf);
    return known;
}

int o2session_import_bios(o2session *s, const char *path, char *msg, int msgsz)
{
    uint32_t size = 0;
    uint8_t *buf;
    int idx, custom = 0;
    const o2_bios_info *b;
    char dest[O2_PATH_MAX];
    char abs[O2_PATH_MAX];
    FILE *f;

    if (msg && msgsz > 0)
        msg[0] = '\0';
    path = paths_resolve(path, abs, sizeof abs);
    buf = read_file(path, MAX_IMAGE, &size);
    if (!buf) {
        session_set_error(s, "%s is not a Odyssey 2 BIOS image: the BIOS is 2K "
                             "(or 1K, or four 512-byte ROMs).", path);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "%s", s->last_error);
        return -1;
    }

    idx = identify(buf, size);
    if (idx < 0) {
        if (size != 0x800 && size != 0x400) {
            free(buf);
            session_set_error(s, "%s is not an Magnavox Odyssey 2 BIOS (%u bytes, and not a "
                                 "ROM MAME knows).", path, size);
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "%s", s->last_error);
            return -1;
        }
        idx = size == 0x800 ? B_SET : B_SYSTEM;
        custom = 1;
    }
    b = &s_bios[idx];

    rom_path(s, b, dest, sizeof dest);
    f = fopen(dest, "wb");
    if (!f || fwrite(buf, 1, size, f) != size) {
        if (f)
            fclose(f);
        free(buf);
        session_set_error(s, "Cannot write %s", dest);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "%s", s->last_error);
        return -1;
    }
    fclose(f);

    if (msg && msgsz > 0) {
        if (custom)
            snprintf(msg, (size_t)msgsz,
                     "Imported, but its CRC-32 (%08X) is not one MAME knows. If the "
                     "console misbehaves, suspect it first. It boots at the next power "
                     "cycle (Reset to CONFIG).", crc32_of(buf, size));
        else if (o2session_have_bios(s))
            snprintf(msg, (size_t)msgsz, "Imported %s. The BIOS boots at the next "
                     "power cycle (Reset to CONFIG).", b->desc);
        else
            snprintf(msg, (size_t)msgsz, "Imported %s. The console needs ic11 and "
                     "ic12 (the system ROM): import the other one too.", b->desc);
    }
    free(buf);
    return idx;
}

const char *o2session_roms_path(const o2session *s)
{
    return s->roms_dir;
}

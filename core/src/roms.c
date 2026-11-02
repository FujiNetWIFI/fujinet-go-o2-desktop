/*
 * roms -- the ROMs the Odyssey 2 / Videopac needs and this app does not ship:
 * what MAME knows, what the user imported, and what a developer build
 * embedded.
 *
 * CONFIG and every FujiNet app boot on this app's own stand-in BIOS
 * (core/o2/stub.a48); a commercial game is BIOS byte-code and runs only on
 * the real console ROM. Every machine also needs its video chip's character
 * set to show text, and The Voice needs its speech ROMs. All of that is
 * copyrighted firmware, which this project does not redistribute (see
 * COMPLIANCE.md): "Import BIOS..." copies a user's own dumps into the ROM
 * directory, each recognised by size and CRC-32 against MAME's tables
 * (src/mame/philips/odyssey2.cpp, src/devices/video/{i8244,ef9340_1}.cpp,
 * the Voice's software-list entry) whatever the file is called, and stored
 * under MAME's file name.
 *
 * The input is a file, a zip (MAME's videopac.zip, i8244.zip, videopacp.zip,
 * ef9340_1.zip work as they are) or a folder searched eight levels deep; a
 * zip found in a folder is opened too. A dump of a console this version does
 * not emulate (the French C52, the Jopac, the Odyssey 3) is recognised and
 * reported, not stored. A file of exactly the BIOS's size whose CRC is not in
 * the table may be imported as the console BIOS, but only when the user
 * hands over that one file: a sweep of a folder never guesses.
 *
 * WITH_O2_ROMS=ON embeds the recognised images a developer put in
 * tools/roms/ for local development; published builds are built with it OFF
 * and core/tests/no_embedded_roms.py checks that claim against the shipped
 * binaries.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "roms_embedded.h"
#include "session_internal.h"
#include "zipread.h"

/* The directory walk's depth limit: folders nested this many levels below the
 * one the user chose are searched; deeper ones are not. */
#define MAX_DEPTH 8
/* A zip inside a zip inside a folder is as far as anyone sensibly goes. */
#define MAX_ZIP_DEPTH 2
#define MAX_ROM_SIZE 0x4000u
#define MAX_NESTED_ZIP (8u * 1024 * 1024)

static const o2_rom_item s_items[O2_ROM_COUNT] = {
    [O2_ROM_BIOS] = { "Odyssey 2 / Videopac G7000 BIOS (o2bios.rom)", "Console",
                      "o2bios.rom", 0x400, 0x8016a315u },
    [O2_ROM_BIOS_G7400] = { "Videopac+ G7400 BIOS (g7400.bin)", "Console",
                            "g7400.bin", 0x400, 0xe20a9f41u },
    [O2_ROM_FONT_8244] = { "8244/8245 character set (charset_i8244.bin)", "Video chips",
                           "charset_i8244.bin", 0x200, 0xb46a3f31u },
    [O2_ROM_FONT_EF9340] = { "EF9340 character set (charset_ef9340_1.rom)", "Video chips",
                             "charset_ef9340_1.rom", 0xa00, 0x8de85988u },
    [O2_ROM_VOICE_INTERNAL] = { "The Voice: SP0256 ROM (sp0256b-019.bin)", "The Voice",
                                "sp0256b-019.bin", 0x800, 0x4bb43724u },
    [O2_ROM_VOICE_SPEECH] = { "The Voice: speech ROM (spr128-003.bin)", "The Voice",
                              "spr128-003.bin", 0x4000, 0x509367b5u },
    [O2_ROM_VOICE_SID] = { "Sid the Spellbinder: speech ROM (spr128-004.bin)", "The Voice",
                           "spr128-004.bin", 0x4000, 0xe79dfb75u },
};

/* Dumps MAME has and this version does not use: recognised so the user is
 * told, not stored. */
static const struct {
    uint32_t size, crc;
    const char *file, *what;
} k_unused[] = {
    { 0x400, 0xa318e8d6u, "c52.rom", "the Videopac C52 (France) BIOS" },
    { 0x400, 0x11647ca5u, "jopac.bin", "the Jopac BIOS" },
    { 0x400, 0xe2b23324u, "odyssey3.bin", "the Odyssey 3 BIOS" },
};
#define N_UNUSED ((int)(sizeof k_unused / sizeof k_unused[0]))

int o2session_rom_item_count(void)
{
    return O2_ROM_COUNT;
}

const o2_rom_item *o2session_rom_item(int i)
{
    return i >= 0 && i < O2_ROM_COUNT ? &s_items[i] : NULL;
}

/* ---- the files ---------------------------------------------------------------- */

static void rom_path(const struct o2session *s, int id, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", s->roms_dir, s_items[id].file);
}

static const o2_embedded_rom *embedded(int id)
{
    for (unsigned i = 0; i < o2_embedded_rom_count; i++)
        if (strcmp(o2_embedded_roms[i].file, s_items[id].file) == 0
            && o2_embedded_roms[i].size == s_items[id].size)
            return &o2_embedded_roms[i];
    return NULL;
}

/* The whole file, if it is exactly `want` bytes (0: any size up to `max`);
 * NULL otherwise. */
static uint8_t *read_file(const char *path, uint32_t want, uint32_t max, uint32_t *size)
{
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    uint8_t *buf;
    size_t got;
    uint32_t cap = want ? want : max;

    if (!f)
        return NULL;
    buf = malloc((size_t)cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)cap + 1, f);
    fclose(f);
    if (got == 0 || got > cap || (want && got != want)) {
        free(buf);
        return NULL;
    }
    *size = (uint32_t)got;
    return buf;
}

uint8_t *roms_load(struct o2session *s, int id, uint32_t *size)
{
    char path[O2_PATH_MAX];
    uint8_t *buf;

    if (id < 0 || id >= O2_ROM_COUNT)
        return NULL;
    rom_path(s, id, path, sizeof path);
    buf = read_file(path, s_items[id].size, 0, size);
    if (buf)
        return buf;
    {
        const o2_embedded_rom *e = embedded(id);
        if (e) {
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

int o2session_rom_item_available(o2session *s, int i)
{
    char path[O2_PATH_MAX];
    struct stat st;

    if (!s || i < 0 || i >= O2_ROM_COUNT)
        return 0;
    if (embedded(i))
        return 1;
    rom_path(s, i, path, sizeof path);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode)
        && (uint32_t)st.st_size == s_items[i].size;
}

int o2session_have_bios(o2session *s, int machine)
{
    return o2session_rom_item_available(
        s, machine == O2_MACHINE_VIDEOPAC_PLUS ? O2_ROM_BIOS_G7400 : O2_ROM_BIOS);
}

int o2session_have_font(o2session *s, int machine)
{
    if (!o2session_rom_item_available(s, O2_ROM_FONT_8244))
        return 0;
    return machine != O2_MACHINE_VIDEOPAC_PLUS
        || o2session_rom_item_available(s, O2_ROM_FONT_EF9340);
}

int o2session_have_voice(o2session *s)
{
    return o2session_rom_item_available(s, O2_ROM_VOICE_INTERNAL)
        && o2session_rom_item_available(s, O2_ROM_VOICE_SPEECH);
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

/* ---- recognition ---------------------------------------------------------------- */

enum { NOT_A_ROM = -1, UNUSED_BASE = 100 };

/* The item (an o2_rom_id), UNUSED_BASE + n for a dump this version does not
 * use, or NOT_A_ROM. By size and CRC-32 only: never the name. */
static int identify(uint32_t size, uint32_t crc)
{
    int i;

    for (i = 0; i < O2_ROM_COUNT; i++)
        if (s_items[i].size == size && s_items[i].crc == crc)
            return i;
    for (i = 0; i < N_UNUSED; i++)
        if (k_unused[i].size == size && k_unused[i].crc == crc)
            return UNUSED_BASE + i;
    return NOT_A_ROM;
}

/* Sizes worth reading at all, so a sweep does not slurp every file it meets. */
static int size_wanted(uint32_t size)
{
    int i;

    for (i = 0; i < O2_ROM_COUNT; i++)
        if (s_items[i].size == size)
            return 1;
    return 0;                   /* the unused dumps share the BIOS's size */
}

/* ---- the import ------------------------------------------------------------------ */

typedef struct {
    struct o2session *s;
    int probe;                  /* media_is_rom: look, do not write */
    int found;                  /* probe: something recognised */
    int guess_bios;             /* the user handed over one file: a 1K file of
                                   an unknown CRC may be the BIOS */
    int got[O2_ROM_COUNT];
    int imported;               /* distinct items written */
    int unused_seen[N_UNUSED];
    int guessed;                /* the BIOS was taken on size alone */
    uint32_t guessed_crc;
    int write_failed;
    char write_error[O2_PATH_MAX + 48];
    char first_dest[O2_PATH_MAX];
    /* the last candidate looked at, for "what is this file" */
    uint32_t last_size, last_crc;
    int last_valid;             /* last_crc was computed */
    int plain_file;             /* the chosen path is one file, not a zip or folder */
} import_ctx;

/* Written through a .part file and renamed, so a failed write leaves the old
 * ROM in place. */
static int write_rom(import_ctx *ctx, int id, const uint8_t *data, uint32_t size)
{
    char path[O2_PATH_MAX], tmp[O2_PATH_MAX + 8];
    FILE *f;
    int ok;

    if (ctx->got[id])
        return 1;               /* identical bytes (same CRC): already done */
    paths_mkdir_p(ctx->s->roms_dir);
    rom_path(ctx->s, id, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0)
        ok = 0;
    if (ok) {
#if defined(_WIN32)
        remove(path);           /* rename() will not replace a file here */
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        snprintf(ctx->write_error, sizeof ctx->write_error, "Cannot write %s", path);
        ctx->write_failed = 1;
        return 0;
    }
    ctx->got[id] = 1;
    ctx->imported++;
    if (!ctx->first_dest[0])
        snprintf(ctx->first_dest, sizeof ctx->first_dest, "%s", path);
    return 1;
}

/* One candidate image. */
static void take(import_ctx *ctx, const uint8_t *data, uint32_t size)
{
    const uint32_t crc = zipread_crc32(data, size);
    const int id = identify(size, crc);

    ctx->last_size = size;
    ctx->last_crc = crc;
    ctx->last_valid = 1;
    if (id >= UNUSED_BASE) {
        ctx->unused_seen[id - UNUSED_BASE] = 1;
        ctx->found = 1;
        return;
    }
    if (id >= 0) {
        ctx->found = 1;
        if (!ctx->probe)
            write_rom(ctx, id, data, size);
        return;
    }
    if (ctx->guess_bios && size == s_items[O2_ROM_BIOS].size) {
        ctx->found = 1;
        if (!ctx->probe && write_rom(ctx, O2_ROM_BIOS, data, size)) {
            ctx->guessed = 1;
            ctx->guessed_crc = crc;
        }
    }
}

static int name_is_zip(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".zip") == 0 || strcmp(name + n - 4, ".ZIP") == 0
                     || strcmp(name + n - 4, ".Zip") == 0);
}

static void import_zip(import_ctx *ctx, zipread *z, int zdepth)
{
    int i;

    for (i = 0; i < zipread_count(z) && !(ctx->probe && ctx->found); i++) {
        uint32_t size = zipread_size(z, i);
        uint8_t *data = NULL;
        const char *name = zipread_name(z, i);

        if (name_is_zip(name) && zdepth < MAX_ZIP_DEPTH && size <= MAX_NESTED_ZIP) {
            if (zipread_extract(z, i, &data, &size, MAX_NESTED_ZIP) == 0) {
                zipread *inner = zipread_open_memory(data, size);
                free(data);
                if (inner) {
                    import_zip(ctx, inner, zdepth + 1);
                    zipread_close(inner);
                }
            }
            continue;
        }
        if (!size_wanted(size))
            continue;
        if (zipread_extract(z, i, &data, &size, MAX_ROM_SIZE) == 0) {
            int guess = ctx->guess_bios;
            ctx->guess_bios = 0;     /* never from inside a zip */
            take(ctx, data, size);
            ctx->guess_bios = guess;
            free(data);
        }
    }
}

static void import_path(import_ctx *ctx, const char *path, int depth);

static void import_dir(import_ctx *ctx, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL && !(ctx->probe && ctx->found)) {
        char path[O2_PATH_MAX];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        import_path(ctx, path, depth + 1);
    }
    closedir(d);
}

/* `depth` is how many folders below the chosen one this path is (0 for the
 * chosen path itself). */
static void import_path(import_ctx *ctx, const char *path, int depth)
{
    struct stat st;
    zipread *z;

    if (stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (depth <= MAX_DEPTH && !ctx->probe)
            import_dir(ctx, path, depth);
        return;
    }
    if (!S_ISREG(st.st_mode))
        return;

    /* only a file the user named is ever taken on size alone */
    if (depth > 0)
        ctx->guess_bios = 0;

    z = zipread_open(path);
    if (z) {
        import_zip(ctx, z, 0);
        zipread_close(z);
        return;
    }
    if (depth == 0)
        ctx->plain_file = 1;
    ctx->last_size = (uint32_t)st.st_size;
    ctx->last_valid = 0;
    if (size_wanted((uint32_t)st.st_size)) {
        uint32_t size = 0;
        uint8_t *data = read_file(path, (uint32_t)st.st_size, 0, &size);
        if (data) {
            take(ctx, data, size);
            free(data);
        }
    }
}

static const char *base_of(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

#if defined(__GNUC__)
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#endif
static void say(char *msg, int msgsz, int *len, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!msg || msgsz <= 0 || *len >= msgsz - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(msg + *len, (size_t)(msgsz - *len), fmt, ap);
    va_end(ap);
    if (n > 0)
        *len += n < msgsz - *len ? n : msgsz - *len - 1;
}

int roms_import_ex(struct o2session *s, const char *path, char *msg, int msgsz,
                   char *first_dest, int destsz)
{
    import_ctx *ctx;
    struct stat st;
    char abs[O2_PATH_MAX];
    int len = 0, i, n, rc;

    if (msg && msgsz > 0)
        msg[0] = '\0';
    if (first_dest && destsz > 0)
        first_dest[0] = '\0';
    if (!s || !path || !*path) {
        session_set_error(s, "No file to import");
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "No file to import");
        return -1;
    }
    path = paths_resolve(path, abs, sizeof abs);
    if (stat(path, &st) != 0) {
        session_set_error(s, "Cannot read %s", path);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot read %s", path);
        return -1;
    }

    ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        session_set_error(s, "Out of memory");
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Out of memory");
        return -1;
    }
    ctx->s = s;
    ctx->guess_bios = S_ISREG(st.st_mode);
    import_path(ctx, path, 0);

    if (ctx->write_failed) {
        session_set_error(s, "%s", ctx->write_error);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "%s", ctx->write_error);
        free(ctx);
        return -1;
    }

    if (ctx->imported > 0) {
        say(msg, msgsz, &len, "Imported ");
        n = 0;
        for (i = 0; i < O2_ROM_COUNT; i++)
            if (ctx->got[i])
                say(msg, msgsz, &len, "%s%s", n++ ? ", " : "", s_items[i].file);
        say(msg, msgsz, &len, ".");
        if (ctx->got[O2_ROM_FONT_EF9340])
            say(msg, msgsz, &len,
                " MAME marks the EF9340 character set a bad dump (taken from the "
                "datasheet): Videopac+ text may show some wrong glyphs.");
        if (ctx->guessed)
            say(msg, msgsz, &len,
                " That file's CRC-32 (%08X) is not one MAME knows: it was taken as the "
                "console BIOS because of its size. If the console misbehaves, suspect it first.",
                ctx->guessed_crc);
    }
    for (i = 0; i < N_UNUSED; i++)
        if (ctx->unused_seen[i])
            say(msg, msgsz, &len, "%sRecognised %s (%s): not used by this version.",
                len ? " " : "", k_unused[i].file, k_unused[i].what);
    if (len == 0) {
        if (ctx->plain_file) {
            say(msg, msgsz, &len, "%s is not a ROM this app imports (%u bytes",
                base_of(path), ctx->last_size);
            if (ctx->last_valid)
                say(msg, msgsz, &len, ", CRC-32 %08X", ctx->last_crc);
            say(msg, msgsz, &len, "). The console BIOS is 1,024 bytes: MAME's o2bios.rom.");
        } else
            say(msg, msgsz, &len,
                "No Odyssey 2 / Videopac ROM that this app uses was found in %s.", path);
    } else if (ctx->imported > 0) {
        say(msg, msgsz, &len, " Takes effect at the next power cycle (Reset to CONFIG).");
    }

    if (first_dest && destsz > 0)
        snprintf(first_dest, (size_t)destsz, "%s", ctx->first_dest);
    rc = ctx->imported;
    free(ctx);
    return rc;
}

int o2session_rom_import(o2session *s, const char *path, char *msg, int msgsz)
{
    return roms_import_ex(s, path, msg, msgsz, NULL, 0);
}

int o2session_media_is_rom(const char *path)
{
    import_ctx *ctx;
    struct stat st;
    char abs[O2_PATH_MAX];
    int found;

    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return 0;
    ctx->probe = 1;             /* guess_bios stays 0: an unknown 1K file is a cartridge */
    import_path(ctx, path, 0);
    found = ctx->found;
    free(ctx);
    return found;
}

const char *o2session_roms_path(const o2session *s)
{
    return s->roms_dir;
}

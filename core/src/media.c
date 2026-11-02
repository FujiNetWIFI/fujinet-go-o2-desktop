/*
 * media -- routing a file the user dropped on the window to the right place.
 *
 * Three destinations, and which one a file wants is not a matter of taste:
 *
 *   BIOS images (recognised by size and CRC, not by name -- a BIOS dump is
 *   a .bin like any cartridge) go to the ROM directory, through
 *   o2session_import_bios.
 *
 *   Cartridges (.st2, .bin, .rom) go to the cartridge directory and are
 *   opened onto the FujiNet cartridge directly, whose own planner (s2map)
 *   serves them.
 *
 *   Anything else goes to the FujiNet SD folder, because FujiNet is what
 *   serves it.
 *
 * Ported from the SMS sibling's media.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static int ext_is(const char *path, const char *const *exts)
{
    const char *dot = strrchr(base_name(path), '.');
    int i;
    if (!dot) return 0;
    for (i = 0; exts[i]; i++) {
        const char *a = dot + 1, *b = exts[i];
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* What MAME's o2 slot takes (st2, bin, rom). The cartridge plans every
 * image itself (s2map_plan: the ST2 header, or a raw image at $0400), so the
 * extension only routes the file. */
static const char *const cart_exts[] = { "st2", "bin", "rom", NULL };

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[16384];
    size_t n;

    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

int o2session_import_media(o2session *s, const char *src_path,
                            char *dest_out, int dest_sz)
{
    const char *name;
    const char *dir;
    struct stat st;
    char abs[O2_PATH_MAX];

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);

    if (o2session_media_is_bios(src_path)) {
        char msg[256];
        int idx = o2session_import_bios(s, src_path, msg, sizeof msg);
        if (idx < 0)
            return -1;
        snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->roms_dir,
                 idx < o2session_bios_count() ? o2session_bios_info(idx)->file : name);
        return 0;
    }

    if (ext_is(src_path, cart_exts)) {
        dir = s->carts_dir;
    } else {
        /* The SD tree only exists once the FujiNet runtime has been
         * provisioned. Test the DIRECTORY, not just the path string: the
         * path is always computed, so a string check passes and the copy
         * then fails with "could not copy", which tells the user nothing
         * about the actual problem. */
        if (!s->fujinet_sd[0] ||
            stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            session_set_error(s,
                "%s is not a cartridge, so FujiNet would serve it -- but the "
                "FujiNet runtime is not available, so there is nowhere to "
                "put it.", name);
            return -1;
        }
        dir = s->fujinet_sd;
    }

    snprintf(dest_out, (size_t)dest_sz, "%s/%s", dir, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, dir);
        return -1;
    }
    return 0;
}

int o2session_media_is_cartridge(const char *path)
{
    return path ? ext_is(path, cart_exts) : 0;
}

/* Into the SD root, so CONFIG shows it at the top of the SD host with no
 * navigation. FujiNet pushes .st2, .bin and .rom files to the cartridge
 * (lib/media/rs232/diskType.cpp: MEDIATYPE_ROM), which plans whatever its
 * s2map can. */
int o2session_import_cart_to_sd(o2session *s, const char *src_path,
                                 char *dest_out, int dest_sz)
{
    const char *name;
    char abs[O2_PATH_MAX];
    struct stat st;

    if (!src_path || !*src_path) {
        session_set_error(s, "No file to import");
        return -1;
    }
    src_path = paths_resolve(src_path, abs, sizeof abs);
    name = base_name(src_path);
    if (!s->fujinet_sd[0] ||
        stat(s->fujinet_sd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        session_set_error(s,
            "The FujiNet runtime is not available, so there is no SD folder "
            "to import %s into.", name);
        return -1;
    }
    {
        char why[256];
        if (!o2session_check_cart(src_path, why, sizeof why)) {
            session_set_error(s, "%s: %s", name, why);
            return -1;
        }
    }
    snprintf(dest_out, (size_t)dest_sz, "%s/%s", s->fujinet_sd, name);
    if (copy_file(src_path, dest_out) != 0) {
        session_set_error(s, "Could not copy %s to %s", name, s->fujinet_sd);
        return -1;
    }
    return 0;
}

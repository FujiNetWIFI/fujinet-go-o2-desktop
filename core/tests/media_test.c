/*
 * media_test -- where a dropped file goes, Import Cartridge to SD, and
 * Import BIOS (the 2K set, or the system ROM's two 512-byte halves).
 *
 * No RCA firmware is used: the BIOS images here are filler whose last four
 * bytes are chosen so the CRC-32 matches an entry in MAME's table (a CRC is
 * linear, so any four bytes can steer it), which exercises identification
 * by size and CRC exactly as a real image would.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "o2session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void write_file(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    for (i = 0; i < n; i++) fputc((int)((i * 7 + 3) & 0xff), f);
    fclose(f);
}

static uint32_t table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
}

/* Filler of `size` bytes whose CRC-32 is `crc`: run the register forward
 * over all but the last four bytes, run it backward from the wanted final
 * register over four zero bytes, and the difference is the four bytes. */
static void write_forged(const char *path, uint32_t size, uint32_t crc)
{
    uint8_t *b = malloc(size);
    uint32_t s = 0xFFFFFFFFu, t = crc ^ 0xFFFFFFFFu;
    FILE *f;

    for (uint32_t i = 0; i < size; i++)
        b[i] = (uint8_t)(i * 13 + 5);
    for (uint32_t i = 0; i < size - 4; i++)
        s = (s >> 8) ^ table[(s ^ b[i]) & 0xFF];
    for (int k = 0; k < 4; k++) {
        int j = 0;
        while ((table[j] >> 24) != (t >> 24))
            j++;
        t = ((t ^ table[j]) << 8) | (uint32_t)j;
    }
    s ^= t;
    for (int k = 0; k < 4; k++)
        b[size - 4 + k] = (uint8_t)(s >> (8 * k));
    f = fopen(path, "wb");
    fwrite(b, 1, size, f);
    fclose(f);
    free(b);
}

/* A minimal ST2: the 256-byte header ("RCA2", 2 blocks, the data block at
 * page $04) and one block of filler. */
/* An ST2 game of `blocks` blocks on the pages a game may use, every page
 * past $0400 in turn ($C4 on: the bank window), padded out to `len`. */
static void write_big_st2(const char *path, unsigned blocks, size_t len)
{
    uint8_t *img = calloc(1, len);
    unsigned pg = 0x04, i;
    FILE *f;

    memcpy(img, "RCA2", 4);
    img[4] = (uint8_t)(blocks + 1);
    for (i = 0; i < blocks; i++, pg++) {
        while ((pg & 0x0F) < 4 || pg == 0x08 || pg == 0x09)
            pg++;
        img[0x40 + i] = (uint8_t)pg;
    }
    f = fopen(path, "wb");
    fwrite(img, 1, len, f);
    fclose(f);
    free(img);
}

static void write_st2(const char *path)
{
    uint8_t img[512];
    FILE *f;

    memset(img, 0, sizeof img);
    memcpy(img, "RCA2", 4);
    img[4] = 2;
    img[0x40] = 0x04;
    for (int i = 0; i < 256; i++)
        img[256 + i] = (uint8_t)(i * 3 + 1);
    f = fopen(path, "wb");
    fwrite(img, 1, sizeof img, f);
    fclose(f);
}

static int bios_index(const char *name)
{
    for (int i = 0; i < o2session_bios_count(); i++)
        if (strcmp(o2session_bios_info(i)->name, name) == 0)
            return i;
    return -1;
}

int main(void)
{
    char cfg[512], data[512], src[700], dest[1200], sd[1200], msg[512];
    o2session_paths p;
    o2session *s;
    const o2_bios_info *bi;
    int idx;

    crc_init();
    test_tmpdir(cfg, sizeof cfg, "mcfg");
    test_tmpdir(data, sizeof data, "mdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = o2session_new(&p);
    if (!s) return 1;

    /* routing by extension */
    check(o2session_media_is_cartridge("game.st2"), ".st2 is a cartridge");
    check(o2session_media_is_cartridge("GAME.ST2"), ".ST2 is a cartridge (case-insensitive)");
    check(o2session_media_is_cartridge("game.bin") && o2session_media_is_cartridge("game.rom"),
          ".bin and .rom are cartridges unless they are a BIOS");
    check(!o2session_media_is_cartridge("game.sms"), ".sms is not (another console's)");
    check(!o2session_media_is_cartridge("notes.txt"), ".txt is nothing");

    snprintf(src, sizeof src, "%s/spacewar.st2", cfg);
    write_st2(src);
    check(o2session_check_cart(src, msg, sizeof msg), "the cartridge plans a minimal ST2");
    check(!o2session_cart_is_app(src), "a game, not a FujiNet app (no claim)");
    check(o2session_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, o2session_carts_path(s), strlen(o2session_carts_path(s))) == 0,
          "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    snprintf(src, sizeof src, "%s/banked.st2", cfg);
    write_big_st2(src, 166, 256 + 166 * 256);
    check(o2session_check_cart(src, msg, sizeof msg),
          "a 42,752-byte ST2 of 166 blocks, the bank window's among them, plans");

    snprintf(src, sizeof src, "%s/big.st2", cfg);
    write_big_st2(src, 166, 50000);
    check(!o2session_check_cart(src, msg, sizeof msg) && strstr(msg, "larger") != NULL,
          "an image past 49,408 bytes is refused, and says why");

    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_file(src, 10);
    check(o2session_import_media(s, src, dest, sizeof dest) == -1,
          "anything else is refused while there is no SD folder");
    check(strstr(o2session_last_error(s), "nowhere to put it") != NULL, "with a useful message");

    /* Import Cartridge to SD. The SD folder needs the runtime's
     * provisioning; make it by hand here. */
    snprintf(src, sizeof src, "%s/tennis.st2", cfg);
    write_st2(src);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/tennis.st2", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");

    /* the BIOS table */
    check(o2session_bios_count() == 6, "MAME's set, the system ROM and the four ROMs are listed");
    idx = bios_index("o2");
    bi = idx >= 0 ? o2session_bios_info(idx) : NULL;
    check(bi && bi->size == 0x800 && bi->crc == 0xa494b339u, "the set: 2K, CRC A494B339");
    check(!o2session_have_bios(s), "nothing is imported yet: the stand-in boots");

    /* one half of the system ROM is not enough */
    snprintf(src, sizeof src, "%s/84932.ic11", cfg);
    write_forged(src, 0x200, 0x283b7e65u);
    check(o2session_media_is_bios(src), "ic11 is recognised by size and CRC");
    idx = o2session_import_bios(s, src, msg, sizeof msg);
    check(idx == bios_index("84932.ic11"), "and imported as ic11");
    check(!o2session_have_bios(s) && strstr(msg, "the other one") != NULL,
          "but without ic12 there is no system ROM yet, and the message says so");
    snprintf(src, sizeof src, "%s/whatever-name.bin", cfg);
    write_forged(src, 0x200, 0xa396b77cu);
    check(o2session_import_media(s, src, dest, sizeof dest) == 0, "a dropped ic12 is imported");
    check(strstr(dest, "84933.ic12") != NULL && exists(dest), "under MAME's file name in the ROM directory");
    check(o2session_have_bios(s), "and ic11 + ic12 make the console's BIOS");

    /* the whole 2K */
    snprintf(src, sizeof src, "%s/Magnavox Odyssey 2 BIOS (1976)(RCA).bin", cfg);
    write_forged(src, 0x800, 0xa494b339u);
    idx = o2session_import_bios(s, src, msg, sizeof msg);
    check(idx == bios_index("o2") && o2session_bios_available(s, idx), "the 2K set imports");
    check(strstr(msg, "next power cycle") != NULL, "and boots at the next power cycle");

    /* an unknown 2K image: imported as the set, with a warning */
    snprintf(src, sizeof src, "%s/patched-bios.bin", cfg);
    write_file(src, 0x800);
    check(!o2session_media_is_bios(src), "an unknown 2K image is not recognised");
    idx = o2session_import_bios(s, src, msg, sizeof msg);
    check(idx == bios_index("o2"), "Import BIOS takes it as the set");
    check(strstr(msg, "CRC-32") != NULL, "with a warning naming its CRC");

    /* and something that is not a BIOS at all */
    snprintf(src, sizeof src, "%s/tiny.bin", cfg);
    write_file(src, 1000);
    check(o2session_import_bios(s, src, msg, sizeof msg) == -1, "a 1000-byte file is refused");
    check(strstr(msg, "not an Magnavox Odyssey 2 BIOS") != NULL, "and says so");

    o2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

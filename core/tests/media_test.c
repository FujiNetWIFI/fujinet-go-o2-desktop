/*
 * media_test -- where a dropped file goes (a ROM to the ROM directory, a
 * cartridge to the cartridge directory, anything else to the FujiNet SD
 * folder), what makes a cartridge image acceptable, and Import Cartridge to SD.
 * No machine is started; no firmware is used.
 *
 * The ROM images here are filler whose last four bytes are chosen so the
 * CRC-32 matches an entry in MAME's table (a CRC is linear, so any four bytes
 * can steer it), which exercises identification by size and CRC exactly as a
 * real image would. The cartridge images are filler too: a FujiNet app is one
 * with the "FUJI" claim where the cartridge looks for it.
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

static void write_bytes(const char *path, const uint8_t *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fwrite(b, 1, n, f);
    fclose(f);
}

static void write_filler(const char *path, size_t n)
{
    uint8_t *b = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)((i * 7 + 3) & 0xff);
    write_bytes(path, b, n);
    free(b);
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
    write_bytes(path, b, size);
    free(b);
}

/* A cartridge image of `size` bytes that claims the mailbox: "FUJI" where the
 * firmware's planner looks for it ($F2C of the first bank, which a 2K bank
 * mirrors at $B2C: offset $72C of the file's last 2K). */
static void write_app(const char *path, size_t size)
{
    uint8_t *b = calloc(1, size);
    memset(b, 0xea, size);
    memcpy(b + size - 2048 + 0x72c, "FUJI", 4);
    write_bytes(path, b, size);
    free(b);
}

/* An APPBANK image: a fixed 1K, then `pages` 2K pages, "APPB",1,pages at
 * $F30 of page 0 and the claim at $F2C. */
static void write_appbank(const char *path, unsigned pages)
{
    size_t size = 1024 + (size_t)pages * 2048;
    uint8_t *b = calloc(1, size);
    memcpy(b + 1024 + 0x72c, "FUJI", 4);
    memcpy(b + 1024 + 0x730, "APPB", 4);
    b[1024 + 0x734] = 1;
    b[1024 + 0x735] = (uint8_t)pages;
    write_bytes(path, b, size);
    free(b);
}

int main(void)
{
    char cfg[400], data[500], src[500], dest[900], sd[900], why[256];
    o2session_paths p;
    o2session *s;

    crc_init();
    test_tmpdir(cfg, sizeof cfg, "mcfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = o2session_new(&p);
    if (!s) return 1;

    /* routing by extension */
    check(o2session_media_is_cartridge("game.bin") && o2session_media_is_cartridge("game.rom")
          && o2session_media_is_cartridge("game.o2"), ".bin, .rom and .o2 are cartridges");
    check(o2session_media_is_cartridge("GAME.BIN") && o2session_media_is_cartridge("Game.O2"),
          "case-insensitively");
    check(!o2session_media_is_cartridge("game.st2") && !o2session_media_is_cartridge("game.sms"),
          "another console's extensions are not");
    check(!o2session_media_is_cartridge("notes.txt") && !o2session_media_is_cartridge("noext")
          && !o2session_media_is_cartridge(NULL), "nor are .txt, nothing, or NULL");

    /* what makes a cartridge acceptable */
    snprintf(src, sizeof src, "%s/game.bin", cfg);
    write_filler(src, 2048);
    check(o2session_check_cart(src, why, sizeof why) == 1, "a 2K game plans");
    check(!o2session_cart_is_app(src), "a game is not a FujiNet app");

    snprintf(src, sizeof src, "%s/app.bin", cfg);
    write_app(src, 2048);
    check(o2session_check_cart(src, why, sizeof why) == 1 && o2session_cart_is_app(src),
          "a 2K image with the claim is a FujiNet app");
    snprintf(src, sizeof src, "%s/app6.bin", cfg);
    write_app(src, 6144);
    check(o2session_cart_is_app(src), "so is a 6K one (two 3K banks)");
    snprintf(src, sizeof src, "%s/appbank.bin", cfg);
    write_appbank(src, 6);
    check(o2session_check_cart(src, why, sizeof why) == 1 && o2session_cart_is_app(src),
          "and an APPBANK image (1K and six 2K pages)");

    snprintf(src, sizeof src, "%s/odd.bin", cfg);
    write_filler(src, 1500);
    check(o2session_check_cart(src, why, sizeof why) == 1 && !o2session_cart_is_app(src),
          "a 1500-byte game still runs from the slot as its own cartridge");
    snprintf(src, sizeof src, "%s/huge.bin", cfg);
    write_filler(src, 70000);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "larger") != NULL,
          "an image past what the cartridge can serve is refused, and says why");
    snprintf(src, sizeof src, "%s/empty.bin", cfg);
    write_bytes(src, (const uint8_t *)"", 0);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "empty") != NULL, "an empty file is refused");
    snprintf(src, sizeof src, "%s/missing.bin", cfg);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "cannot open") != NULL,
          "a missing file says so");

    /* the cartridge types MAME's slot knows by CRC */
    snprintf(src, sizeof src, "%s/chess.bin", cfg);
    write_forged(src, 2048, 0x77066338u);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "Chess Module") != NULL,
          "the Chess Module is refused, with the reason");
    snprintf(src, sizeof src, "%s/home.bin", cfg);
    write_forged(src, 2048, 0x8e824b98u);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "Home Computer Module") != NULL,
          "so is the Home Computer Module");
    snprintf(src, sizeof src, "%s/4in1.bin", cfg);
    write_forged(src, 2048, 0x3bfef56bu);
    check(o2session_check_cart(src, why, sizeof why) == 0 && strstr(why, "4 KB") != NULL,
          "a 4-in-1 that is not 4 KB is refused as MAME refuses it");
    write_forged(src, 4096, 0x3bfef56bu);
    check(o2session_check_cart(src, why, sizeof why) == 1 && !o2session_cart_is_app(src), "a 4 KB one is accepted");
    snprintf(src, sizeof src, "%s/rally.bin", cfg);
    write_filler(src, 16384);
    check(o2session_check_cart(src, why, sizeof why) == 1, "a 16K image is a rally cartridge, accepted");
    snprintf(src, sizeof src, "%s/sid.bin", cfg);
    write_forged(src, 4096, 0x1b9f2f16u);
    check(o2session_check_cart(src, why, sizeof why) == 1 && !o2session_cart_is_app(src),
          "Sid the Spellbinder is a game like another (it needs The Voice when it is loaded)");

    /* a cartridge drops into the cartridge directory */
    snprintf(src, sizeof src, "%s/spacewar.bin", cfg);
    write_filler(src, 2048);
    check(o2session_import_media(s, src, dest, sizeof dest) == 0, "a cartridge imports");
    check(strncmp(dest, o2session_carts_path(s), strlen(o2session_carts_path(s))) == 0,
          "into the cartridge directory");
    check(exists(dest), "and the copy exists");

    /* a ROM goes to the ROM directory, whatever the file is called */
    snprintf(src, sizeof src, "%s/Philips Videopac G7000 BIOS.bin", cfg);
    write_forged(src, 1024, 0x8016a315u);
    check(o2session_media_is_rom(src), "a BIOS dump is a ROM, not a cartridge, though it is a .bin");
    check(o2session_import_media(s, src, dest, sizeof dest) == 0, "a dropped BIOS imports");
    check(strncmp(dest, o2session_roms_path(s), strlen(o2session_roms_path(s))) == 0
          && strstr(dest, "o2bios.rom") != NULL && exists(dest), "under MAME's name in the ROM directory");
    check(o2session_have_bios(s, O2_MACHINE_ODYSSEY2), "and the console has a BIOS");
    snprintf(src, sizeof src, "%s/c52.rom", cfg);
    write_forged(src, 1024, 0xa318e8d6u);
    check(o2session_import_media(s, src, dest, sizeof dest) == -1
          && strstr(o2session_last_error(s), "not used by this version") != NULL,
          "a dump of a console this version does not emulate says so");

    /* anything else goes to the SD folder, which needs FujiNet's runtime */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    write_filler(src, 10);
    check(o2session_import_media(s, src, dest, sizeof dest) == -1,
          "anything else is refused while there is no SD folder");
    check(strstr(o2session_last_error(s), "nowhere to put it") != NULL, "with a useful message");

    /* Import Cartridge to SD. The SD folder needs the runtime's
     * provisioning; make it by hand here. */
    snprintf(src, sizeof src, "%s/tennis.bin", cfg);
    write_filler(src, 2048);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == -1,
          "SD import refuses when there is no SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet", data); test_mkdir(sd);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data); test_mkdir(sd);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "SD import copies into the SD root");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/tennis.bin", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at the top of the SD tree");
    snprintf(src, sizeof src, "%s/appsd.bin", cfg);
    write_appbank(src, 4);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "an APPBANK app imports to SD too");
    snprintf(src, sizeof src, "%s/chesssd.bin", cfg);
    write_forged(src, 2048, 0x77066338u);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == -1
          && strstr(o2session_last_error(s), "Chess Module") != NULL, "a cartridge that cannot run is refused for the SD too");
    snprintf(src, sizeof src, "%s/oddsd.bin", cfg);
    write_filler(src, 1500);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == -1
          && strstr(o2session_last_error(s), "FujiNet cartridge cannot serve") != NULL,
          "and so is one the FujiNet cartridge cannot serve (it runs only from the slot)");

    /* with an SD folder, anything else is served by FujiNet */
    snprintf(src, sizeof src, "%s/notes.txt", cfg);
    check(o2session_import_media(s, src, dest, sizeof dest) == 0, "other files go to the SD folder");
    snprintf(sd, sizeof sd, "%s/fujinet/SD/notes.txt", data);
    check(strcmp(dest, sd) == 0 && exists(dest), "at its top");

    o2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

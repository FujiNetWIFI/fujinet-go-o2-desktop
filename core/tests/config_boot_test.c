/*
 * config_boot_test -- CONFIG, driven like a user from keypad A, boots an
 * image through the in-process FujiNet. The test suite's own counter app goes
 * to the SD host (Import Cartridge to SD); then, on CONFIG's own screens: the
 * status screen -> 5 opens the host slots -> 5 opens slot 1 (SD) -> the bar
 * onto the file with 8 -> 5 boots it -> FujiNet mounts it as ROM media and
 * pushes it, the cart stages it, CONFIG arms the swap and hands over, and the
 * stand-in enters the app (its counter moves). Reset to CONFIG brings CONFIG
 * back; then a game booted the same way with no BIOS imported parks the
 * stand-in, and the session says the BIOS is needed.
 *
 * CONFIG's screens are recognised by their text, read from the cartridge's
 * text engine (o2debug_cart.text); the selection bar is the row drawn
 * inverse. Needs nothing but the FujiNet runtime; SKIPs (77) without it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "o2session.h"
#include "o2debug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

#define FILE_NAME "S2COUNTER.BIN"
#define GAME_NAME "S2GAME.BIN"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static o2debug_cart cart;

static void snap(o2debug *d)
{
    o2debug_cart_get(d, &cart);
}

static void row_text(int r, char *line)
{
    for (int c = 0; c < 16; c++)
    {
        uint8_t ch = cart.text[r][c];
        line[c] = ch >= 0x20 && ch < 0x7f ? (char)ch : ' ';
    }
    line[16] = '\0';
}

/* the row (0-9) holding `text`, or -1 */
static int find_row(const char *text)
{
    char line[17];
    for (int r = 0; r < 10; r++)
    {
        row_text(r, line);
        if (strstr(line, text))
            return r;
    }
    return -1;
}

/* the selection bar: the first row 1-8 drawn inverse at column 2 */
static int bar_row(void)
{
    for (int r = 1; r <= 8; r++)
        if (cart.text_inv[r][2])
            return r;
    return -1;
}

static void dump(void)
{
    char line[17];
    for (int r = 0; r < 10; r++)
    {
        row_text(r, line);
        printf("  %d|%s|\n", r, line);
    }
}

static int wait_text(o2debug *d, const char *text, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 100)
    {
        snap(d);
        if (find_row(text) >= 0)
            return 1;
        sleep_ms(100);
    }
    printf("  (no \"%s\" on screen)\n", text);
    dump();
    return 0;
}

/* One keypad A press, held long enough for CONFIG's scan to see it. */
static void press(o2session *s, int key)
{
    o2session_press(s, O2_TARGET_PORT(0, key), 1);
    sleep_ms(150);
    o2session_press(s, O2_TARGET_PORT(0, key), 0);
    sleep_ms(250);
}

/* From the status screen: open slot 1 (SD), the bar onto `name`, boot it.
 * Returns 1 once the cart has swapped (`swaps` counts up). */
static int boot_from_sd(o2session *s, o2debug *d, const char *name, int swaps)
{
    int target, tries, booted = 0;

    press(s, 5);
    if (!wait_text(d, "HOST", 10000))
        return 0;
    press(s, 5);
    if (!wait_text(d, name, 15000))
        return 0;
    /* the bar onto the file, found before the bar reaches it (a long name
     * on the bar scrolls and stops matching) */
    snap(d);
    target = find_row(name);
    for (tries = 0; tries < 12 && bar_row() != target; tries++)
    {
        press(s, 8);
        snap(d);
    }
    if (target <= 0 || bar_row() != target)
    {
        printf("  (the bar never reached %s)\n", name);
        return 0;
    }
    press(s, 5);
    for (int waited = 0; waited < 30000 && !booted; waited += 100)
    {
        snap(d);
        if (find_row("ERROR") >= 0)
            break;
        booted = cart.swaps >= swaps;
        if (!booted)
            sleep_ms(100);
    }
    if (!booted)
        dump();
    return booted;
}

static uint8_t ram(o2debug *d, uint16_t a)
{
    uint8_t v = 0;
    o2debug_attach(d);
    o2debug_read(d, a, &v, 1);
    o2debug_detach(d);
    return v;
}

int main(void)
{
    char cfg[512], data[512], src[700], dest[1200];
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    o2debug *d;
    int ok;

    test_tmpdir(cfg, sizeof cfg, "cbcfg");
    test_tmpdir(data, sizeof data, "cbdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    s = o2session_new(&p);
    if (!s) return 1;

    o2session_default_opts(s, &o);
    o.enable_audio = 0; o.enable_gamepad = 0; o.enable_fujinet = 1;
    if (o2session_start(s, &o) != 0) {
        printf("start failed: %s\n", o2session_last_error(s));
        return 1;
    }
    if (!o2session_fujinet_running(s)) {
        printf("SKIP: no FujiNet runtime available (%s)\n", o2session_last_error(s));
        o2session_free(s);
        return 77;
    }
    d = o2session_debugger(s);

    snprintf(src, sizeof src, "%s/%s", cfg, FILE_NAME);
    test_rom_write(src, test_counter_app, sizeof test_counter_app);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "the app goes to the SD host");
    snprintf(src, sizeof src, "%s/%s", cfg, GAME_NAME);
    test_rom_write(src, test_counter_game, sizeof test_counter_game);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "and a game");

    check(wait_text(d, "FUJINET CONFIG", 20000), "CONFIG's status screen");
    ok = wait_text(d, "CONNECTED", 20000);
    check(ok, "it sees FujiNet's network");

    check(boot_from_sd(s, d, FILE_NAME, 1),
          "5 opens the host slots, 5 opens SD, 8 onto the file, 5 boots it: the cart swaps it in");
    {
        uint8_t a = ram(d, TEST_RAM_COUNTER);
        sleep_ms(300);
        check(ram(d, TEST_RAM_COUNTER) != a, "the app runs on the stand-in (its counter moves)");
    }
    check(o2session_cart_booted_game(s), "the session sees something other than CONFIG");
    check(!o2session_needs_bios(s), "and no BIOS is needed for an app");

    check(o2session_reset_to_config(s) == 0, "Reset to CONFIG");
    check(wait_text(d, "FUJINET CONFIG", 20000), "CONFIG is back");
    check(wait_text(d, "CONNECTED", 20000), "and sees the network again");

    /* a game with no BIOS: FujiNet pushes it and the cart swaps it in, but
     * the stand-in cannot run BIOS byte-code */
    check(!o2session_have_bios(s), "no BIOS is imported");
    check(boot_from_sd(s, d, GAME_NAME, 1), "CONFIG boots the game the same way");
    {
        int parked = 0;
        for (int waited = 0; waited < 3000 && !parked; waited += 50)
        {
            parked = o2session_needs_bios(s);
            if (!parked)
                sleep_ms(50);
        }
        check(parked, "the stand-in parks, and the session says a BIOS is needed");
    }
    check(o2session_reset_to_config(s) == 0, "Reset to CONFIG");
    check(!o2session_needs_bios(s), "and the request goes away");

    o2session_stop(s);
    o2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

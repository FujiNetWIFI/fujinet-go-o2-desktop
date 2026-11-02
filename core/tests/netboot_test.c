/*
 * netboot_test -- the cartridge bring-up's own clients against the
 * in-process FujiNet, opened as cartridges (they are FujiNet apps, so the
 * stand-in runs them with no BIOS):
 *
 *   fujitest  one real transaction (GET_ADAPTERCONFIG_EXTENDED), its answer
 *             printed by the cart's text engine straight from the reply
 *             window;
 *   fujiboot  MOUNT_HOST -> SET_DEVICE_FULLPATH -> MOUNT_IMAGE of
 *             /S2COUNTER.BIN on the SD host (put there by Import Cartridge
 *             to SD): FujiNet pushes it, the cart stages it, BOOTLOCK, the
 *             hand-over, and the stand-in enters the counter app.
 *
 * Run twice by ctest: with the mailbox on the cart's worker thread, and
 * inline (O2_CART_SYNC=1). Needs O2_TESTROM_DIR: fujinet-firmware's
 * pico/o2/build, built with
 *   BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN pico/o2/build.sh fujiboot fujitest
 * (the pinned tree under third_party/ will do). SKIPs (77) without it or
 * without the FujiNet runtime.
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

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* 1 if `text` is anywhere on the cart's text screen */
static int screen_has(o2debug *d, const char *text)
{
    o2debug_cart c;
    o2debug_cart_get(d, &c);
    for (int r = 0; r < 10; r++)
    {
        char line[17];
        for (int col = 0; col < 16; col++)
            line[col] = c.text[r][col] >= 0x20 && c.text[r][col] < 0x7f ? (char)c.text[r][col] : ' ';
        line[16] = '\0';
        if (strstr(line, text))
            return 1;
    }
    return 0;
}

static void dump(o2debug *d)
{
    char out[2048];
    o2debug_command(d, "text", out, sizeof out);
    printf("%s", out);
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
    char cfg[512], data[512], src[700], dest[1200], fujiboot[1200], fujitest[1200];
    const char *dir = getenv("O2_TESTROM_DIR");
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    o2debug *d;
    o2debug_cart c;

    if (!dir || !*dir) {
        printf("SKIP: O2_TESTROM_DIR is not set (see the header)\n");
        return 77;
    }
    snprintf(fujiboot, sizeof fujiboot, "%s/fujiboot.st2", dir);
    snprintf(fujitest, sizeof fujitest, "%s/fujitest.st2", dir);
    if (!file_exists(fujiboot) || !file_exists(fujitest)) {
        printf("SKIP: no fujiboot.st2 / fujitest.st2 in %s\n", dir);
        return 77;
    }

    test_tmpdir(cfg, sizeof cfg, "nbcfg");
    test_tmpdir(data, sizeof data, "nbdata");
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
    printf("mailbox %s\n", getenv("O2_CART_SYNC") ? "inline (O2_CART_SYNC)" : "on the cart's worker");

    snprintf(src, sizeof src, "%s/S2COUNTER.BIN", cfg);
    test_rom_write(src, test_counter_app, sizeof test_counter_app);
    check(o2session_import_cart_to_sd(s, src, dest, sizeof dest) == 0, "the counter app goes to the SD host");

    /* fujitest: one transaction, the adapter's own answer on screen */
    check(o2session_load_cart(s, fujitest) == 0, "fujitest opens");
    {
        int got = 0;
        for (int waited = 0; waited < 20000 && !got; waited += 100)
        {
            got = screen_has(d, "127.0.0.1") || screen_has(d, "ERR 00");
            if (!got)
                sleep_ms(100);
        }
        if (!got)
            dump(d);
        check(got, "GET_ADAPTERCONFIG_EXTENDED answered, printed from the reply window");
    }

    /* fujiboot: mount, push, stage, hand over */
    check(o2session_load_cart(s, fujiboot) == 0, "fujiboot opens");
    {
        int swapped = 0;
        for (int waited = 0; waited < 30000 && !swapped; waited += 100)
        {
            o2debug_cart_get(d, &c);
            swapped = c.swaps >= 1;
            if (!swapped)
                sleep_ms(100);
        }
        if (!swapped)
            dump(d);
        check(swapped, "the image was pushed, staged and swapped in");
        /* (the boot counters are gone by now: the app claims the mailbox,
         * so the swap repainted its status page from scratch) */
    }
    {
        uint8_t a = ram(d, TEST_RAM_COUNTER);
        sleep_ms(300);
        check(ram(d, TEST_RAM_COUNTER) != a, "and it runs on the stand-in (its counter moves)");
    }
    o2debug_cart_get(d, &c);
    check(c.live_mailbox && c.live_size == sizeof test_counter_app,
          "the cart serves the pushed app, every byte of it");

    o2session_stop(s);
    o2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

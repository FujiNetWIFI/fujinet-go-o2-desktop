/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots and paints on the stand-in, the
 * keyboard and the console's CLEAR reach the machine, a FujiNet app opens
 * and runs, CLEAR restarts it, Reset to CONFIG ejects it, an image the
 * cartridge cannot plan is refused with the reason, a game is refused until
 * a BIOS is imported and runs after, and a remembered cartridge boots at the
 * next start. No RCA firmware: the images and the BIOS are the tests' own
 * (test_rom.h).
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

static uint32_t px[O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT];

static int wait_frames(o2session *s, uint64_t *serial, int count, int timeout_ms)
{
    uint64_t start = o2session_frame_count(s);
    int waited = 0;
    while (waited < timeout_ms) {
        o2session_copy_frame(s, px, serial);
        if (o2session_frame_count(s) >= start + (uint64_t)count) return 1;
        sleep_ms(2); waited += 2;
    }
    return 0;
}

/* Poll the cartridge status until it contains `want`, or time out. */
static int wait_status(o2session *s, const char *want, int timeout_ms)
{
    char st[160];
    int waited = 0;
    while (waited < timeout_ms) {
        o2session_cart_status(s, st, sizeof st);
        if (strstr(st, want)) return 1;
        sleep_ms(20); waited += 20;
    }
    printf("  (status: %s)\n", st);
    return 0;
}

/* The cartridge is powered on by the machine thread, after load_cart (or a
 * power cycle) has returned -- a failed BoIP connect must not stall the
 * caller -- so what it reports (booted_game, the status) is not true until
 * the thread gets there. Poll for it, as wait_status does. */
static int wait_booted_game(o2session *s, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!o2session_cart_booted_game(s) == !!want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* The test image keeps its state in RAM; peek at it through the debugger
 * (which stops the machine while attached) and let it run on. */
static uint8_t ram(o2session *s, uint16_t addr)
{
    o2debug *d = o2session_debugger(s);
    uint8_t v = 0;
    o2debug_attach(d);
    o2debug_read(d, addr, &v, 1);
    o2debug_detach(d);
    return v;
}

static int moving(o2session *s)
{
    uint8_t a = ram(s, TEST_RAM_COUNTER);
    sleep_ms(200);
    return ram(s, TEST_RAM_COUNTER) != a;
}

int main(void)
{
    char cfg[512], data[512], app[700], game[700], big[700], bios[700], launch[512], msg[256];
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    uint64_t serial = 0;

    test_tmpdir(cfg, sizeof cfg, "cfg");
    test_tmpdir(data, sizeof data, "data");
    if (!test_getcwd(launch, sizeof launch))
        return 1;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(app, sizeof app, "%s/counter.bin", cfg);
    check(test_rom_write(app, test_counter_app, sizeof test_counter_app) > 0, "wrote a FujiNet app");
    snprintf(game, sizeof game, "%s/game.bin", cfg);
    check(test_rom_write(game, test_counter_game, sizeof test_counter_game) > 0, "wrote a game");
    snprintf(big, sizeof big, "%s/huge.st2", cfg);
    check(test_rom_write(big, NULL, 20000) > 0, "wrote a 20,000-byte image");
    snprintf(bios, sizeof bios, "%s/testbios.bin", cfg);
    check(test_rom_write(bios, test_bios, sizeof test_bios) > 0, "wrote a BIOS of the tests' own");

    s = o2session_new(&p);
    check(s != NULL, "session created");
    if (!s) return 1;

    check(strcmp(o2session_config_path(s), cfg) == 0, "config path is the given tree");
    check(strncmp(o2session_carts_path(s), data, strlen(data)) == 0, "carts dir is under the data tree");
    check(strncmp(o2session_sd_path(s), data, strlen(data)) == 0, "SD path is under the data tree");
    check(strncmp(o2session_roms_path(s), data, strlen(data)) == 0, "ROM dir is under the data tree");

    o2session_set_int(s, "answer", 42);
    o2session_set_str(s, "greeting", "hello");
    check(o2session_get_int(s, "answer", 0) == 42, "int setting round-trips");
    check(strcmp(o2session_get_str(s, "greeting", ""), "hello") == 0, "string setting round-trips");
    check(o2session_get_int(s, "nope", 7) == 7, "missing setting yields its default");

    o2session_default_opts(s, &o);
    check(o.enable_fujinet == 1 && o.enable_audio == 1, "default opts enable FujiNet and audio");
    check(o.use_bios == 1, "an imported BIOS is used by default");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");
    check(!o2session_have_bios(s), "no BIOS imported: the stand-in");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(o2session_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!o2session_is_running(s)) { printf("error: %s\n", o2session_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG runs link-down");
    check(!o2session_cart_booted_game(s), "CONFIG is not a booted game");
    check(!o2session_needs_bios(s), "and needs no BIOS");
    {
        uint64_t z = 0; int lit = 0, i, tries;
        check(o2session_copy_frame(s, px, &z) == 1, "a forced copy (serial 0) always copies");
        /* up to 15 s: a loaded CI runner can take a while to the first paint */
        for (tries = 0; tries < 150; tries++) {
            lit = 0;
            for (i = 0; i < O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT; i++)
                lit += (px[i] & 0xffffff) != 0;
            if (lit > 200) break;
            sleep_ms(100);
            z = 0;
            o2session_copy_frame(s, px, &z);
        }
        check(lit > 200, "the CONFIG client painted something");
    }
    check(o2session_refresh_rate(s) == 60, "60 Hz");

    /* keyboard: the default map */
    check(o2session_key(s, 'q', 1) == 1, "Q is bound (keypad A 1)");
    check(o2session_buttons_held(s, 0) & (1u << O2_ACT_1), "and holds it");
    check(o2session_key(s, 'q', 0) == 1, "and released");
    check(o2session_key(s, O2_KEYSYM_KP_9, 1) == 1 &&
          (o2session_buttons_held(s, 1) & (1u << O2_ACT_3)), "the numeric keypad's 9 is keypad B 3");
    o2session_key(s, O2_KEYSYM_KP_9, 0);
    check(o2session_key(s, O2_KEYSYM_F11, 1) == 0, "an unbound key is ignored");
    check(o2session_key_sysaction(s, O2_KEYSYM_ESCAPE) == O2_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(o2session_target_for_key(s, O2_KEYSYM_F3) == O2_TARGET_SWITCH(O2_SW_CLEAR),
          "F3 is the console's CLEAR");
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* an image the cartridge cannot plan */
    {
        char why[256];
        check(o2session_check_cart(big, why, sizeof why) == 0, "a 20,000-byte image is refused");
        check(strstr(why, "larger") != NULL, "with the reason");
        check(o2session_load_cart(s, big) == -1, "load_cart refuses it");
        check(!o2session_cart_booted_game(s) && o2session_cart_path(s)[0] == '\0',
              "and CONFIG keeps running");
    }

    /* a game, with no BIOS */
    check(o2session_check_cart(game, NULL, 0) == 1 && !o2session_cart_is_app(game),
          "the game plans, and is not an app");
    check(o2session_load_cart(s, game) == -1, "load_cart refuses a game with no BIOS");
    check(strstr(o2session_last_error(s), "BIOS") != NULL, "and says to import one");

    /* open a FujiNet app: served from power-on, the mailbox live */
    check(o2session_cart_is_app(app), "the app is an app");
    check(o2session_load_cart(s, app) == 0, "load_cart");
    check(strcmp(o2session_cart_path(s), app) == 0, "the cart path is remembered");
    check(strcmp(o2session_get_str(s, "cart", ""), app) == 0, "and persisted");
    check(wait_booted_game(s, 1, 5000), "something other than CONFIG is in the cartridge");
    check(moving(s), "the app runs (its counter moves)");

    /* the keypad reaches the program */
    check(ram(s, TEST_RAM_KEY1) == 0, "keypad A 1 reads up");
    o2session_press(s, O2_TARGET_PORT(0, O2_ACT_1), 1);
    sleep_ms(150);
    check(ram(s, TEST_RAM_KEY1) == 1, "and down while held");
    o2session_press(s, O2_TARGET_PORT(0, O2_ACT_1), 0);
    sleep_ms(150);
    check(ram(s, TEST_RAM_KEY1) == 0, "and up again");

    /* CLEAR: the app restarts (the stand-in clears RAM); it is still open */
    {
        uint8_t before = ram(s, TEST_RAM_IRQS);
        check(o2session_clear(s) == 0, "CLEAR");
        sleep_ms(400);
        check(ram(s, TEST_RAM_IRQS) != before, "the app started over");
        check(moving(s), "and runs");
        check(strcmp(o2session_cart_path(s), app) == 0, "still the open cartridge");
    }

    /* Reset to CONFIG: a power cycle that ejects it */
    check(o2session_reset_to_config(s) == 0, "reset to CONFIG");
    check(o2session_cart_path(s)[0] == '\0', "the cartridge is ejected");
    check(strcmp(o2session_get_str(s, "cart", "x"), "") == 0, "and forgotten");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    check(wait_status(s, "link down", 5000), "CONFIG is back");
    check(!o2session_cart_booted_game(s), "no game in the cartridge");
    check(o2session_cart_link_up(s) == 0, "with no runtime the cart reports link down");

    /* Import BIOS: the game opens and runs on it */
    {
        o2debug_io io;
        check(o2session_import_bios(s, bios, msg, sizeof msg) >= 0, "Import BIOS takes the tests' own");
        check(strstr(msg, "CRC-32") != NULL, "with a warning: MAME does not know it");
        check(o2session_have_bios(s), "the console has a BIOS now");
        check(o2session_load_cart(s, game) == 0, "the game opens");
        o2debug_io_get(o2session_debugger(s), &io);
        check(io.have_bios, "the console powered up with the imported BIOS");
        check(moving(s), "and the game runs");
        check(!o2session_needs_bios(s), "nothing is missing");
        check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");

        /* "use the BIOS" off: the stand-in again, and the game refused */
        o2session_set_int(s, "use_bios", 0);
        check(o2session_reset_to_config(s) == 0, "use_bios off, reset to CONFIG");
        o2debug_io_get(o2session_debugger(s), &io);
        check(!io.have_bios, "the stand-in boots");
        check(wait_status(s, "link down", 5000), "CONFIG runs");
        check(o2session_load_cart(s, game) == -1, "and a game is refused again");
        o2session_set_int(s, "use_bios", 1);
    }

    /* A path relative to where the app started still means that file after
     * the process's working directory moves: the in-process FujiNet makes
     * its runtime root the working directory. */
    {
        char rel[64], abs[1200];
        snprintf(rel, sizeof rel, "s2-rel-%ld.bin", (long)test_getpid());
        snprintf(abs, sizeof abs, "%s/%s", launch, rel);
        check(test_rom_write(abs, test_counter_app, sizeof test_counter_app) > 0,
              "wrote an image in the launch directory");
        check(test_chdir(data) == 0, "the working directory moves away");
        check(o2session_check_cart(rel, NULL, 0) == 1, "a bare name still finds it");
        check(o2session_load_cart(s, rel) == 0, "and opens it");
        check(strcmp(o2session_cart_path(s), abs) == 0, "remembered by its full path");
        check(moving(s), "and it runs");
        test_chdir(launch);
        remove(abs);
    }

    /* a remembered cartridge boots at the next start */
    check(o2session_load_cart(s, app) == 0, "open the app again");
    o2session_stop(s);
    check(!o2session_is_running(s), "session stops");
    o2session_free(s);

    s = o2session_new(&p);
    check(s && o2session_get_int(s, "answer", 0) == 42, "settings persisted across sessions");
    if (s) {
        o2session_default_opts(s, &o);
        check(o.cart_path && strcmp(o.cart_path, app) == 0, "the cartridge is remembered");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(o2session_start(s, &o) == 0, "restart");
        check(moving(s), "and it boots the remembered cartridge");
        o2session_stop(s);
        o2session_free(s);
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

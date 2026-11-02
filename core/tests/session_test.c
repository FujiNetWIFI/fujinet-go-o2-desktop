/*
 * session_test -- the frontend contract end to end, through the public API
 * only: settings round-trip and persist across sessions, paths resolve
 * inside the given tree, CONFIG boots on the stand-in and says what it is
 * missing (the character set), the keyboard, the sticks and the console's
 * RESET reach the machine, a FujiNet app opens and runs, RESET restarts it,
 * Reset to CONFIG ejects it, an image the console cannot take is refused with
 * the reason, a game is refused until a BIOS is imported and runs after, the
 * Videopac+ G7400 will not start without its own BIOS, and a remembered
 * cartridge boots at the next start.
 *
 * No firmware: the programs are this test's own 8048 code (hand-assembled
 * below), and the "BIOS" is a 1 KB image of its own whose last four bytes
 * steer its CRC-32 to MAME's (a CRC is linear), so Import BIOS takes it for
 * the real one. The cartridge programs count in the external RAM; the test
 * reads it back through the debugger. The app also calls the stand-in's
 * readkey, so a key held on the host shows up there.
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

/* ---- the programs ----------------------------------------------------------------
 * Cartridge code at $400 (file offset 0 of a 2K image; the FujiNet app's claim
 * "FUJI" is where the firmware looks for it, $F2C = offset $72C of the image).
 *
 *   $400  jmp $406           the stand-in's reset vector
 *   $402  jmp $430           its vblank interrupt vector: the ISR below
 *   $404  retr               its timer interrupt vector (never enabled)
 *   $406  P1 = $AF (RAM chip select low, writes enabled, the VDC deselected)
 *         ram[$12]++         boots: survives RESET, which resets only the 8048
 *         ram[$10] = ram[$11] = 0
 *   $414  loop: [app: A = readkey, bank 0 again; ram[$11] = A]  ram[$10]++
 *   $430  vblank ISR: bank 1, save A and P1, enable the VDC's reads and read
 *         its status (that is what lets go of INT), restore, retr
 *
 * readkey (the stand-in's, at $0B0) turns interrupts on when it returns, which
 * is why a client that calls it needs an ISR that acknowledges them.
 */
#define RAM_COUNTER 0x10
#define RAM_KEY     0x11
#define RAM_BOOTS   0x12

static const uint8_t prologue[] = {
    0x84, 0x06,                 /* 400: jmp 406 */
    0x84, 0x30,                 /* 402: jmp 430 */
    0x93, 0x00,                 /* 404: retr, nop */
    0x23, 0xAF,                 /* 406: mov a,#AFh */
    0x39,                       /* 408: outl p1,a */
    0xB8, RAM_BOOTS,            /* 409: mov r0,#12h */
    0x80,                       /* 40B: movx a,@r0 */
    0x17,                       /* 40C: inc a */
    0x90,                       /* 40D: movx @r0,a */
    0xB8, RAM_COUNTER,          /* 40E: mov r0,#10h */
    0x27,                       /* 410: clr a */
    0x90,                       /* 411: movx @r0,a */
    0x18,                       /* 412: inc r0 */
    0x90,                       /* 413: movx @r0,a */
};
static const uint8_t app_loop[] = {
    0x14, 0xB0,                 /* 414: call 0B0h (readkey) */
    0xC5,                       /* 416: sel rb0 (it returns in bank 1) */
    0xB8, RAM_KEY,              /* 417: mov r0,#11h */
    0x90,                       /* 419: movx @r0,a */
    0xB8, RAM_COUNTER,          /* 41A: mov r0,#10h */
    0x80,                       /* 41C: movx a,@r0 */
    0x17,                       /* 41D: inc a */
    0x90,                       /* 41E: movx @r0,a */
    0x84, 0x14,                 /* 41F: jmp 414 */
};
static const uint8_t game_loop[] = {
    0xB8, RAM_COUNTER,          /* 414: mov r0,#10h */
    0x80,                       /* 416: movx a,@r0 */
    0x17,                       /* 417: inc a */
    0x90,                       /* 418: movx @r0,a */
    0x84, 0x14,                 /* 419: jmp 414 */
};
static const uint8_t vblank_isr[] = {
    0xD5,                       /* 430: sel rb1 */
    0xAF,                       /*      mov r7,a */
    0x09,                       /*      in a,p1 */
    0xAE,                       /*      mov r6,a */
    0x53, 0xB7,                 /*      anl a,#B7h (P13 and P16 low: the VDC's reads on) */
    0x39,                       /*      outl p1,a */
    0xB8, 0xA1,                 /*      mov r0,#A1h */
    0x80,                       /*      movx a,@r0 (the status: INT lets go) */
    0xFE,                       /*      mov a,r6 */
    0x39,                       /*      outl p1,a */
    0xFF,                       /*      mov a,r7 */
    0x93,                       /*      retr */
};

static void build_cart(uint8_t *img, int app)
{
    memset(img, 0xFF, 2048);
    memcpy(img, prologue, sizeof prologue);
    memcpy(img + 0x30, vblank_isr, sizeof vblank_isr);
    if (app) {
        memcpy(img + sizeof prologue, app_loop, sizeof app_loop);
        memcpy(img + 0x72C, "FUJI", 4);
    } else {
        memcpy(img + sizeof prologue, game_loop, sizeof game_loop);
        memset(img + 0x72C, 0x00, 4);
    }
}

/* ---- CRC-32, and a tail that steers it ---------------------------------------------- */

static uint32_t T[256];
static uint8_t inv[256];

static void crc_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        T[i] = c;
        inv[c >> 24] = (uint8_t)i;
    }
}

/* Set the last four bytes of buf so its CRC-32 is `want`. */
static void forge_tail(uint8_t *buf, uint32_t size, uint32_t want)
{
    uint32_t r = 0xffffffffu, v = ~want;
    uint8_t idx[4];

    for (uint32_t i = 0; i < size - 4; i++)
        r = (r >> 8) ^ T[(r ^ buf[i]) & 0xff];
    idx[3] = inv[v >> 24];          v ^= T[idx[3]];
    idx[2] = inv[(v >> 16) & 0xff]; v ^= T[idx[2]] >> 8;
    idx[1] = inv[(v >> 8) & 0xff];  v ^= T[idx[1]] >> 16;
    idx[0] = inv[v & 0xff];
    for (int k = 0; k < 4; k++) {
        buf[size - 4 + k] = (uint8_t)(idx[k] ^ (r & 0xff));
        r = (r >> 8) ^ T[idx[k]];
    }
}

static int write_file(const char *path, const uint8_t *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

/* ---- watching the machine ------------------------------------------------------------ */

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

/* The programs keep their state in the external RAM; peek at it through the
 * debugger (which stops the machine while attached) and let it run on. */
static uint8_t ram(o2session *s, uint16_t addr)
{
    o2debug *d = o2session_debugger(s);
    uint8_t v = 0;
    o2debug_attach(d);
    o2debug_read(d, O2_SPACE_XDATA, addr, &v, 1);
    o2debug_detach(d);
    return v;
}

static uint8_t prog(o2session *s, uint16_t addr)
{
    o2debug *d = o2session_debugger(s);
    uint8_t v = 0;
    o2debug_attach(d);
    o2debug_read(d, O2_SPACE_PROG, addr, &v, 1);
    o2debug_detach(d);
    return v;
}

/* the counter changes between samples: it wraps in microseconds, so one
 * coincidence is not "stopped" */
static int moving(o2session *s)
{
    uint8_t a = ram(s, RAM_COUNTER);
    for (int i = 0; i < 4; i++) {
        sleep_ms(60);
        if (ram(s, RAM_COUNTER) != a) return 1;
    }
    return 0;
}

static int wait_ram(o2session *s, uint16_t addr, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (ram(s, addr) == want) return 1;
        sleep_ms(30); waited += 30;
    }
    return 0;
}

int main(void)
{
    char cfg[300], data[400], app[500], game[500], big[500], bios[500], font[500], chess[500], sid[500],
         launch[400], msg[512], why[256];
    uint8_t img[2048], rom[1024], fnt[512], blob[70000];
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    uint64_t serial = 0;
    int i;

    crc_tables();
    test_tmpdir(cfg, sizeof cfg, "cfg");
    snprintf(data, sizeof data, "%s/data", cfg);
    if (!test_getcwd(launch, sizeof launch))
        return 1;
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data;
    p.fujinet_lib = "";     /* no runtime: the cart runs link-down */

    snprintf(app, sizeof app, "%s/counter.bin", cfg);
    build_cart(img, 1);
    check(write_file(app, img, sizeof img), "wrote a FujiNet app");
    snprintf(game, sizeof game, "%s/game.bin", cfg);
    build_cart(img, 0);
    check(write_file(game, img, sizeof img), "wrote a game");
    snprintf(big, sizeof big, "%s/huge.bin", cfg);
    memset(blob, 0x55, sizeof blob);
    check(write_file(big, blob, sizeof blob), "wrote a 70,000-byte image");
    snprintf(chess, sizeof chess, "%s/chess.bin", cfg);
    memset(blob, 0x33, 2048);
    forge_tail(blob, 2048, 0x77066338u);
    check(write_file(chess, blob, 2048), "wrote the Chess Module's CRC");
    snprintf(sid, sizeof sid, "%s/sid.bin", cfg);
    build_cart(img, 0);
    forge_tail(img, 2048, 0x1b9f2f16u);
    check(write_file(sid, img, 2048), "wrote Sid the Spellbinder's CRC");
    /* a BIOS of the tests' own: the console's reset vectors straight into the
     * cartridge, NOPs, and four bytes that make its CRC-32 MAME's */
    snprintf(bios, sizeof bios, "%s/testbios.bin", cfg);
    memset(rom, 0x00, sizeof rom);
    memcpy(rom + 0x000, "\x84\x00", 2);
    memcpy(rom + 0x003, "\x84\x02", 2);
    memcpy(rom + 0x007, "\x84\x04", 2);
    forge_tail(rom, sizeof rom, 0x8016a315u);
    check(write_file(bios, rom, sizeof rom), "wrote a BIOS of the tests' own");
    /* and a character set: noise that CRCs as the 8244's */
    snprintf(font, sizeof font, "%s/font.bin", cfg);
    for (i = 0; i < 512; i++) fnt[i] = (uint8_t)(i * 37 + (i >> 3));
    forge_tail(fnt, sizeof fnt, 0xb46a3f31u);
    check(write_file(font, fnt, sizeof fnt), "wrote a character set");

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
    check(o.machine == O2_MACHINE_ODYSSEY2 && o.use_bios == 1 && o.voice == 0, "the Odyssey 2, its BIOS, no Voice");
    check(o.cart_path == NULL, "no remembered cartridge: CONFIG");
    check(!o2session_have_bios(s, O2_MACHINE_ODYSSEY2), "no BIOS imported: the stand-in");

    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    check(o2session_start(s, &o) == 0, "session starts (CONFIG, no FujiNet)");
    if (!o2session_is_running(s)) { printf("error: %s\n", o2session_last_error(s)); return 1; }

    check(wait_frames(s, &serial, 30, 5000), "frames arrive");
    check(wait_status(s, "link down", 5000), "CONFIG runs link-down");
    check(!o2session_cart_booted_game(s), "CONFIG is not a booted game");
    check(!o2session_needs_bios(s), "and needs no BIOS");
    check(o2session_missing(s) == O2_MISSING_FONT, "but the character set is missing, and only that");
    {
        char name[96];
        o2session_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "FujiNet CONFIG") == 0, "the cartridge is the FujiNet CONFIG");
    }
    check(o2session_refresh_rate(s) == 60, "60 Hz");
    check(!o2session_is_paused(s), "not paused");

    /* Import BIOS: the character set, then a power cycle brings it in */
    check(o2session_rom_import(s, font, msg, sizeof msg) == 1, "the character set imports");
    check(o2session_missing(s) == O2_MISSING_FONT, "the running console still lacks it until a power cycle");
    check(o2session_reset_to_config(s) == 0, "reset to CONFIG");
    check(o2session_missing(s) == 0, "now nothing is missing");
    serial = 0;
    check(wait_frames(s, &serial, 10, 5000), "frames flow from the new console");
    {
        uint64_t z = 0; int lit = 0, tries;
        check(o2session_copy_frame(s, px, &z) == 1, "a forced copy (serial 0) always copies");
        /* up to 15 s: a loaded CI runner can take a while to the first paint */
        for (tries = 0; tries < 150; tries++) {
            lit = 0;
            for (i = 0; i < O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT; i++)
                lit += (px[i] & 0xffffff) != 0;
            if (lit > 50) break;
            sleep_ms(100);
            z = 0;
            o2session_copy_frame(s, px, &z);
        }
        check(lit > 50, "the CONFIG client painted something");
    }
    check(wait_status(s, "link down", 5000), "CONFIG is back");

    /* pause and resume */
    o2session_set_paused(s, 1);
    sleep_ms(100);
    {
        uint64_t f0 = o2session_frame_count(s);
        sleep_ms(250);
        check(o2session_is_paused(s) && o2session_frame_count(s) - f0 <= 1, "paused: the frames stop");
    }
    o2session_set_paused(s, 0);
    serial = 0;
    check(!o2session_is_paused(s) && wait_frames(s, &serial, 10, 3000), "resumed: they run again");

    /* keyboard: the default map */
    check(o2session_key(s, 'q', 1) == 1, "Q is bound (matrix 24)");
    check(o2session_key_held(s, 24), "and holds it");
    check(o2session_key(s, 'q', 0) == 1 && !o2session_key_held(s, 24), "and releases it");
    check(o2session_key(s, O2_KEYSYM_UP, 1) == 1 && (o2session_buttons_held(s, 0) & (1u << O2_ACT_UP)),
          "the up arrow is stick 1 up");
    check(o2session_key(s, O2_KEYSYM_RCTRL, 1) == 1 && (o2session_buttons_held(s, 0) & (1u << O2_ACT_FIRE)),
          "Right Ctrl is its button");
    o2session_key(s, O2_KEYSYM_DOWN, 1);
    check((o2session_buttons_held(s, 0) & ((1u << O2_ACT_UP) | (1u << O2_ACT_DOWN))) == 0,
          "up and down together: the stick holds neither");
    o2session_key(s, O2_KEYSYM_DOWN, 0);
    check((o2session_buttons_held(s, 0) & (1u << O2_ACT_UP)) != 0, "up again once down lets go");
    o2session_release_all(s);
    check(o2session_buttons_held(s, 0) == 0, "release_all lets go of the keyboard");
    check(o2session_key(s, O2_KEYSYM_KP_6, 1) == 1 && (o2session_buttons_held(s, 1) & (1u << O2_ACT_RIGHT)),
          "the keypad's 6 is stick 2 right");
    o2session_key(s, O2_KEYSYM_KP_6, 0);
    check(o2session_buttons_held(s, 1) == 0, "released");
    check(o2session_key(s, O2_KEYSYM_F11, 1) == 0, "an unbound key is ignored");
    check(o2session_key_sysaction(s, O2_KEYSYM_ESCAPE) == O2_SYSACT_RESET_CONFIG,
          "Escape is the Reset to CONFIG system action");
    check(o2session_target_for_key(s, O2_KEYSYM_F1) == O2_TARGET_SWITCH(O2_SW_RESET), "F1 is the console's RESET");
    check(o2session_key(s, O2_KEYSYM_F1, 1) == 1 && o2session_switch_held(s, O2_SW_RESET), "holding it holds RESET");
    o2session_key(s, O2_KEYSYM_F1, 0);
    check(!o2session_switch_held(s, O2_SW_RESET), "and letting go lets go");
    o2session_press(s, O2_TARGET_KEY(12), 1);
    check(o2session_key_held(s, 12), "the controllers window can press a key directly");
    o2session_press(s, O2_TARGET_KEY(12), 0);
    check(wait_frames(s, &serial, 10, 3000), "the machine keeps running after input");

    /* images the console cannot take */
    check(o2session_check_cart(big, why, sizeof why) == 0, "a 70,000-byte image is refused");
    check(strstr(why, "larger") != NULL, "with the reason");
    check(o2session_load_cart(s, big) == -1, "load_cart refuses it");
    check(!o2session_cart_booted_game(s) && o2session_cart_path(s)[0] == '\0', "and CONFIG keeps running");
    check(o2session_load_cart(s, chess) == -1 && strstr(o2session_last_error(s), "Chess Module") != NULL,
          "the Chess Module is refused, with MAME's reason");

    /* a game, with no BIOS */
    check(o2session_check_cart(game, NULL, 0) == 1 && !o2session_cart_is_app(game),
          "the game plans, and is not an app");
    check(o2session_load_cart(s, game) == -1, "load_cart refuses a game with no BIOS");
    check(strstr(o2session_last_error(s), "needs the BIOS") != NULL, "and says to import one");
    check(o2session_cart_path(s)[0] == '\0' && wait_status(s, "link down", 3000), "CONFIG is untouched");

    /* open a FujiNet app: served from power-on, the mailbox live */
    check(o2session_cart_is_app(app), "the app is an app");
    check(o2session_load_cart(s, app) == 0, "load_cart");
    check(strcmp(o2session_cart_path(s), app) == 0, "the cart path is remembered");
    check(strcmp(o2session_get_str(s, "cart", ""), app) == 0, "and persisted");
    check(wait_booted_game(s, 1, 5000), "something other than CONFIG is in the cartridge");
    check(moving(s), "the app runs (its counter moves)");
    check(wait_ram(s, RAM_BOOTS, 1, 3000), "it has started once");
    {
        char name[96];
        o2session_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "FujiNet app") == 0, "the cartridge is a FujiNet app");
    }

    /* the keyboard reaches the program through the stand-in's readkey */
    check(wait_ram(s, RAM_KEY, 0xFF, 3000), "no key reads $FF");
    o2session_key(s, 'q', 1);
    check(wait_ram(s, RAM_KEY, 0xC0 + 24, 3000), "Q held reads $C0 + 24");
    o2session_key(s, 'q', 0);
    check(wait_ram(s, RAM_KEY, 0xFF, 3000), "and $FF once it is let go");

    /* RESET: the 8048 starts over, the app is still open */
    {
        uint8_t boots = ram(s, RAM_BOOTS);
        check(o2session_reset(s) == 0, "RESET");
        check(wait_ram(s, RAM_BOOTS, boots + 1, 3000), "the app started over");
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
    check(o2session_media_is_rom(bios), "the BIOS is recognised by size and CRC");
    check(o2session_rom_import(s, bios, msg, sizeof msg) == 1, "Import BIOS takes the tests' own");
    check(o2session_have_bios(s, O2_MACHINE_ODYSSEY2), "the console has a BIOS now");
    check(prog(s, 0x0B0) == 0x15, "(the stand-in is still what is running: readkey's DIS I)");
    check(o2session_load_cart(s, game) == 0, "the game opens");
    check(prog(s, 0x0B0) == 0x00, "the console powered up with the imported BIOS");
    check(moving(s), "and the game runs");
    check(!o2session_needs_bios(s) && o2session_missing(s) == 0, "nothing is missing");
    check(wait_status(s, "game running", 3000), "the cartridge reports a game (mailbox closed)");
    check(o2session_cart_booted_game(s), "a game is a booted cartridge");
    {
        char name[96];
        o2session_cart_type_name(s, name, sizeof name);
        check(strcmp(name, "Standard, 2 KB") == 0, "the cartridge is a standard 2 KB one");
    }
    check(o2session_reset(s) == 0 && moving(s), "RESET on a game: it runs again");

    /* "use the BIOS" off: the stand-in again, and the game refused */
    o2session_set_int(s, "use_bios", 0);
    check(o2session_reset_to_config(s) == 0, "use_bios off, reset to CONFIG");
    check(prog(s, 0x0B0) == 0x15, "the stand-in boots");
    check(wait_status(s, "link down", 5000), "CONFIG runs");
    check(o2session_load_cart(s, game) == -1 && strstr(o2session_last_error(s), "switched off") != NULL,
          "and a game is refused again, with the reason");
    o2session_set_int(s, "use_bios", 1);

    /* Sid the Spellbinder wants The Voice and its ROMs */
    check(o2session_load_cart(s, sid) == -1 && strstr(o2session_last_error(s), "The Voice") != NULL,
          "a game that speaks is refused without the Voice's ROMs");

    /* A path relative to where the app started still means that file after
     * the process's working directory moves: the in-process FujiNet makes
     * its runtime root the working directory. */
    {
        char rel[64], abs[900];
        snprintf(rel, sizeof rel, "o2-rel-%ld.bin", (long)test_getpid());
        snprintf(abs, sizeof abs, "%s/%s", launch, rel);
        build_cart(img, 1);
        check(write_file(abs, img, sizeof img), "wrote an image in the launch directory");
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
        check(o2session_have_bios(s, O2_MACHINE_ODYSSEY2) && o2session_have_font(s, O2_MACHINE_ODYSSEY2),
              "and so are the imported ROMs");
        o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
        check(o2session_start(s, &o) == 0, "restart");
        check(moving(s), "and it boots the remembered cartridge");

        /* the Videopac G7000 is PAL, and the same BIOS */
        o2session_set_int(s, "machine", O2_MACHINE_VIDEOPAC);
        check(o2session_restart(s) == 0, "the Videopac G7000: a power cycle");
        check(o2session_refresh_rate(s) == 50, "50 Hz");
        serial = 0;
        check(wait_frames(s, &serial, 10, 5000) && moving(s), "it runs the same cartridge");
        o2session_set_int(s, "machine", O2_MACHINE_ODYSSEY2);
        check(o2session_restart(s) == 0 && o2session_refresh_rate(s) == 60, "and back to the Odyssey 2");

        /* the Voice, switched on with no ROMs: the console runs, and says what is missing */
        o2session_set_int(s, "voice", 1);
        check(o2session_restart(s) == 0 && (o2session_missing(s) & O2_MISSING_VOICE) != 0,
              "the Voice with no speech ROMs is reported missing");
        o2session_set_int(s, "voice", 0);
        check(o2session_restart(s) == 0 && (o2session_missing(s) & O2_MISSING_VOICE) == 0, "and not once it is off");
        o2session_stop(s);
        o2session_free(s);
    }

    /* The Videopac+ G7400 cannot run on the stand-in: no BIOS, no start */
    {
        char cfg2[300], data2[400];
        o2session_paths p2;
        o2session_start_opts o2;
        o2session *g;

        test_tmpdir(cfg2, sizeof cfg2, "g7400");
        snprintf(data2, sizeof data2, "%s/data", cfg2);
        memset(&p2, 0, sizeof p2);
        p2.config_dir = cfg2; p2.data_dir = data2; p2.fujinet_lib = "";
        g = o2session_new(&p2);
        if (g) {
            o2session_default_opts(g, &o2);
            o2.machine = O2_MACHINE_VIDEOPAC_PLUS;
            o2.enable_fujinet = 0; o2.enable_audio = 0; o2.enable_gamepad = 0;
            check(o2session_start(g, &o2) == -1 && !o2session_is_running(g), "a G7400 with no BIOS does not start");
            check(strstr(o2session_last_error(g), "G7400 needs its BIOS") != NULL
                  && strstr(o2session_last_error(g), "Import BIOS") != NULL, "and says why");
            o2session_free(g);
        }
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

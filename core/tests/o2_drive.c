/*
 * o2_drive -- drive the whole app headless, like a user: a session with
 * the in-process FujiNet, a script of presses and waits, and screenshots.
 * For exploring CONFIG and checking a flow end to end; built, not
 * registered.
 *
 *   o2_drive [--cart image] [--bios file] [--import-sd file]... SCRIPT
 *
 * SCRIPT is a sequence of words:
 *   wait:MS            let the machine run
 *   press:TARGET[:MS]  hold a control (default 100 ms), then release it;
 *                      TARGET is a0-a9 / b0-b9 (keypad A / B), clear, or
 *                      config (Reset to CONFIG)
 *   shot:NAME          write the current frame to NAME.ppm
 *   status             print the cartridge's status
 *   until:TEXT:MS      wait up to MS for the status to contain TEXT
 *   text               print the cart's text engine screen
 *
 * Settings and data go in a fresh tree under $TMPDIR; --bios imports a
 * BIOS into it (never the user's own ROM directory). The FujiNet runtime
 * takes stdout and stderr for its Console Log and changes directory, so
 * this prints to its own copy of stdout and resolves NAME against the
 * directory it started in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "o2session.h"
#include "o2debug.h"
#include "test_tmpdir.h"

static FILE *out;               /* stdout, before the runtime took it */
static char cwd[1024];

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* a0-a9, b0-b9, clear; config is a system action (-2) */
static int target_of(const char *name)
{
    if ((name[0] == 'a' || name[0] == 'b') && name[1] >= '0' && name[1] <= '9' && !name[2])
        return O2_TARGET_PORT(name[0] - 'a', O2_ACT_0 + (name[1] - '0'));
    if (!strcmp(name, "clear"))
        return O2_TARGET_SWITCH(O2_SW_CLEAR);
    if (!strcmp(name, "config"))
        return -2;
    return -1;
}

static void shot(o2session *s, const char *name)
{
    static uint32_t px[O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT];
    uint64_t serial = 0;
    char path[512];
    FILE *f;

    o2session_copy_frame(s, px, &serial);
    if (name[0] == '/')
        snprintf(path, sizeof path, "%s.ppm", name);
    else
        snprintf(path, sizeof path, "%s/%s.ppm", cwd, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", O2SESSION_FB_WIDTH, O2SESSION_FB_HEIGHT);
    for (int i = 0; i < O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT; i++)
    {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(out, "shot %s\n", path);
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], st[160], dest[1200], msg[256];
    const char *cart = NULL, *bios = NULL;
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    int i;

    if (argc < 2)
    {
        fprintf(stderr, "usage: o2_drive [--cart image] [--bios file] [--import-sd file]... SCRIPT\n");
        return 2;
    }
    out = fdopen(dup(STDOUT_FILENO), "w");
    if (!out || !getcwd(cwd, sizeof cwd))
        return 1;
    setvbuf(out, NULL, _IOLBF, 0);
    test_tmpdir(cfg, sizeof cfg, "drcfg");
    test_tmpdir(data, sizeof data, "drdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    s = o2session_new(&p);
    if (!s)
        return 1;

    for (i = 1; i < argc - 1; i++)
    {
        if (!strcmp(argv[i], "--cart"))
            cart = argv[++i];
        else if (!strcmp(argv[i], "--bios"))
            bios = argv[++i];
        else if (!strcmp(argv[i], "--import-sd"))
            i++;   /* after start: the SD tree exists once FujiNet is up */
        else
            break;
    }
    if (bios)
    {
        if (o2session_import_bios(s, bios, msg, sizeof msg) != 0)
            fprintf(out, "bios: %s\n", msg);
        else
            fprintf(out, "bios: %s\n", msg);
    }

    o2session_default_opts(s, &o);
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.cart_path = cart;
    if (o2session_start(s, &o) != 0)
    {
        fprintf(out, "start: %s\n", o2session_last_error(s));
        return 1;
    }
    for (int k = 1; k < argc - 1; k++)
        if (!strcmp(argv[k], "--import-sd"))
        {
            if (o2session_import_cart_to_sd(s, argv[k + 1], dest, sizeof dest) == 0)
                fprintf(out, "imported %s\n", dest);
            else
                fprintf(out, "import failed: %s\n", o2session_last_error(s));
            k++;
        }

    {
        char *script = strdup(argv[argc - 1]);
        for (char *w = strtok(script, " \n"); w; w = strtok(NULL, " \n"))
        {
            if (!strncmp(w, "wait:", 5))
                sleep_ms(atoi(w + 5));
            else if (!strncmp(w, "press:", 6))
            {
                char name[32];
                int ms = 100, t;
                const char *c = strchr(w + 6, ':');
                snprintf(name, sizeof name, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                if (c)
                    ms = atoi(c + 1);
                t = target_of(name);
                if (t == -2) { o2session_sysaction(s, O2_SYSACT_RESET_CONFIG); continue; }
                if (t < 0) { fprintf(out, "no target %s\n", name); continue; }
                o2session_press(s, t, 1);
                sleep_ms(ms);
                o2session_press(s, t, 0);
                sleep_ms(60);
            }
            else if (!strncmp(w, "shot:", 5))
                shot(s, w + 5);
            else if (!strcmp(w, "text"))
            {
                static char text[4096];
                o2debug_command(o2session_debugger(s), "text", text, sizeof text);
                fprintf(out, "%s", text);
            }
            else if (!strcmp(w, "status"))
            {
                o2session_cart_status(s, st, sizeof st);
                fprintf(out, "status: %s (booted %d)\n", st, o2session_cart_booted_game(s));
            }
            else if (!strncmp(w, "until:", 6))
            {
                char text[64];
                const char *c = strrchr(w + 6, ':');
                int ms = c ? atoi(c + 1) : 5000, waited = 0;
                snprintf(text, sizeof text, "%.*s", c ? (int)(c - (w + 6)) : (int)strlen(w + 6), w + 6);
                for (;;)
                {
                    o2session_cart_status(s, st, sizeof st);
                    if (strstr(st, text) || waited >= ms)
                        break;
                    sleep_ms(50);
                    waited += 50;
                }
                fprintf(out, "until %s: %s after %d ms\n", text, st, waited);
            }
        }
        free(script);
    }
    o2session_stop(s);
    o2session_free(s);
    return 0;
}

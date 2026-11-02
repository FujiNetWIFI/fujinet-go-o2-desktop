/* o2_headless.c -- the machine with no host thread: boots it, runs N frames,
 * writes what it made. Side B of the MAME A/B runs (tools/ab/), and the way to
 * look at the core without a window. Built, not registered.
 *
 *   o2_headless [-machine o2|g7000|g7400] [-bios F] [-font F] [-font-ef F]
 *               [-cart F]      an app/CONFIG image on the FujiNet cartridge
 *               [-game F]      a game: its own mapper cartridge, no FujiNet
 *               [-boip HOST:PORT]  dial a FujiNet (default: none, link down)
               [-voice ROM2K -voice-speech ROM16K [-voice-game ROM16K]]
               [-frames N] [-hash] [-ppm N FILE] [-raw FILE] [-vdclog FILE]
               [-wav FILE] [-gain G]
 *               [-key FRAME:ROW:COL:DOWN] [-joy FRAME:STICK:BITS]
 *
 * -hash prints "frame fnv64" for every frame (the A/B compares these with a
 * MAME run's); -ppm writes frame N as a PPM; -raw writes every frame as
 * 360x243 32-bit little-endian ARGB, in order (tools/ab/cart_ab.py compares it
 * with the same dump from MAME); -wav records the sound as mono
 * 16-bit at 48 kHz. With no -bios the stand-in runs. With no -cart/-game
 * CONFIG, the baked image, boots; its link is down (nothing is dialled).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2_internal.h"

static uint8_t *slurp(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "o2_headless: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "o2_headless: cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

static uint64_t fnv64(const uint32_t *px, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= px[i] & 0xffffff;
        h *= 1099511628211ull;
    }
    return h;
}

static void write_ppm(const char *path, const uint32_t *px)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", O2_FB_WIDTH, O2_FB_HEIGHT);
    for (i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* -vdclog: every external-space access with its time in pixel clocks, in the
 * format of tools/ab/vdc_log.lua, to diff against MAME's. */
static FILE *s_vdclog;

static void log_access(o2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    (void)user;
    fprintf(s_vdclog, "%c %llu %u %u %u\n", kind == O2_BUS_XW ? 'W' : 'R',
            (unsigned long long)o2_machine_now(m), (unsigned)addr, (unsigned)data, (unsigned)m->p1);
}

typedef struct { int frame, row, col, down; } key_ev;
typedef struct { int frame, stick, bits; } joy_ev;

int main(int argc, char **argv)
{
    static o2_machine_t m;
    o2_fujinet_t *fj;
    o2_cart_t mapper;
    o2_machine_opts_t mo;
    uint8_t *bios = NULL, *font = NULL, *font_ef = NULL, *cart = NULL, *game = NULL;
    uint8_t *vrom = NULL, *vspeech = NULL, *vgame = NULL;
    uint32_t bios_n = 0, font_n = 0, font_ef_n = 0, cart_n = 0, game_n = 0;
    uint32_t vrom_n = 0, vspeech_n = 0, vgame_n = 0;
    const char *ppm_path = NULL, *wav_path = NULL, *boip = "", *raw_path = NULL, *vdclog_path = NULL;
    FILE *raw = NULL;
    int ppm_frame = -1, frames = 60, hash = 0, machine = O2_MACHINE_ODYSSEY2;
    float gain = 1.0f;
    key_ev keys[256];
    joy_ev joys[256];
    int nkeys = 0, njoys = 0, i, f;
    FILE *wav = NULL;
    uint32_t wav_bytes = 0;
    char why[160];
    const char *err = NULL;
    uint64_t keymask = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-machine") && i + 1 < argc) {
            const char *v = argv[++i];
            machine = !strcmp(v, "g7000") ? O2_MACHINE_VIDEOPAC :
                      !strcmp(v, "g7400") ? O2_MACHINE_VIDEOPAC_PLUS : O2_MACHINE_ODYSSEY2;
        } else if (!strcmp(a, "-bios") && i + 1 < argc) bios = slurp(argv[++i], &bios_n);
        else if (!strcmp(a, "-font") && i + 1 < argc) font = slurp(argv[++i], &font_n);
        else if (!strcmp(a, "-font-ef") && i + 1 < argc) font_ef = slurp(argv[++i], &font_ef_n);
        else if (!strcmp(a, "-voice") && i + 1 < argc) vrom = slurp(argv[++i], &vrom_n);
        else if (!strcmp(a, "-voice-speech") && i + 1 < argc) vspeech = slurp(argv[++i], &vspeech_n);
        else if (!strcmp(a, "-voice-game") && i + 1 < argc) vgame = slurp(argv[++i], &vgame_n);
        else if (!strcmp(a, "-cart") && i + 1 < argc) cart = slurp(argv[++i], &cart_n);
        else if (!strcmp(a, "-game") && i + 1 < argc) game = slurp(argv[++i], &game_n);
        else if (!strcmp(a, "-frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "-hash")) hash = 1;
        else if (!strcmp(a, "-boip") && i + 1 < argc) boip = argv[++i];
        else if (!strcmp(a, "-gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(a, "-ppm") && i + 2 < argc) {
            ppm_frame = atoi(argv[++i]);
            ppm_path = argv[++i];
        } else if (!strcmp(a, "-wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "-raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(a, "-vdclog") && i + 1 < argc) vdclog_path = argv[++i];
        else if (!strcmp(a, "-key") && i + 1 < argc && nkeys < 256) {
            key_ev k;
            if (sscanf(argv[++i], "%d:%d:%d:%d", &k.frame, &k.row, &k.col, &k.down) == 4)
                keys[nkeys++] = k;
        } else if (!strcmp(a, "-joy") && i + 1 < argc && njoys < 256) {
            joy_ev j;
            if (sscanf(argv[++i], "%d:%d:%i", &j.frame, &j.stick, &j.bits) == 3)
                joys[njoys++] = j;
        } else {
            fprintf(stderr, "o2_headless: unknown option %s\n", a);
            return 2;
        }
    }

    memset(&mo, 0, sizeof mo);
    mo.machine = machine;
    mo.bios = bios && bios_n == O2_BIOS_SIZE ? bios : NULL;
    mo.font = font && font_n == 512 ? font : NULL;
    mo.font_ef = font_ef;
    if (vrom && vspeech && vrom_n == 2048 && vspeech_n == 16384) {
        mo.voice = true;
        mo.voice_rom = vrom;
        mo.voice_speech = vspeech;
        mo.voice_game = vgame && vgame_n == 16384 ? vgame : NULL;
    }

    fj = o2_fujinet_create();
    if (game) {
        if (o2_cart_init(&mapper, o2_cart_identify(game, game_n, NULL), game, game_n, &err) != 0) {
            fprintf(stderr, "o2_headless: %s\n", err);
            return 2;
        }
        mo.cart = &mapper;
    } else {
        if (o2_fujinet_power_on(fj, cart, cart_n, boip, true, why, sizeof why) != 0) {
            fprintf(stderr, "o2_headless: %s\n", why);
            return 2;
        }
        mo.cart = o2_fujinet_cart(fj);
        mo.fujinet = fj;
    }
    o2_machine_init(&m, &mo);
    m.audio_gain = gain;

    if (raw_path)
        raw = fopen(raw_path, "wb");
    if (vdclog_path) {
        s_vdclog = fopen(vdclog_path, "w");
        m.bus_hook = log_access;
        m.watch = O2_WATCH_XR | O2_WATCH_XW;
    }
    if (wav_path) {
        uint8_t hdr[44] = { 0 };
        wav = fopen(wav_path, "wb");
        if (wav)
            fwrite(hdr, 1, sizeof hdr, wav);
    }

    for (f = 0; f < frames; f++) {
        int k;

        for (k = 0; k < nkeys; k++) {
            if (keys[k].frame != f)
                continue;
            keymask = keys[k].down ? (m.keys_live | 1ull << (keys[k].row * 8 + keys[k].col))
                                   : (m.keys_live & ~(1ull << (keys[k].row * 8 + keys[k].col)));
            m.keys_live = keymask;
        }
        for (k = 0; k < njoys; k++)
            if (joys[k].frame == f)
                m.joy_live[joys[k].stick & 1] = (uint8_t)joys[k].bits;
        o2_machine_run_frame(&m);
        if (s_vdclog)
            fprintf(s_vdclog, "F %d %llu %u\n", f, (unsigned long long)o2_machine_now(&m), (unsigned)m.p1);
        if (hash)
            printf("%d %016llx\n", f, (unsigned long long)fnv64(m.fb, O2_FB_WIDTH * O2_FB_HEIGHT));
        if (raw) {
            static uint32_t argb[O2_FB_WIDTH * O2_FB_HEIGHT];
            int px;

            for (px = 0; px < O2_FB_WIDTH * O2_FB_HEIGHT; px++)
                argb[px] = 0xff000000u | (m.fb[px] & 0xffffffu);
            fwrite(argb, sizeof argb[0], O2_FB_WIDTH * O2_FB_HEIGHT, raw);
        }
        if (f == ppm_frame && ppm_path)
            write_ppm(ppm_path, m.fb);
        if (wav) {
            int16_t pcm[O2_AUDIO_MAX];
            int n;

            for (n = 0; n < m.out_frames; n++) {
                float s = m.out[2 * n];
                pcm[n] = (int16_t)(s > 1.0f ? 32767 : s < -1.0f ? -32767 : (int)(s * 32767.0f));
            }
            fwrite(pcm, 2, (size_t)m.out_frames, wav);
            wav_bytes += (uint32_t)m.out_frames * 2u;
        }
    }
    if (raw)
        fclose(raw);
    if (s_vdclog)
        fclose(s_vdclog);
    if (wav) {
        uint8_t h[44];
        uint32_t rate = O2_AUDIO_RATE, byterate = O2_AUDIO_RATE * 2u, riff = 36 + wav_bytes;

        memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
        h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
        memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4);
        h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4);
        memcpy(h + 40, &wav_bytes, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    fprintf(stderr, "o2_headless: %d frames, pc=%03x, %llu instructions\n", frames,
            (unsigned)m.cpu.pc, (unsigned long long)m.cpu.instructions);
    o2_fujinet_destroy(fj);
    return 0;
}

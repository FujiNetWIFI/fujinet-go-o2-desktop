/*
 * o2_headless -- boots a machine with no frontend and writes frames as
 * PPM: the "side B" of an A/B run against MAME (tools/ab/), and a quick look
 * at what the core draws. Built, not registered as a test.
 *
 *   o2_headless [options]
 *     -bios FILE      the 2K BIOS (or the 1K system ROM); default: the stand-in
 *     -cart FILE      an image served from power-on (DIRECT); default: CONFIG
 *     -nocart         no cartridge at all (the built-in games, with a BIOS)
 *     -frames N       frames to run (default 300)
 *     -every K        write every K-th frame (default: only the last)
 *     -out PREFIX     output prefix (default "frame"): PREFIX_NNNNN.ppm
 *     -boip HOST:PORT FujiNet's BoIP listener (default 127.0.0.1:11520)
 *     -mame           MAME's power-on (no CLEAR pulse: IE stays 0)
 *     -key F:P:K:D    at frame F press (D=1) / release (D=0) key K (0-9) of
 *                     keypad P (A or B); repeatable
 *     -clear F:D      at frame F press (1) / release (0) the Clear key
 *     -hash           print a CRC-32 of every frame's picture (A/B by hash)
 *     -dump FILE      append every frame's picture to FILE, a byte a pixel
 *                     (1 lit, 0 dark): tools/ab/mame_ab.py's side B
 *     -sync           run the mailbox inline (deterministic)
 *     -wav FILE       the audio, 48 kHz mono 16-bit (the left channel)
 *     -trace FILE     every bus read ("cycle addr data", hex) from frame
 *     -tracefrom F    F (default 0) to -traceto T (default 10): the other
 *     -traceto T      side of tools/ab/abtrace.lua
 *     -keyline L      apply a frame's input events at line L of it rather
 *                     than at its start (probing MAME's input timing)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2_internal.h"

typedef struct {
    int frame, pad, key, down;
} key_ev_t;

static uint8_t *load(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
    {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        fprintf(stderr, "%s: read error\n", path);
        exit(2);
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", O2_FB_WIDTH, O2_FB_HEIGHT);
    for (int i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++)
    {
        uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static FILE *s_trace;

static void trace_hook(o2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    (void)user;
    (void)kind;
    fprintf(s_trace, "%llu %04X %02X\n", (unsigned long long)m->cycles, addr, data);
}

static uint32_t crc32_fb(const uint32_t *fb)
{
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++)
    {
        uint8_t b = (fb[i] & 0xffffff) ? 1 : 0;
        crc ^= b;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

int main(int argc, char **argv)
{
    static o2_machine_t m;
    const char *bios_path = NULL, *cart_path = NULL, *prefix = "frame";
    const char *boip = "127.0.0.1:11520";
    const char *dump_path = NULL;
    FILE *dump = NULL;
    int frames = 300, every = 0, nocart = 0, mame = 0, hash = 0, sync = 0, keyline = 0;
    const char *trace_path = NULL;
    const char *wav_path = NULL;
    FILE *wav = NULL;
    uint32_t wav_samples = 0;
    int trace_from = 0, trace_to = 10;
    key_ev_t evs[256];
    int nev = 0;
    uint8_t *bios = NULL, *cart = NULL;
    uint32_t bios_len = 0, cart_len = 0;
    o2_cart_t *c = NULL;
    char why[200];

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-bios") && i + 1 < argc)
            bios_path = argv[++i];
        else if (!strcmp(argv[i], "-cart") && i + 1 < argc)
            cart_path = argv[++i];
        else if (!strcmp(argv[i], "-nocart"))
            nocart = 1;
        else if (!strcmp(argv[i], "-frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-every") && i + 1 < argc)
            every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-out") && i + 1 < argc)
            prefix = argv[++i];
        else if (!strcmp(argv[i], "-boip") && i + 1 < argc)
            boip = argv[++i];
        else if (!strcmp(argv[i], "-mame"))
            mame = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc)
            dump_path = argv[++i];
        else if (!strcmp(argv[i], "-hash"))
            hash = 1;
        else if (!strcmp(argv[i], "-sync"))
            sync = 1;
        else if (!strcmp(argv[i], "-wav") && i + 1 < argc)
            wav_path = argv[++i];
        else if (!strcmp(argv[i], "-trace") && i + 1 < argc)
            trace_path = argv[++i];
        else if (!strcmp(argv[i], "-tracefrom") && i + 1 < argc)
            trace_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-traceto") && i + 1 < argc)
            trace_to = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-keyline") && i + 1 < argc)
            keyline = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-key") && i + 1 < argc && nev < 256)
        {
            char p;
            key_ev_t *e = &evs[nev];
            if (sscanf(argv[++i], "%d:%c:%d:%d", &e->frame, &p, &e->key, &e->down) == 4)
            {
                e->pad = (p == 'B' || p == 'b') ? O2_KEYPAD_B : O2_KEYPAD_A;
                nev++;
            }
        }
        else if (!strcmp(argv[i], "-clear") && i + 1 < argc && nev < 256)
        {
            key_ev_t *e = &evs[nev];
            if (sscanf(argv[++i], "%d:%d", &e->frame, &e->down) == 2)
            {
                e->pad = -1;
                e->key = -1;
                nev++;
            }
        }
        else
        {
            fprintf(stderr, "usage: see the header of o2_headless.c\n");
            return 2;
        }
    }

    if (bios_path)
        bios = load(bios_path, &bios_len);
    if (cart_path)
        cart = load(cart_path, &cart_len);
    if (!nocart)
    {
        c = o2_cart_create();
        if (o2_cart_power_on(c, cart, cart_len, boip, sync, why, sizeof why) != 0)
        {
            fprintf(stderr, "cart: %s\n", why);
            return 1;
        }
    }
    o2_machine_init(&m, bios, bios_len, c, mame != 0);
    if (dump_path && !(dump = fopen(dump_path, "wb")))
    {
        perror(dump_path);
        return 2;
    }

    if (wav_path)
    {
        static const uint8_t hdr[44] = { 0 };
        if (!(wav = fopen(wav_path, "wb")))
        {
            perror(wav_path);
            return 2;
        }
        fwrite(hdr, 1, sizeof hdr, wav);     /* filled in at the end */
    }
    if (trace_path && !(s_trace = fopen(trace_path, "w")))
    {
        perror(trace_path);
        return 2;
    }
    for (int f = 0; f < frames; f++)
    {
        if (s_trace)
        {
            const bool on = f >= trace_from && f < trace_to;
            m.bus_hook = on ? trace_hook : NULL;
            m.watch = on ? (O2_WATCH_MEMR | O2_WATCH_DMA) : 0;
        }
        if (keyline > 0)
        {
            const uint64_t at = m.frame_end - O2_FRAME_CLOCKS
                              + (uint64_t)keyline * CDP1861_SCREEN_WIDTH;
            while (m.cycles < at)
                o2_machine_step(&m);
        }
        for (int i = 0; i < nev; i++)
        {
            if (evs[i].frame != f)
                continue;
            if (evs[i].pad < 0)
                m.clear_live = evs[i].down != 0;
            else if (evs[i].down)
                m.keys_live[evs[i].pad] |= (uint16_t)(1u << evs[i].key);
            else
                m.keys_live[evs[i].pad] &= (uint16_t)~(1u << evs[i].key);
        }
        o2_machine_latch_inputs(&m);
        o2_machine_run_frame(&m);
        if (c)
            o2_cart_frame(c);
        if (wav)
            for (int i = 0; i < m.out_frames; i++)
            {
                const int16_t v = (int16_t)(m.out[2 * i] * 32767.0f);
                fputc(v & 0xff, wav);
                fputc((v >> 8) & 0xff, wav);
                wav_samples++;
            }
        m.out_frames = 0;
        if (hash)
            printf("%05d %08x\n", f, crc32_fb(m.fb));
        if (dump)
            for (int i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++)
                fputc((m.fb[i] & 0xffffff) ? 1 : 0, dump);
        if ((every > 0 && (f + 1) % every == 0) || f == frames - 1)
        {
            char path[512];
            snprintf(path, sizeof path, "%s_%05d.ppm", prefix, f + 1);
            write_ppm(path, m.fb);
        }
    }
    if (dump)
        fclose(dump);
    if (wav)
    {
        /* RIFF/WAVE, PCM, mono, 48000 Hz, 16-bit */
        uint8_t h[44];
        const uint32_t data = wav_samples * 2, rate = O2_AUDIO_RATE, bps = rate * 2;
        memcpy(h, "RIFF", 4);
        h[4] = (uint8_t)(data + 36); h[5] = (uint8_t)((data + 36) >> 8);
        h[6] = (uint8_t)((data + 36) >> 16); h[7] = (uint8_t)((data + 36) >> 24);
        memcpy(h + 8, "WAVEfmt ", 8);
        h[16] = 16; h[17] = h[18] = h[19] = 0;
        h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
        h[24] = (uint8_t)rate; h[25] = (uint8_t)(rate >> 8); h[26] = (uint8_t)(rate >> 16); h[27] = 0;
        h[28] = (uint8_t)bps; h[29] = (uint8_t)(bps >> 8); h[30] = (uint8_t)(bps >> 16); h[31] = 0;
        h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
        memcpy(h + 36, "data", 4);
        h[40] = (uint8_t)data; h[41] = (uint8_t)(data >> 8); h[42] = (uint8_t)(data >> 16);
        h[43] = (uint8_t)(data >> 24);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, sizeof h, wav);
        fclose(wav);
    }
    if (s_trace)
        fclose(s_trace);
    if (c)
        o2_cart_destroy(c);
    free(bios);
    free(cart);
    return 0;
}

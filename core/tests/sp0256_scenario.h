/*
 * sp0256_scenario.h -- a deterministic, open-loop script for the SP0256,
 * written to compile as both C and C++ so that the SAME script can drive
 * (a) core/o2/sp0256.c in the test and (b) MAME's own sp0256.cpp, built
 * unmodified behind a stub of MAME's device classes, which is how the golden
 * hashes in sp0256_golden.h were produced (see sp0256_test.c).
 *
 * A scenario builds a 64 KB speech-ROM image from a seed (random bytes make
 * a fair microcode fuzz: they hit every opcode, mode and data format), then
 * runs the chip for a number of 10 kHz samples while firing address loads,
 * resets and FIFO writes at pseudo-random times and reading LRQ, standby and
 * the SPB640 status. Everything the chip does is folded into an FNV-1a hash:
 * every output sample and every pin read.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SP0256_SCENARIO_H
#define SP0256_SCENARIO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct sp_drv {
    void *ctx;
    /* generate n samples; write them to out[0..n-1] */
    void (*run)(void *ctx, uint32_t n, int16_t *out);
    void (*write_ald)(void *ctx, uint8_t v);
    void (*reset)(void *ctx);
    void (*spb_write)(void *ctx, unsigned off, uint16_t v);
    unsigned (*spb_read)(void *ctx, unsigned off);
    int (*lrq)(void *ctx);
    int (*sby)(void *ctx);
} sp_drv;

#define SCN_FNV_INIT 2166136261u

static inline uint32_t scn_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;

    while (n--)
    {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t scn_fnv_samples(uint32_t h, const int16_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        const uint16_t v = (uint16_t)s[i];

        h ^= (uint8_t)(v & 0xff);
        h *= 16777619u;
        h ^= (uint8_t)(v >> 8);
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t scn_rand(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

/* The byte pair that encodes `JMP target` (a byte address in the same 4K page
 * as the default page register, $1000-$1FFF): immed4 = the page-offset
 * bits 11..8 of the address bit-reversed, then the low 8 bits bit-reversed. */
static inline void scn_jmp_bytes(uint32_t target, uint8_t out[2])
{
    uint8_t hi = (uint8_t)((target >> 8) & 0xf), lo = (uint8_t)(target & 0xff);
    uint8_t rh = 0, rl = 0;

    for (int i = 0; i < 4; i++)
        if (hi & (1 << i)) rh |= (uint8_t)(1 << (3 - i));
    for (int i = 0; i < 8; i++)
        if (lo & (1 << i)) rl |= (uint8_t)(1 << (7 - i));
    out[0] = (uint8_t)(rh | (0xE << 4));   /* low nibble immed4, high nibble opcode */
    out[1] = rl;
}

/* Number of distinct ROM styles; seed % SCN_STYLES picks one. */
#define SCN_STYLES 4

/* Fill the 64 KB image for `seed`. Styles:
 *   0  random bytes everywhere
 *   1  random bytes, half of them zero (zero is RTS: lots of halts)
 *   2  random internal ROM and external speech ROM ($4000-$7FFF), rest zero
 *   3  as 2, with the allophone table made of JMPs into the internal ROM
 *      (what a real mask ROM looks like) */
static inline void scn_make_rom(uint32_t seed, uint8_t *rom)
{
    uint64_t rs = (0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xD1B54A32D192ED03ull)) | 1;
    const int style = (int)(seed % SCN_STYLES);

    memset(rom, 0, 0x10000);
    for (uint32_t a = 0; a < 0x10000; a++)
    {
        const uint32_t r = scn_rand(&rs);
        int fill = 0;

        switch (style)
        {
            case 0: fill = 1; break;
            case 1: fill = 1; break;
            default: fill = (a >= 0x1000 && a < 0x1800) || (a >= 0x4000 && a < 0x8000); break;
        }
        if (fill)
            rom[a] = (uint8_t)(style == 1 && (r & 0x100) ? 0 : r);
    }
    if (style == 3)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            const uint32_t r = scn_rand(&rs);

            if (r % 16 == 1)            /* into the SPB640 FIFO window */
                scn_jmp_bytes(0x1800, &rom[0x1000 + 2 * n]);
            else if (r % 4)
            {
                const uint32_t target = 0x1100 + (r >> 8) % 0x700;

                scn_jmp_bytes(target, &rom[0x1000 + 2 * n]);
            }
        }
    }
}

/* Run the script: `total` samples. Returns the hash. */
static inline uint32_t scn_run(const sp_drv *d, uint32_t seed, uint32_t total)
{
    uint64_t rs = (0xC0FFEE1234567ull ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ull)) | 1;
    uint32_t h = SCN_FNV_INIT, t = 0;
    int16_t buf[512];

    while (t < total)
    {
        uint32_t step = 1 + scn_rand(&rs) % 300;
        unsigned act;
        uint32_t v;

        if (step > total - t)
            step = total - t;
        d->run(d->ctx, step, buf);       /* step <= 300 <= 512 */
        h = scn_fnv_samples(h, buf, step);
        t += step;

        act = scn_rand(&rs) % 20;
        v = scn_rand(&rs);
        switch (act)
        {
            case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
                d->write_ald(d->ctx, (uint8_t)v);
                break;
            case 8:
                if (v % 8 == 0) d->reset(d->ctx);
                break;
            case 9: case 10:
                d->spb_write(d->ctx, 1, (uint16_t)(v & ((v & 0x10000) ? 0x7ff : 0x3ff)));
                break;
            case 11:
                d->spb_write(d->ctx, 0, (uint16_t)(v & 0xff));
                break;
            default:
                break;
        }

        {
            uint32_t probe[4];

            probe[0] = (uint32_t)d->lrq(d->ctx);
            probe[1] = (uint32_t)d->sby(d->ctx);
            probe[2] = d->spb_read(d->ctx, 0);
            probe[3] = d->spb_read(d->ctx, 1);
            h = scn_fnv(h, probe, sizeof probe);
        }
    }
    return h;
}


/* ---- hand-assembled microcode ----------------------------------------- *
 * Tiny programs for directed tests, in a ROM that needs no real data. The
 * allophone table at $1000 holds a JMP per entry; the programs sit above it.
 * Instructions are read LSB first: a 4-bit immediate, then a 4-bit opcode
 * (raw values: 0 RTS/SETPAGE, 1 SETMODE, 3 LOAD_C, 4 LOAD_2, 8 LOADALL,
 * 0xD JSR, 0xE JMP, 0xF PAUSE). */
typedef struct scn_bw { uint8_t *base; uint32_t bit; } scn_bw;

static inline void scn_put(scn_bw *w, uint32_t value, int nbits)
{
    for (int i = 0; i < nbits; i++, w->bit++)
        if ((value >> i) & 1)
            w->base[w->bit >> 3] = (uint8_t)(w->base[w->bit >> 3] | (1u << (w->bit & 7)));
}

static inline void scn_ins(scn_bw *w, unsigned opcode, unsigned immed4)
{
    scn_put(w, immed4, 4);
    scn_put(w, opcode, 4);
}

/* JMP/JSR to a byte address in the $1000 page (the default page register) */
static inline void scn_jump(scn_bw *w, unsigned opcode, uint32_t target)
{
    uint8_t b[2];

    scn_jmp_bytes(target, b);
    scn_put(w, b[0] & 0xf, 4);
    scn_put(w, opcode, 4);
    scn_put(w, b[1], 8);
}

/* a LOADALL (mode 00) frame: AM, PR, then B0 F0 .. B5 F5 */
static inline void scn_loadall(scn_bw *w, unsigned repeat, unsigned am, unsigned pr,
                               const uint8_t coef[12])
{
    scn_ins(w, 8, repeat);
    scn_put(w, am, 8);
    scn_put(w, pr, 8);
    for (int i = 0; i < 12; i++)
        scn_put(w, coef[i], 8);
}

/* Programs, by ALD value:
 *   0  PAUSE(3); HLT
 *   1  LOADALL x5 periods, amplitude 31, period 40, all coefficients 0;  HLT
 *      (a pulse train: one +124 sample every PR + 1 = 41 samples)
 *   2  as 1 but period 0: noise, +-124 from the LFSR, 2 repeats
 *   3  JSR to a subroutine that does PAUSE(3); RTS; then HLT  (same as 0)
 *   4  JMP $1800: run program 1's bits from the SPB640 FIFO instead
 * and the FIFO image for 4 (scn_synth_fifo). */
#define SCN_SYNTH_PROGS 5
#define SCN_SYNTH_PULSES 5
#define SCN_SYNTH_PERIOD 40
#define SCN_SYNTH_AMP 124

static inline void scn_synth_prog1(scn_bw *w, unsigned repeat, unsigned pr)
{
    static const uint8_t zero[12] = { 0 };

    scn_loadall(w, repeat, 0x1F, pr, zero);
    scn_ins(w, 0, 0);                       /* HLT */
}

static inline void scn_synth_rom(uint8_t *rom)
{
    scn_bw w;

    memset(rom, 0, 0x10000);
    w.base = rom; w.bit = 0x1100u * 8;      /* 0: PAUSE(3); HLT */
    scn_ins(&w, 0xF, 3);
    scn_ins(&w, 0, 0);
    w.bit = 0x1140u * 8;                    /* 1: pulse train */
    scn_synth_prog1(&w, SCN_SYNTH_PULSES, SCN_SYNTH_PERIOD);
    w.bit = 0x1180u * 8;                    /* 2: noise */
    scn_synth_prog1(&w, 2, 0);
    w.bit = 0x11C0u * 8;                    /* 3: JSR $1200; HLT; $1200: PAUSE(3); RTS */
    scn_jump(&w, 0xD, 0x1200);
    scn_ins(&w, 0, 0);
    w.bit = 0x1200u * 8;
    scn_ins(&w, 0xF, 3);
    scn_ins(&w, 0, 0);                      /* RTS (stack non-empty) */
    for (int n = 0; n < 4; n++)
        scn_jmp_bytes(0x1100u + 0x40u * (uint32_t)n, &rom[0x1000 + 2 * n]);
    scn_jmp_bytes(0x1800, &rom[0x1000 + 2 * 4]);    /* 4: into the FIFO */
}

/* program 1's bit stream as 10-bit decles for the SPB640 FIFO */
static inline unsigned scn_synth_fifo(uint16_t *decle, unsigned max)
{
    uint8_t buf[64];
    scn_bw w;
    unsigned n;

    memset(buf, 0, sizeof buf);
    w.base = buf; w.bit = 0;
    scn_synth_prog1(&w, SCN_SYNTH_PULSES, SCN_SYNTH_PERIOD);
    for (n = 0; n * 10 < w.bit + 10 && n < max; n++)
    {
        unsigned v = 0;

        for (unsigned i = 0; i < 10; i++)
        {
            const unsigned b = n * 10 + i;

            if ((buf[b >> 3] >> (b & 7)) & 1)
                v |= 1u << i;
        }
        decle[n] = (uint16_t)v;
    }
    return n;
}

/* Run synthetic program `prog` from a fresh reset: idle 5 samples, ALD,
 * `total` samples in all, pins read as in scn_run. capture (total entries)
 * receives the samples; *busy gets the number of samples after the ALD until
 * standby; *accept gets the samples until LRQ went high again. */
static inline uint32_t scn_synth_run(const sp_drv *d, int prog, uint32_t total,
                                     int16_t *capture, uint32_t *busy, uint32_t *accept)
{
    uint32_t h = SCN_FNV_INIT;
    int16_t one;

    *busy = 0; *accept = 0;
    d->reset(d->ctx);
    if (prog == 4)
    {
        uint16_t decle[40];
        unsigned n = scn_synth_fifo(decle, 40);

        for (unsigned i = 0; i < n; i++)
            d->spb_write(d->ctx, 1, decle[i]);
    }
    d->run(d->ctx, 5, capture);
    d->write_ald(d->ctx, (uint8_t)prog);
    for (uint32_t t = 5; t < total; t++)
    {
        d->run(d->ctx, 1, &one);
        capture[t] = one;
        if (!*accept && d->lrq(d->ctx))
            *accept = t - 4;
        if (!*busy && d->sby(d->ctx))
            *busy = t - 4;
    }
    h = scn_fnv_samples(h, capture, total);
    {
        uint32_t pr[3];

        pr[0] = *busy; pr[1] = *accept; pr[2] = d->spb_read(d->ctx, 1);
        h = scn_fnv(h, pr, sizeof pr);
    }
    return h;
}

#endif /* SP0256_SCENARIO_H */

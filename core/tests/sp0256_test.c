/*
 * sp0256_test -- The Voice's speech chip (core/o2/sp0256.c).
 *
 * Two ctest entries share this binary:
 *
 *   sp0256       needs no ROM. Fuzz scenarios (random microcode, random
 *                address loads / resets / FIFO traffic) and hand-assembled
 *                programs, every output sample and pin read hashed and
 *                compared with goldens made by MAME's own sp0256.cpp
 *                (sp0256_golden.h says how); the same scenarios split into
 *                odd chunks or driven by chip clocks must give the same
 *                hashes; the pulse-train / noise / FIFO / JSR programs are
 *                also checked against what they must do; the ALD / LRQ / SBY
 *                protocol, the Voice's address decode, the clock arithmetic,
 *                the ring, the debugger view and the 48 kHz resampler.
 *
 *   sp0256_rom   `sp0256_test rom`. Needs the Intellivoice's mask ROM
 *                (sp0256-012.bin, 2048 bytes, CRC 0DE7579D), which is never
 *                in the repository: SP0256_ROM=<file>, or a directory in
 *                SP0256_ROM_DIR / O2_ROM_DIR / MAME_ROMS holding it (or MAME's
 *                intv.zip / intv_voice.zip, unzipped with `unzip -p`), or
 *                ~/Workspace/mame/roms. Exits 77 (skip) without it. Plays
 *                "hello" and all 64 allophones and compares with the hashes
 *                of the samples the real MAME binary produced.
 *
 * The scripts are in sp0256_scenario.h (compiles as C and C++ so the same
 * script could drive MAME's class); the goldens in sp0256_golden.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp0256.h"
#include "sp0256_scenario.h"
#include "sp0256_golden.h"
#include "sp0256_romfind.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static void checkf(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void checkf(int ok, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    check(ok, buf);
}

/* ---- drivers for the scenario scripts ---------------------------------- */

typedef struct {
    sp0256_t *chip;
    uint32_t chunk;     /* split every run() into pieces of this many; 0 = whole */
    int by_clocks;      /* drive with sp0256_advance(n * 312) instead */
    sp0256_t *other;    /* a second chip run alongside, to catch shared state */
} dctx_t;

static void d_run(void *c, uint32_t n, int16_t *out)
{
    dctx_t *x = (dctx_t *)c;
    uint32_t done = 0;

    while (done < n)
    {
        uint32_t k = n - done;
        size_t got;

        if (x->chunk && k > x->chunk)
            k = x->chunk;
        if (x->by_clocks)
            sp0256_advance(x->chip, (uint64_t)k * SP0256_CLOCK_DIVIDER);
        else
            sp0256_run_samples(x->chip, k);
        got = sp0256_read_raw(x->chip, out + done, k);
        if (got != k)
        {
            printf("FAIL: raw ring returned %zu of %u\n", got, k);
            failures++;
            memset(out + done + got, 0, (k - got) * sizeof *out);
        }
        done += k;
    }
    if (x->other)
    {
        sp0256_run_samples(x->other, n);
        sp0256_write_alp(x->other, (uint8_t)n);
        if (n & 4)
            sp0256_reset(x->other);
    }
}
static void d_ald(void *c, uint8_t v) { sp0256_write_alp(((dctx_t *)c)->chip, v); }
static void d_reset(void *c) { sp0256_reset(((dctx_t *)c)->chip); }
static void d_spbw(void *c, unsigned o, uint16_t v) { sp0256_spb640_write(((dctx_t *)c)->chip, o, v); }
static unsigned d_spbr(void *c, unsigned o) { return sp0256_spb640_read(((dctx_t *)c)->chip, o); }
static int d_lrq(void *c) { return sp0256_lrq(((dctx_t *)c)->chip); }
static int d_sby(void *c) { return sp0256_sby(((dctx_t *)c)->chip); }

static sp_drv make_drv(dctx_t *x)
{
    sp_drv d;

    d.ctx = x; d.run = d_run; d.write_ald = d_ald; d.reset = d_reset;
    d.spb_write = d_spbw; d.spb_read = d_spbr; d.lrq = d_lrq; d.sby = d_sby;
    return d;
}

/* The chips are big (64 KB of ROM image): static, not on the stack. */
static sp0256_t chip_a, chip_b;
static uint8_t image[SP0256_ROM_SIZE], image2[SP0256_ROM_SIZE];

/* ---- scenarios ----------------------------------------------------------- */

static void test_scenarios(void)
{
    int bad_plain = 0, bad_chunk = 0, bad_clock = 0;
    uint32_t bad_seed = 0;

    for (uint32_t seed = 0; seed < SP0256_GOLDEN_SCN_N; seed++)
    {
        dctx_t x = { &chip_a, 0, 0, NULL };
        sp_drv d = make_drv(&x);
        uint32_t h;

        scn_make_rom(seed, image);
        sp0256_init(&chip_a, NULL, image, sizeof image);
        h = scn_run(&d, seed, SP0256_GOLDEN_SCN_SAMPLES);
        if (h != sp0256_golden_scn[seed])
        {
            if (!bad_plain) bad_seed = seed;
            bad_plain++;
        }

        /* the same script, every run() cut into 7-sample pieces, and again
         * driven by chip clocks: the chip must not care */
        x.chunk = 7;
        sp0256_init(&chip_a, NULL, image, sizeof image);
        if (scn_run(&d, seed, SP0256_GOLDEN_SCN_SAMPLES) != h) bad_chunk++;
        x.chunk = 0; x.by_clocks = 1;
        sp0256_init(&chip_a, NULL, image, sizeof image);
        if (scn_run(&d, seed, SP0256_GOLDEN_SCN_SAMPLES) != h) bad_clock++;
    }
    checkf(!bad_plain, "%d fuzz scenarios (%d styles of random microcode, %d samples each) match MAME's sp0256.cpp",
           SP0256_GOLDEN_SCN_N, SCN_STYLES, SP0256_GOLDEN_SCN_SAMPLES);
    if (bad_plain)
        printf("      (%d bad; first seed %u)\n", bad_plain, bad_seed);
    check(!bad_chunk, "the same scenarios in 7-sample pieces give the same samples and pin reads");
    check(!bad_clock, "the same scenarios driven by chip clocks (sp0256_advance) give the same samples");

    /* a second chip, busy with other things between every step of the
     * script, does not disturb it (no shared state) */
    {
        const uint32_t seed = 22;
        dctx_t x = { &chip_a, 0, 0, &chip_b };
        sp_drv d = make_drv(&x);

        scn_make_rom(seed, image);
        scn_make_rom(5, image2);
        sp0256_init(&chip_a, NULL, image, sizeof image);
        sp0256_init(&chip_b, NULL, image2, sizeof image2);
        check(scn_run(&d, seed, SP0256_GOLDEN_SCN_SAMPLES) == sp0256_golden_scn[seed],
              "two instances coexist: a script is unaffected by another chip running beside it");
    }
}

/* ---- directed programs ----------------------------------------------------- */

static int16_t cap[3000];

static uint32_t run_synth(int prog, uint32_t *busy, uint32_t *accept)
{
    dctx_t x = { &chip_a, 0, 0, NULL };
    sp_drv d = make_drv(&x);

    scn_synth_rom(image);
    sp0256_init(&chip_a, NULL, image, sizeof image);
    return scn_synth_run(&d, prog, 3000, cap, busy, accept);
}

static void test_synth(void)
{
    static int16_t keep[SCN_SYNTH_PROGS][3000];
    uint32_t busy, accept;
    int ok_golden = 1;

    for (int p = 0; p < SCN_SYNTH_PROGS; p++)
    {
        const uint32_t h = run_synth(p, &busy, &accept);

        memcpy(keep[p], cap, sizeof cap);
        if (h != sp0256_golden_synth[p].hash || busy != sp0256_golden_synth[p].busy ||
            accept != sp0256_golden_synth[p].accept)
        {
            ok_golden = 0;
            printf("      program %d: hash %08x busy %u accept %u\n", p, h, busy, accept);
        }
    }
    check(ok_golden, "five hand-assembled programs: samples, standby time and LRQ time match MAME");

    /* program 1: LOADALL, 5 periods, amplitude 31, period register 40, no
     * coefficients: a pulse of 31*4 every PR + 1 samples, exactly 5 of them */
    {
        int n = 0, first = -1, last = -1, spacing_ok = 1;

        for (int i = 0; i < 3000; i++)
            if (keep[1][i])
            {
                if (keep[1][i] != SCN_SYNTH_AMP) spacing_ok = 0;
                if (n && i - last != SCN_SYNTH_PERIOD + 1) spacing_ok = 0;
                if (first < 0) first = i;
                last = i;
                n++;
            }
        checkf(n == SCN_SYNTH_PULSES && spacing_ok,
               "LOADALL pulse train: %d pulses of %d every PR+1 = %d samples (MAME's period)",
               n, SCN_SYNTH_AMP, SCN_SYNTH_PERIOD + 1);
    }
    check(memcmp(keep[0], keep[3], sizeof keep[0]) == 0,
          "JSR to a subroutine ending in RTS plays exactly what the inline code does");
    check(memcmp(keep[1], keep[4], sizeof keep[1]) == 0,
          "microcode fed through the SPB640 FIFO (JMP $1800) plays exactly what ROM does");
    {
        int quiet = 1;

        for (int i = 0; i < 3000; i++)
            if (keep[0][i]) quiet = 0;
        check(quiet, "PAUSE is silent");
    }

    /* program 2: noise. Samples are +-amp following the chip's 15-bit LFSR
     * (x^15 + x^14 + 1, Galois, seed 1): find the run of the model that
     * matches the first outputs, then require it to continue */
    {
        static uint8_t model[32767 + 700];
        uint32_t r = 1;
        int first = -1;

        for (int i = 0; i < 3000 && first < 0; i++)
            if (keep[2][i]) first = i;
        for (size_t i = 0; i < sizeof model; i++)
        {
            const uint32_t bit = r & 1;

            model[i] = (uint8_t)bit;
            r = (r >> 1) ^ (bit ? 0x4001u : 0u);
        }
        {
            int matches = 0, want = 100;

            for (size_t off = 0; off < 32767; off++)
            {
                int ok = 1;

                for (int i = 0; i < want && ok; i++)
                    ok = keep[2][first + i] == (model[off + (size_t)i] ? SCN_SYNTH_AMP : -SCN_SYNTH_AMP);
                matches += ok;
            }
            checkf(first >= 0 && matches == 1,
                   "noise (PR = 0): 100 samples of +-%d follow the LFSR (1 match in the 32767-bit sequence)",
                   SCN_SYNTH_AMP);
        }
    }
}

/* ---- protocol, pins, glue, debugger ---------------------------------------- */

static unsigned frames_seen;
static int last_repeat;
static void frame_hook(void *ctx, const sp0256_lpc12_t *f, int repeat)
{
    (void)ctx; (void)f;
    frames_seen++;
    last_repeat = repeat;
}

static void test_protocol(void)
{
    sp0256_t *c = &chip_a;
    sp0256_debug_t d;
    int16_t out[400];

    scn_synth_rom(image);
    frames_seen = 0;
    sp0256_init(c, NULL, image, sizeof image);

    check(sp0256_lrq(c) == 1 && sp0256_sby(c) == 1 && sp0256_o2_t0(c) == 0,
          "power-on: LRQ high, standby, T0 (= busy) low");
    sp0256_run_samples(c, 500);
    sp0256_read_raw(c, out, 400);
    check(sp0256_lrq(c) == 1 && sp0256_sby(c) == 1, "an idle chip stays ready");

    sp0256_set_frame_hook(c, frame_hook, NULL);
    sp0256_write_alp(c, 1);
    check(sp0256_lrq(c) == 0 && sp0256_sby(c) == 0 && sp0256_o2_t0(c) == 1,
          "an address load drops LRQ and standby at once (T0 high)");
    sp0256_debug_get(c, &d);
    check(d.pending == 1 && d.last_allophone == 1 && d.halted == 1,
          "debug view: allophone 1 latched, not yet started");
    sp0256_write_alp(c, 2);
    sp0256_debug_get(c, &d);
    check(d.alds_dropped == 1 && d.pending == 1 && d.last_allophone == 1,
          "an address load while busy is dropped (counted, latch unchanged)");

    /* the chip starts the command at its next microsequencer step (<= 64 samples) */
    {
        int t;

        for (t = 1; t <= 70 && !sp0256_lrq(c); t++)
            sp0256_run_samples(c, 1);
        checkf(t >= 2 && t <= 66, "LRQ is back after %d samples (the idle sequencer steps every 64)", t - 1);
    }
    sp0256_debug_get(c, &d);
    check(d.allophone == 1 && d.pending == -1 && d.halted == 0 && d.sby == 0 && d.fifo_sel == 0,
          "debug view: allophone 1 running, ROM fetch");
    check(d.rom_addr >= 0x1140 && d.rom_addr < 0x1180 && d.period == SCN_SYNTH_PERIOD && d.amp == 31,
          "debug view: PC inside program 1, period 40, amplitude 31");
    check(d.r[0] == 0x1F && d.r[1] == SCN_SYNTH_PERIOD && d.noise == 0 && d.rate_hz == 10000 && d.clock_hz == 3120000,
          "debug view: registers AM/PR, rate");
    checkf(frames_seen == 1 && last_repeat == SCN_SYNTH_PULSES,
           "frame hook: one LPC frame loaded, repeat %d", SCN_SYNTH_PULSES);
    sp0256_run_samples(c, 400);
    check(sp0256_sby(c) == 1 && sp0256_lrq(c) == 1, "standby again when the program halts");
    sp0256_debug_get(c, &d);
    check(d.allophone == -1 && d.halted == 1 && d.samples_made >= 900,
          "debug view: idle again, samples counted");

    /* reset mid-speech */
    sp0256_write_alp(c, 1);
    sp0256_run_samples(c, 100);
    sp0256_reset(c);
    check(sp0256_lrq(c) == 1 && sp0256_sby(c) == 1, "reset: ready and in standby");
    {
        static int16_t big[SP0256_RING_SIZE];
        int quiet = 1;

        sp0256_read_raw(c, big, SP0256_RING_SIZE);   /* drain history */
        sp0256_run_samples(c, 300);
        sp0256_read_raw(c, out, 300);
        for (int i = 0; i < 300; i++)
            if (out[i]) quiet = 0;
        check(quiet, "reset: the speech stops dead");
    }

    /* SPB640: FIFO full/empty status and contents */
    sp0256_init(c, NULL, image, sizeof image);
    check(sp0256_spb640_read(c, 0) == 0x8000 && sp0256_spb640_read(c, 1) == 0,
          "SPB640 status: LRQ set, FIFO not full");
    for (unsigned i = 0; i < 70; i++)
        sp0256_spb640_write(c, 1, (uint16_t)(0x100 + i));
    sp0256_debug_get(c, &d);
    check(sp0256_spb640_read(c, 1) == 0x8000 && d.fifo_count == 64 && d.fifo[0] == 0x100 && d.fifo[63] == 0x100 + 63,
          "SPB640: 64 decles hold, the 65th and later are dropped");
    sp0256_spb640_write(c, 1, 0x400);
    sp0256_debug_get(c, &d);
    check(sp0256_spb640_read(c, 1) == 0 && d.fifo_count == 0, "SPB640: bit 10 clears the FIFO and resets the chip");
    sp0256_spb640_write(c, 0, 0x1);
    check(sp0256_spb640_read(c, 0) == 0, "SPB640: offset 0 writes the address load");
}

static void test_o2_glue(void)
{
    sp0256_t *c = &chip_a;
    sp0256_debug_t d;

    sp0256_init(c, NULL, NULL, 0);
    /* P1.4 high: not for the chip; A7 low: not for the chip */
    sp0256_o2_io_write(c, 0x10, 0x85, 0x20);
    sp0256_o2_io_write(c, 0x00, 0x05, 0x20);
    check(sp0256_lrq(c) == 1, "Voice decode: ignores writes with P1.4 high or A7 low");

    /* reset (D5 low), release (D5 high: the 7474 swallows that write), then the address */
    sp0256_o2_io_write(c, 0x00, 0x85, 0x00);
    sp0256_o2_io_write(c, 0x00, 0x85, 0x20);
    check(sp0256_lrq(c) == 1, "Voice decode: the write that releases the reset latches no address");
    sp0256_o2_io_write(c, 0x00, 0x80 | 0x41, 0x20);
    sp0256_debug_get(c, &d);
    check(sp0256_lrq(c) == 0 && d.pending == 0x41 && sp0256_o2_t0(c) == 1,
          "Voice decode: next write latches A0-A6 as the allophone (T0 = busy)");
    sp0256_run_samples(c, 100);
    sp0256_o2_io_write(c, 0x00, 0x80 | 0x7F, 0x00);     /* D5 low: reset */
    check(sp0256_lrq(c) == 1 && sp0256_sby(c) == 1 && sp0256_o2_t0(c) == 0, "Voice decode: D5 low resets the chip");
}

static void test_clock(void)
{
    sp0256_t *c = &chip_a;
    int16_t buf[64];

    sp0256_init(c, NULL, NULL, 0);
    sp0256_advance(c, 311);
    check(sp0256_raw_available(c) == 0, "311 chip clocks make no sample");
    sp0256_advance(c, 1);
    check(sp0256_raw_available(c) == 1, "312 clocks make one");
    sp0256_sync_to(c, 311);
    sp0256_sync_to(c, 312);
    check(sp0256_raw_available(c) == 1, "sync_to to a time already passed does nothing");
    sp0256_sync_to(c, 312 * 11 + 5);
    check(sp0256_raw_available(c) == 11, "sync_to the absolute clock count");
    sp0256_read_raw(c, buf, 64);
    sp0256_advance(c, SP0256_O2_CLOCK);
    check(c->samples_made == 11 + 10000 && sp0256_raw_available(c) == 4096,
          "one second of 3.12 MHz is 10000 samples (the ring holds the newest 4096)");
    sp0256_init(c, NULL, NULL, 0);
    sp0256_set_clock(c, 3000000);
    sp0256_advance(c, 3000000);
    {
        sp0256_debug_t d;

        sp0256_debug_get(c, &d);
        check(d.samples_made == 9615 && d.rate_hz == 9615, "a 3.0 MHz clock: 9615 samples a second");
    }
    /* a million single-clock steps lose nothing */
    sp0256_init(c, NULL, NULL, 0);
    for (int i = 0; i < 624; i++)
        sp0256_advance(c, 1);
    check(c->clk_frac == 0 && c->samples_made == 2, "single-clock steps accumulate exactly");
}

static void test_ring(void)
{
    sp0256_t *c = &chip_a;
    static int16_t a[4096], b[4096];

    scn_synth_rom(image);
    /* noise program: never quiet. Run 20000 samples with no reader, then read */
    sp0256_init(c, NULL, image, sizeof image);
    sp0256_write_alp(c, 2);
    sp0256_run_samples(c, 20000);
    check(sp0256_raw_available(c) == 4096, "an unread ring holds the newest 4096 samples");
    sp0256_read_raw(c, a, 4096);
    /* the same run read in pieces: its last 4096 samples */
    {
        static int16_t all[20000];

        sp0256_init(c, NULL, image, sizeof image);
        sp0256_write_alp(c, 2);
        for (int i = 0; i < 20000; i += 1000)
        {
            sp0256_run_samples(c, 1000);
            sp0256_read_raw(c, all + i, 1000);
        }
        memcpy(b, all + 20000 - 4096, sizeof b);
    }
    check(memcmp(a, b, sizeof a) == 0, "ring overflow drops the oldest, keeps the newest, in order");
}

static double goertzel(const float *x, int n0, int n1, double hz, double fs)
{
    double re = 0, im = 0;

    for (int i = n0; i < n1; i++)
    {
        re += x[i] * cos(2 * 3.14159265358979323846 * hz * i / fs);
        im += x[i] * sin(2 * 3.14159265358979323846 * hz * i / fs);
    }
    return 2 * sqrt(re * re + im * im) / (n1 - n0);
}

static void put_raw(sp0256_t *c, int16_t s)
{
    c->ring[c->ring_head & (SP0256_RING_SIZE - 1)] = s;
    if (s) c->nz_last = (int64_t)c->ring_head;
    c->ring_head++;
}

static void test_resampler(void)
{
    sp0256_t *c = &chip_a;
    static float out[60000];
    size_t made;

    /* an impulse at input 100 (after the 24-sample pre-roll: index 124):
     * the 4.8x resampler puts its peak at output (124) * 24/5 = 595.2 ->
     * the output j sits at input j*5/24, so the peak is where j*5/24 == 124 */
    sp0256_init(c, NULL, NULL, 0);
    for (int i = 0; i < 100; i++) put_raw(c, 0);
    put_raw(c, 20000);
    for (int i = 0; i < 100; i++) put_raw(c, 0);
    made = sp0256_render_f32(c, out, 1000);
    {
        int peak = 0;

        for (int j = 0; j < 1000; j++)
            if (fabsf(out[j]) > fabsf(out[peak])) peak = j;
        /* output j is centred on input j * 5/24; input 124 is j = 595.2 */
        checkf(made == 1000 && peak == 595, "resampler: an impulse at input 124 peaks at output %d (expected 595)", peak);
    }
    {   /* an impulse exactly on an output sample: input 125 = 24*5 -> j = 600; kernel centre tap is 1.0 */
        sp0256_init(c, NULL, NULL, 0);
        for (int i = 0; i < 101; i++) put_raw(c, 0);
        put_raw(c, 20000);          /* ring index 24 + 101 = 125 */
        for (int i = 0; i < 100; i++) put_raw(c, 0);
        made = sp0256_render_f32(c, out, 1000);
        checkf(made == 1000 && fabsf(out[600] - 20000.0f / 32768.0f) < 1e-6f,
               "resampler: an impulse on an output sample position comes out unchanged (%.6f)", (double)out[600]);
        /* 24 outputs are 5 input samples: those land on the sinc's zeros */
        checkf(fabsf(out[600 - 24]) < 1e-6f && fabsf(out[600 + 24]) < 1e-6f && fabsf(out[600 + 48]) < 1e-6f,
               "resampler: outputs a whole number of input samples away from it are zero (the sinc's zeros)");
    }

    /* gain: a tone passes flat; its image at 10 kHz - f is rejected */
    {
        const double fs_out = 48000.0;
        const double tones[] = { 500, 1000, 2500, 3400 };

        for (size_t t = 0; t < sizeof tones / sizeof tones[0]; t++)
        {
            double g, img;

            sp0256_init(c, NULL, NULL, 0);
            for (int i = 0; i < 3000; i++)
                put_raw(c, (int16_t)lrint(10000 * sin(2 * 3.14159265358979323846 * tones[t] * i / 10000.0)));
            made = sp0256_render_f32(c, out, 14000);
            g = goertzel(out, 1500, 10000, tones[t], fs_out) * 32768.0 / 10000.0;
            img = goertzel(out, 1500, 10000, 10000 - tones[t], fs_out) * 32768.0 / 10000.0;
            checkf(made == 14000 && g > 0.99 && g < 1.01 && img < 0.01,
                   "resampler: %.0f Hz passes at gain %.3f, image at %.0f Hz is %.1f dB",
                   tones[t], g, 10000 - tones[t], 20 * log10(img + 1e-12));
        }
    }

    /* rational ratio: one second of input is exactly 48000 outputs */
    {
        size_t total = 0;

        sp0256_init(c, NULL, NULL, 0);
        for (int k = 0; k < 10; k++)         /* 0.1 s of chip, then 0.1 s of output */
        {
            sp0256_run_samples(c, 1000);
            total += sp0256_render_f32(c, out, 4800);
        }
        check(total == 48000 && c->rs_in == 10000 && c->rs_rem == 0,
              "resampler: 10000 input samples = 48000 outputs exactly (5/24, no drift)");
    }
    /* underrun: ask for more than the chip has made */
    sp0256_init(c, NULL, NULL, 0);
    sp0256_run_samples(c, 100);
    made = sp0256_render_f32(c, out, 1000);
    {
        int tail_zero = 1;

        for (size_t j = made; j < 1000; j++)
            if (out[j] != 0.0f) tail_zero = 0;
        check(made > 0 && made < 1000 && tail_zero,
              "resampler: an underrun zero-fills and does not run ahead of the chip");
    }
    {   /* ... and the next call, after the chip catches up, continues where it stopped */
        const uint64_t at = c->rs_in;

        sp0256_run_samples(c, 400);
        made = sp0256_render_f32(c, out, 100);
        check(made == 100 && c->rs_in >= at, "resampler: resumes after an underrun");
    }
    /* the chip never run for a long time: the resampler jumps to the newest data, no crash */
    sp0256_init(c, NULL, NULL, 0);
    sp0256_run_samples(c, 30000);
    made = sp0256_render_f32(c, out, 2000);
    check(made == 2000, "resampler: a reader 30000 samples behind the ring resyncs");
}

/* ---- Intellivoice ROM ----------------------------------------------------------- */

/* play address loads at the sample indices given, as the Lua script did; return the hash */
static uint32_t play_events(sp0256_t *c, const sp0256_golden_ald_t *ev, size_t nev, uint32_t total,
                            int16_t *keep, int16_t *first16)
{
    uint32_t h = SCN_FNV_INIT, t = 0;
    size_t next = 0;
    int16_t buf[1000];

    while (t < total)
    {
        uint32_t n = total - t > 1000 ? 1000 : total - t;

        if (next < nev && ev[next].at < t + n)
            n = ev[next].at > t ? ev[next].at - t : 0;
        if (n)
        {
            sp0256_run_samples(c, n);
            if (sp0256_read_raw(c, buf, n) != n)
            {
                failures++;
                return 0;
            }
            h = scn_fnv_samples(h, buf, n);
            if (keep && t < 2000)
                memcpy(keep + t, buf, (n < 2000 - t ? n : 2000 - t) * sizeof *buf);
            t += n;
        }
        while (next < nev && ev[next].at == t)
            sp0256_write_alp(c, ev[next++].value);
    }
    (void)first16;
    return h;
}

static int test_rom(void)
{
    uint8_t rom[2048];
    char how[1300];
    static int16_t head[2000];
    uint32_t h;

    if (!find_rom(rom, how, sizeof how))
    {
        printf("SKIP: the Intellivoice ROM (sp0256-012.bin, CRC %08X) was not found; set SP0256_ROM or "
               "SP0256_ROM_DIR (see the header)\n", SP0256_INTV_ROM_CRC);
        return 77;
    }
    printf("ROM: %s\n", how);
    check(crc32_of(rom, 2048) == SP0256_INTV_ROM_CRC, "ROM CRC32 0DE7579D");

    sp0256_init(&chip_a, rom, NULL, 0);
    h = play_events(&chip_a, sp0256_golden_hello, sizeof sp0256_golden_hello / sizeof sp0256_golden_hello[0],
                    SP0256_GOLDEN_HELLO_SAMPLES, head, NULL);
    checkf(h == SP0256_GOLDEN_HELLO_HASH, "\"hello\" (6 allophones, %d samples): hash %08x (MAME %08x)",
           SP0256_GOLDEN_HELLO_SAMPLES, h, (uint32_t)SP0256_GOLDEN_HELLO_HASH);
    check(memcmp(head + 1008, sp0256_golden_hello_first, sizeof sp0256_golden_hello_first) == 0,
          "\"hello\": the first 16 samples are the ones the real MAME wrote");
    {
        int lo = 0, hi = 0, nz = 0;

        for (int i = 0; i < 2000; i++)
        {
            if (head[i] < lo) lo = head[i];
            if (head[i] > hi) hi = head[i];
            nz += head[i] != 0;
        }
        check(nz > 700 && hi > 3000 && lo < -3000 && hi < 16384 && lo > -16384,
              "\"hello\": audible, within range");
    }

    sp0256_init(&chip_a, rom, NULL, 0);
    h = play_events(&chip_a, sp0256_golden_sweep, 64, SP0256_GOLDEN_SWEEP_SAMPLES, NULL, NULL);
    checkf(h == SP0256_GOLDEN_SWEEP_HASH, "all 64 allophones in turn (%d samples): hash %08x (MAME %08x)",
           SP0256_GOLDEN_SWEEP_SAMPLES, h, (uint32_t)SP0256_GOLDEN_SWEEP_HASH);

    /* the time API and the 48 kHz stage on real speech: one second of
     * chip time in 0.1 s steps, rendering as a frame loop would */
    {
        static float wave[4800];
        static int16_t raw[10000];
        double peak = 0, energy = 0;
        size_t total = 0;
        int steps_ok = 1;

        sp0256_init(&chip_a, rom, NULL, 0);
        sp0256_write_alp(&chip_a, 0x1B);
        for (int k = 1; k <= 10; k++)
        {
            sp0256_sync_to(&chip_a, (uint64_t)k * (SP0256_O2_CLOCK / 10));
            const size_t made = sp0256_render_f32(&chip_a, wave, 4800);

            steps_ok &= made == 4800;
            total += made;
            for (size_t i = 0; i < made; i++)
            {
                if (fabs(wave[i]) > peak) peak = fabs(wave[i]);
                energy += (double)wave[i] * wave[i];
            }
        }
        checkf(steps_ok && total == 48000 && peak > 0.05 && peak < 1.0 && energy > 1.0,
               "48 kHz render of \"HH1\" in 0.1 s steps: %zu samples, peak %.3f", total, peak);
        check(sp0256_read_raw(&chip_a, raw, 10000) == SP0256_RING_SIZE,
              "the raw and 48 kHz outputs are independent readers of the ring");
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "rom"))
    {
        const int r = test_rom();

        if (r == 77)
            return 77;
        printf("%s\n", failures ? "FAILED" : "all passed");
        return failures ? 1 : 0;
    }

    test_scenarios();
    test_synth();
    test_protocol();
    test_o2_glue();
    test_clock();
    test_ring();
    test_resampler();

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

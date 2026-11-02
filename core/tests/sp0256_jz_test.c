/*
 * sp0256_jz_test -- an independent second opinion on the LPC decode.
 *
 * Joseph Zbiciak's jzIntv has its own SP0256 emulation (src/ivoice/ivoice.c),
 * the algorithm MAME's file was ported from. This test compiles that file,
 * unmodified, into the test (a #include of the path CMake passes in
 * SP0256_JZ_IVOICE_C: the jzIntv source tree is GPL and lives outside this
 * repository; nothing of it is copied here), runs its microsequencer over
 * each of the Intellivoice ROM's 64 allophones, and compares the stream of
 * LPC frames it hands its filter (the 16 encoded registers and the repeat
 * count) with the frames core/o2/sp0256.c hands its own (sp0256_set_frame_hook).
 *
 * It is a cross-check of getb / the opcode and data-format tables / SETPAGE,
 * JMP, JSR, RTS and the field and delta arithmetic -- NOT a bit-exactness
 * test of the sound, because the jzIntv in /Workspace (20200712) is a later
 * revision than the one MAME ported and differs on purpose:
 *   - the filter: the period is PR samples (MAME: PR + 1), the noise
 *     generator steps on every sample (MAME: only in noise mode), the delay
 *     line is cleared per instruction (MAME: per pitch impulse), the repeat
 *     count is `repeat` (MAME: repeat + 1), and the output is 8-bit scaled
 *     unless HIGH_QUALITY;
 *   - PAUSE keeps the previous coefficients (MAME clears the register set);
 *   - IA/IP (interpolation) are cleared by most opcodes;
 *   - F5/B5 are cleared in the ten-pole modes; a few DELTA / SETMSB forms
 *     shift or clear differently.
 * The goldens below are what the two agree and disagree on for the
 * Intellivoice ROM; a change in either decoder moves them. Needs the ROM
 * (sp0256_romfind.h) and the jzIntv tree; exits 77 without either.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SP0256_JZ_IVOICE_C

int main(void)
{
    printf("SKIP: built without the jzIntv sources (-DSP0256_JZ_IVOICE_C=<path to src/ivoice/ivoice.c>; "
           "CMake finds them from SP0256_JZINTV_SRC or ~/Workspace/jzintv-20200712-src)\n");
    return 77;
}

#else

#include "sp0256.h"
#include "sp0256_romfind.h"

/* jzIntv's own file, as is (it needs a handful of its services; the
 * sample-recording hooks it calls are stubbed below, after the include) */
#include SP0256_JZ_IVOICE_C

int jzp_printf(const char *fmt, ...) { (void)fmt; return 0; }
void jzp_flush(void) {}
void jzp_clear_and_eol(int n) { (void)n; }
int file_exists(const char *p) { (void)p; return 0; }
uint32_t crc32_tbl[256];

/* what the two decoders are expected to do on the Intellivoice ROM */
#define GOLDEN_FRAMES        595    /* LPC frames over the 64 allophones */
#define GOLDEN_PAUSE         77     /*   of which PAUSE                   */
#define GOLDEN_IDENTICAL     505    /* non-pause frames equal in r[0..13] and repeat */
#define GOLDEN_DIFFER        13     /* non-pause frames that are not      */
#define GOLDEN_INTERP_DIFFER 9      /* non-pause frames whose IA/IP differ */

typedef struct { uint8_t r[16]; int repeat; int pause; } frame_t;
#define MAXF 4000
static frame_t jf[MAXF], mf[MAXF];
static int njf, nmf;
static ivoice_t iv;
static sp0256_t chip;

static void hook(void *ctx, const sp0256_lpc12_t *f, int repeat)
{
    (void)ctx;
    if (nmf < MAXF)
    {
        memcpy(mf[nmf].r, f->r, 16);
        mf[nmf].repeat = repeat;
        nmf++;
    }
}

int main(void)
{
    static uint8_t page[4096];
    uint8_t rom[2048];
    char how[1300];
    int frames = 0, pauses = 0, same = 0, differ = 0, interp = 0, structure_ok = 1;

    if (!find_rom(rom, how, sizeof how))
    {
        printf("SKIP: the Intellivoice ROM (sp0256-012.bin, CRC %08X) was not found; see sp0256_romfind.h\n",
               SP0256_INTV_ROM_CRC);
        return 77;
    }
    printf("ROM: %s\n", how);
    memcpy(page, rom, 2048);

    for (int a = 0; a < 64; a++)
    {
        /* jzIntv: a fresh chip, the command latched, step the sequencer; each
         * time it loads a frame note it and let the filter "finish" at once */
        memset(&iv, 0, sizeof iv);
        iv.rom[1] = page;
        iv.halted = 1; iv.filt.rpt = -1; iv.lrq = 0; iv.ald = a << 4; iv.page = 0x1000 << 3; iv.silent = 1;
        njf = 0;
        for (int guard = 0; guard < MAXF; guard++)
        {
            sp0256_micro(&iv);
            if (iv.halted)
                break;
            if (iv.filt.rpt > 0 && njf < MAXF)
            {
                memcpy(jf[njf].r, iv.filt.r, 16);
                jf[njf].repeat = iv.filt.rpt;
                jf[njf].pause = iv.silent;
                njf++;
            }
            iv.filt.rpt = 0;
            iv.filt.cnt = 0;
        }

        /* ours: the same command, played out for real, frames from the hook */
        sp0256_init(&chip, rom, NULL, 0);
        sp0256_set_frame_hook(&chip, hook, NULL);
        nmf = 0;
        sp0256_write_alp(&chip, (uint8_t)a);
        for (int t = 0; t < 400000; t++)
        {
            int16_t s;

            sp0256_run_samples(&chip, 1);
            sp0256_read_raw(&chip, &s, 1);
            if (t > 100 && sp0256_sby(&chip))
                break;
        }

        if (njf != nmf)
        {
            printf("FAIL: allophone %02X: jzIntv loads %d frames, we load %d\n", a, njf, nmf);
            structure_ok = 0;
            continue;
        }
        for (int i = 0; i < njf; i++)
        {
            frames++;
            if (jf[i].repeat != mf[i].repeat)
            {
                printf("FAIL: allophone %02X frame %d: repeat %d (jzIntv) vs %d\n", a, i, jf[i].repeat, mf[i].repeat);
                structure_ok = 0;
            }
            if (jf[i].pause)
            {
                pauses++;
                if (jf[i].r[1] != mf[i].r[1])
                {
                    printf("FAIL: allophone %02X frame %d: PAUSE period %02X vs %02X\n", a, i, jf[i].r[1], mf[i].r[1]);
                    structure_ok = 0;
                }
                continue;
            }
            if (memcmp(jf[i].r, mf[i].r, 14) == 0)
                same++;
            else
            {
                differ++;
                printf("note: allophone %02X frame %d differs (jzIntv / ours):", a, i);
                for (int k = 0; k < 16; k++)
                    printf(" %02x/%02x", jf[i].r[k], mf[i].r[k]);
                printf("\n");
            }
            if (memcmp(jf[i].r + 14, mf[i].r + 14, 2))
                interp++;
        }
    }

    printf("%s: both decoders load the same frames, the same number per allophone, with the same repeat counts\n",
           structure_ok ? "ok" : "FAIL");
    printf("frames %d, PAUSE %d, identical in r[0..13] %d, different %d, IA/IP different %d\n",
           frames, pauses, same, differ, interp);

    {
        const int ok = structure_ok && frames == GOLDEN_FRAMES && pauses == GOLDEN_PAUSE &&
                       same == GOLDEN_IDENTICAL && differ == GOLDEN_DIFFER && interp == GOLDEN_INTERP_DIFFER;

        printf("%s: the agreement with jzIntv is the recorded one (%d/%d identical, %d known differences)\n",
               ok ? "ok" : "FAIL", GOLDEN_IDENTICAL, GOLDEN_IDENTICAL + GOLDEN_DIFFER, GOLDEN_DIFFER);
        printf("%s\n", ok ? "all passed" : "FAILED");
        return ok ? 0 : 1;
    }
}

#endif

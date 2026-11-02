/* sp0256.c -- the SP0256 Narrator Speech Processor (see sp0256.h).
 *
 * Transposed from MAME's src/devices/sound/sp0256.cpp (BSD-3-Clause,
 * copyright-holders Joseph Zbiciak and Tim Lindner), itself a port of Joseph
 * Zbiciak's jzIntv Intellivoice emulation (src/ivoice/ivoice.c, the origin of
 * the microsequencer, the register decode and the filter). The names are
 * MAME's with the m_ prefix dropped, so the two can be read side by side.
 *
 * Deviations from MAME (the full account is in sp0256.h; each is marked
 * "DEVIATION" or commented where it happens):
 *   1. No device framework: no emu_timer, sound_stream, devcb. The chip is
 *      driven by sp0256_advance()/sp0256_sync_to()/sp0256_run_samples();
 *      sound_stream_update() becomes run_chunk(), writing straight into the
 *      raw ring that was MAME's scratch buffer. The sequence of
 *      micro()/lpc12_update() calls and every sample is unchanged (tested:
 *      the output does not depend on the chunk size).
 *   2. MAME's scratch-ring wrap defect is not reproduced (see sp0256.h); a
 *      full ring drops its oldest sample instead of stalling the stream.
 *   3. micro() stops after SP0256_MICRO_BUDGET instructions (MAME would hang
 *      forever on a ROM that jumps in a loop without ever producing sound).
 *   4. The 48 kHz resampler, which MAME leaves to its sound system.
 *   5. assert(idx0 >= 0 ...) in micro() becomes a guard.
 *   6. sp0256_set_frame_hook() and the Odyssey 2 glue (voice.cpp).
 * Not ported: the drq/sby callbacks (queried instead), save-state, logging.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/sound/sp0256.cpp; copyright-holders
 * Joseph Zbiciak, Tim Lindner -- see COMPLIANCE.md)
 */

#include <math.h>
#include <string.h>

#include "sp0256.h"

#define PER_PAUSE    (64)               /* Equiv timing period for pauses.  */
#define PER_NOISE    (64)               /* Equiv timing period for noise.   */

#define FIFO_ADDR    (0x1800u << 3)     /* SP0256 address of SPB260 speech FIFO.   */

#define SCBUF_MASK   (SP0256_RING_SIZE - 1u)

/* ======================================================================== */
/*  LIMIT            -- Limiter function for digital sample output.         */
/*                      (MAME's HIGH_QUALITY variant: +-8191/8192, output   */
/*                      scaled by 4. jzIntv's default is the 8-bit one.)    */
/* ======================================================================== */
static int16_t limit(int16_t s)
{
    if (s >  8191) return  8191;
    if (s < -8192) return -8192;
    return s;
}

/* ======================================================================== */
/*  LPC12_UPDATE     -- Update the 12-pole filter, outputting samples.      */
/* ======================================================================== */
static int lpc12_update(sp0256_t *sp, int num_samp)
{
    sp0256_lpc12_t *f = &sp->filt;
    int i;

    /* -------------------------------------------------------------------- */
    /*  Iterate up to the desired number of samples.  We actually may       */
    /*  break out early if our repeat count expires.                        */
    /* -------------------------------------------------------------------- */
    for (i = 0; i < num_samp; i++)
    {
        /* ---------------------------------------------------------------- */
        /*  Generate a series of periodic impulses, or random noise.        */
        /* ---------------------------------------------------------------- */
        int do_int = 0;
        uint16_t samp = 0;          /* MAME's local is uint16_t: it wraps  */
        if (f->per)
        {
            if (f->cnt <= 0)
            {
                f->cnt = (int)((uint32_t)f->cnt + f->per);
                samp = (uint16_t)f->amp;
                f->rpt--;
                do_int = f->interp;

                for (int j = 0; j < 6; j++)
                    f->z_data[j][1] = f->z_data[j][0] = 0;
            }
            else
            {
                samp = 0;
                f->cnt--;
            }
        }
        else
        {
            if (--f->cnt <= 0)
            {
                do_int = f->interp;
                f->cnt = PER_NOISE;
                f->rpt--;
                for (int j = 0; j < 6; j++)
                    f->z_data[j][0] = f->z_data[j][1] = 0;
            }

            const int bit = (int)(f->rng & 1);
            f->rng = (f->rng >> 1) ^ (bit ? 0x4001u : 0u);

            samp = (uint16_t)(bit ? f->amp : -f->amp);
        }

        /* ---------------------------------------------------------------- */
        /*  If we need to, process the interpolation registers.             */
        /* ---------------------------------------------------------------- */
        if (do_int)
        {
            f->r[0] = (uint8_t)(f->r[0] + f->r[14]);
            f->r[1] = (uint8_t)(f->r[1] + f->r[15]);

            f->amp  = (f->r[0] & 0x1F) << (((f->r[0] & 0xE0) >> 5) + 0);
            f->per  = f->r[1];
        }

        /* ---------------------------------------------------------------- */
        /*  Stop if we expire our repeat counter and return the actual      */
        /*  number of samples we did.                                       */
        /* ---------------------------------------------------------------- */
        if (f->rpt <= 0)
            break;

        /* ---------------------------------------------------------------- */
        /*  Each 2nd order stage looks like one of these.  The App. Manual  */
        /*  gives the first form, the patent gives the second form.         */
        /*  They're equivalent except for time delay.  I implement the      */
        /*  first form.   (Note: 1/Z == 1 unit of time delay.)              */
        /*                                                                  */
        /*          ---->(+)-------->(+)----------+------->                 */
        /*                ^           ^           |                         */
        /*                |           |           |                         */
        /*                |           |           |                         */
        /*               [B]        [2*F]         |                         */
        /*                ^           ^           |                         */
        /*                |           |           |                         */
        /*                |           |           |                         */
        /*                +---[1/Z]<--+---[1/Z]<--+                         */
        /*                                                                  */
        /*                                                                  */
        /*                +---[2*F]<---+                                    */
        /*                |            |                                    */
        /*                |            |                                    */
        /*                v            |                                    */
        /*          ---->(+)-->[1/Z]-->+-->[1/Z]---+------>                 */
        /*                ^                        |                        */
        /*                |                        |                        */
        /*                |                        |                        */
        /*                +-----------[B]<---------+                        */
        /*                                                                  */
        /* ---------------------------------------------------------------- */
        for (int j = 0; j < 6; j++)
        {
            samp = (uint16_t)(samp + (((int)f->b_coef[j] * (int)f->z_data[j][1]) >> 9));
            samp = (uint16_t)(samp + (((int)f->f_coef[j] * (int)f->z_data[j][0]) >> 8));

            f->z_data[j][1] = f->z_data[j][0];
            f->z_data[j][0] = (int16_t)samp;
        }

        {
            const int16_t out = (int16_t)(limit((int16_t)samp) * 4); /* MAME: << 2 */
            const uint64_t idx = sp->ring_head++;

            sp->ring[idx & SCBUF_MASK] = out;
            sp->last_sample = out;
            if (out)
                sp->nz_last = (int64_t)idx;
        }
    }

    return i;
}

/* ======================================================================== */
/*  qtbl  -- Coefficient Quantization Table.  This comes from a             */
/*              SP0250 data sheet, and should be correct for SP0256.        */
/* ======================================================================== */
static const int16_t qtbl[128] =
{
    0,      9,      17,     25,     33,     41,     49,     57,
    65,     73,     81,     89,     97,     105,    113,    121,
    129,    137,    145,    153,    161,    169,    177,    185,
    193,    201,    209,    217,    225,    233,    241,    249,
    257,    265,    273,    281,    289,    297,    301,    305,
    309,    313,    317,    321,    325,    329,    333,    337,
    341,    345,    349,    353,    357,    361,    365,    369,
    373,    377,    381,    385,    389,    393,    397,    401,
    405,    409,    413,    417,    421,    425,    427,    429,
    431,    433,    435,    437,    439,    441,    443,    445,
    447,    449,    451,    453,    455,    457,    459,    461,
    463,    465,    467,    469,    471,    473,    475,    477,
    479,    481,    482,    483,    484,    485,    486,    487,
    488,    489,    490,    491,    492,    493,    494,    495,
    496,    497,    498,    499,    500,    501,    502,    503,
    504,    505,    506,    507,    508,    509,    510,    511
};

/* ======================================================================== */
/*  LPC12_REGDEC -- Decode the register set in the filter bank.             */
/* ======================================================================== */
static void lpc12_regdec(sp0256_lpc12_t *f)
{
    /* -------------------------------------------------------------------- */
    /*  Decode the Amplitude and Period registers.  Force the 'cnt' to 0    */
    /*  to get an initial impulse.  We compensate elsewhere by setting      */
    /*  the repeat count to "repeat + 1".                                   */
    /* -------------------------------------------------------------------- */
    f->amp = (f->r[0] & 0x1F) << (((f->r[0] & 0xE0) >> 5) + 0);
    f->cnt = 0;
    f->per = f->r[1];

    /* -------------------------------------------------------------------- */
    /*  Decode the filter coefficients from the quant table.                */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < 6; i++)
    {
        /* stage_map is the identity ({ 0, 1, 2, 3, 4, 5 }) in MAME */

#define IQ(x) (((x) & 0x80) ? qtbl[0x7F & -(x)] : -qtbl[(x)])

        f->b_coef[i] = (int16_t)IQ((int)f->r[2 + 2*i]);
        f->f_coef[i] = (int16_t)IQ((int)f->r[3 + 2*i]);

#undef IQ
    }

    /* -------------------------------------------------------------------- */
    /*  Set the Interp flag based on whether we have interpolation parms    */
    /* -------------------------------------------------------------------- */
    f->interp = f->r[14] || f->r[15];
}

/* ======================================================================== */
/*  SP0256_DATAFMT   -- Data format table for the SP0256's microsequencer   */
/*                                                                          */
/*  len     4 bits      Length of field to extract                          */
/*  lshift  4 bits      Left-shift amount on field                          */
/*  param   4 bits      Parameter number being updated                      */
/*  delta   1 bit       This is a delta-update.  (Implies sign-extend)      */
/*  field   1 bit       This is a field replace.                            */
/*  clr5    1 bit       Clear F5, B5.                                       */
/*  clrall  1 bit       Clear all before doing this update                  */
/* ======================================================================== */

#define CR(l,s,p,d,f,c5,ca)         \
        (                           \
            (((l)  & 15) <<  0) |   \
            (((s)  & 15) <<  4) |   \
            (((p)  & 15) <<  8) |   \
            (((d)  &  1) << 12) |   \
            (((f)  &  1) << 13) |   \
            (((c5) &  1) << 14) |   \
            (((ca) &  1) << 15)     \
        )

#define CR_DELTA  CR(0,0,0,1,0,0,0)
#define CR_FIELD  CR(0,0,0,0,1,0,0)
#define CR_CLR5   CR(0,0,0,0,0,1,0)
#define CR_CLRA   CR(0,0,0,0,0,0,1)
#define CR_LEN(x) ((x) & 15)
#define CR_SHF(x) (((x) >> 4) & 15)
#define CR_PRM(x) (((x) >> 8) & 15)

enum { AM = 0, PR, B0, F0, B1, F1, B2, F2, B3, F3, B4, F4, B5, F5, IA, IP };

static const uint16_t sp0256_datafmt[] =
{
    /* -------------------------------------------------------------------- */
    /*  OPCODE 1111: PAUSE                                                  */
    /* -------------------------------------------------------------------- */
    /*    0 */  CR( 0,  0,  0,  0,  0,  0,  1),     /*  Clear all   */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0001: LOADALL                                                */
    /* -------------------------------------------------------------------- */
                /* All modes                */
    /*    1 */  CR( 8,  0,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*    2 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*    3 */  CR( 8,  0,  B0, 0,  0,  0,  0),     /*  B0          */
    /*    4 */  CR( 8,  0,  F0, 0,  0,  0,  0),     /*  F0          */
    /*    5 */  CR( 8,  0,  B1, 0,  0,  0,  0),     /*  B1          */
    /*    6 */  CR( 8,  0,  F1, 0,  0,  0,  0),     /*  F1          */
    /*    7 */  CR( 8,  0,  B2, 0,  0,  0,  0),     /*  B2          */
    /*    8 */  CR( 8,  0,  F2, 0,  0,  0,  0),     /*  F2          */
    /*    9 */  CR( 8,  0,  B3, 0,  0,  0,  0),     /*  B3          */
    /*   10 */  CR( 8,  0,  F3, 0,  0,  0,  0),     /*  F3          */
    /*   11 */  CR( 8,  0,  B4, 0,  0,  0,  0),     /*  B4          */
    /*   12 */  CR( 8,  0,  F4, 0,  0,  0,  0),     /*  F4          */
    /*   13 */  CR( 8,  0,  B5, 0,  0,  0,  0),     /*  B5          */
    /*   14 */  CR( 8,  0,  F5, 0,  0,  0,  0),     /*  F5          */
                /* Mode 01 and 11 only      */
    /*   15 */  CR( 8,  0,  IA, 0,  0,  0,  0),     /*  Amp Interp  */
    /*   16 */  CR( 8,  0,  IP, 0,  0,  0,  0),     /*  Pit Interp  */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0100: LOAD_4                                                 */
    /* -------------------------------------------------------------------- */
                /* Mode 00 and 01           */
    /*   17 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*   18 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*   19 */  CR( 4,  3,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*   20 */  CR( 6,  2,  F3, 0,  0,  0,  0),     /*  F3          */
    /*   21 */  CR( 7,  1,  B4, 0,  0,  0,  0),     /*  B4          */
    /*   22 */  CR( 6,  2,  F4, 0,  0,  0,  0),     /*  F4          */
                /* Mode 01 only             */
    /*   23 */  CR( 8,  0,  B5, 0,  0,  0,  0),     /*  B5          */
    /*   24 */  CR( 8,  0,  F5, 0,  0,  0,  0),     /*  F5          */

                /* Mode 10 and 11           */
    /*   25 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*   26 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*   27 */  CR( 6,  1,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*   28 */  CR( 7,  1,  F3, 0,  0,  0,  0),     /*  F3          */
    /*   29 */  CR( 8,  0,  B4, 0,  0,  0,  0),     /*  B4          */
    /*   30 */  CR( 8,  0,  F4, 0,  0,  0,  0),     /*  F4          */
                /* Mode 11 only             */
    /*   31 */  CR( 8,  0,  B5, 0,  0,  0,  0),     /*  B5          */
    /*   32 */  CR( 8,  0,  F5, 0,  0,  0,  0),     /*  F5          */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0110: SETMSB_6                                               */
    /* -------------------------------------------------------------------- */
                /* Mode 00 only             */
    /*   33 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 00 and 01           */
    /*   34 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*   35 */  CR( 6,  2,  F3, 0,  1,  0,  0),     /*  F3 (5 MSBs) */
    /*   36 */  CR( 6,  2,  F4, 0,  1,  0,  0),     /*  F4 (5 MSBs) */
                /* Mode 01 only             */
    /*   37 */  CR( 8,  0,  F5, 0,  1,  0,  0),     /*  F5 (5 MSBs) */

                /* Mode 10 only             */
    /*   38 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 10 and 11           */
    /*   39 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*   40 */  CR( 7,  1,  F3, 0,  1,  0,  0),     /*  F3 (6 MSBs) */
    /*   41 */  CR( 8,  0,  F4, 0,  1,  0,  0),     /*  F4 (6 MSBs) */
                /* Mode 11 only             */
    /*   42 */  CR( 8,  0,  F5, 0,  1,  0,  0),     /*  F5 (6 MSBs) */

    /*   43 */  0,  /* unused */
    /*   44 */  0,  /* unused */

    /* -------------------------------------------------------------------- */
    /*  Opcode 1001: DELTA_9                                                */
    /* -------------------------------------------------------------------- */
                /* Mode 00 and 01           */
    /*   45 */  CR( 4,  2,  AM, 1,  0,  0,  0),     /*  Amplitude   */
    /*   46 */  CR( 5,  0,  PR, 1,  0,  0,  0),     /*  Period      */
    /*   47 */  CR( 3,  4,  B0, 1,  0,  0,  0),     /*  B0 4 MSBs   */
    /*   48 */  CR( 3,  3,  F0, 1,  0,  0,  0),     /*  F0 5 MSBs   */
    /*   49 */  CR( 3,  4,  B1, 1,  0,  0,  0),     /*  B1 4 MSBs   */
    /*   50 */  CR( 3,  3,  F1, 1,  0,  0,  0),     /*  F1 5 MSBs   */
    /*   51 */  CR( 3,  4,  B2, 1,  0,  0,  0),     /*  B2 4 MSBs   */
    /*   52 */  CR( 3,  3,  F2, 1,  0,  0,  0),     /*  F2 5 MSBs   */
    /*   53 */  CR( 3,  3,  B3, 1,  0,  0,  0),     /*  B3 5 MSBs   */
    /*   54 */  CR( 4,  2,  F3, 1,  0,  0,  0),     /*  F3 6 MSBs   */
    /*   55 */  CR( 4,  1,  B4, 1,  0,  0,  0),     /*  B4 7 MSBs   */
    /*   56 */  CR( 4,  2,  F4, 1,  0,  0,  0),     /*  F4 6 MSBs   */
                /* Mode 01 only             */
    /*   57 */  CR( 5,  0,  B5, 1,  0,  0,  0),     /*  B5 8 MSBs   */
    /*   58 */  CR( 5,  0,  F5, 1,  0,  0,  0),     /*  F5 8 MSBs   */

                /* Mode 10 and 11           */
    /*   59 */  CR( 4,  2,  AM, 1,  0,  0,  0),     /*  Amplitude   */
    /*   60 */  CR( 5,  0,  PR, 1,  0,  0,  0),     /*  Period      */
    /*   61 */  CR( 4,  1,  B0, 1,  0,  0,  0),     /*  B0 7 MSBs   */
    /*   62 */  CR( 4,  2,  F0, 1,  0,  0,  0),     /*  F0 6 MSBs   */
    /*   63 */  CR( 4,  1,  B1, 1,  0,  0,  0),     /*  B1 7 MSBs   */
    /*   64 */  CR( 4,  2,  F1, 1,  0,  0,  0),     /*  F1 6 MSBs   */
    /*   65 */  CR( 4,  1,  B2, 1,  0,  0,  0),     /*  B2 7 MSBs   */
    /*   66 */  CR( 4,  2,  F2, 1,  0,  0,  0),     /*  F2 6 MSBs   */
    /*   67 */  CR( 4,  1,  B3, 1,  0,  0,  0),     /*  B3 7 MSBs   */
    /*   68 */  CR( 5,  1,  F3, 1,  0,  0,  0),     /*  F3 7 MSBs   */
    /*   69 */  CR( 5,  0,  B4, 1,  0,  0,  0),     /*  B4 8 MSBs   */
    /*   70 */  CR( 5,  0,  F4, 1,  0,  0,  0),     /*  F4 8 MSBs   */
                /* Mode 11 only             */
    /*   71 */  CR( 5,  0,  B5, 1,  0,  0,  0),     /*  B5 8 MSBs   */
    /*   72 */  CR( 5,  0,  F5, 1,  0,  0,  0),     /*  F5 8 MSBs   */

    /* -------------------------------------------------------------------- */
    /*  Opcode 1010: SETMSB_A                                               */
    /* -------------------------------------------------------------------- */
                /* Mode 00 only             */
    /*   73 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 00 and 01           */
    /*   74 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*   75 */  CR( 5,  3,  F0, 0,  1,  0,  0),     /*  F0 (5 MSBs) */
    /*   76 */  CR( 5,  3,  F1, 0,  1,  0,  0),     /*  F1 (5 MSBs) */
    /*   77 */  CR( 5,  3,  F2, 0,  1,  0,  0),     /*  F2 (5 MSBs) */

                /* Mode 10 only             */
    /*   78 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 10 and 11           */
    /*   79 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*   80 */  CR( 6,  2,  F0, 0,  1,  0,  0),     /*  F0 (6 MSBs) */
    /*   81 */  CR( 6,  2,  F1, 0,  1,  0,  0),     /*  F1 (6 MSBs) */
    /*   82 */  CR( 6,  2,  F2, 0,  1,  0,  0),     /*  F2 (6 MSBs) */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0010: LOAD_2  Mode 00 and 10                                 */
    /*  Opcode 1100: LOAD_C  Mode 00 and 10                                 */
    /* -------------------------------------------------------------------- */
                /* LOAD_2, LOAD_C  Mode 00  */
    /*   83 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*   84 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*   85 */  CR( 3,  4,  B0, 0,  0,  0,  0),     /*  B0 (S=0)    */
    /*   86 */  CR( 5,  3,  F0, 0,  0,  0,  0),     /*  F0          */
    /*   87 */  CR( 3,  4,  B1, 0,  0,  0,  0),     /*  B1 (S=0)    */
    /*   88 */  CR( 5,  3,  F1, 0,  0,  0,  0),     /*  F1          */
    /*   89 */  CR( 3,  4,  B2, 0,  0,  0,  0),     /*  B2 (S=0)    */
    /*   90 */  CR( 5,  3,  F2, 0,  0,  0,  0),     /*  F2          */
    /*   91 */  CR( 4,  3,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*   92 */  CR( 6,  2,  F3, 0,  0,  0,  0),     /*  F3          */
    /*   93 */  CR( 7,  1,  B4, 0,  0,  0,  0),     /*  B4          */
    /*   94 */  CR( 6,  2,  F4, 0,  0,  0,  0),     /*  F4          */
                /* LOAD_2 only              */
    /*   95 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*   96 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */

                /* LOAD_2, LOAD_C  Mode 10  */
    /*   97 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*   98 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*   99 */  CR( 6,  1,  B0, 0,  0,  0,  0),     /*  B0 (S=0)    */
    /*  100 */  CR( 6,  2,  F0, 0,  0,  0,  0),     /*  F0          */
    /*  101 */  CR( 6,  1,  B1, 0,  0,  0,  0),     /*  B1 (S=0)    */
    /*  102 */  CR( 6,  2,  F1, 0,  0,  0,  0),     /*  F1          */
    /*  103 */  CR( 6,  1,  B2, 0,  0,  0,  0),     /*  B2 (S=0)    */
    /*  104 */  CR( 6,  2,  F2, 0,  0,  0,  0),     /*  F2          */
    /*  105 */  CR( 6,  1,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*  106 */  CR( 7,  1,  F3, 0,  0,  0,  0),     /*  F3          */
    /*  107 */  CR( 8,  0,  B4, 0,  0,  0,  0),     /*  B4          */
    /*  108 */  CR( 8,  0,  F4, 0,  0,  0,  0),     /*  F4          */
                /* LOAD_2 only              */
    /*  109 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*  110 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */

    /* -------------------------------------------------------------------- */
    /*  OPCODE 1101: DELTA_D                                                */
    /* -------------------------------------------------------------------- */
                /* Mode 00 and 01           */
    /*  111 */  CR( 4,  2,  AM, 1,  0,  0,  0),     /*  Amplitude   */
    /*  112 */  CR( 5,  0,  PR, 1,  0,  0,  0),     /*  Period      */
    /*  113 */  CR( 3,  3,  B3, 1,  0,  0,  0),     /*  B3 5 MSBs   */
    /*  114 */  CR( 4,  2,  F3, 1,  0,  0,  0),     /*  F3 6 MSBs   */
    /*  115 */  CR( 4,  1,  B4, 1,  0,  0,  0),     /*  B4 7 MSBs   */
    /*  116 */  CR( 4,  2,  F4, 1,  0,  0,  0),     /*  F4 6 MSBs   */
                /* Mode 01 only             */
    /*  117 */  CR( 5,  0,  B5, 1,  0,  0,  0),     /*  B5 8 MSBs   */
    /*  118 */  CR( 5,  0,  F5, 1,  0,  0,  0),     /*  F5 8 MSBs   */

                /* Mode 10 and 11           */
    /*  119 */  CR( 4,  2,  AM, 1,  0,  0,  0),     /*  Amplitude   */
    /*  120 */  CR( 5,  0,  PR, 1,  0,  0,  0),     /*  Period      */
    /*  121 */  CR( 4,  1,  B3, 1,  0,  0,  0),     /*  B3 7 MSBs   */
    /*  122 */  CR( 5,  1,  F3, 1,  0,  0,  0),     /*  F3 7 MSBs   */
    /*  123 */  CR( 5,  0,  B4, 1,  0,  0,  0),     /*  B4 8 MSBs   */
    /*  124 */  CR( 5,  0,  F4, 1,  0,  0,  0),     /*  F4 8 MSBs   */
                /* Mode 11 only             */
    /*  125 */  CR( 5,  0,  B5, 1,  0,  0,  0),     /*  B5 8 MSBs   */
    /*  126 */  CR( 5,  0,  F5, 1,  0,  0,  0),     /*  F5 8 MSBs   */

    /* -------------------------------------------------------------------- */
    /*  OPCODE 1110: LOAD_E                                                 */
    /* -------------------------------------------------------------------- */
    /*  127 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*  128 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0010: LOAD_2  Mode 01 and 11                                 */
    /*  Opcode 1100: LOAD_C  Mode 01 and 11                                 */
    /* -------------------------------------------------------------------- */
                /* LOAD_2, LOAD_C  Mode 01  */
    /*  129 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*  130 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*  131 */  CR( 3,  4,  B0, 0,  0,  0,  0),     /*  B0 (S=0)    */
    /*  132 */  CR( 5,  3,  F0, 0,  0,  0,  0),     /*  F0          */
    /*  133 */  CR( 3,  4,  B1, 0,  0,  0,  0),     /*  B1 (S=0)    */
    /*  134 */  CR( 5,  3,  F1, 0,  0,  0,  0),     /*  F1          */
    /*  135 */  CR( 3,  4,  B2, 0,  0,  0,  0),     /*  B2 (S=0)    */
    /*  136 */  CR( 5,  3,  F2, 0,  0,  0,  0),     /*  F2          */
    /*  137 */  CR( 4,  3,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*  138 */  CR( 6,  2,  F3, 0,  0,  0,  0),     /*  F3          */
    /*  139 */  CR( 7,  1,  B4, 0,  0,  0,  0),     /*  B4          */
    /*  140 */  CR( 6,  2,  F4, 0,  0,  0,  0),     /*  F4          */
    /*  141 */  CR( 8,  0,  B5, 0,  0,  0,  0),     /*  B5          */
    /*  142 */  CR( 8,  0,  F5, 0,  0,  0,  0),     /*  F5          */
                /* LOAD_2 only              */
    /*  143 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*  144 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */

                /* LOAD_2, LOAD_C  Mode 11  */
    /*  145 */  CR( 6,  2,  AM, 0,  0,  0,  1),     /*  Amplitude   */
    /*  146 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*  147 */  CR( 6,  1,  B0, 0,  0,  0,  0),     /*  B0 (S=0)    */
    /*  148 */  CR( 6,  2,  F0, 0,  0,  0,  0),     /*  F0          */
    /*  149 */  CR( 6,  1,  B1, 0,  0,  0,  0),     /*  B1 (S=0)    */
    /*  150 */  CR( 6,  2,  F1, 0,  0,  0,  0),     /*  F1          */
    /*  151 */  CR( 6,  1,  B2, 0,  0,  0,  0),     /*  B2 (S=0)    */
    /*  152 */  CR( 6,  2,  F2, 0,  0,  0,  0),     /*  F2          */
    /*  153 */  CR( 6,  1,  B3, 0,  0,  0,  0),     /*  B3 (S=0)    */
    /*  154 */  CR( 7,  1,  F3, 0,  0,  0,  0),     /*  F3          */
    /*  155 */  CR( 8,  0,  B4, 0,  0,  0,  0),     /*  B4          */
    /*  156 */  CR( 8,  0,  F4, 0,  0,  0,  0),     /*  F4          */
    /*  157 */  CR( 8,  0,  B5, 0,  0,  0,  0),     /*  B5          */
    /*  158 */  CR( 8,  0,  F5, 0,  0,  0,  0),     /*  F5          */
                /* LOAD_2 only              */
    /*  159 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*  160 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */

    /* -------------------------------------------------------------------- */
    /*  Opcode 0011: SETMSB_3                                               */
    /*  Opcode 0101: SETMSB_5                                               */
    /* -------------------------------------------------------------------- */
                /* Mode 00 only             */
    /*  161 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 00 and 01           */
    /*  162 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*  163 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*  164 */  CR( 5,  3,  F0, 0,  1,  0,  0),     /*  F0 (5 MSBs) */
    /*  165 */  CR( 5,  3,  F1, 0,  1,  0,  0),     /*  F1 (5 MSBs) */
    /*  166 */  CR( 5,  3,  F2, 0,  1,  0,  0),     /*  F2 (5 MSBs) */
                /* SETMSB_3 only            */
    /*  167 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*  168 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */

                /* Mode 10 only             */
    /*  169 */  CR( 0,  0,  0,  0,  0,  1,  0),     /*  Clear 5     */
                /* Mode 10 and 11           */
    /*  170 */  CR( 6,  2,  AM, 0,  0,  0,  0),     /*  Amplitude   */
    /*  171 */  CR( 8,  0,  PR, 0,  0,  0,  0),     /*  Period      */
    /*  172 */  CR( 6,  2,  F0, 0,  1,  0,  0),     /*  F0 (6 MSBs) */
    /*  173 */  CR( 6,  2,  F1, 0,  1,  0,  0),     /*  F1 (6 MSBs) */
    /*  174 */  CR( 6,  2,  F2, 0,  1,  0,  0),     /*  F2 (6 MSBs) */
                /* SETMSB_3 only            */
    /*  175 */  CR( 5,  0,  IA, 0,  0,  0,  0),     /*  Ampl. Intr. */
    /*  176 */  CR( 5,  0,  IP, 0,  0,  0,  0),     /*  Per. Intr.  */
};

static const int16_t sp0256_df_idx[16 * 8] =
{
    /*  OPCODE 0000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1000 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0100 */      17, 22,     17, 24,     25, 30,     25, 32,
    /*  OPCODE 1100 */      83, 94,     129,142,    97, 108,    145,158,
    /*  OPCODE 0010 */      83, 96,     129,144,    97, 110,    145,160,
    /*  OPCODE 1010 */      73, 77,     74, 77,     78, 82,     79, 82,
    /*  OPCODE 0110 */      33, 36,     34, 37,     38, 41,     39, 42,
    /*  OPCODE 1110 */      127,128,    127,128,    127,128,    127,128,
    /*  OPCODE 0001 */      1,  14,     1,  16,     1,  14,     1,  16,
    /*  OPCODE 1001 */      45, 56,     45, 58,     59, 70,     59, 72,
    /*  OPCODE 0101 */      161,166,    162,166,    169,174,    170,174,
    /*  OPCODE 1101 */      111,116,    111,118,    119,124,    119,126,
    /*  OPCODE 0011 */      161,168,    162,168,    169,176,    170,176,
    /*  OPCODE 1011 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 0111 */      -1, -1,     -1, -1,     -1, -1,     -1, -1,
    /*  OPCODE 1111 */      0,  0,      0,  0,      0,  0,      0,  0
};

/* ======================================================================== */
/*  BITREV32       -- Bit-reverse a 32-bit number.                            */
/* ======================================================================== */
static uint32_t bitrev32(uint32_t val)
{
    val = ((val & 0xFFFF0000u) >> 16) | ((val & 0x0000FFFFu) << 16);
    val = ((val & 0xFF00FF00u) >>  8) | ((val & 0x00FF00FFu) <<  8);
    val = ((val & 0xF0F0F0F0u) >>  4) | ((val & 0x0F0F0F0Fu) <<  4);
    val = ((val & 0xCCCCCCCCu) >>  2) | ((val & 0x33333333u) <<  2);
    val = ((val & 0xAAAAAAAAu) >>  1) | ((val & 0x55555555u) <<  1);

    return val;
}

/* ======================================================================== */
/*  SP0256_GETB  -- Get up to 8 bits at the current PC.                     */
/* ======================================================================== */
static uint32_t getb(sp0256_t *sp, int len)
{
    uint32_t data;
    uint32_t d0, d1;

    /* -------------------------------------------------------------------- */
    /*  Fetch data from the FIFO or from the MASK                           */
    /* -------------------------------------------------------------------- */
    if (sp->fifo_sel)
    {
        d0 = sp->fifo[(sp->fifo_tail    ) & 63];
        d1 = sp->fifo[(sp->fifo_tail + 1) & 63];

        data = ((d1 << 10) | d0) >> sp->fifo_bitp;

        /* ---------------------------------------------------------------- */
        /*  Note the PC doesn't advance when we execute from FIFO.          */
        /*  Just the FIFO's bit-pointer advances.   (That's not REALLY      */
        /*  what happens, but that's roughly how it behaves.)               */
        /* ---------------------------------------------------------------- */
        sp->fifo_bitp += (uint32_t)len;
        if (sp->fifo_bitp >= 10)
        {
            sp->fifo_tail++;
            sp->fifo_bitp -= 10;
        }
    }
    else
    {
        /* ---------------------------------------------------------------- */
        /*  Figure out which ROMs are being fetched into, and grab two      */
        /*  adjacent bytes.  The byte we're interested in is extracted      */
        /*  from the appropriate bit-boundary between them.                 */
        /* ---------------------------------------------------------------- */
        uint32_t idx0 = (sp->pc    ) >> 3;
        uint32_t idx1 = (sp->pc + 8) >> 3;

        d0 = sp->rom[idx0 & 0xffff];
        d1 = sp->rom[idx1 & 0xffff];

        data = ((d1 << 8) | d0) >> (sp->pc & 7);

        sp->pc += (uint32_t)len;
    }

    /* -------------------------------------------------------------------- */
    /*  Mask data to the requested length.                                  */
    /* -------------------------------------------------------------------- */
    data &= ((1u << len) - 1);

    return data;
}

/* ======================================================================== */
/*  SP0256_MICRO -- Emulate the microsequencer in the SP0256.  Executes     */
/*                  instructions either until the repeat count != 0 or      */
/*                  the sequencer gets halted by a RTS to 0.                */
/* ======================================================================== */
static void micro(sp0256_t *sp)
{
    uint8_t immed4;
    uint8_t opcode;
    uint16_t cr;
    int ctrl_xfer;
    int repeat;
    int i, idx0, idx1;
    int budget = SP0256_MICRO_BUDGET;

    /* -------------------------------------------------------------------- */
    /*  Only execute instructions while the filter is not busy.             */
    /* -------------------------------------------------------------------- */
    while (sp->filt.rpt <= 0)
    {
        /* ---------------------------------------------------------------- */
        /*  DEVIATION 3: a ROM that never produces sound and never halts    */
        /*  (JMP to itself) hangs MAME. The real chip keeps looping while   */
        /*  time passes, so yield after a budget of instructions: silent,   */
        /*  resuming from the same PC at the next sample.                   */
        /* ---------------------------------------------------------------- */
        if (budget-- <= 0)
        {
            sp->filt.amp = 0;
            sp->filt.rpt = 1;
            return;
        }

        /* ---------------------------------------------------------------- */
        /*  If the CPU is halted, see if we have a new command pending      */
        /*  in the Address LoaD buffer.                                     */
        /* ---------------------------------------------------------------- */
        if (sp->halted && !sp->lrq)
        {
            sp->active_allophone = (sp->ald >> 4) & 0xff;  /* debug only */
            sp->pc       = (uint32_t)sp->ald | (0x1000u << 3);
            sp->fifo_sel = 0;
            sp->halted   = 0;
            sp->lrq      = 1;
            sp->ald      = 0;
            for (i = 0; i < 16; i++)
                sp->filt.r[i] = 0;
        }

        /* ---------------------------------------------------------------- */
        /*  If we're still halted, do nothing.                              */
        /* ---------------------------------------------------------------- */
        if (sp->halted)
        {
            sp->filt.rpt = 1;
            sp->lrq      = 1;
            sp->ald      = 0;
            for (i = 0; i < 16; i++)
                sp->filt.r[i] = 0;

            sp->sby_line = 1;           /* SET_SBY(1) */
            sp->active_allophone = -1;

            return;
        }

        /* ---------------------------------------------------------------- */
        /*  Fetch the first 8 bits of the opcode, which are always in the   */
        /*  same approximate format -- immed4 followed by opcode.           */
        /* ---------------------------------------------------------------- */
        immed4 = (uint8_t)getb(sp, 4);
        opcode = (uint8_t)getb(sp, 4);
        repeat = 0;
        ctrl_xfer = 0;

        /* ---------------------------------------------------------------- */
        /*  Handle the special cases for specific opcodes.                  */
        /* ---------------------------------------------------------------- */
        switch (opcode)
        {
            /* ------------------------------------------------------------ */
            /*  OPCODE 0000:  RTS / SETPAGE                                 */
            /* ------------------------------------------------------------ */
            case 0x0:
            {
                /* -------------------------------------------------------- */
                /*  If immed4 != 0, then this is a SETPAGE instruction.     */
                /* -------------------------------------------------------- */
                if (immed4)     /* SETPAGE */
                {
                    sp->page = bitrev32(immed4) >> 13;
                }

                /* -------------------------------------------------------- */
                /*  Otherwise, this is an RTS / HLT.                        */
                /* -------------------------------------------------------- */
                else
                {
                    uint32_t btrg;

                    /* ---------------------------------------------------- */
                    /*  Figure out our branch target.                       */
                    /* ---------------------------------------------------- */
                    btrg = sp->stack;

                    sp->stack = 0;

                    /* ---------------------------------------------------- */
                    /*  If the branch target is zero, this is a HLT.        */
                    /*  Otherwise, it's an RTS, so set the PC.              */
                    /* ---------------------------------------------------- */
                    if (!btrg)
                    {
                        sp->halted   = 1;
                        sp->pc       = 0;
                        ctrl_xfer    = 1;
                    }
                    else
                    {
                        sp->pc       = btrg;
                        ctrl_xfer    = 1;
                    }
                }

                break;
            }

            /* ------------------------------------------------------------ */
            /*  OPCODE 0111:  JMP          Jump to 12-bit/16-bit Abs Addr   */
            /*  OPCODE 1011:  JSR          Jump to Subroutine               */
            /* ------------------------------------------------------------ */
            case 0xE:
            case 0xD:
            {
                uint32_t btrg;

                /* -------------------------------------------------------- */
                /*  Figure out our branch target.                           */
                /* -------------------------------------------------------- */
                btrg = sp->page                    |
                        (bitrev32(immed4)  >> 17) |
                        (bitrev32(getb(sp, 8)) >> 21);
                ctrl_xfer = 1;

                /* -------------------------------------------------------- */
                /*  If this is a JSR, push our return address on the        */
                /*  stack.  Make sure it's byte aligned.                    */
                /* -------------------------------------------------------- */
                if (opcode == 0xD)
                    sp->stack = (sp->pc + 7) & ~7u;

                /* -------------------------------------------------------- */
                /*  Jump to the new location!                               */
                /* -------------------------------------------------------- */
                sp->pc = btrg;
                break;
            }

            /* ------------------------------------------------------------ */
            /*  OPCODE 1000:  SETMODE      Set the Mode and Repeat MSBs     */
            /* ------------------------------------------------------------ */
            case 0x1:
            {
                sp->mode = ((immed4 & 8u) >> 2) | (immed4 & 4u) | ((immed4 & 3u) << 4);
                break;
            }

            /* ------------------------------------------------------------ */
            /*  OPCODE 0001:  LOADALL      Load All Parameters              */
            /*  OPCODE 0010:  LOAD_2       Load Per, Ampl, Coefs, Interp.   */
            /*  OPCODE 0011:  SETMSB_3     Load Pitch, Ampl, MSBs, & Intrp  */
            /*  OPCODE 0100:  LOAD_4       Load Pitch, Ampl, Coeffs         */
            /*  OPCODE 0101:  SETMSB_5     Load Pitch, Ampl, and Coeff MSBs */
            /*  OPCODE 0110:  SETMSB_6     Load Ampl, and Coeff MSBs.       */
            /*  OPCODE 1001:  DELTA_9      Delta update Ampl, Pitch, Coeffs */
            /*  OPCODE 1010:  SETMSB_A     Load Ampl and MSBs of 3 Coeffs   */
            /*  OPCODE 1100:  LOAD_C       Load Pitch, Ampl, Coeffs         */
            /*  OPCODE 1101:  DELTA_D      Delta update Ampl, Pitch, Coeffs */
            /*  OPCODE 1110:  LOAD_E       Load Pitch, Amplitude            */
            /*  OPCODE 1111:  PAUSE        Silent pause                     */
            /* ------------------------------------------------------------ */
            default:
            {
                repeat = immed4 | (int)(sp->mode & 0x30);
                break;
            }
        }
        if (opcode != 1) sp->mode &= 0xF;

        /* ---------------------------------------------------------------- */
        /*  If this was a control transfer, handle setting "fifo_sel"       */
        /*  and all that ugliness.                                          */
        /* ---------------------------------------------------------------- */
        if (ctrl_xfer)
        {
            /* ------------------------------------------------------------ */
            /*  Set our "FIFO Selected" flag based on whether we're going   */
            /*  to the FIFO's address.                                      */
            /* ------------------------------------------------------------ */
            sp->fifo_sel = sp->pc == FIFO_ADDR;

            /* ------------------------------------------------------------ */
            /*  Control transfers to the FIFO cause it to discard the       */
            /*  partial decle that's at the front of the FIFO.              */
            /* ------------------------------------------------------------ */
            if (sp->fifo_sel && sp->fifo_bitp)
            {
                /* Discard partially-read decle. */
                if (sp->fifo_tail < sp->fifo_head) sp->fifo_tail++;
                sp->fifo_bitp = 0;
            }

            continue;
        }

        /* ---------------------------------------------------------------- */
        /*  Otherwise, if we have a repeat count, then go grab the data     */
        /*  block and feed it to the filter.                                */
        /* ---------------------------------------------------------------- */
        if (!repeat) continue;

        sp->filt.rpt = repeat + 1;

        i = (opcode << 3) | (int)(sp->mode & 6);
        idx0 = sp0256_df_idx[i++];
        idx1 = sp0256_df_idx[i  ];

        /* DEVIATION 5: MAME asserts idx0 >= 0 && idx1 >= 0 && idx1 >= idx0.
         * Every opcode that reaches here has a row; keep the guard so a
         * table error cannot read outside sp0256_datafmt. */
        if (idx0 < 0 || idx1 < 0 || idx1 < idx0)
        {
            sp->filt.rpt = 0;
            continue;
        }

        /* ---------------------------------------------------------------- */
        /*  Step through control words in the description for data block.   */
        /* ---------------------------------------------------------------- */
        for (i = idx0; i <= idx1; i++)
        {
            int len, shf, delta, field, prm, clra, clr5;
            int8_t value;

            /* ------------------------------------------------------------ */
            /*  Get the control word and pull out some important fields.    */
            /* ------------------------------------------------------------ */
            cr = sp0256_datafmt[i];

            len   = CR_LEN(cr);
            shf   = CR_SHF(cr);
            prm   = CR_PRM(cr);
            clra  = cr & CR_CLRA;
            clr5  = cr & CR_CLR5;
            delta = cr & CR_DELTA;
            field = cr & CR_FIELD;
            value = 0;

            /* ------------------------------------------------------------ */
            /*  Clear any registers that were requested to be cleared.      */
            /* ------------------------------------------------------------ */
            if (clra)
            {
                for (int j = 0; j < 16; j++)
                    sp->filt.r[j] = 0;

                sp->silent = 1;
            }

            if (clr5)
                sp->filt.r[B5] = sp->filt.r[F5] = 0;

            /* ------------------------------------------------------------ */
            /*  If this entry has a bitfield with it, grab the bitfield.    */
            /* ------------------------------------------------------------ */
            if (len)
            {
                value = (int8_t)getb(sp, len);
            }
            else
            {
                continue;
            }

            /* ------------------------------------------------------------ */
            /*  Sign extend if this is a delta update.                      */
            /* ------------------------------------------------------------ */
            if (delta)  /* Sign extend */
            {
                if (value & (1 << (len - 1))) value = (int8_t)(value | (int)(~0u << len));
            }

            /* ------------------------------------------------------------ */
            /*  Shift the value to the appropriate precision.               */
            /* ------------------------------------------------------------ */
            if (shf)
                value = (int8_t)(value * (1 << shf));   /* MAME: value <<= shf */

            sp->silent = 0;

            /* ------------------------------------------------------------ */
            /*  If this is a field-replace, insert the field.               */
            /* ------------------------------------------------------------ */
            if (field)
            {
                sp->filt.r[prm] &= (uint8_t)~(~0u << shf); /* Clear the old bits.     */
                sp->filt.r[prm] |= (uint8_t)value;         /* Merge in the new bits.  */

                continue;
            }

            /* ------------------------------------------------------------ */
            /*  If this is a delta update, add to the appropriate field.    */
            /* ------------------------------------------------------------ */
            if (delta)
            {
                sp->filt.r[prm] = (uint8_t)(sp->filt.r[prm] + value);

                continue;
            }

            /* ------------------------------------------------------------ */
            /*  Otherwise, just write the new value.                        */
            /* ------------------------------------------------------------ */
            sp->filt.r[prm] = (uint8_t)value;
        }

        /* ---------------------------------------------------------------- */
        /*  Special case:  Set PAUSE's equivalent period.                   */
        /* ---------------------------------------------------------------- */
        if (opcode == 0xF)
        {
            sp->silent = 1;
            sp->filt.r[1] = PER_PAUSE;
        }

        /* ---------------------------------------------------------------- */
        /*  Now that we've updated the registers, go decode them.           */
        /* ---------------------------------------------------------------- */
        lpc12_regdec(&sp->filt);

        if (sp->frame_hook)         /* not in MAME: debugger / test hook */
            sp->frame_hook(sp->frame_ctx, &sp->filt, repeat);

        /* ---------------------------------------------------------------- */
        /*  Break out since we now have a repeat count.                     */
        /* ---------------------------------------------------------------- */
        break;
    }
}

/* ======================================================================== */
/*  sound_stream_update, minus the stream: generate `length` samples.       */
/*  (DEVIATION 1/2: the scratch ring is the output ring and is never        */
/*  drained here, so MAME's "drain scratch into the stream" and             */
/*  "scratch full" branches are gone; a full ring just overwrites.)         */
/* ======================================================================== */
static void run_chunk(sp0256_t *sp, int length)
{
    int did_samp = 0;

    if (length <= 0)
        return;

    do
    {
        /* ------------------------------------------------------------ */
        /*  If our repeat count expired, emulate the microsequencer.    */
        /* ------------------------------------------------------------ */
        if (sp->filt.rpt <= 0)
            micro(sp);

        /* ------------------------------------------------------------ */
        /*  Do as many samples as we can.                               */
        /* ------------------------------------------------------------ */
        const int do_samp = length - did_samp;

        if (sp->silent && sp->filt.rpt <= 0)
        {
            /* MAME's silent-fill branch. micro() always leaves rpt > 0,
             * so this cannot happen; kept for fidelity. */
            for (int x = 0; x < do_samp; x++)
                sp->ring[sp->ring_head++ & SCBUF_MASK] = 0;
            sp->last_sample = 0;
            did_samp += do_samp;
        }
        else
        {
            did_samp += lpc12_update(sp, do_samp);
        }

    } while (sp->filt.rpt >= 0 && length > did_samp);

    sp->samples_made += (uint64_t)length;
}

/* ======================================================================== */
/*  Public: time                                                            */
/* ======================================================================== */
void sp0256_run_samples(sp0256_t *sp, uint32_t n)
{
    while (n)
    {
        /* The chunk size does not change the output (the micro()/update()
         * call sequence is the same however a run is split); 2048 keeps it
         * under the ring size as MAME's per-update sample count is. */
        const uint32_t c = n > 2048u ? 2048u : n;

        run_chunk(sp, (int)c);
        n -= c;
    }
}

void sp0256_advance(sp0256_t *sp, uint64_t clocks)
{
    const uint64_t acc = sp->clk_frac + clocks;

    sp->clk_total += clocks;
    sp->clk_frac = (uint32_t)(acc % SP0256_CLOCK_DIVIDER);

    /* a single call can never be asked for more than a few million */
    uint64_t n = acc / SP0256_CLOCK_DIVIDER;
    while (n)
    {
        const uint32_t c = n > 0x40000000u ? 0x40000000u : (uint32_t)n;

        sp0256_run_samples(sp, c);
        n -= c;
    }
}

void sp0256_sync_to(sp0256_t *sp, uint64_t clock_now)
{
    if (clock_now > sp->clk_total)
        sp0256_advance(sp, clock_now - sp->clk_total);
}

/* ======================================================================== */
/*  Public: pins                                                            */
/* ======================================================================== */
void sp0256_write_alp(sp0256_t *sp, uint8_t data)
{
    /* ---------------------------------------------------------------- */
    /*  Drop writes to the ALD register if we're busy.                  */
    /* ---------------------------------------------------------------- */
    if (!sp->lrq)
    {
        sp->alds_dropped++;
        return;
    }

    /* ---------------------------------------------------------------- */
    /*  Set LRQ to "busy" and load the 8 LSBs of the data into the ALD  */
    /*  reg.  We take the command address, and multiply by 2 bytes to   */
    /*  get the new PC address.                                         */
    /* ---------------------------------------------------------------- */
    sp->lrq = 0;
    sp->ald = data << 4;
    sp->last_allophone = data;
    sp->sby_line = 0;                   /* SET_SBY(0) */
}

int sp0256_lrq(const sp0256_t *sp)
{
    return sp->lrq;
}

int sp0256_sby(const sp0256_t *sp)
{
    return sp->sby_line;
}

uint16_t sp0256_spb640_read(const sp0256_t *sp, unsigned offset)
{
    offset &= 1;

    /* -------------------------------------------------------------------- */
    /*  Offset 0 returns the SP0256 LRQ status on bit 15.                   */
    /* -------------------------------------------------------------------- */
    if (offset == 0)
        return (uint16_t)(sp->lrq << 15);

    /* -------------------------------------------------------------------- */
    /*  Offset 1 returns the SPB640 FIFO full status on bit 15.             */
    /* -------------------------------------------------------------------- */
    return (sp->fifo_head - sp->fifo_tail) >= 64 ? 0x8000 : 0;
}

void sp0256_spb640_write(sp0256_t *sp, unsigned offset, uint16_t data)
{
    offset &= 1;

    if (offset == 0)
    {
        sp0256_write_alp(sp, (uint8_t)(data & 0xff));
        return;
    }

    /* ---------------------------------------------------------------- */
    /*  If Bit 10 is set, reset the FIFO, and SP0256.                   */
    /* ---------------------------------------------------------------- */
    if (data & 0x400)
    {
        sp->fifo_head = sp->fifo_tail = sp->fifo_bitp = 0;
        sp0256_reset(sp);
        return;
    }

    /* ---------------------------------------------------------------- */
    /*  If the FIFO is full, drop the data.                             */
    /* ---------------------------------------------------------------- */
    if ((sp->fifo_head - sp->fifo_tail) >= 64)
        return;

    /* ---------------------------------------------------------------- */
    /*  FIFO up the lower 10 bits of the data.                          */
    /* ---------------------------------------------------------------- */
    sp->fifo[sp->fifo_head++ & 63] = data & 0x3ff;
}

/* ======================================================================== */
/*  Public: The Voice glue (voice.cpp)                                      */
/* ======================================================================== */
int sp0256_o2_t0(const sp0256_t *sp)
{
    return sp->lrq ? 0 : 1;
}

void sp0256_o2_io_write(sp0256_t *sp, uint8_t p1, uint8_t offset, uint8_t data)
{
    if ((offset & 0x80) && !(p1 & 0x10))
    {
        /* D5: 7474 to SP0256B reset */
        const int reset = !(data & 0x20);

        if (reset)
            sp0256_reset(sp);
        else if (!sp->o2_reset_latch)
        {
            /* A0-A6: SP0256B A1-A7 (A8 to GND) */
            sp0256_write_alp(sp, (uint8_t)(offset & 0x7f));
        }

        sp->o2_reset_latch = reset;
    }
}

/* ======================================================================== */
/*  Public: setup                                                           */
/* ======================================================================== */

/* h(d): windowed sinc, |d| < TAPS/2; Blackman window; cutoff = the input
 * Nyquist (fc = 0.5 cycles per input sample). */
static double rs_tap(double d)
{
    const double pi = 3.14159265358979323846;
    const double half = (double)SP0256_RS_TAPS / 2.0;
    double s, w;

    if (d <= -half || d >= half)
        return 0.0;
    s = (d > -1e-12 && d < 1e-12) ? 1.0 : sin(pi * d) / (pi * d);
    w = 0.42 + 0.5 * cos(pi * d / half) + 0.08 * cos(2.0 * pi * d / half);
    return s * w;
}

static void rs_build(sp0256_t *sp)
{
    for (int p = 0; p <= SP0256_RS_PHASES; p++)
    {
        const double frac = (double)p / (double)SP0256_RS_PHASES;
        double sum = 0.0, h[SP0256_RS_TAPS];

        for (int m = 0; m < SP0256_RS_TAPS; m++)
        {
            /* tap m sits at input index (centre - 7 + m); the output is
             * `frac` past the centre sample */
            h[m] = rs_tap((double)(m - (SP0256_RS_TAPS / 2 - 1)) - frac);
            sum += h[m];
        }
        for (int m = 0; m < SP0256_RS_TAPS; m++)
            sp->rs_kernel[p][m] = (float)(h[m] / sum);   /* unity DC gain */
    }
}

/* Silent pre-roll, in input samples, ahead of the first real sample: the
 * resampler needs TAPS/2 samples of lookahead, and the extra keeps a host
 * that renders whole frames from underrunning on rounding. */
#define RS_PREROLL 24u

void sp0256_reset(sp0256_t *sp)
{
    /* reset FIFO and SP0256 */
    sp->fifo_head = sp->fifo_tail = sp->fifo_bitp = 0;

    memset(&sp->filt, 0, sizeof sp->filt);
    sp->halted   = 1;
    sp->filt.rpt = -1;
    sp->filt.rng = 1;
    sp->lrq      = 1;
    sp->ald      = 0;
    sp->pc       = 0;
    sp->stack    = 0;
    sp->fifo_sel = 0;
    sp->mode     = 0;
    sp->page     = 0x1000u << 3;
    sp->silent   = 1;
    sp->sby_line = 1;                   /* SET_SBY(1) (MAME drops it to 0 first so the callback fires) */
    sp->active_allophone = -1;
}

void sp0256_set_frame_hook(sp0256_t *sp, sp0256_frame_fn fn, void *ctx)
{
    sp->frame_hook = fn;
    sp->frame_ctx = ctx;
}

void sp0256_set_clock(sp0256_t *sp, uint32_t hz)
{
    if (hz)
        sp->clock_hz = hz;
}

void sp0256_load(sp0256_t *sp, uint32_t addr, const uint8_t *data, size_t size)
{
    if (!data || addr >= SP0256_ROM_SIZE)
        return;
    if (size > SP0256_ROM_SIZE - addr)
        size = SP0256_ROM_SIZE - addr;
    memcpy(sp->rom + addr, data, size);
}

void sp0256_init(sp0256_t *sp, const uint8_t *rom2k,
                 const uint8_t *speech, size_t speech_size)
{
    memset(sp, 0, sizeof *sp);

    sp->clock_hz = SP0256_O2_CLOCK;

    /* device_start + device_reset */
    sp->sby_line = 1;
    sp->filt.rng = 1;
    sp0256_reset(sp);

    /* ROM: the region image first, then the internal mask ROM over it. */
    sp0256_load(sp, 0, speech, speech_size);
    if (rom2k)
        sp0256_load(sp, SP0256_INTERNAL_BASE, rom2k, SP0256_INTERNAL_SIZE);

    /* debug / glue */
    sp->last_allophone = -1;
    sp->nz_last = -1;

    /* resampler: start RS_PREROLL silent samples before the first real one */
    rs_build(sp);
    sp->ring_head = RS_PREROLL;
    sp->raw_tail  = RS_PREROLL;
    sp->rs_in     = 0;
    sp->rs_rem    = 0;
}

/* ======================================================================== */
/*  Public: output                                                          */
/* ======================================================================== */
size_t sp0256_raw_available(const sp0256_t *sp)
{
    uint64_t n = sp->ring_head - sp->raw_tail;

    return (size_t)(n > SP0256_RING_SIZE ? SP0256_RING_SIZE : n);
}

size_t sp0256_read_raw(sp0256_t *sp, int16_t *dst, size_t max)
{
    size_t n = sp0256_raw_available(sp);

    /* a reader that fell behind by more than the ring loses the oldest */
    if (sp->ring_head - sp->raw_tail > SP0256_RING_SIZE)
        sp->raw_tail = sp->ring_head - SP0256_RING_SIZE;
    if (n > max)
        n = max;
    for (size_t i = 0; i < n; i++)
        dst[i] = sp->ring[(sp->raw_tail + i) & SCBUF_MASK];
    sp->raw_tail += n;
    return n;
}

/* The output at input position (rs_in + rs_rem/K): 16 taps from the
 * kernel table, interpolated between the two nearest phases. K = 312 *
 * 48000: one input sample is K units, one output sample is clock_hz units
 * (clock / (312 * 48000) = input rate / 48000, exactly rational: at
 * 3.12 MHz 5/24, i.e. a phase step of 25 in 120, so no interpolation
 * between phases is needed at the default clock). */
size_t sp0256_render_f32(sp0256_t *sp, float *dst, size_t n)
{
    const uint64_t K = (uint64_t)SP0256_CLOCK_DIVIDER * SP0256_OUT_HZ;
    const int back = SP0256_RS_TAPS / 2 - 1;     /* taps before the centre */
    const int ahead = SP0256_RS_TAPS / 2;        /* taps after it */
    size_t made = 0;

    /* the resampler fell more than a ring behind: jump to the oldest data */
    if ((int64_t)sp->rs_in - back < (int64_t)sp->ring_head - (int64_t)SP0256_RING_SIZE)
    {
        sp->rs_in = sp->ring_head - SP0256_RING_SIZE + (uint64_t)back;
        sp->rs_rem = 0;
    }

    while (made < n)
    {
        /* needs input indices rs_in-7 .. rs_in+8 */
        if (sp->rs_in + (uint64_t)ahead >= sp->ring_head)
            break;

        const int64_t first = (int64_t)sp->rs_in - back;
        float out = 0.0f;

        /* A window of zero samples is silence: skip the filter. That is so
         * when the newest non-zero sample came before the window. */
        if (sp->nz_last >= first)
        {
            const uint64_t t = (uint64_t)sp->rs_rem * SP0256_RS_PHASES;
            const unsigned ph = (unsigned)(t / K);
            const float w = (float)(t % K) / (float)K;
            const float *k0 = sp->rs_kernel[ph];
            float x[SP0256_RS_TAPS], a0 = 0.0f;

            for (int m = 0; m < SP0256_RS_TAPS; m++)
            {
                const int64_t idx = first + m;   /* negative: before the start */

                x[m] = idx < 0 ? 0.0f : (float)sp->ring[(uint64_t)idx & SCBUF_MASK];
                a0 += x[m] * k0[m];
            }
            if (w != 0.0f)
            {
                const float *k1 = sp->rs_kernel[ph + 1];
                float a1 = 0.0f;

                for (int m = 0; m < SP0256_RS_TAPS; m++)
                    a1 += x[m] * k1[m];
                a0 += w * (a1 - a0);
            }
            out = a0 * (1.0f / 32768.0f);
        }
        dst[made++] = out;

        /* advance by one output sample */
        sp->rs_rem += sp->clock_hz;
        while (sp->rs_rem >= K)
        {
            sp->rs_rem -= (uint32_t)K;
            sp->rs_in++;
        }
    }

    for (size_t i = made; i < n; i++)
        dst[i] = 0.0f;
    return made;
}

/* ======================================================================== */
/*  Public: debugger                                                        */
/* ======================================================================== */
void sp0256_debug_get(const sp0256_t *sp, sp0256_debug_t *d)
{
    int32_t cnt = (int32_t)(sp->fifo_head - sp->fifo_tail);

    memset(d, 0, sizeof *d);
    d->rom_addr  = (sp->pc >> 3) & 0xffff;
    d->rom_bit   = sp->pc & 7;
    d->fifo_sel  = sp->fifo_sel;
    d->halted    = sp->halted;
    d->lrq       = sp->lrq;
    d->sby       = sp->sby_line;
    d->allophone = sp->active_allophone;
    d->pending   = sp->lrq ? -1 : ((sp->ald >> 4) & 0xff);
    d->last_allophone = sp->last_allophone;
    d->mode      = sp->mode;
    d->page      = (sp->page >> 3) & 0xffff;
    d->stack     = (sp->stack >> 3) & 0xffff;
    d->rpt       = sp->filt.rpt;
    d->cnt       = sp->filt.cnt;
    d->period    = sp->filt.per;
    d->amp       = sp->filt.amp;
    d->interp    = sp->filt.interp;
    d->silent    = sp->silent;
    d->noise     = sp->filt.per == 0;
    memcpy(d->r, sp->filt.r, sizeof d->r);
    memcpy(d->b_coef, sp->filt.b_coef, sizeof d->b_coef);
    memcpy(d->f_coef, sp->filt.f_coef, sizeof d->f_coef);
    memcpy(d->z_data, sp->filt.z_data, sizeof d->z_data);

    if (cnt < 0) cnt = 0;
    if (cnt > 64) cnt = 64;
    d->fifo_count = (unsigned)cnt;
    d->fifo_bitp  = sp->fifo_bitp;
    for (int32_t i = 0; i < cnt; i++)
        d->fifo[i] = sp->fifo[(sp->fifo_tail + (uint32_t)i) & 63];

    d->samples_made  = sp->samples_made;
    d->alds_dropped  = sp->alds_dropped;
    d->last_sample   = sp->last_sample;
    d->clock_hz      = sp->clock_hz;
    d->rate_hz       = sp->clock_hz / SP0256_CLOCK_DIVIDER;
}

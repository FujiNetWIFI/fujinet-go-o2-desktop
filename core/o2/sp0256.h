/* sp0256.h -- the General Instrument SP0256 Narrator Speech Processor
 * (The Voice, Odyssey 2 / Videopac).
 *
 * Transposed from MAME's src/devices/sound/sp0256.cpp and sp0256.h
 * (BSD-3-Clause, copyright-holders Joseph Zbiciak and Tim Lindner). MAME's
 * file is itself a port of Joseph Zbiciak's jzIntv Intellivoice emulation
 * (src/ivoice/ivoice.c); the algorithm -- the bit-serial microsequencer, the
 * LPC register decode and the 12-pole (six biquad) integer filter -- is his.
 *
 * What is the same as MAME, line for line, with integer arithmetic throughout:
 *   - the microsequencer (getb, SETPAGE, RTS/HLT, JMP, JSR, SETMODE, the
 *     LOADALL / LOAD_2 / LOAD_4 / LOAD_C / LOAD_E / SETMSB_* / DELTA_* /
 *     PAUSE data formats, the 177-entry control-word table),
 *   - the register decode (the coefficient quantiser table, amplitude and
 *     period decode, interpolation) and the filter, including MAME's
 *     details that a reader might take for bugs: the pitch period is PR + 1
 *     samples, the noise generator only steps in noise mode (PR = 0) and
 *     keeps stepping while the chip idles, the delay line is cleared at each
 *     pitch impulse, an idle chip keeps playing the last frame's noise,
 *   - the ALD latch (LRQ, drop-when-busy), standby, the SPB640 FIFO,
 *   - the 16-bit sample scaling (limit(s) << 2: MAME's HIGH_QUALITY).
 *
 * What is different (each is marked DEVIATION or commented in sp0256.c):
 *   - No emu_timer, sound_stream or device callbacks. The chip is a plain
 *     struct driven by sp0256_advance()/sp0256_sync_to(), in units of its
 *     own clock; LRQ and SBY are queried instead of reported by callbacks.
 *   - MAME's 4096-sample scratch ring has a latent defect that this core
 *     does not reproduce. Its free-space test, m_sc_head + do_samp -
 *     m_sc_tail, is unsigned and is made after m_sc_head was wrapped, so
 *     once an update straddles the 4096-sample wrap the sum underflows,
 *     do_samp becomes ~8000 and lpc12_t::update() runs on past the end of
 *     the requested block: the chip's state gets ahead of real time and the
 *     surplus samples are played later, after whatever happens next (for
 *     example after a reset). Here the samples go straight into the ring and
 *     the chip never runs ahead. Because the sample stream is otherwise
 *     independent of how a run is chunked (tested), this only matters for
 *     comparisons against a MAME that hit the defect; the 60 s real-MAME
 *     sweep this core was checked against did not show it.
 *   - A full ring drops its oldest sample (MAME would stall the stream).
 *   - One sequencer call executes at most SP0256_MICRO_BUDGET instructions.
 *     MAME hangs forever on a ROM that jumps in a loop without producing
 *     sound or halting (a wrong or corrupt image); the real chip loops while
 *     time passes. Here the chip goes silent and resumes where it was at the
 *     next sample. Unreachable on a good ROM.
 *   - sp0256_set_frame_hook() (debugger / tests) and the O2 glue
 *     sp0256_o2_*() (The Voice's address decode) are additions.
 *   - sp0256_render_f32() resamples the chip's 10 kHz to 48 kHz mono float
 *     with a 16-tap windowed-sinc polyphase filter (flat to 3.5 kHz, images
 *     >= 58 dB down for tones up to 3.4 kHz; exactly 24 outputs per 5 inputs
 *     at 3.12 MHz). MAME leaves this to its sound system's resampler; the
 *     chip's own 10 kHz stream, sp0256_read_raw(), is MAME's exactly.
 *
 * TIME. The chip's sample rate is clock / 312 (3.12 MHz / 312 = 10 kHz
 * exactly). Nothing changes between samples, so the host never needs to run
 * it per clock: call sp0256_sync_to(sp, chip_clock_now) before anything that
 * observes or changes the chip (LRQ/SBY query -- on the Odyssey 2 that is a
 * T0 read -- an ALD write, a reset) and again at the end of each emulated
 * video frame before sp0256_render_f32(). One call generates all the 10 kHz
 * samples due since the last one (about 167 per NTSC frame) at roughly 9 ns
 * each. The chip's clock (3.12 MHz crystal on The Voice) is independent of
 * the console's; the host converts its own time base to chip clocks (chip =
 * cpu_cycles * 3120000 / cpu_cycle_rate, kept as a running 64-bit count so
 * no rounding accumulates). The chip makes no sound and does no work other
 * than that while idle -- there is nothing to "stop" when no cartridge uses
 * speech; simply do not create or clock it.
 *
 * MEMORY. The chip addresses a 64 KB space of bytes (MAME's "speech" region):
 * the 2 KB internal mask ROM at $1000-$17FF, $1800 is the FIFO window, and an
 * external speech ROM (the Voice's SPR128-003, 16 KB) wherever the board maps
 * it. On The Voice (MAME hash/videopac.xml): sp0256b-019.bin at $1000,
 * spr128-003.bin at $4000-$7FFF, and a plugged-in cartridge's own speech ROM
 * (e.g. Sid the Spellbinder's spr128-004.bin) at $8000-$BFFF; sp0256_init()
 * and sp0256_load() take exactly those layouts. Bytes nothing maps read 0.
 * The ROM is copied into the struct, so the caller's buffers need not outlive
 * the call.
 *
 * Several instances may coexist; there are no globals and nothing allocates.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/sound/sp0256.cpp; copyright-holders
 * Joseph Zbiciak, Tim Lindner -- see COMPLIANCE.md)
 */

#ifndef O2_SP0256_H
#define O2_SP0256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP0256_CLOCK_DIVIDER  312u        /* 6 * 4 * 13 (MAME CLOCK_DIVIDER) */
#define SP0256_O2_CLOCK       3120000u    /* The Voice's crystal, Hz */
#define SP0256_RATE_HZ        10000u      /* clock / 312 at 3.12 MHz */
#define SP0256_OUT_HZ         48000u      /* sp0256_render_f32()'s rate */
#define SP0256_ROM_SIZE       0x10000u    /* the chip's byte address space */
#define SP0256_INTERNAL_BASE  0x1000u     /* internal 2 KB mask ROM */
#define SP0256_INTERNAL_SIZE  0x0800u
#define SP0256_FIFO_BASE      0x1800u     /* SPB640 FIFO window (byte addr) */
#define SP0256_RING_SIZE      4096u       /* raw-sample ring; power of two */
#define SP0256_MICRO_BUDGET   65536       /* instructions per sequencer call */

/* resampler geometry (see sp0256.c) */
#define SP0256_RS_TAPS        16
#define SP0256_RS_PHASES      120

/* One filter state, MAME's lpc12_t. */
typedef struct sp0256_lpc12 {
    int      rpt, cnt;        /* repeat counter, period down-counter */
    uint32_t per, rng;        /* period, random number generator */
    int      amp;             /* amplitude */
    int16_t  f_coef[6];       /* F0..F5 */
    int16_t  b_coef[6];       /* B0..B5 */
    int16_t  z_data[6][2];    /* time-delay data of the six stages */
    uint8_t  r[16];           /* the encoded register set */
    int      interp;          /* interpolation parameters present */
} sp0256_lpc12_t;

/* Called each time the microsequencer hands the filter a new register set (an
 * LPC "frame"): `filt` holds the decoded registers and coefficients, `repeat`
 * is the repeat count the instruction carried (the filter plays it for
 * repeat + 1 pitch periods). Debugger / test hook; NULL = none. */
typedef void (*sp0256_frame_fn)(void *ctx, const sp0256_lpc12_t *filt, int repeat);

typedef struct sp0256 {
    /* ---- the chip (MAME's sp0256_device members) ---- */
    uint8_t  rom[SP0256_ROM_SIZE];  /* "speech" region, byte addresses */
    int      sby_line;        /* standby line state (1 = standing by) */
    int      silent;          /* flag: silent */
    sp0256_lpc12_t filt;      /* 12-pole filter */
    int      lrq;             /* load request: 1 = can accept a load */
    int      ald;             /* address-load latch (bit address offset) */
    uint32_t pc;              /* microsequencer PC, in BITS */
    uint32_t stack;           /* the one-deep PC stack */
    int      fifo_sel;        /* executing from the FIFO */
    int      halted;          /* microsequencer halted */
    uint32_t mode;            /* mode register */
    uint32_t page;            /* page set by SETPAGE (bit address) */
    uint32_t fifo_head;       /* FIFO head (where new data goes) */
    uint32_t fifo_tail;       /* FIFO tail (where data comes from) */
    uint32_t fifo_bitp;       /* FIFO bit pointer (partial decles) */
    uint16_t fifo[64];        /* the 64-decle SPB640 FIFO */

    /* ---- time ---- */
    uint32_t clock_hz;        /* chip clock; sample rate = clock / 312 */
    uint64_t clk_total;       /* chip clocks consumed so far */
    uint32_t clk_frac;        /* clocks since the last whole sample (< 312) */

    /* ---- the raw 10 kHz output ring (MAME's scratch buffer) ---- */
    int16_t  ring[SP0256_RING_SIZE];
    uint64_t ring_head;       /* samples generated since init */
    uint64_t raw_tail;        /* sp0256_read_raw()'s read position */
    int64_t  nz_last;         /* index of the last non-zero sample, -1 none */

    /* ---- 48 kHz resampler (sp0256_render_f32) ---- */
    uint64_t rs_in;           /* input index the next output is centred on */
    uint32_t rs_rem;          /* sub-sample position, units of 1/(312*48000) */
    float    rs_kernel[SP0256_RS_PHASES + 1][SP0256_RS_TAPS];

    /* ---- debugger / glue state (not chip state) ---- */
    sp0256_frame_fn frame_hook;
    void    *frame_ctx;
    int      active_allophone;/* ALD value being executed, -1 none */
    int      last_allophone;  /* last ALD value accepted, -1 none */
    int      o2_reset_latch;  /* The Voice's 7474 (voice.cpp m_reset) */
    uint64_t samples_made;    /* total samples generated */
    uint64_t alds_dropped;    /* ALD writes dropped because busy */
    int16_t  last_sample;
} sp0256_t;

/* ---- setup ---- */

/* Zero the struct and power it on. rom2k: the 2048-byte internal mask ROM
 * (copied to $1000), may be NULL. speech: an image of the chip's address
 * space as MAME's "speech" region is laid out (byte i = chip address i),
 * speech_size bytes of it (at most 65536), may be NULL; it is copied first,
 * then rom2k over $1000-$17FF. */
void sp0256_init(sp0256_t *sp, const uint8_t *rom2k,
                 const uint8_t *speech, size_t speech_size);

/* Copy bytes into the chip's address space (an extra speech ROM from a
 * plugged-in cartridge goes to $8000). Out-of-range bytes are dropped. */
void sp0256_load(sp0256_t *sp, uint32_t addr, const uint8_t *data, size_t size);

/* Chip clock in Hz (default 3,120,000). Sample rate = clock / 312. */
void sp0256_set_clock(sp0256_t *sp, uint32_t hz);

/* Install (or with fn == NULL remove) the frame hook; see sp0256_frame_fn. */
void sp0256_set_frame_hook(sp0256_t *sp, sp0256_frame_fn fn, void *ctx);

/* MAME device_reset: FIFO, filter, sequencer cleared, LRQ high, standby
 * high. Does not touch the clock or the pending output. Sync first. */
void sp0256_reset(sp0256_t *sp);

/* ---- time ---- */

/* Advance by `clocks` chip clocks, generating every whole sample that falls
 * due. */
void sp0256_advance(sp0256_t *sp, uint64_t clocks);

/* Advance to the absolute chip clock count `clock_now` (a no-op if the chip
 * is already there). Use this from a machine that keeps one running count. */
void sp0256_sync_to(sp0256_t *sp, uint64_t clock_now);

/* Generate `n` samples regardless of the clock (the clock accumulator is not
 * touched). For tests and offline rendering. */
void sp0256_run_samples(sp0256_t *sp, uint32_t n);

/* ---- pins (call sp0256_sync_to first) ---- */

/* ald_w: latch an allophone address. Dropped if LRQ is low (busy), as the
 * chip does. The chip starts it at the next microsequencer step. */
void sp0256_write_alp(sp0256_t *sp, uint8_t value);

/* lrq_r: 1 = the chip can accept an address (!LRQ low on the pin). */
int sp0256_lrq(const sp0256_t *sp);

/* sby_r: 1 = standby (idle), 0 = speaking. */
int sp0256_sby(const sp0256_t *sp);

/* spb640_r / spb640_w: the Intellivoice's two-word interface (not used on
 * the Odyssey 2, kept because the chip executes from the FIFO). Offset 0:
 * read LRQ << 15, write ALD. Offset 1: read FIFO-full << 15, write a decle
 * (bit 10 set resets the FIFO and the chip). */
uint16_t sp0256_spb640_read(const sp0256_t *sp, unsigned offset);
void     sp0256_spb640_write(sp0256_t *sp, unsigned offset, uint16_t data);

/* ---- The Voice glue (MAME src/devices/bus/odyssey2/voice.cpp) ---- */

/* T0 as the console sees it: 1 while the chip is busy (t0_read: lrq ? 0 : 1). */
int sp0256_o2_t0(const sp0256_t *sp);

/* An external-memory write by the console (MOVX @Rr,A with P16 low -- the
 * console does not pass writes on with P16 high): `offset` is the low address
 * byte (A0-A7), `data` the bus, `p1` the console's P1 latch (only bit 4 is
 * used; the console hands carts P1 & 0x13). With P1.4 low and A7 set the
 * write is for the chip: D5 low holds it in reset, D5 high releases it, and
 * the first write after a release latches A0-A6 as the allophone address
 * (voice.cpp io_write; the 7474 latch is sp->o2_reset_latch). Sync first. */
void sp0256_o2_io_write(sp0256_t *sp, uint8_t p1, uint8_t offset, uint8_t data);

/* ---- output ---- */

/* Raw samples (signed 16-bit, 10 kHz at the default clock): copy up to `max`
 * unread samples to `dst`; returns the count. A reader that falls more than
 * SP0256_RING_SIZE behind loses the oldest. */
size_t sp0256_read_raw(sp0256_t *sp, int16_t *dst, size_t max);

/* Raw samples waiting for sp0256_read_raw(). */
size_t sp0256_raw_available(const sp0256_t *sp);

/* 48 kHz mono float (full scale +-1.0 = the raw int16 / 32768, MAME's
 * put_int(..., 32768)). Writes exactly `n` samples to `dst` (overwriting,
 * the caller mixes); returns how many were computed from chip output. The
 * resampler lags the chip by 8 input samples (0.8 ms); if the chip has not
 * been advanced far enough the missing tail is zero-filled and the
 * resampler does not move, so call sp0256_sync_to() for the same emulated
 * time first. Independent of sp0256_read_raw(). */
size_t sp0256_render_f32(sp0256_t *sp, float *dst, size_t n);

/* ---- debugger ---- */

typedef struct sp0256_debug {
    uint32_t rom_addr;        /* byte address of the next fetch (pc >> 3) */
    uint32_t rom_bit;         /* bit within that byte (pc & 7) */
    int      fifo_sel;        /* fetching from the FIFO, not ROM */
    int      halted;
    int      lrq, sby;
    int      allophone;       /* ALD value being spoken, -1 none */
    int      pending;         /* ALD value latched but not started, -1 none */
    int      last_allophone;  /* last ALD value accepted, -1 none */
    uint32_t mode, page, stack;      /* page/stack as byte addresses */
    int      rpt, cnt;        /* repeat count, period down-counter */
    uint32_t period;          /* current period (samples = period + 1) */
    int      amp;             /* current amplitude */
    int      interp;
    int      silent;          /* the filter is in a PAUSE / cleared */
    int      noise;           /* period 0: noise source in use */
    uint8_t  r[16];           /* register set: AM PR B0 F0 .. B5 F5 IA IP */
    int16_t  b_coef[6], f_coef[6];
    int16_t  z_data[6][2];
    unsigned fifo_count;      /* decles in the FIFO */
    unsigned fifo_bitp;
    uint16_t fifo[64];        /* contents, oldest first, fifo_count valid */
    uint64_t samples_made;
    uint64_t alds_dropped;
    int16_t  last_sample;
    uint32_t clock_hz;
    uint32_t rate_hz;         /* clock / 312 */
} sp0256_debug_t;

void sp0256_debug_get(const sp0256_t *sp, sp0256_debug_t *out);

#ifdef __cplusplus
}
#endif

#endif /* O2_SP0256_H */

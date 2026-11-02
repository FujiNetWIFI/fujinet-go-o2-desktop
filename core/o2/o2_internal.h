/* o2_internal.h -- the emulated machine, shared by the core modules.
 *
 * The machine logic is transposed from MAME's Odyssey 2 driver
 * (BSD-3-Clause, copyright-holders Peter Trauner, Wilbert Pol, hap; see
 * COMPLIANCE.md):
 *
 *   src/mame/philips/odyssey2.cpp       the program and I/O maps, the P1/P2
 *                                       decode, the keyboard matrix and the
 *                                       joysticks, T1 = vblank | hblank, the
 *                                       Videopac+ G7400's EF9340 and 8243
 *   src/devices/cpu/mcs48/              the 8048 (mcs48.c)
 *   src/devices/video/i8244.cpp         the 8244/8245 video + sound (i8244.c)
 *   src/devices/bus/odyssey2/           the cartridge slot (carts.c) and The
 *                                       Voice (voice.c, sp0256.c)
 *
 * Field names keep MAME's own (minus the m_ prefix) where there is one.
 *
 * MAME runs the CPU and the VDC's timers in its scheduler; here the machine
 * runs the CPU one instruction at a time and brings the VDC up to the CPU's
 * clock first (i8244.h: "HOW TO DRIVE IT"), which is what MAME's timeslicing
 * amounts to with both clocks derived from one crystal. Absolute time is
 * counted in pixel clocks (20 per NTSC machine cycle, 18 per PAL one); the
 * beam's origin is power-on, as in MAME.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_INTERNAL_H
#define O2_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "carts.h"
#include "ef9340.h"
#include "fujinet_cart.h"
#include "i8243.h"
#include "i8244.h"
#include "mcs48.h"
#include "o2session.h"      /* o2_machine, O2_MISSING_* */
#include "sp0256.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the picture: MAME's visible area, 360 x 243 ---- */
#define O2_FB_WIDTH       I8244_WIDTH
#define O2_FB_HEIGHT      I8244_HEIGHT

/* ---- clocks (odyssey2.cpp) ----
 * The 8048 runs at clock/15: Odyssey 2 (7.15909 MHz * 3/4) / 15 = 357,955 Hz
 * a machine cycle; Videopac (17.734476 MHz / 3) / 15 = 394,100 Hz. */
#define O2_NTSC_CYCLE_HZ  (7159090.0 * 3.0 / 4.0 / 15.0)
#define O2_PAL_CYCLE_HZ   (17734476.0 / 3.0 / 15.0)

/* ---- memory ---- */
#define O2_BIOS_SIZE      0x400         /* the 8048's internal ROM */
#define O2_RAM_SIZE       0x80          /* the 6810 on the bus: A7 low */

/* ---- the no-BIOS stand-in (stub.a48) ---- */
#define O2_STUB_SIZE      0x400
extern const uint8_t o2_stub[O2_STUB_SIZE];

/* ---- sound ---- */
#define O2_AUDIO_RATE     48000
#define O2_AUDIO_MAX      2048          /* stereo frames buffered per frame */
/* MAME's final output level is quiet (I8244_SOUND_GAIN); the host brings it
 * up. The tests that compare with a MAME recording set audio_gain to 1. */
#define O2_AUDIO_BOOST    2.5f

typedef struct o2_machine o2_machine_t;

/* The console's keyboard: bit (row * 8 + col) of a 64-bit matrix. */
#define O2_KEYS_MASK_O2   0x0000ffffffffffffull   /* rows 0-5 */

struct o2_machine {
    mcs48_t cpu;
    i8244_t vdc;
    int machine;                  /* O2_MACHINE_* */
    int is_pal;
    int cycle_clocks;             /* pixel clocks per machine cycle: 20 / 18 */
    double cycle_hz;
    double pixel_hz;

    /* the Videopac+ G7400's text chip and expander (machine == VIDEOPAC_PLUS) */
    bool have_ef;
    ef9340_t ef;
    i8243_t expander;
    uint8_t mix_i8244, mix_ef934x;        /* the colour mix latches (P4-P7) */

    /* The Voice, in front of the cartridge (voice.c) */
    bool have_voice;
    sp0256_t speech;
    o2_cart_t *voice_cart;
    uint64_t speech_t;                    /* pixel clocks the chip has run to */

    /* memory */
    uint8_t rom[O2_BIOS_SIZE];            /* the BIOS, or the stand-in */
    bool have_bios;                       /* the real BIOS is in rom[] */
    bool stub_parked;                     /* the stand-in met a game it cannot run */
    uint8_t ram[O2_RAM_SIZE];

    /* the slot: the cartridge in it (a mapper, or the FujiNet cart), and the
     * FujiNet cart itself when it is the one (NULL for a game) */
    o2_cart_t *cart;
    o2_fujinet_t *fujinet;
    unsigned fj_swaps;                    /* swaps seen: to catch a game arriving */

    /* the ports: the latches as MAME's driver keeps them */
    uint8_t p1, p2;

    /* inputs: live = set by any thread; the rest latched at each frame */
    uint64_t keys_live, keys;
    uint8_t joy_live[2], joy[2];
    bool reset_live, reset_held;

    /* audio, mono from the VDC (and the Voice) mixed up to stereo float at
     * O2_AUDIO_RATE; one frame's worth per run_frame */
    float audio_gain;
    uint64_t audio_t;                     /* the VDC time the samples run to */
    double audio_frac;                    /* fractional sample carried over */
    uint64_t audio_samples;               /* samples produced since power-on */
    float out[2 * O2_AUDIO_MAX];
    int out_frames;

    /* the picture the last run_frame finished */
    uint32_t fb[O2_FB_WIDTH * O2_FB_HEIGHT];
    uint64_t frame_count;

    /* ---- debugger ----
     * instr_hook fires at every instruction boundary on the emulation
     * thread, BEFORE the instruction; it may block (that is how the debugger
     * pauses the machine). bus_hook fires on memory and I/O accesses, only
     * while `watch` is nonzero. */
    void (*instr_hook)(o2_machine_t *m, void *user);
    void *instr_hook_user;
    void (*bus_hook)(o2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data);
    void *bus_hook_user;
    uint8_t watch;                /* O2_WATCH_* */
};

#define O2_WATCH_MEMR 0x01        /* program-space data reads (MOVP) */
#define O2_WATCH_XR   0x02        /* external data reads (MOVX) */
#define O2_WATCH_XW   0x04        /* external data writes */
#define O2_WATCH_P1   0x08        /* port writes */
#define O2_WATCH_P2   0x10
#define O2_WATCH_BUS  0x20
#define O2_WATCH_IR   0x40        /* internal RAM (MOV A,@Rn / MOV @Rn,A / reg access): not watched yet */
#define O2_WATCH_IW   0x80

#define O2_BUS_MEMR  1
#define O2_BUS_XR    2
#define O2_BUS_XW    3
#define O2_BUS_P1W   4
#define O2_BUS_P2W   5
#define O2_BUS_BUSW  6
#define O2_BUS_P1R   7
#define O2_BUS_P2R   8
#define O2_BUS_BUSR  9

/* machine.c */
typedef struct {
    int machine;
    const uint8_t *bios;          /* 1 KB or NULL (the stand-in) */
    const uint8_t *font;          /* 512 B or NULL */
    const uint8_t *font_ef;       /* 0xA00 B or NULL (G7400) */
    bool voice;
    const uint8_t *voice_rom;     /* 2 KB */
    const uint8_t *voice_speech;  /* 16 KB */
    const uint8_t *voice_game;    /* 16 KB or NULL */
    o2_cart_t *cart;              /* the cartridge in the slot (not owned) */
    o2_fujinet_t *fujinet;        /* ... and the FujiNet cart when it is that one */
} o2_machine_opts_t;

/* Power on. The machine does not own the cart; the caller powers it on first. */
void o2_machine_init(o2_machine_t *m, const o2_machine_opts_t *opts);
/* Run until the VDC publishes its next frame; m->fb holds it, m->out the
 * audio of the time that took. */
void o2_machine_run_frame(o2_machine_t *m);
/* One instruction, hooks and all. Returns the machine cycles it took. */
int o2_machine_step(o2_machine_t *m);
/* One instruction with no debugger hooks (the debugger's own stepping). */
int o2_machine_step_quiet(o2_machine_t *m);
/* Latch the live inputs (the frame boundary): the keyboard, the sticks and
 * RESET -- pressing RESET resets the 8048 (only), releasing it lets it run
 * from $000. */
void o2_machine_latch_inputs(o2_machine_t *m);
/* The picture as it stands now (the debugger shows a frame being drawn). */
void o2_machine_snapshot(o2_machine_t *m, uint32_t *dst);
/* Machine time, in pixel clocks. */
static inline uint64_t o2_machine_now(const o2_machine_t *m)
{
    return m->cpu.cycles * (uint64_t)m->cycle_clocks;
}
/* Debugger access: peek never disturbs the machine (no mailbox hotspot, no VDC
 * read side effect); poke writes RAM, the cartridge's own buffers, or a VDC
 * register (ignoring its write lockouts). space is o2debug_space. */
uint8_t o2_machine_peek(o2_machine_t *m, int space, uint16_t addr);
void o2_machine_poke(o2_machine_t *m, int space, uint16_t addr, uint8_t data);
/* The keyboard matrix row the console would read for `sel` (0-7). */
uint8_t o2_machine_key_row(const o2_machine_t *m, int sel);
/* What the running configuration lacks (O2_MISSING_*), from the machine's own
 * point of view: the stand-in met a game; the font was not given. */
unsigned o2_machine_missing(const o2_machine_t *m);

#ifdef __cplusplus
}
#endif

#endif /* O2_INTERNAL_H */

/*
 * host.h -- the emulator host: owns the machine and the FujiNet cartridge,
 * runs the machine on its own thread at real speed, and publishes video
 * frames and audio for the session layer.
 *
 * Process-singleton by design, like every core in this family (and the
 * FujiNet cart below it is one too: fujimail's port interface is context-
 * free C function pointers). o2_host_start refuses a second concurrent
 * session.
 *
 * Pacing: the machine runs a whole frame (to the next VBLANK) at a time
 * against an absolute monotonic deadline ladder at the machine's exact frame
 * rate -- NTSC 59.83 Hz, PAL 49.70 Hz. While a frontend feeds display ticks
 * at that rate (o2_host_notify_vsync) the machine phase-locks one frame per
 * tick instead (only possible near 60 Hz: a 50 Hz machine on a 60 Hz display
 * stays on the wall clock). The siblings' hardest-won lesson applies: VERIFY
 * the pacing -- the boot_smoke test measures it.
 *
 * Energy: a frame identical to its predecessor is not republished, the frame
 * callback tells a frontend when there is something to draw, and the machine
 * sleeps to its deadline rather than spinning.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_HOST_H
#define O2_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "o2_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int machine;                 /* O2_MACHINE_* (core/o2/o2_internal.h) */

    /* ROMs, each copied; NULL = not imported. */
    const uint8_t *bios;         /* the console's 1 KB system ROM; NULL runs the
                                    no-BIOS stand-in (Odyssey 2 / G7000 only) */
    uint32_t bios_size;
    const uint8_t *font;         /* the 8244/8245's 512-byte character set */
    uint32_t font_size;
    const uint8_t *font_ef;      /* the EF9340's 0xA00-byte character set */
    uint32_t font_ef_size;
    bool voice;                  /* The Voice in front of the cartridge */
    const uint8_t *voice_rom;    /* sp0256b-019.bin (2 KB) */
    uint32_t voice_rom_size;
    const uint8_t *voice_speech; /* spr128-003.bin (16 KB) */
    uint32_t voice_speech_size;
    const uint8_t *voice_game;   /* a game's own speech ROM (spr128-004.bin), or NULL */
    uint32_t voice_game_size;

    const uint8_t *cart;         /* an opened image, copied; NULL boots CONFIG */
    uint32_t cart_size;
    int cart_type;               /* O2_CART_* (carts.h) from o2_cart_identify */
    bool cart_app;               /* a FujiNet app (claims the mailbox): served
                                    by the FujiNet cartridge, mailbox live.
                                    Otherwise a game: the mapper cart for
                                    cart_type sits in the slot instead, as a
                                    real cartridge would, and FujiNet is not
                                    reachable until the next power cycle. */
    const char *boip_hostport;   /* fujinet-pc's BoIP listener, "host:port" */
    bool cart_sync;              /* run the mailbox inline (tests) */
} o2_host_opts_t;

/* Start the machine thread. Returns 0, or -1 with o2_host_last_error()
 * set (already running, an image the cartridge cannot map, allocation or
 * thread failure). */
int o2_host_start(const o2_host_opts_t *opts);

/* Stop and join the machine thread and power the cart off. Safe when not
 * running. */
void o2_host_stop(void);

bool o2_host_is_running(void);
const char *o2_host_last_error(void);

/* Pause: the machine thread parks between frames and the audio goes silent
 * (the session's pause; the debugger's own stop is separate). */
void o2_host_set_paused(bool paused);
bool o2_host_is_paused(void);

/* ---- video: latest-wins single slot ----
 * Copies the newest frame into dst (O2_FB_WIDTH x O2_FB_HEIGHT XRGB words)
 * when its serial differs from *serial_inout; returns nonzero and updates the
 * serial when it copied. */
int o2_host_frame_copy(uint32_t *dst, uint64_t *serial_inout);
/* Called on the machine thread right after each new frame is published, so a
 * frontend with no frame clock can draw when there is something to draw
 * instead of polling. Must be quick and must not call back into the host (it
 * runs with a lock held: set_frame_callback(NULL, NULL) waits for a call in
 * flight, so the context may be freed once it returns). */
typedef void (*o2_host_frame_cb)(void *ctx);
void o2_host_set_frame_callback(o2_host_frame_cb cb, void *ctx);
/* Frames the machine has run since start (for pacing checks). */
uint64_t o2_host_frame_count(void);
/* How the deadline ladder fared (boot_smoke prints it): sleeps taken, how
 * late they woke in total and at worst, and resyncs after falling more
 * than four frames behind. Read after o2_host_stop. */
typedef struct {
    uint64_t sleeps, late_ns, resyncs;
    long worst_late_ns;
} o2_host_pacing_t;
void o2_host_pacing(o2_host_pacing_t *out);
/* The running machine's exact frame rate in Hz (0 when stopped). */
double o2_host_frame_rate(void);

void o2_host_notify_vsync(int64_t frame_time_ns);

/* ---- audio: 48 kHz interleaved stereo float (mono mirrored), self-
 * correcting backlog. Returns frames written (no padding). */
int o2_host_audio_copy(float *dst, int max_frames);
void o2_host_audio_reset(void);

/* ---- inputs (callable from any thread; latched by the machine once a
 * frame, at VBLANK, as MAME's input ports are) ---- */
/* The keyboard matrix: bit (row * 8 + col) set = that key held (0-47 on the
 * Odyssey 2; 0-63 on the G7400). */
void o2_host_keys_set(uint64_t matrix);
/* A stick: bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 BUTTON (MAME's JOY bits). */
void o2_host_stick_set(int stick, uint8_t bits);
/* The console's RESET key (resets the 8048 while held). */
void o2_host_reset_set(bool down);
/* Pressed for a few frames and released (the menus' and the controllers
 * window's RESET when it is clicked rather than held). */
void o2_host_reset_pulse(void);
void o2_host_release_all(void);

/* What the running configuration is missing (O2_MISSING_* in o2session.h:
 * BIOS 1, font 2, voice 4): a game met the stand-in, a font was not imported,
 * the Voice has no speech ROM. */
unsigned o2_host_missing(void);

/* ---- the cartridge ---- */
void o2_host_cart_status(o2_cart_status_t *out);
/* The cart device, or NULL when not running (the debugger's page map and
 * mailbox views). Use it under o2_host_lock. */
o2_cart_t *o2_host_cart(void);

/* ---- the run lock ----
 * Held by the machine thread while it runs a frame. Any other thread that
 * reads or edits the machine or the cart (o2_host_machine, o2_host_cart)
 * holds it around the access: it waits for the frame in progress, at most a
 * couple of milliseconds. Re-entrant per thread; a no-op on the machine
 * thread. Never call o2_host_stop while holding it. */
void o2_host_lock(void);
void o2_host_unlock(void);
/* The debugger's park: the machine thread lets go of the run lock while it
 * is parked inside the instruction hook, and takes it back to run on. */
void o2_host_park_release(void);
void o2_host_park_reacquire(void);

/* ---- debugger plumbing ----
 * The live machine; valid only while running, and only touched under
 * o2_host_lock (or on the machine thread). */
o2_machine_t *o2_host_machine(void);

/* Instruction-boundary hook (machine thread; may block). */
void o2_host_set_instr_hook(void (*hook)(o2_machine_t *m, void *user), void *user);
/* Bus hook, fired only for the access kinds in `watch` (O2_WATCH_*). */
void o2_host_set_bus_hook(void (*hook)(o2_machine_t *m, void *user, int kind,
                                        uint16_t addr, uint8_t data),
                           void *user, uint8_t watch);
/* Publish the machine's current framebuffer now (the debugger shows the
 * picture being drawn while stopped). Machine thread only. */
void o2_host_publish_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* O2_HOST_H */

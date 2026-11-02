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
 * against an absolute monotonic deadline ladder at the exact frame rate --
 * 29344 clocks at 1.76 MHz (59.98 Hz). While a frontend feeds display ticks
 * at that rate (o2_host_notify_vsync) the machine phase-locks one frame
 * per tick instead. The siblings' hardest-won lesson applies: VERIFY the
 * pacing -- the boot_smoke test measures it.
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
    const uint8_t *bios;         /* copied; NULL runs the no-BIOS stand-in */
    uint32_t bios_size;
    const uint8_t *cart;         /* an opened image, copied; NULL boots CONFIG */
    uint32_t cart_size;
    const char *boip_hostport;   /* fujinet-pc's BoIP listener, "host:port" */
    bool cart_sync;              /* run the mailbox inline (tests) */
    bool mame_power_on;          /* no CLEAR pulse at power-on (A/B runs) */
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

/* The Clear key, pressed for a few frames and released (the menus' and the
 * keypad window's CLEAR when it is clicked rather than held). */
void o2_host_clear_pulse(void);

/* ---- video: latest-wins single slot ----
 * Copies the newest frame into dst (O2_FB_WIDTH x O2_FB_HEIGHT
 * XRGB words) when its serial differs from *serial_inout; returns nonzero
 * and updates the serial when it copied. */
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

/* ---- audio: 48 kHz interleaved stereo float, self-correcting backlog ----
 * Returns frames written (no padding). */
int o2_host_audio_copy(float *dst, int max_frames);
void o2_host_audio_reset(void);

/* ---- inputs (callable from any thread; latched by the machine once a
 * frame, at VBLANK, as MAME's input ports are) ---- */
void o2_host_keypad_set(int pad, uint16_t mask);  /* bit n = key n */
void o2_host_clear_set(bool down);                /* the Clear key */
void o2_host_release_all(void);

/* The stand-in met a game it cannot run (no BIOS imported). */
bool o2_host_needs_bios(void);

/* ---- the cartridge ---- */
void o2_host_cart_status(o2_cart_status_t *out);
/* The cart device, or NULL when not running (the debugger's page map,
 * text engine and arena dumps). Use it under o2_host_lock. */
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

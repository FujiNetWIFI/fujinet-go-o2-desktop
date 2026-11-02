/*
 * host.c -- see host.h.
 *
 * The frame slot, the audio ring, the deadline ladder and the vsync phase
 * lock are the siblings' (the SMS's, astrocade's and the ColecoVision's
 * host.c): a latest-wins video slot with a serial, an audio ring that trims
 * all the way down to a small fixed cushion when the consumer lags, and a
 * lock that refuses tick sources that are not at the machine's own rate.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "host.h"

#include <pthread.h>
#include <sched.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the machine and its thread ---- */

static o2_machine_t s_machine;
static o2_cart_t *s_cart = NULL;
static pthread_t s_thread;
static atomic_bool s_running = false;
static atomic_bool s_stop_req = false;
/* The machine thread powers the cart on; until it has, nobody else looks
 * at the cart (a status read would race the power-on). */
static atomic_bool s_cart_up = false;
static char s_error[256];

static uint8_t *s_cart_image = NULL;
static uint32_t s_cart_size = 0;
static char s_boip[128];
static bool s_have_boip = false;
static bool s_cart_sync = false;
static long s_frame_ns = 16672727;
static o2_host_pacing_t s_pacing;     /* machine thread only; read after stop */
static atomic_uint_fast64_t s_frames = 0;
static double s_frame_hz = 0.0;

/* inputs, written by any thread, latched by the machine at VBLANK */
static atomic_uint_least16_t s_keys[2];
static atomic_bool s_clear;
static atomic_int s_clear_pulse;        /* frames left of a clicked CLEAR */
static atomic_bool s_needs_bios;

/* ---- the run lock -----------------------------------------------------------
 * The machine thread holds it while it runs a frame and lets go of it between
 * frames (and while the debugger has the machine parked). Every other thread
 * that looks at or edits the machine or the cartridge holds it too
 * (o2_host_lock), so a debugger window refreshing while the machine runs,
 * the status dot and a breakpoint edit never race the emulation. A UI thread
 * waits at most for the rest of one frame's emulation, a couple of
 * milliseconds. Re-entrant per thread, and a no-op on the machine thread,
 * which already holds it. */
static pthread_mutex_t s_run_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int s_lock_waiters;
static _Thread_local int t_lock_depth;

void o2_host_lock(void)
{
    if (t_lock_depth++ == 0)
    {
        atomic_fetch_add(&s_lock_waiters, 1);
        pthread_mutex_lock(&s_run_lock);
        atomic_fetch_sub(&s_lock_waiters, 1);
    }
}

void o2_host_unlock(void)
{
    if (t_lock_depth > 0 && --t_lock_depth == 0)
        pthread_mutex_unlock(&s_run_lock);
}

void o2_host_park_release(void)
{
    pthread_mutex_unlock(&s_run_lock);
}

void o2_host_park_reacquire(void)
{
    pthread_mutex_lock(&s_run_lock);
}

/* The machine thread, between frames: let a waiting UI thread in first
 * (a mutex is not fair, and a machine running behind would relock at once). */
static void run_lock_take(void)
{
    for (int i = 0; i < 2000 && atomic_load(&s_lock_waiters) > 0; i++)
    {
        if (i < 50)
            sched_yield();
        else
        {
            struct timespec ts = { 0, 50000L };
            nanosleep(&ts, NULL);
        }
    }
    pthread_mutex_lock(&s_run_lock);
}

/* ---- frame slot ---- */

static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_frame[O2_FB_WIDTH * O2_FB_HEIGHT];
static uint64_t s_frame_serial = 0;

static pthread_mutex_t s_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static o2_host_frame_cb s_frame_cb;
static void *s_frame_cb_ctx;

void o2_host_set_frame_callback(o2_host_frame_cb cb, void *ctx)
{
    pthread_mutex_lock(&s_cb_lock);
    s_frame_cb = cb;
    s_frame_cb_ctx = ctx;
    pthread_mutex_unlock(&s_cb_lock);
}

/* A frame identical to the one in the slot is not a new frame: the serial
 * stays and nobody is called. The machine runs at 60 Hz whatever it shows,
 * and most of what a FujiNet cartridge shows (CONFIG, a menu) does not move,
 * so without this the frontends would copy, upload and draw the same picture
 * sixty times a second. */
static void frame_publish(const uint32_t *fb)
{
    pthread_mutex_lock(&s_frame_lock);
    if (s_frame_serial != 0 && memcmp(s_frame, fb, sizeof s_frame) == 0)
    {
        pthread_mutex_unlock(&s_frame_lock);
        return;
    }
    memcpy(s_frame, fb, sizeof s_frame);
    s_frame_serial++;
    pthread_mutex_unlock(&s_frame_lock);

    pthread_mutex_lock(&s_cb_lock);
    if (s_frame_cb)
        s_frame_cb(s_frame_cb_ctx);
    pthread_mutex_unlock(&s_cb_lock);
}

int o2_host_frame_copy(uint32_t *dst, uint64_t *serial_inout)
{
    int changed;

    pthread_mutex_lock(&s_frame_lock);
    changed = (*serial_inout != s_frame_serial);
    if (changed)
    {
        memcpy(dst, s_frame, sizeof s_frame);
        *serial_inout = s_frame_serial;
    }
    pthread_mutex_unlock(&s_frame_lock);
    return changed;
}

uint64_t o2_host_frame_count(void)
{
    return atomic_load(&s_frames);
}

double o2_host_frame_rate(void)
{
    return atomic_load(&s_running) ? s_frame_hz : 0.0;
}

void o2_host_publish_frame(void)
{
    static uint32_t fb[O2_FB_WIDTH * O2_FB_HEIGHT];
    o2_machine_snapshot(&s_machine, fb);
    frame_publish(fb);
}

/* ---- audio ring: interleaved stereo frames ---- */

#define RING_CAPACITY          (O2_AUDIO_RATE / 2)
#define RING_TARGET_FRAMES     (O2_AUDIO_RATE / 33)
#define RING_HIGHWATER_FRAMES  (O2_AUDIO_RATE / 10)

static pthread_mutex_t s_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static float s_ring[RING_CAPACITY * 2];
static int s_head = 0;
static int s_count = 0;
static int s_primed = 0;

static void audio_publish(const float *frames, int count)
{
    if (count <= 0)
        return;
    if (count > RING_CAPACITY)
    {
        frames += 2 * (count - RING_CAPACITY);
        count = RING_CAPACITY;
    }

    pthread_mutex_lock(&s_audio_lock);
    for (int i = 0; i < count; i++)
    {
        s_ring[2 * s_head] = frames[2 * i];
        s_ring[2 * s_head + 1] = frames[2 * i + 1];
        s_head = (s_head + 1) % RING_CAPACITY;
    }
    s_count += count;
    if (s_count > RING_CAPACITY)
        s_count = RING_CAPACITY;
    pthread_mutex_unlock(&s_audio_lock);
}

int o2_host_audio_copy(float *dst, int max_frames)
{
    pthread_mutex_lock(&s_audio_lock);

    /* Catch up on lag before copying anything: trim to a small fixed
     * cushion of RECENT audio, never scaled by the caller's request. */
    if (s_count > RING_HIGHWATER_FRAMES)
        s_count = RING_TARGET_FRAMES;

    /* Prime gate: no output until a target's worth has accumulated, so a
     * production burst does not dribble out as several under-filled
     * requests. Primed until the ring runs completely dry. */
    if (!s_primed)
    {
        if (s_count < RING_TARGET_FRAMES)
        {
            pthread_mutex_unlock(&s_audio_lock);
            return 0;
        }
        s_primed = 1;
    }

    const int n = (max_frames < s_count) ? max_frames : s_count;
    int read_pos = ((s_head - s_count) % RING_CAPACITY + RING_CAPACITY) % RING_CAPACITY;
    for (int i = 0; i < n; i++)
    {
        dst[2 * i] = s_ring[2 * read_pos];
        dst[2 * i + 1] = s_ring[2 * read_pos + 1];
        read_pos = (read_pos + 1) % RING_CAPACITY;
    }
    s_count -= n;
    if (s_count == 0)
        s_primed = 0;
    pthread_mutex_unlock(&s_audio_lock);
    return n;
}

void o2_host_audio_reset(void)
{
    pthread_mutex_lock(&s_audio_lock);
    s_head = 0;
    s_count = 0;
    s_primed = 0;
    pthread_mutex_unlock(&s_audio_lock);
}

/* ---- inputs ---- */

void o2_host_keypad_set(int pad, uint16_t mask)
{
    if (pad < 0 || pad > 1)
        return;
    atomic_store(&s_keys[pad], (uint_least16_t)(mask & O2_KEYS_MASK));
}

void o2_host_clear_set(bool down)
{
    atomic_store(&s_clear, down);
}

void o2_host_clear_pulse(void)
{
    /* long enough for the CPU to sit in RESET and the cart to see the bus
     * fall silent, as a finger on the key would */
    atomic_store(&s_clear_pulse, 3);
}

bool o2_host_needs_bios(void)
{
    return atomic_load(&s_needs_bios);
}

/* The machine thread, at the frame boundary: what the threads above set is
 * what the console's ports see from here on. */
static void inputs_take(o2_machine_t *m)
{
    int pulse = atomic_load(&s_clear_pulse);

    m->keys_live[O2_KEYPAD_A] = atomic_load(&s_keys[O2_KEYPAD_A]);
    m->keys_live[O2_KEYPAD_B] = atomic_load(&s_keys[O2_KEYPAD_B]);
    m->clear_live = atomic_load(&s_clear) || pulse > 0;
    if (pulse > 0)
        atomic_store(&s_clear_pulse, pulse - 1);
}

void o2_host_release_all(void)
{
    o2_host_keypad_set(0, 0);
    o2_host_keypad_set(1, 0);
    o2_host_clear_set(false);
}

void o2_host_cart_status(o2_cart_status_t *out)
{
    o2_host_lock();
    o2_cart_status(atomic_load(&s_cart_up) ? s_cart : NULL, out);
    o2_host_unlock();
}

o2_cart_t *o2_host_cart(void)
{
    return atomic_load(&s_cart_up) ? s_cart : NULL;
}

/* ---- debugger plumbing ---- */

o2_machine_t *o2_host_machine(void)
{
    return &s_machine;
}

void o2_host_set_instr_hook(void (*hook)(o2_machine_t *m, void *user), void *user)
{
    o2_host_lock();
    s_machine.instr_hook_user = user;
    s_machine.instr_hook = hook;
    o2_host_unlock();
}

void o2_host_set_bus_hook(void (*hook)(o2_machine_t *m, void *user, int kind,
                                        uint16_t addr, uint8_t data),
                           void *user, uint8_t watch)
{
    o2_host_lock();
    s_machine.bus_hook_user = user;
    s_machine.bus_hook = hook;
    s_machine.watch = hook ? watch : 0;
    o2_host_unlock();
}

/* ---- vsync phase lock ----------------------------------------------------
 * A frontend that presents on the compositor's vsync feeds ticks here. While
 * they keep arriving close to the machine's own rate, the emulator runs one
 * frame per tick and inherits the display's cadence exactly -- which removes
 * the slow beat between 59.92 Hz and a 60 Hz panel. "Close" is measured: the
 * tick interval is smoothed and the lock refuses any source outside +/-8% of
 * the frame period, so a 120 Hz panel, a 60 Hz panel under a PAL machine,
 * or an 8 ms poll timer paces by the wall clock instead. */

static pthread_mutex_t s_vs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_vs_cond = PTHREAD_COND_INITIALIZER;
static uint64_t s_vs_count;
static int64_t s_vs_seen_ns;
static int64_t s_vs_period_ns;

/* A gap longer than this is a pause (a hidden window), not an interval. */
#define VS_STALE_NS 250000000LL

static int64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

void o2_host_notify_vsync(int64_t frame_time_ns)
{
    int64_t now = mono_ns();

    pthread_mutex_lock(&s_vs_lock);
    /* frame_time_ns == 0 is stop()'s wake-up poke, not a display tick */
    if (frame_time_ns != 0)
    {
        int64_t dt = now - s_vs_seen_ns;
        if (s_vs_seen_ns == 0 || dt <= 0 || dt > VS_STALE_NS)
            s_vs_period_ns = 0;
        else if (s_vs_period_ns == 0)
            s_vs_period_ns = dt;
        else
            s_vs_period_ns += (dt - s_vs_period_ns) / 8;
        s_vs_seen_ns = now;
    }
    s_vs_count++;
    pthread_cond_signal(&s_vs_cond);
    pthread_mutex_unlock(&s_vs_lock);
}

static int vsync_usable(long frame_ns)
{
    int ok;
    pthread_mutex_lock(&s_vs_lock);
    ok = s_vs_seen_ns != 0 && (mono_ns() - s_vs_seen_ns) < VS_STALE_NS &&
         s_vs_period_ns > (int64_t)frame_ns * 92 / 100 &&
         s_vs_period_ns < (int64_t)frame_ns * 108 / 100;
    pthread_mutex_unlock(&s_vs_lock);
    return ok;
}

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L)
    {
        t->tv_nsec -= 1000000000L;
        t->tv_sec += 1;
    }
}

static long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (long)((a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec));
}

/* clock_nanosleep(TIMER_ABSTIME) is the right tool on POSIX, but it is a
 * no-op under mingw/Wine (the emulator then free-runs at thousands of fps
 * in a Windows build), and Darwin has no clock_nanosleep at all: both take
 * a plain relative nanosleep, which winpthreads honours. */
static void sleep_until(const struct timespec *next, const struct timespec *now)
{
#if defined(_WIN32) || defined(__APPLE__)
    long remain = ts_diff_ns(next, now);
    struct timespec rel;
    if (remain <= 0)
        return;
    rel.tv_sec = remain / 1000000000L;
    rel.tv_nsec = remain % 1000000000L;
    nanosleep(&rel, NULL);
#else
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
    (void)now;
#endif
}

/* Wait for the next vsync tick, or give up after `timeout_ns`. */
static int wait_vsync(uint64_t *seen, long timeout_ns)
{
    struct timespec deadline;
    int got = 0;

    clock_gettime(CLOCK_REALTIME, &deadline);
    add_ns(&deadline, timeout_ns);

    pthread_mutex_lock(&s_vs_lock);
    while (s_vs_count == *seen)
    {
        if (pthread_cond_timedwait(&s_vs_cond, &s_vs_lock, &deadline) != 0)
            break;
    }
    if (s_vs_count != *seen)
    {
        *seen = s_vs_count;
        got = 1;
    }
    pthread_mutex_unlock(&s_vs_lock);
    return got;
}

/* ---- the thread ---- */

static void *machine_thread(void *arg)
{
    const long frame_ns = s_frame_ns;
    const long vs_timeout_ns = frame_ns * 2;
    struct timespec next, now, frame_start;
    uint64_t vs_seen = 0;
    int locked = 0;
    char why[160];

    (void)arg;

#if defined(__APPLE__)
    /* Darwin coalesces the timers of a default-QoS thread with no UI, by
     * whole frames on a busy machine; an emulator's frame clock is
     * interactive work. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    /* the machine thread holds the run lock whenever it runs the machine;
     * its own nested o2_host_lock calls (the debugger's hooks) are no-ops */
    t_lock_depth = 1;
    memset(&s_pacing, 0, sizeof s_pacing);
    pthread_mutex_lock(&s_run_lock);

    /* Bring the cartridge up here rather than on the caller's thread: the
     * TCP connect to the BoIP listener is quick but not free, and a failed
     * link must not stall the UI -- the mailbox runs link-down and CONFIG
     * says so on screen. The image was checked in o2_host_start. */
    if (o2_cart_power_on(s_cart, s_cart_image, s_cart_size,
                              s_have_boip ? s_boip : NULL, s_cart_sync,
                              why, sizeof why) != 0)
    {
        /* cannot happen after the check; fall back to CONFIG rather than
         * leaving a console with no cartridge */
        fprintf(stderr, "fujinet: %s; booting CONFIG\n", why);
        o2_cart_power_on(s_cart, NULL, 0, s_have_boip ? s_boip : NULL,
                              s_cart_sync, why, sizeof why);
    }
    atomic_store(&s_cart_up, true);

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!atomic_load(&s_stop_req))
    {
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        o2_machine_run_frame(&s_machine);
        o2_cart_frame(s_cart);
        frame_publish(s_machine.fb);
        audio_publish(s_machine.out, s_machine.out_frames);
        s_machine.out_frames = 0;
        atomic_store(&s_needs_bios, s_machine.stub_parked);
        atomic_fetch_add(&s_frames, 1);
        /* MAME's input ports update at the frame boundary */
        inputs_take(&s_machine);
        o2_machine_latch_inputs(&s_machine);

        /* between frames the machine is anyone's who asks */
        pthread_mutex_unlock(&s_run_lock);

        if (vsync_usable(frame_ns) && wait_vsync(&vs_seen, vs_timeout_ns))
        {
            /* Phase-locked: the display's cadence is the machine's. A tick
             * back in under half a frame is a burst from the frame clock,
             * not a vblank; that one frame takes the ladder instead. */
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ts_diff_ns(&now, &frame_start) >= frame_ns / 2)
            {
                locked = 1;
                next = now;
                run_lock_take();
                continue;
            }
            next = frame_start;
        }
        else if (locked)
        {
            locked = 0;
            clock_gettime(CLOCK_MONOTONIC, &next);
        }

        /* Absolute deadline ladder; resync when badly behind (a laptop
         * resume, a debugger pause) rather than fast-forwarding. */
        add_ns(&next, frame_ns);
        clock_gettime(CLOCK_MONOTONIC, &now);
        {
            long behind = ts_diff_ns(&now, &next);
            if (behind > 4 * frame_ns)
            {
                next = now;
                s_pacing.resyncs++;
            }
            else if (behind < 0)
            {
                struct timespec woke;
                long late;
                sleep_until(&next, &now);
                clock_gettime(CLOCK_MONOTONIC, &woke);
                late = ts_diff_ns(&woke, &next);
                s_pacing.sleeps++;
                if (late > 0)
                {
                    s_pacing.late_ns += (uint64_t)late;
                    if (late > s_pacing.worst_late_ns)
                        s_pacing.worst_late_ns = late;
                }
            }
        }
        run_lock_take();
    }
    pthread_mutex_unlock(&s_run_lock);
    return NULL;
}

/* ---- lifecycle ---- */

const char *o2_host_last_error(void)
{
    return s_error;
}

bool o2_host_is_running(void)
{
    return atomic_load(&s_running);
}

int o2_host_start(const o2_host_opts_t *opts)
{
    char why[160];

    if (atomic_load(&s_running))
    {
        snprintf(s_error, sizeof s_error, "a session is already running");
        return -1;
    }
    s_error[0] = '\0';
    if (!opts)
    {
        snprintf(s_error, sizeof s_error, "no options");
        return -1;
    }

    if (opts->cart && opts->cart_size &&
        !o2_cart_check_image(opts->cart, opts->cart_size, NULL, why, sizeof why))
    {
        snprintf(s_error, sizeof s_error, "%s", why);
        return -1;
    }

    if (!s_cart)
    {
        s_cart = o2_cart_create();
        if (!s_cart)
        {
            snprintf(s_error, sizeof s_error, "cannot create the FujiNet cartridge");
            return -1;
        }
    }

    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
    if (opts->cart && opts->cart_size)
    {
        s_cart_image = malloc(opts->cart_size);
        if (!s_cart_image)
        {
            snprintf(s_error, sizeof s_error, "out of memory");
            return -1;
        }
        memcpy(s_cart_image, opts->cart, opts->cart_size);
        s_cart_size = opts->cart_size;
    }
    s_have_boip = opts->boip_hostport != NULL;
    if (s_have_boip)
        snprintf(s_boip, sizeof s_boip, "%s", opts->boip_hostport);
    s_cart_sync = opts->cart_sync;

    /* the exact frame period from the exact clock */
    s_frame_hz = O2_FRAME_HZ;
    s_frame_ns = (long)(1e9 / O2_FRAME_HZ + 0.5);

    o2_machine_init(&s_machine, opts->bios, opts->bios_size, s_cart, opts->mame_power_on);
    atomic_store(&s_clear_pulse, 0);
    atomic_store(&s_needs_bios, false);
    inputs_take(&s_machine);
    o2_machine_latch_inputs(&s_machine);
    o2_host_audio_reset();
    atomic_store(&s_frames, 0);

    atomic_store(&s_stop_req, false);
    if (pthread_create(&s_thread, NULL, machine_thread, NULL) != 0)
    {
        snprintf(s_error, sizeof s_error, "cannot start the machine thread");
        return -1;
    }
    atomic_store(&s_running, true);
    return 0;
}

void o2_host_pacing(o2_host_pacing_t *out)
{
    *out = s_pacing;
}

void o2_host_stop(void)
{
    if (!atomic_load(&s_running))
        return;
    atomic_store(&s_stop_req, true);
    /* wake the thread if it is parked waiting for a vsync */
    o2_host_notify_vsync(0);
    pthread_join(s_thread, NULL);
    atomic_store(&s_running, false);
    atomic_store(&s_cart_up, false);
    o2_cart_power_off(s_cart);
    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
    pthread_mutex_lock(&s_frame_lock);
    s_frame_serial = 0;
    pthread_mutex_unlock(&s_frame_lock);
}

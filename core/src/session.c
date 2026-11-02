/*
 * session.c -- the frontend contract (core/include/o2session.h) over the
 * Magnavox Odyssey 2 host (core/o2/host.h).
 *
 * The C modules beside it (settings, paths, media, roms, audio, gamepads,
 * bindings) share the session struct through session_internal.h; this file
 * owns the lifecycle, the power cycles, the cartridges, the input routing
 * and the cartridge status. Ported from the SMS sibling's session.c (itself
 * the NES sibling's session.cpp, minus MesenCE).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"
#include "bindings.h"
#include "o2debug.h"
#include "host.h"
#include "fuji_mailbox.h"
#include "s2map.h"

/* The largest image the cartridge plans (a 256-byte ST2 header and 64
 * blocks), with room to say "too big" rather than truncate. */
#define CART_MAX (S2MAP_IMAGE_MAX + 1u)

/* ---- helpers shared with the C modules ----------------------------------- */

void session_set_error(struct o2session *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
    va_end(ap);
}

void session_gamepad_event(struct o2session *s, const char *text)
{
    pthread_mutex_lock(&s->pad_event_mtx);
    snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
    pthread_mutex_unlock(&s->pad_event_mtx);
}

int o2session_gamepad_last_event(o2session *s, char *dst, int dstsz)
{
    int n;
    if (!dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&s->pad_event_mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
    pthread_mutex_unlock(&s->pad_event_mtx);
    return n;
}

#ifdef O2_GAMEPAD_STUB
int gamepad_start(struct o2session *s) { session_set_error(s, "gamepad support not built"); return -1; }
void gamepad_stop(struct o2session *s) { (void)s; }
int o2session_gamepad_count(o2session *s) { (void)s; return 0; }
int o2session_gamepad_name(o2session *s, int i, char *d, int n) { (void)s; (void)i; if (d && n > 0) d[0] = 0; return 0; }
void o2session_gamepad_assign(o2session *s, int i, int p) { (void)s; (void)i; (void)p; }
int o2session_gamepad_assignment(o2session *s, int i) { (void)s; (void)i; return -1; }
int o2session_gamepad_effective_port(o2session *s, int i) { (void)s; (void)i; return -1; }
unsigned o2session_gamepad_generation(o2session *s) { (void)s; return 0; }
void o2session_gamepad_capture_begin(o2session *s) { (void)s; }
void o2session_gamepad_capture_cancel(o2session *s) { (void)s; }
int o2session_gamepad_capture_poll(o2session *s, int *b) { (void)s; (void)b; return 0; }
#endif

/* ---- lifecycle ------------------------------------------------------------- */

o2session *o2session_new(const o2session_paths *paths)
{
    struct o2session *s;

    paths_capture_launch_dir();
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;

    pthread_mutex_init(&s->settings_mtx, NULL);
    pthread_mutex_init(&s->sysact_mtx, NULL);
    pthread_mutex_init(&s->pad_event_mtx, NULL);

    if (paths_init(s, paths ? paths->config_dir : NULL,
                   paths ? paths->data_dir : NULL) != 0) {
        o2session_free(s);
        return NULL;
    }
    settings_init(s);
    bindings_init(s);
    roms_provision_embedded(s);

    if (paths && paths->fujinet_lib) {
        snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
        s->fujinet_disabled = !paths->fujinet_lib[0];
    }
    if (paths && paths->fujinet_runtime_src)
        snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
                 paths->fujinet_runtime_src);
    return s;
}

void o2session_free(o2session *s)
{
    if (!s)
        return;
    o2session_stop(s);
    o2session_settings_flush(s);
    settings_free_all(s);
    o2debug_destroy(s);
    pthread_mutex_destroy(&s->settings_mtx);
    pthread_mutex_destroy(&s->sysact_mtx);
    pthread_mutex_destroy(&s->pad_event_mtx);
    free(s);
}

void o2session_default_opts(o2session *s, o2session_start_opts *opts)
{
    memset(opts, 0, sizeof *opts);
    opts->use_bios = o2session_get_int(s, "use_bios", 1);
    opts->analog_joystick = o2session_get_int(s, "analog_joystick", 1);
    opts->enable_fujinet = o2session_get_int(s, "enable_fujinet", 1);
    opts->external_fujinet = o2session_get_int(s, "fujinet_external", 0);
    opts->boip_connect = o2session_get_str(s, "fujinet_external_addr", NULL);
    opts->enable_audio = o2session_get_int(s, "enable_audio", 1);
    opts->enable_gamepad = o2session_get_int(s, "enable_gamepad", 1);
    opts->cart_path = o2session_get_str(s, "cart", NULL);
    if (opts->cart_path && !opts->cart_path[0])
        opts->cart_path = NULL;
}

static uint8_t *read_cart(const char *path, uint32_t *len, char *why, int whysz)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    size_t got;

    if (!f) {
        snprintf(why, (size_t)whysz, "cannot open %s", path);
        return NULL;
    }
    buf = malloc(CART_MAX + 1);
    if (!buf) {
        fclose(f);
        snprintf(why, (size_t)whysz, "out of memory");
        return NULL;
    }
    got = fread(buf, 1, CART_MAX + 1, f);
    fclose(f);
    if (got == 0) {
        free(buf);
        snprintf(why, (size_t)whysz, "%s is empty", path);
        return NULL;
    }
    if (got >= CART_MAX) {
        free(buf);
        snprintf(why, (size_t)whysz,
                 "the image is larger than the cartridge can serve ("
                 O2_CART_MAX_TEXT " bytes)");
        return NULL;
    }
    *len = (uint32_t)got;
    return buf;
}

/* Bring the machine up with s->opts and the given image (NULL: CONFIG). */
static int host_up(struct o2session *s, const char *cart_path)
{
    o2_host_opts_t ho;
    uint8_t *cart = NULL, *bios = NULL;
    uint32_t cart_len = 0, bios_len = 0;
    char why[256];
    int rc;

    memset(&ho, 0, sizeof ho);
    if (cart_path && *cart_path) {
        cart = read_cart(cart_path, &cart_len, why, sizeof why);
        if (!cart) {
            session_set_error(s, "%s", why);
            return -1;
        }
    }
    if (s->opts.use_bios)
        bios = roms_load_bios(s, &bios_len);

    /* The cartridge dials FujiNet's BoIP port -- the in-process runtime's,
     * or a standalone fujinet-pc's when the runtime is switched off or
     * external. A
     * session whose FujiNet is disabled outright (fujinet_lib "", the tests)
     * dials nothing, so it can never pick up someone else's FujiNet (and
     * Windows' two-second refusal of a closed port never holds up power-on). */
    if (s->fujinet_disabled)
        s->boip_endpoint[0] = '\0';
    ho.bios = bios;
    ho.bios_size = bios_len;
    ho.cart = cart;
    ho.cart_size = cart_len;
    ho.boip_hostport = s->boip_endpoint;
    ho.cart_sync = getenv("O2_CART_SYNC") != NULL;
    ho.mame_power_on = getenv("O2_MAME_POWER_ON") != NULL;

    rc = o2_host_start(&ho);
    if (rc != 0)
        session_set_error(s, "%s", o2_host_last_error());
    free(cart);
    free(bios);
    return rc;
}

/* A power cycle: the machine and the cartridge come up fresh; FujiNet stays
 * up. `cart_path` NULL boots CONFIG. */
static int power_cycle(struct o2session *s, const char *cart_path)
{
    int rc;

    /* the console powers up with the BIOS its settings name now: an Import
     * BIOS or a Preferences change takes effect here */
    s->opts.use_bios = o2session_get_int(s, "use_bios", 1);

    o2debug_before_power_cycle(s);
    o2_host_stop();
    rc = host_up(s, cart_path);
    if (rc != 0 && cart_path) {
        /* whatever was running is gone: put CONFIG back */
        char err[256];
        snprintf(err, sizeof err, "%s", s->last_error);
        host_up(s, NULL);
        snprintf(s->last_error, sizeof s->last_error, "%s", err);
    }
    o2debug_after_power_cycle(s);
    return rc;
}

static void set_cart(struct o2session *s, const char *path)
{
    if (path && *path)
        snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
    else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    o2session_set_str(s, "cart", s->cart_path);
}

static void adopt_opts(struct o2session *s, const o2session_start_opts *opts)
{
    s->opts = *opts;
    if (opts->cart_path) {
        char abs[O2_PATH_MAX];
        snprintf(s->cart_path, sizeof s->cart_path, "%s",
                 paths_resolve(opts->cart_path, abs, sizeof abs));
    } else
        s->cart_path[0] = '\0';
    s->opts.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    snprintf(s->boip_addr, sizeof s->boip_addr, "%s",
             opts->boip_connect ? opts->boip_connect : "");
    s->opts.boip_connect = s->boip_addr;
}

int o2session_start(o2session *s, const o2session_start_opts *opts)
{
    o2session_start_opts local;

    if (s->running)
        return 0;
    s->last_error[0] = '\0';

    if (!opts) {
        o2session_default_opts(s, &local);
        opts = &local;
    }
    adopt_opts(s, opts);
    memset(s->pad_src, 0, sizeof s->pad_src);
    memset(s->sw_src, 0, sizeof s->sw_src);
    o2_host_release_all();
    o2debug_trace_fujinet(s);

    /* An external FujiNet is dialed at the address the settings name; a bad
     * address is an error, not a silent fall back to the built-in one. */
    s->fujinet_external = 0;
    snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "127.0.0.1:%d",
             O2SESSION_BOIP_PORT);
    if (s->opts.external_fujinet && !s->fujinet_disabled) {
        char host[64];
        int port;

        if (o2session_parse_hostport(s->opts.boip_connect, host, sizeof host,
                                          &port) != 0) {
            session_set_error(s, "\"%s\" is not an address:port for the "
                              "external FujiNet (example: 127.0.0.1:%d)",
                              s->opts.boip_connect ? s->opts.boip_connect : "",
                              O2SESSION_BOIP_PORT);
            return -1;
        }
        s->fujinet_external = 1;
        snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "%s:%d", host,
                 port);
    }

    /* FujiNet FIRST: it listens and the cartridge dials in, so the listener
     * has to exist before the machine's first transaction or the CONFIG
     * client boots reporting no link. Failing to start is NOT fatal (the
     * cartridge also redials when a transaction finds the link down). An
     * external FujiNet is someone else's to start. */
    if (s->opts.enable_fujinet && !s->fujinet_disabled && !s->fujinet_external) {
        if (fujinet_start(s) == 0)
            fujinet_wait_for_boip(s, 3000);
    }

    /* A remembered cartridge that no longer loads (moved, or an image the
     * cartridge cannot map) must not leave the app unbootable: fall back to
     * CONFIG and say why. */
    if (host_up(s, s->opts.cart_path) != 0) {
        if (s->opts.cart_path) {
            fprintf(stderr, "o2: %s: %s; booting CONFIG\n", s->cart_path, s->last_error);
            set_cart(s, NULL);
            if (host_up(s, NULL) != 0) {
                fujinet_stop(s);
                return -1;
            }
        } else {
            fujinet_stop(s);
            return -1;
        }
    }

    s->running = 1;
    o2debug_after_power_cycle(s);

    if (s->opts.enable_gamepad && gamepad_start(s) != 0) {
        fprintf(stderr, "o2: gamepads unavailable (%s)\n", s->last_error);
        s->last_error[0] = '\0';
    }
    if (s->opts.enable_audio && audio_start(s) != 0) {
        fprintf(stderr, "o2: audio unavailable (%s); continuing silent\n", s->last_error);
        s->last_error[0] = '\0';
    }
    return 0;
}

void o2session_stop(o2session *s)
{
    if (!s->running)
        return;
    audio_stop(s);
    gamepad_stop(s);
    o2debug_before_power_cycle(s);
    o2_host_stop();
    fujinet_stop(s);
    s->fujinet_external = 0;
    s->running = 0;
}

int o2session_is_running(const o2session *s)
{
    return s->running;
}

const char *o2session_last_error(const o2session *s)
{
    return s->last_error;
}

int o2session_restart(o2session *s)
{
    o2session_start_opts opts;

    o2session_settings_flush(s);
    o2session_default_opts(s, &opts);
    if (!s->running)
        return o2session_start(s, &opts);

    /* FujiNet or the device toggles change what runs beside the machine:
     * a full restart. Anything else is a machine option: a power cycle. */
    if (!!opts.enable_fujinet != !!s->opts.enable_fujinet ||
        !!opts.external_fujinet != !!s->opts.external_fujinet ||
        strcmp(opts.boip_connect ? opts.boip_connect : "", s->boip_addr) ||
        !!opts.enable_audio != !!s->opts.enable_audio ||
        !!opts.enable_gamepad != !!s->opts.enable_gamepad) {
        o2session_stop(s);
        return o2session_start(s, &opts);
    }
    adopt_opts(s, &opts);
    return power_cycle(s, s->opts.cart_path);
}

/* ---- cartridges ------------------------------------------------------------ */

/* Read and plan an image: 1 if the cartridge can serve it (*claims set when
 * it is a FujiNet app), else 0 and why. */
static int check_image(const char *path, int *claims, char *why, int whysz)
{
    uint8_t *img;
    uint32_t len = 0;
    char err[256];
    char abs[O2_PATH_MAX];
    bool app = false;
    int ok;

    if (why && whysz > 0)
        why[0] = '\0';
    if (claims)
        *claims = 0;
    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    img = read_cart(path, &len, err, sizeof err);
    if (!img) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err);
        return 0;
    }
    ok = o2_cart_check_image(img, len, &app, why, whysz);
    free(img);
    if (claims)
        *claims = app;
    return ok;
}

int o2session_check_cart(const char *path, char *why, int whysz)
{
    return check_image(path, NULL, why, whysz);
}

int o2session_cart_is_app(const char *path)
{
    int app = 0;
    return check_image(path, &app, NULL, 0) && app;
}

int o2session_load_cart(o2session *s, const char *path)
{
    char why[256];
    char abs[O2_PATH_MAX];
    int app = 0;

    if (!s->running || !path || !*path)
        return -1;
    path = paths_resolve(path, abs, sizeof abs);
    if (!check_image(path, &app, why, sizeof why)) {
        session_set_error(s, "%s", why);
        return -1;
    }
    /* a game is BIOS byte-code: the stand-in cannot run it */
    if (!app && !(o2session_get_int(s, "use_bios", 1) && o2session_have_bios(s))) {
        session_set_error(s, "This cartridge needs the Magnavox Odyssey 2 BIOS. Use Import BIOS "
                             "to add your own copy (the FujiNet apps and CONFIG run without it).");
        return -1;
    }
    if (power_cycle(s, path) != 0) {
        set_cart(s, NULL);
        return -1;
    }
    set_cart(s, path);
    return 0;
}

const char *o2session_cart_path(const o2session *s)
{
    return s->cart_path;
}

int o2session_clear(o2session *s)
{
    if (!s->running)
        return -1;
    o2_host_clear_pulse();
    return 0;
}

int o2session_needs_bios(o2session *s)
{
    return s->running && o2_host_needs_bios();
}

int o2session_reset_to_config(o2session *s)
{
    if (!s->running)
        return -1;
    set_cart(s, NULL);
    return power_cycle(s, NULL);
}

int o2session_eject(o2session *s)
{
    return o2session_reset_to_config(s);
}

/* ---- video / audio --------------------------------------------------------- */

int o2session_copy_frame(o2session *s, uint32_t *dst, uint64_t *serial_inout)
{
    if (!s->running)
        return 0;
    return o2_host_frame_copy(dst, serial_inout);
}

void o2session_set_frame_callback(o2session *s, o2session_frame_cb cb, void *ctx)
{
    (void)s;
    o2_host_set_frame_callback(cb, ctx);
}

uint64_t o2session_frame_count(o2session *s)
{
    return s->running ? o2_host_frame_count() : 0;
}

int o2session_refresh_rate(o2session *s)
{
    (void)s;
    return 60;
}

void o2session_notify_vsync(o2session *s, int64_t frame_time_ns)
{
    if (s->running)
        o2_host_notify_vsync(frame_time_ns);
}

int o2session_render_audio(o2session *s, float *out, int nframes)
{
    int got = 0;

    if (nframes <= 0)
        return 0;
    if (s->running)
        got = o2_host_audio_copy(out, nframes);
    if (got < nframes)
        memset(out + 2 * got, 0, sizeof(float) * 2 * (size_t)(nframes - got));
    {
        const float gain = (float)o2session_get_int(s, "volume", 100) / 100.0f;
        if (gain < 0.999f)
            for (int i = 0; i < 2 * got; i++)
                out[i] *= gain;
    }
    return nframes;
}

void o2session_set_volume(o2session *s, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    o2session_set_int(s, "volume", percent);
}

/* ---- input ----------------------------------------------------------------- */

static void apply_pad(struct o2session *s, int port)
{
    uint16_t mask = 0;
    for (int a = 0; a < O2_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            mask |= (uint16_t)(1u << a);
    o2_host_keypad_set(port, mask);
}

static void pad_src(struct o2session *s, int port, int act, int down, uint8_t src)
{
    if (down)
        s->pad_src[port][act] |= src;
    else
        s->pad_src[port][act] &= (uint8_t)~src;
    apply_pad(s, port);
}

static void switch_src(struct o2session *s, int sw, int down, uint8_t src)
{
    if (down)
        s->sw_src[sw] |= src;
    else
        s->sw_src[sw] &= (uint8_t)~src;
    if (sw == O2_SW_CLEAR)
        o2_host_clear_set(s->sw_src[sw] != 0);
}

void o2session_press(o2session *s, int target, int down)
{
    if (!s->running || target < 0 || target >= O2_TARGET_COUNT)
        return;

    if (target < 2 * O2_ACT_PER_PORT) {
        pad_src(s, target / O2_ACT_PER_PORT, target % O2_ACT_PER_PORT, down, 1);
        return;
    }
    target -= 2 * O2_ACT_PER_PORT;
    if (target < O2_SW_COUNT) {
        switch_src(s, target, down, 1);
        return;
    }
    if (down)
        o2session_sysaction(s, target - O2_SW_COUNT);
}

void o2session_sysaction(o2session *s, int sysact)
{
    switch (sysact) {
    case O2_SYSACT_RESET_CONFIG:
        o2session_reset_to_config(s);
        break;
    case O2_SYSACT_DEBUG_STOP:
        if (s->running) {
            o2debug *d = o2debug_get(s);
            o2debug_attach(d);
            o2debug_stop(d);
        }
        break;
    default:
        break;
    }
}

void o2session_sysaction_post(o2session *s, int sysact)
{
    if (sysact < 0 || sysact >= O2_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&s->sysact_mtx);
    s->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&s->sysact_mtx);
}

int o2session_sysaction_take(o2session *s, int *out)
{
    int found = 0;
    pthread_mutex_lock(&s->sysact_mtx);
    for (int i = 0; i < O2_SYSACT_COUNT; i++) {
        if (s->sysact_pending & (1u << i)) {
            s->sysact_pending &= ~(1u << i);
            if (out)
                *out = i;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s->sysact_mtx);
    return found;
}

unsigned o2session_buttons_held(o2session *s, int port)
{
    unsigned held = 0;
    if (port < 0 || port > 1)
        return 0;
    for (int a = 0; a < O2_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            held |= 1u << a;
    return held;
}

int o2session_switch_held(o2session *s, int sw)
{
    if (sw < 0 || sw >= O2_SW_COUNT)
        return 0;
    return s->sw_src[sw] != 0;
}

/* The gamepad thread's entry point: the same routing as press, from the
 * pad's own source bit, never touching the settings or the debugger. */
void session_gamepad_apply(struct o2session *s, int port, int act, int down)
{
    if (!s->running || port < 0 || port > 1 || act < 0 || act >= O2_ACT_PER_PORT)
        return;
    pad_src(s, port, act, down, 2);
}

void o2session_set_analog(o2session *s, int joystick)
{
    s->opts.analog_joystick = joystick ? 1 : 0;
    o2session_set_int(s, "analog_joystick", s->opts.analog_joystick);
}

/* ---- FujiNet --------------------------------------------------------------- */

int o2session_fujinet_running(const o2session *s)
{
    return s->fujinet_running;
}

const char *o2session_fujinet_webui_url(const o2session *s)
{
    return s->fujinet_external ? "" : s->webui_url;
}

int o2session_parse_hostport(const char *text, char *host, int hostsz,
                              int *port)
{
    char buf[80], *colon, *end;
    const char *name = "127.0.0.1";
    long num = O2SESSION_BOIP_PORT;
    size_t len, i;

    if (!text) text = "";
    while (*text == ' ' || *text == '\t') text++;
    len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                   text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    if (len >= sizeof buf) return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (len) {
        colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            name = buf;
            if (!*name) return -1;
            text = colon + 1;
        } else {
            text = buf;
        }
        if (!*text) return -1;
        num = strtol(text, &end, 10);
        if (*end || num < 1 || num > 65535) return -1;
        if (colon) {
            /* A host name or an IPv4 literal: letters, digits, '-', '_' and
             * '.'; a name of only digits and dots must be a whole address. */
            int digits_only = 1, octet[4], used = 0, k;
            for (i = 0; name[i]; i++) {
                char ch = name[i];
                int alnum = (ch >= '0' && ch <= '9') ||
                            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
                if (!alnum && ch != '-' && ch != '_' && ch != '.') return -1;
                if (!(ch >= '0' && ch <= '9') && ch != '.') digits_only = 0;
            }
            if (name[0] == '.' || name[i - 1] == '.' || strstr(name, "..")) return -1;
            if (digits_only) {
                if (sscanf(name, "%d.%d.%d.%d%n", &octet[0], &octet[1],
                           &octet[2], &octet[3], &used) != 4 || name[used])
                    return -1;
                for (k = 0; k < 4; k++)
                    if (octet[k] < 0 || octet[k] > 255) return -1;
            }
        }
    }
    if ((int)strlen(name) >= hostsz) return -1;
    memcpy(host, name, strlen(name) + 1);
    *port = (int)num;
    return 0;
}

int o2session_fujinet_external(const o2session *s)
{
    return s->fujinet_external;
}

const char *o2session_boip_endpoint(const o2session *s)
{
    return s->boip_endpoint[0] ? s->boip_endpoint : "127.0.0.1:11520";
}

int o2session_cart_link_up(o2session *s)
{
    o2_cart_status_t st;
    if (!s->running)
        return -1;
    o2_host_cart_status(&st);
    return st.link_up ? 1 : 0;
}

int o2session_cart_status(o2session *s, char *dst, int dstsz)
{
    o2_cart_status_t st;

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!s->running)
        return snprintf(dst, (size_t)dstsz, "stopped");
    o2_host_cart_status(&st);
    if (!st.powered)
        return snprintf(dst, (size_t)dstsz, "starting");
    if (st.boot_state == FN_BOOT_XFER)
        return snprintf(dst, (size_t)dstsz, "receiving %d%%", st.boot_pct);
    if (!st.mailbox_mode || (st.booted_game && st.mode == FN_MODE_GAME))
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    if (st.link_up)
        return snprintf(dst, (size_t)dstsz, st.busy ? "connected (busy)" : "connected");
    return snprintf(dst, (size_t)dstsz, "link down");
}

int o2session_cart_booted_game(o2session *s)
{
    o2_cart_status_t st;
    if (!s->running)
        return 0;
    o2_host_cart_status(&st);
    return st.booted_game ? 1 : 0;
}

/* ---- paths ----------------------------------------------------------------- */

const char *o2session_config_path(const o2session *s) { return s->config_dir; }
const char *o2session_data_path(const o2session *s) { return s->data_dir; }
const char *o2session_carts_path(const o2session *s) { return s->carts_dir; }
const char *o2session_sd_path(const o2session *s) { return s->fujinet_sd; }

/* ---- debugger -------------------------------------------------------------- */

o2debug *o2session_debugger(o2session *s)
{
    return o2debug_get(s);
}

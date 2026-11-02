/*
 * session.c -- the frontend contract (core/include/o2session.h) over the
 * Magnavox Odyssey 2 / Philips Videopac host (core/o2/host.h).
 *
 * The C modules beside it (settings, paths, media, roms, keyboard, audio,
 * gamepads, bindings) share the session struct through session_internal.h;
 * this file owns the lifecycle, the power cycles, the cartridges, the input
 * routing and the cartridge status. Ported from the Studio II sibling's
 * session.c.
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
#include "carts.h"
#include "fuji_mailbox.h"
#include "o2map.h"

/* The largest image the cartridge serves: an APPBANK image (a fixed 1K and up
 * to 31 pages of 2K), with room to say "too big" rather than truncate. */
#define CART_MAX (O2_APP_FIXED_BYTES + O2_APP_MAXPAGES * O2_APP_PAGE_BYTES + 1u)

/* ---- the machines ----------------------------------------------------------- */

/* o2_machine_name() and o2_machine_is_pal() are the machine's (machine.c). */

static int clamp_machine(int machine)
{
    return machine >= 0 && machine < O2_MACHINE_COUNT ? machine : O2_MACHINE_ODYSSEY2;
}

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
    pthread_mutex_init(&s->input_mtx, NULL);

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
    pthread_mutex_destroy(&s->input_mtx);
    free(s);
}

void o2session_default_opts(o2session *s, o2session_start_opts *opts)
{
    memset(opts, 0, sizeof *opts);
    opts->machine = clamp_machine(o2session_get_int(s, "machine", O2_MACHINE_ODYSSEY2));
    opts->use_bios = o2session_get_int(s, "use_bios", 1);
    opts->voice = o2session_get_int(s, "voice", 0);
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

/* ---- images: what a cartridge file is ---------------------------------------- */

typedef struct {
    int app;                  /* a FujiNet app: claims the mailbox */
    int type;                 /* o2_cart_type of a game (O2_CART_FUJINET for an app) */
    bool needs_voice;         /* the game carries speech data of its own */
    o2map_plan_t plan;        /* what the FujiNet cartridge makes of the image */
    o2map_err_t plan_err;
} cart_info;

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
                 "the image is larger than the cartridge can serve (%u bytes)",
                 (unsigned)(CART_MAX - 1));
        return NULL;
    }
    *len = (uint32_t)got;
    return buf;
}

/* Identify an image. A FujiNet app is one the cartridge firmware's own
 * planner says claims the mailbox (an APPBANK image, or a bank layout with
 * "FUJI" at $F2C); anything else is a game, which is MAME's cartridge for its
 * CRC and size (the mapper the host puts in the slot). 1 if the image can
 * run, else 0 and why (the Chess and Home Computer modules, a mapper that
 * cannot take the size). */
static int analyse_image(const uint8_t *img, uint32_t len, cart_info *ci,
                         char *why, int whysz)
{
    const char *err = NULL;
    o2_cart_t tmp;

    memset(ci, 0, sizeof *ci);
    if (why && whysz > 0)
        why[0] = '\0';
    ci->plan_err = o2map_plan(img, len, &ci->plan);
    if (ci->plan_err == O2MAP_OK && ci->plan.mailbox_ok) {
        ci->app = 1;
        ci->type = O2_CART_FUJINET;
        return 1;
    }
    ci->type = o2_cart_identify(img, len, &ci->needs_voice);
    if (o2_cart_init(&tmp, ci->type, img, len, &err) != 0) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err ? err : "the cartridge cannot be mapped");
        return 0;
    }
    return 1;
}

static int check_image(const char *path, cart_info *out, char *why, int whysz)
{
    uint8_t *img;
    uint32_t len = 0;
    char err[256];
    char abs[O2_PATH_MAX];
    cart_info ci;
    int ok;

    if (why && whysz > 0)
        why[0] = '\0';
    memset(&ci, 0, sizeof ci);
    if (out)
        *out = ci;
    if (!path || !*path)
        return 0;
    path = paths_resolve(path, abs, sizeof abs);
    img = read_cart(path, &len, err, sizeof err);
    if (!img) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "%s", err);
        return 0;
    }
    ok = analyse_image(img, len, &ci, why, whysz);
    free(img);
    if (out)
        *out = ci;
    return ok;
}

int o2session_check_cart(const char *path, char *why, int whysz)
{
    return check_image(path, NULL, why, whysz);
}

int o2session_cart_is_app(const char *path)
{
    cart_info ci;
    return check_image(path, &ci, NULL, 0) && ci.app;
}

/* Whether the FujiNet cartridge itself can serve the image (a network boot of
 * it from the SD folder): a whole number of 2K or 3K banks, or an APPBANK
 * image. A game of any other size still runs from the slot as its own
 * cartridge, so check_cart allows it; the SD folder is for what CONFIG can
 * boot. */
int session_cart_plan_check(const char *path, char *why, int whysz)
{
    cart_info ci;

    if (!check_image(path, &ci, why, whysz))
        return 0;
    if (ci.plan_err != O2MAP_OK) {
        if (why && whysz > 0)
            snprintf(why, (size_t)whysz, "the FujiNet cartridge cannot serve it: %s",
                     o2map_strerror(ci.plan_err));
        return 0;
    }
    return 1;
}

/* What the console would be missing to run this image (NULL: just power on)
 * with the ROMs imported now: 1 if it can, else 0 and why; *recoverable is
 * set when importing a ROM fixes it. */
static int requirements(struct o2session *s, int machine, int use_bios,
                        const cart_info *ci, int *recoverable, char *why, int whysz)
{
    const int plus = machine == O2_MACHINE_VIDEOPAC_PLUS;

    *recoverable = 0;
    if (plus && !o2session_have_bios(s, machine)) {
        *recoverable = 1;
        snprintf(why, (size_t)whysz,
                 "The Videopac+ G7400 needs its BIOS: Machine > Import BIOS...");
        return 0;
    }
    if (ci && !ci->app && !(plus || (use_bios && o2session_have_bios(s, machine)))) {
        *recoverable = 1;
        if (!use_bios && o2session_have_bios(s, machine))
            snprintf(why, (size_t)whysz,
                     "This cartridge needs the BIOS, which is switched off in "
                     "Preferences (Machine > Use the imported BIOS).");
        else
            snprintf(why, (size_t)whysz,
                     "This cartridge needs the BIOS. Use Machine > Import BIOS... to add "
                     "your own copy (CONFIG and the FujiNet apps run without it).");
        return 0;
    }
    if (ci && ci->needs_voice && !(o2session_have_voice(s) &&
        o2session_rom_item_available(s, O2_ROM_VOICE_SID))) {
        *recoverable = 1;
        snprintf(why, (size_t)whysz,
                 "This cartridge speaks through The Voice and needs its speech ROMs "
                 "(sp0256b-019.bin, spr128-003.bin and spr128-004.bin): Machine > Import BIOS...");
        return 0;
    }
    return 1;
}

/* ---- powering the machine -------------------------------------------------------- */

static void free_host_roms(struct o2session *s)
{
    for (size_t i = 0; i < sizeof s->host_roms / sizeof s->host_roms[0]; i++) {
        free(s->host_roms[i]);
        s->host_roms[i] = NULL;
    }
}

/* Bring the machine up with s->opts and the given image (NULL: CONFIG). */
static int host_up(struct o2session *s, const char *cart_path)
{
    o2_host_opts_t ho;
    cart_info ci;
    uint8_t *cart = NULL, *bios = NULL, *font = NULL, *font_ef = NULL;
    uint8_t *vrom = NULL, *vspeech = NULL, *vgame = NULL;
    uint32_t cart_len = 0, bios_len = 0, font_len = 0, font_ef_len = 0;
    uint32_t vrom_len = 0, vspeech_len = 0, vgame_len = 0;
    const int machine = s->opts.machine;
    const int plus = machine == O2_MACHINE_VIDEOPAC_PLUS;
    unsigned missing = 0;
    char why[256];
    int rc, recoverable = 0;

    memset(&ho, 0, sizeof ho);
    memset(&ci, 0, sizeof ci);
    free_host_roms(s);
    s->refusal_recoverable = 0;

    if (cart_path && *cart_path) {
        cart = read_cart(cart_path, &cart_len, why, sizeof why);
        if (!cart) {
            session_set_error(s, "%s", why);
            return -1;
        }
        if (!analyse_image(cart, cart_len, &ci, why, sizeof why)) {
            session_set_error(s, "%s", why);
            free(cart);
            return -1;
        }
    }
    if (!requirements(s, machine, s->opts.use_bios, cart ? &ci : NULL, &recoverable,
                      why, sizeof why)) {
        session_set_error(s, "%s", why);
        s->refusal_recoverable = recoverable;
        free(cart);
        return -1;
    }

    /* the console's BIOS, or none: the stand-in runs CONFIG and the apps */
    if (plus || s->opts.use_bios)
        bios = roms_load(s, plus ? O2_ROM_BIOS_G7400 : O2_ROM_BIOS, &bios_len);
    if ((plus || (cart && !ci.app)) && !bios) {
        session_set_error(s, "The BIOS could not be read from %s", s->roms_dir);
        free(cart);
        return -1;
    }

    /* the video chips' character sets: without them text is blank, which the
     * missing mask says */
    font = roms_load(s, O2_ROM_FONT_8244, &font_len);
    if (plus)
        font_ef = roms_load(s, O2_ROM_FONT_EF9340, &font_ef_len);
    if (!font || (plus && !font_ef))
        missing |= O2_MISSING_FONT;

    /* The Voice, in front of the cartridge: asked for in Preferences, or a
     * game that speaks needs it (requirements() has seen to its ROMs) */
    if (s->opts.voice || ci.needs_voice) {
        vrom = roms_load(s, O2_ROM_VOICE_INTERNAL, &vrom_len);
        vspeech = roms_load(s, O2_ROM_VOICE_SPEECH, &vspeech_len);
        if (ci.needs_voice)
            vgame = roms_load(s, O2_ROM_VOICE_SID, &vgame_len);
        if (vrom && vspeech && (vgame || !ci.needs_voice)) {
            ho.voice = true;
        } else {
            missing |= O2_MISSING_VOICE;
            free(vrom); free(vspeech); free(vgame);
            vrom = vspeech = vgame = NULL;
        }
    }

    s->host_roms[0] = bios;
    s->host_roms[1] = font;
    s->host_roms[2] = font_ef;
    s->host_roms[3] = vrom;
    s->host_roms[4] = vspeech;
    s->host_roms[5] = vgame;

    /* The cartridge dials FujiNet's BoIP port -- the in-process runtime's,
     * or a standalone fujinet-pc's when the runtime is switched off or
     * external. A session whose FujiNet is disabled outright (fujinet_lib "",
     * the tests) dials nothing, so it can never pick up someone else's FujiNet
     * (and Windows' two-second refusal of a closed port never holds up
     * power-on). */
    if (s->fujinet_disabled)
        s->boip_endpoint[0] = '\0';
    ho.machine = machine;
    ho.bios = bios;
    ho.bios_size = bios_len;
    ho.font = font;
    ho.font_size = font_len;
    ho.font_ef = font_ef;
    ho.font_ef_size = font_ef_len;
    ho.voice_rom = vrom;
    ho.voice_rom_size = vrom_len;
    ho.voice_speech = vspeech;
    ho.voice_speech_size = vspeech_len;
    ho.voice_game = vgame;
    ho.voice_game_size = vgame_len;
    ho.cart = cart;
    ho.cart_size = cart_len;
    ho.cart_type = cart ? ci.type : O2_CART_FUJINET;
    ho.cart_app = cart ? ci.app != 0 : true;
    ho.boip_hostport = s->boip_endpoint;
    ho.cart_sync = getenv("O2_CART_SYNC") != NULL;

    rc = o2_host_start(&ho);
    if (rc != 0) {
        session_set_error(s, "%s", o2_host_last_error());
        free_host_roms(s);
    } else {
        s->run_machine = machine;
        s->run_game = cart && !ci.app;
        s->run_missing = missing;
    }
    free(cart);
    return rc;
}

/* The machine options the settings name now: an Import BIOS or a Preferences
 * change takes effect at the next power cycle, which reads them here. */
static void reload_machine_opts(struct o2session *s)
{
    s->opts.machine = clamp_machine(o2session_get_int(s, "machine", s->opts.machine));
    s->opts.use_bios = o2session_get_int(s, "use_bios", s->opts.use_bios);
    s->opts.voice = o2session_get_int(s, "voice", s->opts.voice);
}

/* A power cycle: the machine and the cartridge come up fresh; FujiNet stays
 * up. `cart_path` NULL boots CONFIG. */
static int power_cycle(struct o2session *s, const char *cart_path)
{
    int rc;

    reload_machine_opts(s);

    o2debug_before_power_cycle(s);
    o2_host_stop();
    rc = host_up(s, cart_path);
    if (rc != 0 && cart_path) {
        /* whatever was running is gone: put CONFIG back */
        char err[256];
        int recoverable = s->refusal_recoverable;
        snprintf(err, sizeof err, "%s", s->last_error);
        host_up(s, NULL);
        snprintf(s->last_error, sizeof s->last_error, "%s", err);
        s->refusal_recoverable = recoverable;
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
    s->opts.machine = clamp_machine(opts->machine);
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
    char why[256];
    int recoverable;

    if (s->running)
        return 0;
    s->last_error[0] = '\0';

    if (!opts) {
        o2session_default_opts(s, &local);
        opts = &local;
    }
    adopt_opts(s, opts);

    /* a Videopac+ G7400 cannot run on the stand-in: say so before anything
     * else starts */
    if (!requirements(s, s->opts.machine, s->opts.use_bios, NULL, &recoverable,
                      why, sizeof why)) {
        session_set_error(s, "%s", why);
        return -1;
    }

    memset(s->pad_src, 0, sizeof s->pad_src);
    memset(s->key_src, 0, sizeof s->key_src);
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

    /* A remembered cartridge that no longer loads (moved, an image the
     * console cannot take, a ROM since deleted) must not leave the app
     * unbootable: fall back to CONFIG and say why. The setting is kept when an
     * import would fix it, so the cartridge is back after one. */
    if (host_up(s, s->opts.cart_path) != 0) {
        if (s->opts.cart_path) {
            const int keep = s->refusal_recoverable;
            fprintf(stderr, "o2: %s: %s; booting CONFIG\n", s->cart_path, s->last_error);
            if (keep) {
                s->cart_path[0] = '\0';
                s->opts.cart_path = NULL;
            } else
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
    free_host_roms(s);
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

int o2session_load_cart(o2session *s, const char *path)
{
    char why[256];
    char abs[O2_PATH_MAX];
    cart_info ci;
    int recoverable;

    if (!s->running || !path || !*path)
        return -1;
    path = paths_resolve(path, abs, sizeof abs);
    if (!check_image(path, &ci, why, sizeof why)) {
        session_set_error(s, "%s", why);
        s->refusal_recoverable = 0;
        return -1;
    }
    /* A game is BIOS byte-code (the stand-in cannot run it) and a speaking
     * one needs The Voice: refuse before touching the running machine. The
     * power cycle reads the machine settings, so judge by them. */
    if (!requirements(s, clamp_machine(o2session_get_int(s, "machine", s->opts.machine)),
                      o2session_get_int(s, "use_bios", s->opts.use_bios), &ci, &recoverable,
                      why, sizeof why)) {
        session_set_error(s, "%s", why);
        s->refusal_recoverable = recoverable;
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

int o2session_reset(o2session *s)
{
    if (!s->running)
        return -1;
    o2_host_reset_pulse();
    return 0;
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

/* What the running configuration lacks: the stand-in met a game (the host
 * saw it), and what was not handed to the host at power-up. Both clear at the
 * next power cycle that has the ROMs. */
unsigned o2session_missing(o2session *s)
{
    if (!s->running)
        return 0;
    return (o2_host_missing() & O2_MISSING_BIOS) | s->run_missing;
}

int o2session_needs_bios(o2session *s)
{
    return (o2session_missing(s) & O2_MISSING_BIOS) != 0;
}

void o2session_set_paused(o2session *s, int paused)
{
    if (s->running)
        o2_host_set_paused(paused != 0);
}

int o2session_is_paused(const o2session *s)
{
    return s->running && o2_host_is_paused();
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
    const int machine = s->running ? s->run_machine
        : clamp_machine(o2session_get_int(s, "machine", O2_MACHINE_ODYSSEY2));
    return o2_machine_is_pal(machine) ? 50 : 60;
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

/* The console's view of a stick: a real one is a set of switches, so
 * opposite directions cancel (MAME's inputs are 8-way for the same reason).
 * input_mtx held. */
static unsigned stick_mask_locked(const struct o2session *s, int port)
{
    unsigned mask = 0;

    for (int a = 0; a < O2_ACT_PER_PORT; a++)
        if (s->pad_src[port][a])
            mask |= 1u << a;
    if ((mask & (1u << O2_ACT_UP)) && (mask & (1u << O2_ACT_DOWN)))
        mask &= ~((1u << O2_ACT_UP) | (1u << O2_ACT_DOWN));
    if ((mask & (1u << O2_ACT_LEFT)) && (mask & (1u << O2_ACT_RIGHT)))
        mask &= ~((1u << O2_ACT_LEFT) | (1u << O2_ACT_RIGHT));
    return mask;
}

static void push_stick_locked(struct o2session *s, int port)
{
    /* o2_action's order is the host's bit order: UP RIGHT DOWN LEFT BUTTON */
    o2_host_stick_set(port, (uint8_t)stick_mask_locked(s, port));
}

static void push_matrix_locked(struct o2session *s)
{
    uint64_t matrix = 0;

    for (int k = 0; k < O2_KEY_COUNT; k++)
        if (s->key_src[k])
            matrix |= 1ull << k;
    o2_host_keys_set(matrix);
}

static void push_reset_locked(struct o2session *s)
{
    o2_host_reset_set(s->sw_src[O2_SW_RESET] != 0);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one target; sysactions are not held inputs and are left to
 * the caller. */
static void source_press(struct o2session *s, int target, uint32_t src, int down)
{
    pthread_mutex_lock(&s->input_mtx);
    if (target < 2 * O2_ACT_PER_PORT) {
        const int port = target / O2_ACT_PER_PORT;
        set_src(&s->pad_src[port][target % O2_ACT_PER_PORT], src, down);
        push_stick_locked(s, port);
    } else if (target < O2_TARGET_SWITCH(0)) {
        set_src(&s->key_src[target - O2_TARGET_KEY(0)], src, down);
        push_matrix_locked(s);
    } else if (target < O2_TARGET_SYSACT(0)) {
        set_src(&s->sw_src[target - O2_TARGET_SWITCH(0)], src, down);
        push_reset_locked(s);
    }
    pthread_mutex_unlock(&s->input_mtx);
}

void o2session_press(o2session *s, int target, int down)
{
    if (!s->running || target < 0 || target >= O2_TARGET_COUNT)
        return;
    if (target >= O2_TARGET_SYSACT(0)) {
        if (down)
            o2session_sysaction(s, target - O2_TARGET_SYSACT(0));
        return;
    }
    source_press(s, target, SRC_KEYBOARD, down);
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
    unsigned held;
    if (port < 0 || port > 1)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = stick_mask_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

int o2session_key_held(o2session *s, int key)
{
    int held;
    if (key < 0 || key >= O2_KEY_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = s->key_src[key] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

int o2session_switch_held(o2session *s, int sw)
{
    int held;
    if (sw < 0 || sw >= O2_SW_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    held = s->sw_src[sw] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

/* Everything the keyboard and the controllers window hold, let go (focus
 * loss): what a key's press asserted, whichever way it was asserted. A
 * gamepad's own holds are left to the gamepad. */
static void release_keyboard_sources(struct o2session *s)
{
    pthread_mutex_lock(&s->input_mtx);
    for (int port = 0; port < 2; port++) {
        for (int a = 0; a < O2_ACT_PER_PORT; a++)
            s->pad_src[port][a] &= ~SRC_KEYBOARD;
        push_stick_locked(s, port);
    }
    for (int k = 0; k < O2_KEY_COUNT; k++)
        s->key_src[k] &= ~SRC_KEYBOARD;
    push_matrix_locked(s);
    for (int w = 0; w < O2_SW_COUNT; w++)
        s->sw_src[w] &= ~SRC_KEYBOARD;
    push_reset_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

void o2session_release_all(o2session *s)
{
    if (!s)
        return;
    memset(s->held_keysym, 0, sizeof s->held_keysym);
    release_keyboard_sources(s);
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the debugger. */
void session_gamepad_press(struct o2session *s, int owner, int target, int down)
{
    if (!s->running || owner < 0 || owner >= O2_MAX_PADS || target < 0
        || target >= O2_TARGET_SYSACT(0))
        return;
    source_press(s, target, SRC_PAD(owner), down);
}

void session_gamepad_stick(struct o2session *s, int owner, int port, int act, int down)
{
    if (!s->running || owner < 0 || owner >= O2_MAX_PADS || port < 0 || port > 1
        || act < 0 || act >= O2_ACT_PER_PORT)
        return;
    source_press(s, O2_TARGET_PORT(port, act), SRC_STICK(owner), down);
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
    /* a game's own cartridge has no mailbox and no FujiNet behind it */
    if (s->run_game)
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    o2_host_cart_status(&st);
    if (!st.powered)
        return snprintf(dst, (size_t)dstsz, "starting");
    if (st.boot_state == FN_BOOT_XFER)
        return snprintf(dst, (size_t)dstsz, "loading %d%%", st.boot_pct);
    if (!st.mailbox)
        return snprintf(dst, (size_t)dstsz, "game running; mailbox closed");
    if (st.link_up)
        return snprintf(dst, (size_t)dstsz,
                        (st.status & FN_R_STATUS_BUSY) ? "connected (busy)" : "connected");
    return snprintf(dst, (size_t)dstsz, "link down");
}

int o2session_cart_booted_game(o2session *s)
{
    o2_cart_status_t st;
    if (!s->running)
        return 0;
    if (s->run_game)
        return 1;
    o2_host_cart_status(&st);
    return st.booted_game ? 1 : 0;
}

int o2session_cart_type_name(o2session *s, char *dst, int dstsz)
{
    o2_cart_status_t st;
    o2_cart_t *c;
    o2_machine_t *m;
    int type = O2_CART_NONE, voice = 0;
    uint32_t size = 0;
    char what[96];

    if (!dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!s->running)
        return 0;

    o2_host_cart_status(&st);
    o2_host_lock();
    c = o2_host_cart();
    if (c) {
        type = c->type;
        size = c->rom_size;
        if (c->type == O2_CART_VOICE && c->sub) {
            type = c->sub->type;
            size = c->sub->rom_size;
            voice = 1;
        }
    }
    m = o2_host_machine();
    if (m && m->have_voice)
        voice = 1;
    o2_host_unlock();

    if (s->run_game || (type != O2_CART_FUJINET && type != O2_CART_NONE)) {
        if (size >= 1024)
            snprintf(what, sizeof what, "%s, %u KB", o2_cart_type_name(type),
                     (unsigned)((size + 1023) / 1024));
        else
            snprintf(what, sizeof what, "%s", o2_cart_type_name(type));
    } else if (!st.booted_game) {
        snprintf(what, sizeof what, "FujiNet CONFIG");
    } else if (st.mailbox) {
        snprintf(what, sizeof what, "FujiNet app");
    } else {
        snprintf(what, sizeof what, "FujiNet cartridge, game");
    }
    return snprintf(dst, (size_t)dstsz, "%s%s", voice ? "The Voice + " : "", what);
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

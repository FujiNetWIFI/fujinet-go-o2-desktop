/*
 * o2session's private state. Not installed; only the core/src sources
 * include it. One struct shared by the session's C modules (settings,
 * paths, media, roms, audio, gamepads, bindings).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_SESSION_INTERNAL_H
#define O2_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "o2session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define O2_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct o2session {
    char config_dir[O2_PATH_MAX];
    char data_dir[O2_PATH_MAX];
    char carts_dir[O2_PATH_MAX];
    char roms_dir[O2_PATH_MAX];       /* <data>/roms: the imported ROMs */
    char settings_file[O2_PATH_MAX];

    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;

    char last_error[256];

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    char fujinet_root[O2_PATH_MAX];    /* <data>/fujinet */
    char fujinet_config[O2_PATH_MAX];  /* .../fnconfig.ini */
    char fujinet_sd[O2_PATH_MAX];      /* .../SD */
    char fujinet_data[O2_PATH_MAX];    /* .../data */
    char fujinet_lib[O2_PATH_MAX];     /* resolved libfujinet path, "" until then */
    int  fujinet_disabled;              /* the caller passed fujinet_lib "" */
    char fujinet_runtime_src[O2_PATH_MAX]; /* caller-given pristine tree, or "" */
    char webui_url[64];                 /* http://127.0.0.1:11521/ */
    int  fujinet_running;
    int  fujinet_external;              /* dialing a FujiNet that isn't ours */
    char boip_endpoint[80];             /* "host:port" the cartridge dials */
    char boip_addr[80];                 /* the external address as typed */

    /* cross-thread system-action latch (see o2session_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the running configuration */
    o2session_start_opts opts;
    char cart_path[O2_PATH_MAX];
    int run_machine;                   /* o2_machine the host was started with */
    int run_game;                      /* the slot holds a game's own cartridge
                                        * (a mapper), not the FujiNet cart */
    unsigned run_missing;              /* O2_MISSING_FONT / _VOICE of what was
                                        * handed to the host at power-up */
    int refusal_recoverable;           /* the last refused power-up lacked a ROM
                                        * an import would supply */
    /* the ROM images handed to the host: the 8244 keeps a pointer to its
     * character set, so they live until the host stops (see roms.c) */
    uint8_t *host_roms[6];

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[O2_TARGET_COUNT];

    /* What drives each stick direction, matrix key and console button, one
     * bit per source (SRC_*), so that letting go of one source never
     * releases another's. input_mtx covers these arrays and the calls that
     * hand the result to the host. */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[2][O2_ACT_PER_PORT];
    uint32_t key_src[O2_KEY_COUNT];
    uint32_t sw_src[O2_SW_COUNT];

    void *audio;              /* audio_sdl.c state, NULL until started */
    void *gamepad;            /* gamepad_sdl.c state, NULL until started */
    void *debugger;           /* o2debug, lazily created */
    int running;

    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct o2session *s);
void settings_free_all(struct o2session *s);

int paths_init(struct o2session *s, const char *config_dir,
               const char *data_dir);
/* The in-process FujiNet makes its runtime root the process's working
 * directory (its PC firmware opens data/ relative to it), so a path a user
 * gave relative to where they started the app -- a command-line argument --
 * is resolved against the directory the first session was created in.
 * paths_resolve returns `path` itself when it is absolute, else `buf`. */
void paths_capture_launch_dir(void);
const char *paths_resolve(const char *path, char *buf, size_t bufsz);
/* Locate libfujinet and provision the runtime tree (fnconfig.ini + data/ +
 * SD/) into <data>/fujinet on first run. Returns 0, or -1 if no runtime is
 * available (not fatal to the session -- see fujinet_start). */
int paths_provision_fujinet(struct o2session *s);

void session_set_error(struct o2session *s, const char *fmt, ...);
/* 1 if the FujiNet cartridge can serve the image (plans it), else 0 and why. */
int  session_cart_plan_check(const char *path, char *why, int whysz);

/* fujinet_runtime.c */
int  fujinet_start(struct o2session *s);
void fujinet_stop(struct o2session *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the cartridge's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct o2session *s, int timeout_ms);

/* Who is holding an input down (the bits of the *_src arrays): the keyboard
 * and the controllers window together, and for each gamepad (by the stable
 * owner number its slot was given, 0 to O2_MAX_PADS - 1) its buttons and its
 * analog stick / hat separately. So letting go of one never releases another,
 * even two pads or two buttons bound to the one key. */
#define O2_MAX_PADS  8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + O2_MAX_PADS + (owner)))

/* paths.c: mkdir -p. */
int paths_mkdir_p(const char *path);

/* roms.c */
/* Write any images compiled in with WITH_O2_ROMS into the ROM directory
 * (once; an existing file is left alone). */
void roms_provision_embedded(struct o2session *s);
/* ROM item `id` (an o2_rom_id) from the ROM directory or the embedded table,
 * at its exact size; a malloc'd buffer (caller frees) and its size, or NULL
 * when it is not there. */
uint8_t *roms_load(struct o2session *s, int id, uint32_t *size);
/* o2session_rom_import, plus the first file it wrote (for import_media). */
int roms_import_ex(struct o2session *s, const char *path, char *msg, int msgsz,
                   char *first_dest, int destsz);

/* audio_sdl.c */
int  audio_start(struct o2session *s);
void audio_stop(struct o2session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct o2session *s);
void gamepad_stop(struct o2session *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a target (a stick direction or button, a matrix key, RESET)
 * -- the pad's SRC_PAD source. stick: the analog stick or hat drives a stick
 * direction -- its SRC_STICK source. */
void session_gamepad_press(struct o2session *s, int owner, int target, int down);
void session_gamepad_stick(struct o2session *s, int owner, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(struct o2session *s, const char *text);

/* debug.c: the session tells the debugger when the machine goes away and
 * comes back (a power cycle replaces it), and starts the FujiNet trace
 * before the first. */
void o2debug_trace_fujinet(struct o2session *s);
void o2debug_before_power_cycle(struct o2session *s);
void o2debug_after_power_cycle(struct o2session *s);
void o2debug_destroy(struct o2session *s);

#ifdef __cplusplus
}
#endif

#endif /* O2_SESSION_INTERNAL_H */

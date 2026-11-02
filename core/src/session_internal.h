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
    char roms_dir[O2_PATH_MAX];       /* <data>/roms: the imported BIOS */
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

    /* keys the keyboard currently holds, by target, so a release clears
     * exactly what its press asserted even if the binding changed meanwhile */
    uint32_t held_keysym[O2_TARGET_COUNT];

    /* what drives each keypad key and console button: the keyboard and
     * the keypad window (bit 0) and each keypad's gamepad (bit 1),
     * combined so letting go of one never releases the other */
    uint8_t pad_src[2][O2_ACT_PER_PORT];
    uint8_t sw_src[O2_SW_COUNT];

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

/* fujinet_runtime.c */
int  fujinet_start(struct o2session *s);
void fujinet_stop(struct o2session *s);
/* Block (up to timeout_ms) until the BoIP port accepts, so the cartridge's
 * first dial-out finds the listener. Returns 0 once up, -1 on timeout. */
int  fujinet_wait_for_boip(struct o2session *s, int timeout_ms);

/* roms.c */
/* Write any images compiled in with WITH_O2_ROMS into the ROM directory
 * (once; an existing file is left alone). */
void roms_provision_embedded(struct o2session *s);
/* The BIOS as the console maps it ($0000-$07FF), assembled from what is
 * imported: the 2K set, or its ROMs (ic11 and ic12 at least); a malloc'd
 * buffer (caller frees) and its size, or NULL when there is none. */
uint8_t *roms_load_bios(struct o2session *s, uint32_t *size);

/* audio_sdl.c */
int  audio_start(struct o2session *s);
void audio_stop(struct o2session *s);

/* gamepad_sdl.c */
int  gamepad_start(struct o2session *s);
void gamepad_stop(struct o2session *s);
/* Called by the gamepad thread with the button/axis state it resolved. */
void session_gamepad_apply(struct o2session *s, int port, int act, int down);
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

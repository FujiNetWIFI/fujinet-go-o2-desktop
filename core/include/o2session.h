/*
 * o2session -- the toolkit-agnostic desktop session for FujiNet Go Odyssey 2.
 *
 * Owns the emulator (the Magnavox Odyssey 2 core on its own paced thread -- see
 * core/o2/host.h), the FujiNet cartridge, the SDL audio and gamepad
 * backends, the in-process FujiNet runtime, the shared settings store, the
 * remappable key/pad bindings, the imported BIOS and the media path layout. Frontends (GTK4, Qt6, AppKit, Win32) drive this API and do only
 * windowing, painting and event translation. A frontend that needs
 * something which is not one of those three things belongs here instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2SESSION_H
#define O2SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MAME's visible area: 100 x 246 (the CDP1861's 64 x 128 picture inside
 * the blanking it draws nothing into). Frontends show it at the TV's 4:3
 * or with square pixels, per the "aspect" setting. */
#define O2SESSION_FB_WIDTH  100
#define O2SESSION_FB_HEIGHT 246

/* FujiNet's BoIP listener and its web admin UI. High ports of this app's own
 * so a standalone fujinet-pc, or a sibling FujiNet Go app, never collides:
 * ADAM uses 65216/65214, Apple II 1985/64001, CoCo 65504/64002,
 * Intellivision 65503/64003, MSX 65505/64003, Astrocade 11500/11501,
 * ColecoVision 11502/11503, Atari 2600 11504/11505, NES 11506/11507, SMS
 * 11508/11521, Atari 7800 11510/11511, H89 11512/11513, Atari 800
 * 11514/11515, MS-DOS 11516/11517; the firmware's MAME dev harness for the
 * Odyssey 2 uses 9995/9997.
 *
 * Direction: FujiNet LISTENS and the Odyssey 2 FujiNet cartridge dials in, as
 * on every sibling. That decides startup ordering -- see o2session_start. */
#define O2SESSION_BOIP_PORT  11520
#define O2SESSION_WEBUI_PORT 11521

/* The host audio device rate; the core streams the beeper at it. */
#define O2SESSION_AUDIO_RATE 48000

/* The accent colour every frontend uses for its highlights (the Map target,
 * held keys, the debugger's current line, the FujiNet status dot): the
 * console case's tan, as on the icon. On the light themes it is too pale for
 * text, so a frontend that draws text in it uses the darker variant. */
#define O2SESSION_ACCENT_RGB      0xC11218
#define O2SESSION_ACCENT_DARK_RGB 0x942324

typedef struct o2session o2session;
typedef struct o2debug o2debug;

/* All members optional (NULL = default).
 *  config_dir:  default $XDG_CONFIG_HOME/fujinet-go-o2
 *  data_dir:    default $XDG_DATA_HOME/fujinet-go-o2
 *  fujinet_lib: path to libfujinet.so/.dylib/.dll; default searches
 *               $FUJINET_LIB, the executable's directory, the install
 *               libdir, then tools/fujinet/work/out. "" disables FujiNet.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini + data/
 *               + SD/ used to provision the user's runtime tree on first
 *               start (a macOS app passes its bundle's runtime dir). */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} o2session_paths;

o2session *o2session_new(const o2session_paths *paths);
void o2session_free(o2session *s);

/* ---- settings (shared INI; one store for every frontend of this target) --- */
int         o2session_get_int(o2session *s, const char *key, int def);
void        o2session_set_int(o2session *s, const char *key, int value);
const char *o2session_get_str(o2session *s, const char *key,
                               const char *def);
void        o2session_set_str(o2session *s, const char *key,
                               const char *value);
void        o2session_settings_flush(o2session *s);

/* ---- lifecycle ------------------------------------------------------------ */
typedef struct {
    const char *cart_path;    /* cartridge image; NULL boots the CONFIG client */
    int use_bios;             /* run the imported BIOS when there is one (else
                                 the stand-in, which runs FujiNet apps only) */
    int analog_joystick;      /* gamepad sticks drive the directions too */
    int enable_fujinet;       /* start the in-process FujiNet runtime */
    int external_fujinet;     /* 1: don't start it; dial another FujiNet
                               * instance at boip_connect instead */
    const char *boip_connect; /* "host:port" or "port" the cartridge dials
                               * (external only); NULL/"" = 127.0.0.1:11520 */
    int enable_audio;         /* open the SDL audio device */
    int enable_gamepad;       /* start the SDL gamepad thread */
} o2session_start_opts;

/* Fills opts from the settings store (keys: cart use_bios analog_joystick
 * enable_fujinet enable_audio enable_gamepad fujinet_external
 * fujinet_external_addr). */
void o2session_default_opts(o2session *s, o2session_start_opts *opts);

/* Starts FujiNet (if enabled) and then the machine.
 *
 * The order is not arbitrary: FujiNet listens and the cartridge dials in, so
 * the listener has to exist before the machine's first transaction or the
 * CONFIG client boots reporting no link. start() brings FujiNet up first and
 * waits briefly for the port.
 *
 * Returns 0, or -1 with o2session_last_error() set. FujiNet failing to
 * start is NOT fatal: the machine boots with the cartridge reporting the
 * link down, which is far more useful than refusing to run. */
int  o2session_start(o2session *s, const o2session_start_opts *opts);
void o2session_stop(o2session *s);
int  o2session_is_running(const o2session *s);
const char *o2session_last_error(const o2session *s);
/* Power-cycle with the current settings (stop + default_opts + start):
 * what Preferences does after a machine option changes. */
int  o2session_restart(o2session *s);

/* ---- cartridges ------------------------------------------------------------
 * The FujiNet cartridge is always in the slot; CONFIG is its resident image.
 *
 * load_cart puts a local image (.st2, or a raw .bin of up to 1K at $0400)
 * into the cartridge in place of CONFIG and power-cycles the console onto it,
 * as the firmware's MAME device serves $FUJINET_IMAGE: a FujiNet app (the
 * "FUJI" claim at $07FC) keeps the mailbox, a game runs alone. An image the
 * cartridge cannot plan is refused with the reason; so is a game while no
 * BIOS is imported (commercial games are BIOS byte-code: only the real BIOS
 * runs them). The path is remembered in the "cart" setting, so it boots
 * again next start. Returns 0 or -1 + error. */
int  o2session_load_cart(o2session *s, const char *path);
const char *o2session_cart_path(const o2session *s);
/* 1 if the image can run on the FujiNet cartridge; else 0 and why. */
int  o2session_check_cart(const char *path, char *why, int whysz);
/* 1 if the image is a FujiNet app (runs without the BIOS). */
int  o2session_cart_is_app(const char *path);

/* CLEAR: the console's only button. It holds the CPU in reset and resets the
 * 1861; the cartridge restarts whatever it is serving. Pressed and released
 * for a few frames (the keypad window and the menus); the bound key (F3,
 * MAME's) holds it for as long as it is down. */
int  o2session_clear(o2session *s);
/* Reset to CONFIG: a power cycle. Ejects an opened cartridge (clears the
 * "cart" setting) and boots CONFIG, exactly as at power-on -- on the
 * hardware, only taking the console's power away brings CONFIG back.
 * Escape by default. Every power cycle (this, Eject, Open Cartridge) takes
 * the BIOS as the settings name it now, so an imported BIOS boots here. */
int  o2session_reset_to_config(o2session *s);
/* Eject: the same as Reset to CONFIG (there is no "no cartridge" state
 * worth having on a FujiNet cartridge). */
int  o2session_eject(o2session *s);
/* 1 when the stand-in met a game it cannot run: no BIOS is imported and
 * CONFIG booted a game from the network. The frontends then say "Import
 * BIOS". Cleared by the next power cycle. */
int  o2session_needs_bios(o2session *s);

/* ---- video ---------------------------------------------------------------
 * copy_frame copies the latest frame into dst (FB_WIDTH*FB_HEIGHT uint32
 * XRGB8888 pixels) iff its serial differs from *serial_inout, updates it and
 * returns 1; returns 0 when unchanged, leaving dst alone. Pass 0 to force a
 * copy. */
int  o2session_copy_frame(o2session *s, uint32_t *dst, uint64_t *serial_inout);
/* Ask to be told when a new frame is ready, for a frontend with no frame
 * clock of its own: cb(ctx) runs on the machine thread after each frame is
 * published (a frame identical to the previous one is not published again).
 * Keep it quick (post an event and return) and do not call into the session
 * from it. Passing NULL stops the calls, and returns only after any call in
 * flight has finished, so ctx may be freed right after. */
typedef void (*o2session_frame_cb)(void *ctx);
void o2session_set_frame_callback(o2session *s, o2session_frame_cb cb, void *ctx);
/* Frames the machine has run since it started. Unlike copy_frame's serial it
 * counts every frame, including the ones identical to their predecessor:
 * use it to ask "is the machine running", not "is the picture changing". */
uint64_t o2session_frame_count(o2session *s);
/* The machine's refresh rate, rounded (60: 1.76 MHz / 29344 = 59.98 Hz). */
int  o2session_refresh_rate(o2session *s);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
 * stream near the console's refresh rate arrives, the machine phase-locks
 * one frame per tick; otherwise it paces on the wall clock. A frontend with
 * no frame clock simply never calls this. */
void o2session_notify_vsync(o2session *s, int64_t frame_time_ns);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend
 * that wants the device itself can pull interleaved stereo float frames at
 * O2SESSION_AUDIO_RATE instead. Returns the frames written (silence is
 * written for any shortfall). */
int  o2session_render_audio(o2session *s, float *out, int nframes);
void o2session_set_volume(o2session *s, int percent);

/* ---- input ---------------------------------------------------------------
 * Every control the machine has, flattened into one target index so the
 * bindings table, the settings store and the frontends can all name them:
 * per keypad (A, then B) its ten keys, then the console's CLEAR, then the
 * session's own actions. The console reads a keypad one key at a time (OUT 2
 * latches the key, EF3/EF4 report it), so any number may be held. */
typedef enum {
    O2_ACT_0 = 0, O2_ACT_1, O2_ACT_2, O2_ACT_3, O2_ACT_4,
    O2_ACT_5, O2_ACT_6, O2_ACT_7, O2_ACT_8, O2_ACT_9,
    O2_ACT_PER_PORT
} o2_action;

typedef enum {
    O2_SW_CLEAR = 0,                 /* F3 by default (MAME's) */
    O2_SW_COUNT
} o2_switch;

typedef enum {
    O2_SYSACT_RESET_CONFIG = 0,      /* Escape by default */
    O2_SYSACT_DEBUG_STOP,            /* stop in the debugger */
    O2_SYSACT_COUNT
} o2_sysaction;

#define O2_TARGET_PORT(port, act) ((port) * O2_ACT_PER_PORT + (act))
#define O2_TARGET_SWITCH(sw)      (2 * O2_ACT_PER_PORT + (sw))
#define O2_TARGET_SYSACT(sa)      (2 * O2_ACT_PER_PORT + O2_SW_COUNT + (sa))
#define O2_TARGET_COUNT           (2 * O2_ACT_PER_PORT + O2_SW_COUNT + O2_SYSACT_COUNT)

/* Apply one control directly. `down` is press/release. The console buttons
 * are held, as the console samples them once a frame. */
void o2session_press(o2session *s, int target, int down);
void o2session_sysaction(o2session *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void o2session_release_all(o2session *s);
/* What the console sees held on a keypad right now (keyboard, gamepads and
 * the keypad window together), one bit per o2_action. For the keypad
 * window's highlights. */
unsigned o2session_buttons_held(o2session *s, int port);
/* Whether a console button is held (o2_switch). */
int o2session_switch_held(o2session *s, int sw);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the HID tables below), so one bindings table serves every
 * frontend. Returns 1 if the key drives a target (and has been applied /
 * released), 0 if it should be ignored. System actions are reported through
 * o2session_key_sysaction instead and left to the frontend. */
int  o2session_key(o2session *s, uint32_t keysym, int down);
/* The system action a keysym is bound to, or -1. */
int  o2session_key_sysaction(o2session *s, uint32_t keysym);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    O2_KEYSYM_NONE = 0,
    O2_KEYSYM_UP = 0xff52, O2_KEYSYM_DOWN = 0xff54,
    O2_KEYSYM_LEFT = 0xff51, O2_KEYSYM_RIGHT = 0xff53,
    O2_KEYSYM_ESCAPE = 0xff1b, O2_KEYSYM_RETURN = 0xff0d,
    O2_KEYSYM_BACKSPACE = 0xff08, O2_KEYSYM_TAB = 0xff09,
    O2_KEYSYM_SPACE = 0x20,
    O2_KEYSYM_F1 = 0xffbe, O2_KEYSYM_F2, O2_KEYSYM_F3, O2_KEYSYM_F4,
    O2_KEYSYM_F5, O2_KEYSYM_F6, O2_KEYSYM_F7, O2_KEYSYM_F8,
    O2_KEYSYM_F9, O2_KEYSYM_F10, O2_KEYSYM_F11, O2_KEYSYM_F12,
    O2_KEYSYM_LSHIFT = 0xffe1, O2_KEYSYM_RSHIFT = 0xffe2,
    O2_KEYSYM_LCTRL = 0xffe3, O2_KEYSYM_RCTRL = 0xffe4,
    O2_KEYSYM_LALT = 0xffe9, O2_KEYSYM_RALT = 0xffea,
    O2_KEYSYM_KP_0 = 0xffb0, O2_KEYSYM_KP_1, O2_KEYSYM_KP_2,
    O2_KEYSYM_KP_3, O2_KEYSYM_KP_4, O2_KEYSYM_KP_5, O2_KEYSYM_KP_6,
    O2_KEYSYM_KP_7, O2_KEYSYM_KP_8, O2_KEYSYM_KP_9,
    O2_KEYSYM_KP_ENTER = 0xff8d, O2_KEYSYM_KP_MULTIPLY = 0xffaa,
    O2_KEYSYM_KP_DIVIDE = 0xffaf, O2_KEYSYM_KP_PERIOD = 0xffae
};
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym. 0
 * when unknown. */
uint32_t o2session_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t o2session_keysym_from_evdev(unsigned code);
uint32_t o2session_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int o2session_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. Persisted in the
 * settings store under "bindings" as only the entries that differ from the
 * defaults. */
typedef enum {
    O2_PAD_BTN_NONE = -1,
    O2_PAD_BTN_SOUTH = 0, O2_PAD_BTN_EAST, O2_PAD_BTN_WEST,
    O2_PAD_BTN_NORTH, O2_PAD_BTN_BACK, O2_PAD_BTN_GUIDE,
    O2_PAD_BTN_START, O2_PAD_BTN_LEFT_STICK, O2_PAD_BTN_RIGHT_STICK,
    O2_PAD_BTN_LEFT_SHOULDER, O2_PAD_BTN_RIGHT_SHOULDER,
    O2_PAD_BTN_DPAD_UP, O2_PAD_BTN_DPAD_DOWN, O2_PAD_BTN_DPAD_LEFT,
    O2_PAD_BTN_DPAD_RIGHT,
    O2_PAD_BTN_LEFT_TRIGGER, O2_PAD_BTN_RIGHT_TRIGGER,
    O2_PAD_BTN_COUNT,
    /* Raw joystick bands for devices SDL has no gamepad mapping for (a
     * plain HID adapter): button index, and hat directions (hat*8 + dir). */
    O2_PAD_BTN_RAW_BASE = 64, O2_PAD_BTN_RAW_LAST = 127,
    O2_PAD_HAT_BASE = 192, O2_PAD_HAT_LAST = 255
} o2_pad_button;
#define O2_PAD_HAT_DIRS 8
#define O2_PAD_BTN_IS_NAMED(b) ((b) >= 0 && (b) < O2_PAD_BTN_COUNT)
#define O2_PAD_BTN_IS_RAW(b)   ((b) >= O2_PAD_BTN_RAW_BASE && (b) <= O2_PAD_BTN_RAW_LAST)
#define O2_PAD_BTN_IS_HAT(b)   ((b) >= O2_PAD_HAT_BASE && (b) <= O2_PAD_HAT_LAST)

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* o2_pad_button, NONE = no gamepad button */
} o2_binding;

const char *o2_target_name(int target);           /* "Player 1: Up" */
const char *o2_target_short_name(int target);     /* "Up" */
o2_binding o2session_binding_get(o2session *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE unbinds. */
void o2session_binding_set_key(o2session *s, int target, uint32_t keysym,
                                char *stolen, int stolensz);
void o2session_binding_set_button(o2session *s, int target, int button,
                                   char *stolen, int stolensz);
void o2session_bindings_reset(o2session *s);
const char *o2_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int o2session_target_for_key(o2session *s, uint32_t keysym);
int o2session_target_for_button(o2session *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void o2session_gamepad_capture_begin(o2session *s);
void o2session_gamepad_capture_cancel(o2session *s);
int  o2session_gamepad_capture_poll(o2session *s, int *button);

void o2session_set_analog(o2session *s, int joystick);

/* ---- gamepads (SDL, hotplugged; started by o2session_start) -------------
 * Pads are assigned to ports in connection order unless assigned
 * explicitly. A pad that disconnects and reconnects gets its port back. */
int  o2session_gamepad_count(o2session *s);
int  o2session_gamepad_name(o2session *s, int idx, char *dst, int dstsz);
void o2session_gamepad_assign(o2session *s, int idx, int port); /* -1 = auto */
int  o2session_gamepad_assignment(o2session *s, int idx);
int  o2session_gamepad_effective_port(o2session *s, int idx);
/* Bumped on every add/remove so a frontend can refresh its lists cheaply. */
unsigned o2session_gamepad_generation(o2session *s);
/* The last hot-plug event as text ("Connected: 8BitDo SN30 Pro (player 1)",
 * "Disconnected: ..."), for a toast; returns length, 0 if none yet. */
int  o2session_gamepad_last_event(o2session *s, char *dst, int dstsz);

/* ---- cross-thread system actions ------------------------------------------
 * The gamepad thread cannot call into a UI toolkit, so a system action it
 * resolves is posted here and a frontend's timer takes it. */
void o2session_sysaction_post(o2session *s, int sysact);
int  o2session_sysaction_take(o2session *s, int *out);

/* ---- the BIOS --------------------------------------------------------------
 * Never shipped (see COMPLIANCE.md), and only needed for games: CONFIG and
 * FujiNet apps boot on the app's own stand-in. Import BIOS copies a user's
 * own dump into the ROM directory, identified by size and CRC-32 against
 * MAME's o2 set: the whole 2K, or one of its four 512-byte ROMs (ic11
 * and ic12 -- the system ROM -- are what the console runs; the cart always
 * hides the built-in games at $0400). */
typedef struct {
    const char *name;         /* "o2", "84932.ic11", ... */
    const char *file;         /* stored as */
    const char *desc;         /* "Magnavox Odyssey 2 BIOS (2K)" */
    uint32_t size, crc;
} o2_bios_info;

int o2session_bios_count(void);
const o2_bios_info *o2session_bios_info(int i);
/* 1 if image i is in the ROM directory. */
int o2session_bios_available(o2session *s, int i);
/* 1 if the console's system ROM can be assembled from what is imported. */
int o2session_have_bios(o2session *s);
/* Import a BIOS image. Returns its index (>= 0), or -1 with the error. A 2K
 * or 1K image whose CRC is not in the table is imported as the 2K set with
 * a warning in `msg`. `msg` always says what happened. It boots at the next
 * power cycle. */
int o2session_import_bios(o2session *s, const char *path, char *msg, int msgsz);
/* 1 if `path` is a BIOS image by size and CRC (a dropped file). */
int o2session_media_is_bios(const char *path);
const char *o2session_roms_path(const o2session *s);

/* ---- FujiNet -------------------------------------------------------------*/
/* Parses the "Use external FujiNet" address field: "host:port" (host a name
 * or an IPv4 literal) or a bare "port". Empty text means the default,
 * 127.0.0.1:11520. Returns 0 and fills host/port, or -1 when the text is not
 * an address:port (nothing is written then). */
int         o2session_parse_hostport(const char *text, char *host,
                                          int hostsz, int *port);
int         o2session_fujinet_running(const o2session *s);
/* 1 when the session was started to dial an external FujiNet. */
int         o2session_fujinet_external(const o2session *s);
/* "host:port" the cartridge dials (the running session's, or the default
 * 127.0.0.1:11520 before the first start). */
const char *o2session_boip_endpoint(const o2session *s);
const char *o2session_fujinet_webui_url(const o2session *s);
int         o2session_fujinet_copy_log(o2session *s, char *dst, int max);
/* The cartridge's link to FujiNet: 1 up, 0 down, -1 not running. */
int         o2session_cart_link_up(o2session *s);
/* Status text from the cartridge ("connected", "link down", "loading 42%",
 * "game running; mailbox closed", ...). Returns length. */
int         o2session_cart_status(o2session *s, char *dst, int dstsz);
/* 1 once anything other than CONFIG is in the cartridge (a network boot or
 * an opened cartridge file). */
int         o2session_cart_booted_game(o2session *s);

/* ---- media ---------------------------------------------------------------
 * Import a cartridge into FujiNet's SD folder so CONFIG can boot it through
 * the cartridge. Returns 0 and the
 * destination path, or -1 with the error. */
int  o2session_import_cart_to_sd(o2session *s, const char *src_path,
                                  char *dest_out, int dest_sz);
/* Routes a dropped file: cartridge images to the cartridge directory (the
 * returned path is usable with load_cart), BIOS images to the ROM directory
 * (as import_bios), anything else to the FujiNet SD folder. */
int  o2session_import_media(o2session *s, const char *src_path,
                             char *dest_out, int dest_sz);
int  o2session_media_is_cartridge(const char *path);

const char *o2session_config_path(const o2session *s);
const char *o2session_data_path(const o2session *s);
const char *o2session_carts_path(const o2session *s);
const char *o2session_sd_path(const o2session *s);

/* ---- debugger ------------------------------------------------------------*/
o2debug *o2session_debugger(o2session *s);

#ifdef __cplusplus
}
#endif

#endif /* O2SESSION_H */

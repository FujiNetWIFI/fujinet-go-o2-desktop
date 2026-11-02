/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the bindings editor and the settings store need.
 *
 * One flat target table (o2session.h's O2_TARGET_* indices): per
 * keypad (A, then B) its ten keys; then the console's CLEAR; then the
 * session's own actions. Ported from the SMS sibling's bindings.c.
 * Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing
 * two things is worse than losing the old binding: the second effect is
 * invisible until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'w' still fires while Shift is held for something else. The numeric
 * keypad's digits are NOT folded onto the top row's: keypad B lives there.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[O2_TARGET_COUNT];
static slot s_defaults[O2_TARGET_COUNT];
static int s_seeded;
static struct o2session *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

static void seed_button(int target, int button)
{
    s_defaults[target].button = button;
}

/* MAME's o2 keys: keypad A on Q W E / A S D / Z X C (keys 1-9, laid
 * out as the keypad is) with 0 on Left Alt; keypad B on the numeric keypad
 * the same way up (7 8 9 = 1 2 3) with 0 on its 0. F3 is CLEAR (MAME's)
 * and Escape is Reset to CONFIG. F5/F7/F8/F9/F11/F12 are the family's
 * window and debugger keys and stay free of the machine. */
static void compute_defaults_locked(void)
{
    static const uint32_t keypad_a[O2_ACT_PER_PORT] = {
        O2_KEYSYM_LALT, 'q', 'w', 'e', 'a', 's', 'd', 'z', 'x', 'c',
    };
    static const uint32_t keypad_b[O2_ACT_PER_PORT] = {
        O2_KEYSYM_KP_0, O2_KEYSYM_KP_7, O2_KEYSYM_KP_8, O2_KEYSYM_KP_9,
        O2_KEYSYM_KP_4, O2_KEYSYM_KP_5, O2_KEYSYM_KP_6,
        O2_KEYSYM_KP_1, O2_KEYSYM_KP_2, O2_KEYSYM_KP_3,
    };
    int t;
    for (t = 0; t < O2_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = O2_PAD_BTN_NONE;
    }

    for (t = 0; t < O2_ACT_PER_PORT; t++) {
        seed_key(O2_TARGET_PORT(0, t), keypad_a[t]);
        seed_key(O2_TARGET_PORT(1, t), keypad_b[t]);
    }
    seed_key(O2_TARGET_SWITCH(O2_SW_CLEAR), O2_KEYSYM_F3);
    seed_key(O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG), O2_KEYSYM_ESCAPE);

    /* gamepad defaults, uniform on both keypads: the D-pad (and, with
     * "analog sticks" on, the left stick) is the games' 2 4 6 8 with the
     * diagonals on 1 3 7 9, in gamepad_sdl.c rather than here; South is 0
     * (fire in most games, and the BIOS's "start") and East 5; Start is
     * the console's CLEAR. */
    {
        int port;
        for (port = 0; port < 2; port++) {
            seed_button(O2_TARGET_PORT(port, O2_ACT_0), O2_PAD_BTN_SOUTH);
            seed_button(O2_TARGET_PORT(port, O2_ACT_5), O2_PAD_BTN_EAST);
        }
        seed_button(O2_TARGET_SWITCH(O2_SW_CLEAR), O2_PAD_BTN_START);
    }
}

static uint32_t fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    if (keysym == O2_KEYSYM_KP_ENTER) return O2_KEYSYM_RETURN;
    if (keysym == O2_KEYSYM_KP_MULTIPLY) return '*';
    if (keysym == O2_KEYSYM_KP_DIVIDE) return '/';
    if (keysym == O2_KEYSYM_KP_PERIOD) return '.';
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < O2_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < O2_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    o2session_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(struct o2session *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(o2session_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

static int target_for_key_locked(uint32_t keysym)
{
    int t;
    if (!keysym) return -1;
    for (t = 0; t < O2_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) return t;
    return -1;
}

int o2session_target_for_key(o2session *s, uint32_t keysym)
{
    int t;
    (void)s;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    t = target_for_key_locked(fold(keysym));
    pthread_mutex_unlock(&s_lock);
    return t;
}

/* Gamepad buttons are scoped to the pad's port: the same button index is
 * free to mean different things on different ports, so only that port's
 * targets and the machine-wide switches/actions are searched. */
int o2session_target_for_button(o2session *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == O2_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < O2_ACT_PER_PORT; t++)
        if (s_table[O2_TARGET_PORT(port, t)].button == button) {
            found = O2_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = 2 * O2_ACT_PER_PORT; t < O2_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

o2_binding o2session_binding_get(o2session *s, int target)
{
    o2_binding b = { 0, O2_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= O2_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b.keysym = s_table[target].keysym;
    b.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

/* A key may legitimately drive several targets (two players' controls can
 * be put on one key by hand). An explicit rebinding is the user saying
 * "this key does exactly this", so it displaces every holder -- and names
 * them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", o2_target_name(victims[i]));
}

void o2session_binding_set_key(o2session *s, int target, uint32_t keysym,
                                  char *stolen, int stolensz)
{
    int victims[O2_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= O2_TARGET_COUNT) return;
    keysym = fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < O2_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void o2session_binding_set_button(o2session *s, int target, int button,
                                     char *stolen, int stolensz)
{
    int victims[O2_TARGET_COUNT], nvictims = 0, t;
    int port = target / O2_ACT_PER_PORT;   /* 2 = switches/sysactions */
    (void)s;
    if (target < 0 || target >= O2_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != O2_PAD_BTN_NONE) {
        for (t = 0; t < O2_TARGET_COUNT; t++) {
            int tport = t / O2_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= 2 || port >= 2)) {
                s_table[t].button = O2_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void o2session_bindings_reset(o2session *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) o2session_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const act_names[O2_ACT_PER_PORT] = {
    "Key 0", "Key 1", "Key 2", "Key 3", "Key 4",
    "Key 5", "Key 6", "Key 7", "Key 8", "Key 9",
};
static const char *const act_short[O2_ACT_PER_PORT] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
};
static const char *const switch_names[O2_SW_COUNT] = {
    "CLEAR",
};
static const char *const sysact_names[O2_SYSACT_COUNT] = {
    "Reset to CONFIG", "Debugger Stop",
};

const char *o2_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= O2_TARGET_COUNT) return "";
    if (target < 2 * O2_ACT_PER_PORT) {
        snprintf(buf, sizeof buf, "Keypad %c: %s", target < O2_ACT_PER_PORT ? 'A' : 'B',
                 act_names[target % O2_ACT_PER_PORT]);
        return buf;
    }
    target -= 2 * O2_ACT_PER_PORT;
    if (target < O2_SW_COUNT) return switch_names[target];
    return sysact_names[target - O2_SW_COUNT];
}

const char *o2_target_short_name(int target)
{
    if (target < 0 || target >= O2_TARGET_COUNT) return "";
    if (target < 2 * O2_ACT_PER_PORT) return act_short[target % O2_ACT_PER_PORT];
    target -= 2 * O2_ACT_PER_PORT;
    if (target < O2_SW_COUNT) return switch_names[target];
    return sysact_names[target - O2_SW_COUNT];
}

static const char *const pad_button_names[O2_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *o2_pad_button_name(int button)
{
    static char buf[32];
    if (O2_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (O2_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - O2_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (O2_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[O2_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int h = (button - O2_PAD_HAT_BASE) / O2_PAD_HAT_DIRS;
        int d = (button - O2_PAD_HAT_BASE) % O2_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", h, dirs[d]);
        return buf;
    }
    return "";
}

int o2session_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case O2_KEYSYM_UP: name = "Up"; break;
    case O2_KEYSYM_DOWN: name = "Down"; break;
    case O2_KEYSYM_LEFT: name = "Left"; break;
    case O2_KEYSYM_RIGHT: name = "Right"; break;
    case O2_KEYSYM_ESCAPE: name = "Escape"; break;
    case O2_KEYSYM_RETURN: name = "Return"; break;
    case O2_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case O2_KEYSYM_TAB: name = "Tab"; break;
    case O2_KEYSYM_SPACE: name = "Space"; break;
    case O2_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case O2_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case O2_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case O2_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case O2_KEYSYM_LALT: name = "Left Alt"; break;
    case O2_KEYSYM_RALT: name = "Right Alt"; break;
    case O2_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case O2_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case O2_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case O2_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= O2_KEYSYM_F1 && keysym <= O2_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - O2_KEYSYM_F1 + 1);
    if (keysym >= O2_KEYSYM_KP_0 && keysym <= O2_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - O2_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= O2SESSION_KEYSYM_HID_BASE
        && keysym <= O2SESSION_KEYSYM_HID_BASE + O2SESSION_HID_USAGE_MAX) {
        const char *hid = o2_hid_usage_name(keysym - O2SESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - O2SESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int o2session_key_sysaction(o2session *s, uint32_t keysym)
{
    int t;
    t = o2session_target_for_key(s, keysym);
    if (t < O2_TARGET_SYSACT(0)) return -1;
    return t - O2_TARGET_SYSACT(0);
}

int o2session_key(o2session *s, uint32_t keysym, int down)
{
    uint32_t folded = fold(keysym);
    int t;

    if (!s) return 0;
    if (down) {
        int hit = 0;
        /* every target the key drives; a system action is the frontend's
         * to fire and is not a machine input */
        pthread_mutex_lock(&s_lock);
        ensure_seeded_locked();
        for (t = 0; t < O2_TARGET_SYSACT(0); t++) {
            if (s_table[t].keysym != folded) continue;
            hit = 1;
            if (s->held_keysym[t] == folded) continue;  /* auto-repeat */
            s->held_keysym[t] = folded;
            pthread_mutex_unlock(&s_lock);
            o2session_press(s, t, 1);
            pthread_mutex_lock(&s_lock);
        }
        pthread_mutex_unlock(&s_lock);
        return hit;
    }
    {
        int hit = 0;
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        for (t = 0; t < O2_TARGET_COUNT; t++) {
            if (s->held_keysym[t] == folded) {
                s->held_keysym[t] = 0;
                o2session_press(s, t, 0);
                hit = 1;
            }
        }
        return hit;
    }
}

void o2session_release_all(o2session *s)
{
    int t;
    if (!s) return;
    for (t = 0; t < O2_TARGET_COUNT; t++) {
        if (s->held_keysym[t]) {
            s->held_keysym[t] = 0;
            o2session_press(s, t, 0);
        }
    }
}

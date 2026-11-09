/*
 * gamepad_map_test -- the gamepad layer end to end with an SDL3 virtual
 * gamepad: hot-plug pickup, the default mapping onto the joystick (the D-pad
 * is the stick's four directions, South is its button, the left analog stick
 * steers too), a button rebound to a matrix key and to the console's RESET,
 * the sources not stepping on each other, and hot-unplug releasing everything
 * the pad held -- with no hardware and no human. Then the pad's port
 * assignment: written to the settings ("pad_assign") when made, restored when
 * a new session (a restart) finds the pad again, found through a stale GUID
 * by the name, kept through garbage, and removed by Automatic.
 *
 * What the pad holds is read back through o2session_buttons_held,
 * o2session_key_held and o2session_switch_held, which report the same
 * per-source bits the machine is driven from. SKIPs (77) where SDL has no
 * virtual joysticks or a machine's real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "o2session.h"
#include "test_tmpdir.h"

#define PAD_NAME "o2test virtual pad"

static int failures;
static int vport;   /* the stick the virtual pad was given */

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The pad thread syncs devices a few times a second and polls state
 * faster; give it two seconds. */
static void expect(o2session *s, unsigned want, int key, int reset, const char *what)
{
    unsigned held = 0;
    int k = 0, r = 0;
    for (int i = 0; i < 100; i++) {
        held = o2session_buttons_held(s, vport);
        k = o2session_key_held(s, 24);
        r = o2session_switch_held(s, O2_SW_RESET);
        if (held == want && k == key && r == reset) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %02X want %02X, key Q %d/%d, RESET %d/%d)\n", what, held, want, k, key, r, reset);
    failures++;
}

#define B(a) (1u << (a))

/* A session on p's directories with only the gamepad thread. */
static o2session *session_up(o2session_paths *p)
{
    o2session *s = o2session_new(p);
    o2session_start_opts o;
    if (!s)
        return NULL;
    o2session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    if (o2session_start(s, &o) != 0) {
        printf("FAIL: start: %s\n", o2session_last_error(s));
        o2session_free(s);
        return NULL;
    }
    return s;
}

/* The virtual pad's slot now, or -1. */
static int pad_slot_now(o2session *s)
{
    for (int idx = 0; idx < o2session_gamepad_count(s); idx++) {
        char name[64];
        o2session_gamepad_name(s, idx, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            return idx;
    }
    return -1;
}

/* The virtual pad's slot once the pad thread has found it (polled, up to
 * five seconds: a slow runner is slow to enumerate), or -1. */
static int wait_for_pad(o2session *s)
{
    int idx = pad_slot_now(s);
    for (int tries = 0; tries < 250 && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(s);
    }
    return idx;
}

/* The pad_assign setting with the pad's entry in it? */
static int has_entry(o2session *s, const char *guid, int port)
{
    char want[160];
    snprintf(want, sizeof want, "%s|%s=%d", guid, PAD_NAME, port);
    return strstr(o2session_get_str(s, "pad_assign", ""), want) != NULL;
}

/* A stale entry (GUID guid, the pad's name) goes to a different pad of the
 * same name that connects first, by the name fallback. The pad with exactly
 * that GUID then connects and takes it over: the decoy goes automatic. No pad
 * is attached on entry; both are detached on return. */
static void takeover(o2session_paths *p, SDL_VirtualJoystickDesc *desc,
                     const char *guid, int want)
{
    SDL_VirtualJoystickDesc other = *desc;
    SDL_JoystickID decoy, owner = 0;
    char text[300], gid[64];
    o2session *s = session_up(p);
    int idx;

    if (!s) { failures++; return; }
    snprintf(text, sizeof text, "%s|%s=%d", guid, PAD_NAME, want);
    o2session_set_str(s, "pad_assign", text);
    o2session_free(s);

    other.vendor_id = (Uint16)(desc->vendor_id ^ 0x5a5a);
    other.product_id = (Uint16)(desc->product_id ^ 0xa5a5);
    decoy = SDL_AttachVirtualJoystick(&other);
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(decoy), gid, sizeof gid);
    if (!decoy || !strcmp(gid, guid)) {
        printf("skip: the takeover test needs a second virtual pad with another GUID\n");
        if (decoy)
            SDL_DetachVirtualJoystick(decoy);
        return;
    }
    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(decoy); return; }
    idx = wait_for_pad(s);
    check(idx >= 0 && o2session_gamepad_assignment(s, idx) == want,
          "takeover: a same-name pad with another GUID gets the entry by name");
    owner = SDL_AttachVirtualJoystick(desc);
    for (int tries = 0; tries < 250 && o2session_gamepad_count(s) < 2; tries++)
        sleep_ms(20);
    check(owner && o2session_gamepad_count(s) == 2, "takeover: the second pad is picked up");
    if (o2session_gamepad_count(s) == 2) {
        check(o2session_gamepad_assignment(s, 1) == want,
              "the pad with the entry's GUID takes the assignment over");
        check(o2session_gamepad_assignment(s, 0) == -1, "and the name-only pad is automatic");
    }
    o2session_free(s);
    if (owner)
        SDL_DetachVirtualJoystick(owner);
    SDL_DetachVirtualJoystick(decoy);
}

/* A pad's port assignment survives a restart. The virtual pad stays attached
 * across the sessions (SDL is held open by the caller). */
static void persistence(o2session_paths *p)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    o2session *s;
    char guid[64];
    int idx, want, auto_port;

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("FAIL: the virtual pad would not attach again: %s\n", SDL_GetError());
        failures++;
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(s);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { o2session_free(s); SDL_DetachVirtualJoystick(vid); return; }
    check(o2session_gamepad_assignment(s, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(o2session_get_str(s, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_port = o2session_gamepad_effective_port(s, idx);
    want = auto_port == 0 ? 1 : 0;

    o2session_gamepad_assign(s, idx, want);
    check(o2session_gamepad_assignment(s, idx) == want
          && o2session_gamepad_effective_port(s, idx) == want, "assigning drives the other port");
    check(has_entry(s, guid, want), "and writes <guid>|<name>=<port> to pad_assign");

    /* the file itself, not just the in-memory store */
    {
        char path[600], line[2048];
        int found = 0;
        FILE *fp;
        snprintf(path, sizeof path, "%s/settings.ini", p->config_dir);
        fp = fopen(path, "r");
        if (fp) {
            while (fgets(line, sizeof line, fp))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(fp);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings */
    o2session_free(s);
    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(s);
    check(idx >= 0, "restart: the pad is picked up again");
    if (idx >= 0) {
        check(o2session_gamepad_assignment(s, idx) == want, "the assignment is restored on device add");
        check(o2session_gamepad_effective_port(s, idx) == want, "and the pad drives that port");
    }

    /* a stale GUID (another USB port): the name finds the entry. Rewrite the
     * stored GUID, restart. */
    {
        char text[300];
        snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", PAD_NAME, want);
        o2session_set_str(s, "pad_assign", text);
    }
    o2session_free(s);
    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(s);
    check(idx >= 0 && o2session_gamepad_assignment(s, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry, and the next run starts automatic */
    o2session_gamepad_assign(s, idx, want);   /* back under its own GUID */
    check(has_entry(s, guid, want), "assigned under the pad's own GUID again");
    o2session_gamepad_assign(s, idx, -1);
    check(o2session_gamepad_assignment(s, idx) == -1, "Automatic: no explicit port");
    check(strstr(o2session_get_str(s, "pad_assign", ""), PAD_NAME) == NULL,
          "and the entry is gone from pad_assign");
    o2session_free(s);
    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(s);
    check(idx >= 0 && o2session_gamepad_assignment(s, idx) == -1
          && o2session_gamepad_effective_port(s, idx) == auto_port, "after a restart it is automatic again");

    /* garbage in the setting is ignored, silently */
    o2session_set_str(s, "pad_assign", ";;|=;zz|x=9;%%%;" "00000000000000000000000000000000|" PAD_NAME "=7");
    o2session_free(s);
    s = session_up(p);
    if (!s) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(s);
    check(idx >= 0 && o2session_gamepad_assignment(s, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");
    o2session_gamepad_assign(s, idx, want);
    check(has_entry(s, guid, want), "and a new assignment still writes");
    o2session_gamepad_assign(s, idx, -1);

    /* a hot replug within a run restores it too */
    o2session_gamepad_assign(s, idx, want);
    SDL_DetachVirtualJoystick(vid);
    for (int tries = 0; tries < 250 && pad_slot_now(s) >= 0; tries++)
        sleep_ms(20);
    check(pad_slot_now(s) < 0, "unplugged: the pad is gone");
    vid = SDL_AttachVirtualJoystick(&desc);
    idx = vid ? wait_for_pad(s) : -1;
    check(idx >= 0 && o2session_gamepad_assignment(s, idx) == want, "unplugged and plugged back in: the port comes back");
    o2session_gamepad_assign(s, idx, -1);

    o2session_free(s);
    if (vid)
        SDL_DetachVirtualJoystick(vid);
    takeover(p, &desc, guid, want);
}

int main(void)
{
    char cfg[512], data[512];
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline;

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        return 1;
    test_tmpdir(cfg, sizeof cfg, "gmcfg");
    test_tmpdir(data, sizeof data, "gmdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg;
    p.data_dir = data;
    p.fujinet_lib = "";      /* the pad only */
    s = o2session_new(&p);
    if (!s)
        return 1;
    o2session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 1;
    if (o2session_start(s, &o) != 0) {
        printf("FAIL: start: %s\n", o2session_last_error(s));
        return 1;
    }

    /* Real pads may already be attached: let the first device scan find
     * them, so the virtual one is known by the slot it adds. */
    sleep_ms(700);
    baseline = o2session_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = PAD_NAME;
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        o2session_free(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj)
        return 1;

    for (i = 0; i < 100 && o2session_gamepad_count(s) <= baseline; i++)
        sleep_ms(20);
    vport = -1;
    for (i = 0; i < o2session_gamepad_count(s); i++) {
        char name[64];
        o2session_gamepad_name(s, i, name, sizeof name);
        if (strcmp(name, PAD_NAME) == 0)
            vport = o2session_gamepad_effective_port(s, i);
    }
    if (vport < 0 || vport > 1) {
        printf("%s: the virtual pad got no keypad (%d; real pads attached?)\n",
               vport < 0 && o2session_gamepad_count(s) <= baseline ? "FAIL" : "SKIP", vport);
        SDL_CloseJoystick(vj);
        SDL_DetachVirtualJoystick(vid);
        o2session_free(s);
        return o2session_gamepad_count(s) <= baseline ? 1 : 77;
    }
    printf("ok: the virtual pad was picked up as stick %d\n", vport + 1);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    expect(s, B(O2_ACT_FIRE), 0, 0, "South is the button");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    expect(s, 0, 0, 0, "released");

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, 0, 0, 0, "East is nothing by default");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);

    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, B(O2_ACT_UP), 0, 0, "D-pad up is up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    expect(s, B(O2_ACT_UP) | B(O2_ACT_LEFT), 0, 0, "up and left together are a diagonal");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    expect(s, B(O2_ACT_LEFT), 0, 0, "up and down together cancel");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(s, B(O2_ACT_LEFT) | B(O2_ACT_DOWN), 0, 0, "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    expect(s, 0, 0, 0, "released");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    expect(s, B(O2_ACT_RIGHT), 0, 0, "D-pad right is right");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    expect(s, 0, 0, 0, "released");

    /* the stick past its threshold steers too (a pad that reports its D-pad
     * as axes would otherwise drive nothing) */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    expect(s, B(O2_ACT_RIGHT) | B(O2_ACT_DOWN), 0, 0, "the stick right and down is right and down");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    expect(s, B(O2_ACT_RIGHT) | B(O2_ACT_DOWN), 0, 0,
           "the D-pad holds right when the analog stick lets go of it: the sources are separate");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(s, 0, 0, 0, "centred");

    /* a button bound to a matrix key, and to the console's RESET */
    o2session_binding_set_button(s, O2_TARGET_KEY(24), O2_PAD_BTN_EAST, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    expect(s, 0, 1, 0, "East, bound to Q, holds the key");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    expect(s, 0, 0, 0, "released");
    o2session_binding_set_button(s, O2_TARGET_SWITCH(O2_SW_RESET), O2_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    expect(s, 0, 0, 1, "Start, bound to RESET, holds it");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, false);
    expect(s, 0, 0, 0, "released");

    /* A trigger past half travel used to deadlock the gamepad thread on its
     * own lock, and the Controllers panel's timers (same lock) froze the UI
     * with it. A hang here is a ctest timeout. */
    o2session_binding_set_button(s, O2_TARGET_KEY(24), O2_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(s, 0, 1, 0, "left trigger, bound to Q, holds the key");
    (void)o2session_gamepad_generation(s);
    for (i = 0; i < o2session_gamepad_count(s); i++)
        (void)o2session_gamepad_effective_port(s, i);
    printf("ok: session lock still free with a trigger held\n");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(s, 0, 0, 0, "left trigger released");
    o2session_gamepad_capture_begin(s);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = -1, got = 0;
        for (i = 0; i < 100 && !got; i++) {
            got = o2session_gamepad_capture_poll(s, &btn);
            if (!got) sleep_ms(20);
        }
        if (got && btn == O2_PAD_BTN_RIGHT_TRIGGER)
            printf("ok: map mode captures a trigger\n");
        else {
            printf("FAIL: map mode captures a trigger (%d)\n", btn);
            failures++;
        }
    }
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    o2session_bindings_reset(s);

    /* a D-pad direction rebound elsewhere stops steering */
    o2session_binding_set_button(s, O2_TARGET_KEY(24), O2_PAD_BTN_DPAD_UP, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    expect(s, 0, 1, 0, "D-pad up, bound to Q, is Q and not up any more");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    expect(s, 0, 0, 0, "released");
    o2session_bindings_reset(s);

    /* hot-unplug while held: the stick, a key and RESET are released, not
     * left holding forever */
    o2session_binding_set_button(s, O2_TARGET_KEY(24), O2_PAD_BTN_EAST, NULL, 0);
    o2session_binding_set_button(s, O2_TARGET_SWITCH(O2_SW_RESET), O2_PAD_BTN_START, NULL, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, -20000);
    expect(s, B(O2_ACT_RIGHT) | B(O2_ACT_FIRE) | B(O2_ACT_UP), 1, 1, "held before the unplug");
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 100 && o2session_gamepad_count(s) != baseline; i++)
        sleep_ms(20);
    if (o2session_gamepad_count(s) == baseline)
        printf("ok: the unplugged pad is gone\n");
    else {
        printf("FAIL: the backend kept the detached pad (%d, baseline %d)\n",
               o2session_gamepad_count(s), baseline);
        failures++;
    }
    expect(s, 0, 0, 0, "and its stick, key and RESET are released");
    o2session_bindings_reset(s);

    o2session_stop(s);
    o2session_free(s);

    persistence(&p);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

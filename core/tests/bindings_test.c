/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * defaults (MAME's keypad layout), steal-and-describe, persistence through
 * the settings store, and names. No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "o2session.h"
#include "test_tmpdir.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    char cfg[512], data[512], stolen[64], name[64];
    o2session_paths p;
    o2session *s;
    o2_binding b;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = o2session_new(&p);
    if (!s) return 1;

    /* defaults: MAME's o2 keys */
    b = o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_1));
    check(b.keysym == 'q' && b.button == O2_PAD_BTN_NONE, "keypad A 1 defaults to Q");
    b = o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_9));
    check(b.keysym == 'c', "keypad A 9 defaults to C");
    b = o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_0));
    check(b.keysym == O2_KEYSYM_LALT && b.button == O2_PAD_BTN_SOUTH,
          "keypad A 0 defaults to Left Alt / South");
    b = o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_5));
    check(b.keysym == 's' && b.button == O2_PAD_BTN_EAST, "keypad A 5 defaults to S / East");
    b = o2session_binding_get(s, O2_TARGET_PORT(1, O2_ACT_1));
    check(b.keysym == O2_KEYSYM_KP_7, "keypad B 1 is the numeric keypad's 7 (laid out alike)");
    b = o2session_binding_get(s, O2_TARGET_PORT(1, O2_ACT_0));
    check(b.keysym == O2_KEYSYM_KP_0, "keypad B 0 is the numeric keypad's 0");
    b = o2session_binding_get(s, O2_TARGET_SWITCH(O2_SW_CLEAR));
    check(b.keysym == O2_KEYSYM_F3 && b.button == O2_PAD_BTN_START, "CLEAR defaults to F3 / Start");
    b = o2session_binding_get(s, O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG));
    check(b.keysym == O2_KEYSYM_ESCAPE, "Reset to CONFIG defaults to Escape");
    check(o2session_target_for_key(s, 'Q') == O2_TARGET_PORT(0, O2_ACT_1), "lookup folds case");
    check(o2session_target_for_key(s, '7') == -1, "the top row's 7 is not the numeric keypad's");
    check(o2session_target_for_button(s, 1, O2_PAD_BTN_SOUTH) == O2_TARGET_PORT(1, O2_ACT_0),
          "button lookup is scoped to the keypad");
    check(o2session_target_for_button(s, 0, O2_PAD_BTN_START) == O2_TARGET_SWITCH(O2_SW_CLEAR),
          "Start is CLEAR");

    /* steal */
    o2session_binding_set_key(s, O2_TARGET_PORT(1, O2_ACT_2), 'w', stolen, sizeof stolen);
    check(strstr(stolen, "Keypad A: Key 2") != NULL, "rebinding W reports the holder it displaced");
    b = o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_2));
    check(b.keysym == 0, "the old holder lost the key");
    check(o2session_target_for_key(s, 'w') == O2_TARGET_PORT(1, O2_ACT_2), "the new holder has it");

    /* names */
    o2session_keysym_name(O2_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    o2session_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    o2session_keysym_name(O2_KEYSYM_KP_7, name, sizeof name);
    check(strcmp(name, "Keypad 7") == 0, "numeric keypad keys say so");
    check(strcmp(o2_pad_button_name(O2_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(o2_target_name(O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0,
          "system action names");
    check(strcmp(o2_target_name(O2_TARGET_PORT(1, O2_ACT_6)), "Keypad B: Key 6") == 0, "target names");
    check(strcmp(o2_target_short_name(O2_TARGET_PORT(0, O2_ACT_3)), "3") == 0, "short names");
    check(strcmp(o2_target_short_name(O2_TARGET_SWITCH(O2_SW_CLEAR)), "CLEAR") == 0,
          "the console's button");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(o2session_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(o2session_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(o2session_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(o2session_keysym_from_evdev(103) == O2_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(o2session_keysym_from_win_scancode(0x48, 1) == O2_KEYSYM_UP, "Windows E0 48 -> Up");
    check(o2session_keysym_from_macos_keycode(0x7E) == O2_KEYSYM_UP, "macOS 0x7E -> Up");

    /* persistence */
    o2session_free(s);
    s = o2session_new(&p);
    check(s && o2session_target_for_key(s, 'w') == O2_TARGET_PORT(1, O2_ACT_2), "the rebinding persisted");
    o2session_bindings_reset(s);
    check(o2session_target_for_key(s, 'w') == O2_TARGET_PORT(0, O2_ACT_2), "reset restores the defaults");
    o2session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

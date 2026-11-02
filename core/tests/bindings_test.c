/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * the defaults (MAME's key map, two sticks, F1 for RESET), that no two
 * targets share a default, steal-and-describe, persistence through the
 * settings store, and names. No machine is started.
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

static int key_target(o2session *s, uint32_t keysym)
{
    return o2session_target_for_key(s, keysym);
}

int main(void)
{
    char cfg[512], data[512], stolen[96], name[64];
    o2session_paths p;
    o2session *s;
    o2_binding b;
    int t, u, clash = 0, tkey;

    test_tmpdir(cfg, sizeof cfg, "bcfg");
    test_tmpdir(data, sizeof data, "bdata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    s = o2session_new(&p);
    if (!s) return 1;

    /* the table's shape */
    check(O2_TARGET_COUNT == 10 + 64 + 1 + 2, "ten stick targets, 64 matrix keys, RESET, two actions");
    check(O2_TARGET_KEY(0) == 10 && O2_TARGET_SWITCH(O2_SW_RESET) == 74
          && O2_TARGET_SYSACT(0) == 75, "the flat target indices");

    /* defaults: MAME's keys, by legend */
    check(key_target(s, '0') == O2_TARGET_KEY(0) && key_target(s, '7') == O2_TARGET_KEY(7)
          && key_target(s, '9') == O2_TARGET_KEY(9), "the digits are the digit keys");
    check(key_target(s, 'q') == O2_TARGET_KEY(24) && key_target(s, 'w') == O2_TARGET_KEY(17)
          && key_target(s, 'p') == O2_TARGET_KEY(15) && key_target(s, 'k') == O2_TARGET_KEY(31)
          && key_target(s, 'a') == O2_TARGET_KEY(32) && key_target(s, 'm') == O2_TARGET_KEY(38)
          && key_target(s, 'l') == O2_TARGET_KEY(14),
          "letters press the key of that legend (MAME's matrix)");
    check(key_target(s, 'Q') == O2_TARGET_KEY(24), "lookup folds case");
    check(key_target(s, 'y') == O2_TARGET_KEY(44) && key_target(s, 'n') == O2_TARGET_KEY(45),
          "Y and N are the Y/YES and N/NO switches");
    check(key_target(s, O2_KEYSYM_SPACE) == O2_TARGET_KEY(12), "Space is SPACE");
    check(key_target(s, '.') == O2_TARGET_KEY(39), ". is .");
    check(key_target(s, '/') == O2_TARGET_KEY(13), "/ is ? (KEYCODE_SLASH)");
    check(key_target(s, O2_KEYSYM_KP_DIVIDE) == O2_TARGET_KEY(42), "the keypad's / is the division sign");
    check(key_target(s, O2_KEYSYM_KP_MULTIPLY) == O2_TARGET_KEY(41), "the keypad's * is the multiplication sign");
    check(key_target(s, 0xffab) == O2_TARGET_KEY(16), "the keypad's + is +");
    check(key_target(s, '-') == O2_TARGET_KEY(40) && key_target(s, '=') == O2_TARGET_KEY(43),
          "- and = are - and =");
    check(key_target(s, O2_KEYSYM_RETURN) == O2_TARGET_KEY(47), "Enter is ENTER");
    check(key_target(s, O2_KEYSYM_KP_ENTER) == O2_TARGET_KEY(47), "so is the keypad's Enter");
    check(key_target(s, O2_KEYSYM_BACKSPACE) == O2_TARGET_KEY(46), "Backspace is CLEAR");
    check(key_target(s, ';') == O2_TARGET_KEY(50) && key_target(s, ',') == O2_TARGET_KEY(54)
          && key_target(s, O2_KEYSYM_LSHIFT) == O2_TARGET_KEY(56) && key_target(s, 0xff57) == O2_TARGET_KEY(57),
          "the Videopac+'s extra rows take the punctuation and the modifiers");

    /* the sticks */
    check(key_target(s, O2_KEYSYM_UP) == O2_TARGET_PORT(0, O2_ACT_UP)
          && key_target(s, O2_KEYSYM_RIGHT) == O2_TARGET_PORT(0, O2_ACT_RIGHT)
          && key_target(s, O2_KEYSYM_DOWN) == O2_TARGET_PORT(0, O2_ACT_DOWN)
          && key_target(s, O2_KEYSYM_LEFT) == O2_TARGET_PORT(0, O2_ACT_LEFT)
          && key_target(s, O2_KEYSYM_RCTRL) == O2_TARGET_PORT(0, O2_ACT_FIRE),
          "stick 1 is the arrow keys and Right Ctrl");
    check(key_target(s, O2_KEYSYM_KP_8) == O2_TARGET_PORT(1, O2_ACT_UP)
          && key_target(s, O2_KEYSYM_KP_6) == O2_TARGET_PORT(1, O2_ACT_RIGHT)
          && key_target(s, O2_KEYSYM_KP_2) == O2_TARGET_PORT(1, O2_ACT_DOWN)
          && key_target(s, O2_KEYSYM_KP_4) == O2_TARGET_PORT(1, O2_ACT_LEFT)
          && key_target(s, O2_KEYSYM_KP_0) == O2_TARGET_PORT(1, O2_ACT_FIRE),
          "stick 2 is the keypad's 8 6 2 4 and 0");
    check(key_target(s, O2_KEYSYM_KP_5) == -1 && key_target(s, O2_KEYSYM_KP_7) == -1,
          "the keypad's other digits are left alone");
    b = o2session_binding_get(s, O2_TARGET_SWITCH(O2_SW_RESET));
    check(b.keysym == O2_KEYSYM_F1 && b.button == O2_PAD_BTN_NONE, "RESET defaults to F1");
    b = o2session_binding_get(s, O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG));
    check(b.keysym == O2_KEYSYM_ESCAPE, "Reset to CONFIG defaults to Escape");
    check(key_target(s, O2_KEYSYM_F9) == -1 && key_target(s, O2_KEYSYM_F11) == -1
          && key_target(s, O2_KEYSYM_F12) == -1, "F9, F11 and F12 are the frontends'");

    /* every default is its own: no key drives two targets */
    for (t = 0; t < O2_TARGET_COUNT; t++) {
        o2_binding bt = o2session_binding_get(s, t);
        if (!bt.keysym) continue;
        for (u = t + 1; u < O2_TARGET_COUNT; u++)
            if (o2session_binding_get(s, u).keysym == bt.keysym) {
                printf("  (%s and %s share a key)\n", o2_target_name(t), o2_target_name(u));
                clash++;
            }
    }
    check(clash == 0, "no two default keys clash");

    /* gamepad defaults, the same on both sticks */
    for (t = 0; t < 2; t++) {
        check(o2session_binding_get(s, O2_TARGET_PORT(t, O2_ACT_UP)).button == O2_PAD_BTN_DPAD_UP
              && o2session_binding_get(s, O2_TARGET_PORT(t, O2_ACT_RIGHT)).button == O2_PAD_BTN_DPAD_RIGHT
              && o2session_binding_get(s, O2_TARGET_PORT(t, O2_ACT_DOWN)).button == O2_PAD_BTN_DPAD_DOWN
              && o2session_binding_get(s, O2_TARGET_PORT(t, O2_ACT_LEFT)).button == O2_PAD_BTN_DPAD_LEFT
              && o2session_binding_get(s, O2_TARGET_PORT(t, O2_ACT_FIRE)).button == O2_PAD_BTN_SOUTH,
              t ? "the second pad: D-pad and South" : "the first pad: D-pad and South");
        check(o2session_target_for_button(s, t, O2_PAD_BTN_SOUTH) == O2_TARGET_PORT(t, O2_ACT_FIRE),
              "button lookup is scoped to the stick");
    }
    check(o2session_target_for_button(s, 0, O2_PAD_BTN_START) == -1, "Start is nothing");

    /* steal: one key, one target */
    o2session_binding_set_key(s, O2_TARGET_KEY(17), O2_KEYSYM_LEFT, stolen, sizeof stolen);
    check(strstr(stolen, "Stick 1: Left") != NULL, "rebinding Left reports the holder it displaced");
    check(o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_LEFT)).keysym == 0, "the old holder lost the key");
    check(key_target(s, O2_KEYSYM_LEFT) == O2_TARGET_KEY(17), "the new holder has it");
    o2session_binding_set_key(s, O2_TARGET_KEY(17), 'w', stolen, sizeof stolen);
    check(stolen[0] == '\0' && key_target(s, 'w') == O2_TARGET_KEY(17),
          "putting W back takes nothing from anyone");
    o2session_binding_set_key(s, O2_TARGET_PORT(0, O2_ACT_LEFT), O2_KEYSYM_LEFT, NULL, 0);

    o2session_binding_set_button(s, O2_TARGET_KEY(24), O2_PAD_BTN_WEST, NULL, 0);
    check(o2session_target_for_button(s, 0, O2_PAD_BTN_WEST) == O2_TARGET_KEY(24)
          && o2session_target_for_button(s, 1, O2_PAD_BTN_WEST) == O2_TARGET_KEY(24),
          "a pad button bound to a matrix key works on either pad");
    o2session_binding_set_button(s, O2_TARGET_PORT(1, O2_ACT_FIRE), O2_PAD_BTN_WEST, stolen, sizeof stolen);
    check(strstr(stolen, "Key: Q") != NULL, "stealing a pad button names the key that had it");
    check(o2session_binding_get(s, O2_TARGET_KEY(24)).button == O2_PAD_BTN_NONE, "which lost it");
    o2session_binding_set_key(s, O2_TARGET_KEY(24), 0, NULL, 0);
    check(o2session_binding_get(s, O2_TARGET_KEY(24)).keysym == 0, "a key can be unbound");

    /* names */
    o2session_keysym_name(O2_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    o2session_keysym_name('a', name, sizeof name);
    check(strcmp(name, "A") == 0, "letters name upper-case");
    o2session_keysym_name(O2_KEYSYM_KP_7, name, sizeof name);
    check(strcmp(name, "Keypad 7") == 0, "numeric keypad keys say so");
    o2session_keysym_name(0xffab, name, sizeof name);
    check(strcmp(name, "Keypad +") == 0, "the keypad's + too");
    check(strcmp(o2_pad_button_name(O2_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0, "pad button names");
    check(strcmp(o2_target_name(O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0,
          "system action names");
    check(strcmp(o2_target_name(O2_TARGET_PORT(1, O2_ACT_FIRE)), "Stick 2: Fire") == 0, "stick target names");
    check(strcmp(o2_target_name(O2_TARGET_PORT(0, O2_ACT_UP)), "Stick 1: Up") == 0, "and the first stick's");
    check(strcmp(o2_target_name(O2_TARGET_KEY(24)), "Key: Q") == 0, "key names");
    check(strcmp(o2_target_name(O2_TARGET_KEY(44)), "Key: Y / YES") == 0, "a shared key says so");
    check(strcmp(o2_target_name(O2_TARGET_KEY(49)), "Key: Lock") == 0, "the G7400's own keys");
    check(strcmp(o2_target_short_name(O2_TARGET_PORT(0, O2_ACT_RIGHT)), "Right") == 0, "short names");
    check(strcmp(o2_target_short_name(O2_TARGET_KEY(47)), "ENTER") == 0, "short key names");
    check(strcmp(o2_target_short_name(O2_TARGET_SWITCH(O2_SW_RESET)), "RESET") == 0, "the console's button");
    check(strcmp(o2_target_name(O2_TARGET_KEY(10)), "Key: 10") == 0, "a position with no key has a number");
    tkey = 0;
    for (t = 0; t < O2_TARGET_COUNT; t++)
        if (o2_target_name(t)[0] == '\0') tkey++;
    check(tkey == 0, "every target has a name");

    /* native key codes: evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(o2session_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(o2session_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(o2session_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(o2session_keysym_from_evdev(103) == O2_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(o2session_keysym_from_win_scancode(0x48, 1) == O2_KEYSYM_UP, "Windows E0 48 -> Up");
    check(o2session_keysym_from_macos_keycode(0x7E) == O2_KEYSYM_UP, "macOS 0x7E -> Up");
    check(o2session_keysym_from_evdev(53) == '/', "evdev KEY_SLASH -> '/' (the ? key)");
    check(o2session_keysym_from_evdev(78) == 0xffab, "evdev KEY_KPPLUS -> the keypad's +");

    /* persistence */
    o2session_binding_set_key(s, O2_TARGET_PORT(1, O2_ACT_UP), 'w', stolen, sizeof stolen);
    check(strstr(stolen, "Key: W") != NULL, "rebinding W names the key it took it from");
    o2session_free(s);
    s = o2session_new(&p);
    check(s && key_target(s, 'w') == O2_TARGET_PORT(1, O2_ACT_UP), "the rebinding persisted");
    check(s && key_target(s, O2_KEYSYM_KP_8) == -1, "and what it displaced stayed displaced");
    o2session_bindings_reset(s);
    check(key_target(s, 'w') == O2_TARGET_KEY(17), "reset restores the defaults");
    check(key_target(s, O2_KEYSYM_KP_8) == O2_TARGET_PORT(1, O2_ACT_UP), "all of them");
    check(strcmp(o2session_get_str(s, "bindings", "x"), "") == 0, "and persists nothing");
    o2session_free(s);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

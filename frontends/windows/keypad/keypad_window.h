/*
 * The Win32 Keypad window: the Odyssey 2's two ten-key keypads on screen,
 * the console's CLEAR button, the Map row and the gamepads, in a fixed-size
 * tool window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "o2session.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void o2_keypad_window_toggle(HWND parent, o2session *session);

/* Called when the gamepad set changed, so the per-keypad pad lines and the
 * gamepad rows refresh. */
void o2_keypad_window_gamepads_changed(void);

/* Give the window first refusal on a message from the main loop. Returns 1
 * if consumed. */
int o2_keypad_pretranslate(MSG *msg);

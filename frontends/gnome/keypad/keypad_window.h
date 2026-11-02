/*
 * The Keypad window -- both Odyssey 2 keypads on screen, held keys lit, the
 * console's CLEAR button, Map mode for rebinding any control to a key or a
 * gamepad button, and the gamepads' keypad assignments.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "o2session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is only used the first
 * time, to set the transient-for relationship. */
void o2_keypad_window_toggle(GtkWindow *parent, o2session *session);
gboolean o2_keypad_window_is_visible(void);

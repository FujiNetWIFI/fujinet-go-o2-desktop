/*
 * The Controllers window -- the Odyssey 2's membrane keyboard and both
 * joysticks drawn like the console's, held keys lit, the gamepads' stick
 * assignments, and Map mode for rebinding any control to a key or a gamepad
 * button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "o2session.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void o2_controllers_toggle(GtkWindow *parent, o2session *session);
gboolean o2_controllers_visible(void);

/*
 * The Controllers window (F9): the Odyssey 2's membrane keyboard drawn as it
 * is on the console, its two joysticks, the gamepads with their assignments,
 * and Map mode for rebinding any key or stick control. Hidden at start; a
 * fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "o2session.h"

/* Shows the window, or hides it when it is showing. */
void o2ControllersToggle(QWidget *parent, o2session *session);
bool o2ControllersVisible();
/* The machine changed (Preferences): rebuild the keyboard for it. */
void o2ControllersRefresh();

/*
 * The emulator display: a GtkWidget that pulls frames from the session on
 * the compositor's own frame clock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "o2session.h"

#define O2_TYPE_DISPLAY (o2_display_get_type())
G_DECLARE_FINAL_TYPE(O2Display, o2_display, O2, DISPLAY, GtkWidget)

GtkWidget *o2_display_new(o2session *session);
/* The picture a television showed (the whole 360 x 243 area at 4:3), or
 * square pixels (360:243). */
void o2_display_set_tv_aspect(O2Display *self, gboolean tv);
void o2_display_set_smooth(O2Display *self, gboolean smooth);

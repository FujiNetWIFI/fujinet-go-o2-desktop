/*
 * The main application window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "o2session.h"

#define O2_TYPE_WINDOW (o2_window_get_type())
G_DECLARE_FINAL_TYPE(O2Window, o2_window, O2, WINDOW,
                     AdwApplicationWindow)

GtkWidget *o2_window_new(AdwApplication *app, o2session *session);
/* main.c: the icon name to use (the installed id, or the in-tree art). */
const char *o2_icon_name(void);
void o2_window_toast(O2Window *self, const char *text);
/* The display's shape: 0 = the television's 4:3, 1 = square pixels. */
void o2_window_apply_aspect(O2Window *self, int aspect);
/* A dropped or command-line file: a BIOS is imported, a cartridge opened,
 * anything else copied to FujiNet's SD folder. */
void o2_window_load_media(O2Window *self, const char *path);
/* A system action (O2_SYSACT_*) from any window's keys, with the main
 * window's feedback: Reset to CONFIG toasts, Debugger Stop opens the
 * debugger. */
void o2_window_run_sysaction(O2Window *self, int sysact);
/* Import BIOS: imports `path` and shows o2session_import_bios's message,
 * which always says what happened, over `over`. Returns the image's index,
 * or -1. */
int o2_show_bios_import(GtkWidget *over, o2session *session, const char *path);

/* The accent colour (O2SESSION_ACCENT_RGB, the console case's tan) as
 * CSS classes every window can use: ".o2-accent" paints a widget's
 * background in it (with dark text: the tan is pale), ".o2-accent-text"
 * its label (the darker tan on a light theme), and ".o2-pill" makes a
 * label a small rounded indicator. Installed once by the main window. */
void o2_install_accent_css(void);

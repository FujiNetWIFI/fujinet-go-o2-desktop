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
/* Smooth (bilinear) scaling of the picture, or the sharp pixels. */
void o2_window_apply_smooth(O2Window *self, gboolean smooth);
/* A dropped or command-line file: a BIOS or ROM set is imported, a cartridge
 * opened, anything else copied to FujiNet's SD folder. */
void o2_window_load_media(O2Window *self, const char *path);
/* A system action (O2_SYSACT_*) from any window's keys, with the main
 * window's feedback: Reset to CONFIG toasts, Debugger Stop opens the
 * debugger. */
void o2_window_run_sysaction(O2Window *self, int sysact);
/* A key from any of the app's windows, handled exactly as the main window
 * handles its own: F9/F11/F12, the system actions, then the machine's keys.
 * Returns whether it was used. keycode is the GDK hardware keycode. */
gboolean o2_window_key(O2Window *self, guint keyval, guint keycode,
                       GdkModifierType state, gboolean down);
/* Focus left one of the app's windows: let go of every key the machine
 * holds (and forget which system-action keys were down). */
void o2_window_release_keys(O2Window *self);

/* Import BIOS...: asks for files (a zip of them too) or, with `folder`, a
 * folder; imports what the session recognises and says what happened in a
 * dialog over `over`. With `apply`, an import that supplies something the
 * running machine was missing power-cycles it (opening `retry` again, if it
 * is a cartridge that was refused for want of the BIOS); without, the caller
 * applies it at its own next power cycle (Preferences, on close).
 * done(imported, user_data) runs afterwards, imported = items taken. */
void o2_window_import_bios(O2Window *self, GtkWidget *over, gboolean folder,
                           gboolean apply, const char *retry,
                           void (*done)(int imported, gpointer user_data),
                           gpointer user_data);

/* The accent colour (O2SESSION_ACCENT_RGB, the Odyssey's power-button red) as
 * CSS classes every window can use: ".o2-accent" paints a widget's
 * background in it (with white text), ".o2-accent-text" its label (a tint of
 * it on a dark theme, the darker red on a light one), and ".o2-pill" makes a
 * label a small rounded indicator. Installed once by the main window. */
void o2_install_accent_css(void);

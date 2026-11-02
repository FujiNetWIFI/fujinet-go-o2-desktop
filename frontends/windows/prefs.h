/*
 * The Preferences window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef O2_WIN_PREFS_H
#define O2_WIN_PREFS_H

#include "win_common.h"
#include "o2session.h"

/* What the window tells the main window. */
typedef struct {
    /* the picture options changed (applied at once) */
    void (*display_changed)(void);
    /* a BIOS import from the window succeeded: the banner and the next
     * power cycle follow it */
    void (*roms_changed)(void);
    /* the window closed; dirty = a setting that needs a power cycle changed */
    void (*closed)(int dirty);
} o2_prefs_hooks;

/* Shows the window, or raises it when it is open. */
void o2_prefs_show(HWND parent, o2session *session, const o2_prefs_hooks *hooks);
/* Closing without applying anything (quitting): the settings are saved. */
void o2_prefs_abandon(void);
/* An import made elsewhere: refresh what the window shows about the ROMs. */
void o2_prefs_roms_changed(void);
/* The gamepad set changed. */
void o2_prefs_gamepads_changed(void);
/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int o2_prefs_pretranslate(MSG *msg);

#endif

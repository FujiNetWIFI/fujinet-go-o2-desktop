/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef O2_WIN_DISPLAY_H
#define O2_WIN_DISPLAY_H

#include <windows.h>

#include "o2session.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND o2_display_create(HWND parent, o2session *session);
/* Stops the render thread; call before the parent is destroyed. */
void o2_display_destroy(void);
/* The picture is on screen (the main window is not minimized): off, the
 * render thread sleeps until it is on again, with no wakeups at all. */
void o2_display_set_active(int active);
/* tv: the television's 4:3 (else square pixels, 360:243); smooth: bilinear
 * (else sharp pixels). Safe from any thread. */
void o2_display_set_options(int tv, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *o2_display_renderer(void);

#endif

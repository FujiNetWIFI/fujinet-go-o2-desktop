/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "o2session.h"

#define FN_VIEW      O2DisplayView
#define FN_SESSION   o2session
#define FN_FB_W      O2SESSION_FB_WIDTH
#define FN_FB_H      O2SESSION_FB_HEIGHT
#define FN_INITIAL_W 972
#define FN_INITIAL_H 729

/* The television's shape: 4:3 (setting "aspect" 0, the default). */
#define TV_ASPECT (4.0 / 3.0)

static inline int fn_machine_hz(FN_SESSION *s) { return o2session_refresh_rate(s); }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    o2session_notify_vsync(s, ns);
}

/* Every frame is the whole 360 x 243; nothing to measure. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    *lines = FN_FB_H;
    return o2session_copy_frame(s, dst, serial);
}

/* Letterboxed at the TV's 4:3, or at the picture's own 360:243. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s;
    return tv ? TV_ASPECT : (double)FN_FB_W / (double)lines;
}

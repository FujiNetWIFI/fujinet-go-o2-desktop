/*
 * The FujiNet console log window -- see fujilog.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef O2_WIN_FUJILOG_H
#define O2_WIN_FUJILOG_H

#include "win_common.h"
#include "o2session.h"

/* Shows (raising an existing one) the FujiNet console log window: the
 * in-process runtime's captured output, refreshed once a second while the
 * window is open. */
void o2_fujilog_show(HWND parent, o2session *session);

#endif

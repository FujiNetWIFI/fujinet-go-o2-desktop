/*
 * debug.c -- the session's side of the debugger: one engine per session
 * (core/debugger/debugger.c), told when a power cycle takes the machine
 * away and brings a new one up, and fed FujiNet's traffic from the start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "session_internal.h"
#include "debugger_internal.h"

o2debug *o2debug_get(o2session *s)
{
    if (!s)
        return NULL;
    if (!s->debugger)
        s->debugger = o2debug_create();
    return s->debugger;
}

/* The FujiNet trace is always on: it is cheap, and it is most useful about
 * what happened before anyone thought to look. */
void o2debug_trace_fujinet(struct o2session *s)
{
    o2debug_fujinet_install(o2debug_get(s));
}

void o2debug_before_power_cycle(struct o2session *s)
{
    if (s->debugger)
        o2debug_power_down(s->debugger);
}

void o2debug_after_power_cycle(struct o2session *s)
{
    if (!s->debugger)
        return;
    o2debug_set_cart_path(s->debugger, s->cart_path);
    o2debug_power_up(s->debugger);
}

void o2debug_destroy(struct o2session *s)
{
    o2debug_free(s->debugger);
    s->debugger = NULL;
}

/*
 * debugger_internal -- the engine's side of the session glue, the prompt's
 * view of the engine, and what debugger.c shares with vdcview.c. Not
 * installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_DEBUGGER_INTERNAL_H
#define O2_DEBUGGER_INTERNAL_H

#include "o2debug.h"
#include "host.h"
#include "o2_internal.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An engine for the running host, with no session: what o2debug_get() makes
 * for a session, and what the tests make directly. */
o2debug *o2debug_create(void);
void o2debug_free(o2debug *d);
/* The machine is about to go away (a power cycle or a stop): resume and
 * unhook, remembering whether a window held the engine. */
void o2debug_power_down(o2debug *d);
/* A new machine is up: hook it again if a window held the engine (which
 * stops it at its first instruction, so a power cycle can be debugged from
 * the reset vector). */
void o2debug_power_up(o2debug *d);
/* The opened cartridge, for symbols next to it ("" for CONFIG). */
void o2debug_set_cart_path(o2debug *d, const char *path);
/* Feed the FujiNet trace from the transport, from now on. */
void o2debug_fujinet_install(o2debug *d);

/* For the prompt (prompt.c). */
symtab *o2debug_symbols(o2debug *d);
/* Evaluate an expression in the machine's context; 0 or -1 with err. */
int o2debug_eval(o2debug *d, const char *text, long *out, char *err, int errsz);
/* Step n instructions (the prompt's `step n`). */
void o2debug_step_n(o2debug *d, int n);
/* Register names the evaluator knows, for completion. */
extern const char *const o2debug_reg_names[];

/* The running machine, or NULL when the host is not running. Every use is
 * under o2_host_lock (or on the machine thread). */
o2_machine_t *o2debug_machine(void);

/* vdcview.c: the prompt's views of the chips. Each takes the host lock. */
int o2debug_text_vdc(char *dst, int dstsz);
int o2debug_text_objects(char *dst, int dstsz);
int o2debug_text_ef(char *dst, int dstsz);
int o2debug_text_voice(char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* O2_DEBUGGER_INTERNAL_H */

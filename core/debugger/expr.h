/*
 * expr -- the debugger's expression evaluator: breakpoint conditions,
 * print, set, and every address argument the prompt takes.
 *
 *   numbers     $C000  0xC000  C000h  49152  %1010
 *   names       whatever the context resolves: registers, flags, labels,
 *               vpos/hpos/frame/cycles, and value/addr in a condition
 *   memory      [expr] a program-space byte, and one per address space:
 *               p[expr] program, x[expr] external data, i[expr] internal
 *               RAM, v[expr] the VDC's registers -- all side-effect free
 *   operators   C's, with C's precedence: unary - ! ~, * / %, + -, << >>,
 *               < <= > >=, == !=, &, ^, |, &&, ||
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_EXPR_H
#define O2_EXPR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* A name to a value; return 1 if known. */
    int (*name)(void *user, const char *name, long *out);
    /* A byte of memory, without side effects. space is the o2debug_space
     * (0 program, 1 external data, 2 internal RAM, 3 the VDC). */
    uint8_t (*read8)(void *user, int space, uint16_t addr);
    void *user;
} expr_ctx;

/* Evaluate; 0 and *out on success, -1 with a message otherwise. With a NULL
 * context, names evaluate to 0 and memory reads to 0 -- a syntax check. */
int expr_eval(const char *text, const expr_ctx *ctx, long *out, char *err, int errsz);

#ifdef __cplusplus
}
#endif

#endif /* O2_EXPR_H */

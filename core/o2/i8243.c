/* i8243.c -- the Intel 8243 I/O expander (see i8243.h).
 *
 * Transposed from MAME's src/devices/machine/i8243.cpp (BSD-3-Clause,
 * copyright-holder Aaron Giles): p2_r, p2_w, output_update, prog_w, cs_w.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/machine/i8243.cpp; copyright-holder
 * Aaron Giles -- see COMPLIANCE.md)
 */

#include <string.h>

#include "i8243.h"

void i8243_init(i8243_t *d, i8243_in_fn in, i8243_out_fn out, void *ctx)
{
    memset(d, 0, sizeof *d);
    d->p2out = 0x0f;
    d->p2 = 0x0f;
    d->prog = true;
    d->cs = false;
    d->in = in;
    d->out = out;
    d->ctx = ctx;
}

/* handle a read from the MCU port */
uint8_t i8243_p2_r(const i8243_t *d)
{
    return d->p2out;
}

/* handle a write to the MCU port */
void i8243_p2_w(i8243_t *d, uint8_t data)
{
    d->p2 = data & 0x0f;
}

/* helper for port writes (an unconfigured write is dropped) */
static void output_update(i8243_t *d, int which)
{
    if (d->out)
        d->out(d->ctx, which, d->p[which]);
}

/* handle a change in the PROG line state */
void i8243_prog_w(i8243_t *d, int state)
{
    const bool level = state != 0;

    /* on high->low transition state, latch opcode/port */
    if (d->prog && !level && !d->cs)
    {
        d->opcode = d->p2;

        /* if this is a read opcode, copy result to p2out */
        if ((d->opcode >> 2) == I8243_OP_READ)
        {
            const int which = d->opcode & 3;

            /* an unconfigured read leaves the latch as it was */
            if (d->in)
                d->p[which] = d->in(d->ctx, which);
            d->p2out = d->p[which] & 0x0f;
        }
        else
        {
            d->p2out = 0x0f;
        }
    }

    /* on low->high transition state, act on opcode */
    else if (!d->prog && level && !d->cs)
    {
        switch (d->opcode >> 2)
        {
        case I8243_OP_READ:
            d->p2out = 0x0f; /* release expander bus */
            break;

        case I8243_OP_WRITE:
            d->p[d->opcode & 3] = d->p2 & 0x0f;
            output_update(d, d->opcode & 3);
            break;

        case I8243_OP_OR:
            d->p[d->opcode & 3] |= d->p2 & 0x0f;
            output_update(d, d->opcode & 3);
            break;

        case I8243_OP_AND:
            d->p[d->opcode & 3] &= d->p2 & 0x0f;
            output_update(d, d->opcode & 3);
            break;
        }
    }

    /* remember the state */
    d->prog = level;
}

/* handle chip select line (active low) */
void i8243_cs_w(i8243_t *d, int state)
{
    d->cs = state != 0;
    if (d->cs)
        d->p2out = 0x0f;
}

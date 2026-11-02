/*
 * symbols_builtin.c -- the labels every session starts with: the 8048's
 * vectors, the console BIOS's entry points (the names the public g7000.h
 * gives them, which the Videopac programming guides and every assembler
 * source for the console use), the cartridge's three entry vectors, and the
 * FujiNet cartridge's mailbox status page ($F20-$F2F, with the reply window
 * from $F30).
 *
 * The mailbox labels are built from the staged fuji_mailbox.h's own macros
 * (the cartridge firmware's header, staged verbatim), so they cannot drift
 * from the protocol the cartridge speaks. The mailbox's console-side
 * registers are MOVX (external data) addresses, not program addresses, so
 * they are named by o2_xdata_name() for `mem xdata`, not here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"
#include "fuji_mailbox.h"

void symtab_add_builtins(symtab *t)
{
    /* The 8048's own vectors and the BIOS (g7000.h). The stand-in carries
     * readkey and getjoystick at the same addresses. */
    static const struct { uint16_t addr; const char *name; } bios[] = {
        { 0x000, "reset" },
        { 0x003, "int_vector" },
        { 0x007, "timer_vector" },
        { 0x009, "irq" },
        { 0x014, "irqend" },
        { 0x01A, "vsyncirq" },
        { 0x044, "soundirq" },
        { 0x04B, "parsesnd" },
        { 0x0B0, "readkey" },
        { 0x0E7, "vdcenable" },
        { 0x0EC, "extramenable" },
        { 0x0F1, "init" },
        { 0x11C, "gfxoff" },
        { 0x127, "gfxon" },
        { 0x13D, "waitforkey" },
        { 0x16B, "clearchar" },
        { 0x176, "waitvsync" },
        { 0x1A2, "playsound" },
        { 0x1B0, "doclock" },
        { 0x23A, "initclock" },
        { 0x261, "putchar23" },
        { 0x26A, "bittest" },
        { 0x280, "bitclear" },
        { 0x28A, "bitset" },
        { 0x293, "random" },
        { 0x2A4, "nibblemixer" },
        { 0x2C3, "selectgame" },
        { 0x38F, "getjoystick" },
        { 0x3B1, "decodejoystick" },
        { 0x3CF, "multiply" },
        { 0x3DD, "divide" },
        { 0x3EA, "printchar" },
        /* the cartridge: where the BIOS (and the stand-in) enter it */
        { 0x400, "cart_start" },
        { 0x402, "cart_irq" },
        { 0x404, "cart_timer" },
    };
    /* The cartridge's published page, as the console reads it by MOVP. */
    static const struct { uint16_t addr; const char *name; } mailbox[] = {
        { FN_R_PAGE,       "FN_R_PAGE" },
        { FN_R_ACKSEQ,     "FN_R_ACKSEQ" },
        { FN_R_STATUS,     "FN_R_STATUS" },
        { FN_R_ERR,        "FN_R_ERR" },
        { FN_R_REPLY_CMD,  "FN_R_REPLY_CMD" },
        { FN_R_RXLEN_LO,   "FN_R_RXLEN_LO" },
        { FN_R_RXLEN_HI,   "FN_R_RXLEN_HI" },
        { FN_R_BOOT_STATE, "FN_R_BOOT_STATE" },
        { FN_R_BOOT_PCT,   "FN_R_BOOT_PCT" },
        { FN_R_BOOT_ERR,   "FN_R_BOOT_ERR" },
        { FN_R_MAGIC0,     "FN_R_MAGIC0" },
        { FN_R_MAGIC1,     "FN_R_MAGIC1" },
        { FN_R_PROTO_VER,  "FN_R_PROTO_VER" },
        { FN_R_CLAIM,      "FN_R_CLAIM" },
        { FN_R_DATA,       "FN_R_DATA" },
    };

    for (unsigned i = 0; i < sizeof bios / sizeof bios[0]; i++)
        symtab_add(t, bios[i].addr, SYM_BANK_ANY, bios[i].name, 1);
    for (unsigned i = 0; i < sizeof mailbox / sizeof mailbox[0]; i++)
        symtab_add(t, mailbox[i].addr, SYM_BANK_ANY, mailbox[i].name, 1);
}

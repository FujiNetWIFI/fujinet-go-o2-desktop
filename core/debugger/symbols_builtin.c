/*
 * symbols_builtin.c -- the labels every session starts with: the console's
 * RAM, the FujiNet client runtime's entry, and the FujiNet cartridge's
 * mailbox (with protocol 2's poke, COPY and bank registers), text engine
 * and raster.
 *
 * The cartridge labels are built from the staged fuji_mailbox.h and
 * s2_text.h's own macros (the cartridge firmware's headers, staged
 * verbatim), so they cannot drift from the protocol the cartridge speaks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symbols.h"
#include "fuji_mailbox.h"
#include "s2_text.h"

#define MB(off) ((uint16_t)(FN_ARENA_BASE + (off)))

void symtab_add_builtins(symtab *t)
{
    static const struct { uint16_t addr; const char *name; } fixed[] = {
        { 0x0000, "RESET" },
        { 0x0400, "CART" },
        { 0x0402, "s2_entry" },             /* every FujiNet client's s2call.inc */
        { FN_CLAIM_ADDR, "FN_CLAIM" },
        { 0x0800, "RAM" },
        { 0x0900, "DISPLAY_RAM" },
        { 0x09ff, "RAM_TOP" },
        { FN_STUB, "FN_STUB" },
        { FN_STUB_T, "FN_STUB_T" },
        { FN_TEXT_BASE, "S2T_CURSOR" },
        { (uint16_t)(S2T_H_PUTC << 8), "S2T_PUTC" },
        { (uint16_t)(S2T_H_OP << 8), "S2T_OP" },
        { (uint16_t)(S2T_H_PARAM << 8), "S2T_PARAM" },
        { FN_RASTER_BASE, "FN_RASTER" },
        { 0xf800, "FN_RASTER_MIRROR" },
    };
    static const struct { uint16_t off; const char *name; } mailbox[] = {
        { FN_R_DATA,         "FN_REPLY" },
        { FN_R_ACKSEQ,       "FN_ACKSEQ" },
        { FN_R_STATUS,       "FN_STATUS" },
        { FN_R_ERR,          "FN_ERR" },
        { FN_R_REPLY_CMD,    "FN_REPLY_CMD" },
        { FN_R_RXLEN_LO,     "FN_RXLEN" },
        { FN_R_BOOT_STATE,   "FN_BOOT_STATE" },
        { FN_R_BOOT_PCT,     "FN_BOOT_PCT" },
        { FN_R_BOOT_ERR,     "FN_BOOT_ERR" },
        { FN_R_MAGIC0,       "FN_MAGIC" },
        { FN_R_PROTO_VER,    "FN_PROTO_VER" },
        { FN_R_SLICE_ECHO,   "FN_SLICE_ECHO" },
        { FN_R_MODE,         "FN_MODE" },
        { FN_R_LINK,         "FN_LINK" },
        { FN_R_BOOT_GOT0,    "FN_BOOT_GOT" },
        { FN_R_BOOT_TOT0,    "FN_BOOT_TOT" },
        { FN_R_ARMED,        "FN_ARMED" },
        { FN_R_STAGED,       "FN_STAGED" },
        { FN_R_STAGED_CRC,   "FN_STAGED_CRC" },
        { FN_R_SWAPS,        "FN_SWAPS" },
        { FN_R_FRAMES,       "FN_FRAMES" },
        { FN_R_COPYDONE,     "FN_COPYDONE" },
        { FN_H_REGSEL,       "FN_REGSEL" },
        { FN_H_REGSEL + FN_REG_DEVICE,    "FN_REG_DEVICE" },
        { FN_H_REGSEL + FN_REG_CMD,       "FN_REG_CMD" },
        { FN_H_REGSEL + FN_REG_NPARAM,    "FN_REG_NPARAM" },
        { FN_H_REGSEL + FN_REG_DATA_RST,  "FN_REG_DATA_RST" },
        { FN_H_REGSEL + FN_REG_RXSLICE,   "FN_REG_RXSLICE" },
        { FN_H_REGSEL + FN_REG_SEQ,       "FN_REG_SEQ" },
        { FN_H_REGSEL + FN_REG_BOOTLOCK,  "FN_REG_BOOTLOCK" },
        { FN_H_REGSEL + FN_REG_BOOTSEL_1, "FN_REG_BOOTSEL_1" },
        { FN_H_REGSEL + FN_REG_BOOTSEL_2, "FN_REG_BOOTSEL_2" },
        { FN_H_REGSEL + FN_REG_CONFIG,    "FN_REG_CONFIG" },
        { FN_H_REGSEL + FN_REG_PTRLO,     "FN_REG_PTRLO" },
        { FN_H_REGSEL + FN_REG_PTRHI,     "FN_REG_PTRHI" },
        { FN_H_REGSEL + FN_REG_POKE,      "FN_REG_POKE" },
        { FN_H_REGSEL + FN_REG_CPSRCLO,   "FN_REG_CPSRC" },
        { FN_H_REGSEL + FN_REG_CPDSTLO,   "FN_REG_CPDST" },
        { FN_H_REGSEL + FN_REG_CPLENLO,   "FN_REG_CPLEN" },
        { FN_H_REGSEL + FN_HOT_BANK,      "FN_BANK" },     /* + n: bank n */
        { FN_H_REGDATA,      "FN_REGDATA" },
        { FN_H_DATA,         "FN_TXDATA" },
    };

    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        symtab_add(t, fixed[i].addr, SYM_BANK_ANY, fixed[i].name, 1);
    for (unsigned i = 0; i < sizeof mailbox / sizeof mailbox[0]; i++)
        symtab_add(t, MB(mailbox[i].off), SYM_BANK_ANY, mailbox[i].name, 1);
}

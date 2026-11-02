/* dasm48.c -- see dasm48.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/cpu/mcs48/mcs48dsm.cpp; copyright-holders
 * Aaron Giles -- see COMPLIANCE.md)
 *
 * MAME's disassemble() is one switch per opcode that streams the text and
 * sets `flags`; here the I8048 cases become a 256-entry table of the same
 * strings (the UPI-41 and 8021/8022 alternatives are dropped, so the opcodes
 * those chips repurpose print "illegal" as MAME does for an 8048). Two things
 * kept from MAME on purpose: 0x01 disassembles as "idl" (a CMOS opcode) even
 * though the 8048 core treats it as illegal, and DJNZ is flagged STEP_OVER.
 */

#include <stdio.h>

#include "dasm48.h"

enum {
    K_NONE,         /* one byte, text as is */
    K_IMM,          /* "...#$%02X" with the operand byte */
    K_PAGE,         /* JMP / CALL: "$%d%02X" with the opcode's page and the operand */
    K_COND          /* "...$%03X" with ((pc + 1) & $700) | the operand */
};

typedef struct {
    const char *fmt;
    uint8_t kind;
    uint32_t flags;
} dasm_op;

#define NOARG(s)   { s, K_NONE, 0 }
#define IMMED(s)   { s, K_IMM, 0 }
#define JUMP(s)    { s, K_PAGE, 0 }
#define CALL(s)    { s, K_PAGE, DASM48_STEP_OVER }
#define JCOND(s)   { s, K_COND, DASM48_STEP_COND }
#define DJNZ(s)    { s, K_COND, DASM48_STEP_OVER }
#define RETURN(s)  { s, K_NONE, DASM48_STEP_OUT }
#define ILLEGAL    { "illegal", K_NONE, 0 }

/* eight entries, for r0-r7 */
#define REGS8(s)  NOARG(s "0"), NOARG(s "1"), NOARG(s "2"), NOARG(s "3"), \
                  NOARG(s "4"), NOARG(s "5"), NOARG(s "6"), NOARG(s "7")
#define IMMR8(s)  IMMED(s "0,#$%02X"), IMMED(s "1,#$%02X"), IMMED(s "2,#$%02X"), \
                  IMMED(s "3,#$%02X"), IMMED(s "4,#$%02X"), IMMED(s "5,#$%02X"), \
                  IMMED(s "6,#$%02X"), IMMED(s "7,#$%02X")
#define DJNZ8(s)  DJNZ(s "0,$%03X"), DJNZ(s "1,$%03X"), DJNZ(s "2,$%03X"), DJNZ(s "3,$%03X"), \
                  DJNZ(s "4,$%03X"), DJNZ(s "5,$%03X"), DJNZ(s "6,$%03X"), DJNZ(s "7,$%03X")

static const dasm_op ops[256] = {
    /* 00 */ NOARG("nop"), NOARG("idl"), NOARG("outl bus,a"), IMMED("add  a,#$%02X"),
             JUMP("jmp  $%d%02X"), NOARG("en   i"), ILLEGAL, NOARG("dec  a"),
             NOARG("ins  a,bus"), NOARG("in   a,p1"), NOARG("in   a,p2"), ILLEGAL,
             NOARG("movd a,p4"), NOARG("movd a,p5"), NOARG("movd a,p6"), NOARG("movd a,p7"),
    /* 10 */ NOARG("inc  @r0"), NOARG("inc  @r1"), JCOND("jb0  $%03X"), IMMED("addc a,#$%02X"),
             CALL("call $%d%02X"), NOARG("dis  i"), JCOND("jtf  $%03X"), NOARG("inc  a"),
             REGS8("inc  r"),
    /* 20 */ NOARG("xch  a,@r0"), NOARG("xch  a,@r1"), ILLEGAL, IMMED("mov  a,#$%02X"),
             JUMP("jmp  $%d%02X"), NOARG("en   tcnti"), JCOND("jnt0 $%03X"), NOARG("clr  a"),
             REGS8("xch  a,r"),
    /* 30 */ NOARG("xchd a,@r0"), NOARG("xchd a,@r1"), JCOND("jb1  $%03X"), ILLEGAL,
             CALL("call $%d%02X"), NOARG("dis  tcnti"), JCOND("jt0  $%03X"), NOARG("cpl  a"),
             ILLEGAL, NOARG("outl p1,a"), NOARG("outl p2,a"), ILLEGAL,
             NOARG("movd p4,a"), NOARG("movd p5,a"), NOARG("movd p6,a"), NOARG("movd p7,a"),
    /* 40 */ NOARG("orl  a,@r0"), NOARG("orl  a,@r1"), NOARG("mov  a,t"), IMMED("orl  a,#$%02X"),
             JUMP("jmp  $%d%02X"), NOARG("strt cnt"), JCOND("jnt1 $%03X"), NOARG("swap a"),
             REGS8("orl  a,r"),
    /* 50 */ NOARG("anl  a,@r0"), NOARG("anl  a,@r1"), JCOND("jb2  $%03X"), IMMED("anl  a,#$%02X"),
             CALL("call $%d%02X"), NOARG("strt t"), JCOND("jt1  $%03X"), NOARG("da   a"),
             REGS8("anl  a,r"),
    /* 60 */ NOARG("add  a,@r0"), NOARG("add  a,@r1"), NOARG("mov  t,a"), ILLEGAL,
             JUMP("jmp  $%d%02X"), NOARG("stop tcnt"), ILLEGAL, NOARG("rrc  a"),
             REGS8("add  a,r"),
    /* 70 */ NOARG("addc a,@r0"), NOARG("addc a,@r1"), JCOND("jb3  $%03X"), ILLEGAL,
             CALL("call $%d%02X"), NOARG("ent0 clk"), JCOND("jf1  $%03X"), NOARG("rr   a"),
             REGS8("addc a,r"),
    /* 80 */ NOARG("movx a,@r0"), NOARG("movx a,@r1"), ILLEGAL, RETURN("ret"),
             JUMP("jmp  $%d%02X"), NOARG("clr  f0"), JCOND("jni  $%03X"), ILLEGAL,
             IMMED("orl  bus,#$%02X"), IMMED("orl  p1,#$%02X"), IMMED("orl  p2,#$%02X"), ILLEGAL,
             NOARG("orld p4,a"), NOARG("orld p5,a"), NOARG("orld p6,a"), NOARG("orld p7,a"),
    /* 90 */ NOARG("movx @r0,a"), NOARG("movx @r1,a"), JCOND("jb4  $%03X"), RETURN("retr"),
             CALL("call $%d%02X"), NOARG("cpl  f0"), JCOND("jnz  $%03X"), NOARG("clr  c"),
             IMMED("anl  bus,#$%02X"), IMMED("anl  p1,#$%02X"), IMMED("anl  p2,#$%02X"), ILLEGAL,
             NOARG("anld p4,a"), NOARG("anld p5,a"), NOARG("anld p6,a"), NOARG("anld p7,a"),
    /* a0 */ NOARG("mov  @r0,a"), NOARG("mov  @r1,a"), ILLEGAL, NOARG("movp a,@a"),
             JUMP("jmp  $%d%02X"), NOARG("clr  f1"), ILLEGAL, NOARG("cpl  c"),
             NOARG("mov  r0,a"), NOARG("mov  r1,a"), NOARG("mov  r2,a"), NOARG("mov  r3,a"),
             NOARG("mov  r4,a"), NOARG("mov  r5,a"), NOARG("mov  r6,a"), NOARG("mov  r7,a"),
    /* b0 */ IMMED("mov  @r0,#$%02X"), IMMED("mov  @r1,#$%02X"), JCOND("jb5  $%03X"), NOARG("jmpp @a"),
             CALL("call $%d%02X"), NOARG("cpl  f1"), JCOND("jf0  $%03X"), ILLEGAL,
             IMMR8("mov  r"),
    /* c0 */ ILLEGAL, ILLEGAL, ILLEGAL, ILLEGAL,
             JUMP("jmp  $%d%02X"), NOARG("sel  rb0"), JCOND("jz   $%03X"), NOARG("mov  a,psw"),
             REGS8("dec  r"),
    /* d0 */ NOARG("xrl  a,@r0"), NOARG("xrl  a,@r1"), JCOND("jb6  $%03X"), IMMED("xrl  a,#$%02X"),
             CALL("call $%d%02X"), NOARG("sel  rb1"), ILLEGAL, NOARG("mov  psw,a"),
             REGS8("xrl  a,r"),
    /* e0 */ ILLEGAL, ILLEGAL, ILLEGAL, NOARG("movp3 a,@a"),
             JUMP("jmp  $%d%02X"), NOARG("sel  mb0"), JCOND("jnc  $%03X"), NOARG("rl   a"),
             DJNZ8("djnz r"),
    /* f0 */ NOARG("mov  a,@r0"), NOARG("mov  a,@r1"), JCOND("jb7  $%03X"), ILLEGAL,
             CALL("call $%d%02X"), NOARG("sel  mb1"), JCOND("jc   $%03X"), NOARG("rlc  a"),
             REGS8("mov  a,r"),
};

int dasm48_length(uint8_t opcode)
{
    return ops[opcode].kind == K_NONE ? 1 : 2;
}

int dasm48(char *buf, size_t n, const uint8_t *opbytes, uint16_t pc, uint32_t *flags)
{
    const uint8_t opcode = opbytes[0];
    const dasm_op *op = &ops[opcode];
    int len = 1;

    if (n == 0)
    {
        if (flags)
            *flags = op->flags | DASM48_SUPPORTED;
        return dasm48_length(opcode);
    }

    switch (op->kind)
    {
    case K_IMM:
        snprintf(buf, n, op->fmt, opbytes[1]);
        len = 2;
        break;
    case K_PAGE:
        snprintf(buf, n, op->fmt, opcode >> 5, opbytes[1]);
        len = 2;
        break;
    case K_COND:
        snprintf(buf, n, op->fmt, ((pc + 1) & 0x700) | opbytes[1]);
        len = 2;
        break;
    default:
        snprintf(buf, n, "%s", op->fmt);
        break;
    }
    if (flags)
        *flags = op->flags | DASM48_SUPPORTED;
    return len;
}

int dasm48_target(const uint8_t *opbytes, uint16_t pc)
{
    const dasm_op *op = &ops[opbytes[0]];

    if (op->kind == K_PAGE)
        return ((opbytes[0] >> 5) << 8) | opbytes[1];
    if (op->kind == K_COND)
        return ((pc + 1) & 0xf00) | opbytes[1];
    return -1;
}

/* dasm48.h -- the Intel 8048 disassembler.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/cpu/mcs48/mcs48dsm.cpp; copyright-holders
 * Aaron Giles -- see COMPLIANCE.md)
 *
 * The I8048 opcode table only (MAME's mcs48_disassembler with upi41 = i802x =
 * false): same mnemonics, spacing, case and operand format, so a listing
 * here and MAME's debugger read alike. core/tests/dasm_test.c holds the two
 * to each other for all 256 opcodes.
 */

#ifndef O2_DASM48_H
#define O2_DASM48_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The flag bits MAME's util::disasm_interface returns beside the length
 * (same values), delivered in *flags. */
#define DASM48_SUPPORTED   0x80000000u  /* the flags below are meaningful */
#define DASM48_STEP_OUT    0x40000000u  /* RET, RETR: the end of a step out */
#define DASM48_STEP_OVER   0x20000000u  /* CALL (and, as in MAME, DJNZ): step over */
#define DASM48_STEP_COND   0x10000000u  /* a conditional jump: may take a branch */

/* Disassemble the instruction at `pc` into buf (at most n bytes, always
 * terminated). `opbytes` holds the opcode and, for a two-byte instruction,
 * the operand after it (read both: the second is only used when the length
 * is 2). *flags (may be NULL) gets DASM48_*. Returns the length in bytes
 * (1 or 2; an illegal opcode is "illegal", length 1).
 *
 * A jump's operand is printed as MAME does: JMP and CALL show the 11-bit
 * address (the page the opcode carries and the operand byte), the
 * conditional jumps and DJNZ show the operand in the page of the operand
 * byte, (pc + 1) & $700. MB (A11) is not shown. */
int dasm48(char *buf, size_t n, const uint8_t *opbytes, uint16_t pc, uint32_t *flags);

/* The length of the instruction that starts with this opcode: 1 or 2. */
int dasm48_length(uint8_t opcode);

/* Where a JMP, CALL, conditional jump or DJNZ at `pc` goes, or -1 for any
 * other opcode. JMP/CALL: the 11-bit address (MB is not known here: or in
 * $800 when it is set and no interrupt is running). Conditional jumps and
 * DJNZ: the actual 12-bit target, in the page of the operand byte. */
int dasm48_target(const uint8_t *opbytes, uint16_t pc);

#ifdef __cplusplus
}
#endif

#endif /* O2_DASM48_H */

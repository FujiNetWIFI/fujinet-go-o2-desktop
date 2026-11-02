/*
 * dasm_test -- the 8048 disassembler against MAME's own (dasm_golden.h, from
 * mcs48dsm.cpp compiled standalone by tools/ab/gen_dasm_golden.sh): every
 * opcode's text and length at four pc/operand pairs, and the STEP_* flags;
 * then the branch targets, and the disassembler against the CPU core -- the
 * opcodes it prints "illegal" are the ones the core does not execute, and
 * every straight-line instruction advances the PC by the printed length.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dasm48.h"
#include "mcs48.h"
#include "dasm_golden.h"

static int failures;
static int checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* flat program memory for the core: 4K, the opcode and its operand at pc */
static uint8_t mem[0x1000];

static uint8_t pread(void *user, uint16_t addr, int kind)
{
    (void)user;
    (void)kind;
    return mem[addr & 0xfff];
}

int main(void)
{
    char buf[64], what[200];
    uint32_t flags;
    int texts = 0;

    /* ---- every opcode against MAME ---- */
    for (int op = 0; op < 256; op++)
    {
        const int nvar = dasm_golden[op].len > 1 ? 4 : 1;
        for (int vi = 0; vi < nvar; vi++)
        {
            const uint8_t bytes[2] = { (uint8_t)op, (uint8_t)dasm_variants[vi].operand };
            const int len = dasm48(buf, sizeof buf, bytes, dasm_variants[vi].pc, &flags);

            snprintf(what, sizeof what, "%02X @%03X: \"%s\" (%d) vs MAME's \"%s\" (%d)", op,
                     dasm_variants[vi].pc, buf, len, dasm_golden[op].text[vi], dasm_golden[op].len);
            check(len == dasm_golden[op].len && strcmp(buf, dasm_golden[op].text[vi]) == 0, what);
            texts++;

            snprintf(what, sizeof what, "%02X: flags %08X vs MAME's %X", op, (unsigned)flags,
                     dasm_golden[op].flags);
            check((flags >> 28) == dasm_golden[op].flags && (flags & 0x0fffffffu) == 0, what);
        }
        snprintf(what, sizeof what, "%02X: dasm48_length %d vs %d", op, dasm48_length((uint8_t)op),
                 dasm_golden[op].len);
        check(dasm48_length((uint8_t)op) == dasm_golden[op].len, what);
    }

    /* ---- the buffer limit ---- */
    {
        const uint8_t bytes[2] = { 0x03, 0x5a };
        char tiny[6];
        memset(tiny, 'x', sizeof tiny);
        check(dasm48(tiny, 5, bytes, 0, NULL) == 2, "a short buffer still reports the length");
        check(tiny[4] == 0 && strncmp(tiny, "add ", 4) == 0, "a short buffer is cut and terminated");
        check(dasm48(NULL, 0, bytes, 0, &flags) == 2 && (flags & DASM48_SUPPORTED),
              "a zero-size buffer is only measured");
    }

    /* ---- targets ---- */
    {
        const uint8_t jmp[2] = { 0xa4, 0x37 };      /* jmp $537 */
        const uint8_t call[2] = { 0xf4, 0x80 };     /* call $780 */
        const uint8_t jnz[2] = { 0x96, 0x10 };      /* jnz in the operand byte's page */
        const uint8_t djnz[2] = { 0xea, 0x42 };
        const uint8_t nop[2] = { 0x00, 0x00 };
        check(dasm48_target(jmp, 0x123) == 0x537, "JMP's target is its page and operand");
        check(dasm48_target(call, 0x123) == 0x780, "CALL's target is its page and operand");
        check(dasm48_target(jnz, 0x123) == 0x110, "JNZ's target is in its page");
        check(dasm48_target(jnz, 0x1ff) == 0x210, "JNZ at a page's last byte jumps into the next page");
        check(dasm48_target(jnz, 0x9a0) == 0x910, "JNZ keeps the MB bit of its pc");
        check(dasm48_target(djnz, 0x040) == 0x042, "DJNZ's target");
        check(dasm48_target(nop, 0x040) == -1, "no target for a NOP");
    }

    /* ---- the core against the disassembler ---- */
    {
        mcs48_bus_t bus;
        mcs48_t cpu;
        int illegal_ops = 0;

        memset(&bus, 0, sizeof bus);
        bus.program_read = pread;
        mcs48_init(&cpu, &bus);
        mcs48_reset(&cpu);

        for (int op = 0; op < 256; op++)
        {
            const uint8_t bytes[2] = { (uint8_t)op, 0x00 };
            const int len = dasm48(buf, sizeof buf, bytes, 0, &flags);
            const int target = dasm48_target(bytes, 0);
            const int is_illegal = strcmp(buf, "illegal") == 0;
            unsigned before;

            /* the CPU's reset state each time, a known place to run from */
            mcs48_reset(&cpu);
            memset(mem, 0, sizeof mem);
            mem[0x100] = (uint8_t)op;
            mem[0x101] = 0x00;
            cpu.pc = 0x100;
            before = cpu.illegal_count;
            mcs48_step(&cpu);

            /* 0x01 is MAME's own inconsistency: the disassembler prints the CMOS
             * "idl", the core table has it illegal */
            if (op == 0x01)
            {
                check(cpu.illegal_count == before + 1 && cpu.pc == 0x101, "01 (idl) is illegal for the core");
                illegal_ops++;
                continue;
            }
            snprintf(what, sizeof what, "%02X \"%s\": core illegal count %u -> %u", op, buf, before,
                     cpu.illegal_count);
            check((cpu.illegal_count != before) == is_illegal, what);
            illegal_ops += is_illegal;

            /* straight-line instructions advance by the printed length */
            if (target < 0 && !(flags & DASM48_STEP_OUT) && op != 0xb3)
            {
                snprintf(what, sizeof what, "%02X \"%s\": pc %03X after, expected %03X", op, buf,
                         cpu.pc, 0x100 + len);
                check(cpu.pc == 0x100 + len, what);
            }
        }
        /* 01 06 0B 22 33 38 3B 63 66 73 82 87 8B 9B A2 A6 B7 C0-C3 D6 E0-E2 F3 = 26 */
        snprintf(what, sizeof what, "%d illegal opcodes, expected 26", illegal_ops);
        check(illegal_ops == 26, what);
    }

    printf("dasm: %d checks (%d opcode texts against MAME), %d failures\n", checks, texts, failures);
    return failures ? 1 : 0;
}

/* mcs48.h -- the Intel 8048, the Odyssey 2 / Videopac's CPU.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/cpu/mcs48/mcs48.cpp and mcs48.h;
 * copyright-holders Dan Boris, Mirko Buffoni, Aaron Giles, Couriersud -- see
 * COMPLIANCE.md)
 *
 * The I8048 of MAME's MCS-48 core only: 1K of internal ROM, 64 bytes of
 * internal RAM, the MB (A11) bank bit, the external bus; no UPI-41 (8041/42),
 * 8021/8022, 8049 or 8050 variants. Field and state names keep MAME's own,
 * minus the m_ prefix, so the two can be diffed side by side.
 *
 * The CPU never owns a program ROM: every program-space read goes through
 * bus.program_read with the full 12-bit address the chip puts out, so the
 * machine decides internal ROM ($000-$3FF, EA low) against the cartridge,
 * exactly as MAME's `program` address space does. MOVX uses a separate 8-bit
 * external data space, as MAME's AS_IO.
 *
 * Timing. One machine cycle = clock / 15. `cycles` is the CPU's own clock, in
 * machine cycles since power-on, and every bus callback sees it up to date:
 * MAME burns an instruction's cycles first and executes it after, so a
 * callback made by an instruction sees the time at the END of the instruction
 * (the counter mode's per-cycle T1 poll sees the time of that cycle). That
 * is what MAME's own scheduler-local time comes to, so the machine can derive
 * the beam position (T1, the 8244's status, vblank interrupts) from `cycles`
 * in a callback.
 *
 * Differences from MAME, all deliberate (see also mcs48.c):
 *  - R0-R7 are addressed through `regbase` (an index into ram[]) where MAME
 *    keeps `regptr` (a pointer into the same RAM); structs stay copyable.
 *  - The ROM/EA address-map view is the machine's business; `ea` is a plain
 *    latch for the EA pin (MCS48_INPUT_EA) that the core itself never reads
 *    (MAME's reset writes BUS with a mask that depends on it, which a callback
 *    here cannot express: bus_write(0xFF) is always called).
 *  - Unwired callbacks read what an undriven pin does in MAME's devcb:
 *    ports and BUS 0xFF, T0/T1 0, MOVX and program reads 0xFF (MAME's open
 *    bus would be whatever the address map says).
 *  - mcs48_step() is one pass of MAME's execute_run loop: interrupt entry (if
 *    one is taken) plus one instruction. Its callbacks (the interrupt
 *    acknowledge, the debugger's instruction hook, ENT0 CLK) are bus fields.
 *
 * Verified against MAME's own I8048 in an Odyssey 2: core/tests/
 * mcs48_ops_test.c runs the probe programs of tools/ab/probes (every legal
 * opcode, the timer, counter, interrupts, banks and ports) and compares pc, A,
 * PSW, T, the prescaler, P1, P2, R0-R7 and the cycle count before every
 * instruction with MAME's trace.
 */

#ifndef O2_MCS48_H
#define O2_MCS48_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSW bits (mcs48.cpp's C_FLAG, A_FLAG, F_FLAG, B_FLAG); bits 0-2 are SP. */
#define MCS48_C_FLAG    0x80
#define MCS48_A_FLAG    0x40    /* AC */
#define MCS48_F_FLAG    0x20    /* F0 */
#define MCS48_B_FLAG    0x10    /* BS: register bank 1 */

/* timecount_enabled bits */
#define MCS48_TIMER_ENABLED    0x01
#define MCS48_COUNTER_ENABLED  0x02

/* execute_set_input() input numbers (MCS48_INPUT_*) */
#define MCS48_INPUT_IRQ  0
#define MCS48_INPUT_EA   1

/* What a program-space read is for. */
enum {
    MCS48_READ_OPCODE = 0,      /* an opcode fetch */
    MCS48_READ_OPERAND,         /* an operand byte after the opcode */
    MCS48_READ_DATA             /* MOVP A,@A / MOVP3 A,@A / JMPP @A */
};

/* mcs48_step() sets last_irq to the interrupt it took, if any. */
enum {
    MCS48_IRQ_NONE = 0,
    MCS48_IRQ_EXTERNAL,         /* INT, vector $003 */
    MCS48_IRQ_TIMER             /* timer overflow, vector $007 */
};

typedef struct mcs48 mcs48_t;

/* The machine. Every callback may be NULL (see the header comment for what
 * an unwired pin reads as). `user` is passed back unchanged. */
typedef struct {
    void *user;

    /* Program space: the 12-bit address (0-$FFF) as the CPU drives it. */
    uint8_t (*program_read)(void *user, uint16_t addr, int kind);

    /* External data space (MOVX), 8-bit address. */
    uint8_t (*ext_read)(void *user, uint8_t addr);
    void (*ext_write)(void *user, uint8_t addr, uint8_t data);

    /* Ports 1 and 2: `port` is 1 or 2. port_read returns the pins (the
     * CPU ANDs it with the output latch, as MAME does: quasi-bidirectional);
     * port_write is called whenever the latch is written, with the latch. */
    uint8_t (*port_read)(void *user, int port);
    void (*port_write)(void *user, int port, uint8_t data);

    /* BUS (DB0-DB7): INS A,BUS / ORL, ANL BUS,#n read it; OUTL BUS,A and
     * the ORL/ANL write it. Also written 0xFF at reset. */
    uint8_t (*bus_read)(void *user);
    void (*bus_write)(void *user, uint8_t data);

    /* T0 (n = 0) and T1 (n = 1). Read by JT0/JNT0/JT1/JNT1, and T1 once per
     * machine cycle while the event counter runs. */
    int (*test_read)(void *user, int n);

    /* PROG pin (the 8243 expander's strobe): 0 = the high-to-low edge that
     * starts a MOVD/ORLD/ANLD, 1 = the low-to-high edge that ends it. P2's
     * low nibble carries the command and data (port_write is called for
     * each step, as in MAME). */
    void (*prog_write)(void *user, int level);

    /* ENT0 CLK: T0 now outputs clock/3. */
    void (*ent0_clk)(void *user);

    /* An interrupt was accepted: 0 = INT, 1 = timer; `pc` is the address it
     * interrupted (MAME's standard_irq_callback). */
    void (*irq_ack)(void *user, int irqline, uint16_t pc);

    /* MAME's debugger_instruction_hook: called with the address of the
     * instruction about to be fetched, after any interrupt entry (so the
     * first instruction of a routine shows as $003 / $007). For tracing and
     * breakpoints; it cannot stop the step. */
    void (*instruction_hook)(void *user, uint16_t pc);
} mcs48_bus_t;

struct mcs48 {
    mcs48_bus_t bus;

    uint16_t prevpc;            /* the instruction last started */
    uint16_t pc;                /* 12-bit program counter (A11 included) */

    uint8_t a;                  /* accumulator */
    uint8_t psw;                /* C AC F0 BS . SP2 SP1 SP0 (bit 3 is never stored; MOV A,PSW reads it 1) */
    bool f1;                    /* F1 (F0 is in psw) */
    uint16_t a11;               /* MB: $000 (SEL MB0) or $800 (SEL MB1) */
    uint8_t p1;                 /* port 1 output latch */
    uint8_t p2;                 /* port 2 output latch */
    uint8_t ea;                 /* EA pin (1 = external ROM) */
    uint8_t timer;              /* 8-bit timer/counter */
    uint8_t prescaler;          /* 5-bit timer prescaler */
    uint8_t t1_history;         /* the last 8 samples of T1 (counter mode) */

    bool irq_state;             /* the INT line is asserted */
    bool irq_polled;            /* the last instruction was a JNI that fell through */
    bool irq_in_progress;       /* an interrupt routine is running */
    bool timer_overflow;        /* a timer overflow awaiting its interrupt */
    bool timer_flag;            /* TF: set on overflow, cleared by JTF */
    bool tirq_enabled;          /* EN/DIS TCNTI */
    bool xirq_enabled;          /* EN/DIS I */
    uint8_t timecount_enabled;  /* MCS48_TIMER_ENABLED | MCS48_COUNTER_ENABLED */

    uint8_t ram[64];            /* internal RAM; R0-R7 are ram[0-7] / ram[24-31],
                                   the stack ram[8-23] */
    int regbase;                /* 0 or 24: the active register bank in ram[] */

    /* ---- not in MAME: the host's and the debugger's view ---- */
    uint64_t cycles;            /* machine cycles since power-on */
    uint64_t instructions;      /* instructions executed (an interrupt entry
                                   counts with the instruction it precedes) */
    int last_irq;               /* MCS48_IRQ_* taken by the last mcs48_step() */
    uint8_t bus_out;            /* the last byte driven on BUS */
    unsigned illegal_count;     /* illegal opcodes executed (MAME logs them) */
    uint16_t illegal_pc;        /* the last one's address */
};

/* MAME's device_start: everything zero, the bus installed, EA low (internal
 * ROM). It does not run the reset; call mcs48_reset() next, as MAME does
 * when a machine starts. */
void mcs48_init(mcs48_t *c, const mcs48_bus_t *bus);

/* MAME's device_reset (what releasing the RESET line does): PC 0, SP 0, BS 0,
 * MB 0, F0 and F1 clear, interrupts and timer/counter off, TF off, ports
 * written 0xFF, BUS written 0xFF. As in MAME it leaves A, C, AC, the RAM,
 * the timer and the prescaler alone. */
void mcs48_reset(mcs48_t *c);

/* One pass of MAME's execute_run loop: take an interrupt if one is pending
 * (INT first, then the timer's), then fetch and execute one instruction.
 * Returns the machine cycles consumed (an interrupt entry is 2 more). The
 * INT line is sampled here, so the machine sets it between calls. */
int mcs48_step(mcs48_t *c);

/* The INT pin (MCS48_INPUT_IRQ): level != 0 asserts. Level-sensitive, as
 * MAME's: the machine clears it when the device that raised it is serviced. */
void mcs48_set_int(mcs48_t *c, int line);

/* The EA pin (MCS48_INPUT_EA). */
void mcs48_set_ea(mcs48_t *c, int ea);

/* ---- debugger helpers (pure) ---- */

static inline int mcs48_sp(const mcs48_t *c) { return c->psw & 7; }

/* R0-R7 of the active bank (n = 0-7) / of bank 0 or 1. */
static inline uint8_t mcs48_reg(const mcs48_t *c, int n) { return c->ram[c->regbase + (n & 7)]; }
static inline uint8_t mcs48_reg_bank(const mcs48_t *c, int bank, int n)
{
    return c->ram[(bank ? 24 : 0) + (n & 7)];
}

/* The two stack entries a CALL pushes, as the return address (12 bits) and
 * the PSW's upper nibble: level 0-7 of the 8-deep stack at ram[8 + 2 * n]. */
static inline uint16_t mcs48_stack_pc(const mcs48_t *c, int n)
{
    return (uint16_t)(c->ram[8 + 2 * (n & 7)] | ((c->ram[9 + 2 * (n & 7)] & 0x0f) << 8));
}
static inline uint8_t mcs48_stack_psw(const mcs48_t *c, int n)
{
    return (uint8_t)(c->ram[9 + 2 * (n & 7)] & 0xf0);
}

#ifdef __cplusplus
}
#endif

#endif /* O2_MCS48_H */

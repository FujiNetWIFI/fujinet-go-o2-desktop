/* mcs48.c -- the Intel 8048 (see mcs48.h).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/cpu/mcs48/mcs48.cpp; copyright-holders
 * Dan Boris, Mirko Buffoni, Aaron Giles, Couriersud -- see COMPLIANCE.md)
 *
 * The functions keep MAME's names and order (opcode_fetch, argument_fetch,
 * update_regptr, push_pc_psw, pull_pc_psw, pull_pc, execute_add/addc/jmp/
 * call/jcc, expander_operation, check_irqs, burn_cycles; mcs48_step is one
 * pass of execute_run, mcs48_init / mcs48_reset are device_start /
 * device_reset), with the m_ prefix dropped from the state. MAME's 256-entry
 * handler table of the
 * I8048 (s_mcs48_opcodes) becomes the switch in execute_op(); every case
 * names the MAME handler(s) it stands for and burns the same cycles.
 *
 * Deviations from MAME (the rest is line for line):
 *  - Only the I8048 table. UPI-41 (STS/DBB/OBF/IBF, P2 flags and DMA), 8021/
 *    8022 and the 8049/8050 variants are gone, so p2_mask() is always 0xFF
 *    and the IBF term of the external interrupt is dropped.
 *  - update_regptr() sets `regbase` (0 or 24) instead of a pointer into the
 *    RAM; RAM accesses wrap at 64 bytes as MAME's 6-bit data space does.
 *  - burn_cycles() counts the machine cycles up in `cycles` (MAME counts the
 *    run budget `icount` down); in counter mode the cycle is added after T1
 *    is polled, as icount is decremented after test_r(1).
 *  - illegal() counts the opcode (illegal_count / illegal_pc) where MAME
 *    logs it.
 *  - Unwired callbacks (see mcs48.h).
 *  - ENT0 CLK calls bus.ent0_clk where MAME calls its t0_clk delegate; the
 *    interrupt acknowledge (standard_irq_callback) is bus.irq_ack and the
 *    debugger_instruction_hook is bus.instruction_hook, called at the same
 *    place in the loop (after check_irqs, before the opcode fetch).
 *  - mcs48_set_ea() only stores the pin: MAME's update_ea() switches the
 *    internal-ROM view of the program map, which here is the machine's.
 */

#include <string.h>

#include "mcs48.h"

/* ------------------------------------------------------------------------
 * Callback shims (MAME's program_r, ram_r, ext_r, port_r, test_r ... )
 * ---------------------------------------------------------------------- */

static inline uint8_t program_r(mcs48_t *c, uint16_t a, int kind)
{
    return c->bus.program_read ? c->bus.program_read(c->bus.user, (uint16_t)(a & 0xfff), kind) : 0xff;
}

static inline uint8_t ram_r(const mcs48_t *c, unsigned a)  { return c->ram[a & 0x3f]; }
static inline void ram_w(mcs48_t *c, unsigned a, uint8_t v) { c->ram[a & 0x3f] = v; }

static inline uint8_t ext_r(mcs48_t *c, uint8_t a)
{
    return c->bus.ext_read ? c->bus.ext_read(c->bus.user, a) : 0xff;
}
static inline void ext_w(mcs48_t *c, uint8_t a, uint8_t v)
{
    if (c->bus.ext_write)
        c->bus.ext_write(c->bus.user, a, v);
}
static inline uint8_t port_r(mcs48_t *c, int p)
{
    return c->bus.port_read ? c->bus.port_read(c->bus.user, p) : 0xff;
}
static inline void port_w(mcs48_t *c, int p, uint8_t v)
{
    if (c->bus.port_write)
        c->bus.port_write(c->bus.user, p, v);
}
static inline int test_r(mcs48_t *c, int n)
{
    return c->bus.test_read ? c->bus.test_read(c->bus.user, n) : 0;
}
static inline uint8_t bus_r(mcs48_t *c)
{
    return c->bus.bus_read ? c->bus.bus_read(c->bus.user) : 0xff;
}
static inline void bus_w(mcs48_t *c, uint8_t v)
{
    c->bus_out = v;
    if (c->bus.bus_write)
        c->bus.bus_write(c->bus.user, v);
}
static inline void prog_w(mcs48_t *c, int v)
{
    if (c->bus.prog_write)
        c->bus.prog_write(c->bus.user, v);
}

/* r0-r7 map to memory via regbase */
#define R(n)   c->ram[c->regbase + (n)]

/* ------------------------------------------------------------------------
 * Inline functions (MAME's "INLINE FUNCTIONS")
 * ---------------------------------------------------------------------- */

/* opcode_fetch / argument_fetch -- the PC increments within its 2K bank */
static uint8_t opcode_fetch(mcs48_t *c)
{
    uint16_t address = c->pc;
    c->pc = (uint16_t)(((c->pc + 1) & 0x7ff) | (c->pc & 0x800));
    return program_r(c, address, MCS48_READ_OPCODE);
}

static uint8_t argument_fetch(mcs48_t *c)
{
    uint16_t address = c->pc;
    c->pc = (uint16_t)(((c->pc + 1) & 0x7ff) | (c->pc & 0x800));
    return program_r(c, address, MCS48_READ_OPERAND);
}

/* update_regptr -- point at the appropriate register bank */
static void update_regptr(mcs48_t *c)
{
    c->regbase = (c->psw & MCS48_B_FLAG) ? 24 : 0;
}

/* push_pc_psw -- push the PC and PSW values onto the stack */
static void push_pc_psw(mcs48_t *c)
{
    uint8_t sp = c->psw & 0x07;
    ram_w(c, 8 + 2u * sp, (uint8_t)c->pc);
    ram_w(c, 9 + 2u * sp, (uint8_t)(((c->pc >> 8) & 0x0f) | (c->psw & 0xf0)));
    c->psw = (uint8_t)((c->psw & 0xf0) | ((sp + 1) & 0x07));
}

/* pull_pc_psw -- pull the PC and PSW values from the stack */
static void pull_pc_psw(mcs48_t *c)
{
    uint8_t sp = (uint8_t)((c->psw - 1) & 0x07);
    c->pc = ram_r(c, 8 + 2u * sp);
    c->pc |= (uint16_t)(ram_r(c, 9 + 2u * sp) << 8);
    c->psw = (uint8_t)(((c->pc >> 8) & 0xf0) | sp);
    c->pc &= c->irq_in_progress ? 0x7ff : 0xfff;
    update_regptr(c);
}

/* pull_pc -- pull the PC value from the stack, leaving the upper part of
 * PSW intact */
static void pull_pc(mcs48_t *c)
{
    uint8_t sp = (uint8_t)((c->psw - 1) & 0x07);
    c->pc = ram_r(c, 8 + 2u * sp);
    c->pc |= (uint16_t)(ram_r(c, 9 + 2u * sp) << 8);
    c->pc &= c->irq_in_progress ? 0x7ff : 0xfff;
    c->psw = (uint8_t)((c->psw & 0xf0) | sp);
}

/* execute_add / execute_addc -- the logic of ADD and ADDC */
static void execute_add(mcs48_t *c, uint8_t dat)
{
    uint16_t temp = (uint16_t)(c->a + dat);
    uint16_t temp4 = (uint16_t)((c->a & 0x0f) + (dat & 0x0f));

    c->psw &= (uint8_t)~(MCS48_C_FLAG | MCS48_A_FLAG);
    c->psw |= (uint8_t)((temp4 << 2) & MCS48_A_FLAG);
    c->psw |= (uint8_t)((temp >> 1) & MCS48_C_FLAG);
    c->a = (uint8_t)temp;
}

static void execute_addc(mcs48_t *c, uint8_t dat)
{
    uint8_t carryin = (uint8_t)((c->psw & MCS48_C_FLAG) >> 7);
    uint16_t temp = (uint16_t)(c->a + dat + carryin);
    uint16_t temp4 = (uint16_t)((c->a & 0x0f) + (dat & 0x0f) + carryin);

    c->psw &= (uint8_t)~(MCS48_C_FLAG | MCS48_A_FLAG);
    c->psw |= (uint8_t)((temp4 << 2) & MCS48_A_FLAG);
    c->psw |= (uint8_t)((temp >> 1) & MCS48_C_FLAG);
    c->a = (uint8_t)temp;
}

/* execute_jmp -- the logic of a JMP: A11 is forced to 0 during an interrupt */
static void execute_jmp(mcs48_t *c, uint16_t address)
{
    uint16_t a11 = c->irq_in_progress ? 0 : c->a11;
    c->pc = (uint16_t)(address | a11);
}

/* execute_call -- the logic of a CALL */
static void execute_call(mcs48_t *c, uint16_t address)
{
    push_pc_psw(c);
    execute_jmp(c, address);
}

/* execute_jcc -- the logic of a conditional jump: the target is in the page
 * of the operand byte */
static void execute_jcc(mcs48_t *c, bool result)
{
    uint16_t pch = c->pc & 0xf00;
    uint8_t offset = argument_fetch(c);
    if (result)
        c->pc = (uint16_t)(pch | offset);
}

/* expander_operation -- an operation via the 8243 expander chip. MAME's
 * expander_op: 0 = read, 1 = write, 2 = or, 3 = and. */
enum { EXPANDER_OP_READ = 0, EXPANDER_OP_WRITE = 1, EXPANDER_OP_OR = 2, EXPANDER_OP_AND = 3 };

static void expander_operation(mcs48_t *c, int operation, uint8_t port)
{
    /* put opcode on low 4 bits of P2 (overwriting latch) */
    c->p2 = (uint8_t)((c->p2 & 0xf0) | ((unsigned)operation << 2) | (port & 3));
    port_w(c, 2, c->p2);

    /* generate high-to-low transition on PROG line */
    prog_w(c, 0);

    /* transfer data on low 4 bits of P2 */
    if (operation != EXPANDER_OP_READ)
    {
        c->p2 = (uint8_t)((c->p2 & 0xf0) | (c->a & 0x0f));
        port_w(c, 2, c->p2);
    }
    else
    {
        /* place P20-P23 in input mode */
        c->p2 |= 0x0f;
        port_w(c, 2, c->p2);

        /* input data to lower 4 bits of A (upper 4 bits are cleared) */
        c->a = port_r(c, 2) & 0x0f;
    }

    /* generate low-to-high transition on PROG line */
    prog_w(c, 1);
}

/* burn_cycles -- burn cycles, processing timers and counters */
static void burn_cycles(mcs48_t *c, int count)
{
    if (c->timecount_enabled)
    {
        bool timerover = false;

        /* if the timer is enabled, accumulate prescaler cycles */
        if (c->timecount_enabled & MCS48_TIMER_ENABLED)
        {
            uint8_t oldtimer = c->timer;
            c->prescaler = (uint8_t)(c->prescaler + count);
            c->timer = (uint8_t)(c->timer + (c->prescaler >> 5));
            c->prescaler &= 0x1f;
            timerover = c->timer < oldtimer;
        }

        /* if the counter is enabled, poll the T1 test input once for each cycle */
        else if (c->timecount_enabled & MCS48_COUNTER_ENABLED)
        {
            for ( ; count > 0; count--, c->cycles++)
            {
                c->t1_history = (uint8_t)((c->t1_history << 1) | (test_r(c, 1) & 1));
                if ((c->t1_history & 3) == 2)
                {
                    if (++c->timer == 0)
                        timerover = true;
                }
            }
        }

        /* if either source caused a timer overflow, set the flags */
        if (timerover)
        {
            c->timer_flag = true;

            /* according to the docs, if an overflow occurs with interrupts disabled,
               the overflow is not stored */
            if (c->tirq_enabled)
                c->timer_overflow = true;
        }
    }

    /* (note: if timer counter is enabled, count was already reduced to 0) */
    c->cycles += (uint64_t)count;
}

/* ------------------------------------------------------------------------
 * Opcode handlers (MAME's OPHANDLER table for the I8048)
 * ---------------------------------------------------------------------- */

static void illegal(mcs48_t *c)
{
    burn_cycles(c, 1);
    c->illegal_count++;
    c->illegal_pc = c->prevpc;
}

static void execute_op(mcs48_t *c, unsigned opcode)
{
    const int n = opcode & 7;           /* the register / port in the low bits */
    const unsigned pg = (opcode >> 5) & 7;      /* the page of JMP, CALL; the bit of JBn */

    switch (opcode)
    {
    /* ---- 0x00 ---- */
    case 0x00: burn_cycles(c, 1); break;                                        /* nop */
    case 0x02: burn_cycles(c, 2); bus_w(c, c->a); break;                        /* outl_bus_a */
    case 0x03: burn_cycles(c, 2); execute_add(c, argument_fetch(c)); break;     /* add_a_n */
    case 0x04: case 0x24: case 0x44: case 0x64:
    case 0x84: case 0xa4: case 0xc4: case 0xe4:                                 /* jmp_0 .. jmp_7 */
        burn_cycles(c, 2);
        execute_jmp(c, (uint16_t)(argument_fetch(c) | (pg << 8)));
        break;
    case 0x05: burn_cycles(c, 1); c->xirq_enabled = true; break;               /* en_i */
    case 0x07: burn_cycles(c, 1); c->a--; break;                                /* dec_a */
    case 0x08: burn_cycles(c, 2); c->a = bus_r(c); break;                       /* ins_a_bus */
    case 0x09: burn_cycles(c, 2); c->a = port_r(c, 1) & c->p1; break;          /* in_a_p1 */
    case 0x0a: burn_cycles(c, 2); c->a = port_r(c, 2) & c->p2; break;          /* in_a_p2 */
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:                                 /* movd_a_p4 .. p7 */
        burn_cycles(c, 2); expander_operation(c, EXPANDER_OP_READ, (uint8_t)(4 + n)); break;

    /* ---- 0x10 ---- */
    case 0x10: case 0x11:                                                       /* inc_xr0, inc_xr1 */
        burn_cycles(c, 1); ram_w(c, R(n), (uint8_t)(ram_r(c, R(n)) + 1)); break;
    case 0x12: case 0x32: case 0x52: case 0x72:
    case 0x92: case 0xb2: case 0xd2: case 0xf2:                                 /* jb_0 .. jb_7 */
        burn_cycles(c, 2); execute_jcc(c, (c->a & (1u << pg)) != 0); break;
    case 0x13: burn_cycles(c, 2); execute_addc(c, argument_fetch(c)); break;    /* adc_a_n */
    case 0x14: case 0x34: case 0x54: case 0x74:
    case 0x94: case 0xb4: case 0xd4: case 0xf4:                                 /* call_0 .. call_7 */
        burn_cycles(c, 2);
        execute_call(c, (uint16_t)(argument_fetch(c) | (pg << 8)));
        break;
    case 0x15: burn_cycles(c, 1); c->xirq_enabled = false; break;              /* dis_i */
    case 0x16: burn_cycles(c, 2); execute_jcc(c, c->timer_flag); c->timer_flag = false; break;  /* jtf */
    case 0x17: burn_cycles(c, 1); c->a++; break;                                /* inc_a */
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:                                 /* inc_r0 .. inc_r7 */
        burn_cycles(c, 1); R(n)++; break;

    /* ---- 0x20 ---- */
    case 0x20: case 0x21:                                                       /* xch_a_xr0, xch_a_xr1 */
    {
        burn_cycles(c, 1);
        uint8_t tmp = c->a; c->a = ram_r(c, R(n)); ram_w(c, R(n), tmp);
        break;
    }
    case 0x23: burn_cycles(c, 2); c->a = argument_fetch(c); break;              /* mov_a_n */
    case 0x25: burn_cycles(c, 1); c->tirq_enabled = true; break;               /* en_tcnti */
    case 0x26: burn_cycles(c, 2); execute_jcc(c, test_r(c, 0) == 0); break;     /* jnt_0 */
    case 0x27: burn_cycles(c, 1); c->a = 0; break;                              /* clr_a */
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x2c: case 0x2d: case 0x2e: case 0x2f:                                 /* xch_a_r0 .. xch_a_r7 */
    {
        burn_cycles(c, 1);
        uint8_t tmp = c->a; c->a = R(n); R(n) = tmp;
        break;
    }

    /* ---- 0x30 ---- */
    case 0x30: case 0x31:                                                       /* xchd_a_xr0, xchd_a_xr1 */
    {
        burn_cycles(c, 1);
        uint8_t oldram = ram_r(c, R(n));
        ram_w(c, R(n), (uint8_t)((oldram & 0xf0) | (c->a & 0x0f)));
        c->a = (uint8_t)((c->a & 0xf0) | (oldram & 0x0f));
        break;
    }
    case 0x35: burn_cycles(c, 1); c->tirq_enabled = false; c->timer_overflow = false; break;  /* dis_tcnti */
    case 0x36: burn_cycles(c, 2); execute_jcc(c, test_r(c, 0) != 0); break;     /* jt_0 */
    case 0x37: burn_cycles(c, 1); c->a ^= 0xff; break;                          /* cpl_a */
    case 0x39: burn_cycles(c, 2); c->p1 = c->a; port_w(c, 1, c->p1); break;     /* outl_p1_a */
    case 0x3a: burn_cycles(c, 2); c->p2 = c->a; port_w(c, 2, c->p2); break;     /* outl_p2_a (p2_mask = 0xff) */
    case 0x3c: case 0x3d: case 0x3e: case 0x3f:                                 /* movd_p4_a .. p7_a */
        burn_cycles(c, 2); expander_operation(c, EXPANDER_OP_WRITE, (uint8_t)(4 + n)); break;

    /* ---- 0x40 ---- */
    case 0x40: case 0x41: burn_cycles(c, 1); c->a |= ram_r(c, R(n)); break;     /* orl_a_xr0, orl_a_xr1 */
    case 0x42: burn_cycles(c, 1); c->a = c->timer; break;                       /* mov_a_t */
    case 0x43: burn_cycles(c, 2); c->a |= argument_fetch(c); break;             /* orl_a_n */
    case 0x45:                                                                  /* strt_cnt */
        burn_cycles(c, 1);
        if (!(c->timecount_enabled & MCS48_COUNTER_ENABLED))
            c->t1_history = (uint8_t)test_r(c, 1);
        c->timecount_enabled = MCS48_COUNTER_ENABLED;
        break;
    case 0x46: burn_cycles(c, 2); execute_jcc(c, test_r(c, 1) == 0); break;     /* jnt_1 */
    case 0x47: burn_cycles(c, 1); c->a = (uint8_t)((c->a << 4) | (c->a >> 4)); break;  /* swap_a */
    case 0x48: case 0x49: case 0x4a: case 0x4b:
    case 0x4c: case 0x4d: case 0x4e: case 0x4f: burn_cycles(c, 1); c->a |= R(n); break;  /* orl_a_r0 .. r7 */

    /* ---- 0x50 ---- */
    case 0x50: case 0x51: burn_cycles(c, 1); c->a &= ram_r(c, R(n)); break;     /* anl_a_xr0, anl_a_xr1 */
    case 0x53: burn_cycles(c, 2); c->a &= argument_fetch(c); break;             /* anl_a_n */
    case 0x55:                                                                  /* strt_t */
        burn_cycles(c, 1); c->timecount_enabled = MCS48_TIMER_ENABLED; c->prescaler = 0; break;
    case 0x56: burn_cycles(c, 2); execute_jcc(c, test_r(c, 1) != 0); break;     /* jt_1 */
    case 0x57:                                                                  /* da_a */
        burn_cycles(c, 1);
        if ((c->a & 0x0f) > 0x09 || (c->psw & MCS48_A_FLAG))
        {
            if (c->a > 0xf9)
                c->psw |= MCS48_C_FLAG;
            c->a = (uint8_t)(c->a + 0x06);
        }
        if ((c->a & 0xf0) > 0x90 || (c->psw & MCS48_C_FLAG))
        {
            c->a = (uint8_t)(c->a + 0x60);
            c->psw |= MCS48_C_FLAG;
        }
        break;
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f: burn_cycles(c, 1); c->a &= R(n); break;  /* anl_a_r0 .. r7 */

    /* ---- 0x60 ---- */
    case 0x60: case 0x61: burn_cycles(c, 1); execute_add(c, ram_r(c, R(n))); break;  /* add_a_xr0, add_a_xr1 */
    case 0x62: burn_cycles(c, 1); c->timer = c->a; break;                       /* mov_t_a */
    case 0x65: burn_cycles(c, 1); c->timecount_enabled = 0; break;             /* stop_tcnt */
    case 0x67:                                                                  /* rrc_a */
    {
        burn_cycles(c, 1);
        uint8_t newc = (uint8_t)((c->a << 7) & MCS48_C_FLAG);
        c->a = (uint8_t)((c->a >> 1) | (c->psw & MCS48_C_FLAG));
        c->psw = (uint8_t)((c->psw & ~MCS48_C_FLAG) | newc);
        break;
    }
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: burn_cycles(c, 1); execute_add(c, R(n)); break;  /* add_a_r0 .. r7 */

    /* ---- 0x70 ---- */
    case 0x70: case 0x71: burn_cycles(c, 1); execute_addc(c, ram_r(c, R(n))); break;  /* adc_a_xr0, adc_a_xr1 */
    case 0x75:                                                                  /* ent0_clk */
        burn_cycles(c, 1);
        if (c->bus.ent0_clk)
            c->bus.ent0_clk(c->bus.user);
        break;
    case 0x76: burn_cycles(c, 2); execute_jcc(c, c->f1); break;                 /* jf1 */
    case 0x77: burn_cycles(c, 1); c->a = (uint8_t)((c->a >> 1) | (c->a << 7)); break;  /* rr_a */
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: burn_cycles(c, 1); execute_addc(c, R(n)); break;  /* adc_a_r0 .. r7 */

    /* ---- 0x80 ---- */
    case 0x80: case 0x81: burn_cycles(c, 2); c->a = ext_r(c, R(n)); break;      /* movx_a_xr0, movx_a_xr1 */
    case 0x83: burn_cycles(c, 2); pull_pc(c); break;                            /* ret */
    case 0x85: burn_cycles(c, 1); c->psw &= (uint8_t)~MCS48_F_FLAG; break;     /* clr_f0 */
    case 0x86:                                                                  /* jni */
        burn_cycles(c, 2);
        c->irq_polled = (c->irq_state == 0);
        execute_jcc(c, c->irq_state != 0);
        break;
    case 0x88: burn_cycles(c, 2); bus_w(c, (uint8_t)(bus_r(c) | argument_fetch(c))); break;  /* orl_bus_n */
    case 0x89: burn_cycles(c, 2); c->p1 |= argument_fetch(c); port_w(c, 1, c->p1); break;    /* orl_p1_n */
    case 0x8a: burn_cycles(c, 2); c->p2 |= argument_fetch(c); port_w(c, 2, c->p2); break;    /* orl_p2_n (p2_mask = 0xff) */
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:                                 /* orld_p4_a .. p7_a */
        burn_cycles(c, 2); expander_operation(c, EXPANDER_OP_OR, (uint8_t)(4 + n)); break;

    /* ---- 0x90 ---- */
    case 0x90: case 0x91: burn_cycles(c, 2); ext_w(c, R(n), c->a); break;       /* movx_xr0_a, movx_xr1_a */
    case 0x93:                                                                  /* retr */
        burn_cycles(c, 2);
        /* implicitly clear the IRQ in progress flip flop */
        c->irq_in_progress = false;
        pull_pc_psw(c);
        break;
    case 0x95: burn_cycles(c, 1); c->psw ^= MCS48_F_FLAG; break;               /* cpl_f0 */
    case 0x96: burn_cycles(c, 2); execute_jcc(c, c->a != 0); break;             /* jnz */
    case 0x97: burn_cycles(c, 1); c->psw &= (uint8_t)~MCS48_C_FLAG; break;     /* clr_c */
    case 0x98: burn_cycles(c, 2); bus_w(c, (uint8_t)(bus_r(c) & argument_fetch(c))); break;  /* anl_bus_n */
    case 0x99: burn_cycles(c, 2); c->p1 &= argument_fetch(c); port_w(c, 1, c->p1); break;    /* anl_p1_n */
    case 0x9a: burn_cycles(c, 2); c->p2 &= argument_fetch(c); port_w(c, 2, c->p2); break;    /* anl_p2_n (p2_mask = 0xff) */
    case 0x9c: case 0x9d: case 0x9e: case 0x9f:                                 /* anld_p4_a .. p7_a */
        burn_cycles(c, 2); expander_operation(c, EXPANDER_OP_AND, (uint8_t)(4 + n)); break;

    /* ---- 0xA0 ---- */
    case 0xa0: case 0xa1: burn_cycles(c, 1); ram_w(c, R(n), c->a); break;       /* mov_xr0_a, mov_xr1_a */
    case 0xa3:                                                                  /* movp_a_xa */
        burn_cycles(c, 2);
        c->a = program_r(c, (uint16_t)((c->pc & 0xf00) | c->a), MCS48_READ_DATA);
        break;
    case 0xa5: burn_cycles(c, 1); c->f1 = false; break;                         /* clr_f1 */
    case 0xa7: burn_cycles(c, 1); c->psw ^= MCS48_C_FLAG; break;               /* cpl_c */
    case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xac: case 0xad: case 0xae: case 0xaf: burn_cycles(c, 1); R(n) = c->a; break;  /* mov_r0_a .. r7_a */

    /* ---- 0xB0 ---- */
    case 0xb0: case 0xb1: burn_cycles(c, 2); ram_w(c, R(n), argument_fetch(c)); break;  /* mov_xr0_n, mov_xr1_n */
    case 0xb3:                                                                  /* jmpp_xa */
        burn_cycles(c, 2);
        c->pc &= 0xf00;
        c->pc |= program_r(c, (uint16_t)(c->pc | c->a), MCS48_READ_DATA);
        break;
    case 0xb5: burn_cycles(c, 1); c->f1 = !c->f1; break;                        /* cpl_f1 */
    case 0xb6: burn_cycles(c, 2); execute_jcc(c, (c->psw & MCS48_F_FLAG) != 0); break;  /* jf0 */
    case 0xb8: case 0xb9: case 0xba: case 0xbb:
    case 0xbc: case 0xbd: case 0xbe: case 0xbf: burn_cycles(c, 2); R(n) = argument_fetch(c); break;  /* mov_r0_n .. r7_n */

    /* ---- 0xC0 ---- */
    case 0xc5: burn_cycles(c, 1); c->psw &= (uint8_t)~MCS48_B_FLAG; update_regptr(c); break;  /* sel_rb0 */
    case 0xc6: burn_cycles(c, 2); execute_jcc(c, c->a == 0); break;             /* jz */
    case 0xc7: burn_cycles(c, 1); c->a = (uint8_t)(c->psw | 0x08); break;       /* mov_a_psw */
    case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xce: case 0xcf: burn_cycles(c, 1); R(n)--; break;  /* dec_r0 .. r7 */

    /* ---- 0xD0 ---- */
    case 0xd0: case 0xd1: burn_cycles(c, 1); c->a ^= ram_r(c, R(n)); break;     /* xrl_a_xr0, xrl_a_xr1 */
    case 0xd3: burn_cycles(c, 2); c->a ^= argument_fetch(c); break;             /* xrl_a_n */
    case 0xd5: burn_cycles(c, 1); c->psw |= MCS48_B_FLAG; update_regptr(c); break;  /* sel_rb1 */
    case 0xd7:                                                                  /* mov_psw_a */
        burn_cycles(c, 1); c->psw = (uint8_t)(c->a & ~0x08); update_regptr(c); break;
    case 0xd8: case 0xd9: case 0xda: case 0xdb:
    case 0xdc: case 0xdd: case 0xde: case 0xdf: burn_cycles(c, 1); c->a ^= R(n); break;  /* xrl_a_r0 .. r7 */

    /* ---- 0xE0 ---- */
    case 0xe3:                                                                  /* movp3_a_xa */
        burn_cycles(c, 2);
        c->a = program_r(c, (uint16_t)(0x300 | c->a), MCS48_READ_DATA);
        break;
    case 0xe5: burn_cycles(c, 1); c->a11 = 0x000; break;                        /* sel_mb0 */
    case 0xe6: burn_cycles(c, 2); execute_jcc(c, (c->psw & MCS48_C_FLAG) == 0); break;  /* jnc */
    case 0xe7: burn_cycles(c, 1); c->a = (uint8_t)((c->a << 1) | (c->a >> 7)); break;   /* rl_a */
    case 0xe8: case 0xe9: case 0xea: case 0xeb:
    case 0xec: case 0xed: case 0xee: case 0xef:                                 /* djnz_r0 .. djnz_r7 */
        burn_cycles(c, 2); execute_jcc(c, --R(n) != 0); break;

    /* ---- 0xF0 ---- */
    case 0xf0: case 0xf1: burn_cycles(c, 1); c->a = ram_r(c, R(n)); break;      /* mov_a_xr0, mov_a_xr1 */
    case 0xf5: burn_cycles(c, 1); c->a11 = 0x800; break;                        /* sel_mb1 */
    case 0xf6: burn_cycles(c, 2); execute_jcc(c, (c->psw & MCS48_C_FLAG) != 0); break;  /* jc */
    case 0xf7:                                                                  /* rlc_a */
    {
        burn_cycles(c, 1);
        uint8_t newc = (uint8_t)(c->a & MCS48_C_FLAG);
        c->a = (uint8_t)((c->a << 1) | (c->psw >> 7));
        c->psw = (uint8_t)((c->psw & ~MCS48_C_FLAG) | newc);
        break;
    }
    case 0xf8: case 0xf9: case 0xfa: case 0xfb:
    case 0xfc: case 0xfd: case 0xfe: case 0xff: burn_cycles(c, 1); c->a = R(n); break;  /* mov_a_r0 .. r7 */

    /* the I8048 table's `illegal` entries: 01 06 0B 22 33 38 3B 63 66 73 82
       87 8B 9B A2 A6 B7 C0-C3 D6 E0-E2 F3 (the UPI-41 opcodes included) */
    default:
        illegal(c);
        break;
    }
}

/* ------------------------------------------------------------------------
 * Initialization/reset (device_start, device_reset)
 * ---------------------------------------------------------------------- */

void mcs48_init(mcs48_t *c, const mcs48_bus_t *bus)
{
    /* zerofill */
    memset(c, 0, sizeof *c);
    if (bus)
        c->bus = *bus;

    /* default EA pin state: the 8048 has its ROM inside */
    c->ea = 0;

    /* ensure that regptr is valid */
    update_regptr(c);
}

void mcs48_reset(mcs48_t *c)
{
    /* confirmed from reset description */
    c->pc = 0;
    c->psw = c->psw & (MCS48_C_FLAG | MCS48_A_FLAG);
    update_regptr(c);
    c->f1 = false;
    c->a11 = 0;

    c->tirq_enabled = false;
    c->xirq_enabled = false;
    c->timecount_enabled = 0;
    c->timer_flag = false;

    /* confirmed from interrupt logic description */
    c->irq_in_progress = false;
    c->timer_overflow = false;

    c->irq_polled = false;

    /* BUS is set to high-impedance (except when EA=1) */
    bus_w(c, 0xff);

    /* port 1 and port 2 are set to input mode */
    c->p1 = 0xff;
    c->p2 = 0xff;
    port_w(c, 1, c->p1);
    port_w(c, 2, c->p2);
}

/* ------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */

/* check_irqs -- check for and process IRQs */
static void check_irqs(mcs48_t *c)
{
    c->last_irq = MCS48_IRQ_NONE;

    /* if something is in progress, we do nothing */
    if (c->irq_in_progress)
        return;

    /* external interrupts take priority */
    else if (c->irq_state && c->xirq_enabled)
    {
        /* indicate we took the external IRQ */
        if (c->bus.irq_ack)
            c->bus.irq_ack(c->bus.user, 0, c->pc);
        c->last_irq = MCS48_IRQ_EXTERNAL;

        burn_cycles(c, 2);
        c->irq_in_progress = true;

        /* force JNI to be taken (hack) */
        if (c->irq_polled)
        {
            c->pc = (uint16_t)(((c->prevpc + 1) & 0x7ff) | (c->prevpc & 0x800));
            execute_jcc(c, true);
        }

        /* transfer to location 0x03 */
        execute_call(c, 0x03);
    }

    /* timer overflow interrupts follow */
    else if (c->timer_overflow && c->tirq_enabled)
    {
        if (c->bus.irq_ack)
            c->bus.irq_ack(c->bus.user, 1, c->pc);
        c->last_irq = MCS48_IRQ_TIMER;

        burn_cycles(c, 2);
        c->irq_in_progress = true;

        /* transfer to location 0x07 */
        execute_call(c, 0x07);

        /* timer overflow flip-flop is reset once taken */
        c->timer_overflow = false;
    }
}

int mcs48_step(mcs48_t *c)
{
    const uint64_t start = c->cycles;

    /* check interrupts */
    check_irqs(c);
    c->irq_polled = false;

    c->prevpc = c->pc;
    if (c->bus.instruction_hook)
        c->bus.instruction_hook(c->bus.user, c->pc);

    /* fetch and process opcode */
    unsigned opcode = opcode_fetch(c);
    execute_op(c, opcode);
    c->instructions++;

    return (int)(c->cycles - start);
}

void mcs48_set_int(mcs48_t *c, int line)
{
    c->irq_state = (line != 0);
}

void mcs48_set_ea(mcs48_t *c, int ea)
{
    c->ea = (uint8_t)(ea != 0);
}

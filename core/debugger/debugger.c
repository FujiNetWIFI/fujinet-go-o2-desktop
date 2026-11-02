/*
 * debugger.c -- the o2debug engine (core/include/o2debug.h).
 *
 * Built on the core's two hooks (core/o2/host.h): the instruction hook,
 * fired at every instruction boundary (the 8048's next state is a fetch at
 * cpu.pc, or an interrupt entry followed by the fetch at its vector), and
 * the bus hook, fired on program-data reads, external-data reads and writes
 * and port writes only while a breakpoint of that kind exists. Attached,
 * the instruction hook checks the stop request, the breakpoints and the step
 * mode; stopping PARKS the emulation thread right there (a condition
 * variable) until a window resumes it -- the CPU's own progress is what
 * stops, there is no "paused" flag a run loop polls. Detached, neither hook
 * is installed, so a session that never opens the debugger pays nothing. The
 * design and the feature set (prompt, conditions, breakpoints of every
 * class, scanline and frame steps, symbols, save) are the SMS and Studio II
 * siblings'.
 *
 * What the machine's bus hook does not cover, this engine does itself from
 * the instruction hook, because the 8048 reads these without a bus cycle the
 * machine reports:
 *   - IN A,P1 / IN A,P2 / INS A,BUS (breakpoints "in"): the opcode at the PC
 *     is looked at before it runs, and the breakpoint (with `value` = the
 *     byte now in A) is judged at the next instruction boundary;
 *   - internal-RAM reads and writes: the opcode is decoded for the register
 *     or @R0/@R1 address it touches (and CALL / RET / RETR for the stack),
 *     and the 64 bytes are compared before and after, which also catches the
 *     two stack bytes an interrupt entry pushes. Judged at the next boundary
 *     as well. Only while such a breakpoint exists.
 *
 * Threading. Everything public is called from UI threads; the hooks run on
 * the emulation thread. Every public call that reads or edits the machine
 * holds the host's run lock (o2_host_lock), which the emulation thread
 * holds while it runs a frame and lets go of between frames and while it is
 * parked here -- so a window refreshing live values while the machine runs
 * waits a moment instead of tearing, and a stopped machine is simply the
 * UI's. Lock order: the run lock, then d->lock (the hooks run with the run
 * lock held and take d->lock). The symbol table is only ever touched under
 * the run lock. The 8048 stops between instructions with nothing fetched
 * yet, so a PC change while stopped is just cpu.pc.
 *
 * The FujiNet trace stands apart: the transport feeds it (fujitrace.c) from
 * session start, attached or not, on its own thread and under its own lock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"

#include "dasm48.h"
#include "fuji_mailbox.h"
#include "fujitcp_host.h"
#include "host.h"

#define MAX_BP     128
#define TRACE_N    4096
#define PROG_SIZE  0x1000

enum { MODE_RUN = 0, MODE_STEP, MODE_OVER, MODE_OUT, MODE_RUNTO, MODE_SCANLINE, MODE_FRAME };

static const int space_size[4] = { PROG_SIZE, 0x100, 0x40, 0x100 };
static const char *const space_names[4] = { "prog", "xdata", "iram", "vdc" };

struct o2debug {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    atomic_int attached;
    atomic_int stopped;          /* the emulation thread is parked */
    atomic_int stop_req;
    atomic_int mode;
    long count;                  /* step n / scanline n */
    uint16_t target;             /* run to; step over: the return address */
    int target_sp;               /* step over: the SP it returns to; step out: the SP it leaves */
    int out_ret;                 /* step out: the RET ran, stop at the next boundary */
    int last_vpos;
    uint64_t target_frame;
    int was_attached;            /* across a power cycle */

    char reason[128];
    int reason_addr;
    atomic_uint generation;

    o2debug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t map_exec[PROG_SIZE];     /* nonzero: an enabled exec bp covers it */
    uint8_t map_mem[4][PROG_SIZE];   /* O2DEBUG_BP_READ / _WRITE bits, by space */
    uint8_t map_io[3];               /* O2DEBUG_BP_IN / _OUT bits: 0 BUS, 1 P1, 2 P2 */
    int iram_watch, in_watch;        /* an enabled iram / IN breakpoint exists */
    atomic_int break_pending;
    char break_reason[128];

    /* the access a condition sees as `value` / `addr` */
    long acc_value, acc_addr;
    int in_access;
    atomic_int editing;              /* a debugger edit is writing a port: not an access */

    /* what the instruction about to run will do, judged after it has */
    int armed;
    uint64_t arm_instr;
    int arm_in;                      /* IN port (0 BUS, 1 P1, 2 P2) or -1 */
    int arm_n;                       /* internal-RAM addresses it touches */
    uint8_t arm_addr[2];
    uint8_t arm_rw;                  /* 1 read, 2 write */
    uint8_t iram_prev[64];

    atomic_int trace_on;
    o2debug_trace ring[TRACE_N];
    int ring_head, ring_count;

    symtab syms;
    char cart_path[1024];

    /* the FujiNet trace, fed on the transport's thread */
    pthread_mutex_t fuji_lock;
    fujitrace fuji;
};

static o2debug *g_traced;       /* the engine the transport's tap feeds */

o2_machine_t *o2debug_machine(void)
{
    return o2_host_is_running() ? o2_host_machine() : NULL;
}

static void bump(o2debug *d)
{
    atomic_fetch_add(&d->generation, 1);
}

/* the address of the instruction about to run */
static uint16_t cur_pc(const o2_machine_t *m)
{
    return (uint16_t)(m->cpu.pc & 0xfff);
}

static uint8_t pbyte(o2_machine_t *m, uint16_t a)
{
    return o2_machine_peek(m, O2_SPACE_PROG, (uint16_t)(a & 0xfff));
}

/* the operand byte's address: the PC increments within its 2K half */
static uint16_t next_in_half(uint16_t a)
{
    return (uint16_t)(((a + 1) & 0x7ff) | (a & 0x800));
}

/* ---- expressions ---------------------------------------------------------- */

const char *const o2debug_reg_names[] = {
    "a", "pc", "psw", "sp", "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r0'", "r1'", "r2'", "r3'", "r4'", "r5'", "r6'", "r7'", "t", "cnt", "p1", "p2",
    "bus", "c", "ac", "f0", "f1", "bs", "mb", "vpos", "hpos", "frame", "cycles",
    "value", "addr", NULL,
};

static uint8_t expr_read(void *user, int space, uint16_t addr)
{
    o2_machine_t *m = o2debug_machine();
    (void)user;
    return m ? o2_machine_peek(m, space, addr) : 0xff;
}

static void beam_of(o2_machine_t *m, int *vpos, int *hpos)
{
    i8244_beam(&m->vdc, o2_machine_now(m), vpos, hpos);
}

/* rN (the bank in use) and rN' (bank 1, as the MCS-48 manual writes it; bank 0
 * is i[0]..i[7]) */
static int reg_name(const char *name, int *n, int *other)
{
    if (name[0] != 'r' || name[1] < '0' || name[1] > '7')
        return 0;
    *n = name[1] - '0';
    if (name[2] == '\0')
        *other = 0;
    else if (name[2] == '\'' && name[3] == '\0')
        *other = 1;
    else
        return 0;
    return 1;
}

static int expr_name(void *user, const char *raw, long *out)
{
    o2debug *d = user;
    o2_machine_t *m = o2debug_machine();
    char name[64];
    int n, other;
    size_t i;

    for (i = 0; raw[i] && i + 1 < sizeof name; i++)
        name[i] = (char)tolower((unsigned char)raw[i]);
    name[i] = '\0';

    if (!strcmp(name, "value") && d->in_access) { *out = d->acc_value; return 1; }
    if (!strcmp(name, "addr") && d->in_access) { *out = d->acc_addr; return 1; }
    if (m)
    {
        const mcs48_t *c = &m->cpu;
        if (reg_name(name, &n, &other))
        {
            const int bank = other ? 1 : (c->psw & MCS48_B_FLAG) != 0;
            *out = mcs48_reg_bank(c, bank, n);
            return 1;
        }
        if (!strcmp(name, "a"))   { *out = c->a; return 1; }
        if (!strcmp(name, "pc"))  { *out = cur_pc(m); return 1; }
        if (!strcmp(name, "psw")) { *out = c->psw | 0x08; return 1; }
        if (!strcmp(name, "sp"))  { *out = c->psw & 7; return 1; }
        if (!strcmp(name, "t"))   { *out = c->timer; return 1; }
        if (!strcmp(name, "cnt")) { *out = c->prescaler; return 1; }
        if (!strcmp(name, "p1"))  { *out = c->p1; return 1; }
        if (!strcmp(name, "p2"))  { *out = c->p2; return 1; }
        if (!strcmp(name, "bus")) { *out = c->bus_out; return 1; }
        if (!strcmp(name, "c"))   { *out = (c->psw & MCS48_C_FLAG) != 0; return 1; }
        if (!strcmp(name, "ac"))  { *out = (c->psw & MCS48_A_FLAG) != 0; return 1; }
        if (!strcmp(name, "f0"))  { *out = (c->psw & MCS48_F_FLAG) != 0; return 1; }
        if (!strcmp(name, "f1"))  { *out = c->f1; return 1; }
        if (!strcmp(name, "bs"))  { *out = (c->psw & MCS48_B_FLAG) != 0; return 1; }
        if (!strcmp(name, "mb"))  { *out = c->a11 != 0; return 1; }
        if (!strcmp(name, "vpos") || !strcmp(name, "hpos"))
        {
            int vp, hp;
            beam_of(m, &vp, &hp);
            *out = name[0] == 'v' ? vp : hp;
            return 1;
        }
        if (!strcmp(name, "frame"))  { *out = (long)m->vdc.frames; return 1; }
        if (!strcmp(name, "cycles")) { *out = (long)c->cycles; return 1; }
    }
    {
        int a = symtab_find(&d->syms, raw);
        if (a >= 0)
        {
            *out = a;
            return 1;
        }
    }
    return 0;
}

int o2debug_eval(o2debug *d, const char *text, long *out, char *err, int errsz)
{
    expr_ctx ctx = { expr_name, expr_read, d };
    int rc;
    o2_host_lock();
    rc = expr_eval(text, &ctx, out, err, errsz);
    o2_host_unlock();
    return rc;
}

/* ---- the hooks (emulation thread) ----------------------------------------- */

static void trace_record(o2debug *d, o2_machine_t *m, uint16_t pc)
{
    o2debug_trace *t = &d->ring[d->ring_head];
    const mcs48_t *c = &m->cpu;
    int vp, hp;

    t->pc = pc;
    t->a = c->a;
    t->psw = (uint8_t)(c->psw | 0x08);
    t->sp = (uint8_t)(c->psw & 7);
    t->r0 = mcs48_reg(c, 0);
    t->r1 = mcs48_reg(c, 1);
    t->bytes[0] = pbyte(m, pc);
    t->len = (uint8_t)dasm48_length(t->bytes[0]);
    t->bytes[1] = t->len > 1 ? pbyte(m, next_in_half(pc)) : 0;
    t->cycles = c->cycles;
    beam_of(m, &vp, &hp);
    t->vpos = (int16_t)vp;
    t->hpos = (int16_t)hp;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
}

static int cond_true(o2debug *d, const o2debug_breakpoint *b)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (o2debug_eval(d, b->condition, &v, err, sizeof err) != 0)
        return 1;   /* a condition that no longer evaluates stops, rather than hides */
    return v != 0;
}

/* Park until a window resumes. Called on the emulation thread, with the run
 * lock held and d->lock not; lets go of the run lock while parked. */
static void park(o2debug *d, o2_machine_t *m, uint16_t pc, const char *reason)
{
    /* the picture as far as the beam has drawn it, and the cart's latest */
    o2_host_publish_frame();
    o2_fujinet_frame(m->fujinet);

    pthread_mutex_lock(&d->lock);
    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = pc;
    atomic_store(&d->mode, MODE_RUN);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 1);
    bump(d);
    /* parked, the machine is the windows' */
    o2_host_park_release();
    while (atomic_load(&d->stopped) && atomic_load(&d->attached))
        pthread_cond_wait(&d->cond, &d->lock);
    atomic_store(&d->stopped, 0);
    pthread_mutex_unlock(&d->lock);
    o2_host_park_reacquire();
}

/* One breakpoint check for an access (d->lock and the run lock held). The
 * port breakpoints match `space` as the port (0 BUS, 1 P1, 2 P2) and ignore
 * the address. Returns 1 and arms the stop when one hits. */
static int hit_check(o2debug *d, int type, int space, unsigned addr, unsigned value)
{
    static const char *const port_names[3] = { "bus", "p1", "p2" };
    const int io = (type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
    int hit = 0;

    if (atomic_load(&d->break_pending))
        return 1;
    d->acc_value = (long)value;
    d->acc_addr = (long)addr;
    d->in_access = 1;
    for (int i = 0; i < d->nbp; i++)
    {
        o2debug_breakpoint *b = &d->bp[i];
        if (!b->enabled || !(b->type & type) || b->space != space)
            continue;
        if (!io && (addr < b->start || addr > b->end))
            continue;
        if (!cond_true(d, b))
            continue;
        b->hits++;
        if (io)
            snprintf(d->break_reason, sizeof d->break_reason, "%s %s %s $%02X",
                     type == O2DEBUG_BP_IN ? "IN" : "OUT", port_names[space],
                     type == O2DEBUG_BP_IN ? "->" : "<-", value & 0xff);
        else
            snprintf(d->break_reason, sizeof d->break_reason, "%s %s $%02X = $%02X",
                     type == O2DEBUG_BP_READ ? "read" : "write", space_names[space],
                     addr, value & 0xff);
        atomic_store(&d->break_pending, 1);
        hit = 1;
        break;
    }
    d->in_access = 0;
    return hit;
}

/* The bits of an opcode that touch internal RAM: the register it names or
 * the @R0/@R1 pointer it follows, and the stack for CALL / RET / RETR.
 * Returns 1 read, 2 write, 3 both, and the (up to two) addresses. */
static int iram_access(const mcs48_t *c, uint8_t op, int *n, uint8_t addr[2])
{
    const int sp = c->psw & 7;

    *n = 1;
    switch (op & 0xf8)
    {
    case 0x18: case 0x28: case 0xc8: case 0xe8:    /* inc / xch / dec / djnz Rn */
        addr[0] = (uint8_t)(c->regbase + (op & 7));
        return 3;
    case 0x48: case 0x58: case 0x68: case 0x78: case 0xd8: case 0xf8:  /* orl anl add addc xrl mov a,Rn */
        addr[0] = (uint8_t)(c->regbase + (op & 7));
        return 1;
    case 0xa8: case 0xb8:                           /* mov Rn,a / mov Rn,#imm */
        addr[0] = (uint8_t)(c->regbase + (op & 7));
        return 2;
    default:
        break;
    }
    switch (op & 0xfe)
    {
    case 0x10: case 0x20: case 0x30:                /* inc @Ri / xch a,@Ri / xchd a,@Ri */
        addr[0] = (uint8_t)(c->ram[c->regbase + (op & 1)] & 0x3f);
        return 3;
    case 0x40: case 0x50: case 0x60: case 0x70: case 0xd0: case 0xf0:
        addr[0] = (uint8_t)(c->ram[c->regbase + (op & 1)] & 0x3f);
        return 1;
    case 0xa0: case 0xb0:                           /* mov @Ri,a / mov @Ri,#imm */
        addr[0] = (uint8_t)(c->ram[c->regbase + (op & 1)] & 0x3f);
        return 2;
    default:
        break;
    }
    if ((op & 0x1f) == 0x14)                        /* call: pushes PC and PSW */
    {
        addr[0] = (uint8_t)(8 + 2 * sp);
        addr[1] = (uint8_t)(9 + 2 * sp);
        *n = 2;
        return 2;
    }
    if (op == 0x83 || op == 0x93)                   /* ret / retr: pulls them */
    {
        const int top = (sp - 1) & 7;
        addr[0] = (uint8_t)(8 + 2 * top);
        addr[1] = (uint8_t)(9 + 2 * top);
        *n = 2;
        return 1;
    }
    *n = 0;
    return 0;
}

/* Before an instruction runs: what will be judged once it has. */
static void arm(o2debug *d, o2_machine_t *m, uint16_t pc)
{
    const mcs48_t *c = &m->cpu;
    uint8_t op;

    d->armed = 0;
    if (!d->in_watch && !d->iram_watch)
        return;
    op = pbyte(m, pc);
    d->arm_instr = c->instructions;
    d->arm_in = -1;
    d->arm_n = 0;
    d->arm_rw = 0;
    if (d->in_watch)
    {
        if (op == 0x08) d->arm_in = 0;         /* ins a,bus */
        else if (op == 0x09) d->arm_in = 1;    /* in a,p1 */
        else if (op == 0x0a) d->arm_in = 2;    /* in a,p2 */
    }
    if (d->iram_watch)
    {
        int n;
        memcpy(d->iram_prev, c->ram, sizeof d->iram_prev);
        d->arm_rw = (uint8_t)iram_access(c, op, &n, d->arm_addr);
        d->arm_n = n;
    }
    d->armed = 1;
}

/* After it: an IN judged with A, the internal RAM it touched or changed. */
static void resolve(o2debug *d, o2_machine_t *m)
{
    const mcs48_t *c = &m->cpu;
    /* an interrupt entry runs the vector's instruction instead of the one
     * armed; a held reset or a parked stand-in runs none */
    const int ran = c->instructions == d->arm_instr + 1 && c->last_irq == MCS48_IRQ_NONE;

    d->armed = 0;
    if (atomic_load(&d->break_pending))
        return;
    pthread_mutex_lock(&d->lock);
    if (ran && d->arm_in >= 0)
        hit_check(d, O2DEBUG_BP_IN, d->arm_in, (unsigned)d->arm_in, c->a);
    if (d->iram_watch && !atomic_load(&d->break_pending))
    {
        uint8_t done[64];
        memset(done, 0, sizeof done);
        if (ran)
            for (int i = 0; i < d->arm_n; i++)
            {
                const unsigned a = d->arm_addr[i] & 0x3f;
                if ((d->arm_rw & 1) && hit_check(d, O2DEBUG_BP_READ, O2_SPACE_IRAM, a, c->ram[a]))
                    break;
                if (d->arm_rw & 2)
                {
                    done[a] = 1;
                    if (hit_check(d, O2DEBUG_BP_WRITE, O2_SPACE_IRAM, a, c->ram[a]))
                        break;
                }
            }
        for (unsigned a = 0; a < 64 && !atomic_load(&d->break_pending); a++)
            if (c->ram[a] != d->iram_prev[a] && !done[a])
                hit_check(d, O2DEBUG_BP_WRITE, O2_SPACE_IRAM, a, c->ram[a]);
    }
    pthread_mutex_unlock(&d->lock);
}

/* Does the step mode say stop here? Fills why. */
static int mode_stops(o2debug *d, o2_machine_t *m, uint16_t pc, const char **why)
{
    switch (atomic_load(&d->mode))
    {
    case MODE_STEP:
        if (--d->count <= 0)
        {
            *why = "step";
            return 1;
        }
        break;
    case MODE_OVER:
        if (pc == d->target && (m->cpu.psw & 7) == d->target_sp)
        {
            *why = "step over";
            return 1;
        }
        break;
    case MODE_OUT:
        if (d->out_ret)
        {
            *why = "step out";
            return 1;
        }
        else
        {
            const uint8_t op = pbyte(m, pc);
            if ((op == 0x83 || op == 0x93) && (m->cpu.psw & 7) <= d->target_sp)
                d->out_ret = 1;
        }
        break;
    case MODE_RUNTO:
        if (pc == d->target)
        {
            *why = "run to";
            return 1;
        }
        break;
    case MODE_SCANLINE:
    {
        int vp, hp;
        beam_of(m, &vp, &hp);
        if (vp != d->last_vpos)
        {
            d->last_vpos = vp;
            if (--d->count <= 0)
            {
                *why = "scanline";
                return 1;
            }
        }
        break;
    }
    case MODE_FRAME:
        if (m->vdc.frames >= d->target_frame)
        {
            *why = "frame";
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

static void instr_hook(o2_machine_t *m, void *user)
{
    o2debug *d = user;
    uint16_t pc = cur_pc(m);
    char why[128];
    const char *mode_why = NULL;
    int stop = 0;

    if (d->armed)
        resolve(d, m);
    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, m, pc);

    if (atomic_load(&d->break_pending))
    {
        atomic_store(&d->break_pending, 0);
        pthread_mutex_lock(&d->lock);
        snprintf(why, sizeof why, "%s", d->break_reason);
        pthread_mutex_unlock(&d->lock);
        stop = 1;
    }
    else if (atomic_load(&d->stop_req))
    {
        snprintf(why, sizeof why, "stopped");
        stop = 1;
    }
    else if (d->map_exec[pc])
    {
        pthread_mutex_lock(&d->lock);
        for (int i = 0; i < d->nbp; i++)
        {
            o2debug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & O2DEBUG_BP_EXEC) && pc >= b->start && pc <= b->end &&
                cond_true(d, b))
            {
                b->hits++;
                snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
                stop = 1;
                break;
            }
        }
        pthread_mutex_unlock(&d->lock);
    }
    if (!stop && mode_stops(d, m, pc, &mode_why))
    {
        snprintf(why, sizeof why, "%s", mode_why);
        stop = 1;
    }
    if (stop)
    {
        park(d, m, pc, why);
        /* a poke or a PC change while stopped is simply what the next fetch sees */
        pc = cur_pc(m);
    }
    arm(d, m, pc);
}

static void bus_hook(o2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    o2debug *d = user;

    if (atomic_load(&d->editing) || atomic_load(&d->break_pending))
        return;
    switch (kind)
    {
    case O2_BUS_MEMR:
        if (!(d->map_mem[O2_SPACE_PROG][addr & 0xfff] & O2DEBUG_BP_READ))
            return;
        pthread_mutex_lock(&d->lock);
        hit_check(d, O2DEBUG_BP_READ, O2_SPACE_PROG, addr & 0xfff, data);
        pthread_mutex_unlock(&d->lock);
        break;
    case O2_BUS_XR:
    case O2_BUS_XW:
    {
        const int type = kind == O2_BUS_XR ? O2DEBUG_BP_READ : O2DEBUG_BP_WRITE;
        /* the VDC answers a read with P13 low and P16 low, a write with P13 low */
        const int vdc = kind == O2_BUS_XR ? (m->p1 & 0x48) == 0 : (m->p1 & 0x08) == 0;
        const int a = addr & 0xff;
        const int x = (d->map_mem[O2_SPACE_XDATA][a] & type) != 0;
        const int v = vdc && (d->map_mem[O2_SPACE_VDC][a] & type) != 0;

        if (!x && !v)
            return;
        pthread_mutex_lock(&d->lock);
        if (!(x && hit_check(d, type, O2_SPACE_XDATA, (unsigned)a, data)) && v)
            hit_check(d, type, O2_SPACE_VDC, (unsigned)a, data);
        pthread_mutex_unlock(&d->lock);
        break;
    }
    case O2_BUS_P1W:
    case O2_BUS_P2W:
    case O2_BUS_BUSW:
    {
        const int port = kind == O2_BUS_P1W ? 1 : kind == O2_BUS_P2W ? 2 : 0;
        if (!(d->map_io[port] & O2DEBUG_BP_OUT))
            return;
        pthread_mutex_lock(&d->lock);
        hit_check(d, O2DEBUG_BP_OUT, port, (unsigned)port, data);
        pthread_mutex_unlock(&d->lock);
        break;
    }
    default:
        break;
    }
}

/* What the machine's bus hook must watch, from the enabled breakpoints. */
static uint8_t watch_bits(o2debug *d)
{
    uint8_t w = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        const o2debug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        if (b->type & O2DEBUG_BP_READ)
            w |= b->space == O2_SPACE_PROG ? O2_WATCH_MEMR
               : b->space == O2_SPACE_XDATA || b->space == O2_SPACE_VDC ? O2_WATCH_XR : 0;
        if (b->type & O2DEBUG_BP_WRITE)
            w |= b->space == O2_SPACE_XDATA || b->space == O2_SPACE_VDC ? O2_WATCH_XW : 0;
        if (b->type & O2DEBUG_BP_OUT)
            w |= b->space == 1 ? O2_WATCH_P1 : b->space == 2 ? O2_WATCH_P2 : O2_WATCH_BUS;
    }
    return w;
}

/* Rebuild the lookup maps and the bus watch. Called with the run lock and
 * d->lock held (the hooks read the maps under the run lock). */
static void rebuild_locked(o2debug *d)
{
    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    memset(d->map_io, 0, sizeof d->map_io);
    d->iram_watch = d->in_watch = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        const o2debug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        if (b->type & O2DEBUG_BP_EXEC)
            for (unsigned a = b->start; a <= b->end && a < PROG_SIZE; a++)
                d->map_exec[a] = 1;
        if (b->type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE))
        {
            const int sp = b->space >= 0 && b->space < 4 ? b->space : 0;
            for (unsigned a = b->start; a <= b->end && a < (unsigned)space_size[sp]; a++)
                d->map_mem[sp][a] |= (uint8_t)(b->type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE));
            if (sp == O2_SPACE_IRAM)
                d->iram_watch = 1;
        }
        if (b->type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT))
        {
            const int port = b->space >= 0 && b->space < 3 ? b->space : 0;
            d->map_io[port] |= (uint8_t)(b->type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT));
            if (b->type & O2DEBUG_BP_IN)
                d->in_watch = 1;
        }
    }
    d->armed = 0;
    if (atomic_load(&d->attached) && o2_host_is_running())
        o2_host_set_bus_hook(bus_hook, d, watch_bits(d));
}

/* ---- lifecycle -------------------------------------------------------------- */

o2debug *o2debug_create(void)
{
    o2debug *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    pthread_mutex_init(&d->fuji_lock, NULL);
    fujitrace_init(&d->fuji);
    d->next_id = 1;
    d->reason_addr = -1;
    symtab_init(&d->syms);
    return d;
}

void o2debug_free(o2debug *d)
{
    if (!d)
        return;
    if (g_traced == d)
    {
        fujitcp_set_trace(NULL, NULL);
        g_traced = NULL;
    }
    o2debug_detach(d);
    symtab_free(&d->syms);
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->cond);
    pthread_mutex_destroy(&d->fuji_lock);
    free(d);
}

/* the transport's thread: the cartridge's worker, or the emulation thread
 * in sync mode */
static void fuji_tap(void *user, int dir, const uint8_t *buf, size_t len)
{
    o2debug *d = user;
    pthread_mutex_lock(&d->fuji_lock);
    fujitrace_feed(&d->fuji, dir, buf, len);
    pthread_mutex_unlock(&d->fuji_lock);
}

void o2debug_fujinet_install(o2debug *d)
{
    if (!d)
        return;
    g_traced = d;
    fujitcp_set_trace(fuji_tap, d);
}

static void hook(o2debug *d)
{
    if (!o2_host_is_running())
        return;
    o2_host_set_bus_hook(bus_hook, d, watch_bits(d));
    o2_host_set_instr_hook(instr_hook, d);
}

static void unhook(void)
{
    if (!o2_host_is_running())
        return;
    o2_host_set_instr_hook(NULL, NULL);
    o2_host_set_bus_hook(NULL, NULL, 0);
}

void o2debug_attach(o2debug *d)
{
    if (!d)
        return;
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    if (!atomic_load(&d->attached))
    {
        atomic_store(&d->attached, 1);
        atomic_store(&d->stop_req, 1);
        atomic_store(&d->mode, MODE_RUN);
        atomic_store(&d->break_pending, 0);
        d->armed = 0;
        hook(d);
    }
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
}

void o2debug_detach(o2debug *d)
{
    if (!d)
        return;
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->attached, 0);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->mode, MODE_RUN);
    unhook();
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
    /* wait for the parked thread to leave the hook */
    for (int i = 0; i < 2000 && atomic_load(&d->stopped); i++)
    {
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
}

int o2debug_is_attached(o2debug *d)
{
    return d && atomic_load(&d->attached);
}

void o2debug_power_down(o2debug *d)
{
    if (!d)
        return;
    d->was_attached = atomic_load(&d->attached);
    o2debug_detach(d);
}

void o2debug_power_up(o2debug *d)
{
    int want;

    if (!d)
        return;
    /* attached across the power cycle, or attached before there was a
     * machine to hook (a window opened before the session started) */
    want = d->was_attached || atomic_load(&d->attached);
    atomic_store(&d->attached, 0);
    if (want)
        o2debug_attach(d);
    d->was_attached = 0;
}

void o2debug_set_cart_path(o2debug *d, const char *path)
{
    if (d)
        snprintf(d->cart_path, sizeof d->cart_path, "%s", path ? path : "");
}

symtab *o2debug_symbols(o2debug *d)
{
    return &d->syms;
}

int o2debug_is_stopped(o2debug *d)
{
    return d && atomic_load(&d->stopped);
}

void o2debug_stop(o2debug *d)
{
    if (!d)
        return;
    if (!atomic_load(&d->attached))
        o2debug_attach(d);
    atomic_store(&d->stop_req, 1);
}

/* Leave the parked state with a mode. */
static void go(o2debug *d, int mode)
{
    pthread_mutex_lock(&d->lock);
    atomic_store(&d->mode, mode);
    atomic_store(&d->stop_req, 0);
    atomic_store(&d->stopped, 0);
    pthread_cond_broadcast(&d->cond);
    bump(d);
    pthread_mutex_unlock(&d->lock);
}

void o2debug_resume(o2debug *d)
{
    if (d && atomic_load(&d->stopped))
        go(d, MODE_RUN);
}

int o2debug_stop_reason(o2debug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = snprintf(dst, (size_t)dstsz, "%s", atomic_load(&d->stopped) ? d->reason : "running");
    if (address)
        *address = atomic_load(&d->stopped) ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->lock);
    return n;
}

unsigned o2debug_generation(o2debug *d)
{
    return d ? atomic_load(&d->generation) : 0;
}

/* ---- stepping ---------------------------------------------------------------- */

void o2debug_step_n(o2debug *d, int n)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->count = n > 0 ? n : 1;
    go(d, MODE_STEP);
}

void o2debug_step(o2debug *d)
{
    o2debug_step_n(d, 1);
}

void o2debug_step_over(o2debug *d)
{
    o2_machine_t *m;
    int over = 0;

    if (!d)
        return;
    o2_host_lock();
    m = o2debug_machine();
    if (m && atomic_load(&d->stopped))
    {
        const uint16_t pc = cur_pc(m);
        uint8_t ob[2];
        uint32_t fl = 0;
        int len;

        ob[0] = pbyte(m, pc);
        ob[1] = pbyte(m, next_in_half(pc));
        len = dasm48(NULL, 0, ob, pc, &fl);
        /* a CALL (and, as in MAME, DJNZ) comes back to the instruction after
         * it, with the stack where it was */
        if (fl & DASM48_STEP_OVER)
        {
            d->target = len > 1 ? next_in_half(next_in_half(pc)) : next_in_half(pc);
            d->target_sp = m->cpu.psw & 7;
            over = 1;
        }
    }
    o2_host_unlock();
    if (over)
        go(d, MODE_OVER);
    else
        o2debug_step(d);
}

void o2debug_step_out(o2debug *d)
{
    o2_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    o2_host_lock();
    m = o2debug_machine();
    if (m && atomic_load(&d->stopped))
    {
        /* the first RET or RETR to run at this stack depth or shallower ends
         * the step; anything deeper is a call (or an interrupt) of its own */
        d->target_sp = m->cpu.psw & 7;
        d->out_ret = 0;
        ok = 1;
    }
    o2_host_unlock();
    if (ok)
        go(d, MODE_OUT);
}

void o2debug_scanline(o2debug *d, int n)
{
    o2_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    o2_host_lock();
    m = o2debug_machine();
    if (m && atomic_load(&d->stopped))
    {
        int hp;
        beam_of(m, &d->last_vpos, &hp);
        d->count = n > 0 ? n : 1;
        ok = 1;
    }
    o2_host_unlock();
    if (ok)
        go(d, MODE_SCANLINE);
}

void o2debug_frame(o2debug *d, int n)
{
    o2_machine_t *m;
    int ok = 0;

    if (!d)
        return;
    o2_host_lock();
    m = o2debug_machine();
    if (m && atomic_load(&d->stopped))
    {
        d->target_frame = m->vdc.frames + (uint64_t)(n > 0 ? n : 1);
        ok = 1;
    }
    o2_host_unlock();
    if (ok)
        go(d, MODE_FRAME);
}

void o2debug_run_to(o2debug *d, uint16_t addr)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->target = (uint16_t)(addr & 0xfff);
    go(d, MODE_RUNTO);
}

/* ---- CPU ---------------------------------------------------------------------- */

static void cpu_get_locked(o2_machine_t *m, o2debug_cpu *o)
{
    const mcs48_t *c;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    c = &m->cpu;
    o->a = c->a;
    o->pc = cur_pc(m);
    o->mb = c->a11 != 0;
    o->psw = c->psw | 0x08;
    o->cy = (c->psw & MCS48_C_FLAG) != 0;
    o->ac = (c->psw & MCS48_A_FLAG) != 0;
    o->f0 = (c->psw & MCS48_F_FLAG) != 0;
    o->f1 = c->f1;
    o->bs = (c->psw & MCS48_B_FLAG) != 0;
    o->sp = c->psw & 7;
    memcpy(o->iram, c->ram, sizeof o->iram);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 8; i++)
            o->r[b][i] = mcs48_reg_bank(c, b, i);
    o->timer = c->timer;
    o->prescaler = c->prescaler;
    o->timer_mode = (c->timecount_enabled & MCS48_TIMER_ENABLED) ? 1
                  : (c->timecount_enabled & MCS48_COUNTER_ENABLED) ? 2 : 0;
    o->tf = c->timer_flag;
    o->tcnti_en = c->tirq_enabled;
    o->ext_int_en = c->xirq_enabled;
    o->in_interrupt = c->irq_in_progress;
    /* will be taken at the next instruction boundary */
    o->irq_pending = c->irq_state && c->xirq_enabled && !c->irq_in_progress;
    o->timer_irq_pending = c->timer_overflow && c->tirq_enabled && !c->irq_in_progress;
    o->t0 = o2_cart_t0(m->cart);
    o->t1 = i8244_t1(&m->vdc, o2_machine_now(m));
    o->int_line = i8244_irq(&m->vdc);
    o->p1 = c->p1;
    o->p2 = c->p2;
    o->bus = c->bus_out;
    o->cycles = c->cycles;
    o->instructions = c->instructions;
    beam_of(m, &o->vpos, &o->hpos);
    o->frame = (uint32_t)m->vdc.frames;
    o->machine = m->machine;
}

void o2debug_cpu_get(o2debug *d, o2debug_cpu *o)
{
    (void)d;
    o2_host_lock();
    cpu_get_locked(o2debug_machine(), o);
    o2_host_unlock();
}

/* A debugger store to a port, through the machine's own latch handling (the
 * cartridge hears P1 and P2; a bus hook is not an access). */
static void write_port(o2debug *d, o2_machine_t *m, int port, uint8_t v)
{
    mcs48_t *c = &m->cpu;

    atomic_store(&d->editing, 1);
    if (port == 1 || port == 2)
    {
        if (port == 1)
            c->p1 = v;
        else
            c->p2 = v;
        if (c->bus.port_write)
            c->bus.port_write(c->bus.user, port, v);
    }
    else
    {
        c->bus_out = v;
        if (c->bus.bus_write)
            c->bus.bus_write(c->bus.user, v);
    }
    atomic_store(&d->editing, 0);
}

void o2debug_cpu_set(o2debug *d, int reg, int v)
{
    o2_machine_t *m;
    mcs48_t *c;

    if (!d)
        return;
    o2_host_lock();
    m = o2debug_machine();
    if (!m || !atomic_load(&d->stopped))
    {
        o2_host_unlock();
        return;
    }
    c = &m->cpu;
    if (reg >= O2_REG_R0 && reg < O2_REG_R0 + 8)
        c->ram[c->regbase + (reg - O2_REG_R0)] = (uint8_t)v;
    else if (reg >= O2_REG_R0_B1 && reg < O2_REG_R0_B1 + 8)
        c->ram[24 + (reg - O2_REG_R0_B1)] = (uint8_t)v;
    else
    {
        switch (reg)
        {
        case O2_REG_A:   c->a = (uint8_t)v; break;
        case O2_REG_PC:  c->pc = (uint16_t)(v & 0xfff); break;
        case O2_REG_PSW:
            c->psw = (uint8_t)(v & 0xf7);          /* bit 3 is never stored */
            c->regbase = (c->psw & MCS48_B_FLAG) ? 24 : 0;
            break;
        case O2_REG_SP:  c->psw = (uint8_t)((c->psw & 0xf8) | (v & 7)); break;
        case O2_REG_T:   c->timer = (uint8_t)v; break;
        case O2_REG_CNT: c->prescaler = (uint8_t)(v & 0x1f); break;
        case O2_REG_P1:  write_port(d, m, 1, (uint8_t)v); break;
        case O2_REG_P2:  write_port(d, m, 2, (uint8_t)v); break;
        case O2_REG_BUS: write_port(d, m, 0, (uint8_t)v); break;
        case O2_REG_C:
            c->psw = (uint8_t)(v ? c->psw | MCS48_C_FLAG : c->psw & ~MCS48_C_FLAG);
            break;
        case O2_REG_AC:
            c->psw = (uint8_t)(v ? c->psw | MCS48_A_FLAG : c->psw & ~MCS48_A_FLAG);
            break;
        case O2_REG_F0:
            c->psw = (uint8_t)(v ? c->psw | MCS48_F_FLAG : c->psw & ~MCS48_F_FLAG);
            break;
        case O2_REG_F1:  c->f1 = v != 0; break;
        case O2_REG_BS:
            c->psw = (uint8_t)(v ? c->psw | MCS48_B_FLAG : c->psw & ~MCS48_B_FLAG);
            c->regbase = (c->psw & MCS48_B_FLAG) ? 24 : 0;
            break;
        case O2_REG_MB:  c->a11 = v ? 0x800 : 0; break;
        default: break;
        }
    }
    pthread_mutex_lock(&d->lock);
    d->reason_addr = cur_pc(m);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
    bump(d);
}

/* ---- sound and I/O -------------------------------------------------------------- */

void o2debug_io_get(o2debug *d, o2debug_io *o)
{
    o2_machine_t *m;

    (void)d;
    memset(o, 0, sizeof *o);
    o2_host_lock();
    m = o2debug_machine();
    if (m)
    {
        o->machine = m->machine;
        o->have_bios = m->have_bios;
        o->have_font = m->vdc.font != NULL;
        o->stub_running = !m->have_bios;
        o->missing = (int)o2_machine_missing(m);
        o->p1 = m->p1;
        o->p2 = m->p2;
        o->cart_bank = m->p1 & 3;
        o->kbd_enable = !(m->p1 & 0x04);
        o->vdc_select = !(m->p1 & 0x08);
        o->ram_select = !(m->p1 & 0x10);
        o->ef_select = !(m->p1 & 0x20);
        o->copy_mode = (m->p1 & 0x40) != 0;
        o->lum = !(m->p1 & 0x80);
        o->kbd_row = m->p2 & 7;
        o->keys = m->keys;
        o->sticks[0] = m->joy[0];
        o->sticks[1] = m->joy[1];
        o->reset_held = m->reset_held;
        o->voice_present = m->have_voice;
        if (m->have_voice)
            sp0256_debug_get(&m->speech, &o->voice);
        o->t0 = o2_cart_t0(m->cart);
        o->audio_samples = m->audio_samples;
    }
    o2_host_unlock();
}

/* ---- memory ------------------------------------------------------------------- */

int o2debug_read(o2debug *d, int space, uint16_t addr, uint8_t *dst, int n)
{
    o2_machine_t *m;
    (void)d;
    o2_host_lock();
    m = o2debug_machine();
    if (m)
        for (int i = 0; i < n; i++)
            dst[i] = o2_machine_peek(m, space, (uint16_t)(addr + i));
    o2_host_unlock();
    return m ? n : 0;
}

void o2debug_write(o2debug *d, int space, uint16_t addr, uint8_t value)
{
    o2_machine_t *m;
    o2_host_lock();
    m = o2debug_machine();
    if (m)
        o2_machine_poke(m, space, addr, value);
    o2_host_unlock();
    if (m)
        bump(d);
}

/* ---- labels ---------------------------------------------------------------------- */

int o2debug_label_address(o2debug *d, const char *label)
{
    int a;
    if (!d)
        return -1;
    o2_host_lock();
    a = symtab_find(&d->syms, label);
    o2_host_unlock();
    return a;
}

int o2debug_address_label(o2debug *d, uint16_t addr, char *dst, int dstsz)
{
    const char *l;
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    o2_host_lock();
    l = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    n = snprintf(dst, (size_t)dstsz, "%s", l ? l : "");
    o2_host_unlock();
    return n;
}

int o2debug_set_label(o2debug *d, uint16_t addr, const char *label)
{
    if (!d)
        return -1;
    o2_host_lock();
    symtab_remove_at(&d->syms, addr);
    if (label && *label)
        symtab_add(&d->syms, addr, SYM_BANK_ANY, label, 0);
    o2_host_unlock();
    bump(d);
    return 0;
}

int o2debug_load_symbols(o2debug *d, const char *path, char *msg, int msgsz)
{
    int n;
    if (!d)
        return -1;
    if (!path || !*path)
    {
        static const char *const exts[] = { ".map", ".lst", ".sym", NULL };
        char cand[1100];
        const char *dot;
        size_t stem;

        if (!d->cart_path[0])
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "No cartridge file is open; choose a symbol file.");
            return -1;
        }
        dot = strrchr(d->cart_path, '.');
        stem = dot ? (size_t)(dot - d->cart_path) : strlen(d->cart_path);
        for (int i = 0; exts[i]; i++)
        {
            FILE *f;
            snprintf(cand, sizeof cand, "%.*s%s", (int)stem, d->cart_path, exts[i]);
            f = fopen(cand, "r");
            if (f)
            {
                fclose(f);
                o2_host_lock();
                n = symtab_load(&d->syms, cand, msg, msgsz);
                o2_host_unlock();
                bump(d);
                return n;
            }
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "No .map, .lst or .sym next to %s", d->cart_path);
        return -1;
    }
    o2_host_lock();
    n = symtab_load(&d->syms, path, msg, msgsz);
    o2_host_unlock();
    bump(d);
    return n;
}

/* ---- disassembly ------------------------------------------------------------------ */

/* Decode one instruction; *target is the 12-bit place a jump or call goes to
 * (-1 for any other), resolved against the MB latch when the line is the PC
 * line, else against a label in either half, else the half the line is in. */
static int decode_at(o2debug *d, o2_machine_t *m, uint16_t addr, uint16_t pc, char *text,
                     size_t textsz, uint32_t *flags, int *target, int *page_form)
{
    uint8_t ob[2];
    int len, t;

    ob[0] = pbyte(m, addr);
    ob[1] = pbyte(m, next_in_half(addr));
    len = dasm48(text, textsz, ob, addr, flags);
    t = dasm48_target(ob, addr);
    *page_form = 0;
    if (t >= 0 && ((ob[0] & 0x1f) == 0x04 || (ob[0] & 0x1f) == 0x14))
    {
        /* JMP / CALL carry 11 bits; bit 11 is the MB latch, which a listing
         * can only guess */
        const int lo = t & 0x7ff;
        *page_form = 1;
        if (addr == pc)
            t = lo | (m->cpu.irq_in_progress ? 0 : m->cpu.a11);
        else if (symtab_lookup(&d->syms, (uint16_t)lo, SYM_BANK_ANY))
            t = lo;
        else if (symtab_lookup(&d->syms, (uint16_t)(lo | 0x800), SYM_BANK_ANY))
            t = lo | 0x800;
        else
            t = lo | (addr & 0x800);
    }
    *target = t;
    return len;
}

static void fill_line(o2debug *d, o2_machine_t *m, uint16_t addr, o2debug_line *l, uint16_t pc)
{
    uint32_t flags;
    int target, page_form;
    const char *lab;

    memset(l, 0, sizeof *l);
    addr &= 0xfff;
    l->length = decode_at(d, m, addr, pc, l->disasm, sizeof l->disasm, &flags, &target, &page_form);
    l->address = addr;
    l->is_pc = addr == pc;
    l->has_breakpoint = d->map_exec[addr] != 0;
    for (int i = 0, o = 0; i < l->length && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X", i ? " " : "",
                      pbyte(m, (uint16_t)(addr + i)));
    lab = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    if (lab)
        snprintf(l->label, sizeof l->label, "%s", lab);
    if (target >= 0)
    {
        const char *t = symtab_lookup(&d->syms, (uint16_t)target, SYM_BANK_ANY);
        if (t)
            snprintf(l->comment, sizeof l->comment, "%s", t);
        else if (page_form && (target & 0x800))
            snprintf(l->comment, sizeof l->comment, "$%03X", target);
    }
}

int o2debug_disassemble(o2debug *d, uint16_t addr, o2debug_line *out, int max, int *pc_line)
{
    o2_machine_t *m;
    uint16_t pc;
    int n = 0;

    if (pc_line)
        *pc_line = -1;
    if (!d)
        return 0;
    o2_host_lock();
    m = o2debug_machine();
    if (m)
    {
        pc = cur_pc(m);
        addr &= 0xfff;
        while (n < max)
        {
            fill_line(d, m, addr, &out[n], pc);
            if (out[n].is_pc && pc_line)
                *pc_line = n;
            addr = (uint16_t)((addr + out[n].length) & 0xfff);
            n++;
        }
    }
    o2_host_unlock();
    return n;
}

static int insn_len(o2_machine_t *m, uint16_t addr)
{
    return dasm48_length(pbyte(m, addr));
}

static int row_address_locked(o2_machine_t *m, uint16_t addr, int rows)
{
    addr &= 0xfff;
    if (!m || rows == 0)
        return addr;
    if (rows > 0)
    {
        for (int i = 0; i < rows; i++)
            addr = (uint16_t)((addr + insn_len(m, addr)) & 0xfff);
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on
     * addr, from as far back as the rows could take. Instructions are one
     * or two bytes, so `rows` rows are at most 2 * rows back. */
    rows = -rows;
    for (int back = rows * 2; back >= 1; back--)
    {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)((addr - back) & 0xfff);
        while (count < 256)
        {
            const int dist = (addr - a) & 0xfff;
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)((a + insn_len(m, a)) & 0xfff);
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (addr - rows) & 0xfff;
}

int o2debug_row_address(o2debug *d, uint16_t addr, int rows)
{
    int a;
    (void)d;
    o2_host_lock();
    a = row_address_locked(o2debug_machine(), addr, rows);
    o2_host_unlock();
    return a;
}

/* ---- the cartridge ----------------------------------------------------------------- */

static void cart_get_locked(o2_machine_t *m, o2debug_cart *o)
{
    const o2_cart_t *c, *inner;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    c = m->cart;
    o->present = c != NULL;
    if (!c)
    {
        o->type_name = o2_cart_type_name(O2_CART_NONE);
        return;
    }
    inner = c->type == O2_CART_VOICE && c->sub ? c->sub : c;
    o->voice_in_front = m->have_voice || c->type == O2_CART_VOICE;
    o->type = inner->type;
    o->type_name = o2_cart_type_name(inner->type);
    o->rom_size = inner->rom_size;
    o->bank = inner->bank;
    o->control = inner->control;
    if (m->fujinet)
    {
        o2_cart_status_t st;
        o2_fujinet_status(m->fujinet, &st);
        o->type = O2_CART_FUJINET;
        o->type_name = o2_cart_type_name(O2_CART_FUJINET);
        o->link_up = st.link_up;
        o->link_off = st.link_off;
        o->mailbox = st.mailbox;
        o->app_live = st.app_live;
        o->booted_game = st.booted_game;
        o->armed = st.armed;
        o->staged_app = st.staged_app;
        o->staged_mailbox = st.staged_mailbox;
        o->live_size = st.live_size;
        o->live_crc = st.live_crc;
        o->staged_size = st.staged_size;
        o->staged_crc = st.staged_crc;
        o->swaps = (int)st.swaps;
        o->nbanks = (int)st.nbanks;
        o->fbank = (int)st.bank;
        o->bank = (int)st.bank;
        o->rom_size = st.live_size;
        o->boot_state = st.boot_state;
        o->boot_pct = st.boot_pct;
        o->boot_err = st.boot_err;
        o->ackseq = st.ackseq;
        o->status = st.status;
        o->last_error = st.err;
        o->reply_cmd = st.reply_cmd;
        o->rxlen = st.rxlen;
        o->queue_depth = st.queue;
        o2_fujinet_mailbox(m->fujinet, o->mailbox_page);
    }
    else
        for (unsigned i = 0; i < sizeof o->mailbox_page; i++)
            o->mailbox_page[i] = pbyte(m, (uint16_t)(0xf20 + i));
}

void o2debug_cart_get(o2debug *d, o2debug_cart *o)
{
    (void)d;
    o2_host_lock();
    cart_get_locked(o2debug_machine(), o);
    o2_host_unlock();
}

int o2debug_cart_info(o2debug *d, char *dst, int dstsz)
{
    o2debug_cart c;
    int n;

    o2debug_cart_get(d, &c);
    if (!c.present)
        return snprintf(dst, (size_t)dstsz, "No cartridge");
    if (c.type == O2_CART_FUJINET)
    {
        const char *what = !c.booted_game ? "CONFIG" : c.mailbox ? "app (mailbox live)" : "game image";
        n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s", what,
                     c.link_off ? "off" : c.link_up ? "up" : "down");
        if (c.live_size)
            n += snprintf(dst + n, n < dstsz ? (size_t)(dstsz - n) : 0, ", %u bytes", c.live_size);
        return n < dstsz ? n : dstsz - 1;
    }
    n = snprintf(dst, (size_t)dstsz, "%s cartridge, %u bytes%s", c.type_name, c.rom_size,
                 c.voice_in_front ? ", behind The Voice" : "");
    return n < dstsz ? n : dstsz - 1;
}

/* ---- FujiNet --------------------------------------------------------------------- */

int o2debug_fujinet_trace(o2debug *d, o2debug_fuji_packet *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->fuji_lock);
    n = fujitrace_read(&d->fuji, out, max);
    pthread_mutex_unlock(&d->fuji_lock);
    return n;
}

void o2debug_fujinet_trace_clear(o2debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->fuji_lock);
    fujitrace_clear(&d->fuji);
    pthread_mutex_unlock(&d->fuji_lock);
}

int o2debug_fujinet_state(o2debug *d, char *dst, int dstsz)
{
    o2debug_cart c;
    int n;

    o2debug_cart_get(d, &c);
    if (c.type != O2_CART_FUJINET)
        return snprintf(dst, (size_t)dstsz, "The cartridge in the slot is not the FujiNet cartridge.\n");
    n = snprintf(dst, (size_t)dstsz,
        "Link         %s\n"
        "Image        %s%s, %u bytes, CRC %08X\n"
        "\n"
        "Mailbox\n"
        "  ACKSEQ     $%02X\n"
        "  STATUS     $%02X  (link %d, busy %d)\n"
        "  last error %u\n"
        "  reply cmd  $%02X\n"
        "  RXLEN      %u\n"
        "  queue      %u\n"
        "\n"
        "Boot         state %d  %d%%\n"
        "             error %d\n"
        "Staged       %s%s  %u bytes, CRC %08X\n"
        "Swaps        %d\n",
        c.link_off ? "off (FujiNet is switched off)" : c.link_up ? "up" : "down",
        !c.booted_game ? "CONFIG" : c.mailbox ? "an app" : "a game", c.app_live ? " (APPBANK)" : "",
        c.live_size, c.live_crc,
        c.ackseq, c.status, c.status & 1, (c.status >> 1) & 1, c.last_error, c.reply_cmd,
        c.rxlen, c.queue_depth,
        c.boot_state, c.boot_pct, c.boot_err,
        c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
        c.staged_size && c.armed ? ", armed" : "", c.staged_size, c.staged_crc, c.swaps);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- breakpoints ---------------------------------------------------------------- */

int o2debug_breakpoint_add(o2debug *d, int type, int space, uint16_t start, uint16_t end,
                           const char *cond)
{
    o2debug_breakpoint *b;
    char err[96];
    int id;

    if (!d || !(type & (O2DEBUG_BP_EXEC | O2DEBUG_BP_READ | O2DEBUG_BP_WRITE |
                        O2DEBUG_BP_IN | O2DEBUG_BP_OUT)))
        return -1;
    if (cond && *cond && expr_eval(cond, NULL, NULL, err, sizeof err) != 0)
        return -1;
    if (type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT))
    {
        if (space < 0 || space > 2)
            return -1;
        start = end = 0;
    }
    else if (type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE))
    {
        if (space < 0 || space > 3)
            return -1;
    }
    else
        space = O2_SPACE_PROG;
    if (end < start)
    {
        uint16_t t = start;
        start = end;
        end = t;
    }
    {
        const int top = (type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) ? 0 : space_size[space] - 1;
        if (type & O2DEBUG_BP_EXEC)
        {
            if (start >= PROG_SIZE)
                return -1;
            if (end >= PROG_SIZE)
                end = PROG_SIZE - 1;
        }
        else if (!(type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)))
        {
            if (start > top)
                return -1;
            if (end > top)
                end = (uint16_t)top;
        }
    }
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    if (d->nbp >= MAX_BP)
    {
        pthread_mutex_unlock(&d->lock);
        o2_host_unlock();
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->space = space;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
    return id;
}

void o2debug_breakpoint_remove(o2debug *d, int id)
{
    if (!d)
        return;
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
    {
        if (d->bp[i].id == id)
        {
            memmove(&d->bp[i], &d->bp[i + 1], (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
}

void o2debug_breakpoint_enable(o2debug *d, int id, int enabled)
{
    if (!d)
        return;
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
}

int o2debug_breakpoint_list(o2debug *d, o2debug_breakpoint *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->lock);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->lock);
    return n;
}

void o2debug_breakpoint_clear(o2debug *d)
{
    if (!d)
        return;
    o2_host_lock();
    pthread_mutex_lock(&d->lock);
    d->nbp = 0;
    rebuild_locked(d);
    bump(d);
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
}

int o2debug_breakpoint_check(o2debug *d, uint16_t addr)
{
    return d && d->map_exec[addr & 0xfff] != 0;
}

int o2debug_breakpoint_toggle(o2debug *d, uint16_t addr)
{
    int found = -1;
    if (!d)
        return 0;
    addr &= 0xfff;
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->nbp; i++)
        if (d->bp[i].type == O2DEBUG_BP_EXEC && d->bp[i].start == addr && d->bp[i].end == addr)
        {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->lock);
    if (found >= 0)
    {
        o2debug_breakpoint_remove(d, found);
        return 0;
    }
    return o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, addr, addr, NULL) >= 0;
}

/* ---- trace ---------------------------------------------------------------------- */

void o2debug_trace_enable(o2debug *d, int on)
{
    if (d)
    {
        atomic_store(&d->trace_on, on != 0);
        bump(d);
    }
}

int o2debug_trace_enabled(o2debug *d)
{
    return d && atomic_load(&d->trace_on);
}

/* The ring is written by the instruction hook, under the run lock. */
int o2debug_trace_read(o2debug *d, o2debug_trace *out, int max)
{
    int n;
    if (!d)
        return 0;
    o2_host_lock();
    n = d->ring_count < max ? d->ring_count : max;
    for (int i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    o2_host_unlock();
    return n;
}

void o2debug_trace_clear(o2debug *d)
{
    if (!d)
        return;
    o2_host_lock();
    d->ring_head = d->ring_count = 0;
    o2_host_unlock();
    bump(d);
}

/* ---- files ------------------------------------------------------------------------ */

static int fail_msg(char *msg, int msgsz, const char *fmt, const char *arg)
{
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, fmt, arg ? arg : "");
    return -1;
}

static int write_bytes(const char *path, const uint8_t *data, size_t n, char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        if (f)
            fclose(f);
        return fail_msg(msg, msgsz, "Cannot write %s", path);
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static int save_locked(o2debug *d, o2_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz)
{
    if (!d || !m || !kind || !path)
        return fail_msg(msg, msgsz, "The machine is not running.%s", "");
    if (!strcmp(kind, "ram"))
    {
        uint8_t buf[64 + O2_RAM_SIZE];
        memcpy(buf, m->cpu.ram, 64);
        memcpy(buf + 64, m->ram, O2_RAM_SIZE);
        return write_bytes(path, buf, sizeof buf, msg, msgsz);
    }
    if (!strcmp(kind, "mailbox"))
    {
        o2debug_cart c;
        cart_get_locked(m, &c);
        return write_bytes(path, c.mailbox_page, sizeof c.mailbox_page, msg, msgsz);
    }
    if (!strcmp(kind, "vdc"))
        return write_bytes(path, m->vdc.reg, sizeof m->vdc.reg, msg, msgsz);
    if (!strcmp(kind, "font"))
    {
        if (!m->vdc.font)
            return fail_msg(msg, msgsz, "No character set is imported (the glyphs are blank).%s", "");
        return write_bytes(path, m->vdc.font, 512, msg, msgsz);
    }
    if (!strcmp(kind, "screen"))
    {
        FILE *f = fopen(path, "wb");
        if (!f)
            return fail_msg(msg, msgsz, "Cannot write %s", path);
        fprintf(f, "P6\n%d %d\n255\n", O2_FB_WIDTH, O2_FB_HEIGHT);
        for (int i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++)
        {
            uint8_t px[3] = { (uint8_t)(m->fb[i] >> 16), (uint8_t)(m->fb[i] >> 8), (uint8_t)m->fb[i] };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (!strcmp(kind, "dis") || !strcmp(kind, "regs"))
    {
        FILE *f = fopen(path, "w");
        if (!f)
            return fail_msg(msg, msgsz, "Cannot write %s", path);
        if (!strcmp(kind, "dis"))
        {
            const uint16_t pc = cur_pc(m);
            unsigned a = 0;
            while (a < PROG_SIZE)
            {
                o2debug_line l;
                fill_line(d, m, (uint16_t)a, &l, pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %03X  %-6s %-20s%s%s\n", l.address, l.bytes, l.disasm,
                        l.comment[0] ? "; " : "", l.comment);
                a += (unsigned)l.length;
            }
        }
        else
        {
            o2debug_cpu c;
            o2debug_io io;
            i8244_snapshot v;

            cpu_get_locked(m, &c);
            i8244_snapshot_get(&m->vdc, o2_machine_now(m), &v);
            fprintf(f, "A=%02X PC=%03X MB=%d PSW=%02X (C=%d AC=%d F0=%d F1=%d BS=%d SP=%d)\n", c.a,
                    c.pc, c.mb, c.psw, c.cy, c.ac, c.f0, c.f1, c.bs, c.sp);
            for (int b = 0; b < 2; b++)
            {
                fprintf(f, "bank %d:", b);
                for (int i = 0; i < 8; i++)
                    fprintf(f, " R%d=%02X", i, c.r[b][i]);
                fprintf(f, "\n");
            }
            fprintf(f, "T=%02X prescaler=%02X mode=%d TF=%d TCNTI=%d EN-I=%d in-ISR=%d\n", c.timer,
                    c.prescaler, c.timer_mode, c.tf, c.tcnti_en, c.ext_int_en, c.in_interrupt);
            fprintf(f, "P1=%02X P2=%02X BUS=%02X  T0=%d T1=%d INT=%d\n", c.p1, c.p2, c.bus, c.t0,
                    c.t1, c.int_line);
            fprintf(f, "cycles=%llu instructions=%llu frame=%u line=%d dot=%d\n",
                    (unsigned long long)c.cycles, (unsigned long long)c.instructions, c.frame,
                    c.vpos, c.hpos);
            fprintf(f, "VDC control=%02X status=%02X collision mask=%02X result=%02X colour=%02X "
                       "sound=%02X shift=%06X\n",
                    v.control, v.status, v.col_mask, v.col_result, v.color_reg, v.snd_reg, v.shift);
            o2debug_io_get(d, &io);
            fprintf(f, "I/O P1=%02X P2=%02X bank=%d kbd=%d vdc=%d ram=%d copy=%d lum=%d row=%d "
                       "keys=%012llX sticks=%02X,%02X\n",
                    io.p1, io.p2, io.cart_bank, io.kbd_enable, io.vdc_select, io.ram_select,
                    io.copy_mode, io.lum, io.kbd_row, (unsigned long long)io.keys, io.sticks[0],
                    io.sticks[1]);
            fprintf(f, "BIOS: %s\n", io.have_bios ? "imported" : "the stand-in");
        }
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    return fail_msg(msg, msgsz, "Unknown save kind %s (dis, ram, mailbox, vdc, font, screen, regs)", kind);
}

int o2debug_save(o2debug *d, const char *kind, const char *path, char *msg, int msgsz)
{
    int rc;
    o2_host_lock();
    rc = save_locked(d, o2debug_machine(), kind, path, msg, msgsz);
    o2_host_unlock();
    return rc;
}

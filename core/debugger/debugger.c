/*
 * debugger.c -- the o2debug engine (core/include/o2debug.h).
 *
 * Built on the core's two hooks (core/o2/host.h): the instruction hook,
 * fired at every instruction boundary (the CPU's next state is a fetch, R(P)
 * on the next opcode), and the bus hook, fired on data reads, writes and I/O
 * only while a breakpoint of that kind exists. Attached, the instruction
 * hook checks the stop request, the breakpoints and the step mode; stopping
 * PARKS the emulation thread right there (a condition variable) until a
 * window resumes it -- the CPU's own progress is what stops, there is no
 * "paused" flag a run loop polls. Detached, neither hook is installed, so a
 * session that never opens the debugger pays nothing. The design and the
 * feature set (prompt, conditions, read/write/IN/OUT breakpoints, scanline
 * and frame steps, symbols, save) are the SMS sibling's.
 *
 * Threading. Everything public is called from UI threads; the hooks run on
 * the emulation thread. Every public call that reads or edits the machine
 * holds the host's run lock (o2_host_lock), which the emulation thread
 * holds while it runs a frame and lets go of between frames and while it is
 * parked here -- so a window refreshing live values while the machine runs
 * waits a moment instead of tearing, and a stopped machine is simply the
 * UI's. Lock order: the run lock, then d->lock (the hooks run with the run
 * lock held and take d->lock). The CDP1802 stops between instructions with
 * nothing fetched yet, so a PC change is just R(P).
 *
 * The FujiNet trace stands apart: the transport feeds it (fujitrace.c) from
 * session start, attached or not, on its own thread and under its own lock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"

#include "host.h"
#include "dasm1802.h"
#include "fuji_mailbox.h"
#include "fujitcp_host.h"
#include "s2map.h"

#define MAX_BP     128
#define TRACE_N    4096

enum { MODE_RUN = 0, MODE_STEP, MODE_OVER, MODE_OUT, MODE_RUNTO, MODE_SCANLINE, MODE_FRAME };

struct o2debug {
    pthread_mutex_t lock;
    pthread_cond_t cond;

    atomic_int attached;
    atomic_int stopped;          /* the emulation thread is parked */
    atomic_int stop_req;
    atomic_int mode;
    long count;
    uint16_t target, target2;    /* step over: past the SEP, or past SCRT's DW */
    int target_p;                /* the P the step over / out waits for */
    int out_scrt;                /* step out: SCRT's R6 return, else P changing */
    int last_vpos;
    uint64_t target_frame;
    int was_attached;            /* across a power cycle */

    char reason[128];
    int reason_addr;
    atomic_uint generation;

    o2debug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t map_exec[65536];     /* nonzero: an enabled exec bp covers it */
    uint8_t map_mem[65536];      /* O2DEBUG_BP_READ / _WRITE bits */
    uint8_t map_io[8];           /* O2DEBUG_BP_IN / _OUT bits */
    atomic_int break_pending;
    char break_reason[128];
    int break_addr;

    /* the access a condition sees as `value` / `addr` */
    long acc_value, acc_addr;
    int in_access;

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

static o2_machine_t *machine(void)
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
    return m->cpu.r[m->cpu.p];
}

static uint8_t peek_cb(void *user, uint16_t addr)
{
    return o2_machine_peek((o2_machine_t *)user, addr);
}

/* ---- expressions ---------------------------------------------------------- */

const char *const o2debug_reg_names[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
    "r12", "r13", "r14", "r15", "pc", "p", "x", "d", "b", "t", "df", "ie", "q",
    "ef1", "ef2", "ef3", "ef4", "vpos", "hpos", "frame", "cycles", "keylatch",
    "value", "addr", NULL,
};

static uint8_t expr_read(void *user, uint16_t addr)
{
    o2_machine_t *m = machine();
    (void)user;
    return m ? o2_machine_peek(m, addr) : 0xff;
}

/* rN, rN.0 (low byte), rN.1 (high byte), as 1802 assemblers write them */
static int reg_name(const char *name, int *reg, int *half)
{
    char *end;
    long n;

    if (name[0] != 'r' || name[1] < '0' || name[1] > '9')
        return 0;
    n = strtol(name + 1, &end, 10);
    if (n < 0 || n > 15)
        return 0;
    *reg = (int)n;
    *half = -1;
    if (*end == '.' && (end[1] == '0' || end[1] == '1') && end[2] == '\0')
        *half = end[1] - '0';
    else if (*end != '\0')
        return 0;
    return 1;
}

static int ef_now(o2_machine_t *m, int n)
{
    switch (n)
    {
    case 0: return m->vdc.efx_line;
    case 2: return (m->keys[O2_KEYPAD_A] >> m->keylatch) & 1;
    case 3: return (m->keys[O2_KEYPAD_B] >> m->keylatch) & 1;
    default: return 0;
    }
}

static int expr_name(void *user, const char *name, long *out)
{
    o2debug *d = user;
    o2_machine_t *m = machine();
    int reg, half;

    if (!strcmp(name, "value") && d->in_access) { *out = d->acc_value; return 1; }
    if (!strcmp(name, "addr") && d->in_access) { *out = d->acc_addr; return 1; }
    if (m)
    {
        const cdp1802_t *c = &m->cpu;
        if (reg_name(name, &reg, &half))
        {
            *out = half < 0 ? c->r[reg] : half ? c->r[reg] >> 8 : c->r[reg] & 0xff;
            return 1;
        }
        if (!strcmp(name, "pc")) { *out = cur_pc(m); return 1; }
        if (!strcmp(name, "p"))  { *out = c->p; return 1; }
        if (!strcmp(name, "x"))  { *out = c->x; return 1; }
        if (!strcmp(name, "d"))  { *out = c->d; return 1; }
        if (!strcmp(name, "b"))  { *out = c->b; return 1; }
        if (!strcmp(name, "t"))  { *out = c->t; return 1; }
        if (!strcmp(name, "df")) { *out = c->df; return 1; }
        if (!strcmp(name, "ie")) { *out = c->ie; return 1; }
        if (!strcmp(name, "q"))  { *out = c->q; return 1; }
        if (!strncmp(name, "ef", 2) && name[2] >= '1' && name[2] <= '4' && !name[3])
        {
            *out = ef_now(m, name[2] - '1');
            return 1;
        }
        if (!strcmp(name, "vpos")) { *out = cdp1861_vpos(m->cycles); return 1; }
        if (!strcmp(name, "hpos")) { *out = cdp1861_hpos(m->cycles); return 1; }
        if (!strcmp(name, "frame")) { *out = (long)m->frame_count; return 1; }
        if (!strcmp(name, "cycles")) { *out = (long)m->cycles; return 1; }
        if (!strcmp(name, "keylatch")) { *out = m->keylatch; return 1; }
    }
    {
        int a = symtab_find(&d->syms, name);
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
    const cdp1802_t *c = &m->cpu;

    t->pc = pc;
    t->p = c->p;
    t->x = c->x;
    t->d = c->d;
    t->df = c->df;
    t->ie = c->ie;
    t->q = c->q;
    t->rx = c->r[c->x];
    t->bytes[0] = o2_machine_peek(m, pc);
    t->len = (uint8_t)cdp1802_length(t->bytes[0]);
    t->bytes[1] = o2_machine_peek(m, (uint16_t)(pc + 1));
    t->bytes[2] = o2_machine_peek(m, (uint16_t)(pc + 2));
    t->cycles = m->cycles;
    t->vpos = (int16_t)cdp1861_vpos(m->cycles);
    t->hpos = (int16_t)cdp1861_hpos(m->cycles);
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
    o2_cart_frame(m->cart);

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
    /* nothing is fetched yet: a poke or a PC change while stopped is simply
     * what the next fetch sees */
    m->cpu.pc = cur_pc(m);
}

static void instr_hook(o2_machine_t *m, void *user)
{
    o2debug *d = user;
    const uint16_t pc = cur_pc(m);
    char why[128];

    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, m, pc);

    if (atomic_load(&d->break_pending))
    {
        atomic_store(&d->break_pending, 0);
        pthread_mutex_lock(&d->lock);
        snprintf(why, sizeof why, "%s", d->break_reason);
        pthread_mutex_unlock(&d->lock);
        park(d, m, pc, why);
        return;
    }
    if (atomic_load(&d->stop_req))
    {
        park(d, m, pc, "stopped");
        return;
    }
    if (d->map_exec[pc])
    {
        int hit = 0;
        pthread_mutex_lock(&d->lock);
        for (int i = 0; i < d->nbp; i++)
        {
            o2debug_breakpoint *b = &d->bp[i];
            if (b->enabled && (b->type & O2DEBUG_BP_EXEC) && pc >= b->start && pc <= b->end &&
                cond_true(d, b))
            {
                b->hits++;
                hit = 1;
                break;
            }
        }
        pthread_mutex_unlock(&d->lock);
        if (hit)
        {
            snprintf(why, sizeof why, "breakpoint: exec $%04X", pc);
            park(d, m, pc, why);
            return;
        }
    }
    switch (atomic_load(&d->mode))
    {
    case MODE_STEP:
        if (--d->count <= 0)
            park(d, m, pc, "step");
        break;
    case MODE_OVER:
        if (m->cpu.p == d->target_p && (pc == d->target || pc == d->target2))
            park(d, m, pc, "step over");
        break;
    case MODE_OUT:
        if (d->out_scrt ? (m->cpu.p == 3 && pc == d->target) : m->cpu.p != d->target_p)
            park(d, m, pc, "step out");
        break;
    case MODE_RUNTO:
        if (pc == d->target)
            park(d, m, pc, "run to");
        break;
    case MODE_SCANLINE:
    {
        const int vpos = cdp1861_vpos(m->cycles);
        if (vpos != d->last_vpos)
        {
            d->last_vpos = vpos;
            if (--d->count <= 0)
                park(d, m, pc, "scanline");
        }
        break;
    }
    case MODE_FRAME:
        if (m->frame_count >= d->target_frame)
            park(d, m, pc, "frame");
        break;
    default:
        break;
    }
}

static void bus_hook(o2_machine_t *m, void *user, int kind, uint16_t addr, uint8_t data)
{
    o2debug *d = user;
    int type, bits;
    const char *verb;

    (void)m;
    switch (kind)
    {
    case O2_BUS_MEMR: type = O2DEBUG_BP_READ;  bits = d->map_mem[addr]; verb = "read"; break;
    case O2_BUS_MEMW: type = O2DEBUG_BP_WRITE; bits = d->map_mem[addr]; verb = "write"; break;
    case O2_BUS_IOR:  type = O2DEBUG_BP_IN;    bits = d->map_io[addr & 7]; verb = "INP"; break;
    case O2_BUS_IOW:  type = O2DEBUG_BP_OUT;   bits = d->map_io[addr & 7]; verb = "OUT"; break;
    default: return;
    }
    if (!(bits & type) || atomic_load(&d->break_pending))
        return;

    pthread_mutex_lock(&d->lock);
    d->acc_value = data;
    d->acc_addr = addr;
    d->in_access = 1;
    for (int i = 0; i < d->nbp; i++)
    {
        o2debug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & type) && addr >= b->start && addr <= b->end && cond_true(d, b))
        {
            b->hits++;
            if (kind == O2_BUS_IOR || kind == O2_BUS_IOW)
                snprintf(d->break_reason, sizeof d->break_reason, "%s %d %s $%02X", verb,
                         addr & 7, kind == O2_BUS_IOR ? "->" : "<-", data);
            else
                snprintf(d->break_reason, sizeof d->break_reason, "%s $%04X = $%02X", verb, addr, data);
            atomic_store(&d->break_pending, 1);
            break;
        }
    }
    d->in_access = 0;
    pthread_mutex_unlock(&d->lock);
}

/* What the bus hook must watch, from the enabled breakpoints. */
static uint8_t watch_bits(o2debug *d)
{
    uint8_t w = 0;
    for (int i = 0; i < d->nbp; i++)
    {
        if (!d->bp[i].enabled)
            continue;
        if (d->bp[i].type & O2DEBUG_BP_READ)  w |= O2_WATCH_MEMR;
        if (d->bp[i].type & O2DEBUG_BP_WRITE) w |= O2_WATCH_MEMW;
        if (d->bp[i].type & O2DEBUG_BP_IN)    w |= O2_WATCH_IOR;
        if (d->bp[i].type & O2DEBUG_BP_OUT)   w |= O2_WATCH_IOW;
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
    for (int i = 0; i < d->nbp; i++)
    {
        const o2debug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        for (unsigned a = b->start; a <= b->end && a <= 0xffff; a++)
        {
            if (b->type & O2DEBUG_BP_EXEC)
                d->map_exec[a] = 1;
            if (b->type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE))
                d->map_mem[a] |= (uint8_t)(b->type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE));
            if ((b->type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) && a < 8)
                d->map_io[a] |= (uint8_t)(b->type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT));
        }
    }
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

void o2debug_step(o2debug *d)
{
    if (!d || !atomic_load(&d->stopped))
        return;
    d->count = 1;
    go(d, MODE_STEP);
}

void o2debug_step_over(o2debug *d)
{
    o2_machine_t *m;
    int over = 0;

    if (!d)
        return;
    o2_host_lock();
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        const uint16_t pc = cur_pc(m);
        const uint8_t op = o2_machine_peek(m, pc);
        /* SEP to another register is the 1802's call; the caller's P comes
         * back with R(P) past it -- or past the inline DW of the standard
         * call technique (SEP R4; DW target) */
        if ((op & 0xf0) == 0xd0 && (op & 0x0f) != m->cpu.p)
        {
            d->target = (uint16_t)(pc + 1);
            d->target2 = (uint16_t)(pc + 3);
            d->target_p = m->cpu.p;
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
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        /* documented-approximate, as on every sibling: SCRT code (P = 3)
         * returns to the address its call left in R6; anything else returns
         * when a SEP, RET or DIS changes P */
        d->out_scrt = m->cpu.p == 3;
        d->target = m->cpu.r[6];
        d->target_p = m->cpu.p;
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
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        d->last_vpos = cdp1861_vpos(m->cycles);
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
    m = machine();
    if (m && atomic_load(&d->stopped))
    {
        d->target_frame = m->frame_count + (uint64_t)(n > 0 ? n : 1);
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
    d->target = addr;
    go(d, MODE_RUNTO);
}

/* ---- CPU ---------------------------------------------------------------------- */

static const char *const state_names[] = {
    "fetch", "init", "execute", "execute (2nd)", "DMA in", "DMA out", "interrupt",
};
static const char *const mode_names[] = { "load", "reset (CLEAR)", "pause", "run" };

static void cpu_get_locked(o2_machine_t *m, o2debug_cpu *o)
{
    const cdp1802_t *c;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    c = &m->cpu;
    for (int i = 0; i < 16; i++)
        o->r[i] = c->r[i];
    o->pc = cur_pc(m);
    o->p = c->p; o->x = c->x; o->d = c->d; o->b = c->b; o->t = c->t;
    o->df = c->df; o->ie = c->ie; o->q = c->q;
    for (int i = 0; i < 4; i++)
        o->ef[i] = ef_now(m, i);
    o->state = c->state;
    o->state_name = c->state >= 0 && c->state < 7 ? state_names[c->state] : "?";
    o->mode = c->mode;
    o->mode_name = c->mode >= 0 && c->mode < 4 ? mode_names[c->mode] : "?";
    o->int_line = m->vdc.int_line;
    o->dma_line = m->vdc.dma_line;
    o->cycles = m->cycles;
    o->instructions = c->instructions;
    o->vpos = cdp1861_vpos(m->cycles);
    o->hpos = cdp1861_hpos(m->cycles);
    o->frame = (uint32_t)m->frame_count;
}

void o2debug_cpu_get(o2debug *d, o2debug_cpu *o)
{
    (void)d;
    o2_host_lock();
    cpu_get_locked(machine(), o);
    o2_host_unlock();
}

void o2debug_cpu_set(o2debug *d, int reg, int v)
{
    o2_machine_t *m;
    cdp1802_t *c;

    if (!d)
        return;
    o2_host_lock();
    m = machine();
    if (!m || !atomic_load(&d->stopped))
    {
        o2_host_unlock();
        return;
    }
    c = &m->cpu;
    if (reg >= O2_REG_R0 && reg < O2_REG_R0 + 16)
        c->r[reg - O2_REG_R0] = (uint16_t)v;
    else
    {
        switch (reg)
        {
        case O2_REG_PC: c->r[c->p] = (uint16_t)v; break;
        case O2_REG_P:  c->p = (uint8_t)(v & 15); break;
        case O2_REG_X:  c->x = (uint8_t)(v & 15); break;
        case O2_REG_D:  c->d = (uint8_t)v; break;
        case O2_REG_B:  c->b = (uint8_t)v; break;
        case O2_REG_T:  c->t = (uint8_t)v; break;
        case O2_REG_DF: c->df = v != 0; break;
        case O2_REG_IE: c->ie = v != 0; break;
        case O2_REG_Q:
            c->q = v != 0;
            if (c->bus.q_write)
                c->bus.q_write(c->bus.user, c->q);
            break;
        default: break;
        }
    }
    /* the instruction about to run is wherever R(P) points now */
    c->pc = c->r[c->p];
    pthread_mutex_lock(&d->lock);
    d->reason_addr = c->pc;
    pthread_mutex_unlock(&d->lock);
    o2_host_unlock();
    bump(d);
}

/* ---- video ---------------------------------------------------------------------- */

static void video_get_locked(o2_machine_t *m, o2debug_video *o)
{
    const cdp1861_t *v;

    memset(o, 0, sizeof *o);
    if (!m)
        return;
    v = &m->vdc;
    o->display_on = v->disp;
    o->int_line = v->int_line;
    o->efx_line = v->efx_line;
    o->dma_line = v->dma_line;
    o->vpos = cdp1861_vpos(m->cycles);
    o->hpos = cdp1861_hpos(m->cycles);
    o->in_display = o->vpos >= CDP1861_SCANLINE_DISPLAY_START && o->vpos < CDP1861_SCANLINE_DISPLAY_END;
    o->dma_pointer = m->cpu.r[0];
    o->last_dma_addr = v->last_dma_addr;
    o->dma_bytes = v->dma_bytes;
    o->inp1_count = m->inp1_count;
    o->next_int_clock = v->int_next;
    o->next_efx_clock = v->efx_next;
    o->next_dma_clock = v->dma_next;
    o->next_int_line = cdp1861_vpos(v->int_next);
    o->next_efx_line = cdp1861_vpos(v->efx_next);
    o->frame = (uint32_t)m->frame_count;
    if (m->cart)
    {
        o2_cart_status_t st;
        o2_cart_status(m->cart, &st);
        o->short_lines = st.short_lines;
    }
}

void o2debug_video_get(o2debug *d, o2debug_video *o)
{
    (void)d;
    o2_host_lock();
    video_get_locked(machine(), o);
    o2_host_unlock();
}

#define LIT   0x00ffffffu
#define DARK  0x00000000u

static uint32_t dim(uint32_t px)
{
    /* blanking drawn at a third, so the visible area stands out */
    return ((px >> 2) & 0x3f3f3fu) | 0x101010u;
}

int o2debug_video_view(o2debug *d, int view, uint32_t *dst, int *width, int *height)
{
    o2_machine_t *m;
    int ok = 0;

    (void)d;
    o2_host_lock();
    m = machine();
    if (m)
    {
        ok = 1;
        switch (view)
        {
        case O2DEBUG_VIEW_SCREEN:
        {
            const int W = CDP1861_SCREEN_WIDTH, H = CDP1861_TOTAL_SCANLINES;
            const int bv = cdp1861_vpos(m->cycles), bh = cdp1861_hpos(m->cycles);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    uint32_t px = m->vdc.bitmap[y * W + x];
                    const int vis = x >= CDP1861_VIS_X0 && y >= CDP1861_VIS_Y0;
                    if (!vis)
                        px = dim(px);
                    if ((x == CDP1861_VIS_X0 - 1 || y == CDP1861_VIS_Y0 - 1) &&
                        x >= CDP1861_VIS_X0 - 1 && y >= CDP1861_VIS_Y0 - 1)
                        px = O2SESSION_ACCENT_RGB;
                    if (y == bv || x == bh)
                        px = (px & 0xffffffu) == 0 ? 0x00603030u : 0x00ff8080u;
                    dst[y * W + x] = px;
                }
            *width = W;
            *height = H;
            break;
        }
        case O2DEBUG_VIEW_RASTER:
        {
            uint8_t raster[FN_RASTER_SIZE];
            if (!m->cart || o2_cart_copy_raster(m->cart, raster, sizeof raster) != sizeof raster)
                memset(raster, 0, sizeof raster);
            for (int y = 0; y < 128; y++)
                for (int x = 0; x < 64; x++)
                    dst[y * 64 + x] = (raster[y * 8 + x / 8] >> (7 - x % 8)) & 1 ? LIT : DARK;
            *width = 64;
            *height = 128;
            break;
        }
        case O2DEBUG_VIEW_RAM:
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 64; x++)
                    dst[y * 64 + x] = (m->ram[0x100 + y * 8 + x / 8] >> (7 - x % 8)) & 1 ? LIT : DARK;
            *width = 64;
            *height = 32;
            break;
        default:
            ok = 0;
            break;
        }
    }
    o2_host_unlock();
    return ok;
}

/* ---- sound and I/O -------------------------------------------------------------- */

static void io_get_locked(o2_machine_t *m, o2debug_io *o)
{
    memset(o, 0, sizeof *o);
    if (!m)
        return;
    o->q = m->cpu.q;
    o->beep_hz = O2_BEEP_HZ;
    o->keylatch = m->keylatch;
    o->keys[0] = m->keys[0];
    o->keys[1] = m->keys[1];
    o->ef3 = ef_now(m, 2);
    o->ef4 = ef_now(m, 3);
    o->clear_held = m->clear_held;
    memcpy(o->last_out, m->last_out, sizeof o->last_out);
    o->inp1_count = m->inp1_count;
    o->have_bios = m->have_bios;
    o->stub_parked = m->stub_parked;
}

void o2debug_io_get(o2debug *d, o2debug_io *o)
{
    (void)d;
    o2_host_lock();
    io_get_locked(machine(), o);
    o2_host_unlock();
}

/* ---- memory ------------------------------------------------------------------- */

int o2debug_read(o2debug *d, uint16_t addr, uint8_t *dst, int n)
{
    o2_machine_t *m;
    (void)d;
    o2_host_lock();
    m = machine();
    if (m)
        for (int i = 0; i < n; i++)
            dst[i] = o2_machine_peek(m, (uint16_t)(addr + i));
    o2_host_unlock();
    return m ? n : 0;
}

void o2debug_write(o2debug *d, uint16_t addr, uint8_t value)
{
    o2_machine_t *m;
    o2_host_lock();
    m = machine();
    if (m)
        o2_machine_poke(m, addr, value);
    o2_host_unlock();
    if (m)
        bump(d);
}

void o2debug_ram_get(o2debug *d, uint8_t out[512])
{
    o2_machine_t *m;
    (void)d;
    o2_host_lock();
    m = machine();
    if (m)
        memcpy(out, m->ram, O2_RAM_SIZE);
    else
        memset(out, 0, O2_RAM_SIZE);
    o2_host_unlock();
}

/* ---- labels ---------------------------------------------------------------------- */

int o2debug_bank_at(o2debug *d, uint16_t addr)
{
    (void)d;
    (void)addr;
    return SYM_BANK_ANY;     /* the console has none; the cart's window ($C400,
                              * protocol 2) banks, but labels name addresses */
}

int o2debug_label_address(o2debug *d, const char *label)
{
    return d ? symtab_find(&d->syms, label) : -1;
}

int o2debug_address_label(o2debug *d, uint16_t addr, char *dst, int dstsz)
{
    const char *l;
    if (!d || !dst || dstsz <= 0)
        return 0;
    l = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    return snprintf(dst, (size_t)dstsz, "%s", l ? l : "");
}

int o2debug_set_label(o2debug *d, uint16_t addr, const char *label)
{
    if (!d)
        return -1;
    symtab_remove_at(&d->syms, addr);
    if (label && *label)
        symtab_add(&d->syms, addr, SYM_BANK_ANY, label, 0);
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
                n = symtab_load(&d->syms, cand, msg, msgsz);
                bump(d);
                return n;
            }
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "No .map, .lst or .sym next to %s", d->cart_path);
        return -1;
    }
    n = symtab_load(&d->syms, path, msg, msgsz);
    bump(d);
    return n;
}

/* ---- disassembly ------------------------------------------------------------------ */

static int decode_at(o2_machine_t *m, uint16_t addr, char *text, size_t textsz,
                     unsigned *flags, int32_t *target)
{
    /* MAME's disassembler shows an OUT's inline byte when P == X now */
    return dasm1802(addr, peek_cb, m, m->cpu.p == m->cpu.x, text, textsz, flags, target);
}

static void fill_line(o2debug *d, o2_machine_t *m, uint16_t addr, o2debug_line *l, uint16_t pc)
{
    unsigned flags;
    int32_t target;
    const char *lab;
    uint8_t op;

    memset(l, 0, sizeof *l);
    l->length = decode_at(m, addr, l->disasm, sizeof l->disasm, &flags, &target);
    l->address = addr;
    l->is_pc = addr == pc;
    l->has_breakpoint = d->map_exec[addr] != 0;
    for (int i = 0, o = 0; i < l->length && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X", i ? " " : "",
                      o2_machine_peek(m, (uint16_t)(addr + i)));
    lab = symtab_lookup(&d->syms, addr, SYM_BANK_ANY);
    if (lab)
        snprintf(l->label, sizeof l->label, "%s", lab);

    op = o2_machine_peek(m, addr);
    if (target >= 0)
    {
        const char *t = symtab_lookup(&d->syms, (uint16_t)target, SYM_BANK_ANY);
        if (t)
            snprintf(l->comment, sizeof l->comment, "%s", t);
    }
    else if (op >= 0x61 && op <= 0x6f && op != 0x68)
    {
        const char *pn = o2_port_name(op & 7, op < 0x68);
        if (pn)
            snprintf(l->comment, sizeof l->comment, "%s", pn);
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
    m = machine();
    if (m)
    {
        pc = cur_pc(m);
        while (n < max)
        {
            fill_line(d, m, addr, &out[n], pc);
            if (out[n].is_pc && pc_line)
                *pc_line = n;
            addr = (uint16_t)(addr + out[n].length);
            n++;
        }
    }
    o2_host_unlock();
    return n;
}

static int row_address_locked(o2debug *d, o2_machine_t *m, uint16_t addr, int rows);

int o2debug_row_address(o2debug *d, uint16_t addr, int rows)
{
    int a;
    o2_host_lock();
    a = row_address_locked(d, machine(), addr, rows);
    o2_host_unlock();
    return a;
}

static int insn_len(o2_machine_t *m, uint16_t addr)
{
    char text[64];
    unsigned flags;
    int32_t target;
    return decode_at(m, addr, text, sizeof text, &flags, &target);
}

static int row_address_locked(o2debug *d, o2_machine_t *m, uint16_t addr, int rows)
{
    if (!d || !m || rows == 0)
        return addr;
    if (rows > 0)
    {
        for (int i = 0; i < rows; i++)
            addr = (uint16_t)(addr + insn_len(m, addr));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on
     * addr, from as far back as the rows could take. */
    rows = -rows;
    for (int back = rows * 3; back >= 1; back--)
    {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256)
        {
            int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + insn_len(m, a));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
}

/* ---- the cartridge ----------------------------------------------------------------- */

static void cart_get_locked(o2debug_cart *o)
{
    o2_cart_status_t st;
    o2_cart_t *c = o2_host_cart();

    memset(o, 0, sizeof *o);
    o2_host_cart_status(&st);
    o->present = st.powered;
    if (!st.powered)
        return;
    o->link_up = st.link_up;
    o->busy = st.busy;
    o->direct = st.direct;
    o->booted_game = st.booted_game;
    o->mailbox_mode = st.mailbox_mode;
    o->mode = st.mode;
    o->mode_name = st.mode == FN_MODE_BOOT ? (st.direct ? "app (opened file)" : "CONFIG")
                 : st.mode == FN_MODE_GAME ? "game"
                 : st.mode == FN_MODE_APP ? "app (mailbox live)" : "?";
    o->live_mailbox = st.live_mailbox;
    o->live_st2 = st.live_st2;
    o->live_blocks = st.live_blocks;
    o->live_size = st.live_size;
    o->live_crc = st.live_crc;
    o->staged = st.staged;
    o->staged_claim = st.staged_claim;
    o->staged_config = st.staged_config;
    o->armed = st.armed;
    o->staged_crc = st.staged_crc;
    o->swaps = st.swaps;
    o->nbanks = st.nbanks;
    o->bank = st.bank;
    if (c)
    {
        o2_cart_text_t tx;
        o2_cart_page_types(c, o->page_type);
        o2_cart_text(c, &tx);
        memcpy(o->text, tx.chars, sizeof o->text);
        memcpy(o->text_inv, tx.inv, sizeof o->text_inv);
        o->cursor_row = tx.row;
        o->cursor_col = tx.col;
        o->inverse = tx.inverse;
    }
    o->boot_state = st.boot_state;
    o->boot_pct = st.boot_pct;
    o->boot_err = st.boot_err;
    o->boot_got = st.boot_got;
    o->boot_total = st.boot_total;
    o->ackseq = st.ackseq;
    o->status = st.status;
    o->last_error = st.err;
    o->reply_cmd = st.reply_cmd;
    o->rxlen = st.rxlen;
    o->frames = st.frames;
    o->short_lines = st.short_lines;
    o->queue_depth = st.queue;
}

void o2debug_cart_get(o2debug *d, o2debug_cart *o)
{
    (void)d;
    o2_host_lock();
    cart_get_locked(o);
    o2_host_unlock();
}

int o2debug_cart_info(o2debug *d, char *dst, int dstsz)
{
    o2debug_cart c;
    o2debug_cart_get(d, &c);
    if (!c.present)
        return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: not powered");
    if (!c.mailbox_mode)
        return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: game, %u bytes%s (no mailbox)",
                        c.live_size, c.direct ? " (opened file)" : "");
    return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, %u bytes, link %s",
                    c.mode_name, c.live_size, c.link_up ? "up" : "down");
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
    if (!c.present)
        return snprintf(dst, (size_t)dstsz, "The cartridge is not powered.\n");
    n = snprintf(dst, (size_t)dstsz,
        "Link         %s%s\n"
        "Mode         %s%s\n"
        "\n"
        "Mailbox\n"
        "  ACKSEQ     $%02X\n"
        "  STATUS     $%02X\n"
        "  last error %u\n"
        "  reply cmd  $%02X\n"
        "  RXLEN      %u\n"
        "  queue      %u\n"
        "\n"
        "Boot         state %d  %d%%\n"
        "             error %d\n"
        "             %u of %u bytes\n"
        "Staged       %s%s%s\n"
        "             CRC %08X\n"
        "Swaps        %d\n",
        c.link_up ? "up" : "down", c.busy ? ", transaction in flight" : "",
        c.mode_name ? c.mode_name : "?", c.booted_game ? ", game booted" : "",
        c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth,
        c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total,
        c.staged ? (c.staged_config ? "CONFIG" : c.staged_claim ? "a FujiNet app" : "a game")
                 : "nothing",
        c.staged && c.armed ? ", armed" : "", "",
        c.staged_crc, c.swaps);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- breakpoints ---------------------------------------------------------------- */

int o2debug_breakpoint_add(o2debug *d, int type, uint16_t start, uint16_t end, const char *cond)
{
    o2debug_breakpoint *b;
    char err[96];
    int id;

    if (!d || !type)
        return -1;
    if (cond && *cond && expr_eval(cond, NULL, NULL, err, sizeof err) != 0)
        return -1;
    if (end < start)
    {
        uint16_t t = start;
        start = end;
        end = t;
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
    return d && d->map_exec[addr] != 0;
}

int o2debug_breakpoint_toggle(o2debug *d, uint16_t addr)
{
    int found = -1;
    if (!d)
        return 0;
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
    return o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, addr, addr, NULL) >= 0;
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

static int write_bytes(const char *path, const uint8_t *data, size_t n, char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        if (f)
            fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static int save_locked(o2debug *d, o2_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz);

int o2debug_save(o2debug *d, const char *kind, const char *path, char *msg, int msgsz)
{
    int rc;
    o2_host_lock();
    rc = save_locked(d, machine(), kind, path, msg, msgsz);
    o2_host_unlock();
    return rc;
}

/* The pages worth disassembling: the BIOS (or the stand-in) and every page
 * the cartridge serves as code -- not the raster, not the hotspots. */
static int code_page(o2_machine_t *m, const uint8_t *types, unsigned pg)
{
    if (pg < 4)
        return 1;
    if (!m->cart)
        return pg < 8;
    return types[pg] == S2PG_ROM && pg < (FN_ARENA_BASE >> 8);
}

static int save_locked(o2debug *d, o2_machine_t *m, const char *kind, const char *path,
                       char *msg, int msgsz)
{
    if (!d || !m || !kind || !path)
    {
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "The machine is not running.");
        return -1;
    }
    if (!strcmp(kind, "ram"))
        return write_bytes(path, m->ram, sizeof m->ram, msg, msgsz);
    if (!strcmp(kind, "arena") || !strcmp(kind, "raster"))
    {
        o2_cart_t *c = o2_host_cart();
        uint8_t buf[FN_ARENA_SIZE];
        uint32_t n;

        if (!c)
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "There is no cartridge.");
            return -1;
        }
        n = !strcmp(kind, "arena") ? o2_cart_copy_arena(c, buf, sizeof buf)
                                   : o2_cart_copy_raster(c, buf, sizeof buf);
        return write_bytes(path, buf, n, msg, msgsz);
    }
    if (!strcmp(kind, "screen"))
    {
        static uint32_t fb[O2_FB_WIDTH * O2_FB_HEIGHT];
        FILE *f = fopen(path, "wb");
        if (!f)
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        o2_machine_snapshot(m, fb);
        fprintf(f, "P6\n%d %d\n255\n", O2_FB_WIDTH, O2_FB_HEIGHT);
        for (int i = 0; i < O2_FB_WIDTH * O2_FB_HEIGHT; i++)
        {
            uint8_t px[3] = { (uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i] };
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
        {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        if (!strcmp(kind, "dis"))
        {
            const uint16_t pc = cur_pc(m);
            uint8_t types[256];
            uint32_t a = 0;
            if (m->cart)
                o2_cart_page_types(m->cart, types);
            else
                memset(types, 0, sizeof types);
            while (a < 0x10000)
            {
                o2debug_line l;
                if (!code_page(m, types, a >> 8))
                {
                    a = (a & ~0xffu) + 0x100;
                    continue;
                }
                fill_line(d, m, (uint16_t)a, &l, pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-10s %-20s%s%s\n", l.address, l.bytes, l.disasm,
                        l.comment[0] ? "; " : "", l.comment);
                a += (uint32_t)l.length;
            }
        }
        else
        {
            o2debug_cpu c;
            o2debug_video v;
            cpu_get_locked(m, &c);
            video_get_locked(m, &v);
            for (int r = 0; r < 16; r++)
                fprintf(f, "R%-2d=%04X%s", r, c.r[r], r % 8 == 7 ? "\n" : " ");
            fprintf(f, "P=%X X=%X D=%02X DF=%d B=%02X T=%02X IE=%d Q=%d EF1-4=%d%d%d%d\n",
                    c.p, c.x, c.d, c.df, c.b, c.t, c.ie, c.q, c.ef[0], c.ef[1], c.ef[2], c.ef[3]);
            fprintf(f, "state=%s mode=%s cycles=%llu frame=%u vpos=%d hpos=%d\n",
                    c.state_name, c.mode_name, (unsigned long long)c.cycles, c.frame, c.vpos, c.hpos);
            fprintf(f, "1861 display=%d INT=%d EFx=%d DMA=%d R0=%04X dma_bytes=%llu\n",
                    v.display_on, v.int_line, v.efx_line, v.dma_line, v.dma_pointer,
                    (unsigned long long)v.dma_bytes);
        }
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Unknown save kind %s (dis, ram, arena, raster, screen, regs)", kind);
    return -1;
}

/*
 * mcs48_ops_test -- the 8048 core held to MAME's, instruction for instruction.
 *
 * Each probe (tools/ab/probes/NAME.a48, assembled with AS into a 1K "BIOS" and a
 * 2K cartridge) is a program that exercises one class of instruction. MAME's
 * odyssey2 runs it with a debugger trace that logs, before every instruction,
 *
 *     pc a psw tc tpre p1 p2 r0..r7 totalcycles
 *
 * (tools/ab/mcs48_trace.lua; tools/ab/mcs48_ab.py regenerates the goldens).
 * mcs48_golden.h keeps the probe images and, per probe, the instruction count,
 * an FNV-1a hash of MAME's trace lines and sample lines. This test runs the
 * same bytes on mcs48.c in a small model of the Odyssey 2 around the CPU and
 * checks count and hash; on a difference it prints the first sample line that
 * parts, which narrows it to a window (tools/ab/mcs48_ab.py --check diffs the
 * whole trace against a live MAME).
 *
 * The model is MAME's odyssey2.cpp reduced to what a probe can see:
 *   program   $000-$3FF the probe's ROM; $400-$BFF the standard 2K cartridge
 *             (o2_rom_device: bank P10/P11 * $800, masked to the cart size),
 *             $C00-$FFF mirroring $800-$BFF as read_rom0c does
 *   MOVX      the 6810's 128 bytes at $00-$7F while P14 is low (writes need
 *             P16 low as well); the 8244's status register $A1 while P13 and
 *             P16 are low (reading it acknowledges the vblank interrupt);
 *             everything else reads $FF
 *   ports     P1/P2 read back their latches (no keys, no stick pressed), BUS
 *             and T0 float
 *   T1        the 8244's hblank | vblank, a function of the machine cycle:
 *             20 pixel clocks to the cycle, 455 to the line, 263 lines, the
 *             beam on line 243 at power-on (the screen's frame starts at the
 *             bottom of the visible area); hblank from pixel 366 to 452,
 *             vblank from line 242 pixel 365 to line 0 pixel 365
 *   INT       the 8244's vblank interrupt, raised at the first instruction
 *             boundary at or after line 242 pixel 365 of each frame
 * A probe that touches the 8244 beyond $A1 is outside the model: the test
 * counts it and fails.
 *
 * Two instances also run interleaved, to hold the core to "no globals".
 *
 * Usage:  mcs48_ops_test                       all probes against the goldens
 *         mcs48_ops_test --run ROM CART [N]    print a probe's trace (the
 *                                              files are 1024 and 2048 bytes);
 *                                              N caps the instructions
 *         mcs48_ops_test --list                the probes' names
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcs48.h"

/* what mcs48_golden.h holds (see tools/ab/mcs48_ab.py --gen) */
typedef struct { uint16_t addr, len; const uint8_t *data; } mcs48_seg;
typedef struct { unsigned index; const char *text; } mcs48_sample;
typedef struct {
    const char *name;
    const mcs48_seg *rom;       /* the assembled bytes at 0-$3FF; the rest is $FF */
    unsigned nrom;
    const mcs48_seg *cart;      /* the cartridge, offsets 0-$7FF */
    unsigned ncart;
    unsigned count;             /* MAME's trace: instructions up to the end marker */
    uint64_t hash;              /* FNV-1a of its lines */
    const mcs48_sample *sample;
    unsigned nsample;
} mcs48_probe;

#include "mcs48_golden.h"

#define DONE_PC        0x3fe        /* the probes' end marker (common_end.inc) */
#define MAX_STEPS      2000000

#define PIX_PER_CYCLE  20           /* pixel clocks per machine cycle (nominal) */
#define PIX_ATTO       139682557420ULL      /* HZ_TO_ATTOSECONDS(7159090), the pixel */
#define CYCLE_ATTO     2793647246162ULL     /* HZ_TO_ATTOSECONDS(357955), the machine cycle */
#define LINE_PIX       455
#define FRAME_LINES    263
#define FRAME_PIX      (LINE_PIX * FRAME_LINES)
#define START_LINE     243          /* the beam's line at power-on */
#define HBLANK_START   366
#define HBLANK_END     453
#define VBLANK_LINE    242
/* The 8244's vblank timer is set from power-on to screen().time_until_pos(242,
 * 365) = 119575 pixel clocks, and repeats every frame. MAME accepts the INT one
 * machine cycle (20 pixel clocks) earlier than that: probes vblank and extint
 * pass for 17 to 21 pixel clocks, over fifteen frames and on both parities of
 * the instruction boundaries; 16 and 22 do not. */
#define VBLANK_PIX     (119575 - PIX_PER_CYCLE)
#define BGATE_START    413

#define FNV_OFFSET     0xcbf29ce484222325ULL
#define FNV_PRIME      0x100000001b3ULL
#define LINE_MAX_CHARS 96

typedef struct {
    mcs48_t cpu;
    uint8_t rom[1024];
    uint8_t cart[2048];
    uint8_t xram[128];
    uint8_t p1, p2;
    uint64_t vblank_atto;           /* the next vblank start, in attoseconds */
    uint8_t vdc_status;             /* the 8244's control_status: $08 after a vblank */
    unsigned unmodelled;            /* accesses the model has no answer for */
    /* the trace */
    uint64_t hash;
    unsigned count;
    int finished;                   /* the end marker has been traced */
    FILE *dump;
    const mcs48_sample *want;       /* the golden's sample lines, in index order, or NULL */
    unsigned nwant, next_want;
    char (*got)[LINE_MAX_CHARS];    /* the lines at those indexes */
} rig_t;

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

/* ---- the beam, from the machine cycle ---- */

static void beam(uint64_t cycle, int *vpos, int *hpos)
{
    /* screen_device::vpos(): the frame starts at the bottom of the visible area,
     * so at power-on the beam is on line 243 (8244: 243 visible lines). The time
     * of a machine cycle is MAME's HZ_TO_ATTOSECONDS(5369317 / 15 rounded up)
     * against the pixel clock's, so a cycle is a hair under 20 pixels. */
    const unsigned pos = (unsigned)(((cycle * CYCLE_ATTO + PIX_ATTO / 2) / PIX_ATTO
                                     + START_LINE * LINE_PIX) % FRAME_PIX);
    *vpos = (int)(pos / LINE_PIX);
    *hpos = (int)(pos % LINE_PIX);
}

/* i8244_device::vblank() | hblank() */
static int t1_level(uint64_t cycle)
{
    int v, h, vblank, hblank;
    beam(cycle, &v, &h);
    if ((v == VBLANK_LINE && h >= HBLANK_START - 1) || (v == 0 && h <= HBLANK_START - 1))
        vblank = 1;
    else
        vblank = v > VBLANK_LINE;
    hblank = h >= HBLANK_START && h < HBLANK_END;
    return vblank | hblank;
}

/* i8244_device::get_y_beam() */
static int y_beam(uint64_t cycle)
{
    int v, h;
    beam(cycle, &v, &h);
    if (v == 0 && h < HBLANK_START)
        v = FRAME_LINES;
    if (h >= BGATE_START)
        v++;
    return v > 263 ? 263 : v;
}

/* ---- the bus ---- */

static uint8_t rig_program_read(void *user, uint16_t addr, int kind)
{
    rig_t *rig = user;
    (void)kind;
    if (addr < 0x400)
        return rig->rom[addr];
    {
        /* o2_cart_slot_device::read_rom04 / read_rom0c, then the standard cart */
        unsigned offset = addr < 0xc00 ? addr - 0x400u : addr - 0x800u;
        offset = (offset + (unsigned)(rig->p1 & 3) * 0x800u) & (sizeof rig->cart - 1);
        return rig->cart[offset];
    }
}

static uint8_t vdc_read(rig_t *rig, uint8_t offset)
{
    uint8_t data;
    int v, h;

    if ((offset & 0xe0) == 0xa0)
        offset &= (uint8_t)~0x10;           /* $A0-$AF mirror at $B0-$BF */
    if (offset != 0xa1)
    {
        rig->unmodelled++;
        return 0xff;
    }
    beam(rig->cpu.cycles, &v, &h);
    data = rig->vdc_status;
    data |= (h >= 225 && h < BGATE_START && y_beam(rig->cpu.cycles) <= VBLANK_LINE) ? 1 : 0;
    mcs48_set_int(&rig->cpu, 0);            /* m_irq_func(CLEAR_LINE) */
    rig->vdc_status &= (uint8_t)~0xcc;
    return data;
}

static uint8_t rig_ext_read(void *user, uint8_t offset)
{
    rig_t *rig = user;
    uint8_t data = 0xff;                    /* the cartridge's io_read */

    if (!(rig->p1 & 0x10) && !(offset & 0x80))
        data &= rig->xram[offset];
    if ((rig->p1 & 0x48) == 0)
        data &= vdc_read(rig, offset);
    return data;
}

static void rig_ext_write(void *user, uint8_t offset, uint8_t data)
{
    rig_t *rig = user;

    if (!(rig->p1 & 0x40) && !(rig->p1 & 0x10) && !(offset & 0x80))
        rig->xram[offset] = data;
    if (!(rig->p1 & 0x08))
        rig->unmodelled++;
}

static uint8_t rig_port_read(void *user, int port)
{
    (void)user;
    (void)port;
    return 0xff;                            /* no key, no stick: the pins float high */
}

static void rig_port_write(void *user, int port, uint8_t data)
{
    rig_t *rig = user;
    if (port == 1)
        rig->p1 = data;
    else
        rig->p2 = data;
}

static int rig_test_read(void *user, int n)
{
    rig_t *rig = user;
    return n == 1 ? t1_level(rig->cpu.cycles) : 0;
}

static void rig_hook(void *user, uint16_t pc)
{
    rig_t *rig = user;
    const mcs48_t *c = &rig->cpu;
    char text[LINE_MAX_CHARS];

    snprintf(text, sizeof text,
             "%03X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %" PRIu64 "\n",
             pc, c->a, c->psw, c->timer, c->prescaler, c->p1, c->p2,
             mcs48_reg(c, 0), mcs48_reg(c, 1), mcs48_reg(c, 2), mcs48_reg(c, 3),
             mcs48_reg(c, 4), mcs48_reg(c, 5), mcs48_reg(c, 6), mcs48_reg(c, 7), c->cycles);
    for (const char *p = text; *p; p++)
        rig->hash = (rig->hash ^ (uint8_t)*p) * FNV_PRIME;
    if (rig->want && rig->next_want < rig->nwant && rig->want[rig->next_want].index == rig->count)
        memcpy(rig->got[rig->next_want++], text, sizeof text);
    if (rig->dump)
        fputs(text, rig->dump);
    rig->count++;
    if (pc == DONE_PC)
        rig->finished = 1;
}

static void rig_init(rig_t *rig)
{
    mcs48_bus_t bus;

    memset(&bus, 0, sizeof bus);
    bus.user = rig;
    bus.program_read = rig_program_read;
    bus.ext_read = rig_ext_read;
    bus.ext_write = rig_ext_write;
    bus.port_read = rig_port_read;
    bus.port_write = rig_port_write;
    bus.test_read = rig_test_read;
    bus.instruction_hook = rig_hook;

    memset(rig->xram, 0, sizeof rig->xram);
    rig->p1 = rig->p2 = 0xff;
    rig->vblank_atto = (uint64_t)VBLANK_PIX * PIX_ATTO;
    rig->vdc_status = 0;
    rig->unmodelled = 0;
    rig->hash = FNV_OFFSET;
    rig->count = 0;
    rig->finished = 0;
    rig->next_want = 0;
    mcs48_init(&rig->cpu, &bus);
    mcs48_reset(&rig->cpu);
}

/* The 8244's vblank timer: at the first instruction boundary at or after its
 * time (MAME fires timers at the end of the CPU's timeslice, which runs to
 * the next timer and overshoots by the last instruction). */
static void rig_events(rig_t *rig)
{
    while (rig->cpu.cycles * CYCLE_ATTO >= rig->vblank_atto)
    {
        rig->vdc_status |= 0x08;
        mcs48_set_int(&rig->cpu, 1);
        rig->vblank_atto += (uint64_t)FRAME_PIX * PIX_ATTO;
    }
}

/* Step until the end marker has been traced. */
static int rig_run(rig_t *rig, unsigned max)
{
    for (unsigned n = 0; n < max && !rig->finished; n++)
    {
        rig_events(rig);
        mcs48_step(&rig->cpu);
    }
    return rig->finished;
}

static void load_segments(uint8_t *dst, size_t size, const mcs48_seg *seg, unsigned n)
{
    memset(dst, 0xff, size);
    for (unsigned i = 0; i < n; i++)
        memcpy(dst + seg[i].addr, seg[i].data, seg[i].len);
}

/* ---- --run ---- */

static int read_file(const char *path, uint8_t *dst, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f)
        return -1;
    memset(dst, 0xff, size);
    got = fread(dst, 1, size, f);
    fclose(f);
    return got > 0 ? 0 : -1;
}

static int run_files(const char *rom, const char *cart, unsigned max)
{
    static rig_t rig;

    memset(&rig, 0, sizeof rig);
    if (read_file(rom, rig.rom, sizeof rig.rom) || read_file(cart, rig.cart, sizeof rig.cart))
    {
        fprintf(stderr, "mcs48_ops_test: cannot read %s or %s\n", rom, cart);
        return 2;
    }
    rig_init(&rig);
    rig.dump = stdout;
    if (!rig_run(&rig, max ? max : MAX_STEPS))
    {
        fprintf(stderr, "mcs48_ops_test: no end marker after %u instructions\n", rig.count);
        return 1;
    }
    if (rig.unmodelled)
        fprintf(stderr, "mcs48_ops_test: %u accesses outside the model\n", rig.unmodelled);
    return rig.unmodelled ? 1 : 0;
}

/* ---- the goldens ---- */

static void check_probe(const mcs48_probe *probe, rig_t *rig)
{
    static char got[256][LINE_MAX_CHARS];
    char what[256];

    load_segments(rig->rom, sizeof rig->rom, probe->rom, probe->nrom);
    load_segments(rig->cart, sizeof rig->cart, probe->cart, probe->ncart);
    rig_init(rig);
    rig->want = probe->sample;
    rig->nwant = probe->nsample < 256 ? probe->nsample : 256;
    rig->got = got;
    rig->dump = NULL;

    snprintf(what, sizeof what, "%s: ran to the end marker", probe->name);
    check(rig_run(rig, probe->count + 64), what);

    snprintf(what, sizeof what, "%s: %u instructions, MAME's %u", probe->name, rig->count, probe->count);
    check(rig->count == probe->count, what);

    snprintf(what, sizeof what, "%s: trace hash %016" PRIx64 ", MAME's %016" PRIx64, probe->name,
             rig->hash, probe->hash);
    check(rig->hash == probe->hash, what);

    snprintf(what, sizeof what, "%s: stayed inside the model (%u accesses outside)", probe->name,
             rig->unmodelled);
    check(rig->unmodelled == 0, what);

    if (rig->hash != probe->hash || rig->count != probe->count)
    {
        /* the first sample line that parts narrows it to a window */
        for (unsigned i = 0; i < rig->next_want; i++)
        {
            if (strcmp(got[i], probe->sample[i].text) != 0)
            {
                printf("  %s line %u:\n    ours  %s    MAME  %s", probe->name, probe->sample[i].index,
                       got[i], probe->sample[i].text);
                break;
            }
        }
    }
    rig->want = NULL;
}

/* two instances stepped alternately keep to their own state */
static void check_interleaved(const mcs48_probe *probe)
{
    static rig_t a, b;
    char what[160];

    load_segments(a.rom, sizeof a.rom, probe->rom, probe->nrom);
    load_segments(a.cart, sizeof a.cart, probe->cart, probe->ncart);
    memcpy(b.rom, a.rom, sizeof a.rom);
    memcpy(b.cart, a.cart, sizeof a.cart);
    rig_init(&a);
    rig_init(&b);
    a.want = b.want = NULL;
    a.dump = b.dump = NULL;
    for (unsigned n = 0; n < probe->count + 64 && !(a.finished && b.finished); n++)
    {
        if (!a.finished)
        {
            rig_events(&a);
            mcs48_step(&a.cpu);
        }
        if (!b.finished)
        {
            rig_events(&b);
            mcs48_step(&b.cpu);
        }
    }
    snprintf(what, sizeof what, "%s: two instances interleaved end alike (%" PRIu64 " cycles)",
             probe->name, a.cpu.cycles);
    check(a.finished && b.finished && a.cpu.cycles == b.cpu.cycles && a.hash == b.hash &&
          a.count == probe->count && memcmp(a.cpu.ram, b.cpu.ram, 64) == 0, what);
}

/* ---- the API, on its own ---- */

typedef struct {
    uint8_t rom[4096];
    int nport;
    struct { int port; uint8_t data; } port[16];
    int nprog;
    int prog[8];
    int events[32];             /* 1000 + port write order, prog: 2000 + level */
    int nevents;
    uint8_t p2_pins;
    int irq_acks;
} api_t;

static uint8_t api_program(void *user, uint16_t addr, int kind)
{
    api_t *api = user;
    (void)kind;
    return api->rom[addr & 0xfff];
}

static void api_port_write(void *user, int port, uint8_t data)
{
    api_t *api = user;
    if (api->nport < 16)
    {
        api->port[api->nport].port = port;
        api->port[api->nport].data = data;
        api->nport++;
    }
    if (api->nevents < 32)
        api->events[api->nevents++] = 1000 + api->nport - 1;
}

static uint8_t api_port_read(void *user, int port)
{
    api_t *api = user;
    return port == 2 ? api->p2_pins : 0xff;
}

static void api_prog(void *user, int level)
{
    api_t *api = user;
    if (api->nprog < 8)
        api->prog[api->nprog++] = level;
    if (api->nevents < 32)
        api->events[api->nevents++] = 2000 + level;
}

static void api_irq_ack(void *user, int irqline, uint16_t pc)
{
    api_t *api = user;
    (void)irqline;
    (void)pc;
    api->irq_acks++;
}

static void api_checks(void)
{
    static api_t api;
    mcs48_bus_t bus;
    mcs48_t c;

    memset(&bus, 0, sizeof bus);
    bus.user = &api;
    bus.program_read = api_program;
    bus.port_write = api_port_write;
    bus.port_read = api_port_read;
    bus.prog_write = api_prog;
    bus.irq_ack = api_irq_ack;

    /* init does not reset; reset sets the pins high and writes them */
    mcs48_init(&c, &bus);
    check(c.p1 == 0 && c.p2 == 0 && api.nport == 0, "init zeroes the state and writes no port");
    mcs48_reset(&c);
    check(c.pc == 0 && c.p1 == 0xff && c.p2 == 0xff && c.psw == 0 && c.regbase == 0 &&
          api.nport == 2 && api.port[0].port == 1 && api.port[1].port == 2 &&
          api.port[0].data == 0xff && api.port[1].data == 0xff,
          "reset: PC 0, P1 and P2 written $FF");

    /* a reset keeps A, C, AC, RAM and the timer (MAME's device_reset does) */
    c.a = 0x5a;
    c.psw = 0xff;
    c.ram[40] = 0x77;
    c.timer = 0x12;
    c.f1 = true;
    c.a11 = 0x800;
    mcs48_reset(&c);
    check(c.a == 0x5a && c.psw == (MCS48_C_FLAG | MCS48_A_FLAG) && c.ram[40] == 0x77 && c.timer == 0x12 &&
          !c.f1 && c.a11 == 0 && c.regbase == 0, "reset keeps A, C, AC, RAM and the timer; clears BS, SP, F0, F1, MB");

    /* unwired callbacks: the program space reads $FF (MOV A,R7) */
    {
        mcs48_bus_t none;
        memset(&none, 0, sizeof none);
        mcs48_init(&c, &none);
        mcs48_reset(&c);
        check(mcs48_step(&c) == 1 && c.pc == 1 && c.illegal_count == 0, "unwired program space reads $FF (MOV A,R7)");
    }

    /* expander: MOVD P4,A  = P2 command, PROG low, P2 data, PROG high */
    memset(&api, 0, sizeof api);
    api.rom[0] = 0x3c;                      /* movd p4,a */
    api.rom[1] = 0x0d;                      /* movd a,p5 */
    api.rom[2] = 0x8e;                      /* orld p6,a */
    api.rom[3] = 0x9f;                      /* anld p7,a */
    api.p2_pins = 0xf6;
    mcs48_init(&c, &bus);
    mcs48_reset(&c);
    api.nport = api.nprog = api.nevents = 0;
    c.a = 0x0b;
    check(mcs48_step(&c) == 2, "MOVD P4,A: 2 cycles");
    check(api.nport == 2 && api.port[0].data == 0xf4 && api.port[1].data == 0xfb &&
          api.nprog == 2 && api.prog[0] == 0 && api.prog[1] == 1 &&
          api.events[0] == 1000 && api.events[1] == 2000 && api.events[2] == 1001 && api.events[3] == 2001,
          "MOVD P4,A: P2 = command, PROG low, P2 = data, PROG high");
    api.nport = api.nprog = api.nevents = 0;
    check(mcs48_step(&c) == 2 && c.a == 0x06, "MOVD A,P5 reads P2's low nibble");
    check(api.nport == 2 && api.port[0].data == 0xf1 && api.port[1].data == 0xff &&
          api.events[0] == 1000 && api.events[1] == 2000 && api.events[2] == 1001 && api.events[3] == 2001,
          "MOVD A,P5: command, PROG low, P2 input mode, PROG high");
    api.nport = 0;
    c.a = 0x03;
    mcs48_step(&c);
    check(api.nport == 2 && api.port[0].data == 0xfa && api.port[1].data == 0xf3, "ORLD P6,A");
    api.nport = 0;
    mcs48_step(&c);
    check(api.nport == 2 && api.port[0].data == 0xff && api.port[1].data == 0xf3, "ANLD P7,A");

    /* the interrupt: taken at the next step, vector 3, 2 more cycles; A11 forced 0 */
    memset(&api, 0, sizeof api);
    mcs48_init(&c, &bus);
    mcs48_reset(&c);
    api.rom[0x003] = 0x93;                  /* retr at the vector */
    api.rom[0x850] = 0x05;                  /* en i, at $850 */
    c.pc = 0x850;
    c.a11 = 0x800;
    mcs48_step(&c);
    check(c.xirq_enabled && c.pc == 0x851, "EN I");
    mcs48_set_int(&c, 1);
    check(mcs48_step(&c) == 4 && c.last_irq == MCS48_IRQ_EXTERNAL && api.irq_acks == 1,
          "INT taken: 2 cycles of entry and the RETR at the vector (2)");
    check(c.pc == 0x851 && c.psw == 0 && !c.irq_in_progress && mcs48_stack_pc(&c, 0) == 0x851,
          "the RETR came back to $851, A11 kept");

    /* the timer: 32 cycles a tick; overflow sets TF, and with TCNTI the interrupt */
    memset(&api, 0, sizeof api);
    mcs48_init(&c, &bus);
    mcs48_reset(&c);
    api.rom[0x100] = 0x23; api.rom[0x101] = 0xff;   /* mov a,#$FF */
    api.rom[0x102] = 0x62;                  /* mov t,a */
    api.rom[0x103] = 0x25;                  /* en tcnti */
    api.rom[0x104] = 0x55;                  /* strt t */
    api.rom[0x007] = 0x93;                  /* retr */
    c.pc = 0x100;
    mcs48_step(&c); mcs48_step(&c); mcs48_step(&c); mcs48_step(&c);
    check(c.timecount_enabled == MCS48_TIMER_ENABLED && c.timer == 0xff && c.tirq_enabled && c.cycles == 5,
          "STRT T starts the timer");
    for (int n = 0; n < 33 && c.last_irq == MCS48_IRQ_NONE; n++)
        mcs48_step(&c);
    check(c.last_irq == MCS48_IRQ_TIMER && c.timer_flag && c.timer == 0 && api.irq_acks == 1 &&
          c.cycles == 5 + 32 + 4,
          "the timer overflows after 32 cycles and interrupts");
    check(!c.timer_overflow && !c.irq_in_progress, "the overflow is consumed and the RETR ended the interrupt");

    /* stack: eight deep, the ninth call overwrites the first */
    memset(&api, 0, sizeof api);
    mcs48_init(&c, &bus);
    mcs48_reset(&c);
    for (int n = 0; n < 9; n++)
    {
        api.rom[n * 2] = 0x14;              /* call $0xx */
        api.rom[n * 2 + 1] = (uint8_t)((n + 1) * 2);
    }
    for (int n = 0; n < 9; n++)
        mcs48_step(&c);
    check(mcs48_sp(&c) == 1 && mcs48_stack_pc(&c, 0) == 18, "the stack is 8 deep and wraps");

    /* R0-R7 of both banks are the RAM at 0 and 24 */
    mcs48_init(&c, &bus);
    mcs48_reset(&c);
    api.rom[0] = 0xb8; api.rom[1] = 0x11;   /* mov r0,#$11 */
    api.rom[2] = 0xd5;                      /* sel rb1 */
    api.rom[3] = 0xb8; api.rom[4] = 0x22;   /* mov r0,#$22 */
    for (int n = 0; n < 4; n++)
        mcs48_step(&c);
    check(c.ram[0] == 0x11 && c.ram[24] == 0x22 && mcs48_reg(&c, 0) == 0x22 &&
          mcs48_reg_bank(&c, 0, 0) == 0x11 && mcs48_reg_bank(&c, 1, 0) == 0x22 && (c.psw & MCS48_B_FLAG),
          "register banks 0 and 1 are ram[0] and ram[24]");
}

int main(int argc, char **argv)
{
    static rig_t rig;
    unsigned total = 0;

    if (argc >= 2 && strcmp(argv[1], "--list") == 0)
    {
        for (unsigned i = 0; i < mcs48_nprobes; i++)
            printf("%s\n", mcs48_probes[i].name);
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "--run") == 0)
        return run_files(argv[2], argv[3], argc >= 5 ? (unsigned)strtoul(argv[4], NULL, 0) : 0);

    api_checks();

    for (unsigned i = 0; i < mcs48_nprobes; i++)
    {
        check_probe(&mcs48_probes[i], &rig);
        total += rig.count;
    }
    if (mcs48_nprobes)
        check_interleaved(&mcs48_probes[0]);

    printf("cpu_ops: %u probes, %u instructions compared with MAME, %d checks, %d failures\n",
           mcs48_nprobes, total, checks, failures);
    return failures ? 1 : 0;
}

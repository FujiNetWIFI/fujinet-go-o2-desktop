/* ef9340_test.c -- the Videopac+ G7400's text chips: the EF9340/EF9341
 * (core/o2/ef9340.c), the colour mixer and the 8243 I/O expander
 * (core/o2/i8243.c).
 *
 * No ROM is needed: the character generator is generated here (a hash of
 * glyph and slice), which is also what the MAME runs behind the goldens were
 * given in place of their BAD_DUMP charset_ef9340_1.rom. Four groups:
 *
 *   - the 8243's state machine (every operation, CS, the PROG edges, the
 *     G7400's nibble wiring);
 *   - the EF9341 register interface and the cell and extended-RAM address
 *     maps;
 *   - the timing (absolute pixel clocks, the line and blink timers, a
 *     register change taking effect on the next line, the ordering of an
 *     access and a line start) and the renderer (placement, service row,
 *     double height and width, cursor, blink, delimiters, boxing, conceal,
 *     underline, graphics, extended characters, Y0 and zoom, the 210/250
 *     line window), and the debugger's snapshot;
 *   - the mixer, and the MAME A/B: a Videopac+ G7400 cartridge (an 8048
 *     program of ~360 bytes, one image per scenario) fills the 24 x 40 page
 *     and the service row from an 8-bit LFSR (attribute and code masked per
 *     scenario, so every cell kind, double-size, inverted, blinking,
 *     delimiter and extended cell occurs), fills the extended RAM for every
 *     extended cell by slice writes, reads a cell back through the data
 *     registers and rewrites it elsewhere, copies a glyph's ten slices from
 *     the font into an extended character (read slice, write slice), sets
 *     Y0 / X / Y / M / R, and programs the 8243 (MOVD, then ORLD, then ANLD
 *     on P4-P7) and P1's LUM. It ran in MAME's videopacp; the composited
 *     screen at fixed frames was compared with this implementation pixel for
 *     pixel, together with the chip's registers, page RAM and extended RAM
 *     (MAME's save-state items). This file replays the program access for
 *     access (ef9340_io() and the 8243's PROG protocol) and checks a hash of
 *     every 10-line band of those frames and MAME's blink state against the
 *     tables at GOLDEN. The 8245 behind it was flat (control register 0,
 *     one background colour), so the mix is checked per pixel value, not per
 *     VDC image; test_mixer() covers the formula with varied rows.
 *
 * To redo the pixel comparison against MAME's own dumps (never committed):
 *   EF9340_MAME_DIR=<dir holding <scenario>_<frame>.dump> ef9340_test
 * where a dump is 32 state bytes (R M Y0 X Y blink prescaler h_parity TA TB
 * busy mix_i8244 mix_ef934x P1 P2), the 1 KB attribute page, the 1 KB code
 * page, the 2 KB extended RAM and the 360 x 243 palette indices of the
 * screen. EF9340_FONT=<charset_ef9340_1.rom> replays with MAME's own
 * character generator (dumps made with it; the band hashes then do not
 * apply). EF9340_PRINT_GOLDEN=1 prints the band hashes of the current build.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ef9340.h"
#include "i8243.h"

static int g_fail;
static int g_checks;

#define CHECK(cond, ...) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_fail++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

#define CHECK_EQ(got, want) \
    do { \
        const long long g_ = (long long)(got), w_ = (long long)(want); \
        CHECK(g_ == w_, "%s = %lld (0x%llx), want %lld (0x%llx)", #got, g_, (unsigned long long)g_, w_, \
              (unsigned long long)w_); \
    } while (0)

/* ======================================================================
 * the generated character generator
 * ==================================================================== */

static uint8_t testfont_byte(unsigned glyph, unsigned slice)
{
    uint32_t h = (glyph * 10u + slice + 1u) * 2654435761u;

    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return (uint8_t)(h >> 4);
}

static uint8_t g_font[EF9340_FONT_SIZE];

static void make_testfont(void)
{
    for (unsigned g = 0; g < 256; g++)
        for (unsigned s = 0; s < 10; s++)
            g_font[g * 10 + s] = testfont_byte(g, s);
}

/* ======================================================================
 * the 8243
 * ==================================================================== */

typedef struct {
    int writes;
    int last_port;
    uint8_t last_data;
    int reads;
    uint8_t in_value[4];
} x_probe_t;

static uint8_t x_in(void *ctx, int port)
{
    x_probe_t *p = ctx;

    p->reads++;
    return p->in_value[port];
}

static void x_out(void *ctx, int port, uint8_t data)
{
    x_probe_t *p = ctx;

    p->writes++;
    p->last_port = port;
    p->last_data = data;
}

/* The 8048's expander_operation (mcs48.cpp), with its P2 low nibble handed to
 * the 8243 and PROG pulsed; returns the nibble the CPU would read back. */
static uint8_t expander(i8243_t *x, uint8_t *p2, int op, int port, uint8_t a)
{
    uint8_t got = 0;

    *p2 = (uint8_t)((*p2 & 0xf0) | (op << 2) | (port & 3));
    i8243_p2_w(x, *p2 & 0x0f);
    i8243_prog_w(x, 0);
    if (op != I8243_OP_READ)
    {
        *p2 = (uint8_t)((*p2 & 0xf0) | (a & 0x0f));
        i8243_p2_w(x, *p2 & 0x0f);
    }
    else
    {
        *p2 |= 0x0f;
        i8243_p2_w(x, *p2 & 0x0f);
        got = i8243_p2_r(x);    /* what a CPU that wired p2_r would read */
    }
    i8243_prog_w(x, 1);
    return got;
}

static void test_i8243(void)
{
    i8243_t x;
    x_probe_t probe;
    uint8_t p2 = 0xff;

    memset(&probe, 0, sizeof probe);
    i8243_init(&x, x_in, x_out, &probe);
    CHECK_EQ(i8243_p2_r(&x), 0x0f);
    CHECK_EQ(x.prog, 1);
    CHECK_EQ(x.cs, 0);

    /* WRITE: the port latch takes the nibble when PROG goes high */
    expander(&x, &p2, I8243_OP_WRITE, 1, 0x0a);
    CHECK_EQ(probe.writes, 1);
    CHECK_EQ(probe.last_port, 1);
    CHECK_EQ(probe.last_data, 0x0a);
    CHECK_EQ(x.p[1], 0x0a);
    CHECK_EQ(x.p[0], 0);

    /* OR and AND work on the latch and report the result */
    expander(&x, &p2, I8243_OP_OR, 1, 0x05);
    CHECK_EQ(probe.last_data, 0x0f);
    expander(&x, &p2, I8243_OP_AND, 1, 0x36);
    CHECK_EQ(probe.last_data, 0x06);
    CHECK_EQ(x.p[1], 0x06);
    CHECK_EQ(probe.writes, 3);

    /* the high nibble of P2 is not the expander's: only the low four count */
    p2 = 0xf0;
    expander(&x, &p2, I8243_OP_WRITE, 3, 0xff);
    CHECK_EQ(probe.last_port, 3);
    CHECK_EQ(probe.last_data, 0x0f);

    /* READ: the port is read when PROG falls and driven until it rises */
    probe.in_value[2] = 0xa7;
    probe.reads = 0;
    i8243_p2_w(&x, (I8243_OP_READ << 2) | 2);
    i8243_prog_w(&x, 0);
    CHECK_EQ(probe.reads, 1);
    CHECK_EQ(i8243_p2_r(&x), 0x07);
    CHECK_EQ(x.p[2], 0xa7);                 /* MAME keeps the byte it read */
    i8243_prog_w(&x, 1);
    CHECK_EQ(i8243_p2_r(&x), 0x0f);         /* bus released */

    /* a write operation drives nothing */
    i8243_p2_w(&x, (I8243_OP_WRITE << 2) | 0);
    i8243_prog_w(&x, 0);
    CHECK_EQ(i8243_p2_r(&x), 0x0f);
    i8243_p2_w(&x, 0x03);
    i8243_prog_w(&x, 1);
    CHECK_EQ(x.p[0], 0x03);

    /* PROG that does not change is not an edge */
    {
        const int w = probe.writes;

        i8243_prog_w(&x, 1);
        i8243_prog_w(&x, 1);
        CHECK_EQ(probe.writes, w);
        i8243_p2_w(&x, (I8243_OP_WRITE << 2) | 0);
        i8243_prog_w(&x, 0);
        i8243_prog_w(&x, 0);            /* no second latch: opcode stays */
        i8243_p2_w(&x, 0x0c);           /* (data changed after the first edge) */
        i8243_prog_w(&x, 1);
        CHECK_EQ(x.p[0], 0x0c);
        CHECK_EQ(probe.writes, w + 1);
    }

    /* CS high: no operation, and the bus is released at once */
    {
        const int w = probe.writes;

        i8243_p2_w(&x, (I8243_OP_READ << 2) | 2);
        i8243_prog_w(&x, 0);
        CHECK_EQ(i8243_p2_r(&x), 0x07);
        i8243_cs_w(&x, 1);
        CHECK_EQ(i8243_p2_r(&x), 0x0f);
        i8243_prog_w(&x, 1);
        expander(&x, &p2, I8243_OP_WRITE, 0, 0x01);
        CHECK_EQ(probe.writes, w);
        CHECK_EQ(x.p[0], 0x0c);
        i8243_cs_w(&x, 0);
        expander(&x, &p2, I8243_OP_WRITE, 0, 0x01);
        CHECK_EQ(probe.writes, w + 1);
        CHECK_EQ(x.p[0], 0x01);
    }

    /* unconfigured ports (MAME logs and carries on): a read keeps the latch,
     * a write still updates the latch */
    i8243_init(&x, NULL, NULL, NULL);
    expander(&x, &p2, I8243_OP_WRITE, 2, 0x09);
    CHECK_EQ(x.p[2], 0x09);
    i8243_p2_w(&x, (I8243_OP_READ << 2) | 2);
    i8243_prog_w(&x, 0);
    CHECK_EQ(i8243_p2_r(&x), 0x09);
    i8243_prog_w(&x, 1);

    /* the G7400's wiring: P4 and P5 are the EF9340 table's nibbles, P6 and
     * P7 the 8245's, high nibble first */
    {
        ef9340_mix_t mix;

        ef9340_mix_reset(&mix);
        CHECK_EQ(mix.vdc, 0xff);
        CHECK_EQ(mix.ef, 0xff);
        ef9340_mix_port_w(&mix, 0, 0x3);        /* P4: ef bits 7-4 */
        CHECK_EQ(mix.ef, 0x3f);
        ef9340_mix_port_w(&mix, 1, 0xa);        /* P5: ef bits 3-0 */
        CHECK_EQ(mix.ef, 0x3a);
        ef9340_mix_port_w(&mix, 2, 0x5);        /* P6: vdc bits 7-4 */
        CHECK_EQ(mix.vdc, 0x5f);
        ef9340_mix_port_w(&mix, 3, 0xc);        /* P7: vdc bits 3-0 */
        CHECK_EQ(mix.vdc, 0x5c);
        ef9340_mix_port_w(&mix, 3, 0xf1);       /* only a nibble */
        CHECK_EQ(mix.vdc, 0x51);
    }
}

/* ======================================================================
 * the EF9341 interface and the address maps
 * ==================================================================== */

static ef9340_t g_e;

static void cmd(ef9340_t *e, uint8_t ta, uint8_t tb)
{
    ef9340_write(e, 0, 1, 0, ta);
    ef9340_write(e, 0, 1, 1, tb);
}

static void dat(ef9340_t *e, uint8_t ta, uint8_t tb)
{
    ef9340_write(e, 0, 0, 0, ta);
    ef9340_write(e, 0, 0, 1, tb);
}

static void fresh(ef9340_t *e, const uint8_t *font)
{
    ef9340_init(e, font);
}

static void test_addresses(void)
{
    /* ef9340_get_c_addr */
    CHECK_EQ(ef9340_cell_address(0, 0), 0x000);
    CHECK_EQ(ef9340_cell_address(31, 0), 0x01f);
    CHECK_EQ(ef9340_cell_address(0, 1), 0x020);
    CHECK_EQ(ef9340_cell_address(31, 23), 0x2ff);
    CHECK_EQ(ef9340_cell_address(32, 0), 0x300);        /* columns 32-39 live at 0x300 */
    CHECK_EQ(ef9340_cell_address(39, 0), 0x307);
    CHECK_EQ(ef9340_cell_address(32, 1), 0x320);
    CHECK_EQ(ef9340_cell_address(32, 8), 0x308);
    CHECK_EQ(ef9340_cell_address(39, 23), 0x300 | (7 << 5) | 16 | 7);
    CHECK_EQ(ef9340_cell_address(0, 31), 0x318);        /* the service row */
    CHECK_EQ(ef9340_cell_address(7, 31), 0x31f);
    CHECK_EQ(ef9340_cell_address(8, 31), 0x338);
    CHECK_EQ(ef9340_cell_address(39, 31), 0x318 | (32 << 2) | 7);
    CHECK_EQ(ef9340_cell_address(5, 24), 0x318 | 5);    /* rows 24-31 all take that branch */

    /* all displayed cells are distinct */
    {
        static uint8_t seen[0x400];

        memset(seen, 0, sizeof seen);
        for (int y = 0; y < 24; y++)
            for (int x = 0; x < 40; x++)
                seen[ef9340_cell_address((uint8_t)x, (uint8_t)y)]++;
        for (int x = 0; x < 40; x++)
            seen[ef9340_cell_address((uint8_t)x, 31)]++;
        for (int a = 0; a < 0x400; a++)
            CHECK(seen[a] <= 1, "cell address 0x%03x used %d times", a, seen[a]);
    }

    /* vpp_state::ef934x_extram_address, values from an independent evaluation
     * of MAME's expression */
    {
        static const struct { uint8_t a, b, slice; uint16_t addr; } t[] = {
            {0x00, 0xa0, 0, 0x080}, {0x00, 0xa0, 1, 0x280}, {0x00, 0xa0, 9, 0x220}, {0x00, 0xff, 0, 0x19f},
            {0x80, 0xa0, 0, 0x480}, {0x80, 0xff, 9, 0x67f}, {0x07, 0xc5, 3, 0x325}, {0x00, 0xa5, 8, 0x025},
            {0x00, 0xe5, 9, 0x265}, {0x40, 0xbf, 7, 0x2ff}, {0x00, 0xa0, 12, 0x020}, {0x00, 0xa0, 15, 0x220},
        };
        static uint8_t seen[EF9340_EXRAM_SIZE];

        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
            CHECK_EQ(ef9340_exram_address((uint32_t)t[i].a << 12 | (uint32_t)t[i].b << 4 | t[i].slice), t[i].addr);

        /* the 192 extended glyphs x 10 slices never share a byte */
        memset(seen, 0, sizeof seen);
        for (unsigned a7 = 0; a7 <= 0x80; a7 += 0x80)
            for (unsigned b = 0xa0; b < 0x100; b++)
                for (unsigned s = 0; s < 10; s++)
                {
                    const unsigned ad = ef9340_exram_address(a7 << 12 | b << 4 | s);

                    CHECK(ad < EF9340_EXRAM_SIZE && !seen[ad], "extended RAM 0x%03x reused", ad);
                    seen[ad] = 1;
                }
    }
}

static void test_interface(void)
{
    ef9340_t *e = &g_e;

    fresh(e, g_font);
    CHECK_EQ(ef9340_read(e, 0, 1, 0), 0);       /* command A: busy flag, never set */
    CHECK_EQ(ef9340_read(e, 0, 1, 1), 0);

    /* the command registers: TA is latched, TB executes */
    cmd(e, 0x27, 0x00);                         /* begin row: X = 0, Y = TA & 1f */
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 7);
    cmd(e, 0x3d, 0x20);                         /* load Y */
    CHECK_EQ(e->y, 0x1d);
    cmd(e, 0xff, 0x40);                         /* load X: & 3f */
    CHECK_EQ(e->x, 0x3f);
    cmd(e, 0xa5, 0x80);                         /* load M */
    CHECK_EQ(e->m, 0xa5);
    cmd(e, 0xc3, 0xa0);                         /* load R */
    CHECK_EQ(e->r, 0xc3);
    cmd(e, 0xff, 0xc0);                         /* load Y0: & 3f */
    CHECK_EQ(e->y0, 0x3f);
    cmd(e, 0x12, 0xe0);                         /* not interpreted */
    CHECK_EQ(e->y0, 0x3f);
    CHECK_EQ(e->tb, 0xe0);
    CHECK_EQ(ef9340_read(e, 0, 0, 0), 0x12);    /* data A reads TA */
    /* only the top three bits of TB select the command */
    cmd(e, 0x05, 0x3f);
    CHECK_EQ(e->y, 5);

    /* write + increment: a row of 40, then the next row, 24 rows then 0 */
    cmd(e, 0, 0x80);                            /* M = 0 */
    cmd(e, 22, 0x00);                           /* row 22 */
    for (int i = 0; i < 40; i++)
        dat(e, (uint8_t)(0x10 + i), (uint8_t)(0x40 + i));
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 23);
    CHECK_EQ(e->ram_a[ef9340_cell_address(7, 22)], 0x17);
    CHECK_EQ(e->ram_b[ef9340_cell_address(7, 22)], 0x47);
    CHECK_EQ(e->ram_a[ef9340_cell_address(39, 22)], 0x10 + 39);
    for (int i = 0; i < 40; i++)
        dat(e, 1, 2);
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 0);                          /* 24 wraps to 0 */

    /* the service row (31) wraps through (31 + 1) & 1f = 0 */
    cmd(e, 31, 0x00);
    for (int i = 0; i < 40; i++)
        dat(e, 3, (uint8_t)i);
    CHECK_EQ(e->y, 0);
    CHECK_EQ(e->ram_b[ef9340_cell_address(9, 31)], 9);

    /* increment C: wraps at 40, 48, 56 and 64 only */
    cmd(e, 3, 0x20);                            /* Y = 3 */
    cmd(e, 39, 0x40);
    cmd(e, 0, 0x60);
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 4);
    cmd(e, 47, 0x40);
    cmd(e, 0, 0x60);
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 5);
    cmd(e, 41, 0x40);
    cmd(e, 0, 0x60);
    CHECK_EQ(e->x, 42);                         /* a column past 40 keeps counting */
    CHECK_EQ(e->y, 5);
    cmd(e, 63, 0x40);
    cmd(e, 0, 0x60);
    CHECK_EQ(e->x, 0);
    CHECK_EQ(e->y, 6);

    /* write without increment */
    cmd(e, 0x40, 0x80);
    cmd(e, 2, 0x20);
    cmd(e, 4, 0x40);
    dat(e, 0x77, 0x66);
    CHECK_EQ(e->x, 4);
    CHECK_EQ(e->y, 2);
    CHECK_EQ(e->ram_a[ef9340_cell_address(4, 2)], 0x77);
    CHECK_EQ(e->ram_b[ef9340_cell_address(4, 2)], 0x66);
    /* a read mode ignores a data write */
    cmd(e, 0x20, 0x80);
    dat(e, 0x11, 0x22);
    CHECK_EQ(e->ram_a[ef9340_cell_address(4, 2)], 0x77);

    /* read + increment: TB is read first (the previous byte), the cell is
     * fetched into TA and TB by that read, TA is then read separately */
    cmd(e, 4, 0x40);
    cmd(e, 2, 0x20);
    CHECK_EQ(ef9340_read(e, 0, 0, 1), 0x20);    /* the old TB: the last command byte */
    CHECK_EQ(e->x, 5);
    CHECK_EQ(ef9340_read(e, 0, 0, 0), 0x77);    /* TA: the cell's attribute */
    CHECK_EQ(e->tb, 0x66);
    /* read without increment */
    cmd(e, 0x60, 0x80);
    cmd(e, 4, 0x40);
    (void)ef9340_read(e, 0, 0, 1);
    CHECK_EQ(e->x, 4);
    CHECK_EQ(ef9340_read(e, 0, 0, 1), 0x66);
    CHECK_EQ(e->x, 4);

    /* slices: write slice goes to the extended RAM for b >= a0, M's low
     * nibble counts 0..9 */
    cmd(e, 0x40, 0x80);
    cmd(e, 6, 0x40);
    cmd(e, 10, 0x20);
    dat(e, 0x00, 0xa5);
    cmd(e, 0x80, 0x80);
    for (int s = 0; s < 10; s++)
    {
        CHECK_EQ(e->m, 0x80 | s);
        dat(e, (uint8_t)(0xc0 + s), 0);
    }
    CHECK_EQ(e->m, 0x80);                       /* (9 + 1) % 10 */
    for (int s = 0; s < 10; s++)
        CHECK_EQ(ef9340_exram_r(e, 0x00u << 12 | 0xa5u << 4 | (unsigned)s), 0xc0 + s);
    cmd(e, 0x8e, 0x80);                         /* slice 14 -> (14 + 1) % 10 = 5 */
    dat(e, 0x99, 0);
    CHECK_EQ(e->m, 0x85);
    CHECK_EQ(ef9340_exram_r(e, 0x00u << 12 | 0xa5u << 4 | 14u), 0x99);
    /* slices 8-15 share storage in pairs (address bits 5-6 come from the
     * character, not the slice, once slice bit 3 is set): 14 is slice 8 */
    CHECK_EQ(ef9340_exram_r(e, 0x00u << 12 | 0xa5u << 4 | 8u), 0x99);
    cmd(e, 0x88, 0x80);
    dat(e, 0xc8, 0);                            /* put slice 8 back */
    /* a cell with b < a0 stores nothing */
    {
        uint8_t before[EF9340_EXRAM_SIZE];

        memcpy(before, e->exram, sizeof before);
        cmd(e, 0x40, 0x80);
        cmd(e, 7, 0x40);
        dat(e, 0x00, 0x9f);
        cmd(e, 0x80, 0x80);
        dat(e, 0xee, 0);
        CHECK(memcmp(before, e->exram, sizeof before) == 0, "slice write into a non-extended cell changed the RAM");
        CHECK_EQ(e->m, 0x81);
    }

    /* read slice: font for b < 80, extended RAM for b >= a0, ff between */
    cmd(e, 0x40, 0x80);
    cmd(e, 8, 0x40);
    dat(e, 0x00, 0x41);
    cmd(e, 9, 0x40);
    dat(e, 0x80, 0x41);                         /* a bit 7 selects the graphics half of the font */
    cmd(e, 10, 0x40);
    dat(e, 0x00, 0x85);
    cmd(e, 11, 0x40);
    dat(e, 0x00, 0xa5);
    for (int s = 0; s < 10; s++)
    {
        cmd(e, 8, 0x40);
        cmd(e, 0xa0 | s, 0x80);
        (void)ef9340_read(e, 0, 0, 1);
        CHECK_EQ(e->ta, g_font[0x41 * 10 + s]);
        CHECK_EQ(e->tb, 0xff);
        CHECK_EQ(e->m, 0xa0 | ((s + 1) % 10));
        CHECK_EQ(ef9340_read(e, 0, 0, 0), g_font[0x41 * 10 + s]);

        cmd(e, 9, 0x40);
        cmd(e, 0xa0 | s, 0x80);
        (void)ef9340_read(e, 0, 0, 1);
        CHECK_EQ(e->ta, g_font[0xc1 * 10 + s]);

        cmd(e, 10, 0x40);
        cmd(e, 0xa0 | s, 0x80);
        (void)ef9340_read(e, 0, 0, 1);
        CHECK_EQ(e->ta, 0xff);

        cmd(e, 11, 0x40);
        cmd(e, 0xa0 | s, 0x80);
        (void)ef9340_read(e, 0, 0, 1);
        CHECK_EQ(e->ta, 0xc0 + s);
    }
    cmd(e, 8, 0x40);
    cmd(e, 0xaa, 0x80);                         /* slice 10: no font data */
    (void)ef9340_read(e, 0, 0, 1);
    CHECK_EQ(e->ta, 0xff);
    CHECK_EQ(e->m, 0xa1);

    /* io(): A2 is R/W, and a MOVX of the other direction acts too */
    fresh(e, g_font);
    CHECK_EQ(ef9340_io(e, 0, 2, 0x31), 0x31);   /* write command A */
    CHECK_EQ(e->ta, 0x31);
    CHECK_EQ(ef9340_io(e, 0, 3, 0x20), 0x20);   /* write command B: load Y = 0x11 */
    CHECK_EQ(e->y, 0x11);
    CHECK_EQ(ef9340_io(e, 0, 4, 0xff), 0x31);   /* read data A */
    CHECK_EQ(ef9340_io(e, 0, 4, 0x0f), 0x01);   /* ... and'ed with the bus */
    CHECK_EQ(ef9340_io(e, 0, 6, 0xff), 0x00);   /* read command A */
    CHECK_EQ(ef9340_io(e, 0, 0, 0x55), 0x55);
    CHECK_EQ(e->ta, 0x55);
    {
        /* a write at an A2=1 address reads (here with a side effect) and
         * leaves the data alone */
        cmd(e, 0x20, 0x80);
        cmd(e, 0, 0x40);
        cmd(e, 0, 0x20);
        CHECK_EQ(ef9340_io(e, 0, 5, 0xaa), 0x20);   /* TB (0x20, the last command) & the bus */
    }
}

/* ======================================================================
 * time and rendering
 * ==================================================================== */

static uint8_t px(const ef9340_t *e, int x, int y)
{
    return e->layer[(size_t)y * EF9340_LAYER_STRIDE + (size_t)x];
}

static void run_frames(ef9340_t *e, unsigned n)
{
    ef9340_run_until(e, (uint64_t)n * EF9340_FRAME_CLOCKS);
}

/* Put cell (x, y) = (a, b) through the interface. */
static void put(ef9340_t *e, int x, int y, uint8_t a, uint8_t b)
{
    cmd(e, 0x40, 0x80);
    cmd(e, (uint8_t)x, 0x40);
    cmd(e, (uint8_t)y, 0x20);
    dat(e, a, b);
}

static void set_r(ef9340_t *e, uint8_t r)
{
    cmd(e, r, 0xa0);
}

static void set_y0(ef9340_t *e, uint8_t v)
{
    cmd(e, v, 0xc0);
}

static void set_cursor(ef9340_t *e, int x, int y)
{
    cmd(e, (uint8_t)x, 0x40);
    cmd(e, (uint8_t)y, 0x20);
}

/* Screen row and column of a page cell's pixel: the text area starts at
 * (15, 5); the service row occupies lines 0-9 of it, page row n lines
 * 10 + 10n.. */
static int cell_x(int x, int bit)
{
    return EF9340_OFFSET_X + x * 8 + bit;
}

static int cell_y(int row, int slice)
{
    return EF9340_OFFSET_Y + 10 + row * 10 + slice;
}

/* A font with just enough in it to read the picture: glyph g, slice s has
 * pixels (bit n set) wherever (g + s + n) is a multiple of 3 -- a different
 * pattern per glyph and slice. */
static uint8_t g_dfont[EF9340_FONT_SIZE];

static void make_dfont(void)
{
    for (unsigned g = 0; g < 256; g++)
        for (unsigned s = 0; s < 10; s++)
        {
            uint8_t v = 0;

            for (unsigned n = 0; n < 8; n++)
                if ((g + s + n) % 3 == 0)
                    v |= (uint8_t)(1u << n);
            g_dfont[g * 10 + s] = v;
        }
}

static void test_timing(void)
{
    ef9340_t *e = &g_e;

    /* the beam */
    CHECK_EQ(ef9340_vpos(0), 243);
    CHECK_EQ(ef9340_hpos(0), 0);
    CHECK_EQ(ef9340_vpos(455), 243);
    CHECK_EQ(ef9340_vpos(456), 244);
    CHECK_EQ(ef9340_hpos(457), 1);
    CHECK_EQ(ef9340_vpos(70 * 456 - 1), 312);
    CHECK_EQ(ef9340_vpos(70 * 456), 0);
    CHECK_EQ(ef9340_vpos(EF9340_FRAME_CLOCKS - 1), 242);
    CHECK_EQ(ef9340_vpos(EF9340_FRAME_CLOCKS), 243);
    CHECK_EQ(EF9340_FRAME_CLOCKS, 142728);

    /* power-on at 0: the first line is line 0, 70 lines in; the blink counter
     * is armed at the same clock */
    fresh(e, g_dfont);
    CHECK_EQ(e->line_next, 70u * 456);
    CHECK_EQ(e->blink_next, 70u * 456);
    CHECK_EQ(ef9340_next_event(e), 70u * 456);

    /* a reset at a clock inside a frame arms the next line 0 */
    ef9340_reset(e, 5 * 456 + 17);
    CHECK_EQ(e->line_next, 5u * 456 + 17 + (70u * 456 - (5u * 456 + 17)));
    ef9340_reset(e, 70u * 456);                     /* at line 0 itself: next frame */
    CHECK_EQ(e->line_next, 70u * 456 + EF9340_FRAME_CLOCKS);
    ef9340_reset(e, 0);

    /* nothing before line 0; the lines before the text area's first line
     * (screen row 5) draw nothing; one frame is 313 lines of which the first
     * 5 are above the text area */
    ef9340_run_until(e, 70 * 456 - 1);
    CHECK_EQ(e->lines_drawn, 0);
    ef9340_run_until(e, 70 * 456 + 4 * 456);
    CHECK_EQ(e->lines_drawn, 0);
    ef9340_run_until(e, 70 * 456 + 5 * 456 - 1);
    CHECK_EQ(e->lines_drawn, 0);
    ef9340_run_until(e, 70 * 456 + 5 * 456);
    CHECK_EQ(e->lines_drawn, 1);
    ef9340_run_until(e, 70u * 456 + 312u * 456);
    CHECK_EQ(e->lines_drawn, 308);
    CHECK_EQ(e->line_next, 70u * 456 + 313u * 456);
    CHECK_EQ(e->blink_next, 70u * 456 + EF9340_FRAME_CLOCKS);

    /* the line draws what the registers say at its own clock: display on in
     * the middle of the frame shows from that line down */
    fresh(e, g_dfont);
    set_r(e, 0x08);                                 /* service row only, display off */
    put(e, 0, 0, 7, 'A');
    {
        const uint64_t t = 70u * 456 + 100u * 456 + 200;    /* inside line 100 */

        ef9340_run_until(e, t - 1);
        CHECK_EQ(px(e, 20, 100), 0);
        CHECK_EQ(px(e, 20, 99), 0);
        set_r(e, 0x09);
        /* ef9340_write() runs the chip to the access first: line 100 was
         * drawn at 100 * 456 + 70 * 456, before this write */
        ef9340_write(e, t, 1, 0, 0x09);
        ef9340_write(e, t, 1, 1, 0xa0);
        ef9340_run_until(e, 70u * 456 + 120u * 456);
        CHECK_EQ(px(e, 20, 100), 0);                /* line 100 was drawn with the display off */
        CHECK_EQ(px(e, 20, 101), 8 | 0);            /* line 101: opaque (bg 0) */
        CHECK_EQ(px(e, 14, 101), 0);
        CHECK_EQ(px(e, 335, 101), 0);
        CHECK_EQ(px(e, 334, 101), 8);
    }

    /* events due at or before an access's clock fire before it: a write AT a
     * line's start comes after that line's draw, one clock earlier before it */
    {
        const uint64_t line100 = 70u * 456 + 100u * 456;

        fresh(e, g_dfont);
        set_r(e, 0x01);
        ef9340_write(e, line100, 1, 0, 0x00);
        ef9340_write(e, line100, 1, 1, 0xa0);
        ef9340_run_until(e, line100 + 456);
        CHECK_EQ(px(e, 20, 100), 8);                /* drawn with the display on */
        CHECK_EQ(px(e, 20, 101), 0);

        fresh(e, g_dfont);
        set_r(e, 0x01);
        ef9340_write(e, line100 - 1, 1, 0, 0x00);
        ef9340_write(e, line100 - 1, 1, 1, 0xa0);
        ef9340_run_until(e, line100 + 456);
        CHECK_EQ(px(e, 20, 99), 8);
        CHECK_EQ(px(e, 20, 100), 0);                /* the new R was in place for line 100 */
    }

    /* running in one step or in many gives the same layer and counters */
    {
        static ef9340_t a, b;

        fresh(&a, g_dfont);
        fresh(&b, g_dfont);
        for (ef9340_t *c = &a; c != NULL; c = (c == &a) ? &b : NULL)
        {
            set_r(c, 0x59);
            set_y0(c, 0x23);
            for (int y = 0; y < 24; y++)
                put(c, 3, y, (uint8_t)(0x11 * (y & 7)), (uint8_t)(0x20 + y));
        }
        ef9340_run_until(&a, 7u * EF9340_FRAME_CLOCKS + 12345);
        for (uint64_t t = 0; t <= 7u * EF9340_FRAME_CLOCKS + 12345; t += 37)
            ef9340_run_until(&b, t);
        ef9340_run_until(&b, 7u * EF9340_FRAME_CLOCKS + 12345);
        CHECK(memcmp(a.layer, b.layer, sizeof a.layer) == 0, "layer differs between one run and many");
        CHECK_EQ(a.lines_drawn, b.lines_drawn);
        CHECK_EQ(a.blink, b.blink);
        CHECK_EQ(a.blink_prescaler, b.blink_prescaler);
        CHECK_EQ(a.h_parity, b.h_parity);
        CHECK_EQ(a.line_next, b.line_next);
        CHECK_EQ(ef9340_next_event(&a), ef9340_next_event(&b));
    }
}

static void test_debug(void)
{
    ef9340_t *e = &g_e;
    static ef9340_snapshot_t s;
    static uint8_t vis[EF9340_VIS_W * EF9340_VIS_H];
    static uint32_t rgb[EF9340_VIS_W * EF9340_VIS_H];

    fresh(e, g_dfont);
    set_r(e, 0xd9);                                 /* display, service row, 625 lines, blink, no cursor */
    set_y0(e, 0x2b);
    put(e, 1, 2, 0x25, 'Q');                        /* alphanumeric, double width, fg 5 */
    put(e, 2, 2, 0x4a, 0xa7);                       /* inverted, extended code, bit 3 */
    put(e, 3, 2, 0x80 | 0x20 | 0x03, 0x41);         /* graphics */
    put(e, 4, 2, 0x31, 0x83);                       /* delimiter */
    put(e, 5, 2, 0x82, 0x90);                       /* illegal */
    put(e, 33, 4, 0x07, 'R');                       /* column >= 32 */
    put(e, 6, 31, 0x01, 'S');                       /* service row */
    ef9340_exram_w(e, 0x12u << 12 | 0xa7u << 4 | 4u, 0x5c);
    cmd(e, 9, 0x40);
    cmd(e, 9, 0x20);
    cmd(e, 0xa3, 0x80);
    run_frames(e, 40);
    ef9340_snapshot(e, &s);
    CHECK_EQ(s.x, 9);
    CHECK_EQ(s.y, 9);
    CHECK_EQ(s.r, 0xd9);
    CHECK_EQ(s.display_on, 1);
    CHECK_EQ(s.box_mode, 0);
    CHECK_EQ(s.service_row, 1);
    CHECK_EQ(s.cursor_on, 1);
    CHECK_EQ(s.lines625, 1);
    CHECK_EQ(s.blink_enabled, 1);
    CHECK_EQ(s.first_row, 0x0b);
    CHECK_EQ(s.zoom, 1);
    CHECK_EQ(s.mode, 0xa0);
    CHECK_EQ(s.slice, 3);
    CHECK(strcmp(s.mode_name, "read slice") == 0, "mode name %s", s.mode_name);
    CHECK_EQ(s.blink_prescaler, 16);                /* 40 frames, cut at 24 */
    CHECK_EQ(s.blink, 1);
    CHECK_EQ(s.cursor_visible, 1);
    CHECK_EQ(s.page[2][1].kind, EF9340_CELL_ALNUM);
    CHECK_EQ(s.page[2][1].fg, 5);
    CHECK_EQ(s.page[2][1].glyph, 'Q');
    CHECK_EQ(s.page[2][1].flags, EF9340_ATTR_DOUBLE_WIDTH);
    CHECK_EQ(s.page[2][2].kind, EF9340_CELL_ALNUM);
    CHECK_EQ(s.page[2][2].flags, EF9340_ATTR_INVERTED | EF9340_ATTR_NO_BLINK | EF9340_ATTR_EXTENDED);
    CHECK_EQ(s.page[2][2].glyph, 0xa7);
    CHECK_EQ(s.page[2][3].kind, EF9340_CELL_GRAPHIC);
    CHECK_EQ(s.page[2][3].fg, 3);
    CHECK_EQ(s.page[2][3].bg, 2);
    CHECK_EQ(s.page[2][3].glyph, 0xc1);
    CHECK_EQ(s.page[2][4].kind, EF9340_CELL_DELIMITER);
    CHECK_EQ(s.page[2][4].bg, 3);
    CHECK_EQ(s.page[2][5].kind, EF9340_CELL_ILLEGAL);
    CHECK_EQ(s.page[4][33].b, 'R');
    CHECK_EQ(s.service[6].b, 'S');
    CHECK_EQ(s.service[6].a, 0x01);
    CHECK_EQ(s.font_present, 1);
    CHECK_EQ(s.exram[ef9340_exram_address(0x12u << 12 | 0xa7u << 4 | 4u)], 0x5c);
    CHECK(memcmp(s.ram_a, e->ram_a, sizeof s.ram_a) == 0, "ram_a copy");
    CHECK_EQ(s.lines_drawn, e->lines_drawn);

    /* the layer: copy and colour view agree with the rows */
    ef9340_layer_copy(e, vis);
    ef9340_layer_rgb(e, rgb, NULL);
    for (int y = 0; y < EF9340_VIS_H; y += 7)
        for (int x = 0; x < EF9340_VIS_W; x += 5)
        {
            CHECK_EQ(vis[y * EF9340_VIS_W + x], px(e, x, y));
            CHECK_EQ(rgb[y * EF9340_VIS_W + x], ef9340_debug_color(px(e, x, y)));
        }
    CHECK_EQ(ef9340_debug_color(8 | 5), 0xff00ff);
    CHECK_EQ(ef9340_debug_color(8), 0x000000);
    CHECK_EQ(ef9340_debug_color(0), 0x202020);

    /* decode of a lone cell */
    {
        ef9340_cell_t c;

        ef9340_decode_cell(0x10 | 7, 0x41, &c);
        CHECK_EQ(c.flags, EF9340_ATTR_DOUBLE_HEIGHT);
        CHECK_EQ(c.glyph, 0x41);
        ef9340_decode_cell(0x80 | 0x08 | 7, 0xff, &c);
        CHECK_EQ(c.kind, EF9340_CELL_GRAPHIC);
        CHECK_EQ(c.flags, EF9340_ATTR_NO_BLINK | EF9340_ATTR_EXTENDED);
    }

    /* a power-on reset clears everything MAME zero-fills */
    ef9340_reset(e, 5 * EF9340_FRAME_CLOCKS);
    ef9340_snapshot(e, &s);
    CHECK_EQ(s.r, 0);
    CHECK_EQ(s.page[2][1].b, 0);
    CHECK_EQ(s.exram[0], 0);
    CHECK_EQ(px(e, 40, 40), 0);
    CHECK_EQ(e->line_next, 5u * EF9340_FRAME_CLOCKS + 70u * 456);
}

static void test_blink_counter(void)
{
    ef9340_t *e = &g_e;

    /* 525-line mode: toggles every 32 frames; the state is that after the
     * frame's line 0 */
    fresh(e, g_dfont);
    run_frames(e, 31);
    CHECK_EQ(e->blink_prescaler, 31);
    CHECK_EQ(e->blink, 0);
    run_frames(e, 32);
    CHECK_EQ(e->blink_prescaler, 0);
    CHECK_EQ(e->blink, 1);
    run_frames(e, 64);
    CHECK_EQ(e->blink, 0);
    run_frames(e, 96);
    CHECK_EQ(e->blink, 1);

    /* 625-line mode (R bit 6): the prescaler is cut at 24 */
    fresh(e, g_dfont);
    set_r(e, 0x40);
    run_frames(e, 23);
    CHECK_EQ(e->blink_prescaler, 23);
    CHECK_EQ(e->blink, 0);
    run_frames(e, 24);
    CHECK_EQ(e->blink_prescaler, 0);
    CHECK_EQ(e->blink, 1);
    run_frames(e, 48);
    CHECK_EQ(e->blink, 0);
    CHECK_EQ(e->blink_prescaler, 0);
    run_frames(e, 72);
    CHECK_EQ(e->blink, 1);

    /* the line timer fires before the blink counter on the same clock: the
     * first line of a frame is drawn with the old blink state */
    fresh(e, g_dfont);
    set_r(e, 0x81 | 0x08);              /* blink enabled, display on, service row */
    put(e, 0, 0, 0x07, 'A');
    cmd(e, 0x00, 0xc0);
    /* run to frame 32's line 0 minus one clock, then through it: line 0 is above
     * the text area, so look at the bookkeeping instead */
    run_frames(e, 31);
    ef9340_run_until(e, 31u * EF9340_FRAME_CLOCKS + 70u * 456 - 1);
    CHECK_EQ(e->blink, 0);
    ef9340_run_until(e, 31u * EF9340_FRAME_CLOCKS + 70u * 456);
    CHECK_EQ(e->blink, 1);
    CHECK_EQ(e->blink_prescaler, 0);
}

static void test_render(void)
{
    ef9340_t *e = &g_e;
    const uint8_t FG = 5;

    /* --- placement: the service row, the page, the borders --- */
    fresh(e, g_dfont);
    set_r(e, 0x09);                                 /* display + service row, 525 lines */
    put(e, 0, 0, FG, 'A');
    put(e, 39, 19, 0x03, 'B');
    put(e, 5, 31, 0x04, 'C');                       /* the service row's cell 5 */
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
    {
        for (int bit = 0; bit < 8; bit++)
        {
            const int on = (g_dfont['A' * 10 + s] >> bit) & 1;

            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)), on ? (8 | FG) : 8);
            CHECK_EQ(px(e, cell_x(39, bit), cell_y(19, s)), ((g_dfont['B' * 10 + s] >> bit) & 1) ? (8 | 3) : 8);
            CHECK_EQ(px(e, cell_x(5, bit), EF9340_OFFSET_Y + s), ((g_dfont['C' * 10 + s] >> bit) & 1) ? (8 | 4) : 8);
        }
    }
    /* column 15 is the first, 334 the last; nothing outside */
    CHECK_EQ(px(e, 14, cell_y(0, 0)), 0);
    CHECK_EQ(px(e, 335, cell_y(0, 0)), 0);
    CHECK_EQ(px(e, 14, EF9340_OFFSET_Y), 0);
    CHECK_EQ(px(e, 15, EF9340_OFFSET_Y + 0), 8 | (((g_dfont[0 * 10] & 1)) ? 0 : 0));     /* cell 0 of the service row is blank (A=0, B=0): glyph 0 */
    /* rows above the text area are never drawn */
    for (int y = 0; y < EF9340_OFFSET_Y; y++)
        CHECK_EQ(px(e, 100, y), 0);
    /* 210 lines of text: screen rows 5 .. 214; row 215 is blank (display
     * automaton off) */
    CHECK_EQ(px(e, 20, 214) & 8, 8);
    CHECK_EQ(px(e, 20, 215), 0);
    CHECK_EQ(px(e, 20, 242), 0);

    /* without the service row: the first 10 lines are opaque black */
    set_r(e, 0x01);
    run_frames(e, 3);
    for (int y = EF9340_OFFSET_Y; y < EF9340_OFFSET_Y + 10; y++)
        CHECK_EQ(px(e, 100, y), 8);
    CHECK_EQ(px(e, cell_x(5, 0), cell_y(0, 0)) & 8, 8);

    /* display off: transparent everywhere */
    set_r(e, 0x08);
    run_frames(e, 4);
    CHECK_EQ(px(e, 100, 100), 0);
    CHECK_EQ(px(e, 100, EF9340_OFFSET_Y + 3), 0);

    /* 625 lines: 250 lines of text, rows to 254 */
    set_r(e, 0x49);
    run_frames(e, 5);
    CHECK_EQ(px(e, 20, 214), 8);
    CHECK_EQ(px(e, 20, 242) & 8, 8);
    CHECK_EQ(e->layer[(size_t)254 * EF9340_LAYER_STRIDE + 20] & 8, 8);
    CHECK_EQ(e->layer[(size_t)255 * EF9340_LAYER_STRIDE + 20], 0);

    /* --- Y0: the first page row; zoom doubles the line height --- */
    fresh(e, g_dfont);
    set_r(e, 0x01);
    for (int y = 0; y < 24; y++)
        put(e, 0, y, 7, (uint8_t)(0x30 + y));
    set_y0(e, 5);
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        CHECK_EQ(px(e, cell_x(0, 0), cell_y(0, s)) & 1, (g_dfont[(0x30 + 5) * 10 + s] & 1) ? 1 : 0);
    /* row 19 is followed by row 0 */
    for (int s = 0; s < 10; s++)
        CHECK_EQ(px(e, cell_x(0, 1), cell_y(19, s)) & 1, (g_dfont[(0x30 + 0) * 10 + s] >> 1 & 1));
    set_y0(e, 0x20 | 2);
    run_frames(e, 3);
    for (int s = 0; s < 10; s++)
        for (int z = 0; z < 2; z++)
            CHECK_EQ(px(e, cell_x(0, 2), cell_y(0, 2 * s + z)) & 1,
                     (g_dfont[(0x30 + 2) * 10 + s] >> 2 & 1) ? 1 : 0);
    /* the second page row starts 20 lines down in zoom */
    for (int s = 0; s < 10; s++)
        CHECK_EQ(px(e, cell_x(0, 3), EF9340_OFFSET_Y + 10 + 20 + 2 * s) & 1,
                 (g_dfont[(0x30 + 3) * 10 + s] >> 3 & 1));

    /* --- double width: the glyph's bits doubled, the second cell shows the
     * upper half; a lone double-width cell is cut after 8 pixels --- */
    fresh(e, g_dfont);
    set_r(e, 0x01);
    put(e, 3, 0, 0x20 | 7, 'D');
    put(e, 4, 0, 0x20 | 7, 'E');                    /* the second half (its own glyph unused) */
    put(e, 6, 0, 0x20 | 2, 'F');
    put(e, 7, 0, 0x00 | 2, 'G');                    /* a normal cell after a lone half */
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
    {
        const uint8_t d = g_dfont['D' * 10 + s];

        for (int bit = 0; bit < 8; bit++)
        {
            CHECK_EQ(px(e, cell_x(3, bit), cell_y(0, s)) & 7, ((d >> (bit / 2)) & 1) ? 7 : 0);
            CHECK_EQ(px(e, cell_x(4, bit), cell_y(0, s)) & 7, ((d >> (4 + bit / 2)) & 1) ? 7 : 0);
        }
        /* the pair after a lone double-width F: F's lower half, then G normally */
        for (int bit = 0; bit < 8; bit++)
        {
            const uint8_t f = g_dfont['F' * 10 + s];
            const uint8_t g = g_dfont['G' * 10 + s];

            CHECK_EQ(px(e, cell_x(6, bit), cell_y(0, s)) & 7, ((f >> (bit / 2)) & 1) ? 2 : 0);
            /* cell 7 is double width too? no: attribute 2 has no 0x20 bit; it shows G */
            CHECK_EQ(px(e, cell_x(7, bit), cell_y(0, s)) & 7, ((g >> bit) & 1) ? 2 : 0);
        }
    }

    /* --- double height: the top half in one row, the bottom in the next --- */
    fresh(e, g_dfont);
    set_r(e, 0x01);
    put(e, 1, 0, 0x10 | 3, 'H');
    put(e, 1, 1, 0x10 | 3, 'H');
    put(e, 2, 2, 0x10 | 3, 'H');                    /* alone: top half, then nothing special */
    put(e, 2, 3, 0x03, 'H');
    run_frames(e, 2);
    {
        static const int top[10] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4};
        static const int bot[10] = {4, 5, 5, 6, 6, 7, 7, 8, 8, 9};

        for (int s = 0; s < 10; s++)
            for (int bit = 0; bit < 8; bit++)
            {
                CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, s)) & 7,
                         ((g_dfont['H' * 10 + top[s]] >> bit) & 1) ? 3 : 0);
                CHECK_EQ(px(e, cell_x(1, bit), cell_y(1, s)) & 7,
                         ((g_dfont['H' * 10 + bot[s]] >> bit) & 1) ? 3 : 0);
                CHECK_EQ(px(e, cell_x(2, bit), cell_y(2, s)) & 7,
                         ((g_dfont['H' * 10 + top[s]] >> bit) & 1) ? 3 : 0);
                /* the next row has no double-height cell: parity was reset, normal glyph */
                CHECK_EQ(px(e, cell_x(2, bit), cell_y(3, s)) & 7,
                         ((g_dfont['H' * 10 + s] >> bit) & 1) ? 3 : 0);
            }
    }

    /* --- cursor, inversion and blink --- */
    fresh(e, g_dfont);
    set_r(e, 0x11);                                 /* display + cursor */
    put(e, 4, 2, 6, 'K');
    put(e, 5, 2, 0x40 | 6, 'K');                    /* inverted attribute */
    set_cursor(e, 4, 2);
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            const int on = (g_dfont['K' * 10 + s] >> bit) & 1;

            CHECK_EQ(px(e, cell_x(4, bit), cell_y(2, s)) & 7, on ? 0 : 6);   /* inverted: fg <-> bg(0) */
            CHECK_EQ(px(e, cell_x(5, bit), cell_y(2, s)) & 7, on ? 0 : 6);   /* attribute inversion */
        }
    set_cursor(e, 5, 2);                            /* both inversions cancel */
    run_frames(e, 3);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            const int on = (g_dfont['K' * 10 + s] >> bit) & 1;

            CHECK_EQ(px(e, cell_x(4, bit), cell_y(2, s)) & 7, on ? 6 : 0);
            CHECK_EQ(px(e, cell_x(5, bit), cell_y(2, s)) & 7, on ? 6 : 0);
        }

    fresh(e, g_dfont);
    set_r(e, 0x81);                                 /* display + blink */
    put(e, 4, 2, 6, 'K');
    put(e, 5, 2, 6 | 0x08, 'K');                    /* a bit 3: does not blink */
    put(e, 6, 2, 6 | 0x40, 'K');                    /* inverted: blinks the other way round */
    run_frames(e, 2);                               /* blink = 0 */
    for (int bit = 0; bit < 8; bit++)
    {
        const int on = (g_dfont['K' * 10 + 3] >> bit) & 1;

        CHECK_EQ(px(e, cell_x(4, bit), cell_y(2, 3)) & 7, on ? 6 : 0);
        CHECK_EQ(px(e, cell_x(5, bit), cell_y(2, 3)) & 7, on ? 6 : 0);
        /* inverted attribute: its blink phase is the other way round, so now it
         * is the hidden glyph (all background) that gets inverted: solid */
        CHECK_EQ(px(e, cell_x(6, bit), cell_y(2, 3)) & 7, 6);
    }
    run_frames(e, 33);                              /* blink = 1 */
    CHECK_EQ(e->blink, 1);
    {
        int any4 = 0, any5 = 0, any6 = 0;

        for (int s = 0; s < 10; s++)
            for (int bit = 0; bit < 8; bit++)
            {
                any4 |= px(e, cell_x(4, bit), cell_y(2, s)) & 7;                 /* hidden: all bg */
                any5 |= (px(e, cell_x(5, bit), cell_y(2, s)) & 7) ? 1 : 0;
                any6 |= (px(e, cell_x(6, bit), cell_y(2, s)) & 7) ? 0 : 0;
            }
        CHECK_EQ(any4, 0);
        CHECK(any5, "a bit-3 cell was blanked by blink");
        (void)any6;
        /* the inverted cell, with the blink phase flipped, is drawn (inverted) */
        for (int bit = 0; bit < 8; bit++)
        {
            const int on = (g_dfont['K' * 10 + 3] >> bit) & 1;

            CHECK_EQ(px(e, cell_x(6, bit), cell_y(2, 3)) & 7, on ? 0 : 6);
        }
    }

    /* --- graphics, delimiters, boxing, conceal, underline --- */
    fresh(e, g_dfont);
    set_r(e, 0x01);
    put(e, 0, 0, 0x80 | (3 << 4) | 5, 0x41);            /* graphics: glyph 41 | 80, fg 5 on bg 3 */
    put(e, 1, 0, 0x80 | (3 << 4) | 5, 0x85);            /* illegal: solid, previous colours */
    put(e, 2, 0, 0x80 | (3 << 4) | 5, 0x00);
    put(e, 3, 0, (2 << 4) | 4, 0x80);                   /* delimiter: bg 2 */
    put(e, 4, 0, 0x01, 'M');                            /* alphanumeric after it: bg 2 */
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)) & 7,
                     ((g_dfont[0xc1 * 10 + s] >> bit) & 1) ? 5 : 3);
            CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, s)) & 7, 5);                   /* 0xff: all fg */
            CHECK_EQ(px(e, cell_x(2, bit), cell_y(0, s)) & 7,
                     ((g_dfont[0x80 * 10 + s] >> bit) & 1) ? 5 : 3);
            CHECK_EQ(px(e, cell_x(3, bit), cell_y(0, s)) & 7, 4);                   /* the delimiter is solid fg */
            CHECK_EQ(px(e, cell_x(4, bit), cell_y(0, s)) & 7,
                     ((g_dfont['M' * 10 + s] >> bit) & 1) ? 1 : 2);
        }

    /* boxing: with R bit 1 everything is transparent until a delimiter with
     * b bit 1 clear opens a box; conceal (R bit 2, b bit 0) blanks to the next
     * delimiter; b bit 2 underlines slice 9 */
    fresh(e, g_dfont);
    set_r(e, 0x03);
    put(e, 0, 0, 7, 'N');                               /* outside the box: transparent */
    put(e, 1, 0, 0x00, 0x82);                           /* delimiter with b bit 1 set: the box opens after it */
    put(e, 2, 0, 7, 'N');
    put(e, 3, 0, 0x00, 0x80);                           /* b bit 1 clear: it closes after it */
    put(e, 4, 0, 7, 'N');
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)), 0);
            CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, s)), 0);       /* the delimiter itself is still boxed out */
            CHECK_EQ(px(e, cell_x(2, bit), cell_y(0, s)), ((g_dfont['N' * 10 + s] >> bit) & 1) ? (8 | 7) : 8);
            CHECK_EQ(px(e, cell_x(3, bit), cell_y(0, s)), 8);       /* ... and the closing one is drawn: solid fg 0 */
            CHECK_EQ(px(e, cell_x(4, bit), cell_y(0, s)), 0);
        }

    fresh(e, g_dfont);
    set_r(e, 0x05);                                     /* conceal enabled, no boxing */
    put(e, 0, 0, 7, 'N');
    put(e, 1, 0, 0x00, 0x81);                           /* conceal on (b bit 0), box state: !(b & 2) && R & 2 = 0 */
    put(e, 2, 0, 7, 'N');
    put(e, 3, 0, 0x00, 0x80);                           /* conceal off */
    put(e, 4, 0, 7, 'N');
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)) & 7, ((g_dfont['N' * 10 + s] >> bit) & 1) ? 7 : 0);
            CHECK_EQ(px(e, cell_x(2, bit), cell_y(0, s)) & 7, 0);   /* concealed: pixels 0 but opaque */
            CHECK_EQ(px(e, cell_x(2, bit), cell_y(0, s)) & 8, 8);
            CHECK_EQ(px(e, cell_x(4, bit), cell_y(0, s)) & 7, ((g_dfont['N' * 10 + s] >> bit) & 1) ? 7 : 0);
        }

    fresh(e, g_dfont);
    set_r(e, 0x01);
    put(e, 0, 0, 0x00, 0x84);                           /* underline on */
    put(e, 1, 0, 6, 'N');
    run_frames(e, 2);
    for (int bit = 0; bit < 8; bit++)
    {
        CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, 9)) & 7, 6);               /* slice 9 solid */
        CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, 8)) & 7, ((g_dfont['N' * 10 + 8] >> bit) & 1) ? 6 : 0);
    }

    /* --- extended characters --- */
    fresh(e, g_dfont);
    set_r(e, 0x01);
    put(e, 0, 0, 0x05, 0xa5);
    put(e, 1, 0, 0x80 | 0x05, 0xa5);                    /* the other half of the extended RAM */
    for (int s = 0; s < 10; s++)
    {
        ef9340_exram_w(e, 0x00u << 12 | 0xa5u << 4 | (unsigned)s, (uint8_t)(0x11 * s + 3));
        ef9340_exram_w(e, 0x80u << 12 | 0xa5u << 4 | (unsigned)s, (uint8_t)(0x1d * s + 9));
    }
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
        {
            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)) & 7, (((0x11 * s + 3) & 0xff) >> bit & 1) ? 5 : 0);
            CHECK_EQ(px(e, cell_x(1, bit), cell_y(0, s)) & 7, (((0x1d * s + 9) & 0xff) >> bit & 1) ? 5 : 0);
        }

    /* --- no font: blank glyphs --- */
    fresh(e, NULL);
    set_r(e, 0x01);
    put(e, 0, 0, 7, 'A');
    run_frames(e, 2);
    for (int s = 0; s < 10; s++)
        for (int bit = 0; bit < 8; bit++)
            CHECK_EQ(px(e, cell_x(0, bit), cell_y(0, s)), 8);
    ef9340_set_font(e, g_dfont);
    run_frames(e, 3);
    CHECK(px(e, cell_x(0, 0), cell_y(0, 0)) != 0, "font installed after init is used");
}

/* ======================================================================
 * the mixer
 * ==================================================================== */

static void test_mixer(void)
{
    ef9340_mix_t m;
    int cx = -1;

    ef9340_mix_reset(&m);
    /* power-on tables 0xff/0xff: the 8245 wins on every colour; the EF9340's
     * pixels are dim and do not assert CX */
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 0, 1, &cx), 5);
    CHECK_EQ(cx, 0);
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 8, 1, &cx), 13);        /* LUM adds bit 3 */
    CHECK_EQ(ef9340_mix_pixel(&m, 0x0d, 8 | 3, 0, 1, NULL), 0x0d);  /* the 8245's own bit 3 stays */

    /* transparent EF9340 pixel with a Videopac+ cartridge (pin B high): the
     * 8245 decoder is off, the EF9340's black shows */
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 0, 0, 1, &cx), 0);
    CHECK_EQ(cx, 0);
    /* ... but with a plain cartridge (pin B low) the decoder is always on */
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 0, 0, 0, &cx), 5);

    /* the EF9340 wins where the 8245's colour has its bit clear */
    m.vdc = 0xff & ~(1u << 5);
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 0, 1, &cx), 3);
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 8, 1, &cx), 3);         /* LUM only brightens the 8245's input */
    CHECK_EQ(ef9340_mix_pixel(&m, 6, 8 | 3, 8, 1, &cx), 14);

    /* an EF9340 colour with its bit clear in `ef` is bright and drives CX */
    m.ef = 0xff & ~(1u << 3);
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 0, 1, &cx), 3 | 8);
    CHECK_EQ(cx, 1);
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 4, 0, 1, &cx), 4);
    CHECK_EQ(cx, 0);
    /* CX is computed for pixels the 8245 wins as well */
    m.vdc = 0xff;
    CHECK_EQ(ef9340_mix_pixel(&m, 5, 8 | 3, 0, 1, &cx), 5);
    CHECK_EQ(cx, 1);

    /* the span helper is the same loop */
    {
        static ef9340_t e;
        uint8_t vdc[EF9340_VIS_W], out[EF9340_VIS_W], cxr[EF9340_VIS_W];

        ef9340_init(&e, g_dfont);
        for (int x = 0; x < EF9340_VIS_W; x++)
        {
            vdc[x] = (uint8_t)(x & 15);
            e.layer[(size_t)50 * EF9340_LAYER_STRIDE + (size_t)x] = (uint8_t)((x * 7) & 15);
        }
        m.vdc = 0x5a;
        m.ef = 0xc3;
        ef9340_mix_span(&e, &m, 50, 20, 300, vdc, 8, 1, out, cxr);
        for (int x = 20; x < 300; x++)
        {
            int c = 0;
            const uint8_t want = ef9340_mix_pixel(&m, vdc[x], (uint8_t)((x * 7) & 15), 8, 1, &c);

            CHECK_EQ(out[x], want);
            CHECK_EQ(cxr[x], c);
        }
    }
}

/* ======================================================================
 * the MAME A/B
 * ==================================================================== */

typedef struct {
    const char *name;
    uint8_t seed1, seed2, amask, aor, bmask, bor, rval, y0, xcur, ycur, mfin, p1fin, vbg;
    uint8_t mix4, mix5, mix6, mix7, mixo, mixa;
    int pin_b;
    int nframes;
    int frames[4];
    uint8_t blink[4], prescaler[4];     /* MAME's blink state after the frame */
    uint32_t bands[4][25];              /* FNV-1a of the 10-row bands of the composited frame */
} scenario_t;

#define NBANDS 25

/* GOLDEN BEGIN (generated from the MAME runs by a script; see the comment at the top) */
static const scenario_t g_scenarios[] = {
    {"text", 0xa5, 0x3c, 0x07, 0x00, 0x5f, 0x20, 0x09, 0x00, 5, 3, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 1,
     {100},
     {1},
     {4},
     {
         {
             0x8cdf073au, 0x7813ead6u, 0x3dfee8b2u, 0x86114479u, 0x874923a7u,
             0xd07211a0u, 0x76257651u, 0x86a5d21eu, 0x9def47f2u, 0x0ba409e9u,
             0x46b8ef0eu, 0xa6752862u, 0x3a62e937u, 0x9898145au, 0x09ed2fd1u,
             0x7e92bf88u, 0xda7f402eu, 0x50671a3du, 0x504ab31eu, 0x82870c1bu,
             0x24ed74c0u, 0x56225dcfu, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
    {"attr", 0x6b, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x99, 0x00, 11, 6, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 3,
     {100, 132, 164},
     {1, 0, 1},
     {4, 4, 4},
     {
         {
             0x77cc373fu, 0x08baf5deu, 0x0eae2871u, 0x6afef7a3u, 0x6a271cf7u,
             0xc057dcd3u, 0x2f579d79u, 0x12368f6fu, 0x77c16378u, 0x5c050e2du,
             0x1936ce7eu, 0xf1b3da1du, 0xa32d0b49u, 0xb25b0daeu, 0x6e216415u,
             0x099df212u, 0x9c721241u, 0x0ee202d1u, 0xaed7e257u, 0x80c389e9u,
             0xaf50083eu, 0x8363591fu, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
         {
             0x7cb1e5d2u, 0x2749c7dau, 0x654e2f79u, 0xbffd4251u, 0x5c41e35du,
             0x097d3952u, 0x9f937c55u, 0x9fb7a1b4u, 0xe18275ddu, 0xa1fd32c7u,
             0x0e55d2fcu, 0x2adfbe07u, 0x4ffacd1au, 0xd4f10c3du, 0x0001ab8au,
             0x3938bc20u, 0x1a45ce73u, 0xbf8c7db0u, 0x2fa60cbeu, 0xaab5f566u,
             0x4d981b1du, 0x7d1dfbd6u, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
         {
             0x77cc373fu, 0x08baf5deu, 0x0eae2871u, 0x6afef7a3u, 0x6a271cf7u,
             0xc057dcd3u, 0x2f579d79u, 0x12368f6fu, 0x77c16378u, 0x5c050e2du,
             0x1936ce7eu, 0xf1b3da1du, 0xa32d0b49u, 0xb25b0daeu, 0x6e216415u,
             0x099df212u, 0x9c721241u, 0x0ee202d1u, 0xaed7e257u, 0x80c389e9u,
             0xaf50083eu, 0x8363591fu, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
    {"zoom", 0x13, 0x3c, 0x77, 0x00, 0xff, 0x00, 0x59, 0x27, 62, 2, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 1,
     {100},
     {0},
     {4},
     {
         {
             0xc7f5e13cu, 0x35b29928u, 0x2257d326u, 0xa436e9d5u, 0x19562285u,
             0x1cd7d219u, 0x0d0668eau, 0xb2ce352au, 0x89ed4f64u, 0x9ffaae12u,
             0xb4eb79bcu, 0xbd400324u, 0x497ddb0eu, 0x230afa6fu, 0x1a202c39u,
             0x033d51ceu, 0xabc13272u, 0x77131909u, 0xf3b5aaa9u, 0xb7adcd08u,
             0x8a1a78a5u, 0x5ceab50cu, 0x86a44583u, 0xcff7a967u, 0x1e3815beu,
         },
     }},
    {"box", 0xc9, 0x3c, 0x7f, 0x00, 0xff, 0x00, 0x0f, 0x03, 20, 20, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 1,
     {100},
     {1},
     {4},
     {
         {
             0xf6a1b665u, 0xedd72745u, 0x26ec3989u, 0x150e5cacu, 0xe4218382u,
             0x4edf5c99u, 0xfc0fb945u, 0x1d8fb30au, 0x6dc555afu, 0xb4cd0242u,
             0xe004f7c7u, 0x7afa7b74u, 0x1cba99afu, 0xd3ceb33au, 0xa848531du,
             0xa0fdc553u, 0x28017554u, 0xeb385bd2u, 0xe3e34d1fu, 0x50bc831au,
             0x117fbfd1u, 0x4d545ce7u, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
    {"blink625", 0x4e, 0x3c, 0xff, 0x00, 0xff, 0x00, 0xc9, 0x00, 5, 3, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 4,
     {100, 112, 124, 136},
     {0, 0, 1, 1},
     {4, 16, 4, 16},
     {
         {
             0x03307802u, 0x55ade65au, 0x8e04b9c1u, 0xcfcb440eu, 0xda4ac827u,
             0xc7545358u, 0xf4ece925u, 0x1956eb15u, 0x8b978fe4u, 0xb3fcd7dfu,
             0xe8b35683u, 0x7e43655cu, 0xb5ade294u, 0xf5373204u, 0x4ceadf2cu,
             0x6367b568u, 0x61138099u, 0xf6c625c8u, 0x1e7d8e8fu, 0x0ed96af2u,
             0x1d2c805bu, 0x01d30f60u, 0xb1a532dau, 0x6e7cdf44u, 0x1018c98eu,
         },
         {
             0x03307802u, 0x55ade65au, 0x8e04b9c1u, 0xcfcb440eu, 0xda4ac827u,
             0xc7545358u, 0xf4ece925u, 0x1956eb15u, 0x8b978fe4u, 0xb3fcd7dfu,
             0xe8b35683u, 0x7e43655cu, 0xb5ade294u, 0xf5373204u, 0x4ceadf2cu,
             0x6367b568u, 0x61138099u, 0xf6c625c8u, 0x1e7d8e8fu, 0x0ed96af2u,
             0x1d2c805bu, 0x01d30f60u, 0xb1a532dau, 0x6e7cdf44u, 0x1018c98eu,
         },
         {
             0x4dc12705u, 0x72ef16a5u, 0x73916193u, 0x3eafd1d5u, 0x363b7e2cu,
             0x3195241fu, 0x1f7f0ffbu, 0x37aab496u, 0x63fe0034u, 0x7c7b3c28u,
             0xe41afd99u, 0x6fc8b2f0u, 0xa1928e25u, 0xaac94ab9u, 0x288806cau,
             0x8b6d0d46u, 0xe97caeb7u, 0xd5b18810u, 0xeddc658eu, 0x115cd530u,
             0x5a501da8u, 0x4e607002u, 0xa6358583u, 0x1d47c3f7u, 0x38312694u,
         },
         {
             0x4dc12705u, 0x72ef16a5u, 0x73916193u, 0x3eafd1d5u, 0x363b7e2cu,
             0x3195241fu, 0x1f7f0ffbu, 0x37aab496u, 0x63fe0034u, 0x7c7b3c28u,
             0xe41afd99u, 0x6fc8b2f0u, 0xa1928e25u, 0xaac94ab9u, 0x288806cau,
             0x8b6d0d46u, 0xe97caeb7u, 0xd5b18810u, 0xeddc658eu, 0x115cd530u,
             0x5a501da8u, 0x4e607002u, 0xa6358583u, 0x1d47c3f7u, 0x38312694u,
         },
     }},
    {"svc", 0x91, 0x3c, 0x7f, 0x00, 0x7f, 0x00, 0x99, 0x0d, 2, 31, 0x00, 0xdf, 0x08,
     0xf, 0xf, 0x0, 0x0, 0x0, 0xf, 0, 2,
     {100, 140},
     {1, 0},
     {4, 12},
     {
         {
             0x1205ab1eu, 0xe6f3ebf7u, 0x5ae5e357u, 0x8e68e18cu, 0x36c42cdau,
             0xa1d66738u, 0x72e44637u, 0x478f97eeu, 0x5d974d0du, 0x052d0d09u,
             0x1991bd78u, 0x1cca5430u, 0x41d5666au, 0xfdf44dd2u, 0x73483c4bu,
             0xc866793eu, 0xf1b1cda3u, 0xabc73621u, 0xc6cdd897u, 0x598735d1u,
             0x9f68a37eu, 0x654e4f4eu, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
         {
             0x7b59c78au, 0x76699111u, 0x1e178ce3u, 0x2a13db2bu, 0xf4cf9dedu,
             0xa2cd651cu, 0x6038585eu, 0x6601d901u, 0x502afc14u, 0xfabe996fu,
             0x5bea3439u, 0x8ffed6aau, 0x4b474f43u, 0xad3a7dfau, 0x2c4620e5u,
             0x8efd8d5du, 0x76a60372u, 0xa3ccfb27u, 0x46758c68u, 0xab90be45u,
             0x930fcc5fu, 0x0eedc333u, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
    {"mix0", 0x2d, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x09, 0x00, 5, 3, 0x00, 0x5f, 0x10,
     0xa, 0x6, 0x3, 0xc, 0x4, 0xb, 0, 1,
     {100},
     {1},
     {4},
     {
         {
             0x748b08f3u, 0x526210f9u, 0xcee170c2u, 0x1e62f237u, 0x3de0420au,
             0xfba5ed6eu, 0x413ca7c3u, 0x8560772bu, 0x496e071au, 0xa9ad13f0u,
             0x6d7ccdd5u, 0x31edc5d7u, 0x679cc950u, 0x890290d1u, 0x4ae95dcdu,
             0xc2a5a414u, 0x9efa4c48u, 0xb5490532u, 0xd42c854bu, 0x96df3243u,
             0xee67d689u, 0x99300f9du, 0xd2025d05u, 0xd2025d05u, 0xbed3f525u,
         },
     }},
    {"mix1", 0x2d, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x09, 0x00, 5, 3, 0x00, 0x5f, 0x10,
     0xa, 0x6, 0x3, 0xc, 0x4, 0xb, 1, 1,
     {100},
     {1},
     {4},
     {
         {
             0x748b08f3u, 0x526210f9u, 0xcee170c2u, 0x1e62f237u, 0x3de0420au,
             0xfba5ed6eu, 0x413ca7c3u, 0x8560772bu, 0x496e071au, 0xa9ad13f0u,
             0x6d7ccdd5u, 0x31edc5d7u, 0x679cc950u, 0x890290d1u, 0x4ae95dcdu,
             0xc2a5a414u, 0x9efa4c48u, 0xb5490532u, 0xd42c854bu, 0x96df3243u,
             0xee67d689u, 0x99300f9du, 0xd2025d05u, 0xd2025d05u, 0xbed3f525u,
         },
     }},
    {"mix2", 0x58, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x09, 0x00, 5, 3, 0x00, 0x5f, 0x08,
     0xa, 0x6, 0x3, 0xf, 0x4, 0xb, 0, 1,
     {100},
     {1},
     {4},
     {
         {
             0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u,
             0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u,
             0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u,
             0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u,
             0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x21ab8cc5u, 0x767f5a45u,
         },
     }},
    {"mix3", 0x58, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x09, 0x00, 5, 3, 0x00, 0x5f, 0x08,
     0xa, 0x6, 0x3, 0xf, 0x4, 0xb, 1, 1,
     {100},
     {1},
     {4},
     {
         {
             0x6685ac05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u,
             0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u,
             0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u,
             0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u, 0x0e87db05u,
             0x0e87db05u, 0x7a048c05u, 0xd2025d05u, 0xd2025d05u, 0xbed3f525u,
         },
     }},
    {"box1", 0xc9, 0x3c, 0x7f, 0x00, 0xff, 0x00, 0x0f, 0x03, 20, 20, 0x00, 0xdf, 0x28,
     0xf, 0xf, 0xf, 0xf, 0x0, 0xf, 1, 1,
     {100},
     {1},
     {4},
     {
         {
             0x948b91fdu, 0x191d068du, 0x2d25535du, 0xe03beeb5u, 0x7b16e85du,
             0x88c5577du, 0x636c899du, 0x5bacda8du, 0xe6f8831du, 0x654b83c5u,
             0xdaa7099du, 0x827b75a5u, 0xdd1ea2adu, 0x7d78c47du, 0xe0a7f8edu,
             0xd79c394du, 0x65ab271du, 0x5fe9293du, 0x53245e75u, 0x09256bbdu,
             0x1efb487du, 0xafe0c83du, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
    {"ibit", 0x77, 0x3c, 0xff, 0x00, 0xff, 0x00, 0x0f, 0x00, 5, 3, 0x00, 0xdf, 0x18,
     0xf, 0xf, 0xf, 0xf, 0x0, 0xf, 1, 1,
     {100},
     {1},
     {4},
     {
         {
             0xa4b030c5u, 0xf8ce0ec5u, 0xa60ca4c5u, 0xe2539685u, 0xa7ba88c5u,
             0x8bc36b45u, 0xfe277345u, 0xd7c67345u, 0x1e005d45u, 0xe9859485u,
             0x1ab05585u, 0x7ace6dc5u, 0x810db185u, 0x28b567c5u, 0x3224a145u,
             0xad915185u, 0xee223445u, 0xe379e405u, 0x2bf6a5c5u, 0x438d7dc5u,
             0x1b13adc5u, 0xb82385c5u, 0x4207f105u, 0x4207f105u, 0xaa081b25u,
         },
     }},
};
/* GOLDEN END */

static uint8_t rnd8(uint8_t *s)
{
    for (int i = 0; i < 8; i++)
    {
        const int carry = *s & 1;

        *s >>= 1;
        if (carry)
            *s ^= 0xb8;
    }
    return *s;
}

static void efcmd(ef9340_t *e, uint8_t ta, uint8_t tb)
{
    ef9340_io(e, 0, 2, ta);
    ef9340_io(e, 0, 3, tb);
}

static void efdat(ef9340_t *e, uint8_t ta, uint8_t tb)
{
    ef9340_io(e, 0, 0, ta);
    ef9340_io(e, 0, 1, tb);
}

static void efpos(ef9340_t *e, int x, int y)
{
    efcmd(e, (uint8_t)x, 0x40);
    efcmd(e, (uint8_t)y, 0x20);
}

static void mix_out(void *ctx, int port, uint8_t data)
{
    ef9340_mix_port_w(ctx, port, data);
}

/* The test cart's program (efgen.a48), access for access. */
static int replay(const scenario_t *sc, ef9340_t *e, ef9340_mix_t *mix)
{
    uint8_t rs1 = sc->seed1, rs2 = sc->seed2;
    uint8_t p2 = 0xff;
    i8243_t x;

    ef9340_init(e, g_font);
    efcmd(e, sc->rval, 0xa0);
    efcmd(e, 0x00, 0x80);

    for (int pass = 0; pass < 2; pass++)
    {
        int row = 0;

        if (pass == 1)
            rs1 = sc->seed1;
        for (;;)
        {
            if (pass == 0)
                efcmd(e, (uint8_t)row, 0x00);
            for (int col = 0; col < 40; col++)
            {
                const uint8_t a = (uint8_t)((rnd8(&rs1) & sc->amask) | sc->aor);
                const uint8_t b = (uint8_t)((rnd8(&rs1) & sc->bmask) | sc->bor);

                if (pass == 0)
                {
                    efdat(e, a, b);
                }
                else if (b >= 0xa0)
                {
                    efpos(e, col, row);
                    efcmd(e, 0x80, 0x80);
                    for (int s = 0; s < 10; s++)
                        efdat(e, rnd8(&rs2), 0);
                }
            }
            if (row == 31)
                break;
            row = row == 23 ? 31 : row + 1;
        }
    }

    /* a cell read back and written elsewhere */
    efpos(e, 3, 2);
    efcmd(e, 0x60, 0x80);
    (void)ef9340_io(e, 0, 5, 0xff);
    {
        const uint8_t rb = ef9340_io(e, 0, 5, 0xff);
        const uint8_t ra = ef9340_io(e, 0, 4, 0xff);

        efpos(e, 5, 2);
        efcmd(e, 0x40, 0x80);
        efdat(e, ra, rb);
    }

    /* glyph 41h copied from the font into extended character a5h */
    efpos(e, 7, 3);
    efcmd(e, 0x40, 0x80);
    efdat(e, 0x07, 0x41);
    efpos(e, 9, 3);
    efdat(e, 0x06, 0xa5);
    for (int k = 0; k < 10; k++)
    {
        uint8_t slice;

        efpos(e, 7, 3);
        efcmd(e, (uint8_t)(0xa0 | k), 0x80);
        (void)ef9340_io(e, 0, 5, 0xff);
        slice = ef9340_io(e, 0, 4, 0xff);
        efpos(e, 9, 3);
        efcmd(e, (uint8_t)(0x80 | k), 0x80);
        efdat(e, slice, 0);
    }

    efcmd(e, sc->y0, 0xc0);
    efcmd(e, sc->xcur, 0x40);
    efcmd(e, sc->ycur, 0x20);
    efcmd(e, sc->mfin, 0x80);
    efcmd(e, sc->rval, 0xa0);

    ef9340_mix_reset(mix);
    i8243_init(&x, NULL, mix_out, mix);
    expander(&x, &p2, I8243_OP_WRITE, 0, sc->mix4);
    expander(&x, &p2, I8243_OP_WRITE, 1, sc->mix5);
    expander(&x, &p2, I8243_OP_WRITE, 2, sc->mix6);
    expander(&x, &p2, I8243_OP_WRITE, 3, sc->mix7);
    expander(&x, &p2, I8243_OP_OR, 0, sc->mixo);
    expander(&x, &p2, I8243_OP_OR, 2, sc->mixo);
    expander(&x, &p2, I8243_OP_AND, 1, sc->mixa);
    expander(&x, &p2, I8243_OP_AND, 3, sc->mixa);

    return (~sc->p1fin >> 4) & 8;
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* vpp_state::screen_update over the whole visible frame, with the 8245
 * showing only its background colour. */
static void composite(const scenario_t *sc, const ef9340_t *e, const ef9340_mix_t *mix, int lum, uint8_t *out)
{
    const uint8_t d = (uint8_t)(((sc->vbg >> 3 & 1) << 2) | ((sc->vbg >> 4 & 1) << 1) | (sc->vbg >> 5 & 1));
    uint8_t vdc[EF9340_VIS_W];

    memset(vdc, d, sizeof vdc);
    for (int y = 0; y < EF9340_VIS_H; y++)
        ef9340_mix_span(e, mix, y, 0, EF9340_VIS_W, vdc, lum, sc->pin_b, out + (size_t)y * EF9340_VIS_W, NULL);
}

static long long read_file(const char *path, uint8_t *buf, size_t max)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, max, f);
    fclose(f);
    return (long long)n;
}

#define DUMP_STATE 32
#define DUMP_SIZE (DUMP_STATE + 0x400 + 0x400 + 0x800 + EF9340_VIS_W * EF9340_VIS_H)

static int g_print_golden;

static void test_mame_ab(void)
{
    static ef9340_t e;
    static ef9340_mix_t mix;
    static uint8_t frame[EF9340_VIS_W * EF9340_VIS_H];
    static uint8_t dump[DUMP_SIZE + 16];
    const char *dir = getenv("EF9340_MAME_DIR");
    uint8_t realfont[EF9340_FONT_SIZE];
    const char *fontpath = getenv("EF9340_FONT");

    if (fontpath)
    {
        if (read_file(fontpath, realfont, sizeof realfont) != EF9340_FONT_SIZE)
        {
            fprintf(stderr, "cannot read %s\n", fontpath);
            g_fail++;
            return;
        }
        memcpy(g_font, realfont, sizeof g_font);
    }

    for (size_t si = 0; si < sizeof g_scenarios / sizeof g_scenarios[0]; si++)
    {
        const scenario_t *sc = &g_scenarios[si];
        int lum;

        lum = replay(sc, &e, &mix);
        for (int fi = 0; fi < sc->nframes; fi++)
        {
            const int n = sc->frames[fi];
            uint32_t bands[NBANDS];

            ef9340_run_until(&e, (uint64_t)n * EF9340_FRAME_CLOCKS);
            composite(sc, &e, &mix, lum, frame);
            for (int b = 0; b < NBANDS; b++)
            {
                const int y0 = b * 10;
                const int rows = (y0 + 10 <= EF9340_VIS_H) ? 10 : EF9340_VIS_H - y0;

                bands[b] = fnv1a(frame + (size_t)y0 * EF9340_VIS_W, (size_t)rows * EF9340_VIS_W);
            }

            if (g_print_golden)
            {
                printf("/* %s frame %d: blink %d prescaler %d */\n", sc->name, n, e.blink, e.blink_prescaler);
                printf("    {");
                for (int b = 0; b < NBANDS; b++)
                    printf("%s0x%08xu", b ? ", " : "", bands[b]);
                printf("},\n");
            }
            else if (!fontpath)
            {
                for (int b = 0; b < NBANDS; b++)
                    CHECK(bands[b] == sc->bands[fi][b], "%s frame %d: rows %d-%d differ from MAME (0x%08x, want 0x%08x)",
                          sc->name, n, b * 10, b * 10 + 9, bands[b], sc->bands[fi][b]);
                CHECK_EQ(e.blink, sc->blink[fi]);
                CHECK_EQ(e.blink_prescaler, sc->prescaler[fi]);
            }

            if (dir)
            {
                char path[1024];
                long long got;

                snprintf(path, sizeof path, "%s/%s_%d.dump", dir, sc->name, n);
                got = read_file(path, dump, sizeof dump);
                if (got != DUMP_SIZE)
                {
                    fprintf(stderr, "no MAME dump %s (%lld bytes)\n", path, got);
                    g_fail++;
                    continue;
                }
                CHECK_EQ(e.r, dump[0]);
                CHECK_EQ(e.m, dump[1]);
                CHECK_EQ(e.y0, dump[2]);
                CHECK_EQ(e.x, dump[3]);
                CHECK_EQ(e.y, dump[4]);
                CHECK_EQ(e.blink, dump[5]);
                CHECK_EQ(e.blink_prescaler, dump[6]);
                CHECK_EQ(e.h_parity, dump[7]);
                CHECK_EQ(e.ta, dump[8]);
                CHECK_EQ(e.tb, dump[9]);
                CHECK_EQ(e.busy, dump[10]);
                CHECK_EQ(mix.vdc, dump[11]);
                CHECK_EQ(mix.ef, dump[12]);
                CHECK(memcmp(e.ram_a, dump + DUMP_STATE, 0x400) == 0, "%s %d: attribute RAM differs from MAME", sc->name, n);
                CHECK(memcmp(e.ram_b, dump + DUMP_STATE + 0x400, 0x400) == 0, "%s %d: code RAM differs from MAME", sc->name, n);
                CHECK(memcmp(e.exram, dump + DUMP_STATE + 0x800, 0x800) == 0, "%s %d: extended RAM differs from MAME", sc->name, n);
                {
                    const uint8_t *want = dump + DUMP_STATE + 0x1000;
                    int bad = 0;

                    for (int i = 0; i < EF9340_VIS_W * EF9340_VIS_H && bad < 5; i++)
                        if (frame[i] != want[i])
                        {
                            fprintf(stderr, "FAIL %s frame %d: pixel (%d,%d) = %d, MAME %d\n", sc->name, n,
                                    i % EF9340_VIS_W, i / EF9340_VIS_W, frame[i], want[i]);
                            bad++;
                            g_fail++;
                        }
                    g_checks++;
                }
            }
        }
    }
}

int main(void)
{
    make_testfont();
    make_dfont();
    g_print_golden = getenv("EF9340_PRINT_GOLDEN") != NULL;

    if (!g_print_golden)
    {
        test_i8243();
        test_addresses();
        test_interface();
        test_timing();
        test_debug();
        test_blink_counter();
        test_render();
        test_mixer();
    }
    test_mame_ab();

    if (g_fail)
    {
        fprintf(stderr, "ef9340_test: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    printf("ef9340_test: %d checks passed\n", g_checks);
    return 0;
}

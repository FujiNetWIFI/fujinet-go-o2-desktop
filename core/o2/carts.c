/* carts.c -- the cartridge mappers: standard, 4-in-1, rally, KTAA.
 *
 * Transposed from MAME's src/devices/bus/odyssey2/{slot,rom,4in1,rally,ktaa}.cpp
 * (BSD-3-Clause, copyright-holders Fabio Priuli, Wilbert Pol, hap -- see
 * COMPLIANCE.md). The behaviour is MAME's; the software list's PCB feature
 * is replaced by a small table of image CRCs (the software list's special
 * mappers) plus MAME's own rule for a plain file. The Chess and Home Computer
 * modules carry a second CPU of their own and are not emulated: they are
 * recognised so the frontends can say so.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "carts.h"

#include <string.h>

uint32_t o2_crc32(const uint8_t *p, uint32_t n)
{
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t crc = 0xffffffffu;
    while (n--)
        crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static uint32_t bit_floor(uint32_t v)
{
    uint32_t r = 0;
    if (v) {
        r = 1;
        while (r <= v / 2)
            r <<= 1;
    }
    return r;
}

/* ---- the empty slot ---- */

static uint8_t empty_read(o2_cart_t *c, uint16_t a, int p) { (void)c; (void)a; (void)p; return 0xff; }
static uint8_t empty_io_read(o2_cart_t *c, uint8_t o, int p) { (void)c; (void)o; (void)p; return 0xff; }
static void empty_io_write(o2_cart_t *c, uint8_t o, uint8_t d) { (void)c; (void)o; (void)d; }
static void empty_wr(o2_cart_t *c, uint8_t d) { (void)c; (void)d; }
static uint8_t empty_bus_read(o2_cart_t *c) { (void)c; return 0xff; }
static int empty_t0(o2_cart_t *c) { (void)c; return 0; }

const o2_cart_ops_t o2_cart_empty_ops = {
    empty_read, empty_io_read, empty_io_write, empty_wr, empty_wr,
    empty_bus_read, empty_wr, empty_t0,
};

/* An address in $400-$FFF as the offset MAME's read_rom04 sees ($C00-$FFF is
 * read_rom0c, which the standard and rally mappers forward as offset + $400,
 * so $C00 mirrors $800). */
static inline uint32_t rom04_offset(uint16_t addr)
{
    return addr < 0xc00 ? (uint32_t)addr - 0x400 : (uint32_t)addr - 0xc00 + 0x400;
}

/* ---- o2_rom: standard, 2K pages, bank = P10/P11 ---- */

static uint8_t std_read(o2_cart_t *c, uint16_t addr, int peek)
{
    (void)peek;
    uint32_t off = (rom04_offset(addr) + (uint32_t)c->bank * 0x800u) & c->cart_mask;
    return off < c->rom_size ? c->rom[off] : 0xff;
}

static void std_write_p1(o2_cart_t *c, uint8_t d) { c->bank = d & 3; }

static const o2_cart_ops_t std_ops = {
    std_read, empty_io_read, empty_io_write, std_write_p1, empty_wr,
    empty_bus_read, empty_wr, empty_t0,
};

/* ---- o2_4in1: Videopac 31 and 40; the image is 4K, linear at $000-$FFF,
 * and P11 high turns the data space into a window on it ---- */

static uint8_t f4_read(o2_cart_t *c, uint16_t addr, int peek)
{
    (void)peek;
    return c->rom[addr];                 /* read_rom04: rom[off + $400] */
}

static uint8_t f4_io_read(o2_cart_t *c, uint8_t off, int peek)
{
    (void)peek;
    return (c->control & 2) ? c->rom[(uint32_t)c->bank << 8 | off] : 0xff;
}

static void f4_write_p1(o2_cart_t *c, uint8_t d) { c->control = d; }
static void f4_write_p2(o2_cart_t *c, uint8_t d) { c->bank = d; }

static const o2_cart_ops_t f4_ops = {
    f4_read, f4_io_read, empty_io_write, f4_write_p1, f4_write_p2,
    empty_bus_read, empty_wr, empty_t0,
};

/* ---- o2_rally: Videopac+ 55/58/59/60; a bank latch written through the
 * data space ($80-$FF with P14 low); P10 high shows the last bank ---- */

static uint8_t rally_read(o2_cart_t *c, uint16_t addr, int peek)
{
    (void)peek;
    uint8_t bank = (c->control & 1) ? 0xff : c->bank;
    uint32_t off = (rom04_offset(addr) + (uint32_t)bank * 0x800u) & c->cart_mask;
    return off < c->rom_size ? c->rom[off] : 0xff;
}

static void rally_io_write(o2_cart_t *c, uint8_t off, uint8_t data)
{
    if ((off & 0x80) && !(c->control & 0x10))
        c->bank = data;
}

static const o2_cart_ops_t rally_ops = {
    rally_read, empty_io_read, rally_io_write, f4_write_p1, empty_wr,
    empty_bus_read, empty_wr, empty_t0,
};

/* ---- o2_ktaa: Kill the Attacking Aliens; 3K pages selected by P1 ---- */

static uint8_t ktaa_read(o2_cart_t *c, uint16_t addr, int peek)
{
    (void)peek;
    uint32_t off = (uint32_t)addr - 0x400;         /* read_rom0c is read_rom04 + $800 */
    off += c->page_size - 0xc00;
    off += (uint32_t)c->bank * c->page_size;
    return off < c->rom_size ? c->rom[off] : 0xff;
}

static void ktaa_write_p1(o2_cart_t *c, uint8_t d) { c->bank = d & c->bank_mask; }

static const o2_cart_ops_t ktaa_ops = {
    ktaa_read, empty_io_read, empty_io_write, ktaa_write_p1, empty_wr,
    empty_bus_read, empty_wr, empty_t0,
};

/* ---- identification and setup ---- */

static const struct {
    uint32_t crc;
    int type;
    bool voice;
} k_db[] = {
    { 0x3bfef56b, O2_CART_4IN1, false },       /* 4 in 1 Row (Europe) */
    { 0x9b5e9356, O2_CART_4IN1, false },       /* 4 en 1 Ligne (France) */
    { 0xafb23f89, O2_CART_4IN1, false },       /* Musician (Europe) */
    { 0xd94d4109, O2_CART_RALLY, false },      /* Air Battle (Europe, plus) */
    { 0xbe11157c, O2_CART_RALLY, false },      /* Neutron Star (Europe, plus) */
    { 0xc4134df8, O2_CART_RALLY, false },      /* Helicopter Rescue (Europe) */
    { 0x0d2d721d, O2_CART_RALLY, false },      /* Trans American Rally (Europe) */
    { 0x4e2cc6d3, O2_CART_KTAA, false },       /* Kill the Attacking Aliens */
    { 0x77066338, O2_CART_CHESS, false },      /* Chess Module (Europe) */
    { 0x8e824b98, O2_CART_HOMECOMP, false },   /* Home Computer Module (Europe) */
    { 0x1b9f2f16, O2_CART_STD, true },         /* Sid the Spellbinder! (USA) */
};

int o2_cart_identify(const uint8_t *img, uint32_t size, bool *needs_voice)
{
    if (needs_voice)
        *needs_voice = false;
    uint32_t crc = o2_crc32(img, size);
    for (size_t i = 0; i < sizeof k_db / sizeof k_db[0]; i++) {
        if (k_db[i].crc == crc) {
            if (needs_voice)
                *needs_voice = k_db[i].voice;
            return k_db[i].type;
        }
    }
    return size == 0x4000 ? O2_CART_RALLY : O2_CART_STD;   /* slot.cpp */
}

int o2_cart_init(o2_cart_t *c, int type, const uint8_t *rom, uint32_t size, const char **err)
{
    static const char *dummy;
    if (!err)
        err = &dummy;
    memset(c, 0, sizeof *c);
    c->type = type;
    c->rom = rom;
    c->rom_size = size;
    if (!rom || size == 0) {
        *err = "the cartridge image is empty";
        return -1;
    }
    switch (type) {
    case O2_CART_STD:
        c->cart_mask = bit_floor(size) - 1;
        c->ops = &std_ops;
        return 0;
    case O2_CART_4IN1:
        if (size != 0x1000) {
            *err = "a 4-in-1 cartridge image must be 4 KB";
            return -1;
        }
        c->ops = &f4_ops;
        return 0;
    case O2_CART_RALLY: {
        if (size < 0x800) {
            *err = "a rally cartridge image must be at least 2 KB";
            return -1;
        }
        uint32_t p2 = bit_floor(size);
        if (p2 != size)
            p2 <<= 1;                  /* 12K Videopac+ images sit in a 16K ROM */
        c->cart_mask = p2 - 1;
        c->ops = &rally_ops;
        return 0;
    }
    case O2_CART_KTAA: {
        bool err_size;
        if (size & (size - 1)) {
            err_size = size != 0xc00 && size != 0xc00 * 2 && size != 0xc00 * 4;
            c->page_size = 0xc00;
        } else {
            err_size = size < 0x1000;
            c->page_size = 0x1000;
        }
        if (err_size) {
            *err = "a KTAA cartridge image must be a multiple of 3 KB or 4 KB";
            return -1;
        }
        c->bank_mask = (uint8_t)(size / c->page_size - 1);
        c->ops = &ktaa_ops;
        return 0;
    }
    case O2_CART_CHESS:
        *err = "the Chess Module has a CPU of its own (NSC800), which this app does not emulate";
        return -1;
    case O2_CART_HOMECOMP:
        *err = "the Home Computer Module has a Z80 of its own, which this app does not emulate";
        return -1;
    default:
        *err = "unknown cartridge type";
        return -1;
    }
}

const char *o2_cart_type_name(int type)
{
    switch (type) {
    case O2_CART_STD:      return "Standard";
    case O2_CART_4IN1:     return "4-in-1";
    case O2_CART_RALLY:    return "Videopac+ rally";
    case O2_CART_KTAA:     return "KTAA";
    case O2_CART_VOICE:    return "The Voice";
    case O2_CART_FUJINET:  return "FujiNet";
    case O2_CART_CHESS:    return "Chess Module";
    case O2_CART_HOMECOMP: return "Home Computer Module";
    default:               return "none";
    }
}

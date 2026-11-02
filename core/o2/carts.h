/* carts.h -- the cartridge slot and its mappers.
 *
 * Transposed from MAME's Odyssey 2 cartridge bus (src/devices/bus/odyssey2/
 * slot.cpp, rom.cpp, 4in1.cpp, rally.cpp, ktaa.cpp; BSD-3-Clause,
 * copyright-holders Fabio Priuli, Wilbert Pol, hap -- see COMPLIANCE.md).
 *
 * The slot is a set of function pointers, because the FujiNet cartridge
 * (fujinet_cart.c) and The Voice (voice.c) sit in the same slot and the
 * machine must not know which it has. Each mapper sees exactly what MAME's
 * device_o2_cart_interface sees:
 *
 *   read    $400-$FFF program fetches and MOVP/MOVX reads of cart space
 *           (MAME's read_rom04 for $400-$BFF and read_rom0c for $C00-$FFF)
 *   io_read / io_write   the external data space (MOVX), only while the
 *                        console selects the cart (P14 low), offset = A0-A7
 *   write_p1 / write_p2  the console's port latches as MAME hands them over
 *                        (P1 & $13: bank P10/P11 and P14; P2 & $0F: A8-A11)
 *   bus_read / bus_write the 8048's BUS port
 *   t0                   the cart's T0 input (The Voice's !LRQ)
 *
 * A read made by the debugger passes peek = 1: it must change nothing (the
 * FujiNet cart's windows have side effects; MAME's side_effects_disabled()).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME; see COMPLIANCE.md)
 */

#ifndef O2_CARTS_H
#define O2_CARTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum o2_cart_type {
    O2_CART_NONE = 0,
    O2_CART_STD,        /* o2_rom: 2K pages, up to 8K, P10/P11 bank select */
    O2_CART_4IN1,       /* o2_4in1: 4K, the second 2K read as I/O (Videopac 31, 40) */
    O2_CART_RALLY,      /* o2_rally: up to 16K, bank latch written through I/O */
    O2_CART_KTAA,       /* o2_ktaa: Kill the Attacking Aliens, 3K pages */
    O2_CART_VOICE,      /* o2_voice: The Voice, with a cartridge behind it */
    O2_CART_FUJINET,    /* the FujiNet cartridge (fujinet_cart.c) */
    /* recognised so they can be refused politely */
    O2_CART_CHESS,      /* Chess Module: its own NSC800 */
    O2_CART_HOMECOMP,   /* Home Computer Module: its own Z80 */
};

typedef struct o2_cart o2_cart_t;

typedef struct {
    uint8_t (*read)(o2_cart_t *c, uint16_t addr, int peek);   /* addr $400-$FFF */
    uint8_t (*io_read)(o2_cart_t *c, uint8_t offset, int peek);
    void (*io_write)(o2_cart_t *c, uint8_t offset, uint8_t data);
    void (*write_p1)(o2_cart_t *c, uint8_t data);
    void (*write_p2)(o2_cart_t *c, uint8_t data);
    uint8_t (*bus_read)(o2_cart_t *c);
    void (*bus_write)(o2_cart_t *c, uint8_t data);
    int (*t0)(o2_cart_t *c);
} o2_cart_ops_t;

struct o2_cart {
    const o2_cart_ops_t *ops;
    int type;                       /* enum o2_cart_type */

    /* the image (not owned) and the mapper's state */
    const uint8_t *rom;
    uint32_t rom_size;
    uint32_t cart_mask;             /* std: bit_floor(size) - 1 */
    uint32_t page_size;             /* ktaa */
    uint8_t bank_mask;              /* ktaa */
    uint8_t control;                /* P1 as MAME hands it over */
    uint8_t bank;

    o2_cart_t *sub;                 /* voice: the cartridge behind it */
    void *priv;                     /* voice/fujinet: their own state */
};

/* What a plain image is: by its CRC-32 against the software list's special
 * mappers (4-in-1 and Musician, the Videopac+ rally carts, KTAA, the two
 * modules with their own CPUs), else MAME's own rule -- exactly 16K is a
 * rally cart, anything else a standard one. `needs_voice` is set when the
 * game carries speech data of its own (Sid the Spellbinder). */
int o2_cart_identify(const uint8_t *img, uint32_t size, bool *needs_voice);

/* Set up c for `type` over an image of `size` bytes (not copied). Returns 0,
 * or -1 and a message in *err when MAME would refuse the image (a 4-in-1
 * that is not 4K, a rally cart that is not 2^n, a KTAA that is not a
 * multiple of 3K or 4K). */
int o2_cart_init(o2_cart_t *c, int type, const uint8_t *rom, uint32_t size, const char **err);

const char *o2_cart_type_name(int type);

/* CRC-32 (IEEE: the zip/MAME polynomial). */
uint32_t o2_crc32(const uint8_t *p, uint32_t n);

/* The slot with nothing in it: reads $FF. */
extern const o2_cart_ops_t o2_cart_empty_ops;

/* Thin dispatch, so the machine reads like MAME's slot device. */
static inline uint8_t o2_cart_read(o2_cart_t *c, uint16_t addr, int peek)
{
    return c ? c->ops->read(c, addr, peek) : 0xff;
}
static inline uint8_t o2_cart_io_read(o2_cart_t *c, uint8_t off, int peek)
{
    return c ? c->ops->io_read(c, off, peek) : 0xff;
}
static inline void o2_cart_io_write(o2_cart_t *c, uint8_t off, uint8_t data)
{
    if (c) c->ops->io_write(c, off, data);
}
static inline void o2_cart_write_p1(o2_cart_t *c, uint8_t data)
{
    if (c) c->ops->write_p1(c, data);
}
static inline void o2_cart_write_p2(o2_cart_t *c, uint8_t data)
{
    if (c) c->ops->write_p2(c, data);
}
static inline uint8_t o2_cart_bus_read(o2_cart_t *c)
{
    return c ? c->ops->bus_read(c) : 0xff;
}
static inline void o2_cart_bus_write(o2_cart_t *c, uint8_t data)
{
    if (c) c->ops->bus_write(c, data);
}
static inline int o2_cart_t0(o2_cart_t *c)
{
    return c ? c->ops->t0(c) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* O2_CARTS_H */

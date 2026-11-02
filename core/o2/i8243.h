/* i8243.h -- the Intel 8243 I/O expander (MCS-48 family), the Videopac+
 * G7400's colour-mix latch.
 *
 * Transposed from MAME's src/devices/machine/i8243.cpp and i8243.h
 * (BSD-3-Clause, copyright-holder Aaron Giles): the same state machine,
 * the same names minus m_. The expander shares the CPU's P20-P23 with the
 * PROG strobe: the 8048 puts a 4-bit opcode (operation << 2 | port) on
 * P20-P23, pulls PROG low, puts the data nibble on P20-P23 (or floats them
 * for a read) and lets PROG go high.
 *
 * What is different: MAME binds four read and four write callbacks (P4..P7)
 * with devcb; here one `in` and one `out` function pointer take the port
 * number (0..3 for P4..P7) and a context pointer. An unset `in` leaves the
 * port latch as it is (MAME logs "Unconfigured read" and does the same); an
 * unset `out` drops the write (MAME logs it).
 *
 * Wiring on the G7400 (vpp_state): the CPU's P2 output callback calls
 * i8243_p2_w(p2 & 0x0f), its PROG output callback calls i8243_prog_w(level),
 * and CS is never driven (it reads low: selected). MAME does NOT connect
 * i8243_p2_r() to the CPU's P2 input on the G7400 (the keyboard decoder owns
 * that callback), so expander reads (MOVD A,P4-7) return the port-2 pins'
 * pull-ups, not the expander's data; the G7400 only writes the expander.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/machine/i8243.cpp; copyright-holder
 * Aaron Giles -- see COMPLIANCE.md)
 */

#ifndef O2_I8243_H
#define O2_I8243_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The expander operations (mcs48_cpu_device::EXPANDER_OP_*), the top two
 * bits of the latched opcode. */
enum {
    I8243_OP_READ = 0,
    I8243_OP_WRITE = 1,
    I8243_OP_OR = 2,
    I8243_OP_AND = 3
};

typedef uint8_t (*i8243_in_fn)(void *ctx, int port);               /* port 0..3 = P4..P7; low nibble used */
typedef void (*i8243_out_fn)(void *ctx, int port, uint8_t data);   /* data: the port's 4-bit latch */

typedef struct {
    uint8_t p[4];       /* 4 ports' worth of data */
    uint8_t p2out;      /* port 2 bits that will be returned */
    uint8_t p2;         /* most recent port 2 value */
    uint8_t opcode;     /* latched opcode */
    bool    prog;       /* previous PROG state */
    bool    cs;         /* chip select (active low, as the pin) */

    i8243_in_fn  in;
    i8243_out_fn out;
    void        *ctx;
} i8243_t;

/* device constructor + device_start state: p = 0, p2out = p2 = 0x0f,
 * opcode 0, PROG high, CS low. The callbacks are kept. */
void i8243_init(i8243_t *d, i8243_in_fn in, i8243_out_fn out, void *ctx);

uint8_t i8243_p2_r(const i8243_t *d);
void    i8243_p2_w(i8243_t *d, uint8_t data);       /* the CPU's P2 low nibble (data & 0x0f) */
void    i8243_prog_w(i8243_t *d, int state);
void    i8243_cs_w(i8243_t *d, int state);

#ifdef __cplusplus
}
#endif

#endif /* O2_I8243_H */

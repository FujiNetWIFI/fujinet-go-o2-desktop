/* ef9340.h -- the Thomson EF9340 + EF9341 teletext-style text chips of the
 * Philips Videopac+ G7400 (with the 2 x 1 KB page RAM, the 2 KB extended
 * character RAM and the colour mixer the G7400 wires around them).
 *
 * Transposed from MAME's src/devices/video/ef9340_1.cpp and ef9340_1.h
 * (BSD-3-Clause, copyright-holders Wilbert Pol, hap), plus the parts of
 * src/mame/philips/odyssey2.cpp (vpp_state) that sit between the chips and
 * the console: the EF9341 I/O decode (io_vpp), the extended RAM address
 * latch, the 8243 colour-mix ports and the per-pixel mix of the EF9340 layer
 * with the 8245's picture (vpp_state::screen_update). The 8243 itself is
 * i8243.h; the 8245 is i8244.h.
 *
 * What is the same as MAME: the EF9341 command/data protocol (TA/TB, load
 * X/Y/M/R/Y0, begin row, increment C, write / read / without increment /
 * slice), the cell address map, the scanline renderer (service row,
 * double height and width, inversion, blink, cursor, boxing, conceal,
 * underline, graphics cells, extended characters, zoom, Y0 scrolling,
 * 525/625-line display window), the blink counters and the mixer.
 *
 * What is different (all deliberate):
 *   - no emu_timer, screen or bitmap: the chip is a plain struct whose two
 *     timers (the scanline draw and the blink counter) are absolute pixel
 *     clock times that ef9340_run_until() fires, and the "bitmap" is the
 *     layer array below (8-bit, not 16);
 *   - the extended character RAM (MAME's vpp_state::m_ef934x_extram plus
 *     its read/write callbacks) lives inside the chip, with the address
 *     latch of vpp_state::ef934x_extram_address;
 *   - the character generator ROM is not built in: the caller passes a
 *     pointer to the 0xA00-byte image (MAME's charset_ef9340_1.rom, marked
 *     BAD_DUMP there, CRC 8de85988) or NULL, which draws blank glyphs and
 *     reads them as 0;
 *   - the layer is rendered even where MAME's screen would not show it
 *     (the whole 456 x 313 raster), exactly as MAME's tmp_bitmap is.
 *
 * TIME. The time base is the 8245's: absolute pixel clocks (uint64_t),
 * 7.0937 MHz on the G7400 ((8.867 MHz * 2 / 5) * 2, MAME's screen clock),
 * 456 per line, 313 lines per frame (142 728 per frame, 49.70 Hz). One 8048
 * machine cycle (5.911 MHz / 15) is 18 pixel clocks (MAME's real ratio is
 * 18.0007, the crystals being given to three decimals; use 18). Time zero
 * is the start of vblank, i.e. the beam is at line 243 (VIS_H), pixel 0 at
 * clock 0, and "line 0" starts 70 lines later: ef9340_vpos(t) and
 * ef9340_hpos(t) are MAME's screen().vpos() / hpos() for that screen.
 *
 * The scanline timer fires at pixel 0 of every line and draws that line from
 * the registers as they are at that clock; ef9340_run_until(e, t) fires every
 * event due at or before t, and ef9340_read / write / io do that first with
 * the `now` they are given.
 *
 * WHICH CLOCK TO PASS. MAME's scheduler gives the CPU a slice that ends at
 * the next timer of any device, lets the instruction that starts before it
 * finish (the MCS-48 burns an instruction's cycles first, so a MOVX sees the
 * clock at the END of its instruction), and only then fires the timer. So a
 * write inside an instruction that straddles a line start reaches the chip
 * before that line is drawn. To reproduce that, pass the clock at which the
 * CPU STARTED the instruction (cycles before it, times 18) to these
 * functions, not the end-of-instruction clock the 8245 wants: events due at
 * or before the start fire first, the access follows, events in the
 * instruction's own window fire after it (at the next instruction's start).
 * The difference is one machine cycle, 18 of a line's 456 pixel clocks.
 *
 * Several instances may coexist; there are no globals and nothing allocates.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/video/ef9340_1.cpp and
 * src/mame/philips/odyssey2.cpp; copyright-holders Wilbert Pol, hap, Peter
 * Trauner -- see COMPLIANCE.md)
 */

#ifndef O2_EF9340_H
#define O2_EF9340_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The G7400's screen: set_raw(clock * 2, 456, 0, 360, 313, 0, 243). */
#define EF9340_HTOTAL          456
#define EF9340_VTOTAL          313
#define EF9340_VIS_W           360
#define EF9340_VIS_H           243
#define EF9340_FRAME_CLOCKS    (EF9340_HTOTAL * EF9340_VTOTAL)   /* 142728 */
#define EF9340_CLOCKS_PER_MACHINE_CYCLE 18

/* set_offsets(15, 5) in vpp_state::g7400: the text area's top-left on the
 * raster. 40 cells of 8 pixels from column 15; row 5 is the service row's
 * first line. */
#define EF9340_OFFSET_X        15
#define EF9340_OFFSET_Y        5
#define EF9340_COLUMNS         40
#define EF9340_ROWS            24

#define EF9340_FONT_SIZE       0xA00    /* 256 glyphs x 10 slices */
#define EF9340_RAM_SIZE        0x400    /* each of the A (attribute) and B (code) pages */
#define EF9340_EXRAM_SIZE      0x800    /* the G7400's extended character RAM */

/* The layer's row stride (the largest raster this struct supports). */
#define EF9340_LAYER_STRIDE    456
#define EF9340_LAYER_ROWS      313

typedef struct {
    /* the EF9341 (GEN): the CPU-facing registers */
    uint8_t ta, tb;
    bool    busy;           /* never left set: MAME completes a command at once */

    /* the EF9340 (VIN) */
    uint8_t x, y, y0;       /* active position, first displayed row (+ zoom bit 5) */
    uint8_t r, m;           /* mode registers */
    bool    blink;
    int     blink_prescaler;
    bool    h_parity;       /* double height: this row shows the bottom half */

    uint8_t ram_a[EF9340_RAM_SIZE];     /* attributes */
    uint8_t ram_b[EF9340_RAM_SIZE];     /* character codes */
    uint8_t exram[EF9340_EXRAM_SIZE];   /* extended characters */

    const uint8_t *font;    /* EF9340_FONT_SIZE bytes, or NULL (blank) */

    /* geometry: the G7400's by default; set between init and reset to run
     * another board. offset_x + 320 <= htotal, htotal <= EF9340_LAYER_STRIDE,
     * vtotal <= EF9340_LAYER_ROWS. */
    int htotal, vtotal, vis_h;
    int offset_x, offset_y;

    /* the two timers, as absolute pixel clocks */
    uint64_t line_next, blink_next;

    uint64_t lines_drawn;   /* for the debugger */

    /* MAME's m_tmp_bitmap: the text layer, one byte a pixel, raster order.
     * bits 0-2 the colour (bit 0 red, 1 green, 2 blue), bit 3 "I": set where
     * the chip draws an opaque pixel (text, the service row's blank, the
     * graphics background), clear for transparent pixels (boxed mode,
     * display off, outside the text area). Rows 0..vtotal-1, columns
     * 0..htotal-1; the visible part is columns 0..359, rows 0..242. */
    uint8_t layer[EF9340_LAYER_ROWS * EF9340_LAYER_STRIDE];
} ef9340_t;

/* device_start: zeroed state, the G7400 geometry, the font (or NULL), and
 * ef9340_reset(e, 0). */
void ef9340_init(ef9340_t *e, const uint8_t *font);

/* Install the character generator image (EF9340_FONT_SIZE bytes, kept by
 * pointer) or NULL. Takes effect from the next line drawn. */
void ef9340_set_font(ef9340_t *e, const uint8_t *font);

/* Power-on at clock `now`: all state zero (MAME has no device_reset: the
 * console's RESET key never reaches the chip), the layer cleared, the line
 * and blink timers armed at the next line 0. Not for the RESET key. */
void ef9340_reset(ef9340_t *e, uint64_t now);

/* Fire every timer due at or before `now`, in MAME's order (the line before
 * the blink counter when both fall on one clock). */
void ef9340_run_until(ef9340_t *e, uint64_t now);

/* The next timer's clock (a machine scheduling its CPU slices needs no
 * other bound: the chip raises no interrupt and drives no CPU pin). */
uint64_t ef9340_next_event(const ef9340_t *e);

/* The beam at a clock: MAME's screen().vpos() / hpos() for the G7400 screen. */
static inline int ef9340_vpos(uint64_t t)
{
    return (int)((EF9340_VIS_H + (t % EF9340_FRAME_CLOCKS) / EF9340_HTOTAL) % EF9340_VTOTAL);
}

static inline int ef9340_hpos(uint64_t t)
{
    return (int)(t % EF9340_HTOTAL);
}

/* ---- the CPU's side --------------------------------------------------
 *
 * ef9341_write / ef9341_read, with `command` and `b` as MAME takes them
 * (any non-zero command selects the command register, b the B register of
 * the pair: TA = (command 0, b 0) data A, (0, 1) data B, (1, 0) command A,
 * (1, 1) command B). Each runs the chip up to `now` first.
 */
void    ef9340_write(ef9340_t *e, uint64_t now, int command, int b, uint8_t data);
uint8_t ef9340_read(ef9340_t *e, uint64_t now, int command, int b);

/* vpp_state::io_vpp for a MOVX at external address `offset` (R0/R1), to be
 * called when P1.5 is low, for BOTH reads and writes: A2 is the chip's R/W
 * pin, the CPU's RD/WR strobes are not (so a MOVX read at 0-3 *writes* the
 * data the other devices put on the bus, and a MOVX write at 4-7 *reads*,
 * with the read's side effects). `data` is the bus value so far (the
 * write's data, or what the cartridge, RAM and VDC returned and'ed
 * together); the return value is the bus value after the chip: for a read
 * `data & the chip's byte`, for a write `data` unchanged. A2=1: read of
 * register (offset & 2, offset & 1). A2=0: write of `data` to it. */
uint8_t ef9340_io(ef9340_t *e, uint64_t now, uint8_t offset, uint8_t data);

/* The extended character RAM through the address latch of
 * vpp_state::ef934x_extram_address; `offset` is a << 12 | b << 4 | slice as
 * the chip forms it (a, b the cell's attribute and code, slice 0-15). */
uint8_t ef9340_exram_r(const ef9340_t *e, uint32_t offset);
void    ef9340_exram_w(ef9340_t *e, uint32_t offset, uint8_t data);
uint16_t ef9340_exram_address(uint32_t offset);

/* The page RAM address of a cell (MAME's ef9340_get_c_addr): rows 0-23 and
 * the service row 31, columns 0-39 (columns 32-39 and the service row live
 * in the 0x300 area). */
uint16_t ef9340_cell_address(uint8_t x, uint8_t y);

/* ---- the picture ------------------------------------------------------
 *
 * The text layer is in e->layer; ef9340_layer_row() is its row y (0..312),
 * column 0 (visible columns 0..359). A pixel is 0 (transparent, colour 0) or
 * 8 | colour. */
static inline const uint8_t *ef9340_layer_row(const ef9340_t *e, int y)
{
    return e->layer + (size_t)y * EF9340_LAYER_STRIDE;
}

/* ---- the colour mixer (vpp_state) -------------------------------------
 *
 * The 8243's P4-P7 hold two 8-bit mix tables: for each of the 8 colours,
 * whether the 8245's pixel of that colour wins over the EF9340's (`vdc`,
 * m_mix_i8244) and whether an EF9340 pixel of that colour is dim (bit set)
 * or bright and asserts CX (bit clear) (`ef`, m_mix_ef934x). Both are 0xFF
 * at power-on.
 */
typedef struct {
    uint8_t vdc;
    uint8_t ef;
} ef9340_mix_t;

void ef9340_mix_reset(ef9340_mix_t *m);

/* The 8243's port callback (vpp_state::i8243_port_w<P>): `port` 0..3 is
 * P4..P7, `nibble` the 4 bits written. As MAME's code (not its comment)
 * has it: P4 = bits 7-4 and P5 = bits 3-0 of `ef`, P6 = bits 7-4 and
 * P7 = bits 3-0 of `vdc`. The caller must first bring the picture up to the
 * beam (MAME calls screen update_now() here) with the old tables. */
void ef9340_mix_port_w(ef9340_mix_t *m, int port, uint8_t nibble);

/* One pixel of vpp_state::screen_update.
 *   vdc_pixel  the 8245's pixel for this x, 0..15 (colour in bits 0-2, the
 *              chip's own luminance in bit 3)
 *   ef_pixel   the EF9340 layer pixel, 0..15
 *   lum        0 or 8: the console's LUM line, (~P1 >> 4) & 8
 *   cart_b     the cartridge's pin B (1 for a Videopac+ cartridge: the
 *              display is limited to the EF9340's opaque area; 0 for a
 *              plain Videopac cartridge, which MAME also assumes for every
 *              file loaded with -cart)
 *   *cx        out (may be NULL): the CX line for this pixel, to be handed
 *              to the 8245 as i8244_write_cx(x, cx) in the same pass
 * returns the palette index 0..15 (MAME's odyssey2_colors). */
static inline uint8_t ef9340_mix_pixel(const ef9340_mix_t *m, uint8_t vdc_pixel,
                                       uint8_t ef_pixel, int lum, int cart_b, int *cx)
{
    const unsigned d = vdc_pixel & 7u;
    const bool en = (ef_pixel & 8u) || !cart_b;     /* 8245 decoder enable */
    const unsigned c = ef_pixel & 7u;
    const bool i2 = !((m->ef >> c) & 1u);           /* decoder output, tied to CX */

    if (cx)
        *cx = i2;
    if (en && ((m->vdc >> d) & 1u))
        return (uint8_t)((vdc_pixel | lum) & 15u);  /* the 8245's input */
    return (uint8_t)(c | (i2 ? 8u : 0u));           /* the EF9340's input */
}

/* The loop of vpp_state::screen_update over x0 <= x < x1 (MAME's cliprect is
 * inclusive: pass max_x + 1) of raster row y: vdc_row[x] is the 8245's pixel
 * (already drawn for the span, before its LUM is applied), out[x] gets the
 * mixed palette index (out may be vdc_row itself) and cx[x] (may be NULL) the
 * CX line, which the caller forwards as i8244_write_cx(x, 1) wherever it is
 * set. Rows other than 0..242 and columns beyond 359 are not in MAME's clip:
 * don't pass them. The mix tables, `lum` and `cart_b` are the values at the
 * time of the render: bring the 8245's picture up to the beam (i8244_catch_up)
 * before changing any of them, as MAME's update_now() does. */
void ef9340_mix_span(const ef9340_t *e, const ef9340_mix_t *m, int y, int x0, int x1,
                     const uint8_t *vdc_row, int lum, int cart_b,
                     uint8_t *out, uint8_t *cx);

/* ---- the debugger ------------------------------------------------------ */

enum {
    EF9340_CELL_ALNUM = 0,  /* alphanumeric: code b, foreground a & 7 */
    EF9340_CELL_GRAPHIC,    /* a bit 7: block graphics, fg a & 7 on bg a >> 4 & 7 */
    EF9340_CELL_DELIMITER,  /* alphanumeric with b 0x80-0x9f: a blank that sets
                             * background/box/conceal/underline for what follows */
    EF9340_CELL_ILLEGAL     /* graphics with b 0x80-0x9f: drawn solid */
};

enum {
    EF9340_ATTR_DOUBLE_HEIGHT = 1,  /* a bit 4 (alphanumeric only) */
    EF9340_ATTR_DOUBLE_WIDTH  = 2,  /* a bit 5 (alphanumeric only) */
    EF9340_ATTR_INVERTED      = 4,  /* a bit 6 (alphanumeric only) */
    EF9340_ATTR_NO_BLINK      = 8,  /* a bit 3 set: blink does not hide the cell */
    EF9340_ATTR_EXTENDED      = 16  /* b >= 0xA0: the glyph is in the extended RAM */
};

typedef struct {
    uint8_t a, b;           /* the cell's attribute and code */
    uint8_t kind;           /* EF9340_CELL_* */
    uint8_t fg, bg;         /* bg is meaningful for graphics and delimiters */
    uint8_t flags;          /* EF9340_ATTR_* */
    uint8_t glyph;          /* the font index: (b & 0x7f) or (b | 0x80) for
                             * graphics; the extended glyph's b for EXTENDED */
} ef9340_cell_t;

typedef struct {
    /* EF9341 */
    uint8_t ta, tb;
    bool    busy;
    /* EF9340 */
    uint8_t x, y, y0, r, m;
    bool    blink;
    int     blink_prescaler;
    bool    h_parity;
    /* R decoded */
    bool    display_on;         /* r bit 0 */
    bool    box_mode;           /* r bit 1: boxed display, blank outside delimiter boxes */
    bool    conceal_enabled;    /* r bit 2 */
    bool    service_row;        /* r bit 3: the 10-line row above the page is shown */
    bool    cursor_on;          /* r bit 4 */
    bool    lines625;           /* r bit 6: 250 text lines and a 24-frame blink, else 210 and 32 */
    bool    blink_enabled;      /* r bit 7 */
    /* Y0 decoded */
    uint8_t first_row;          /* y0 & 0x1f */
    bool    zoom;               /* y0 bit 5: rows below the service row are double height */
    /* M decoded */
    uint8_t mode;               /* m & 0xe0: 0x00 write+inc, 0x20 read+inc, 0x40 write,
                                 * 0x60 read, 0x80 write slice, 0xa0 read slice */
    uint8_t slice;              /* m & 0x0f */
    const char *mode_name;
    /* the cursor: cell (x, y) is inverted while cursor_on; true when (x, y)
     * addresses a cell (x < 40, y < 24 or the service row 31) */
    bool    cursor_visible;

    ef9340_cell_t page[EF9340_ROWS][EF9340_COLUMNS];
    ef9340_cell_t service[EF9340_COLUMNS];      /* row 31 */

    uint8_t ram_a[EF9340_RAM_SIZE];
    uint8_t ram_b[EF9340_RAM_SIZE];
    uint8_t exram[EF9340_EXRAM_SIZE];
    bool    font_present;
    uint64_t lines_drawn;
} ef9340_snapshot_t;

/* A copy of everything the chip knows (no side effects on it). */
void ef9340_snapshot(const ef9340_t *e, ef9340_snapshot_t *s);

/* Decode one page cell the way the renderer sees it (kind, colours, flags). */
void ef9340_decode_cell(uint8_t a, uint8_t b, ef9340_cell_t *c);

/* The visible raster of the text layer, EF9340_VIS_W x EF9340_VIS_H bytes
 * (pixel values 0..15 as in e->layer), without any compositing. */
void ef9340_layer_copy(const ef9340_t *e, uint8_t *dst);

/* The same as 32-bit 0x00RRGGBB through a 16-entry table indexed by the
 * layer's pixel value; NULL uses ef9340_debug_color(). */
void ef9340_layer_rgb(const ef9340_t *e, uint32_t *dst, const uint32_t *pal16);

/* A view colour for a layer pixel: the eight colours at full intensity
 * (bit 0 red, 1 green, 2 blue) when opaque (bit 3), dark grey 0x202020 when
 * transparent. */
uint32_t ef9340_debug_color(uint8_t pixel);

#ifdef __cplusplus
}
#endif

#endif /* O2_EF9340_H */

/* ef9340.c -- the EF9340 + EF9341 (see ef9340.h).
 *
 * Transposed from MAME's src/devices/video/ef9340_1.cpp (BSD-3-Clause,
 * copyright-holders Wilbert Pol, hap) -- device_start, blink_update,
 * ef9340_get_c_addr, ef9340_inc_c, ef9341_write, ef9341_read, draw_scanline --
 * and, from src/mame/philips/odyssey2.cpp (copyright-holders Peter Trauner,
 * Wilbert Pol, hap), vpp_state::io_vpp, ef934x_extram_address / _r / _w,
 * i8243_port_w and the mixing loop of screen_update. The names follow
 * MAME's (minus the m_ prefix).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/video/ef9340_1.cpp and
 * src/mame/philips/odyssey2.cpp; copyright-holders Wilbert Pol, hap, Peter
 * Trauner -- see COMPLIANCE.md)
 */

#include <string.h>

#include "ef9340.h"

/* ---- time: screen_device::time_until_pos for this screen --------------- */

static int64_t frame_clocks(const ef9340_t *e)
{
    return (int64_t)e->htotal * e->vtotal;
}

/* MAME measures time from the start of vblank, which is the line after the
 * visible area (vis_h); a position the beam is already at or past goes to
 * the next frame. */
static uint64_t time_until_pos(const ef9340_t *e, uint64_t now, int vpos, int hpos)
{
    const int64_t frame = frame_clocks(e);
    const int64_t line = (vpos + e->vtotal - e->vis_h) % e->vtotal;
    const int64_t target = line * e->htotal + hpos;
    const int64_t cur = (int64_t)(now % (uint64_t)frame);
    int64_t delta = target - cur;

    if (delta <= 0)
        delta += frame;
    return (uint64_t)delta;
}

/* screen().vpos() at an event clock. */
static int vpos_at(const ef9340_t *e, uint64_t t)
{
    const uint64_t frame = (uint64_t)frame_clocks(e);

    return (int)(((uint64_t)e->vis_h + (t % frame) / (uint64_t)e->htotal) % (uint64_t)e->vtotal);
}

/* ---- device_start -------------------------------------------------------- */

void ef9340_init(ef9340_t *e, const uint8_t *font)
{
    memset(e, 0, sizeof *e);
    e->font = font;
    e->htotal = EF9340_HTOTAL;
    e->vtotal = EF9340_VTOTAL;
    e->vis_h = EF9340_VIS_H;
    e->offset_x = EF9340_OFFSET_X;
    e->offset_y = EF9340_OFFSET_Y;
    ef9340_reset(e, 0);
}

void ef9340_set_font(ef9340_t *e, const uint8_t *font)
{
    e->font = font;
}

void ef9340_reset(ef9340_t *e, uint64_t now)
{
    e->ta = 0;
    e->tb = 0;
    e->busy = false;

    e->x = 0;
    e->y = 0;
    e->y0 = 0;
    e->r = 0;
    e->m = 0;
    e->blink = false;
    e->blink_prescaler = 0;
    e->h_parity = false;

    memset(e->ram_a, 0, sizeof e->ram_a);
    memset(e->ram_b, 0, sizeof e->ram_b);
    /* MAME zero-fills vpp_state::m_ef934x_extram in machine_start. */
    memset(e->exram, 0, sizeof e->exram);
    memset(e->layer, 0, sizeof e->layer);

    e->lines_drawn = 0;
    e->line_next = now + time_until_pos(e, now, 0, 0);
    e->blink_next = now + time_until_pos(e, now, 0, 0);
}

/* ---- the extended character RAM (vpp_state) ----------------------------- */

uint16_t ef9340_exram_address(uint32_t offset)
{
    const uint8_t latch = (uint8_t)((offset >> 12 & 0x80) | (offset >> 4 & 0x7f));
    const uint16_t address = (uint16_t)((latch & 0x1f) | (offset << 9 & 0x200) | (latch << 3 & 0x400));

    if (offset & 8)
        return (uint16_t)(address | (latch & 0x60));
    return (uint16_t)(address | (offset << 4 & 0x60) | (latch << 2 & 0x180));
}

uint8_t ef9340_exram_r(const ef9340_t *e, uint32_t offset)
{
    return e->exram[ef9340_exram_address(offset)];
}

void ef9340_exram_w(ef9340_t *e, uint32_t offset, uint8_t data)
{
    e->exram[ef9340_exram_address(offset)] = data;
}

/* ---- the cell address and the position counters ------------------------- */

uint16_t ef9340_cell_address(uint8_t x, uint8_t y)
{
    if ((y & 0x18) == 0x18)
        return (uint16_t)(0x318 | ((x & 0x38) << 2) | (x & 0x07));
    if (x & 0x20)
        return (uint16_t)(0x300 | ((y & 0x07) << 5) | (y & 0x18) | (x & 0x07));
    return (uint16_t)((y & 0x1f) << 5 | (x & 0x1f));
}

static void inc_c(ef9340_t *e)
{
    e->x++;
    if (e->x == 40 || e->x == 48 || e->x == 56 || e->x == 64)
    {
        e->y = (uint8_t)((e->y + 1) & 0x1f);
        if (e->y == 24)
            e->y = 0;
        e->x = 0;
    }
}

static uint8_t font_at(const ef9340_t *e, unsigned index)
{
    return e->font ? e->font[index] : 0;
}

/* ---- the EF9341's CPU interface ------------------------------------------ */

static void write_now(ef9340_t *e, int command, int b, uint8_t data)
{
    if (command)
    {
        if (b)
        {
            e->tb = data;
            e->busy = true;
            switch (e->tb & 0xe0)
            {
            case 0x00: /* begin row */
                e->x = 0;
                e->y = e->ta & 0x1f;
                break;
            case 0x20: /* load Y */
                e->y = e->ta & 0x1f;
                break;
            case 0x40: /* load X */
                e->x = e->ta & 0x3f;
                break;
            case 0x60: /* increment C */
                inc_c(e);
                break;
            case 0x80: /* load M */
                e->m = e->ta;
                break;
            case 0xa0: /* load R */
                e->r = e->ta;
                break;
            case 0xc0: /* load Y0 */
                e->y0 = e->ta & 0x3f;
                break;
            case 0xe0: /* not interpreted */
                break;
            }
            e->busy = false;
        }
        else
        {
            e->ta = data;
        }
    }
    else
    {
        if (b)
        {
            const uint16_t addr = ef9340_cell_address(e->x, e->y) & 0x3ff;

            e->tb = data;
            e->busy = true;
            switch (e->m & 0xe0)
            {
            case 0x00: /* write */
                e->ram_a[addr] = e->ta;
                e->ram_b[addr] = e->tb;
                inc_c(e);
                break;

            case 0x40: /* write without increment */
                e->ram_a[addr] = e->ta;
                e->ram_b[addr] = e->tb;
                break;

            case 0x80: /* write slice */
            {
                const uint8_t a = e->ram_a[addr];
                const uint8_t cb = e->ram_b[addr];
                const uint8_t slice = e->m & 0x0f;

                if (cb >= 0xa0)
                    ef9340_exram_w(e, (uint32_t)a << 12 | (uint32_t)cb << 4 | slice, e->ta);

                /* increment slice number */
                e->m = (uint8_t)((e->m & 0xf0) | ((slice + 1) % 10));
                break;
            }

            default: /* illegal or read command */
                break;
            }
            e->busy = false;
        }
        else
        {
            e->ta = data;
        }
    }
}

static uint8_t read_now(ef9340_t *e, int command, int b)
{
    uint8_t data;

    if (command)
    {
        data = b ? 0 : (e->busy ? 0x80 : 0);
    }
    else if (b)
    {
        const uint16_t addr = ef9340_cell_address(e->x, e->y) & 0x3ff;

        data = e->tb;
        e->busy = true;
        switch (e->m & 0xe0)
        {
        case 0x20: /* read */
            e->ta = e->ram_a[addr];
            e->tb = e->ram_b[addr];
            inc_c(e);
            break;

        case 0x60: /* read without increment */
            e->ta = e->ram_a[addr];
            e->tb = e->ram_b[addr];
            break;

        case 0xa0: /* read slice */
        {
            const uint8_t a = e->ram_a[addr];
            const uint8_t cb = e->ram_b[addr];
            const uint8_t slice = e->m & 0x0f;

            e->ta = 0xff;
            e->tb = 0xff;

            if (cb >= 0xa0)
                e->ta = ef9340_exram_r(e, (uint32_t)a << 12 | (uint32_t)cb << 4 | slice);
            else if (cb < 0x80 && slice < 10)
                e->ta = font_at(e, (unsigned)(((a & 0x80) | (cb & 0x7f)) * 10) + slice);

            /* increment slice number */
            e->m = (uint8_t)((e->m & 0xf0) | ((slice + 1) % 10));
            break;
        }

        default: /* illegal or write command */
            break;
        }
        e->busy = false;
    }
    else
    {
        data = e->ta;
    }
    return data;
}

void ef9340_write(ef9340_t *e, uint64_t now, int command, int b, uint8_t data)
{
    ef9340_run_until(e, now);
    write_now(e, command, b, data);
}

uint8_t ef9340_read(ef9340_t *e, uint64_t now, int command, int b)
{
    ef9340_run_until(e, now);
    return read_now(e, command, b);
}

uint8_t ef9340_io(ef9340_t *e, uint64_t now, uint8_t offset, uint8_t data)
{
    ef9340_run_until(e, now);
    /* A2 to the R/W pin */
    if (offset & 4)
        data &= read_now(e, offset & 0x02, offset & 0x01);
    else
        write_now(e, offset & 0x02, offset & 0x01, data);
    return data;
}

/* ---- the timers ----------------------------------------------------------- */

static void blink_update(ef9340_t *e)
{
    /* blink rate is approximately 0.5s */
    e->blink_prescaler = (e->blink_prescaler + 1) & 0x1f;
    if ((e->r & 0x40) && e->blink_prescaler == 24)
        e->blink_prescaler = 0;

    if (e->blink_prescaler == 0)
        e->blink = !e->blink;
}

static void draw_scanline(ef9340_t *e, int screen_vpos)
{
    int vpos = screen_vpos - e->offset_y;
    uint8_t *row;
    int zoom, slice, max_vpos;
    bool dh = false;

    if (vpos < 0)
        return;

    row = e->layer + (size_t)screen_vpos * EF9340_LAYER_STRIDE + (size_t)e->offset_x;
    e->lines_drawn++;

    zoom = ((e->y0 & 0x20) && vpos >= 10) ? 2 : 1;
    slice = vpos % 10;
    if (zoom == 2)
        slice = ((vpos - 10) % 20) / 2;

    if (vpos == 0)
        e->h_parity = false;

    /* display automaton active at 40-290, or 32-242 */
    max_vpos = (e->r & 0x40) ? 250 : 210;

    if ((e->r & 0x01) && vpos < max_vpos)
    {
        int y_row = 0;
        uint16_t char_data = 0x00;
        uint8_t fg = 0;
        uint8_t bg = 0;
        bool conceal = false;
        bool box = (e->r & 0x02) != 0;
        bool underline = false;
        bool w_parity = false;

        if (vpos < 10)
        {
            /* service row */
            if (e->r & 0x08)
            {
                /* service row is enabled */
                y_row = 31;
            }
            else
            {
                /* service row is disabled */
                memset(row, 8, 40 * 8);
                return;
            }
        }
        else
        {
            /* displaying regular row */
            y_row = ((e->y0 & 0x1f) + (vpos - 10) / (10 * zoom)) % 24;
        }

        for (int x = 0; x < 40; x++)
        {
            int s = slice;
            const uint16_t addr = ef9340_cell_address((uint8_t)x, (uint8_t)y_row);
            const uint8_t a = e->ram_a[addr];
            const uint8_t b = e->ram_b[addr];
            bool blink = (e->r & 0x80) && e->blink;
            const bool cursor = (e->r & 0x10) && x == e->x && y_row == e->y;
            bool invert = cursor && !blink;
            bool dw = false;
            bool del = false;

            if (a & 0x80)
            {
                /* graphics */
                if ((b & 0xe0) != 0x80)
                {
                    fg = a & 0x07;
                    bg = a >> 4 & 0x07;

                    if (b & 0x80)
                        char_data = ef9340_exram_r(e, (uint32_t)a << 12 | (uint32_t)b << 4 | (uint32_t)s);
                    else
                        char_data = font_at(e, (unsigned)((b | 0x80) * 10 + s));
                }
                else
                {
                    /* illegal */
                    char_data = 0xff;
                }
            }
            else
            {
                /* alphanumeric */
                if ((b & 0xe0) != 0x80)
                {
                    /* double height */
                    if (a & 0x10)
                    {
                        dh = true;
                        if (e->h_parity)
                            s += 10;
                        if (s > 0)
                            s = (s - 1) / 2;
                    }

                    fg = a & 0x07;
                    {
                        uint16_t c;

                        if (b & 0x80)
                            c = ef9340_exram_r(e, (uint32_t)a << 12 | (uint32_t)b << 4 | (uint32_t)s);
                        else if (s == 9 && underline)
                            c = 0xff;
                        else
                            c = font_at(e, (unsigned)((b & 0x7f) * 10 + s));

                        /* double width */
                        dw = (a & 0x20) != 0;
                        if (dw)
                        {
                            if (!w_parity)
                            {
                                /* bitswap<16>(c,7,7,6,6,5,5,4,4,3,3,2,2,1,1,0,0) */
                                char_data = 0;
                                for (int i = 0; i < 8; i++)
                                    if (c >> i & 1)
                                        char_data |= (uint16_t)(3u << (2 * i));
                            }
                        }
                        else
                        {
                            char_data = c;
                        }
                    }

                    /* inverted */
                    if (a & 0x40)
                    {
                        invert = !invert;
                        blink = (e->r & 0x80) && !e->blink;
                    }
                }
                else
                {
                    /* deliminator */
                    fg = a & 0x07;
                    bg = a >> 4 & 0x07;
                    char_data = 0xff;

                    del = true;
                }
            }

            /* blink character */
            if (blink && !cursor && (b & 0xe0) != 0x80 && !(a & 0x08))
                char_data &= (uint16_t)~0xffu;

            if (invert)
                char_data ^= 0xff;

            if (dw)
                w_parity = !w_parity;
            else
                w_parity = false;

            for (int i = 0; i < 8; i++)
            {
                const uint8_t d = (conceal || box) ? 0 : (char_data & 1) ? fg : bg;

                row[x * 8 + i] = (uint8_t)(d | (box ? 0 : 8));
                char_data >>= 1;
            }

            if (del)
            {
                conceal = (b & 0x01) && (e->r & 0x04);
                box = !(b & 0x02) && (e->r & 0x02);
                underline = (b & 0x04) != 0;
            }
        }
    }
    else
    {
        memset(row, 0, 40 * 8);
    }

    /* determine next h parity */
    if (vpos >= 10 && (vpos & 1) && slice == 9)
    {
        if (dh)
            e->h_parity = !e->h_parity;
        else
            e->h_parity = false;
    }
}

uint64_t ef9340_next_event(const ef9340_t *e)
{
    return e->line_next <= e->blink_next ? e->line_next : e->blink_next;
}

void ef9340_run_until(ef9340_t *e, uint64_t now)
{
    for (;;)
    {
        /* MAME allocates the line timer first, so it fires first when both
         * are due on the same clock (the first line of a frame). */
        if (e->line_next <= e->blink_next)
        {
            if (e->line_next > now)
                return;
            draw_scanline(e, vpos_at(e, e->line_next));
            e->line_next += (uint64_t)e->htotal;
        }
        else
        {
            if (e->blink_next > now)
                return;
            blink_update(e);
            e->blink_next += (uint64_t)frame_clocks(e);
        }
    }
}

/* ---- the mixer (vpp_state) -------------------------------------------------- */

void ef9340_mix_reset(ef9340_mix_t *m)
{
    m->vdc = 0xff;
    m->ef = 0xff;
}

void ef9340_mix_port_w(ef9340_mix_t *m, int port, uint8_t nibble)
{
    uint8_t data = nibble & 0x0f;
    uint8_t mask = 0x0f;

    /* i8243_port_w<P>: P4 and P6 are the high nibbles */
    if (!(port & 1))
    {
        data = (uint8_t)(data << 4);
        mask = (uint8_t)(mask << 4);
    }

    /* The comment in MAME says P4,P5 are the 8245's side and P6,P7 the
     * EF9340's; the code does the opposite, and the code is what runs. */
    if (port & 2)
        m->vdc = (uint8_t)((m->vdc & ~mask) | (data & mask));
    else
        m->ef = (uint8_t)((m->ef & ~mask) | (data & mask));
}

void ef9340_mix_span(const ef9340_t *e, const ef9340_mix_t *m, int y, int x0, int x1,
                     const uint8_t *vdc_row, int lum, int cart_b,
                     uint8_t *out, uint8_t *cx)
{
    const uint8_t *ef_row = ef9340_layer_row(e, y);

    for (int x = x0; x < x1; x++)
    {
        int c = 0;

        out[x] = ef9340_mix_pixel(m, vdc_row[x], ef_row[x], lum, cart_b, &c);
        if (cx)
            cx[x] = (uint8_t)c;
    }
}

/* ---- the debugger ----------------------------------------------------------- */

void ef9340_decode_cell(uint8_t a, uint8_t b, ef9340_cell_t *c)
{
    memset(c, 0, sizeof *c);
    c->a = a;
    c->b = b;
    if (a & 0x80)
    {
        if ((b & 0xe0) == 0x80)
        {
            c->kind = EF9340_CELL_ILLEGAL;
        }
        else
        {
            c->kind = EF9340_CELL_GRAPHIC;
            c->fg = a & 7;
            c->bg = a >> 4 & 7;
            c->glyph = (b & 0x80) ? b : (uint8_t)(b | 0x80);
        }
    }
    else if ((b & 0xe0) == 0x80)
    {
        c->kind = EF9340_CELL_DELIMITER;
        c->fg = a & 7;
        c->bg = a >> 4 & 7;
    }
    else
    {
        c->kind = EF9340_CELL_ALNUM;
        c->fg = a & 7;
        c->glyph = (b & 0x80) ? b : (uint8_t)(b & 0x7f);
        if (a & 0x10)
            c->flags |= EF9340_ATTR_DOUBLE_HEIGHT;
        if (a & 0x20)
            c->flags |= EF9340_ATTR_DOUBLE_WIDTH;
        if (a & 0x40)
            c->flags |= EF9340_ATTR_INVERTED;
    }
    if (a & 0x08)
        c->flags |= EF9340_ATTR_NO_BLINK;
    if (c->kind != EF9340_CELL_DELIMITER && c->kind != EF9340_CELL_ILLEGAL && b >= 0xa0)
        c->flags |= EF9340_ATTR_EXTENDED;
}

static const char *mode_name(uint8_t m)
{
    switch (m & 0xe0)
    {
    case 0x00: return "write, increment";
    case 0x20: return "read, increment";
    case 0x40: return "write";
    case 0x60: return "read";
    case 0x80: return "write slice";
    case 0xa0: return "read slice";
    default:   return "illegal";
    }
}

void ef9340_snapshot(const ef9340_t *e, ef9340_snapshot_t *s)
{
    memset(s, 0, sizeof *s);
    s->ta = e->ta;
    s->tb = e->tb;
    s->busy = e->busy;
    s->x = e->x;
    s->y = e->y;
    s->y0 = e->y0;
    s->r = e->r;
    s->m = e->m;
    s->blink = e->blink;
    s->blink_prescaler = e->blink_prescaler;
    s->h_parity = e->h_parity;

    s->display_on = (e->r & 0x01) != 0;
    s->box_mode = (e->r & 0x02) != 0;
    s->conceal_enabled = (e->r & 0x04) != 0;
    s->service_row = (e->r & 0x08) != 0;
    s->cursor_on = (e->r & 0x10) != 0;
    s->lines625 = (e->r & 0x40) != 0;
    s->blink_enabled = (e->r & 0x80) != 0;
    s->first_row = e->y0 & 0x1f;
    s->zoom = (e->y0 & 0x20) != 0;
    s->mode = e->m & 0xe0;
    s->slice = e->m & 0x0f;
    s->mode_name = mode_name(e->m);
    s->cursor_visible = s->cursor_on && e->x < 40 && (e->y < 24 || e->y == 31);

    for (int y = 0; y < EF9340_ROWS; y++)
        for (int x = 0; x < EF9340_COLUMNS; x++)
        {
            const uint16_t addr = ef9340_cell_address((uint8_t)x, (uint8_t)y) & 0x3ff;

            ef9340_decode_cell(e->ram_a[addr], e->ram_b[addr], &s->page[y][x]);
        }
    for (int x = 0; x < EF9340_COLUMNS; x++)
    {
        const uint16_t addr = ef9340_cell_address((uint8_t)x, 31) & 0x3ff;

        ef9340_decode_cell(e->ram_a[addr], e->ram_b[addr], &s->service[x]);
    }

    memcpy(s->ram_a, e->ram_a, sizeof s->ram_a);
    memcpy(s->ram_b, e->ram_b, sizeof s->ram_b);
    memcpy(s->exram, e->exram, sizeof s->exram);
    s->font_present = e->font != NULL;
    s->lines_drawn = e->lines_drawn;
}

void ef9340_layer_copy(const ef9340_t *e, uint8_t *dst)
{
    for (int y = 0; y < EF9340_VIS_H; y++)
        memcpy(dst + (size_t)y * EF9340_VIS_W, ef9340_layer_row(e, y), EF9340_VIS_W);
}

uint32_t ef9340_debug_color(uint8_t pixel)
{
    const uint32_t r = (pixel & 1) ? 0xffu : 0u;
    const uint32_t g = (pixel & 2) ? 0xffu : 0u;
    const uint32_t b = (pixel & 4) ? 0xffu : 0u;

    if (!(pixel & 8))
        return 0x202020u;   /* transparent (the layer always has colour 0 there) */
    return r << 16 | g << 8 | b;
}

void ef9340_layer_rgb(const ef9340_t *e, uint32_t *dst, const uint32_t *pal16)
{
    for (int y = 0; y < EF9340_VIS_H; y++)
    {
        const uint8_t *src = ef9340_layer_row(e, y);

        for (int x = 0; x < EF9340_VIS_W; x++)
            dst[(size_t)y * EF9340_VIS_W + x] = pal16 ? pal16[src[x] & 15] : ef9340_debug_color(src[x]);
    }
}

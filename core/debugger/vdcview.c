/*
 * vdcview.c -- the debugger's glue for the chips that draw and speak: the
 * 8244/8245 VDC, the Videopac+ G7400's EF9340 text chip and The Voice's
 * SP0256. Thin on purpose: the chips decode their own registers
 * (i8244_snapshot_get, i8244_describe_register, the renderers, ef9340_snapshot,
 * sp0256_debug_get); this file takes the host lock, picks the machine's
 * chip, and does the three things the chips do not -- scale an image up,
 * decide which object the pointer is over, and print a chip as prompt text.
 *
 * This file is also the marker that the debugger is ported: core/CMakeLists.txt
 * compiles the debugger once it exists.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugger_internal.h"
#include "host.h"

#define FRAME_W I8244_WIDTH
#define FRAME_H I8244_HEIGHT

#define VIEW_FG 0x00e0e0e0u
#define VIEW_BG 0x00101010u

/* ---- the snapshot, the registers ------------------------------------------- */

void o2debug_vdc_get(o2debug *d, i8244_snapshot *out)
{
    o2_machine_t *m;

    (void)d;
    memset(out, 0, sizeof *out);
    o2_host_lock();
    m = o2debug_machine();
    if (m)
        i8244_snapshot_get(&m->vdc, o2_machine_now(m), out);
    o2_host_unlock();
}

void o2debug_vdc_describe(o2debug *d, unsigned offset, char *name, size_t name_n,
                          char *text, size_t text_n)
{
    o2_machine_t *m;

    (void)d;
    if (name && name_n)
        name[0] = '\0';
    if (text && text_n)
        text[0] = '\0';
    o2_host_lock();
    m = o2debug_machine();
    if (m)
        i8244_describe_register(&m->vdc, offset & 0xff, name, name_n, text, text_n);
    o2_host_unlock();
}

void o2debug_vdc_write(o2debug *d, unsigned offset, uint8_t value)
{
    /* MAME's data masks, the write lockouts ignored: the machine's poke */
    o2debug_write(d, O2_SPACE_VDC, (uint16_t)(offset & 0xff), value);
}

/* ---- the views -------------------------------------------------------------- */

/* Nearest-neighbour: every source pixel becomes scale x scale. */
static void upscale(const uint32_t *src, int w, int h, int scale, uint32_t *dst)
{
    const size_t dw = (size_t)w * (size_t)scale;

    for (int y = 0; y < h; y++)
    {
        uint32_t *row = dst + (size_t)y * (size_t)scale * dw;

        for (int x = 0; x < w; x++)
            for (int k = 0; k < scale; k++)
                row[(size_t)x * (size_t)scale + (size_t)k] = src[(size_t)y * (size_t)w + (size_t)x];
        for (int k = 1; k < scale; k++)
            memcpy(row + (size_t)k * dw, row, dw * sizeof *row);
    }
}

int o2debug_vdc_view(o2debug *d, int view, unsigned layers, unsigned flags,
                     int scale, uint32_t *dst, size_t cap, int *w, int *h)
{
    /* one frame's worth of scratch; the host lock serialises its users */
    static uint32_t tmp[FRAME_W * FRAME_H];
    o2_machine_t *m;
    int ok = 0, rw = 0, rh = 0;

    (void)d;
    if (scale < 1)
        scale = 1;
    o2_host_lock();
    m = o2debug_machine();
    switch (view)
    {
    case O2DEBUG_VDC_FRAME:
    case O2DEBUG_VDC_RENDER:
    case O2DEBUG_VDC_PROGRESS:
        rw = FRAME_W * scale;
        rh = FRAME_H * scale;
        if (!m)
            break;
        ok = 1;
        if (!dst)
            break;
        if (cap < (size_t)rw * (size_t)rh)
        {
            ok = 0;
            break;
        }
        if (view == O2DEBUG_VDC_FRAME)
            memcpy(tmp, m->fb, sizeof tmp);
        else if (view == O2DEBUG_VDC_RENDER)
            i8244_debug_render(&m->vdc, layers, flags, tmp, FRAME_W, FRAME_H);
        else
        {
            i8244_catch_up(&m->vdc, o2_machine_now(m));
            i8244_frame_in_progress(&m->vdc, o2_machine_now(m), flags, tmp, NULL, NULL);
        }
        if (scale == 1)
            memcpy(dst, tmp, sizeof tmp);
        else
            upscale(tmp, FRAME_W, FRAME_H, scale, dst);
        break;
    case O2DEBUG_VDC_FONT:
        if (m)
            ok = i8244_render_font_sheet(&m->vdc, scale, VIEW_FG, VIEW_BG, dst, cap, &rw, &rh) == 0;
        break;
    case O2DEBUG_VDC_SHAPES:
        if (m)
            ok = i8244_render_shape_sheet(&m->vdc, scale, VIEW_BG, dst, cap, &rw, &rh) == 0;
        break;
    case O2DEBUG_VDC_PALETTE:
        if (m)
            ok = i8244_render_palette_strip(&m->vdc, 16 * scale, dst, cap, &rw, &rh) == 0;
        break;
    default:
        break;
    }
    o2_host_unlock();
    if (w)
        *w = rw;
    if (h)
        *h = rh;
    return ok;
}

int o2debug_vdc_object(o2debug *d, int kind, int index, int scale, uint32_t bg,
                       uint32_t *dst, size_t cap, int *w, int *h)
{
    o2_machine_t *m;
    int rc = -1, rw = 0, rh = 0;

    (void)d;
    if (scale < 1)
        scale = 1;
    o2_host_lock();
    m = o2debug_machine();
    if (m)
        switch (kind)
        {
        case 0: rc = i8244_render_sprite(&m->vdc, index, scale, bg, dst, cap, &rw, &rh); break;
        case 1: rc = i8244_render_char(&m->vdc, 0, index, scale, bg, dst, cap, &rw, &rh); break;
        case 2: rc = i8244_render_char(&m->vdc, 1, index, scale, bg, dst, cap, &rw, &rh); break;
        case 3: rc = i8244_render_glyph(&m->vdc, index, scale, VIEW_FG, bg, dst, cap, &rw, &rh); break;
        default: break;
        }
    o2_host_unlock();
    if (w)
        *w = rw;
    if (h)
        *h = rh;
    return rc == 0;
}

/* ---- what is under the pointer ----------------------------------------------- */

typedef struct { int kind, index, lit; } candidate;

/* A font pixel of a character box (16 px wide: 8 font bits, 2 px each; the
 * rows are 2 lines each, from the snapshot's first font byte). */
static int char_lit(const i8244_char_info *c, const uint8_t *font, int x, int y)
{
    int col = (x - c->x) / 2, row = (y - c->y) / 2;

    if (!font || col < 0 || col > 7 || row < 0 || row >= c->rows)
        return 0;
    return (font[(c->rom_offset + row) & 0x1ff] >> (col ^ 7)) & 1;
}

/* A shape pixel of a sprite box: 8 bits a row, each 2 px (4 zoomed), and the
 * S / X9 attributes nudge a row by half a bit (the chip's own rule, which
 * the snapshot's box already includes). */
static int sprite_lit(const i8244_sprite_info *s, int x, int y)
{
    const int zpx = s->zoom ? 4 : 2;
    const int row = (y - s->y) / zpx;
    int shift, col;

    if (row < 0 || row > 7)
        return 0;
    if (s->s && s->x9)
        shift = row & 1;
    else if (s->s)
        shift = (row & 1) ^ 1;
    else
        shift = s->x9;
    col = (x - (s->x + shift * (zpx / 2))) / zpx;
    if (x < s->x + shift * (zpx / 2) || col < 0 || col > 7)
        return 0;
    return (s->shape[row] >> col) & 1;
}

static int in_box(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

int o2debug_vdc_hit_test(o2debug *d, int x, int y, o2debug_hit *out, int max)
{
    o2_machine_t *m;
    i8244_snapshot s;
    candidate c[4 + 16 + 12];
    const uint8_t *font = NULL;
    int n = 0, count = 0;

    (void)d;
    o2_host_lock();
    m = o2debug_machine();
    if (!m || !out || max <= 0)
    {
        o2_host_unlock();
        return 0;
    }
    i8244_snapshot_get(&m->vdc, o2_machine_now(m), &s);
    font = m->vdc.font;
    o2_host_unlock();

    /* the chip's drawing order, topmost first (i8244.c: render_row draws the
     * grid, then the quads from 3 down to 0, the singles from 11 down to 0
     * -- where two of those overlap, the one with the HIGHER index wins, as
     * major_pixel() skips a pixel under an already lit one of a higher
     * index -- and last the sprites from 3 down to 0, each overwriting what
     * is there, so sprite 0 is on top of everything) */
    for (int i = 0; i < 4; i++)
    {
        const i8244_sprite_info *p = &s.sprites[i];

        if (p->visible && in_box(x, y, p->x, p->y, p->width, p->height))
        {
            c[n].kind = 0;
            c[n].index = i;
            c[n++].lit = sprite_lit(p, x, y);
        }
    }
    for (int q = 3; q >= 0; q--)
        for (int j = 0; j < 4; j++)
        {
            const i8244_char_info *p = &s.quads[q].chars[j];

            if (p->visible && in_box(x, y, p->x, p->y, p->width, p->height))
            {
                c[n].kind = 2;
                c[n].index = 4 * q + j;
                c[n++].lit = char_lit(p, font, x, y);
            }
        }
    for (int i = 11; i >= 0; i--)
    {
        const i8244_char_info *p = &s.singles[i];

        if (p->visible && in_box(x, y, p->x, p->y, p->width, p->height))
        {
            c[n].kind = 1;
            c[n].index = i;
            c[n++].lit = char_lit(p, font, x, y);
        }
    }
    /* the ones with a lit pixel under the pointer first, then those whose
     * box only covers it, each in drawing priority */
    for (int pass = 1; pass >= 0; pass--)
        for (int i = 0; i < n && count < max; i++)
            if (c[i].lit == pass)
            {
                out[count].kind = c[i].kind;
                out[count].index = c[i].index;
                count++;
            }
    return count;
}

/* ---- the EF9340 ---------------------------------------------------------------- */

int o2debug_ef_present(o2debug *d)
{
    o2_machine_t *m;
    int present;

    (void)d;
    o2_host_lock();
    m = o2debug_machine();
    present = m && m->have_ef;
    o2_host_unlock();
    return present;
}

void o2debug_ef_get(o2debug *d, ef9340_snapshot_t *out)
{
    o2_machine_t *m;

    (void)d;
    memset(out, 0, sizeof *out);
    o2_host_lock();
    m = o2debug_machine();
    if (m && m->have_ef)
        ef9340_snapshot(&m->ef, out);
    o2_host_unlock();
}

int o2debug_ef_view(o2debug *d, uint32_t *dst, size_t cap, int *w, int *h)
{
    o2_machine_t *m;
    int ok = 0;

    (void)d;
    if (w)
        *w = EF9340_VIS_W;
    if (h)
        *h = EF9340_VIS_H;
    o2_host_lock();
    m = o2debug_machine();
    if (m && m->have_ef)
    {
        ok = 1;
        if (dst)
        {
            if (cap < (size_t)EF9340_VIS_W * EF9340_VIS_H)
                ok = 0;
            else
                ef9340_layer_rgb(&m->ef, dst, NULL);
        }
    }
    o2_host_unlock();
    return ok;
}

/* ---- text for the prompt -------------------------------------------------------- */

typedef struct {
    char *dst;
    int cap, len;
} textbuf;

static void put(textbuf *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (t->len >= t->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(t->dst + t->len, (size_t)(t->cap - t->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        t->len += n < t->cap - t->len ? n : t->cap - t->len - 1;
}

int o2debug_text_vdc(char *dst, int dstsz)
{
    textbuf t = { dst, dstsz, 0 };
    o2_machine_t *m;

    if (dstsz <= 0)
        return 0;
    dst[0] = '\0';
    o2_host_lock();
    m = o2debug_machine();
    if (!m)
        put(&t, "the machine is not running\n");
    else
    {
        i8244_snapshot s;
        char name[48], text[160];

        i8244_snapshot_get(&m->vdc, o2_machine_now(m), &s);
        put(&t, "%s: beam line %d dot %d (x %d y %d), frame %llu, %.2f Hz, INTR %d\n",
            s.is_pal ? "8245 (PAL)" : "8244 (NTSC)", s.vpos, s.hpos, s.x_beam, s.y_beam,
            (unsigned long long)s.frames, s.frame_hz, s.irq);
        for (unsigned a = 0xa0; a <= 0xaa; a++)
        {
            i8244_describe_register(&m->vdc, a, name, sizeof name, text, sizeof text);
            put(&t, "$%02X %-10s %s\n", a, name, text);
        }
        put(&t, "write lockouts: objects %s, grid %s; P17 LUM %s\n",
            s.lock_objects ? "locked" : "open", s.lock_grid ? "locked" : "open",
            s.p17_lum ? "on" : "off");
        put(&t, "font: %s\n", m->vdc.font ? "imported" : "NOT imported (glyphs are blank)");
    }
    o2_host_unlock();
    return t.len;
}

int o2debug_text_objects(char *dst, int dstsz)
{
    textbuf t = { dst, dstsz, 0 };
    o2_machine_t *m;

    if (dstsz <= 0)
        return 0;
    dst[0] = '\0';
    o2_host_lock();
    m = o2debug_machine();
    if (!m)
        put(&t, "the machine is not running\n");
    else
    {
        i8244_snapshot s;

        i8244_snapshot_get(&m->vdc, o2_machine_now(m), &s);
        for (int i = 0; i < 4; i++)
        {
            const i8244_sprite_info *p = &s.sprites[i];
            put(&t, "sprite %d  x %3d y %3d  %-6s%s%s%s  shape", i, p->x, p->y,
                i8244_colour_name(p->color), p->zoom ? " zoom" : "", p->s ? " S" : "",
                p->x9 ? " X9" : "");
            for (int r = 0; r < 8; r++)
                put(&t, " %02X", p->shape[r]);
            put(&t, "%s\n", !p->visible ? "  (off screen)" : p->clipped ? "  (clipped)" : "");
        }
        for (int i = 0; i < 12; i++)
        {
            const i8244_char_info *c = &s.singles[i];
            put(&t, "single %2d  x %3d y %3d  %-6s glyph %2d ptr %02X rows %d%s\n", i, c->x, c->y,
                i8244_colour_name(c->color), c->glyph, c->reg_ptr, c->rows,
                c->ntsc_hidden ? "  (hidden: NTSC Y<14)" : !c->visible ? "  (off screen)" : "");
        }
        for (int q = 0; q < 4; q++)
        {
            const i8244_quad_info *qd = &s.quads[q];
            put(&t, "quad %d    x %3d y %3d  rows %d  glyphs", q, qd->x, qd->y, qd->rows);
            for (int j = 0; j < 4; j++)
                put(&t, " %2d(%s)", qd->chars[j].glyph, i8244_colour_name(qd->chars[j].color));
            put(&t, "%s\n", qd->chars[0].ntsc_hidden ? "  (hidden: NTSC Y<14)" : "");
        }
    }
    o2_host_unlock();
    return t.len;
}

int o2debug_text_ef(char *dst, int dstsz)
{
    textbuf t = { dst, dstsz, 0 };
    o2_machine_t *m;

    if (dstsz <= 0)
        return 0;
    dst[0] = '\0';
    o2_host_lock();
    m = o2debug_machine();
    if (!m)
        put(&t, "the machine is not running\n");
    else if (!m->have_ef)
        put(&t, "Videopac+ G7400 only (this machine has no EF9340)\n");
    else
    {
        static ef9340_snapshot_t e;     /* ~10 KB; the host lock serialises */

        ef9340_snapshot(&m->ef, &e);
        put(&t, "EF9340: R=%02X (%s%s) M=%02X (%s) X=%d Y=%d Y0=%02X  font %s\n", e.r,
            e.display_on ? "display on" : "display off", e.box_mode ? ", boxed" : "", e.m,
            e.mode_name ? e.mode_name : "?", e.x, e.y, e.y0,
            e.font_present ? "imported" : "NOT imported");
        for (int r = 0; r < EF9340_ROWS; r++)
        {
            char line[EF9340_COLUMNS + 1];

            for (int c = 0; c < EF9340_COLUMNS; c++)
            {
                const ef9340_cell_t *cell = &e.page[r][c];
                line[c] = cell->kind == EF9340_CELL_ALNUM && cell->b >= 0x20 && cell->b < 0x7f
                              ? (char)cell->b : ' ';
            }
            line[EF9340_COLUMNS] = '\0';
            put(&t, "%2d |%s|\n", r, line);
        }
    }
    o2_host_unlock();
    return t.len;
}

int o2debug_text_voice(char *dst, int dstsz)
{
    textbuf t = { dst, dstsz, 0 };
    o2_machine_t *m;

    if (dstsz <= 0)
        return 0;
    dst[0] = '\0';
    o2_host_lock();
    m = o2debug_machine();
    if (!m)
        put(&t, "the machine is not running\n");
    else if (!m->have_voice)
        put(&t, "The Voice is not present\n");
    else
    {
        sp0256_debug_t v;

        sp0256_debug_get(&m->speech, &v);
        put(&t, "SP0256: LRQ %d SBY %d%s  speaking %d pending %d last %d  dropped %llu\n", v.lrq,
            v.sby, v.halted ? " halted" : "", v.allophone, v.pending, v.last_allophone,
            (unsigned long long)v.alds_dropped);
        put(&t, "  ROM $%04X.%u  %s  mode %u page $%X stack $%X  FIFO %u decles\n", v.rom_addr,
            v.rom_bit, v.fifo_sel ? "from FIFO" : "from ROM", v.mode, v.page, v.stack,
            v.fifo_count);
        put(&t, "  period %u amp %d rpt %d cnt %d%s%s  %u Hz clock, %u Hz samples, %llu made\n",
            v.period, v.amp, v.rpt, v.cnt, v.noise ? " noise" : "", v.silent ? " silent" : "",
            v.clock_hz, v.rate_hz, (unsigned long long)v.samples_made);
    }
    o2_host_unlock();
    return t.len;
}

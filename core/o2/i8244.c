/* i8244.c -- the Intel 8244 / 8245 video + sound chip (see i8244.h).
 *
 * Transposed from MAME's src/devices/video/i8244.cpp (BSD-3-Clause,
 * copyright-holders Peter Trauner, Wilbert Pol, hap) and the Odyssey 2 driver
 * src/mame/philips/odyssey2.cpp.  MAME's names are kept minus the m_ (reg,
 * collision_map, control_status, sh_prescaler ...), its register semantics,
 * rendering order and collision bookkeeping are transposed line for line.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * (transposed from MAME src/devices/video/i8244.cpp; copyright-holders
 *  Peter Trauner, Wilbert Pol, hap -- see COMPLIANCE.md)
 *
 * What is deliberately different from MAME:
 *  - Time.  MAME's scheduler/emu_timers become absolute pixel-clock times
 *    run against the CPU's clock (cdp1861.c's model).  screen().vpos()/
 *    hpos()/time_until_pos() are transposed exactly, including t = 0 being
 *    the start of VBLANK (line 243).
 *  - screen().update_now() / update_partial() are transposed (upd_pos /
 *    upd_scan): the lazy renderer is MAME's, so a mid-frame register write
 *    splits a line at the same pixel.  MAME's guard against rendering a
 *    frame twice ("m_last_partial_reset > machine().time()") cannot trigger
 *    with one CPU running at the scheduler's time and is dropped.
 *  - The palette index bitmap is 8-bit (MAME's is ind16, values 0..15); the
 *    P17 LUM step of odyssey2_state::screen_update is folded into the
 *    renderer (it can be taken over by the line callback, as vpp_state's is).
 *  - Sound: MAME's 3.58 MHz duty-cycle stream is replaced by an event list of
 *    (time, level) segments, box-averaged by i8244_sound_mix().  The 16-step
 *    PWM carrier (at the stream rate) is therefore averaged, not generated.
 *  - The font is a pointer given by the caller (MAME: a device ROM region).
 */

#include "i8244.h"

#include <stdio.h>
#include <string.h>

#define W I8244_WIDTH
#define H I8244_HEIGHT

/* ------------------------------------------------------------------------ */
/* palettes: odyssey2.cpp odyssey2_colors[] and i8244.cpp i8244_colors[]      */
/* ------------------------------------------------------------------------ */

static const uint8_t k_pal_o2[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0x79, 0x00, 0x00 }, { 0x00, 0x6d, 0x07 }, { 0x77, 0x67, 0x0b },
    { 0x1a, 0x37, 0xbe }, { 0x94, 0x30, 0x9f }, { 0x2a, 0xaa, 0xbe }, { 0xce, 0xce, 0xce },
    { 0x67, 0x67, 0x67 }, { 0xc7, 0x51, 0x51 }, { 0x56, 0xc4, 0x69 }, { 0xc6, 0xb8, 0x6a },
    { 0x5c, 0x80, 0xf6 }, { 0xdc, 0x84, 0xe8 }, { 0x77, 0xe6, 0xeb }, { 0xff, 0xff, 0xff },
};

static const uint8_t k_pal_rgb[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xb6, 0x00, 0x00 }, { 0x00, 0xb6, 0x00 }, { 0xb6, 0xb6, 0x00 },
    { 0x00, 0x00, 0xb6 }, { 0xb6, 0x00, 0xb6 }, { 0x00, 0xb6, 0xb6 }, { 0xb6, 0xb6, 0xb6 },
    { 0x49, 0x49, 0x49 }, { 0xff, 0x49, 0x49 }, { 0x49, 0xff, 0x49 }, { 0xff, 0xff, 0x49 },
    { 0x49, 0x49, 0xff }, { 0xff, 0x49, 0xff }, { 0x49, 0xff, 0xff }, { 0xff, 0xff, 0xff },
};

void i8244_set_palette(i8244_t *v, int kind)
{
    const uint8_t (*p)[3] = kind == I8244_PAL_RGB ? k_pal_rgb : k_pal_o2;

    v->palette_kind = kind == I8244_PAL_RGB ? I8244_PAL_RGB : I8244_PAL_O2;
    for (int i = 0; i < 16; i++)
        v->palette[i] = (uint32_t)p[i][0] << 16 | (uint32_t)p[i][1] << 8 | p[i][2];
}

const char *i8244_colour_name(int c)
{
    static const char *const n[8] = { "black", "red", "green", "yellow",
                                      "blue", "violet", "cyan", "white" };
    return n[c & 7];
}

void i8244_palette_colors(const i8244_t *v, uint32_t out[16])
{
    memcpy(out, v->palette, 16 * sizeof(uint32_t));
}

/* ------------------------------------------------------------------------ */
/* time and beam                                                              */
/* ------------------------------------------------------------------------ */

/* screen().vpos()/hpos() at an absolute time: the frame's pixel position
 * with t = 0 at the start of VBLANK (line 243). */
static inline void beam_hv(const i8244_t *v, uint64_t now, int *vp, int *hp)
{
    uint64_t p = (now + v->origin) % v->frame_clocks;

    *vp = (int)(p / (uint64_t)v->htotal);
    *hp = (int)(p % (uint64_t)v->htotal);
}

void i8244_beam(const i8244_t *v, uint64_t now, int *vpos, int *hpos)
{
    beam_hv(v, now, vpos, hpos);
}

/* screen().time_until_pos(vpos, hpos): a position the beam is already at or
 * past goes to the next frame. */
static uint64_t until_pos(const i8244_t *v, uint64_t now, int vpos, int hpos)
{
    int64_t target = (int64_t)vpos * v->htotal + hpos;
    int64_t cur = (int64_t)((now + v->origin) % v->frame_clocks);
    int64_t d = target - cur;

    if (d <= 0)
        d += (int64_t)v->frame_clocks;
    return (uint64_t)d;
}

/* get_y_beam() */
static int y_beam(const i8244_t *v, int vp, int hp)
{
    /* Y resets before hblank on the first scanline */
    if (vp == 0 && hp < v->hblank_start)
        vp = v->vtotal;
    /* Y increments on BG sync */
    if (hp >= v->bgate_start)
        vp++;
    return vp > 263 ? 263 : vp;
}

/* get_x_beam() */
static int x_beam(int hp)
{
    return hp >> 1;
}

int i8244_vblank(const i8244_t *v, uint64_t now)
{
    int vp, hp;
    int start = v->vblank_start;
    int end = v->vblank_end;

    beam_hv(v, now, &vp, &hp);
    if ((vp == start && hp >= (v->hblank_start - 1)) || (vp == end && hp <= (v->hblank_start - 1)))
        return 1;
    if (end < start)
        return (vp > start || vp < end) ? 1 : 0;
    return (vp > start && vp < end) ? 1 : 0;
}

int i8244_hblank(const i8244_t *v, uint64_t now)
{
    int vp, hp;
    int start = v->hblank_start;
    int end = v->hblank_end;

    beam_hv(v, now, &vp, &hp);
    (void)vp;
    if (end < start)
        return (hp >= start || hp < end) ? 1 : 0;
    return (hp >= start && hp < end) ? 1 : 0;
}

uint64_t i8244_t1_next_edge(const i8244_t *v, uint64_t now)
{
    int vp, hp;
    int level = i8244_t1(v, now);
    const int cand[3] = { v->hblank_start - 1, v->hblank_start, v->hblank_end };

    beam_hv(v, now, &vp, &hp);
    for (int line = 0; line <= v->vtotal + 1; line++)
    {
        for (int c = 0; c < 3; c++)
        {
            uint64_t t = now - (uint64_t)hp + (uint64_t)line * (uint64_t)v->htotal + (uint64_t)cand[c];

            if (t > now && i8244_t1(v, t) != level)
                return t;
        }
    }
    return now + v->frame_clocks; /* never changes (not reachable) */
}

/* ------------------------------------------------------------------------ */
/* sound event list                                                           */
/* ------------------------------------------------------------------------ */

static inline int snd_level(const i8244_t *v)
{
    /* sound_stream_update: (sh_output & sound >> 7) * 0.5 on a duty of
     * volume/16 */
    return (v->sh_output && (v->reg[0xaa] & 0x80)) ? (v->reg[0xaa] & 0xf) : 0;
}

/* The output level may have changed at time t. */
static void snd_note(i8244_t *v, uint64_t t)
{
    int level = snd_level(v);

    if (level == v->snd_cur)
        return;
    if (v->snd_count)
    {
        uint32_t last = (v->snd_head + v->snd_count - 1) % I8244_SND_EVENTS;

        if (v->snd_t[last] == t)
        {
            v->snd_level[last] = (uint8_t)level;
            v->snd_cur = (uint8_t)level;
            return;
        }
    }
    if (v->snd_count == I8244_SND_EVENTS)
    {
        /* nobody is mixing: forget the oldest change, keep the level it made */
        v->snd_base = v->snd_level[v->snd_head];
        v->snd_head = (v->snd_head + 1) % I8244_SND_EVENTS;
        v->snd_count--;
    }
    {
        uint32_t at = (v->snd_head + v->snd_count) % I8244_SND_EVENTS;

        v->snd_t[at] = t;
        v->snd_level[at] = (uint8_t)level;
        v->snd_count++;
        v->snd_cur = (uint8_t)level;
    }
}

void i8244_sound_mix(i8244_t *v, uint64_t t0, uint64_t t1, float *out, int n)
{
    uint32_t idx = 0;
    int level = v->snd_base;
    double span, scale;

    if (n <= 0)
        return;
    if (t1 <= t0)
    {
        for (int i = 0; i < n; i++)
            out[i] = (float)(level * (double)I8244_SOUND_GAIN / 16.0);
        return;
    }
    span = (double)(t1 - t0);
    scale = (double)I8244_SOUND_GAIN / 16.0;

    /* the level at t0 */
    while (idx < v->snd_count && v->snd_t[(v->snd_head + idx) % I8244_SND_EVENTS] <= t0)
        level = v->snd_level[(v->snd_head + idx++) % I8244_SND_EVENTS];

    for (int i = 0; i < n; i++)
    {
        double a = (double)t0 + span * (double)i / (double)n;
        double b = (double)t0 + span * (double)(i + 1) / (double)n;
        double pos = a, acc = 0.0;

        while (idx < v->snd_count)
        {
            uint32_t at = (v->snd_head + idx) % I8244_SND_EVENTS;
            double et = (double)v->snd_t[at];

            if (et >= b)
                break;
            if (et < pos)
                et = pos;
            acc += (double)level * (et - pos);
            pos = et;
            level = v->snd_level[at];
            idx++;
        }
        acc += (double)level * (b - pos);
        out[i] = (float)(acc / (b - a) * scale);
    }

    /* consumed */
    v->snd_head = (v->snd_head + idx) % I8244_SND_EVENTS;
    v->snd_count -= idx;
    v->snd_base = (uint8_t)level;
}

/* ------------------------------------------------------------------------ */
/* IRQ                                                                        */
/* ------------------------------------------------------------------------ */

static void irq_set(i8244_t *v, int level)
{
    if (v->irq == level)
        return;
    v->irq = level;
    if (v->irq_cb)
        v->irq_cb(v->irq_user, level);
}

/* ------------------------------------------------------------------------ */
/* rendering                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    const uint8_t *r;       /* the register file ($00-$FF)                  */
    const uint8_t *font;
    int ntsc;
    uint16_t *cmap;         /* collision map [0x200]                        */
    uint8_t *pmap;          /* priority map [0x200]                         */
    uint8_t *cstat;         /* collision_status ($A2)                       */
    uint8_t *kstat;         /* control_status ($A1 sticky bits)             */
    uint8_t *px;            /* the row being drawn, W palette indices       */
    uint8_t *hl;            /* optional highlight row (bit0 hit, bit1 overlap) */
    unsigned layers, flags;
    int x0, x1;             /* cliprect.min_x / max_x                       */
} rctx;

static inline int bitn(unsigned val, int n)
{
    return (val >> n) & 1;
}

static inline int font_byte(const rctx *c, int offset)
{
    return c->font ? c->font[offset & 0x1ff] : 0;
}

/* draw_grid() */
static void draw_grid(rctx *c, int scanline)
{
    const uint8_t *r = c->r;
    unsigned ctl = r[0xa0];
    unsigned col = r[0xa3];
    /* bitswap<4>(color, 6, 0, 1, 2) */
    uint8_t color = (uint8_t)((bitn(col, 6) << 3) | (bitn(col, 0) << 2) | (bitn(col, 1) << 1) | bitn(col, 2));
    const int x_grid_offset = 13;
    const int y_grid_offset = 24;
    const int width = 16;
    const int height = 24;
    int w = (ctl & 0x80) ? width : 2;

    /* horizontal part of the grid */
    if (c->layers & I8244_L_GRID)
    {
        for (int y = 0; y < 9; y++)
        {
            if (y_grid_offset + y * height <= scanline && scanline < y_grid_offset + y * height + 3)
            {
                for (int i = 0; i < 9; i++)
                {
                    if (bitn((unsigned)(r[0xd0 + i] << 8 | r[0xc0 + i]), y))
                    {
                        for (int k = 0; k < width + 2; k++)
                        {
                            int x = (x_grid_offset + i * width + k) * 2;

                            for (int px = x; px < x + 2; px++)
                            {
                                if (px >= c->x0 && px <= c->x1)
                                {
                                    c->cmap[px] |= 0x20;
                                    c->px[px] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* dots part of the grid */
    if ((ctl & 0x40) && (c->layers & I8244_L_DOTS))
    {
        for (int y = 0; y < 9; y++)
        {
            if (y_grid_offset + y * height <= scanline && scanline < y_grid_offset + y * height + 3)
            {
                for (int i = 0; i < 10; i++)
                {
                    for (int k = 0; k < 2; k++)
                    {
                        int x = (x_grid_offset + i * width + k) * 2;

                        for (int px = x; px < x + 2; px++)
                        {
                            if (px >= c->x0 && px <= c->x1)
                            {
                                c->cmap[px] |= 0x20;
                                c->px[px] = color;
                            }
                        }
                    }
                }
            }
        }
    }

    /* vertical part of the grid */
    if (c->layers & I8244_L_VGRID)
    {
        for (int j = 1, y = 0; y < 8; y++, j <<= 1)
        {
            if (y_grid_offset + y * height <= scanline && scanline < y_grid_offset + (y + 1) * height)
            {
                for (int i = 0; i < 10; i++)
                {
                    if (r[0xe0 + i] & j)
                    {
                        for (int k = 0; k < w; k++)
                        {
                            int x = (x_grid_offset + i * width + k) * 2;

                            for (int px = x; px < x + 2; px++)
                            {
                                if (px >= c->x0 && px <= c->x1)
                                {
                                    c->cmap[px] |= 0x10;
                                    c->px[px] = color;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/* major_pixel() */
static void major_pixel(rctx *c, unsigned index, int x, int y, int pixel, uint8_t color)
{
    const unsigned coll = c->r[0xa2];

    (void)y;
    for (int px = x; px < x + 2; px++)
    {
        if (px >= c->x0 && px <= c->x1)
        {
            uint8_t colx = (uint8_t)c->cmap[px];

            /* check collision with self */
            if (index < c->pmap[px])
            {
                *c->kstat |= 0x80;

                /* TODO (MAME): much more complex on actual console */
                if (colx & 0x80)
                    continue;
            }
            else
                c->pmap[px] = (uint8_t)index;

            if (pixel)
            {
                /* check if we collide with an already drawn source object */
                if (coll & colx)
                    *c->cstat |= 0x80;

                /* check if an already drawn object would collide with us */
                if (coll & 0x80)
                    *c->cstat |= colx;

                if (c->hl)
                {
                    if (colx)
                        c->hl[px] |= 2;
                    if ((coll & colx) || ((coll & 0x80) && colx))
                        c->hl[px] |= 1;
                }

                c->cmap[px] |= 0x80;
                c->px[px] = color;
            }
        }
    }
}

/* draw_major() */
static void draw_major(rctx *c, int scanline)
{
    const uint8_t *r = c->r;

    /* quad objects */
    if (c->layers & I8244_L_QUADS)
    {
        for (int i = 3; i >= 0; i--)
        {
            const uint8_t *q = r + 0x40 + i * 16;
            int y = q[0];

            if (c->ntsc && y < 0xe && !(c->flags & I8244_F_SHOW_HIDDEN))
                continue;

            /* character height is always determined by the height of the 4th character */
            int height = 7 - (((y >> 1) + q[3 * 4 + 2]) & 7);
            if (height == 0)
                height = 8;

            if (y <= scanline && scanline < y + height * 2)
            {
                int x = (q[1] + 5) * 2;

                /* MAME advances the SAME x in both loops: 8 pixels * 2 in
                 * the inner one and 16 more in the outer one, so a quad's
                 * characters are 32 pixels (16 x units) apart */
                for (int j = 0; j < 4; j++, x += 16)
                {
                    const uint8_t *s = q + j * 4;
                    int offset = (s[2] | ((s[3] & 0x01) << 8)) + (y >> 1) + ((scanline - y) >> 1);
                    uint8_t color = (uint8_t)(8 + ((s[3] >> 1) & 0x07));

                    for (int cx = 0; cx < 8; cx++, x += 2)
                        major_pixel(c, (unsigned)(4 * j + 16 * i + 0x40), x, scanline,
                                    bitn((unsigned)font_byte(c, offset), cx ^ 7), color);
                }
            }
        }
    }

    /* regular foreground objects */
    if (c->layers & I8244_L_SINGLES)
    {
        for (int i = 11; i >= 0; i--)
        {
            const uint8_t *s = r + 0x10 + i * 4;
            int y = s[0];

            if (c->ntsc && y < 0xe && !(c->flags & I8244_F_SHOW_HIDDEN))
                continue;

            int height = 7 - (((y >> 1) + s[2]) & 7);
            if (height == 0)
                height = 8;

            if (y <= scanline && scanline < y + height * 2)
            {
                int offset = (s[2] | ((s[3] & 0x01) << 8)) + (y >> 1) + ((scanline - y) >> 1);
                int x = (s[1] + 5) * 2;
                uint8_t color = (uint8_t)(8 + ((s[3] >> 1) & 0x07));

                for (int cx = 0; cx < 8; cx++, x += 2)
                    major_pixel(c, (unsigned)(4 * i + 0x10), x, scanline,
                                bitn((unsigned)font_byte(c, offset), cx ^ 7), color);
            }
        }
    }
}

/* draw_minor() */
static void draw_minor(rctx *c, int scanline)
{
    const uint8_t *r = c->r;
    const unsigned coll = r[0xa2];

    if (!(c->layers & I8244_L_SPRITES))
        return;

    /* minor system (sprites) */
    for (int i = 3; i >= 0; i--)
    {
        int y = r[i * 4];
        int height = 8;
        int zoom_enable = (r[i * 4 + 2] & 4) != 0;
        int zoom_px = zoom_enable ? 4 : 2;

        if (y <= scanline && scanline < y + height * zoom_px)
        {
            uint8_t color = (uint8_t)(8 + ((r[i * 4 + 2] >> 3) & 0x07));
            unsigned chr = r[0x80 + i * 8 + ((scanline - y) / zoom_px)];
            int x = (r[i * 4 + 1] + 5) * 2;
            int x_shift = 0;

            switch (r[i * 4 + 2] & 0x03)
            {
            case 1: /* X9 attribute set */
                x_shift = 1;
                break;
            case 2: /* S attribute set */
                x_shift = (((scanline - y) / zoom_px) & 0x01) ^ 0x01;
                break;
            case 3: /* X9 and S attributes set */
                x_shift = ((scanline - y) / zoom_px) & 0x01;
                break;
            default:
                break;
            }

            x += x_shift * (zoom_px / 2);

            for (unsigned m = 0x01; m < 0x100; m <<= 1, x += zoom_px)
            {
                if (chr & m)
                {
                    for (int px = x; px < x + zoom_px; px++)
                    {
                        if (px >= c->x0 && px <= c->x1)
                        {
                            /* put zoom flag on high byte for later collision detection */
                            uint16_t mask = (uint16_t)((zoom_enable ? 0x101 : 1) << i);
                            uint8_t colx = (uint8_t)c->cmap[px];

                            /* check if we collide with an already drawn source object */
                            if (coll & colx)
                                *c->cstat |= (uint8_t)mask;

                            /* check if an already drawn object would collide with us */
                            if (coll & mask)
                                *c->cstat |= colx;

                            if (c->hl)
                            {
                                if (colx)
                                    c->hl[px] |= 2;
                                if ((coll & colx) || ((coll & mask) && colx))
                                    c->hl[px] |= 1;
                            }

                            c->cmap[px] |= mask;
                            c->px[px] = color;
                        }
                    }
                }
            }
        }
    }
}

/* One scanline of screen_update() over x0..x1: background fill, collision
 * maps cleared when the line starts, grid, objects, the edge-case pass. */
static void render_row(rctx *c, int scanline)
{
    const uint8_t *r = c->r;
    unsigned ctl = r[0xa0];
    unsigned col = r[0xa3];
    int force = (c->flags & I8244_F_IGNORE_ENABLE) != 0;
    uint8_t bg = 0;

    /* draw background color: bitswap<3>(color, 3, 4, 5) */
    if (c->layers & I8244_L_BG)
        bg = (uint8_t)((bitn(col, 3) << 2) | (bitn(col, 4) << 1) | bitn(col, 5));
    memset(c->px + c->x0, bg, (size_t)(c->x1 - c->x0 + 1));

    /* clear collision maps */
    if (c->x0 == 0)
    {
        memset(c->cmap, 0, 0x200 * sizeof(uint16_t));
        memset(c->pmap, 0, 0x200);
    }

    /* display grid if enabled */
    if (((ctl & 0x08) || force) && scanline >= 24 && scanline <= 218)
        draw_grid(c, scanline);

    /* display objects if enabled */
    if (((ctl & 0x20) || force) && scanline <= 242)
    {
        draw_major(c, scanline);
        draw_minor(c, scanline);
    }

    /* go over the collision map again for edge cases on this scanline */
    {
        const unsigned coll = r[0xa2];

        for (int x = c->x0; x <= c->x1; x++)
        {
            if (x > 0)
            {
                uint16_t colx0 = c->cmap[x - 1];
                uint16_t colx1 = c->cmap[x];

                /* grid or minor to the left of major */
                if (colx1 & 0x80)
                {
                    if (coll & colx0 & 0x3f)
                        *c->cstat |= 0x80;

                    if (coll & 0x80)
                        *c->cstat |= (uint8_t)(colx0 & 0x3f);
                }

                /* grid to the left of non-zoomed minor */
                {
                    uint8_t mask = (uint8_t)((colx1 & 0xf) & ((~(unsigned)colx1) >> 8));

                    if (coll & colx0 & 0x30)
                        *c->cstat |= mask;

                    if (coll & mask)
                        *c->cstat |= (uint8_t)(colx0 & 0x30);
                }
            }
        }
    }
}

/* odyssey2_state::screen_update + i8244_device::screen_update for a clip
 * rectangle. */
static void render_clip(i8244_t *v, int y0, int y1, int x0, int x1)
{
    rctx c;

    if (y0 > y1 || x0 > x1)
        return;
    c.r = v->reg;
    c.font = v->font;
    c.ntsc = v->vtotal == 263;
    c.cmap = v->collision_map;
    c.pmap = v->priority_map;
    c.cstat = &v->collision_status;
    c.kstat = &v->control_status;
    c.hl = NULL;
    c.layers = I8244_L_ALL;
    c.flags = 0;
    c.x0 = x0;
    c.x1 = x1;

    for (int y = y0; y <= y1; y++)
    {
        c.px = v->bitmap + (size_t)y * W;
        render_row(&c, y);
        if (v->line_cb)
            v->line_cb(v->line_user, v, y, x0, x1);
        else if (v->lum)
        {
            /* apply external LUM setting */
            for (int x = x0; x <= x1; x++)
                c.px[x] |= (uint8_t)v->lum;
        }
    }
}

/* screen_device::update_partial(scanline): render from the last scanline up
 * to and including `scanline`. */
static void upd_pos(i8244_t *v, int vpos, int hpos);

static void upd_scan(i8244_t *v, int scanline)
{
    int top, bottom;

    /* skip if we already rendered this line */
    if (scanline < v->last_scan)
        return;

    /* if we left off in the middle of a scanline, eg. with update_now(),
     * finish that scanline first */
    if (v->part_hpos > 0)
    {
        upd_pos(v, v->last_scan + 1, 0);

        /* check again if scanline was already rendered */
        if (scanline < v->last_scan)
            return;
    }

    top = v->last_scan > 0 ? v->last_scan : 0;
    bottom = scanline < H - 1 ? scanline : H - 1;

    /* skip if entirely outside of visible area */
    if (top > bottom)
        return;

    render_clip(v, top, bottom, 0, W - 1);

    /* remember where we left off */
    v->last_scan = scanline + 1;
    v->part_hpos = 0;
}

/* screen_device::update_partial(vpos, hpos): from the last beam position up
 * to (but not including) the new one. */
static void upd_pos(i8244_t *v, int vpos, int hpos)
{
    /* skip if we already rendered this line */
    if (vpos < v->last_scan)
        return;

    /* if beam position is the same or less, there's nothing to update */
    if (vpos == v->last_scan && hpos <= v->part_hpos)
        return;

    /* start off by doing a partial update up to the line before us, in case
     * that was necessary */
    if (vpos > v->last_scan)
    {
        /* if the line before us was incomplete, we must do it in two pieces */
        if (v->part_hpos > 0)
        {
            /* now finish the previous partial scanline */
            if (v->last_scan >= 0 && v->last_scan < H && v->part_hpos < W)
                render_clip(v, v->last_scan, v->last_scan, v->part_hpos, W - 1);
            v->part_hpos = 0;
            v->last_scan++;
        }
        if (vpos > v->last_scan)
            upd_scan(v, vpos - 1);
    }

    /* now draw this partial scanline */
    if (hpos > 0)
    {
        int x0 = v->part_hpos > 0 ? v->part_hpos : 0;
        int x1 = hpos - 1 < W - 1 ? hpos - 1 : W - 1;

        if (vpos >= 0 && vpos < H && x0 <= x1)
            render_clip(v, vpos, vpos, x0, x1);
    }

    /* remember where we left off */
    v->part_hpos = hpos;
    v->last_scan = vpos;
}

/* screen().update_now() */
static void update_now(i8244_t *v, uint64_t now)
{
    int vp, hp;

    beam_hv(v, now, &vp, &hp);
    upd_pos(v, vp, hp);
}

void i8244_catch_up(i8244_t *v, uint64_t now)
{
    i8244_run_until(v, now);
    update_now(v, now);
}

static void publish_frame(i8244_t *v)
{
    for (size_t i = 0; i < (size_t)W * H; i++)
        v->frame[i] = v->palette[v->bitmap[i] & 15];
    v->frames++;
}

/* ------------------------------------------------------------------------ */
/* timers                                                                     */
/* ------------------------------------------------------------------------ */

static void sound_update(i8244_t *v, uint64_t t);

static void refresh_next(i8244_t *v)
{
    uint64_t n = v->next_hblank;

    if (v->next_vblank < n)
        n = v->next_vblank;
    if (v->next_svblank < n)
        n = v->next_svblank;
    if (v->next_sstart < n)
        n = v->next_sstart;
    v->next_event = n;
}

void i8244_run_until(i8244_t *v, uint64_t now)
{
    while (v->next_event <= now)
    {
        uint64_t t = v->next_event;

        if (v->next_hblank == t)
        {
            /* hblank starts (updates sound shift register) */
            v->next_hblank += (uint64_t)v->htotal;
            sound_update(v, t);
        }
        else if (v->next_vblank == t)
        {
            /* vblank starts */
            v->next_vblank += v->frame_clocks;
            v->control_status |= 0x08;
            irq_set(v, 1);
        }
        else if (v->next_svblank == t)
        {
            /* screen VBLANK: video_manager::frame_update() ->
             * update_partial(visarea.max_y), then the bitmap is the frame */
            v->next_svblank += v->frame_clocks;
            upd_scan(v, H - 1);
            publish_frame(v);
        }
        else
        {
            /* screen scanline 0: reset_partial_updates() */
            v->next_sstart += v->frame_clocks;
            v->last_scan = 0;
            v->part_hpos = 0;
        }
        refresh_next(v);
    }
}

/* ------------------------------------------------------------------------ */
/* I/O                                                                        */
/* ------------------------------------------------------------------------ */

static unsigned fix_register_mirrors(unsigned offset)
{
    offset &= 0xff;

    /* quad x/y registers are mirrored for each quad */
    if ((offset & 0xc2) == 0x40)
        offset &= ~0x0cu;

    /* registers $A0-$AF are mirrored at $B0-$BF */
    if ((offset & 0xe0) == 0xa0)
        offset &= ~0x10u;

    return offset & 0xff;
}

static uint8_t read_reg(const i8244_t *v, unsigned offset)
{
    uint8_t data;

    switch (offset)
    {
    case 0x02: case 0x06: case 0x0a: case 0x0e:
    case 0xa3: case 0xa7: case 0xa8: case 0xa9:
        /* write-only registers */
        data = 0;
        break;

    default:
        data = v->reg[offset];

        /* object x/y/attr registers are not accessible when display is
         * enabled (sprite shape registers still are) */
        if (offset < 0x80 && (v->reg[0xa0] & 0x20))
            data = 0;

        /* grid registers are not accessible when grid is enabled */
        else if (offset >= 0xc0 && (v->reg[0xa0] & 0x08))
            data = 0;
        break;
    }
    return data;
}

void i8244_set_access_lag(i8244_t *v, uint64_t lag_px)
{
    v->access_lag = lag_px;
}

/* the events that happen before an access at `now` (see i8244_set_access_lag) */
static inline void run_before_access(i8244_t *v, uint64_t now)
{
    i8244_run_until(v, now > v->access_lag ? now - v->access_lag : 0);
}

uint8_t i8244_read(i8244_t *v, uint64_t now, unsigned offset)
{
    uint8_t data;
    int vp, hp;

    run_before_access(v, now);
    offset = fix_register_mirrors(offset);

    /* update screen before accessing video status registers */
    if (offset == 0xa1 || offset == 0xa2)
        update_now(v, now);

    beam_hv(v, now, &vp, &hp);

    switch (offset)
    {
    case 0xa1:
        data = v->control_status;

        /* hstatus (not same as hblank), goes high at falling edge of X=0x70 */
        data |= (hp >= 225 && hp < v->bgate_start && y_beam(v, vp, hp) <= v->vblank_start) ? 1 : 0;

        /* position strobe status */
        data |= v->reg[0xa0] & 0x02;

        irq_set(v, 0);
        v->control_status &= (uint8_t)~0xcc;
        break;

    case 0xa2:
        data = v->collision_status;
        v->collision_status = 0;
        break;

    case 0xa4:
        data = (v->reg[0xa0] & 0x02) ? (uint8_t)y_beam(v, vp, hp) : v->y_beam_pos;
        break;

    case 0xa5:
        data = (v->reg[0xa0] & 0x02) ? (uint8_t)x_beam(hp) : v->x_beam_pos;
        break;

    default:
        data = read_reg(v, offset);
        break;
    }
    return data;
}

uint8_t i8244_peek(const i8244_t *v, uint64_t now, unsigned offset)
{
    int vp, hp;

    offset = fix_register_mirrors(offset);
    beam_hv(v, now, &vp, &hp);
    switch (offset)
    {
    case 0xa1:
        return (uint8_t)(v->control_status
                         | ((hp >= 225 && hp < v->bgate_start && y_beam(v, vp, hp) <= v->vblank_start) ? 1 : 0)
                         | (v->reg[0xa0] & 0x02));
    case 0xa2:
        return v->collision_status;
    case 0xa4:
        return (v->reg[0xa0] & 0x02) ? (uint8_t)y_beam(v, vp, hp) : v->y_beam_pos;
    case 0xa5:
        return (v->reg[0xa0] & 0x02) ? (uint8_t)x_beam(hp) : v->x_beam_pos;
    default:
        return read_reg(v, offset);
    }
}

/* data masks common to write() and poke() */
static uint8_t mask_data(unsigned offset, uint8_t data)
{
    /* color registers d4-d7 are not connected */
    if ((offset & 0x83) == 0x03)
        data &= 0x0f;

    /* major systems Y CAM d0 is not connected! */
    if (offset >= 0x10 && (offset & 0x83) == 0x00)
        data &= (uint8_t)~0x01;

    /* horizontal grid high byte only d0 is connected */
    if ((offset & 0xf0) == 0xd0)
        data &= 0x01;

    return data;
}

void i8244_write(i8244_t *v, uint64_t now, unsigned offset, uint8_t data)
{
    run_before_access(v, now);
    offset = fix_register_mirrors(offset);

    /* object x/y/attr registers are not accessible when display is enabled
     * (sprite shape registers still are) */
    if (offset < 0x80 && (v->reg[0xa0] & 0x20))
        return;

    /* grid registers are not accessible when grid is enabled
     * grid registers >= 0xf0 are unmapped */
    if ((offset >= 0xc0 && (v->reg[0xa0] & 0x08)) || offset >= 0xf0)
        return;

    /* update screen before accessing video registers */
    if ((offset <= 0xa0 || offset == 0xa2 || offset == 0xa3) && data != v->reg[offset])
        update_now(v, now);

    data = mask_data(offset, data);

    switch (offset)
    {
    case 0xa0:
        if ((v->reg[0xa0] & 0x02) && !(data & 0x02))
        {
            /* toggling strobe bit, tuck away values */
            int vp, hp;

            beam_hv(v, now, &vp, &hp);
            v->x_beam_pos = (uint8_t)x_beam(hp);
            v->y_beam_pos = (uint8_t)y_beam(v, vp, hp);
        }
        break;

    case 0xa7: case 0xa8: case 0xa9:
        v->sh_written = 1;
        break;

    case 0xaa:
        /* update the sound (the level up to now was the old one) */
        data &= (uint8_t)~0x40;
        v->reg[0xaa] = data;
        snd_note(v, now);
        return;

    case 0xa1: case 0xa4: case 0xa5:
        /* read-only registers */
        return;

    case 0x03: case 0x07: case 0x0b: case 0x0f:
    case 0xa6: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
    case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
    case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
    case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee: case 0xef:
        /* unused registers */
        return;

    default:
        break;
    }

    v->reg[offset] = data;
}

void i8244_poke(i8244_t *v, uint64_t now, unsigned offset, uint8_t data)
{
    i8244_catch_up(v, now);
    offset = fix_register_mirrors(offset);
    if (offset == 0xa1 || offset == 0xa2 || offset == 0xa4 || offset == 0xa5)
    {
        if (offset == 0xa2)
            v->collision_status = data;
        return;
    }
    data = mask_data(offset, data);
    if (offset >= 0xa7 && offset <= 0xa9)
        v->sh_written = 1;
    if (offset == 0xaa)
        data &= (uint8_t)~0x40;
    v->reg[offset] = data;
    if (offset == 0xaa)
        snd_note(v, now);
}

void i8244_set_p1_lum(i8244_t *v, uint64_t now, int p17)
{
    p17 = p17 ? 1 : 0;
    if (p17 == v->p17)
        return;
    /* LUM changed: the lines so far keep the old level */
    i8244_run_until(v, now);
    update_now(v, now);
    v->p17 = p17;
    v->lum = p17 ? 0 : 8;
}

void i8244_write_cx(i8244_t *v, int x, int cx)
{
    if (cx && x >= 0 && x < 0x200)
    {
        uint8_t colx = (uint8_t)v->collision_map[x];

        /* check if we collide with an already drawn source object */
        if (colx)
        {
            /* external overlap interrupt */
            if (v->reg[0xa0] & 0x10)
            {
                irq_set(v, 1);
                v->control_status |= 0x40;
            }

            if (colx & v->reg[0xa2])
                v->collision_status |= 0x40;
        }

        /* check if an already drawn object would collide with us */
        if (v->reg[0xa2] & 0x40)
            v->collision_status |= colx;
    }
}

/* ------------------------------------------------------------------------ */
/* sound shifter                                                              */
/* ------------------------------------------------------------------------ */

static void sound_update(i8244_t *v, uint64_t t)
{
    uint8_t *r = v->reg;
    uint8_t prescaler_mask;

    /* clock prescaler */
    v->sh_prescaler++;
    prescaler_mask = (r[0xaa] & 0x20) ? 3 : 0xf;
    if ((v->sh_prescaler & prescaler_mask) == 0)
        v->sh_pending = 1;

    /* clock shift registers */
    if (v->sh_pending && !v->sh_written)
    {
        uint32_t signal;
        int feedback;

        v->sh_pending = 0;

        signal = (uint32_t)r[0xa9] | ((uint32_t)r[0xa8] << 8) | ((uint32_t)r[0xa7] << 16);
        v->sh_output = (int)(signal & 1);
        feedback = v->sh_output;
        signal >>= 1;

        /* noise tap is on bits 0 and 5 and fed back to bit 15 */
        if (r[0xaa] & 0x10)
        {
            feedback ^= (int)(signal >> 4 & 1); /* pre-shift bit 5 */
            signal = (signal & ~0x8000u) | ((uint32_t)feedback << 15);
        }

        /* loop sound */
        signal |= (uint32_t)feedback << 23;

        r[0xa9] = (uint8_t)(signal & 0xff);
        r[0xa8] = (uint8_t)((signal >> 8) & 0xff);
        r[0xa7] = (uint8_t)((signal >> 16) & 0xff);

        /* the output level (and so the sound) changes here */
        snd_note(v, t);

        /* sound interrupt */
        if (++v->sh_count == 24)
        {
            v->sh_count = 0;
            if (r[0xa0] & 0x04)
            {
                v->control_status |= 0x04;
                irq_set(v, 1);
            }
        }
    }
    else if (v->sh_written)
    {
        v->sh_count = 0;
        v->sh_written = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* life cycle                                                                 */
/* ------------------------------------------------------------------------ */

void i8244_set_font(i8244_t *v, const uint8_t *font512)
{
    v->font = font512;
}

void i8244_set_irq_callback(i8244_t *v, i8244_irq_cb cb, void *user)
{
    v->irq_cb = cb;
    v->irq_user = user;
}

void i8244_set_line_callback(i8244_t *v, i8244_line_cb cb, void *user)
{
    v->line_cb = cb;
    v->line_user = user;
}

void i8244_reset(i8244_t *v, uint64_t now)
{
    /* device_start: zerofill */
    memset(v->reg, 0, sizeof v->reg);
    memset(v->collision_map, 0, sizeof v->collision_map);
    memset(v->priority_map, 0, sizeof v->priority_map);
    memset(v->bitmap, 0, sizeof v->bitmap);
    memset(v->frame, 0, sizeof v->frame);
    v->x_beam_pos = 0;
    v->y_beam_pos = 0;
    v->control_status = 0;
    v->collision_status = 0;
    v->sh_written = 0;
    v->sh_pending = 0;
    v->sh_prescaler = 0;
    v->sh_count = 0;
    v->sh_output = 0;
    v->irq = 0;
    v->p17 = 1;          /* odyssey2_state::m_p1 = 0xff */
    v->lum = 0;
    v->last_scan = 0;
    v->part_hpos = 0;
    v->frames = 0;
    v->snd_head = v->snd_count = 0;
    v->snd_base = v->snd_cur = 0;

    /* allocate timers: vblank at (vblank_start, hblank_start - 1) once a
     * frame, hblank at (0, hblank_start) every line, the screen's own VBLANK
     * start and scanline 0 */
    v->next_vblank = now + until_pos(v, now, v->vblank_start, v->hblank_start - 1);
    v->next_hblank = now + until_pos(v, now, 0, v->hblank_start);
    v->next_svblank = now + until_pos(v, now, H, 0);
    v->next_sstart = now + until_pos(v, now, 0, 0);
    refresh_next(v);
}

void i8244_init(i8244_t *v, int is_pal, const uint8_t *font512)
{
    memset(v, 0, sizeof *v);
    v->is_pal = is_pal ? 1 : 0;
    v->font = font512;
    if (v->is_pal)
    {
        /* i8245_device::set_default_params(): this timing is partially
         * derived externally, 8245 on the PAL console is set to slave mode
         * in vblank (M/S pin) */
        v->htotal = I8244_PAL_HTOTAL;
        v->vtotal = I8244_PAL_VTOTAL;
        v->vblank_start = 242;
        v->vblank_end = 312;
        v->hblank_start = 366;
        v->hblank_end = 454;
        v->bgate_start = 414;
        v->cycle_clocks = I8244_PAL_CYCLE_CLOCKS;
        v->pixel_hz = I8244_PAL_PIXEL_HZ;
    }
    else
    {
        v->htotal = I8244_NTSC_HTOTAL;
        v->vtotal = I8244_NTSC_VTOTAL;
        v->vblank_start = 242;
        v->vblank_end = 0;
        v->hblank_start = 366;
        v->hblank_end = 453;
        v->bgate_start = 413;
        v->cycle_clocks = I8244_NTSC_CYCLE_CLOCKS;
        v->pixel_hz = I8244_NTSC_PIXEL_HZ;
    }
    v->frame_clocks = (uint64_t)v->htotal * (uint64_t)v->vtotal;
    /* MAME's screen is at the start of VBLANK (line visarea.bottom() + 1 =
     * 243) at machine start */
    v->origin = (uint64_t)H * (uint64_t)v->htotal;
    i8244_set_palette(v, I8244_PAL_O2);
    i8244_reset(v, 0);
}

/* ------------------------------------------------------------------------ */
/* debugger: snapshot                                                         */
/* ------------------------------------------------------------------------ */

static void fill_char(const i8244_t *v, i8244_char_info *c, int index, const uint8_t *s)
{
    int y = s[0];

    (void)v;
    c->index = index;
    c->reg_y = s[0];
    c->reg_x = s[1];
    c->reg_ptr = s[2];
    c->reg_color = s[3];
    c->x = (s[1] + 5) * 2;
    c->y = y;
    c->color = (s[3] >> 1) & 7;
    c->bit8 = s[3] & 1;
    c->rom_offset = ((s[2] | ((s[3] & 1) << 8)) + (y >> 1)) & 0x1ff;
    c->glyph = c->rom_offset >> 3;
    c->first_row = c->rom_offset & 7;
}

static void fill_char_geom(const i8244_t *v, i8244_char_info *c, int height_ptr)
{
    /* the height comes from the (4th) character's pointer */
    int rows = 7 - (((c->y >> 1) + height_ptr) & 7);
    int ntsc = v->vtotal == 263;

    if (rows == 0)
        rows = 8;
    c->rows = rows;
    c->width = 16;
    c->height = rows * 2;
    c->ntsc_hidden = ntsc && c->y < 0xe;
    c->visible = !c->ntsc_hidden && c->x < W && c->x + c->width > 0 && c->y < H && c->y + c->height > 0;
    c->clipped = c->visible && (c->x < 0 || c->x + c->width > W || c->y + c->height > H);
}

static unsigned pattern_period(uint32_t sig)
{
    static const unsigned divs[] = { 1, 2, 3, 4, 6, 8, 12, 24 };

    for (unsigned i = 0; i < sizeof divs / sizeof divs[0]; i++)
    {
        unsigned p = divs[i];
        uint32_t rot = ((sig >> p) | (sig << (24 - p))) & 0xffffff;

        if (p == 24 || rot == (sig & 0xffffff))
            return p;
    }
    return 24;
}

void i8244_snapshot_get(const i8244_t *v, uint64_t now, i8244_snapshot *s)
{
    const uint8_t *r = v->reg;
    int vp, hp;

    memset(s, 0, sizeof *s);
    memcpy(s->regs, r, 0x100);
    beam_hv(v, now, &vp, &hp);

    s->is_pal = v->is_pal;
    s->pixel_hz = v->pixel_hz;
    s->line_hz = v->pixel_hz / v->htotal;
    s->frame_hz = v->pixel_hz / (double)v->frame_clocks;
    s->hpos = hp;
    s->vpos = vp;
    s->x_beam = x_beam(hp);
    s->y_beam = y_beam(v, vp, hp);
    s->x_beam_latched = v->x_beam_pos;
    s->y_beam_latched = v->y_beam_pos;
    s->last_scan = v->last_scan;
    s->part_hpos = v->part_hpos;
    s->frames = v->frames;
    s->irq = v->irq;

    s->control = r[0xa0];
    s->ctl_bit0 = r[0xa0] & 1;
    s->strobe = (r[0xa0] >> 1) & 1;
    s->sound_irq_en = (r[0xa0] >> 2) & 1;
    s->grid_en = (r[0xa0] >> 3) & 1;
    s->ext_irq_en = (r[0xa0] >> 4) & 1;
    s->fg_en = (r[0xa0] >> 5) & 1;
    s->dots_en = (r[0xa0] >> 6) & 1;
    s->fill = (r[0xa0] >> 7) & 1;
    s->lock_objects = s->fg_en;
    s->lock_grid = s->grid_en;

    s->status = i8244_peek(v, now, 0xa1);
    s->st_hstatus = s->status & 1;
    s->st_strobe = (s->status >> 1) & 1;
    s->st_sound = (s->status >> 2) & 1;
    s->st_vblank = (s->status >> 3) & 1;
    s->st_ext_overlap = (s->status >> 6) & 1;
    s->st_char_overlap = (s->status >> 7) & 1;

    s->col_mask = r[0xa2];
    s->col_result = v->collision_status;

    s->color_reg = r[0xa3];
    s->bg_color = (bitn(r[0xa3], 3) << 2) | (bitn(r[0xa3], 4) << 1) | bitn(r[0xa3], 5);
    s->grid_color = (bitn(r[0xa3], 0) << 2) | (bitn(r[0xa3], 1) << 1) | bitn(r[0xa3], 2);
    s->grid_lum = bitn(r[0xa3], 6);
    s->p17_lum = v->lum;

    s->snd_reg = r[0xaa];
    s->shift = (uint32_t)r[0xa9] | ((uint32_t)r[0xa8] << 8) | ((uint32_t)r[0xa7] << 16);
    s->snd_enable = (r[0xaa] >> 7) & 1;
    s->snd_fast = (r[0xaa] >> 5) & 1;
    s->snd_noise = (r[0xaa] >> 4) & 1;
    s->snd_volume = r[0xaa] & 0xf;
    s->sh_output = v->sh_output;
    s->sh_count = v->sh_count;
    s->sh_prescaler = v->sh_prescaler;
    s->sh_pending = v->sh_pending;
    s->sh_written = v->sh_written;
    s->shift_clock_hz = s->line_hz / (s->snd_fast ? 4.0 : 16.0);
    if (s->snd_noise)
    {
        s->pattern_period = 0;
        s->freq_hz = 0.0;
    }
    else
    {
        s->pattern_period = (int)pattern_period(s->shift);
        s->freq_hz = s->pattern_period == 1 ? 0.0 : s->shift_clock_hz / s->pattern_period;
    }

    /* sprites */
    for (int i = 0; i < 4; i++)
    {
        i8244_sprite_info *p = &s->sprites[i];
        int zpx;

        p->index = i;
        p->reg_y = r[i * 4];
        p->reg_x = r[i * 4 + 1];
        p->reg_color = r[i * 4 + 2];
        p->x = (p->reg_x + 5) * 2;
        p->y = p->reg_y;
        p->color = (p->reg_color >> 3) & 7;
        p->zoom = (p->reg_color >> 2) & 1;
        p->s = (p->reg_color >> 1) & 1;
        p->x9 = p->reg_color & 1;
        memcpy(p->shape, r + 0x80 + i * 8, 8);
        zpx = p->zoom ? 4 : 2;
        p->width = 8 * zpx + ((p->s || p->x9) ? zpx / 2 : 0);
        p->height = 8 * zpx;
        p->visible = p->x < W && p->x + p->width > 0 && p->y < H && p->y + p->height > 0;
        p->clipped = p->visible && (p->x < 0 || p->x + p->width > W || p->y + p->height > H);
    }

    /* singles */
    for (int i = 0; i < 12; i++)
    {
        i8244_char_info *c = &s->singles[i];
        const uint8_t *q = r + 0x10 + i * 4;

        fill_char(v, c, i, q);
        fill_char_geom(v, c, q[2]);
    }

    /* quads: the x/y registers are mirrored, the height comes from the 4th */
    for (int i = 0; i < 4; i++)
    {
        i8244_quad_info *q = &s->quads[i];
        const uint8_t *b = r + 0x40 + i * 16;

        q->index = i;
        q->x = (b[1] + 5) * 2;
        q->y = b[0];
        for (int j = 0; j < 4; j++)
        {
            uint8_t tmp[4];

            tmp[0] = b[0];
            tmp[1] = b[1];
            tmp[2] = b[j * 4 + 2];
            tmp[3] = b[j * 4 + 3];
            fill_char(v, &q->chars[j], 4 * i + j, tmp);
            q->chars[j].x = (b[1] + 5) * 2 + 32 * j;  /* MAME's shared x: 32 px pitch */
            fill_char_geom(v, &q->chars[j], b[3 * 4 + 2]);
        }
        q->rows = q->chars[0].rows;
    }

    /* grid */
    for (int i = 0; i < 9; i++)
        s->hgrid[i] = (uint16_t)((r[0xd0 + i] << 8 | r[0xc0 + i]) & 0x1ff);
    for (int i = 0; i < 10; i++)
        s->vgrid[i] = r[0xe0 + i];
    s->grid_x0 = 13;
    s->grid_y0 = 24;
    s->cell_w = 16;
    s->cell_h = 24;
}

/* ------------------------------------------------------------------------ */
/* debugger: register descriptions                                            */
/* ------------------------------------------------------------------------ */

void i8244_describe_register(const i8244_t *v, unsigned offset, char *name, size_t name_n,
                             char *text, size_t text_n)
{
    unsigned fixed = fix_register_mirrors(offset);
    uint8_t d = v->reg[fixed];
    char nm[48] = "";
    char tx[160] = "";
    static const char *const attr[4] = { "Y", "X", "PTR", "COLOR" };

    offset &= 0xff;
    if (fixed < 0x10)
    {
        int i = fixed >> 2;

        switch (fixed & 3)
        {
        case 0: snprintf(nm, sizeof nm, "SPRITE%d Y", i); snprintf(tx, sizeof tx, "y=%d", d); break;
        case 1: snprintf(nm, sizeof nm, "SPRITE%d X", i); snprintf(tx, sizeof tx, "x=%d (px %d)", d, (d + 5) * 2); break;
        case 2:
            snprintf(nm, sizeof nm, "SPRITE%d CTRL", i);
            snprintf(tx, sizeof tx, "colour %d (%s), zoom %d, S %d, X9 %d", (d >> 3) & 7,
                     i8244_colour_name((d >> 3) & 7), (d >> 2) & 1, (d >> 1) & 1, d & 1);
            break;
        default: snprintf(nm, sizeof nm, "SPRITE%d (unused)", i); break;
        }
    }
    else if (fixed < 0x40)
    {
        int i = (fixed - 0x10) >> 2;

        snprintf(nm, sizeof nm, "CHAR%d %s", i, attr[fixed & 3]);
        switch (fixed & 3)
        {
        case 0: snprintf(tx, sizeof tx, "y=%d%s", d, (v->vtotal == 263 && d < 14) ? " (NTSC: not drawn)" : ""); break;
        case 1: snprintf(tx, sizeof tx, "x=%d (px %d)", d, (d + 5) * 2); break;
        case 2: snprintf(tx, sizeof tx, "ptr=$%02X", d); break;
        default: snprintf(tx, sizeof tx, "colour %d (%s), font bit 8 = %d", (d >> 1) & 7, i8244_colour_name((d >> 1) & 7), d & 1); break;
        }
    }
    else if (fixed < 0x80)
    {
        int q = (fixed - 0x40) >> 4;
        int j = ((fixed - 0x40) >> 2) & 3;

        snprintf(nm, sizeof nm, "QUAD%d CHAR%d %s", q, j, attr[fixed & 3]);
        switch (fixed & 3)
        {
        case 0: snprintf(tx, sizeof tx, "y=%d (shared by the quad)", d); break;
        case 1: snprintf(tx, sizeof tx, "x=%d (px %d, shared; +32 px per char)", d, (d + 5) * 2); break;
        case 2: snprintf(tx, sizeof tx, "ptr=$%02X", d); break;
        default: snprintf(tx, sizeof tx, "colour %d (%s), font bit 8 = %d", (d >> 1) & 7, i8244_colour_name((d >> 1) & 7), d & 1); break;
        }
    }
    else if (fixed < 0xa0)
    {
        snprintf(nm, sizeof nm, "SHAPE%d ROW%d", (fixed - 0x80) >> 3, (fixed - 0x80) & 7);
        snprintf(tx, sizeof tx, "%02X", d);
        for (int b = 0; b < 8; b++)
            tx[2 + 1 + b] = (d >> b) & 1 ? '#' : '.';
        tx[2] = ' ';
        tx[11] = 0;
    }
    else if (fixed < 0xb0)
    {
        switch (fixed)
        {
        case 0xa0:
            snprintf(nm, sizeof nm, "CONTROL");
            snprintf(tx, sizeof tx, "%s%s%s%s%s%s%s%s", (d & 0x80) ? "fill " : "", (d & 0x40) ? "dots " : "",
                     (d & 0x20) ? "fg " : "", (d & 0x10) ? "ext-irq " : "", (d & 0x08) ? "grid " : "",
                     (d & 0x04) ? "snd-irq " : "", (d & 0x02) ? "strobe " : "", (d & 0x01) ? "b0" : "");
            break;
        case 0xa1:
            snprintf(nm, sizeof nm, "STATUS");
            snprintf(tx, sizeof tx, "read-only; sticky %02X", v->control_status);
            break;
        case 0xa2:
            snprintf(nm, sizeof nm, "COLLISION");
            snprintf(tx, sizeof tx, "mask %02X, latch %02X", d, v->collision_status);
            break;
        case 0xa3:
            snprintf(nm, sizeof nm, "COLOURS");
            snprintf(tx, sizeof tx, "bg %d (%s), grid %d (%s)%s", (bitn(d, 3) << 2) | (bitn(d, 4) << 1) | bitn(d, 5),
                     i8244_colour_name((bitn(d, 3) << 2) | (bitn(d, 4) << 1) | bitn(d, 5)),
                     (bitn(d, 0) << 2) | (bitn(d, 1) << 1) | bitn(d, 2),
                     i8244_colour_name((bitn(d, 0) << 2) | (bitn(d, 1) << 1) | bitn(d, 2)),
                     bitn(d, 6) ? " bright" : "");
            break;
        case 0xa4: snprintf(nm, sizeof nm, "Y BEAM"); snprintf(tx, sizeof tx, "latched %d", v->y_beam_pos); break;
        case 0xa5: snprintf(nm, sizeof nm, "X BEAM"); snprintf(tx, sizeof tx, "latched %d", v->x_beam_pos); break;
        case 0xa7: snprintf(nm, sizeof nm, "SHIFT1"); snprintf(tx, sizeof tx, "bits 23-16"); break;
        case 0xa8: snprintf(nm, sizeof nm, "SHIFT2"); snprintf(tx, sizeof tx, "bits 15-8"); break;
        case 0xa9: snprintf(nm, sizeof nm, "SHIFT3"); snprintf(tx, sizeof tx, "bits 7-0"); break;
        case 0xaa:
            snprintf(nm, sizeof nm, "SOUND");
            snprintf(tx, sizeof tx, "%s%s%s vol %d", (d & 0x80) ? "on " : "off ", (d & 0x20) ? "fast " : "slow ",
                     (d & 0x10) ? "noise" : "loop", d & 0xf);
            break;
        default: snprintf(nm, sizeof nm, "UNUSED"); break;
        }
    }
    else if (fixed < 0xc0)
        snprintf(nm, sizeof nm, "UNUSED");
    else if (fixed < 0xd0)
    {
        snprintf(nm, sizeof nm, "HGRID COL%d LOW", fixed - 0xc0);
        snprintf(tx, sizeof tx, "lines 0-7: %02X", d);
    }
    else if (fixed < 0xe0)
    {
        snprintf(nm, sizeof nm, "HGRID COL%d HIGH", fixed - 0xd0);
        snprintf(tx, sizeof tx, "line 8: %d", d & 1);
    }
    else if (fixed < 0xf0)
    {
        snprintf(nm, sizeof nm, "VGRID COL%d", fixed - 0xe0);
        snprintf(tx, sizeof tx, "rows 0-7: %02X", d);
    }
    else
        snprintf(nm, sizeof nm, "UNMAPPED");

    if (offset != fixed && fixed < 0x80 && (offset & 0xc2) == 0x40)
        snprintf(nm + strlen(nm), sizeof nm - strlen(nm), " (mirror)");
    else if (offset != fixed)
        snprintf(nm + strlen(nm), sizeof nm - strlen(nm), " (mirror of $%02X)", fixed);

    if (name && name_n)
        snprintf(name, name_n, "%s", nm);
    if (text && text_n)
        snprintf(text, text_n, "%s", tx);
}

/* ------------------------------------------------------------------------ */
/* debugger: renderers                                                        */
/* ------------------------------------------------------------------------ */

static inline uint32_t blend(uint32_t a, uint32_t b)
{
    return ((a & 0xfefefeu) >> 1) + ((b & 0xfefefeu) >> 1);
}

static void put_px(uint32_t *dst, int dw, int dh, int x, int y, uint32_t col)
{
    if (x >= 0 && y >= 0 && x < dw && y < dh)
        dst[(size_t)y * (size_t)dw + (size_t)x] = col;
}

static void draw_box(uint32_t *dst, int dw, int dh, int x, int y, int w, int h, uint32_t col)
{
    for (int i = 0; i < w; i++)
    {
        put_px(dst, dw, dh, x + i, y, col);
        put_px(dst, dw, dh, x + i, y + h - 1, col);
    }
    for (int j = 0; j < h; j++)
    {
        put_px(dst, dw, dh, x, y + j, col);
        put_px(dst, dw, dh, x + w - 1, y + j, col);
    }
}

/* 3x5 digits for the index labels */
static const uint8_t k_digits[10][5] = {
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

static void draw_label(uint32_t *dst, int dw, int dh, int x, int y, int num, uint32_t col)
{
    char buf[8];
    int n = snprintf(buf, sizeof buf, "%d", num);

    for (int k = 0; k < n; k++)
        for (int row = 0; row < 5; row++)
            for (int cx = 0; cx < 3; cx++)
                if ((k_digits[buf[k] - '0'][row] >> (2 - cx)) & 1)
                    put_px(dst, dw, dh, x + k * 4 + cx, y + row, col);
}

void i8244_debug_render(const i8244_t *v, unsigned layers, unsigned flags,
                        uint32_t *dst, int dw, int dh)
{
    uint16_t cmap[0x200];
    uint8_t pmap[0x200];
    uint8_t row[W];
    uint8_t hl[W];
    uint8_t cs = 0, ks = 0;
    rctx c;
    int cw = dw < W ? dw : W;
    int ch = dh < H ? dh : H;

    if (dw <= 0 || dh <= 0)
        return;
    memset(dst, 0, (size_t)dw * (size_t)dh * sizeof(uint32_t));
    memset(cmap, 0, sizeof cmap);
    memset(pmap, 0, sizeof pmap);

    c.r = v->reg;
    c.font = v->font;
    c.ntsc = v->vtotal == 263;
    c.cmap = cmap;
    c.pmap = pmap;
    c.cstat = &cs;
    c.kstat = &ks;
    c.px = row;
    c.hl = hl;
    c.layers = layers;
    c.flags = flags;
    c.x0 = 0;
    c.x1 = W - 1;

    for (int y = 0; y < ch; y++)
    {
        memset(hl, 0, sizeof hl);
        render_row(&c, y);
        for (int x = 0; x < cw; x++)
        {
            uint32_t col = v->palette[(row[x] | v->lum) & 15];

            if ((flags & I8244_F_COLLISION) && (hl[x] & 1))
                col = blend(col, 0xff0000u);
            else if ((flags & I8244_F_OVERLAP) && (hl[x] & 2))
                col = blend(col, 0xffff00u);
            dst[(size_t)y * (size_t)dw + (size_t)x] = col;
        }
    }

    if (flags & I8244_F_BOXES)
    {
        i8244_snapshot snap;

        i8244_snapshot_get(v, 0, &snap);
        if (layers & I8244_L_SPRITES)
            for (int i = 0; i < 4; i++)
            {
                const i8244_sprite_info *p = &snap.sprites[i];

                draw_box(dst, dw, ch, p->x, p->y, p->width, p->height, 0xff40ffu);
                draw_label(dst, dw, ch, p->x + 1, p->y + 1, i, 0xff40ffu);
            }
        if (layers & I8244_L_SINGLES)
            for (int i = 0; i < 12; i++)
            {
                const i8244_char_info *p = &snap.singles[i];

                if (p->ntsc_hidden && !(flags & I8244_F_SHOW_HIDDEN))
                    continue;
                draw_box(dst, dw, ch, p->x, p->y, p->width, p->height, 0x40ffffu);
                draw_label(dst, dw, ch, p->x + 1, p->y + 1, i, 0x40ffffu);
            }
        if (layers & I8244_L_QUADS)
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                {
                    const i8244_char_info *p = &snap.quads[i].chars[j];

                    if (p->ntsc_hidden && !(flags & I8244_F_SHOW_HIDDEN))
                        continue;
                    draw_box(dst, dw, ch, p->x, p->y, p->width, p->height, 0x90ff40u);
                    if (j == 0)
                        draw_label(dst, dw, ch, p->x + 1, p->y + 1, i, 0x90ff40u);
                }
    }
}

void i8244_frame_in_progress(const i8244_t *v, uint64_t now, unsigned flags,
                             uint32_t *dst, int *bx, int *by)
{
    int vp, hp;
    int last = v->last_scan;
    int part = v->part_hpos;

    beam_hv(v, now, &vp, &hp);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            uint32_t col = v->palette[v->bitmap[(size_t)y * W + x] & 15];
            int done = y < last || (y == last && x < part);

            dst[(size_t)y * W + x] = done ? col : ((col & 0xfefefeu) >> 1);
        }
    if (flags & I8244_F_BEAM)
    {
        for (int x = 0; x < W; x++)
            if (vp < H)
                dst[(size_t)vp * W + x] = blend(dst[(size_t)vp * W + x], 0xffffffu);
        for (int y = 0; y < H; y++)
            if (hp < W)
                dst[(size_t)y * W + hp] = blend(dst[(size_t)y * W + hp], 0xffffffu);
    }
    if (bx)
        *bx = hp;
    if (by)
        *by = vp;
}

/* ---- object bitmaps ------------------------------------------------------ */

static int begin_bitmap(uint32_t *dst, size_t cap, int w, int h, uint32_t bg, int *ow, int *oh)
{
    *ow = w;
    *oh = h;
    if (!dst)
        return 0;
    if (cap < (size_t)w * (size_t)h)
        return -1;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        dst[i] = bg;
    return 0;
}

static void blit_block(uint32_t *dst, int w, int x, int y, int scale, uint32_t col)
{
    for (int j = 0; j < scale; j++)
        for (int i = 0; i < scale; i++)
            dst[(size_t)(y * scale + j) * (size_t)w + (size_t)(x * scale + i)] = col;
}

int i8244_render_sprite(const i8244_t *v, int index, int scale, uint32_t bg,
                        uint32_t *dst, size_t cap, int *w, int *h)
{
    const uint8_t *r = v->reg;
    int ow, oh;

    if (index < 0 || index > 3 || scale < 1)
        return -1;
    if (begin_bitmap(dst, cap, 8 * scale, 8 * scale, bg, &ow, &oh) < 0)
        return -1;
    *w = ow;
    *h = oh;
    if (dst)
    {
        uint32_t col = v->palette[8 + ((r[index * 4 + 2] >> 3) & 7)];

        for (int row = 0; row < 8; row++)
            for (int cx = 0; cx < 8; cx++)
                if ((r[0x80 + index * 8 + row] >> cx) & 1)
                    blit_block(dst, ow, cx, row, scale, col);
    }
    return 0;
}

int i8244_render_char(const i8244_t *v, int kind, int index, int scale, uint32_t bg,
                      uint32_t *dst, size_t cap, int *w, int *h)
{
    {
        const uint8_t *r = v->reg;
        const uint8_t *s;
        int y, ptr, rows, ow, oh, offset;
        uint32_t col;

        if (scale < 1)
            return -1;
        if (kind == 0)
        {
            if (index < 0 || index > 11)
                return -1;
            s = r + 0x10 + index * 4;
            y = s[0];
            ptr = s[2];
            rows = 7 - (((y >> 1) + ptr) & 7);
        }
        else
        {
            const uint8_t *q;

            if (index < 0 || index > 15)
                return -1;
            q = r + 0x40 + (index >> 2) * 16;
            s = q + (index & 3) * 4;
            y = q[0];
            ptr = s[2];
            rows = 7 - (((y >> 1) + q[3 * 4 + 2]) & 7);
        }
        if (rows == 0)
            rows = 8;
        if (begin_bitmap(dst, cap, 8 * scale, rows * scale, bg, &ow, &oh) < 0)
            return -1;
        *w = ow;
        *h = oh;
        if (dst)
        {
            col = v->palette[8 + ((s[3] >> 1) & 7)];
            offset = ((ptr | ((s[3] & 1) << 8)) + (y >> 1));
            for (int row = 0; row < rows; row++)
            {
                unsigned b = v->font ? v->font[(offset + row) & 0x1ff] : 0;

                for (int cx = 0; cx < 8; cx++)
                    if ((b >> (cx ^ 7)) & 1)
                        blit_block(dst, ow, cx, row, scale, col);
            }
        }
    }
    return 0;
}

int i8244_render_glyph(const i8244_t *v, int glyph, int scale, uint32_t fg, uint32_t bg,
                       uint32_t *dst, size_t cap, int *w, int *h)
{
    int ow, oh;

    if (glyph < 0 || glyph > 63 || scale < 1)
        return -1;
    if (begin_bitmap(dst, cap, 8 * scale, 8 * scale, bg, &ow, &oh) < 0)
        return -1;
    *w = ow;
    *h = oh;
    if (dst)
        for (int row = 0; row < 8; row++)
        {
            unsigned b = v->font ? v->font[glyph * 8 + row] : 0;

            for (int cx = 0; cx < 8; cx++)
                if ((b >> (cx ^ 7)) & 1)
                    blit_block(dst, ow, cx, row, scale, fg);
        }
    return 0;
}

int i8244_render_font_sheet(const i8244_t *v, int scale, uint32_t fg, uint32_t bg,
                            uint32_t *dst, size_t cap, int *w, int *h)
{
    int ow, oh;

    if (scale < 1)
        return -1;
    /* 16 glyphs a row, 4 rows, 1 logical pixel between */
    if (begin_bitmap(dst, cap, (16 * 9 - 1) * scale, (4 * 9 - 1) * scale, bg, &ow, &oh) < 0)
        return -1;
    *w = ow;
    *h = oh;
    if (dst)
        for (int g = 0; g < 64; g++)
        {
            int gx = (g % 16) * 9;
            int gy = (g / 16) * 9;

            for (int row = 0; row < 8; row++)
            {
                unsigned b = v->font ? v->font[g * 8 + row] : 0;

                for (int cx = 0; cx < 8; cx++)
                    if ((b >> (cx ^ 7)) & 1)
                        blit_block(dst, ow, gx + cx, gy + row, scale, fg);
            }
        }
    return 0;
}

int i8244_render_shape_sheet(const i8244_t *v, int scale, uint32_t bg,
                             uint32_t *dst, size_t cap, int *w, int *h)
{
    const uint8_t *r = v->reg;
    int ow, oh;

    if (scale < 1)
        return -1;
    if (begin_bitmap(dst, cap, (4 * 9 - 1) * scale, 8 * scale, bg, &ow, &oh) < 0)
        return -1;
    *w = ow;
    *h = oh;
    if (dst)
        for (int i = 0; i < 4; i++)
        {
            uint32_t col = v->palette[8 + ((r[i * 4 + 2] >> 3) & 7)];

            for (int row = 0; row < 8; row++)
                for (int cx = 0; cx < 8; cx++)
                    if ((r[0x80 + i * 8 + row] >> cx) & 1)
                        blit_block(dst, ow, i * 9 + cx, row, scale, col);
        }
    return 0;
}

int i8244_render_palette_strip(const i8244_t *v, int cell, uint32_t *dst, size_t cap, int *w, int *h)
{
    int ow, oh;

    if (cell < 1)
        return -1;
    if (begin_bitmap(dst, cap, 8 * cell, 2 * cell, 0, &ow, &oh) < 0)
        return -1;
    *w = ow;
    *h = oh;
    if (dst)
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < cell; j++)
                for (int k = 0; k < cell; k++)
                    dst[(size_t)((i / 8) * cell + j) * (size_t)ow + (size_t)((i % 8) * cell + k)] = v->palette[i];
    return 0;
}

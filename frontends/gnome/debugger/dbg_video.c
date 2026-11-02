/*
 * The debugger's Video tab (the 8244/8245) and its EF9340 tab (the Videopac+
 * G7400's text chip).
 *
 * The Video tab shows everything the chip knows, from one snapshot
 * (o2debug_vdc_get) per refresh:
 *
 *   left   the display at 2x, nearest neighbour: the frame the machine
 *          finished, the CURRENT registers re-rendered with chosen layers and
 *          overlays, or the frame as far as the beam has got; with the
 *          pointer over it, the object under it (o2debug_vdc_hit_test),
 *          named and outlined;
 *   right  six sections: Objects (the four sprites, twelve singles and the
 *          four quads of four characters, each with its position, its
 *          registers and its bitmap), Grid (its 9 x 10 lines drawn), Registers
 *          (all 256, decoded, editable), Control/Status ($A0-$A3 decoded),
 *          Sound (the shift register and what it makes) and Sheets (the font,
 *          the sprite shapes, the palette).
 *
 * Idle cost: the caller refreshes this tab only while it shows, and only the
 * section that shows is redrawn. The picture is rendered by the core into one
 * reused buffer and a GPU texture is built only when its pixels changed (a
 * stopped machine costs nothing, a running one at most ten uploads a
 * second); an object's bitmap is fetched only when its registers changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_common.h"

#include <math.h>
#include <string.h>

#define FRAME_W I8244_WIDTH
#define FRAME_H I8244_HEIGHT
#define SCALE 2                 /* the picture's pixels: nearest, as the core scales */

enum { VIEW_FRAME, VIEW_RENDER, VIEW_PROGRESS };
enum { SEC_OBJECTS, SEC_GRID, SEC_REGS, SEC_CONTROL, SEC_SOUND, SEC_SHEETS, SEC_COUNT };

#define NLAYERS 7
#define NFLAGS 6
/* the checkboxes' order and what each means to the core */
static const struct { const char *name; unsigned bit; } k_layers[NLAYERS] = {
    { "Background", I8244_L_BG }, { "Grid", I8244_L_GRID }, { "Vertical grid", I8244_L_VGRID },
    { "Dots", I8244_L_DOTS }, { "Sprites", I8244_L_SPRITES }, { "Singles", I8244_L_SINGLES },
    { "Quads", I8244_L_QUADS },
};
static const struct { const char *name; unsigned bit; } k_flags[NFLAGS] = {
    { "Bounding boxes", I8244_F_BOXES }, { "Collision highlight", I8244_F_COLLISION },
    { "Overlap highlight", I8244_F_OVERLAP }, { "Show hidden", I8244_F_SHOW_HIDDEN },
    { "Ignore enables", I8244_F_IGNORE_ENABLE }, { "Beam cross-hair", I8244_F_BEAM },
};
enum { FLAG_BOXES, FLAG_COLLISION, FLAG_OVERLAP, FLAG_HIDDEN, FLAG_IGNORE, FLAG_BEAM };

/* An object's row in the tables: its bitmap and its text, and the registers
 * the bitmap was last fetched for. */
typedef struct {
    GtkPicture *pic;
    GtkWidget *label;
    uint8_t sig[16];
    gboolean have_sig;
} ObjRow;

/* A fixed-size image drawn with cairo (image_area_new below). */
typedef struct {
    GtkWidget *widget;
    uint32_t *px;
    int w, h;
} ImageArea;

struct DbgVideo {
    o2session *session;
    o2debug *dbg;
    i8244_snapshot snap;
    int tick;                   /* live refreshes since the sections last redrew */

    /* the picture */
    GtkDropDown *view;
    GtkCheckButton *layer[NLAYERS], *flag[NFLAGS];
    GtkPicture *pic;
    GtkWidget *overlay_area;
    GtkLabel *hover_label;
    uint32_t *buf, *prev;       /* the rendered view, and the one on the GPU */
    size_t cap;                 /* pixels each holds */
    int pw, ph;
    int mx, my;                 /* the pointer, in frame pixels */
    gboolean over;
    o2debug_hit hit[4];
    int nhit;

    GtkNotebook *sections;

    /* objects */
    ObjRow spr[4], single[12], quad[16];
    GtkLabel *quad_head[4];

    /* grid */
    GtkWidget *grid_area;
    GtkLabel *grid_text;
    uint32_t palette[16];
    gboolean have_palette;

    /* registers */
    GtkWidget *reg_row[256];
    GtkLabel *reg_name[256], *reg_text[256];
    GtkEntry *reg_val[256];
    uint8_t reg_shown[256];
    gboolean reg_built;

    /* control/status, sound */
    GtkLabel *control_text, *sound_text;
    GtkWidget *shift_area;

    /* sheets */
    ImageArea *sheet_font, *sheet_shapes, *sheet_palette;
};

/* ---- images ------------------------------------------------------------------- */

static void set_picture(GtkPicture *pic, const uint32_t *px, int w, int h)
{
    GBytes *bytes = g_bytes_new(px, (gsize)w * (gsize)h * 4);
    GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8X8, bytes, (gsize)w * 4);
    gtk_picture_set_paintable(pic, GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(GTK_WIDGET(pic), w, h);
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

/* A fixed-size image drawn with cairo, for the pictures whose size varies:
 * a GtkPicture asked for its height at a width answers with the aspect
 * ratio's, which stretches it when its column is wider than the image. A
 * drawing area is exactly the size it is given. */

static void image_area_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    ImageArea *ia = ud;
    cairo_surface_t *surf;
    (void)area; (void)width; (void)height;
    if (!ia->px) return;
    surf = cairo_image_surface_create_for_data((unsigned char *)ia->px, CAIRO_FORMAT_RGB24,
                                               ia->w, ia->h, ia->w * 4);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surf);
}

static void image_area_free(gpointer p)
{
    ImageArea *ia = p;
    g_free(ia->px);
    g_free(ia);
}

static ImageArea *image_area_new(void)
{
    ImageArea *ia = g_new0(ImageArea, 1);
    ia->widget = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ia->widget), image_area_draw, ia, NULL);
    gtk_widget_set_halign(ia->widget, GTK_ALIGN_START);
    gtk_widget_set_valign(ia->widget, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(ia->widget), "image-area", ia, image_area_free);
    return ia;
}

static void image_area_set(ImageArea *ia, const uint32_t *px, int w, int h)
{
    if (ia->w != w || ia->h != h) {
        g_free(ia->px);
        ia->px = g_new(uint32_t, (gsize)w * (gsize)h);
        ia->w = w;
        ia->h = h;
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(ia->widget), w);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(ia->widget), h);
    }
    memcpy(ia->px, px, (gsize)w * (gsize)h * 4);
    gtk_widget_queue_draw(ia->widget);
}

static GtkPicture *new_picture(void)
{
    GtkPicture *pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(pic), GTK_ALIGN_START);
    return pic;
}

/* The core's view `kind` into the shared buffer, and onto `pic` when its
 * pixels differ from what `prev` (if any) last showed. */
static gboolean show_view(DbgVideo *v, GtkPicture *pic, ImageArea *area, int kind,
                          unsigned layers, unsigned flags, int scale, gboolean cache)
{
    int w = 0, h = 0;
    if (!o2debug_vdc_view(v->dbg, kind, layers, flags, scale, v->buf, v->cap, &w, &h) ||
        w <= 0 || h <= 0 || (size_t)w * (size_t)h > v->cap)
        return FALSE;
    if (cache) {
        if (v->pw == w && v->ph == h &&
            memcmp(v->buf, v->prev, (size_t)w * (size_t)h * 4) == 0)
            return FALSE;
        memcpy(v->prev, v->buf, (size_t)w * (size_t)h * 4);
        v->pw = w;
        v->ph = h;
    }
    if (pic)
        set_picture(pic, v->buf, w, h);
    else
        image_area_set(area, v->buf, w, h);
    return TRUE;
}

static void fetch_palette(DbgVideo *v)
{
    int w = 0, h = 0, cell, k;
    if (v->have_palette) return;
    if (!o2debug_vdc_view(v->dbg, O2DEBUG_VDC_PALETTE, 0, 0, 1, v->buf, v->cap, &w, &h) || w < 128)
        return;
    cell = w / 8;
    for (k = 0; k < 8; k++) {
        v->palette[k] = v->buf[(size_t)(cell / 2) * w + k * cell + cell / 2];
        v->palette[8 + k] = v->buf[(size_t)(cell + cell / 2) * w + k * cell + cell / 2];
    }
    v->have_palette = TRUE;
}

/* ---- the picture ----------------------------------------------------------------- */

static unsigned layer_mask(DbgVideo *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < NLAYERS; i++)
        if (gtk_check_button_get_active(v->layer[i]))
            m |= k_layers[i].bit;
    return m;
}

static unsigned flag_mask(DbgVideo *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < NFLAGS; i++)
        if (gtk_check_button_get_active(v->flag[i]))
            m |= k_flags[i].bit;
    return m;
}

static int view_now(DbgVideo *v)
{
    guint sel = gtk_drop_down_get_selected(v->view);
    return sel == VIEW_RENDER || sel == VIEW_PROGRESS ? (int)sel : VIEW_FRAME;
}

/* What each view can use: the layers and most overlays belong to the
 * re-render; the frame in progress takes the beam; the boxes are drawn here
 * over the frame and the frame in progress (the core draws them in the
 * re-render). */
static void sync_sensitivity(DbgVideo *v)
{
    const int view = view_now(v);
    int i;
    for (i = 0; i < NLAYERS; i++)
        gtk_widget_set_sensitive(GTK_WIDGET(v->layer[i]), view == VIEW_RENDER);
    for (i = 0; i < NFLAGS; i++) {
        gboolean on = view == VIEW_RENDER ? i != FLAG_BEAM
                    : view == VIEW_PROGRESS ? (i == FLAG_BOXES || i == FLAG_BEAM)
                    : i == FLAG_BOXES;
        gtk_widget_set_sensitive(GTK_WIDGET(v->flag[i]), on);
    }
}

static void picture_update(DbgVideo *v)
{
    const int view = view_now(v);
    const unsigned core_kind = view == VIEW_RENDER ? O2DEBUG_VDC_RENDER
                             : view == VIEW_PROGRESS ? O2DEBUG_VDC_PROGRESS : O2DEBUG_VDC_FRAME;
    unsigned flags = flag_mask(v);

    if (view != VIEW_RENDER)
        flags &= I8244_F_BEAM;       /* the boxes are drawn over it, below */
    if (show_view(v, v->pic, NULL, (int)core_kind, layer_mask(v), flags, SCALE, TRUE) ||
        (flag_mask(v) & I8244_F_BOXES))
        gtk_widget_queue_draw(v->overlay_area);
}

/* The bounding boxes of every object the chip has, in frame pixels. */
typedef struct { int x, y, w, h; char name[8]; int visible; } Box;

static int object_boxes(const i8244_snapshot *s, Box *out)
{
    int n = 0, i, q, j;
    for (i = 0; i < 4; i++) {
        const i8244_sprite_info *p = &s->sprites[i];
        Box b = { p->x, p->y, p->width, p->height, "", p->visible };
        g_snprintf(b.name, sizeof b.name, "S%d", i);
        out[n++] = b;
    }
    for (i = 0; i < 12; i++) {
        const i8244_char_info *c = &s->singles[i];
        Box b = { c->x, c->y, c->width, c->height, "", c->visible };
        g_snprintf(b.name, sizeof b.name, "C%d", i);
        out[n++] = b;
    }
    for (q = 0; q < 4; q++)
        for (j = 0; j < 4; j++) {
            const i8244_char_info *c = &s->quads[q].chars[j];
            Box b = { c->x, c->y, c->width, c->height, "", c->visible };
            g_snprintf(b.name, sizeof b.name, "Q%d.%d", q, j);
            out[n++] = b;
        }
    return n;
}

/* The box of what is under the pointer. */
static gboolean hit_box(const DbgVideo *v, Box *b)
{
    const i8244_snapshot *s = &v->snap;
    int k, i;
    if (!v->over || v->nhit <= 0) return FALSE;
    k = v->hit[0].kind;
    i = v->hit[0].index;
    if (k == 0 && i >= 0 && i < 4) {
        b->x = s->sprites[i].x; b->y = s->sprites[i].y;
        b->w = s->sprites[i].width; b->h = s->sprites[i].height;
        return TRUE;
    }
    if (k == 1 && i >= 0 && i < 12) {
        b->x = s->singles[i].x; b->y = s->singles[i].y;
        b->w = s->singles[i].width; b->h = s->singles[i].height;
        return TRUE;
    }
    if (k == 2 && i >= 0 && i < 16) {
        const i8244_char_info *c = &s->quads[i / 4].chars[i % 4];
        b->x = c->x; b->y = c->y; b->w = c->width; b->h = c->height;
        return TRUE;
    }
    return FALSE;
}

static void draw_overlay(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    Box boxes[4 + 12 + 16], hb;
    int n, i;
    (void)width; (void)height;

    /* the core draws the boxes of the re-render itself */
    if ((flag_mask(v) & I8244_F_BOXES) && view_now(v) != VIEW_RENDER) {
        n = object_boxes(&v->snap, boxes);
        cairo_set_line_width(cr, 1);
        for (i = 0; i < n; i++) {
            if (!boxes[i].visible) continue;
            dbg_set_rgb(cr, 0xFFE040, 0.9);
            cairo_rectangle(cr, boxes[i].x * SCALE + 0.5, boxes[i].y * SCALE + 0.5,
                            boxes[i].w * SCALE - 1, boxes[i].h * SCALE - 1);
            cairo_stroke(cr);
            dbg_set_rgb(cr, 0xFFE040, 1.0);
            dbg_draw_centred(cr, wg, boxes[i].name, boxes[i].x * SCALE + 1, boxes[i].y * SCALE + 1,
                             24, 12, FALSE);
        }
    }
    if (hit_box(v, &hb)) {
        dbg_set_rgb(cr, O2SESSION_ACCENT_RGB, 0.25);
        cairo_rectangle(cr, hb.x * SCALE, hb.y * SCALE, hb.w * SCALE, hb.h * SCALE);
        cairo_fill_preserve(cr);
        dbg_set_rgb(cr, 0xFF4A52, 1.0);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
}

/* "Single 3: x=.. y=.. glyph=.. colour=..", for what is under the pointer. */
static void describe_hit(DbgVideo *v, char *dst, int n)
{
    const i8244_snapshot *s = &v->snap;
    char more[40] = "";
    if (!v->over) {
        g_snprintf(dst, n, "Move the pointer over the picture to see what draws there");
        return;
    }
    if (v->nhit <= 0) {
        g_snprintf(dst, n, "Pointer x=%d y=%d: nothing but the background", v->mx, v->my);
        return;
    }
    if (v->nhit > 1)
        g_snprintf(more, sizeof more, "   (+%d under it)", v->nhit - 1);
    switch (v->hit[0].kind) {
    case 0: {
        const i8244_sprite_info *p = &s->sprites[v->hit[0].index & 3];
        g_snprintf(dst, n, "Sprite %d: x=%d y=%d colour=%s zoom=%dx S=%d X9=%d   pointer %d,%d%s",
                   p->index, p->x, p->y, i8244_colour_name(p->color), p->zoom ? 2 : 1,
                   p->s, p->x9, v->mx, v->my, more);
        break;
    }
    case 1: {
        const i8244_char_info *c = &s->singles[v->hit[0].index % 12];
        g_snprintf(dst, n, "Single %d: x=%d y=%d glyph=%d colour=%s ptr=$%02X   pointer %d,%d%s",
                   c->index, c->x, c->y, c->glyph, i8244_colour_name(c->color), c->reg_ptr,
                   v->mx, v->my, more);
        break;
    }
    case 2: {
        const int i = v->hit[0].index & 15;
        const i8244_char_info *c = &s->quads[i / 4].chars[i % 4];
        g_snprintf(dst, n, "Quad %d char %d: x=%d y=%d glyph=%d colour=%s ptr=$%02X   pointer %d,%d%s",
                   i / 4, i % 4, c->x, c->y, c->glyph, i8244_colour_name(c->color), c->reg_ptr,
                   v->mx, v->my, more);
        break;
    }
    default:
        g_snprintf(dst, n, "Pointer x=%d y=%d%s", v->mx, v->my, more);
        break;
    }
}

static void hover_update(DbgVideo *v, double x, double y, gboolean over)
{
    char text[200];
    o2debug_hit old = v->hit[0];
    const int oldn = v->nhit, oldx = v->mx, oldy = v->my;
    const gboolean was = v->over;

    v->over = over;
    if (over) {
        v->mx = CLAMP((int)x / SCALE, 0, FRAME_W - 1);
        v->my = CLAMP((int)y / SCALE, 0, FRAME_H - 1);
        if (was && v->mx == oldx && v->my == oldy)
            return;
        o2debug_vdc_get(v->dbg, &v->snap);
        v->nhit = o2debug_vdc_hit_test(v->dbg, v->mx, v->my, v->hit, 4);
        if (v->nhit < 0) v->nhit = 0;
    } else {
        v->nhit = 0;
    }
    describe_hit(v, text, sizeof text);
    gtk_label_set_text(v->hover_label, text);
    if (oldn != v->nhit || (v->nhit > 0 && (old.kind != v->hit[0].kind || old.index != v->hit[0].index)) ||
        was != over)
        gtk_widget_queue_draw(v->overlay_area);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    (void)c;
    hover_update(ud, x, y, TRUE);
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    (void)c;
    hover_update(ud, 0, 0, FALSE);
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgVideo *v = ud;
    (void)o; (void)ps;
    sync_sensitivity(v);
    v->pw = 0;                  /* a different picture: not comparable */
    picture_update(v);
}

static void on_option_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    picture_update(ud);
}

/* ---- objects ------------------------------------------------------------------------ */

#define OBJ_BOX 40              /* the cell an object's bitmap sits in */
#define OBJ_SCALE 4             /* one font or shape pixel as 4 x 4 */

static ObjRow *obj_row_new(GtkWidget *parent, ObjRow *r)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    r->pic = new_picture();
    gtk_widget_set_halign(GTK_WIDGET(r->pic), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(r->pic), GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(cell, OBJ_BOX, OBJ_BOX);
    gtk_widget_set_valign(cell, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(cell), GTK_WIDGET(r->pic));
    r->label = dbg_mono_label();
    gtk_label_set_selectable(GTK_LABEL(r->label), FALSE);
    gtk_box_append(GTK_BOX(row), cell);
    gtk_box_append(GTK_BOX(row), r->label);
    gtk_box_append(GTK_BOX(parent), row);
    return r;
}

static const char *vis_name(int visible, int clipped)
{
    return !visible ? "off screen" : clipped ? "clipped" : "visible";
}

/* One object's bitmap, fetched only when the registers it draws from
 * changed. */
static void obj_bitmap(DbgVideo *v, ObjRow *r, int kind, int index, const uint8_t *sig, int nsig)
{
    int w = 0, h = 0;
    uint8_t cur[16] = { 0 };
    memcpy(cur, sig, (size_t)nsig);
    if (r->have_sig && memcmp(cur, r->sig, sizeof cur) == 0)
        return;
    if (!o2debug_vdc_object(v->dbg, kind, index, OBJ_SCALE, 0x00202020, v->buf, v->cap, &w, &h) ||
        w <= 0 || h <= 0)
        return;
    set_picture(r->pic, v->buf, w, h);
    memcpy(r->sig, cur, sizeof cur);
    r->have_sig = TRUE;
}

static void refresh_objects(DbgVideo *v)
{
    const i8244_snapshot *s = &v->snap;
    char text[400];
    int i, q, j;

    for (i = 0; i < 4; i++) {
        const i8244_sprite_info *p = &s->sprites[i];
        g_snprintf(text, sizeof text,
            "S%d  x=%-3d y=%-3d  %-6s  zoom %dx  %s%s\n"
            "    S=%d X9=%d  shape %02X %02X %02X %02X %02X %02X %02X %02X",
            i, p->x, p->y, i8244_colour_name(p->color), p->zoom ? 2 : 1,
            vis_name(p->visible, p->clipped), s->fg_en ? "" : "  (foreground off)",
            p->s, p->x9, p->shape[0], p->shape[1], p->shape[2], p->shape[3],
            p->shape[4], p->shape[5], p->shape[6], p->shape[7]);
        gtk_label_set_text(GTK_LABEL(v->spr[i].label), text);
        {
            uint8_t sig[11];
            sig[0] = p->reg_y; sig[1] = p->reg_x; sig[2] = p->reg_color;
            memcpy(sig + 3, p->shape, 8);
            obj_bitmap(v, &v->spr[i], 0, i, sig, sizeof sig);
        }
    }
    for (i = 0; i < 12; i++) {
        const i8244_char_info *c = &s->singles[i];
        g_snprintf(text, sizeof text,
            "C%-2d x=%-3d y=%-3d  ptr $%02X  glyph %-2d  %-6s  bit8 %d\n"
            "    first row %d, %d rows  %s%s",
            i, c->x, c->y, c->reg_ptr, c->glyph, i8244_colour_name(c->color), c->bit8,
            c->first_row, c->rows,
            c->ntsc_hidden ? "hidden (NTSC: y < 14)" : vis_name(c->visible, c->clipped),
            s->fg_en ? "" : "  (foreground off)");
        gtk_label_set_text(GTK_LABEL(v->single[i].label), text);
        {
            uint8_t sig[4] = { c->reg_y, c->reg_x, c->reg_ptr, c->reg_color };
            obj_bitmap(v, &v->single[i], 1, i, sig, sizeof sig);
        }
    }
    for (q = 0; q < 4; q++) {
        const i8244_quad_info *qd = &s->quads[q];
        g_snprintf(text, sizeof text, "Quad %d   shared x=%d y=%d   %d rows (from the 4th "
                   "character's pointer)   16 px pitch", q, qd->x, qd->y, qd->rows);
        gtk_label_set_text(v->quad_head[q], text);
        for (j = 0; j < 4; j++) {
            const i8244_char_info *c = &qd->chars[j];
            ObjRow *r = &v->quad[q * 4 + j];
            g_snprintf(text, sizeof text,
                "Q%d.%d x=%-3d y=%-3d  ptr $%02X  glyph %-2d  %-6s  bit8 %d  %s",
                q, j, c->x, c->y, c->reg_ptr, c->glyph, i8244_colour_name(c->color), c->bit8,
                c->ntsc_hidden ? "hidden (NTSC: y < 14)" : vis_name(c->visible, c->clipped));
            gtk_label_set_text(GTK_LABEL(r->label), text);
            {
                uint8_t sig[16];
                memcpy(sig, s->regs + 0x40 + q * 16, 16);
                sig[0] = c->reg_y; sig[1] = c->reg_x;
                obj_bitmap(v, r, 2, q * 4 + j, sig, sizeof sig);
            }
        }
    }
}

static GtkWidget *build_objects(DbgVideo *v)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    int i, q, j;

    gtk_box_append(GTK_BOX(box), dbg_heading("Sprites (4: 8 x 8, 8 shape bytes each)"));
    for (i = 0; i < 4; i++)
        obj_row_new(box, &v->spr[i]);
    gtk_box_append(GTK_BOX(box), dbg_heading("Single characters (12)"));
    for (i = 0; i < 12; i++)
        obj_row_new(box, &v->single[i]);
    gtk_box_append(GTK_BOX(box), dbg_heading("Quads (4 x 4 characters)"));
    for (q = 0; q < 4; q++) {
        v->quad_head[q] = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(v->quad_head[q]), "dim-label");
        gtk_label_set_xalign(v->quad_head[q], 0);
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->quad_head[q]));
        for (j = 0; j < 4; j++)
            obj_row_new(box, &v->quad[q * 4 + j]);
    }
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the grid -------------------------------------------------------------------------- */

#define GRID_CW 28              /* one cell of the diagram */
#define GRID_CH 20
#define GRID_PAD 22

static void draw_grid(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    const i8244_snapshot *s = &v->snap;
    GtkWidget *wg = GTK_WIDGET(area);
    const guint32 on = v->have_palette ? (v->palette[(s->grid_lum ? 8 : 0) + (s->grid_color & 7)] & 0xFFFFFF)
                                        : 0xFFE040;
    const double alpha = s->grid_en ? 1.0 : 0.35;
    int i, y;
    (void)width; (void)height;

    cairo_rectangle(cr, 0, 0, GRID_PAD * 2 + 9 * GRID_CW, GRID_PAD * 2 + 8 * GRID_CH);
    dbg_set_rgb(cr, 0x101010, 1.0);
    cairo_fill(cr);

    for (i = 0; i < 10; i++) {
        char t[4];
        g_snprintf(t, sizeof t, "%d", i);
        dbg_set_rgb(cr, 0x909090, 1.0);
        dbg_draw_centred(cr, wg, t, GRID_PAD + i * GRID_CW - 10, 2, 20, GRID_PAD - 4, TRUE);
    }
    for (y = 0; y < 9; y++) {
        char t[4];
        g_snprintf(t, sizeof t, "%d", y);
        dbg_set_rgb(cr, 0x909090, 1.0);
        dbg_draw_centred(cr, wg, t, 0, GRID_PAD + y * GRID_CH - 8, GRID_PAD - 4, 16, TRUE);
    }
    /* the 9 horizontal lines of 9 segments, and every point between them */
    for (y = 0; y < 9; y++)
        for (i = 0; i < 9; i++) {
            const gboolean lit = (s->hgrid[i] >> y) & 1;
            cairo_move_to(cr, GRID_PAD + i * GRID_CW, GRID_PAD + y * GRID_CH);
            cairo_line_to(cr, GRID_PAD + (i + 1) * GRID_CW, GRID_PAD + y * GRID_CH);
            if (lit) {
                dbg_set_rgb(cr, on, alpha);
                cairo_set_line_width(cr, 4);
            } else {
                dbg_set_rgb(cr, 0x606060, 0.35);
                cairo_set_line_width(cr, 1);
            }
            cairo_stroke(cr);
        }
    /* the 10 vertical lines of 8 segments; "fill" makes them as wide as a cell */
    for (i = 0; i < 10; i++)
        for (y = 0; y < 8; y++) {
            const gboolean lit = (s->vgrid[i] >> y) & 1;
            if (lit && s->fill) {
                dbg_set_rgb(cr, on, 0.55 * alpha);
                cairo_rectangle(cr, GRID_PAD + i * GRID_CW, GRID_PAD + y * GRID_CH,
                                GRID_CW, GRID_CH);
                cairo_fill(cr);
                continue;
            }
            cairo_move_to(cr, GRID_PAD + i * GRID_CW, GRID_PAD + y * GRID_CH);
            cairo_line_to(cr, GRID_PAD + i * GRID_CW, GRID_PAD + (y + 1) * GRID_CH);
            if (lit) {
                dbg_set_rgb(cr, on, alpha);
                cairo_set_line_width(cr, 3);
            } else {
                dbg_set_rgb(cr, 0x606060, 0.35);
                cairo_set_line_width(cr, 1);
            }
            cairo_stroke(cr);
        }
    if (s->dots_en) {
        dbg_set_rgb(cr, on, alpha);
        for (y = 0; y < 9; y++)
            for (i = 0; i < 10; i++) {
                cairo_rectangle(cr, GRID_PAD + i * GRID_CW - 2, GRID_PAD + y * GRID_CH - 2, 5, 5);
                cairo_fill(cr);
            }
    }
}

static void refresh_grid(DbgVideo *v)
{
    const i8244_snapshot *s = &v->snap;
    char text[900];
    int i, lit_h = 0, lit_v = 0;

    fetch_palette(v);
    for (i = 0; i < 9; i++)
        lit_h += __builtin_popcount(s->hgrid[i]);
    for (i = 0; i < 10; i++)
        lit_v += __builtin_popcount(s->vgrid[i]);
    g_snprintf(text, sizeof text,
        "Grid         %s   (control bit 3)\n"
        "Dots         %s   (bit 6)\n"
        "Fill         %s   (bit 7: vertical lines %s)\n"
        "Colour       %d %s%s   ($A3: bits 0-2, brightness bit 6)\n"
        "Cells        %d x %d units, from x=%d (x2 px) y=%d\n"
        "Lines lit    %d of 81 horizontal, %d of 80 vertical\n"
        "Write lock   $C0-$FF %s while the grid is on",
        s->grid_en ? "on" : "off", s->dots_en ? "on" : "off", s->fill ? "on" : "off",
        s->fill ? "16 units wide" : "2 units wide", s->grid_color,
        i8244_colour_name(s->grid_color), s->grid_lum ? " (bright)" : "",
        s->cell_w, s->cell_h, s->grid_x0, s->grid_y0, lit_h, lit_v,
        s->lock_grid ? "ignore writes and read 0" : "are plain RAM");
    gtk_label_set_text(v->grid_text, text);
    gtk_widget_queue_draw(v->grid_area);
}

static GtkWidget *build_grid(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    v->grid_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->grid_area), GRID_PAD * 2 + 9 * GRID_CW);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->grid_area), GRID_PAD * 2 + 8 * GRID_CH);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->grid_area), draw_grid, v, NULL);
    gtk_widget_set_halign(v->grid_area, GTK_ALIGN_START);
    v->grid_text = GTK_LABEL(dbg_mono_label());
    gtk_box_append(GTK_BOX(box), dbg_heading("The grid: 9 horizontal lines of 9 segments, 10 vertical of 8"));
    gtk_box_append(GTK_BOX(box), v->grid_area);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->grid_text));
    gtk_box_append(GTK_BOX(box), dbg_hint("Lit segments in the grid's colour; the faint ones are "
                                          "off. Registers $C0-$CF/$D0-$D8 (horizontal) and "
                                          "$E0-$E9 (vertical) hold the bits.", 40));
    dbg_padded(box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the registers ------------------------------------------------------------------------ */

/* The register file folds mirrors onto one cell (the quads' shared x and y,
 * and $A0-$AF repeated at $B0-$BF). */
static unsigned fold_mirrors(unsigned off)
{
    off &= 0xFF;
    if ((off & 0xC2) == 0x40) off &= ~0x0Cu;
    if ((off & 0xE0) == 0xA0) off &= ~0x10u;
    return off;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgVideo *v = ud;
    unsigned off = (unsigned)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long val;
    if (dbg_parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &val)) {
        gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
        o2debug_vdc_write(v->dbg, off, (uint8_t)val);
                dbg_video_refresh(v, FALSE);
    } else {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
    }
}

static GtkWidget *build_regs(DbgVideo *v)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    int i;

    for (i = 0; i < 256; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *addr, *name, *val, *text;
        char a[8];
        g_snprintf(a, sizeof a, "$%02X", i);
        addr = gtk_label_new(a);
        gtk_widget_add_css_class(addr, "monospace");
        gtk_widget_add_css_class(addr, "dim-label");
        name = gtk_label_new("");
        gtk_widget_add_css_class(name, "monospace");
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 24);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        val = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(val), 2);
        gtk_editable_set_width_chars(GTK_EDITABLE(val), 2);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(val), 2);
        gtk_widget_add_css_class(val, "monospace");
        gtk_widget_add_css_class(val, "o2-hex");
        g_object_set_data(G_OBJECT(val), "reg", GINT_TO_POINTER(i));
        g_signal_connect(val, "activate", G_CALLBACK(on_reg_activate), v);
        text = gtk_label_new("");
        gtk_widget_add_css_class(text, "monospace");
        gtk_label_set_xalign(GTK_LABEL(text), 0);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_label_set_width_chars(GTK_LABEL(text), 20);
        gtk_label_set_max_width_chars(GTK_LABEL(text), 20);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(row), addr);
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), val);
        gtk_box_append(GTK_BOX(row), text);
        gtk_box_append(GTK_BOX(list), row);
        v->reg_row[i] = row;
        v->reg_name[i] = GTK_LABEL(name);
        v->reg_val[i] = GTK_ENTRY(val);
        v->reg_text[i] = GTK_LABEL(text);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(outer), dbg_hint("Every register $00-$FF with its name and what it "
                                            "means now. Enter in a value writes it, with the chip's "
                                            "own masks; a running program may write it "
                                            "again.", 40));
    gtk_box_append(GTK_BOX(outer), scroll);
    return dbg_padded(outer);
}

/* Values from the snapshot's raw file; a decoded line only for the ones whose
 * value changed (and the three that depend on the beam), because each
 * description is a call into the running machine. */
static void refresh_regs(DbgVideo *v)
{
    int i;
    for (i = 0; i < 256; i++) {
        const uint8_t val = v->snap.regs[fold_mirrors((unsigned)i)];
        const gboolean live = i == 0xA1 || i == 0xA4 || i == 0xA5 || i == 0xB1 || i == 0xB4 || i == 0xB5;
        char hex[4];

        if (!v->reg_built || v->reg_shown[i] != val || live) {
            char name[64], text[160];
            o2debug_vdc_describe(v->dbg, (unsigned)i, name, sizeof name, text, sizeof text);
            if (!v->reg_built)
                gtk_label_set_text(v->reg_name[i], name);
            gtk_label_set_text(v->reg_text[i], text);
            gtk_widget_set_tooltip_text(GTK_WIDGET(v->reg_text[i]), text);
            v->reg_shown[i] = val;
        }
        g_snprintf(hex, sizeof hex, "%02X", val);
        dbg_set_entry(v->dbg, v->reg_val[i], hex);
    }
    v->reg_built = TRUE;
}

/* ---- control and status ---------------------------------------------------------------------- */

static void refresh_control(DbgVideo *v)
{
    static const char *const src[8] = { "sprite 0", "sprite 1", "sprite 2", "sprite 3",
                                        "vertical grid", "horizontal grid and dots",
                                        "external (CX)", "characters" };
    const i8244_snapshot *s = &v->snap;
    GString *t = g_string_new(NULL);
    int b;

    g_string_append_printf(t,
        "CONTROL  $A0 = $%02X\n"
        "  7 fill          %-3s  vertical grid lines 16 units wide, not 2\n"
        "  6 dots          %-3s\n"
        "  5 foreground    %-3s  objects drawn; writes to $00-$7F %s\n"
        "  4 external IRQ  %-3s  an overlap with the external (CX) input interrupts\n"
        "  3 grid          %-3s  grid drawn; writes to $C0-$FF %s\n"
        "  2 sound IRQ     %-3s  interrupt when the shift register reloads\n"
        "  1 strobe        %-3s  $A4/$A5 read %s\n"
        "  0 (unused)      %d\n\n",
        s->control, s->fill ? "on" : "off", s->dots_en ? "on" : "off",
        s->fg_en ? "on" : "off", s->lock_objects ? "ignored" : "accepted",
        s->ext_irq_en ? "on" : "off", s->grid_en ? "on" : "off",
        s->lock_grid ? "ignored" : "accepted", s->sound_irq_en ? "on" : "off",
        s->strobe ? "on" : "off", s->strobe ? "the latched beam position" : "the live beam",
        s->ctl_bit0);
    g_string_append_printf(t,
        "STATUS   $A1 reads $%02X now\n"
        "  7 characters overlapped each other   %d  (sticky)\n"
        "  6 external overlap (CX)              %d  (sticky)\n"
        "  3 vertical blank                     %d  (sticky)\n"
        "  2 sound shift register reloaded      %d  (sticky)\n"
        "  1 strobe                             %d\n"
        "  0 horizontal status                  %d\n"
        "  _INTR line                           %s\n\n",
        s->status, s->st_char_overlap, s->st_ext_overlap, s->st_vblank, s->st_sound,
        s->st_strobe, s->st_hstatus, s->irq ? "asserted" : "-");
    g_string_append_printf(t,
        "COLLISION  $A2   mask $%02X   latch $%02X   (a read clears the latch)\n"
        "  bit  source                      mask  latch\n", s->col_mask, s->col_result);
    for (b = 0; b < 8; b++)
        g_string_append_printf(t, "   %d   %-26s   %d     %d\n", b, src[b],
                               (s->col_mask >> b) & 1, (s->col_result >> b) & 1);
    g_string_append_printf(t,
        "\nCOLOURS  $A3 = $%02X\n"
        "  background    %d %s\n"
        "  grid          %d %s%s\n"
        "  P17 (LUM)     %s\n\n",
        s->color_reg, s->bg_color, i8244_colour_name(s->bg_color), s->grid_color,
        i8244_colour_name(s->grid_color), s->grid_lum ? " (bright)" : "",
        s->p17_lum ? "low: every pixel brightened" : "high");
    g_string_append_printf(t,
        "BEAM     now x=%d y=%d   (hpos %d, vpos %d)\n"
        "         $A5/$A4 latched x=%d y=%d\n"
        "         lazy renderer at line %d, dot %d   frame %llu\n"
        "         %s   %.2f Hz frame   %.0f Hz line   pixel clock %.0f Hz\n",
        s->x_beam, s->y_beam, s->hpos, s->vpos, s->x_beam_latched, s->y_beam_latched,
        s->last_scan, s->part_hpos, (unsigned long long)s->frames,
        s->is_pal ? "8245 (PAL)" : "8244 (NTSC)", s->frame_hz, s->line_hz, s->pixel_hz);
    gtk_label_set_text(v->control_text, t->str);
    g_string_free(t, TRUE);
}

/* ---- sound ----------------------------------------------------------------------------------------- */

#define SHIFT_CELL 20

static void draw_shift(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgVideo *v = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int i;
    (void)width; (void)height;
    /* bit 23 (the first one out) on the left */
    for (i = 0; i < 24; i++) {
        const int bit = 23 - i;
        const gboolean one = (v->snap.shift >> bit) & 1;
        double x = i * SHIFT_CELL + 0.5 + (i / 8) * 6, y = 14.5;
        char t[4];
        cairo_rectangle(cr, x, y, SHIFT_CELL - 2, 24);
        if (one) {
            dbg_set_rgb(cr, O2SESSION_ACCENT_RGB, 1.0);
            cairo_fill_preserve(cr);
        }
        dbg_set_fg(cr, wg, 0.35);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        g_snprintf(t, sizeof t, "%d", one);
        if (one) cairo_set_source_rgb(cr, 1, 1, 1);
        else dbg_set_fg(cr, wg, 0.8);
        dbg_draw_centred(cr, wg, t, x, y, SHIFT_CELL - 2, 24, TRUE);
        g_snprintf(t, sizeof t, "%d", bit);
        dbg_set_fg(cr, wg, 0.5);
        dbg_draw_centred(cr, wg, t, x - 2, 0, SHIFT_CELL + 2, 14, FALSE);
    }
}

static void refresh_sound(DbgVideo *v)
{
    const i8244_snapshot *s = &v->snap;
    char text[1000], tone[96];

    if (s->snd_noise)
        g_snprintf(tone, sizeof tone, "noise: the shift register is a noise source");
    else if (s->freq_hz > 0.5)
        g_snprintf(tone, sizeof tone, "%.1f Hz (the pattern repeats every %d bits)", s->freq_hz,
                   s->pattern_period);
    else
        g_snprintf(tone, sizeof tone, "a steady level (no tone)");
    g_snprintf(text, sizeof text,
        "SOUND    $AA = $%02X   shift register $A7-$A9 = $%06X\n"
        "  7 enable        %s\n"
        "  5 clock         %s: the register shifts at line rate / %d = %.0f Hz\n"
        "  4 mode          %s\n"
        "  3-0 volume      %d of 15\n"
        "  output now      %d (the bit at the end of the register)\n"
        "  sound IRQ       %s; reload pending %d\n"
        "  tone            %s\n",
        s->snd_reg, s->shift, s->snd_enable ? "on" : "off",
        s->snd_fast ? "fast" : "slow", s->snd_fast ? 4 : 16, s->shift_clock_hz,
        s->snd_noise ? "noise" : "loop (the register rotates)", s->snd_volume,
        s->sh_output, s->sound_irq_en ? "enabled" : "off", s->sh_pending, tone);
    gtk_label_set_text(v->sound_text, text);
    gtk_widget_queue_draw(v->shift_area);
}

static GtkWidget *build_sound(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    v->shift_area = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(v->shift_area), 24 * SHIFT_CELL + 2 * 6);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(v->shift_area), 44);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->shift_area), draw_shift, v, NULL);
    gtk_widget_set_halign(v->shift_area, GTK_ALIGN_START);
    v->sound_text = GTK_LABEL(dbg_mono_label());
    gtk_box_append(GTK_BOX(box), dbg_heading("The 24-bit shift register"));
    gtk_box_append(GTK_BOX(box), v->shift_area);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->sound_text));
    gtk_box_append(GTK_BOX(box), dbg_hint("The chip's only voice: a 24-bit pattern shifted out "
                                          "once per clock, at the volume set. A repeating "
                                          "pattern is a tone.", 40));
    dbg_padded(box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- sheets --------------------------------------------------------------------------------------------- */

static void refresh_sheets(DbgVideo *v)
{
    show_view(v, NULL, v->sheet_font, O2DEBUG_VDC_FONT, 0, 0, 3, FALSE);
    show_view(v, NULL, v->sheet_shapes, O2DEBUG_VDC_SHAPES, 0, 0, 3, FALSE);
    show_view(v, NULL, v->sheet_palette, O2DEBUG_VDC_PALETTE, 0, 0, 2, FALSE);
}

static GtkWidget *build_sheets(DbgVideo *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    v->sheet_font = image_area_new();
    v->sheet_shapes = image_area_new();
    v->sheet_palette = image_area_new();
    gtk_box_append(GTK_BOX(box), dbg_heading("Character set: 64 glyphs, the lower 256 bytes "
                                             "(colour bit 0 = 0) above the upper"));
    gtk_box_append(GTK_BOX(box), v->sheet_font->widget);
    gtk_box_append(GTK_BOX(box), dbg_heading("Sprite shape tables ($80-$9F), in their colours"));
    gtk_box_append(GTK_BOX(box), v->sheet_shapes->widget);
    gtk_box_append(GTK_BOX(box), dbg_heading("Palette: 8 colours (top), the same bright (below)"));
    gtk_box_append(GTK_BOX(box), v->sheet_palette->widget);
    dbg_padded(box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ---- the tab ------------------------------------------------------------------------------------------------- */

static void refresh_section(DbgVideo *v, int sec)
{
    switch (sec) {
    case SEC_OBJECTS: refresh_objects(v); break;
    case SEC_GRID: refresh_grid(v); break;
    case SEC_REGS: refresh_regs(v); break;
    case SEC_CONTROL: refresh_control(v); break;
    case SEC_SOUND: refresh_sound(v); break;
    case SEC_SHEETS: refresh_sheets(v); break;
    default: break;
    }
}

static void on_section_switch(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgVideo *v = ud;
    (void)nb; (void)page;
    o2debug_vdc_get(v->dbg, &v->snap);
    refresh_section(v, (int)num);
}

void dbg_video_refresh(DbgVideo *v, gboolean live)
{
    o2debug_vdc_get(v->dbg, &v->snap);
    picture_update(v);
    /* while the machine runs, the sections look at the registers a few times
     * a second; stopped, every call is a change worth showing */
    if (live && ++v->tick < 5)
        return;
    v->tick = 0;
    refresh_section(v, gtk_notebook_get_current_page(v->sections));
}

static GtkWidget *check(const char *label, gboolean active, GCallback cb, gpointer ud)
{
    GtkWidget *c = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), active);
    g_signal_connect(c, "toggled", cb, ud);
    return c;
}

DbgVideo *dbg_video_new(o2session *session, o2debug *dbg, GtkWidget **root)
{
    static const char *const views[] = { "Frame (what the display shows)",
                                         "Rendered from the registers",
                                         "Frame in progress, to the beam", NULL };
    DbgVideo *v = g_new0(DbgVideo, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row, *overlay, *layers, *flags, *lscroll;
    GtkEventController *motion;
    int i;

    v->session = session;
    v->dbg = dbg;
    v->cap = (size_t)O2DEBUG_VDC_VIEW_MAX_PIXELS * SCALE * SCALE;
    v->buf = g_new0(uint32_t, v->cap);
    v->prev = g_new0(uint32_t, v->cap);

    /* the view and its layers and overlays */
    v->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    g_signal_connect(v->view, "notify::selected", G_CALLBACK(on_view_changed), v);
    row = dbg_labeled("View", GTK_WIDGET(v->view));
    layers = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(layers), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(layers), 7);
    for (i = 0; i < NLAYERS; i++) {
        v->layer[i] = GTK_CHECK_BUTTON(check(k_layers[i].name, TRUE, G_CALLBACK(on_option_toggled), v));
        gtk_flow_box_append(GTK_FLOW_BOX(layers), GTK_WIDGET(v->layer[i]));
    }
    flags = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flags), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flags), 6);
    for (i = 0; i < NFLAGS; i++) {
        v->flag[i] = GTK_CHECK_BUTTON(check(k_flags[i].name, FALSE, G_CALLBACK(on_option_toggled), v));
        gtk_flow_box_append(GTK_FLOW_BOX(flags), GTK_WIDGET(v->flag[i]));
    }

    /* the picture, with the overlay that boxes and outlines over it */
    v->pic = new_picture();
    gtk_widget_set_size_request(GTK_WIDGET(v->pic), FRAME_W * SCALE, FRAME_H * SCALE);
    v->overlay_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->overlay_area), draw_overlay, v, NULL);
    gtk_widget_set_can_target(v->overlay_area, FALSE);
    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(v->pic));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), v->overlay_area);
    gtk_widget_set_halign(overlay, GTK_ALIGN_START);
    gtk_widget_set_valign(overlay, GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), v);
    gtk_widget_add_controller(overlay, motion);

    v->hover_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(v->hover_label), "monospace");
    gtk_label_set_xalign(v->hover_label, 0);
    gtk_label_set_ellipsize(v->hover_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(v->hover_label, 1);
    gtk_label_set_text(v->hover_label, "Move the pointer over the picture to see what draws there");

    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), layers);
    gtk_box_append(GTK_BOX(left), flags);
    gtk_box_append(GTK_BOX(left), overlay);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(v->hover_label));
    gtk_widget_set_size_request(left, FRAME_W * SCALE, -1);
    gtk_widget_set_hexpand(left, FALSE);

    /* the sections */
    v->sections = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_scrollable(v->sections, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(v->sections), TRUE);
    gtk_notebook_append_page(v->sections, build_objects(v), gtk_label_new("Objects"));
    gtk_notebook_append_page(v->sections, build_grid(v), gtk_label_new("Grid"));
    gtk_notebook_append_page(v->sections, build_regs(v), gtk_label_new("Registers"));
    {
        GtkWidget *scroll = gtk_scrolled_window_new();
        v->control_text = GTK_LABEL(dbg_mono_label());
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dbg_padded(GTK_WIDGET(v->control_text)));
        gtk_notebook_append_page(v->sections, scroll, gtk_label_new("Control/Status"));
    }
    gtk_notebook_append_page(v->sections, build_sound(v), gtk_label_new("Sound"));
    gtk_notebook_append_page(v->sections, build_sheets(v), gtk_label_new("Sheets"));
    g_signal_connect(v->sections, "switch-page", G_CALLBACK(on_section_switch), v);

    /* the picture cannot shrink, so the column scrolls rather than hold the
     * window to its height on a small screen */
    lscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscroll), left);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(lscroll), TRUE);
    gtk_widget_set_hexpand(lscroll, FALSE);
    gtk_box_append(GTK_BOX(box), lscroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->sections));
    sync_sensitivity(v);
    *root = dbg_padded(box);
    return v;
}

void dbg_video_free(DbgVideo *v)
{
    if (!v) return;
    g_free(v->buf);
    g_free(v->prev);
    g_free(v);
}

/* ======================================================================== */
/* the EF9340 tab                                                            */
/* ======================================================================== */

struct DbgEf {
    o2session *session;
    o2debug *dbg;
    GtkWidget *stack;
    GtkTextView *regs_text, *page_text;
    ImageArea *image;
    uint32_t *buf, *prev;
    gboolean have_prev;
};

static const char *cell_kind(int k)
{
    static const char *const n[4] = { "alphanumeric", "graphic", "delimiter", "illegal" };
    return k >= 0 && k < 4 ? n[k] : "?";
}

/* One cell as a character: the alphanumeric ones as themselves, the
 * rest as a mark. */
static char cell_char(const ef9340_cell_t *c)
{
    switch (c->kind) {
    case EF9340_CELL_ALNUM: {
        const int code = c->b & 0x7F;
        return code >= 0x20 && code < 0x7F ? (char)code : '.';
    }
    case EF9340_CELL_GRAPHIC: return '#';
    case EF9340_CELL_DELIMITER: return '|';
    default: return '?';
    }
}

void dbg_ef_refresh(DbgEf *e)
{
    ef9340_snapshot_t s;
    GString *t;
    int r, c;

    if (!o2debug_ef_present(e->dbg)) {
        gtk_stack_set_visible_child_name(GTK_STACK(e->stack), "absent");
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(e->stack), "present");
    o2debug_ef_get(e->dbg, &s);

    t = g_string_new(NULL);
    g_string_append_printf(t,
        "EF9341 (GEN)   TA $%02X   TB $%02X   %s\n"
        "EF9340 (VIN)   X %d   Y %d   Y0 $%02X   R $%02X   M $%02X\n\n"
        "R  $%02X   display   %s\n"
        "           box mode %s   conceal %s   service row %s\n"
        "           cursor %s   %s lines   blink %s\n"
        "Y0 $%02X   first row %d   zoom %s\n"
        "M  $%02X   mode %s   slice %d\n"
        "cursor     %s   blink phase %s\n"
        "font       %s   lines drawn %llu\n",
        s.ta, s.tb, s.busy ? "busy" : "ready", s.x, s.y, s.y0, s.r, s.m,
        s.r, s.display_on ? "on" : "off", s.box_mode ? "on" : "off",
        s.conceal_enabled ? "on" : "off", s.service_row ? "shown" : "hidden",
        s.cursor_on ? "on" : "off", s.lines625 ? "250 (625-line)" : "210", s.blink_enabled ? "on" : "off",
        s.y0, s.first_row, s.zoom ? "on (rows below the service row double height)" : "off",
        s.m, s.mode_name ? s.mode_name : "?", s.slice,
        s.cursor_visible ? "visible" : "not on a cell", s.blink ? "hidden" : "shown",
        s.font_present ? "imported" : "NOT imported (text draws blank)",
        (unsigned long long)s.lines_drawn);
    dbg_set_text(e->regs_text, t->str);
    g_string_free(t, TRUE);

    t = g_string_new("      0         1         2         3\n"
                     "      0123456789012345678901234567890123456789\n");
    if (s.service_row) {
        g_string_append(t, "svc   ");
        for (c = 0; c < EF9340_COLUMNS; c++)
            g_string_append_c(t, cell_char(&s.service[c]));
        g_string_append_c(t, '\n');
    }
    for (r = 0; r < EF9340_ROWS; r++) {
        g_string_append_printf(t, "%02d    ", r);
        for (c = 0; c < EF9340_COLUMNS; c++)
            g_string_append_c(t, cell_char(&s.page[r][c]));
        g_string_append_c(t, '\n');
    }
    g_string_append_printf(t, "\n# graphic cell   | delimiter   ? illegal   . code with no glyph\n"
                              "kinds: %s, %s, %s, %s\n",
                           cell_kind(0), cell_kind(1), cell_kind(2), cell_kind(3));
    dbg_set_text(e->page_text, t->str);
    g_string_free(t, TRUE);

    {
        int w = 0, h = 0;
        if (o2debug_ef_view(e->dbg, e->buf, (size_t)EF9340_VIS_W * EF9340_VIS_H, &w, &h) &&
            w > 0 && h > 0) {
            const size_t bytes = (size_t)w * h * 4;
            if (!e->have_prev || memcmp(e->buf, e->prev, bytes) != 0) {
                memcpy(e->prev, e->buf, bytes);
                e->have_prev = TRUE;
                image_area_set(e->image, e->buf, w, h);
            }
        }
    }
}

DbgEf *dbg_ef_new(o2session *session, o2debug *dbg, GtkWidget **root)
{
    DbgEf *e = g_new0(DbgEf, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *pscroll, *absent, *regs, *page;

    e->session = session;
    e->dbg = dbg;
    e->buf = g_new0(uint32_t, (size_t)EF9340_VIS_W * EF9340_VIS_H);
    e->prev = g_new0(uint32_t, (size_t)EF9340_VIS_W * EF9340_VIS_H);

    regs = dbg_mono_view(&e->regs_text, FALSE);
    gtk_text_view_set_wrap_mode(e->regs_text, GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(regs, 380, -1);
    page = dbg_mono_view(&e->page_text, FALSE);
    gtk_widget_set_size_request(page, 420, -1);
    e->image = image_area_new();
    pscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pscroll), e->image->widget);
    gtk_widget_set_hexpand(pscroll, TRUE);
    gtk_widget_set_vexpand(pscroll, TRUE);

    gtk_box_append(GTK_BOX(left), dbg_heading("Registers"));
    gtk_box_append(GTK_BOX(left), regs);
    gtk_box_append(GTK_BOX(mid), dbg_heading("Text page (40 x 24)"));
    gtk_box_append(GTK_BOX(mid), page);
    gtk_box_append(GTK_BOX(right), dbg_heading("The text layer, rendered"));
    gtk_box_append(GTK_BOX(right), pscroll);
    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), mid);
    gtk_box_append(GTK_BOX(box), right);

    absent = gtk_label_new("Videopac+ G7400 only");
    gtk_widget_add_css_class(absent, "title-2");
    gtk_widget_add_css_class(absent, "dim-label");
    e->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(e->stack), dbg_padded(box), "present");
    gtk_stack_add_named(GTK_STACK(e->stack), absent, "absent");
    gtk_stack_set_visible_child_name(GTK_STACK(e->stack), "absent");
    *root = e->stack;
    return e;
}

void dbg_ef_free(DbgEf *e)
{
    if (!e) return;
    g_free(e->buf);
    g_free(e->prev);
    g_free(e);
}

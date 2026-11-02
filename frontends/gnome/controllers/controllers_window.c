/*
 * The Controllers window -- the Odyssey 2's membrane keyboard (drawn from the
 * session's layout table, so every frontend shows the same caps for the
 * machine: the console's, or the Videopac+ G7400's), both joysticks drawn like
 * the console's (a black base, the red fire button in its corner, a stick
 * with four direction pads), the console's Reset to CONFIG and debugger
 * buttons, the gamepads and the stick each drives, and Map mode.
 *
 * Every control is pressed on the raw button press and released on the
 * raw release (a GtkGestureDrag, not a click: a click gesture gives up when
 * the pointer wobbles), because the console scans its keyboard and samples
 * the sticks once per frame -- a press present only for the instant of a click
 * falls between frames and a polling program never sees it. A key on screen
 * is HELD for as long as the mouse button is down, exactly like the real one,
 * and dragging off a cap lets it go. Losing the focus, hiding and rebuilding
 * let go of whatever the mouse holds here (only that: the main window
 * releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this window
 * together (o2session_key_held, o2session_buttons_held) -- is painted in the
 * accent colour, so the window doubles as an input tester for a new gamepad.
 *
 * Keys typed with the window active go through the main window
 * (o2_window_key), so the console, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE: the drawn parts have fixed sizes (key caps at a fixed number of
 * pixels per key unit), labels ellipsize rather than grow, so neither Map mode
 * nor a long gamepad name resizes the window, and tiling compositors float
 * it. The keyboard is rebuilt when the machine changes.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a key cap, a
 * joystick control or a console button to pick the target, then press a
 * keyboard key OR a gamepad button. While armed the same gestures pick the
 * target instead of pressing, every control shows the key bound to it, and
 * typed keys are not typed into the console; once a target is picked the
 * other controls are greyed. Rebinding steals the key from whatever held it.
 * Escape cancels.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define PANEL_PAD 14            /* the silver case around the plate */
#define PLATE_PAD 12            /* the black keyboard plate around the caps */
#define KBD_WIDTH_PX 560.0      /* the unit is chosen to land near this */
#define UNIT_MAX 44             /* pixels per key unit */
#define UNIT_MIN 30
#define STICK_W 214
#define STICK_H 172
#define STRIP_GAP 12
#define STRIP_H 34
#define MAX_ITEMS 80
#define MAX_PADS 8

/* the pad rings: radii and the angle each pad spans, as drawn and as hit */
#define RING_R0 24.0
#define RING_R1 56.0
#define PAD_DRAW_SPAN (38.0 * G_PI / 180.0)
#define PAD_HIT_SPAN (45.0 * G_PI / 180.0)

enum { PANE_KBD, PANE_STICK0, PANE_STICK1, PANE_STRIP, PANE_COUNT };
enum { SH_RECT, SH_WEDGE };
/* kinds beyond o2_keycap_kind */
enum { K_FIRE = 100, K_PAD, K_BUTTON };

typedef struct {
    int shape;
    int kind;                   /* o2_keycap_kind, or K_* */
    int target;                 /* what a press drives and Map rebinds */
    const char *label;
    double x, y, w, h;          /* SH_RECT */
    double a;                   /* SH_WEDGE: the pad's centre angle */
    gboolean lit;               /* what the console sees held */
} Item;

typedef struct {
    GtkWidget *area;
    int port;                   /* a stick pane's */
    Item items[MAX_ITEMS];
    int n;
    int pressed;                /* the item the mouse holds, or -1 */
    int hover;
} Pane;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the O2Window: typed keys go through it */
static o2session *g_session;
static AdwWindowTitle *g_title;
static Pane g_pane[PANE_COUNT];

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_held_timer;
static guint g_pad_timer;

static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];     /* each pad's "drives stick N" */
static int g_npad_rows;
static unsigned g_pad_generation = (unsigned)-1;

/* what the keyboard was built for */
static int g_built_machine = -1;
static int g_unit = UNIT_MAX;
static double g_kbd_w, g_kbd_h; /* in key units */

static void refresh_all(void);

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cr, guint32 c)
{
    cairo_set_source_rgb(cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0,
                         (c & 0xFF) / 255.0);
}

static guint32 mix(guint32 a, guint32 b, double t)
{
    guint32 out = 0;
    int s;
    for (s = 16; s >= 0; s -= 8) {
        double ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= (guint32)(ca + (cb - ca) * t + 0.5) << s;
    }
    return out;
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *cr, GtkWidget *w, const char *text, double px, gboolean bold,
                   guint32 colour, double x, double y, double bw, double bh)
{
    PangoLayout *l = gtk_widget_create_pango_layout(w, text);
    PangoFontDescription *fd = pango_font_description_new();
    int tw, th;

    pango_font_description_set_family(fd, "Sans");
    pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_layout_set_font_description(l, fd);
    pango_layout_set_width(l, (int)(bw * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(l, &tw, &th);
    rgb(cr, colour);
    cairo_move_to(cr, x, y + (bh - th) / 2.0);
    pango_cairo_show_layout(cr, l);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cr);
    pango_font_description_free(fd);
    g_object_unref(l);
}

/* A host key's name as a small legend: the long ones shortened to fit a cap. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    o2session_keysym_name(keysym, name, sizeof name);
    if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean is_sysact(int target)
{
    return target >= O2_TARGET_SYSACT(0);
}

/* What the console sees of an item held, from any source. */
static gboolean item_held(const Item *it, const unsigned held[2])
{
    int t = it->target;
    if (t < 2 * O2_ACT_PER_PORT)
        return ((held[t / O2_ACT_PER_PORT] >> (t % O2_ACT_PER_PORT)) & 1) != 0;
    if (t < O2_TARGET_SWITCH(0))
        return o2session_key_held(g_session, t - O2_TARGET_KEY(0)) != 0;
    if (t < O2_TARGET_SYSACT(0))
        return o2session_switch_held(g_session, t - O2_TARGET_SWITCH(0)) != 0;
    return FALSE;
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* ---- drawing ------------------------------------------------------------------- */

#define COL_SILVER_TOP 0xD2CFCE
#define COL_SILVER_BOT 0xB1AEAD
#define COL_SILVER_EDGE 0x7D7A79
#define COL_PLATE 0x171717
#define COL_CAP 0xF0EBDF
#define COL_CAP_SIDE 0xA39E91
#define COL_LEGEND 0x1E1E1E
#define COL_FUNC 0x2E2222
#define COL_FUNC_SIDE 0x120B0B
#define COL_FUNC_LEGEND 0xFF5C54
#define COL_RESET_SIDE 0x7A0B10
#define COL_BASE 0x222222
#define COL_PAD 0x3D3D3D

typedef struct {
    gboolean held, selected, dim, hot;
} ItemState;

static ItemState item_state(const Pane *p, int i)
{
    const Item *it = &p->items[i];
    ItemState st;
    st.held = it->lit || p->pressed == i;
    st.selected = g_map_state >= 0 && g_map_state == it->target;
    st.dim = g_map_state >= 0 && !st.selected;
    st.hot = p->hover == i && !st.held;
    return st;
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    o2_binding b;
    dst[0] = '\0';
    if (!showing_bindings()) return;
    b = o2session_binding_get(g_session, target);
    if (b.keysym)
        legend_name(b.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

static void draw_cap(cairo_t *cr, GtkWidget *w, const Pane *p, int i)
{
    const Item *it = &p->items[i];
    const ItemState st = item_state(p, i);
    guint32 face = COL_CAP, side = COL_CAP_SIDE, ink = COL_LEGEND;
    double fh = it->h - 3, ly = it->y, lh = fh;
    char bound[40];
    const int units = (int)g_utf8_strlen(it->label, -1);
    double px;

    if (it->kind == O2_KEYCAP_FUNCTION) {
        face = COL_FUNC; side = COL_FUNC_SIDE; ink = COL_FUNC_LEGEND;
    } else if (it->kind == O2_KEYCAP_RESET) {
        face = O2SESSION_ACCENT_RGB; side = COL_RESET_SIDE; ink = 0xFFFFFF;
    }
    if (st.held || st.selected) {
        face = O2SESSION_ACCENT_RGB; side = O2SESSION_ACCENT_DARK_RGB; ink = 0xFFFFFF;
        if (it->kind == O2_KEYCAP_RESET) face = 0xE8353C;
    } else if (st.hot) {
        face = mix(face, 0xFFFFFF, 0.18);
    }

    if (st.dim) cairo_push_group(cr);
    rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 5);
    rgb(cr, side);
    cairo_fill(cr);
    rrect(cr, it->x, it->y, it->w, fh, 5);
    rgb(cr, face);
    cairo_fill_preserve(cr);
    rgb(cr, mix(face, 0x000000, 0.35));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    bound_text(it->target, bound, sizeof bound);
    if (bound[0]) {
        lh = fh * 0.62;
        legend(cr, w, bound, MAX(7.0, g_unit * 0.2), FALSE,
               mix(ink, face, 0.35), it->x, it->y + lh - 2, it->w, fh - lh);
    }
    if (it->kind == O2_KEYCAP_RESET) {
        /* the console's RESET cap carries a triangle */
        double cx = it->x + it->w / 2, cy = ly + lh / 2, r = MIN(it->w, lh) * 0.22;
        cairo_move_to(cr, cx, cy - r);
        cairo_line_to(cr, cx + r * 1.1, cy + r * 0.8);
        cairo_line_to(cr, cx - r * 1.1, cy + r * 0.8);
        cairo_close_path(cr);
        rgb(cr, ink);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    } else {
        px = it->kind == O2_KEYCAP_SPACE ? g_unit * 0.22
           : units <= 1 ? g_unit * 0.36 : units <= 3 ? g_unit * 0.25 : g_unit * 0.21;
        legend(cr, w, it->label, px, it->kind != O2_KEYCAP_SPACE, ink, it->x, ly, it->w, lh);
    }
    if (st.dim) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.4);
    }
}

static void draw_kbd(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Pane *p = ud;
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, h);
    int i;
    (void)area;

    /* the case */
    cairo_pattern_add_color_stop_rgb(g, 0, ((COL_SILVER_TOP >> 16) & 0xFF) / 255.0,
                                     ((COL_SILVER_TOP >> 8) & 0xFF) / 255.0,
                                     (COL_SILVER_TOP & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgb(g, 1, ((COL_SILVER_BOT >> 16) & 0xFF) / 255.0,
                                     ((COL_SILVER_BOT >> 8) & 0xFF) / 255.0,
                                     (COL_SILVER_BOT & 0xFF) / 255.0);
    rrect(cr, 0.5, 0.5, w - 1, h - 1, 14);
    cairo_set_source(cr, g);
    cairo_fill_preserve(cr);
    rgb(cr, COL_SILVER_EDGE);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    cairo_pattern_destroy(g);

    /* the keyboard plate */
    rrect(cr, PANEL_PAD, PANEL_PAD, w - 2 * PANEL_PAD, h - 2 * PANEL_PAD, 8);
    rgb(cr, COL_PLATE);
    cairo_fill(cr);

    for (i = 0; i < p->n; i++)
        draw_cap(cr, GTK_WIDGET(area), p, i);
}

/* A stick: the base, the fire corner, the pad ring and the knob. */
static void draw_stick(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Pane *p = ud;
    GtkWidget *widget = GTK_WIDGET(area);
    const double cx = STICK_W - 86, cy = STICK_H - 64;
    double kx = 0, ky = 0;
    char name[24], bound[40];
    int i;

    rrect(cr, 0.5, 0.5, w - 1, h - 1, 14);
    rgb(cr, COL_BASE);
    cairo_fill_preserve(cr);
    rgb(cr, 0x000000);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    /* the lit top edge of the moulding */
    cairo_move_to(cr, 16, 2.5);
    cairo_line_to(cr, w - 16, 2.5);
    rgb(cr, 0x454545);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    /* the well the pads sit in */
    cairo_arc(cr, cx, cy, RING_R1 + 5, 0, 2 * G_PI);
    rgb(cr, 0x151515);
    cairo_fill(cr);

    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        const ItemState st = item_state(p, i);
        guint32 face, ink = 0xFFFFFF;

        if (it->kind == K_PAD) {
            const double mid = (RING_R0 + RING_R1) / 2;
            const double px = cx + cos(it->a) * mid, py = cy + sin(it->a) * mid;
            face = st.held || st.selected ? O2SESSION_ACCENT_RGB
                 : st.hot ? mix(COL_PAD, 0xFFFFFF, 0.15) : COL_PAD;
            if (st.dim) cairo_push_group(cr);
            cairo_arc(cr, cx, cy, RING_R1, it->a - PAD_DRAW_SPAN, it->a + PAD_DRAW_SPAN);
            cairo_arc_negative(cr, cx, cy, RING_R0, it->a + PAD_DRAW_SPAN, it->a - PAD_DRAW_SPAN);
            cairo_close_path(cr);
            rgb(cr, face);
            cairo_fill(cr);
            bound_text(it->target, bound, sizeof bound);
            if (bound[0]) {
                legend(cr, widget, bound, 9, TRUE, st.held || st.selected ? ink : 0xBDBDBD,
                       px - 20, py - 9, 40, 18);
            } else {
                /* an arrow pointing the way the pad pushes */
                const double ux = cos(it->a), uy = sin(it->a);
                cairo_move_to(cr, px + ux * 6, py + uy * 6);
                cairo_line_to(cr, px - ux * 4 - uy * 6, py - uy * 4 + ux * 6);
                cairo_line_to(cr, px - ux * 4 + uy * 6, py - uy * 4 - ux * 6);
                cairo_close_path(cr);
                rgb(cr, st.held || st.selected ? ink : 0xBDBDBD);
                cairo_fill(cr);
            }
            if (st.dim) {
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.4);
            }
            if (st.held) {
                /* the stick leans toward the direction held */
                kx += cos(it->a) * 7;
                ky += sin(it->a) * 7;
            }
        }
    }

    /* the stick: a knob on its shaft */
    cairo_arc(cr, cx + kx, cy + ky, 19, 0, 2 * G_PI);
    rgb(cr, 0x0B0B0B);
    cairo_fill(cr);
    cairo_arc(cr, cx + kx - 5, cy + ky - 5, 6, 0, 2 * G_PI);
    rgb(cr, 0x4A4A4A);
    cairo_fill(cr);

    /* the fire button, in its corner */
    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        const ItemState st = item_state(p, i);
        if (it->kind != K_FIRE) continue;
        if (st.dim) cairo_push_group(cr);
        rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 6);
        rgb(cr, COL_RESET_SIDE);
        cairo_fill(cr);
        rrect(cr, it->x, it->y, it->w, it->h - 3, 6);
        rgb(cr, st.held || st.selected ? 0xFF4048 : st.hot ? 0xD8262D : O2SESSION_ACCENT_RGB);
        cairo_fill_preserve(cr);
        rgb(cr, st.held || st.selected ? 0xFFFFFF : 0x6B0A0E);
        cairo_set_line_width(cr, st.held || st.selected ? 2 : 1);
        cairo_stroke(cr);
        bound_text(it->target, bound, sizeof bound);
        if (bound[0]) {
            legend(cr, widget, "FIRE", 12, TRUE, 0xFFFFFF, it->x, it->y + 2, it->w, 16);
            legend(cr, widget, bound, 9, FALSE, 0xFFD9D9, it->x, it->y + 17, it->w, 12);
        } else {
            legend(cr, widget, "FIRE", 13, TRUE, 0xFFFFFF, it->x, it->y, it->w, it->h - 3);
        }
        if (st.dim) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.4);
        }
    }

    g_snprintf(name, sizeof name, "STICK %d", p->port + 1);
    legend(cr, widget, name, 11, TRUE, 0x8A8A8A, 12, h - 28, 64, 16);
}

/* The console buttons under the sticks. */
static void draw_strip(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud)
{
    Pane *p = ud;
    int i;
    (void)w; (void)h;

    for (i = 0; i < p->n; i++) {
        const Item *it = &p->items[i];
        const ItemState st = item_state(p, i);
        guint32 face = st.held || st.selected ? O2SESSION_ACCENT_RGB
                     : st.hot ? 0x4A4A52 : 0x38383E;
        char bound[40], text[120];

        if (st.dim) cairo_push_group(cr);
        rrect(cr, it->x, it->y + 3, it->w, it->h - 3, 7);
        rgb(cr, st.held || st.selected ? O2SESSION_ACCENT_DARK_RGB : 0x1A1A1D);
        cairo_fill(cr);
        rrect(cr, it->x, it->y, it->w, it->h - 3, 7);
        rgb(cr, face);
        cairo_fill(cr);
        bound_text(it->target, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", it->label, bound);
        else
            g_snprintf(text, sizeof text, "%s", it->label);
        legend(cr, GTK_WIDGET(area), text, 12, FALSE, 0xFFFFFF, it->x + 4, it->y, it->w - 8,
               it->h - 3);
        if (st.dim) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.4);
        }
    }
}

/* ---- hit testing -------------------------------------------------------------------- */

static gboolean item_hit(const Item *it, double x, double y)
{
    if (it->shape == SH_RECT)
        return x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h;
    {
        const double cx = STICK_W - 86, cy = STICK_H - 64;
        const double dx = x - cx, dy = y - cy;
        const double r = sqrt(dx * dx + dy * dy);
        double d;
        if (r < RING_R0 || r > RING_R1) return FALSE;
        d = remainder(atan2(dy, dx) - it->a, 2 * G_PI);
        return fabs(d) <= PAD_HIT_SPAN;
    }
}

static int pane_hit(const Pane *p, double x, double y)
{
    int i;
    for (i = p->n - 1; i >= 0; i--)
        if (item_hit(&p->items[i], x, y))
            return i;
    return -1;
}

/* ---- pressing ------------------------------------------------------------- */

static void run_sysaction(int sa)
{
    if (g_parent)
        o2_window_run_sysaction(O2_WINDOW(g_parent), sa);
    else
        o2session_sysaction(g_session, sa);
}

/* Lets go of what the mouse holds in one pane. A system action fires on
 * release, like a real button: pressing and dragging off should not
 * power-cycle the cartridge. */
static void pane_release(Pane *p, gboolean activate)
{
    const Item *it;
    if (p->pressed < 0) return;
    it = &p->items[p->pressed];
    p->pressed = -1;
    if (is_sysact(it->target)) {
        if (activate) run_sysaction(it->target - O2_TARGET_SYSACT(0));
    } else {
        o2session_press(g_session, it->target, 0);
    }
    gtk_widget_queue_draw(p->area);
}

static void mouse_release(void)
{
    int i;
    for (i = 0; i < PANE_COUNT; i++)
        pane_release(&g_pane[i], FALSE);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    o2session_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[220];
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (o2session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        o2session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(o2_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "o2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a key or control with the mouse. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "o2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a key or control to remap it");
    } else {
        char msg[160];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   o2_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        o2session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

static void on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    Pane *p = ud;
    int i = pane_hit(p, x, y);
    const Item *it;
    (void)g;
    if (i < 0) return;
    it = &p->items[i];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target */
        set_map_state(it->target);
        return;
    }
    mouse_release();
    p->pressed = i;
    if (!is_sysact(it->target))
        o2session_press(g_session, it->target, 1);
    gtk_widget_queue_draw(p->area);
}

static void on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer ud)
{
    (void)g; (void)ox; (void)oy;
    pane_release(ud, TRUE);
}

static void on_cancel(GtkGesture *g, GdkEventSequence *seq, gpointer ud)
{
    (void)g; (void)seq;
    pane_release(ud, FALSE);
}

/* Dragging off the cap lets it go. */
static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer ud)
{
    Pane *p = ud;
    int i = pane_hit(p, x, y);
    (void)c;
    if (p->pressed >= 0 && i != p->pressed)
        pane_release(p, FALSE);
    if (i != p->hover) {
        p->hover = i;
        gtk_widget_queue_draw(p->area);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer ud)
{
    Pane *p = ud;
    (void)c;
    pane_release(p, FALSE);
    if (p->hover >= 0) {
        p->hover = -1;
        gtk_widget_queue_draw(p->area);
    }
}

static gboolean on_query_tooltip(GtkWidget *w, int x, int y, gboolean kbd,
                                 GtkTooltip *tip, gpointer ud)
{
    Pane *p = ud;
    int i = kbd ? -1 : pane_hit(p, x, y);
    o2_binding b;
    char key[32], text[260], name[80];
    (void)w;
    if (i < 0) return FALSE;
    b = o2session_binding_get(g_session, p->items[i].target);
    o2session_keysym_name(b.keysym, key, sizeof key);
    g_strlcpy(name, o2_target_name(p->items[i].target), sizeof name);
    g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s", name,
               key[0] ? key : "none",
               b.button != O2_PAD_BTN_NONE ? o2_pad_button_name(b.button) : "none");
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

static void pane_init(Pane *p, int port, GtkDrawingAreaDrawFunc draw)
{
    GtkGesture *drag = gtk_gesture_drag_new();
    GtkEventController *motion = gtk_event_controller_motion_new();

    p->area = gtk_drawing_area_new();
    p->port = port;
    p->pressed = p->hover = -1;
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(p->area), draw, p, NULL);
    gtk_widget_set_halign(p->area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(p->area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a key must not steal focus. */
    gtk_widget_set_focusable(p->area, FALSE);
    gtk_widget_set_has_tooltip(p->area, TRUE);
    g_signal_connect(p->area, "query-tooltip", G_CALLBACK(on_query_tooltip), p);

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), p);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), p);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_cancel), p);
    gtk_widget_add_controller(p->area, GTK_EVENT_CONTROLLER(drag));
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), p);
    gtk_widget_add_controller(p->area, motion);
}

/* ---- building the controls ------------------------------------------------------------ */

static Item *pane_add(Pane *p, int shape, int kind, int target, const char *label)
{
    Item *it;
    if (p->n >= MAX_ITEMS) return NULL;
    it = &p->items[p->n++];
    memset(it, 0, sizeof *it);
    it->shape = shape;
    it->kind = kind;
    it->target = target;
    it->label = label;
    return it;
}

static int machine_now(void)
{
    int m = o2session_get_int(g_session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

static void build_keyboard(void)
{
    Pane *p = &g_pane[PANE_KBD];
    float kw, kh;
    int n, i;
    const o2_keycap *caps = o2session_keyboard_layout(g_built_machine, &n, &kw, &kh);
    const double off = PANEL_PAD + PLATE_PAD;

    g_kbd_w = kw;
    g_kbd_h = kh;
    g_unit = (int)(KBD_WIDTH_PX / kw);
    if (g_unit > UNIT_MAX) g_unit = UNIT_MAX;
    if (g_unit < UNIT_MIN) g_unit = UNIT_MIN;

    p->n = 0;
    p->pressed = p->hover = -1;
    for (i = 0; i < n; i++) {
        const o2_keycap *c = &caps[i];
        Item *it = pane_add(p, SH_RECT, c->kind,
                            c->target >= 0 ? O2_TARGET_KEY(c->target)
                                           : O2_TARGET_SWITCH(O2_SW_RESET), c->label);
        if (!it) break;
        it->x = off + c->x * g_unit;
        it->y = off + c->y * g_unit;
        it->w = c->w * g_unit;
        it->h = c->h * g_unit;
    }
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), (int)(kw * g_unit + 2 * off));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), (int)(kh * g_unit + 2 * off));
}

static void build_stick(int port)
{
    static const double angles[O2_ACT_FIRE] = { -G_PI / 2, 0, G_PI / 2, G_PI };
    Pane *p = &g_pane[PANE_STICK0 + port];
    Item *it;
    int act;

    p->n = 0;
    p->pressed = p->hover = -1;
    it = pane_add(p, SH_RECT, K_FIRE, O2_TARGET_PORT(port, O2_ACT_FIRE), "FIRE");
    it->x = 14; it->y = 14; it->w = 78; it->h = 38;
    for (act = O2_ACT_UP; act < O2_ACT_FIRE; act++) {
        it = pane_add(p, SH_WEDGE, K_PAD, O2_TARGET_PORT(port, act), "");
        it->a = angles[act];
    }
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), STICK_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), STICK_H);
}

static void build_strip(void)
{
    Pane *p = &g_pane[PANE_STRIP];
    const int bw = STICK_W;
    Item *it;

    p->n = 0;
    p->pressed = p->hover = -1;
    it = pane_add(p, SH_RECT, K_BUTTON, O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG),
                  "Reset to CONFIG");
    it->x = 0; it->y = 0; it->w = bw; it->h = STRIP_H;
    it = pane_add(p, SH_RECT, K_BUTTON, O2_TARGET_SYSACT(O2_SYSACT_DEBUG_STOP),
                  "Stop in Debugger");
    it->x = bw + STRIP_GAP; it->y = 0; it->w = bw; it->h = STRIP_H;
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(p->area), 2 * bw + STRIP_GAP);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(p->area), STRIP_H);
}

static void rebuild(void)
{
    mouse_release();
    g_built_machine = machine_now();
    build_keyboard();
    build_stick(0);
    build_stick(1);
    build_strip();
    adw_window_title_set_subtitle(g_title, o2_machine_name(g_built_machine));
    refresh_all();
}

static void refresh_all(void)
{
    int i;
    for (i = 0; i < PANE_COUNT; i++)
        if (g_pane[i].area)
            gtk_widget_queue_draw(g_pane[i].area);
}

/* What the console sees held, painted in the accent. Redraws only what
 * changed. */
static gboolean held_tick(gpointer d)
{
    unsigned held[2];
    int i, k;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    held[0] = o2session_buttons_held(g_session, 0);
    held[1] = o2session_buttons_held(g_session, 1);
    for (i = 0; i < PANE_COUNT; i++) {
        Pane *p = &g_pane[i];
        gboolean changed = FALSE;
        for (k = 0; k < p->n; k++) {
            gboolean on = item_held(&p->items[k], held);
            if (on != p->items[k].lit) {
                p->items[k].lit = on;
                changed = TRUE;
            }
        }
        if (changed)
            gtk_widget_queue_draw(p->area);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int i;
    for (i = 0; i < g_npad_rows; i++) {
        int effective = o2session_gamepad_effective_port(g_session, i);
        char text[32];
        if (effective >= 0) g_snprintf(text, sizeof text, "drives stick %d", effective + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[i])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[i]), text);
    }
}

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1-2 = that stick */
    o2session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the stick it drives and the stick it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Stick 1", "Stick 2", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_npad_rows = 0;

    n = o2session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n && i < MAX_PADS; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128];
        int assign = o2session_gamepad_assignment(g_session, i);

        o2session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(dd, FALSE);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_port[g_npad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in
 * Preferences. */
static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = o2session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if (machine_now() != g_built_machine)
        rebuild();
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key(guint keyval, guint code, GdkModifierType st, gboolean down)
{
    /* an open drop-down (a gamepad's stick) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    /* The window's own keys and the shortcuts are never Map's. */
    if (g_map_state != -2 && keyval != GDK_KEY_F9 && keyval != GDK_KEY_F11 &&
        keyval != GDK_KEY_F12 && !(st & GDK_CONTROL_MASK)) {
        guint32 keysym;
        char stolen[128], name[32];
        if (!down) return TRUE;
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0)
            return TRUE;          /* armed, no target yet: typing is not typed */
        keysym = code >= 8 ? o2session_keysym_from_evdev(code - 8) : 0;
        if (!keysym) keysym = keyval;
        o2session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        o2session_keysym_name(keysym, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);        /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return TRUE;
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return o2_window_key(O2_WINDOW(g_parent), keyval, code, st, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    return on_key(keyval, code, st, TRUE);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    on_key(keyval, code, st, FALSE);
}

/* Only what this window's mouse holds: the main window releases its own. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    mouse_release();
    g_swallow_code = 0;     /* its release went elsewhere */
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    o2session_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *b, gpointer d)
{
    (void)d;
    o2session_set_int(g_session, "controllers_show_bindings",
                      gtk_check_button_get_active(b) ? 1 : 0);
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    hide_window();
    return TRUE;
}

/* The refresh timers run only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer) g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_held_timer = g_pad_timer = 0;
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_capture_timer = g_held_timer = g_pad_timer = 0;
    memset(g_pane, 0, sizeof g_pane);
    g_window = NULL;
    g_npad_rows = 0;
    g_built_machine = -1;
    g_map_state = -2;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *sticks, *maprow, *toolbar, *header, *defaults, *padtitle;
    GtkEventController *keys, *focus;

    o2_install_accent_css();
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);

    memset(g_pane, 0, sizeof g_pane);
    pane_init(&g_pane[PANE_KBD], 0, draw_kbd);
    pane_init(&g_pane[PANE_STICK0], 0, draw_stick);
    pane_init(&g_pane[PANE_STICK1], 1, draw_stick);
    pane_init(&g_pane[PANE_STRIP], 0, draw_strip);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    gtk_box_append(GTK_BOX(root), g_pane[PANE_KBD].area);
    sticks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, STRIP_GAP);
    gtk_widget_set_halign(sticks, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(sticks), g_pane[PANE_STICK0].area);
    gtk_box_append(GTK_BOX(sticks), g_pane[PANE_STICK1].area);
    gtk_box_append(GTK_BOX(root), sticks);
    gtk_box_append(GTK_BOX(root), g_pane[PANE_STRIP].area);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings),
        o2session_get_int(g_session, "controllers_show_bindings", 0) != 0);
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint ("Press a key or a gamepad button for Stick 1: Right")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(root), maprow);
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Controllers", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g_title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_built_machine = -1;
    set_map_state(-2);
    rebuild();
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void show_window(void)
{
    if (machine_now() != g_built_machine)
        rebuild();
    g_pad_generation = o2session_gamepad_generation(g_session);
    refresh_pads();
    gtk_window_present(GTK_WINDOW(g_window));
}

void o2_controllers_toggle(GtkWindow *parent, o2session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);

    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        show_window();
}

gboolean o2_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}

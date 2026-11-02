/*
 * Debugger window (GTK4/libadwaita) over the session's CDP1802 / CDP1861
 * debugger engine, via core/include/o2debug.h.
 *
 * Tabs: a Prompt (the contract's command set, with tab completion and a
 * command history), the 1802's registers + the console's 512 bytes of RAM,
 * the disassembly with a breakpoint gutter, the 1861 (its state, and the
 * whole raster, the cartridge's served raster and the BIOS's display page
 * drawn), the beeper, the keypads and the I/O ports, breakpoints, the
 * FujiNet cartridge (its state, a map of the pages it serves and its text
 * engine's screen), and FujiNet: every FujiBus packet on the cartridge's
 * link, both ways, with hex. A toolbar carries the stepping controls,
 * symbol loading and file saves; the family's keys apply (F5 run/stop, F7
 * step, F8 step over, Shift+F8 step out, F12 close).
 *
 * The engine exists only while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling), hiding it detaches and
 * lets the machine run on.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only when something changed, so a stopped machine costs
 * nothing and a running one shows live values twice a second. Every
 * o2debug call waits for the frame in progress (the host's run lock), so
 * looking at a running machine is safe.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "o2debug.h"
#include "../window.h"

#define DISASM_WINDOW 48
#define NREGS 25
#define RAM_BASE 0x0800
#define RAM_SIZE 512
#define TRACE_MAX 512

/* The video views are drawn bigger by these, nearest-neighbour, about as a
 * television showed them: a 1861 dot is some three lines wide (the 100 x
 * 246 area fills a 4:3 screen), and each row of the BIOS's display page is
 * four lines tall on the screen. */
static const struct { int sx, sy; } k_view_scale[3] = { { 6, 2 }, { 6, 2 }, { 6, 8 } };
/* The scaled buffer, in view-sized units: the screen at 6 x 2 is the most
 * any view takes. */
#define VIEW_SCALE_MAX 12

/* The colours of the cartridge's page map, by S2PG_* type. */
#define PAGE_HOT_RGB    0xE66100
#define PAGE_RASTER_RGB 0x3584E4

typedef struct {
    GtkWindow *win;
    o2session *session;
    o2debug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* prompt */
    GtkTextView *prompt_out;
    GtkEntry *prompt_in;
    GtkScrolledWindow *prompt_scroll;
    GPtrArray *history;
    guint history_pos;

    /* cpu + ram */
    GtkEntry *reg[NREGS];     /* R0-RF, then PC D DF P X T B IE Q */
    GtkWidget *ef[4];         /* EF1-EF4, read-only */
    GtkLabel *beam;
    GtkTextView *ram_view;
    GtkEntry *ram_goto, *ram_addr, *ram_val;
    int ram_mark_row;         /* the row Go to found, highlighted; -1 none */

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first line */
    uint16_t line_addr[DISASM_WINDOW];
    int line_count;

    /* video */
    GtkTextView *video_text;
    GtkPicture *video_pic;
    GtkDropDown *video_view;
    guint32 *view_px, *view_px2;

    /* sound & i/o */
    GtkTextView *io_text;
    GtkWidget *keys_area[2];
    unsigned io_keys[2];      /* as the console latched them, bit n = key n */
    int io_latch;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;

    /* cart */
    GtkTextView *cart_text;
    GtkWidget *page_map, *text_grid;
    o2debug_cart cart;   /* the last read, for the two drawings */

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* The register entries: R0-RF, then R(P) and the rest of the 1802, each
 * with how many hex digits it shows. */
static const struct { const char *name; int reg; int digits; } k_regs[NREGS] = {
    { "R0", O2_REG_R0 + 0, 4 }, { "R1", O2_REG_R0 + 1, 4 },
    { "R2", O2_REG_R0 + 2, 4 }, { "R3", O2_REG_R0 + 3, 4 },
    { "R4", O2_REG_R0 + 4, 4 }, { "R5", O2_REG_R0 + 5, 4 },
    { "R6", O2_REG_R0 + 6, 4 }, { "R7", O2_REG_R0 + 7, 4 },
    { "R8", O2_REG_R0 + 8, 4 }, { "R9", O2_REG_R0 + 9, 4 },
    { "RA", O2_REG_R0 + 10, 4 }, { "RB", O2_REG_R0 + 11, 4 },
    { "RC", O2_REG_R0 + 12, 4 }, { "RD", O2_REG_R0 + 13, 4 },
    { "RE", O2_REG_R0 + 14, 4 }, { "RF", O2_REG_R0 + 15, 4 },
    { "PC", O2_REG_PC, 4 }, { "D", O2_REG_D, 2 }, { "DF", O2_REG_DF, 1 },
    { "P", O2_REG_P, 1 }, { "X", O2_REG_X, 1 }, { "T", O2_REG_T, 2 },
    { "B", O2_REG_B, 2 }, { "IE", O2_REG_IE, 1 }, { "Q", O2_REG_Q, 1 },
};

/* The tabs, in order, by the names O2_DEBUGGER_TAB takes. */
static const char *const k_tab_names[] = {
    "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet",
};
#define TAB_FUJINET 7         /* refreshed live only while it shows */

/* ---- helpers -------------------------------------------------------------- */

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the FujiNet packets) follows its end while its reader is there.
 * New text grows the view only once it is laid out (or its tab first
 * shows), so the end is pinned whenever the view's height changes; whether
 * to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    g_signal_connect(vadj(view), "changed", G_CALLBACK(on_log_resized), view);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled RAM dump back
 * to its top. The top line is noted and scrolled back to once the new text
 * is laid out (scroll_to_mark waits for that). A log that is following
 * goes to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    {
        /* unchanged text leaves the buffer, the scroll position and any
         * selection alone */
        GtkTextIter s, e;
        char *old;
        gboolean same;

        gtk_text_buffer_get_bounds(b, &s, &e);
        old = gtk_text_buffer_get_text(b, &s, &e, TRUE);
        same = strcmp(old, text) == 0;
        g_free(old);
        if (same)
            return;
    }
    gtk_text_buffer_set_text(b, text, -1);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, GtkScrolledWindow *scroll, const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    GtkAdjustment *adj;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    adj = gtk_scrolled_window_get_vadjustment(scroll);
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

static int parse_num(const char *text, long *out)
{
    char *end;
    long v;
    while (*text == ' ') text++;
    if (*text == '$') v = strtol(text + 1, &end, 16);
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) v = strtol(text + 2, &end, 16);
    else if (*text == '#') v = strtol(text + 1, &end, 10);
    else v = strtol(text, &end, 16);
    if (end == text || *end) return 0;
    *out = v;
    return 1;
}

/* A label or a number, as typed into an address box. */
static int parse_addr(DbgWin *w, const char *text, long *out)
{
    int a = o2debug_label_address(w->dbg, text);
    if (a >= 0) { *out = a; return 1; }
    return parse_num(text, out);
}

/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && !o2debug_is_stopped(w->dbg) &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))))
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), editable);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    /* beside a wrapped hint, an entry keeps its own height */
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    return box;
}

static GtkWidget *heading(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

/* The tan is pale, so a line marked in it keeps dark text on either theme. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", O2SESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#000000", NULL);
}

static void set_rgb(cairo_t *cr, guint32 rgb, double alpha)
{
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

/* The theme's text colour, for lines and labels drawn by hand. */
static void set_fg(cairo_t *cr, GtkWidget *w, double alpha)
{
    GdkRGBA c;
    gtk_widget_get_color(w, &c);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, c.alpha * alpha);
}

/* The accent as a line colour: the tan on a dark theme, the darker tan on
 * a light one (the tan alone is too pale to see against white). */
static guint32 accent_line_rgb(void)
{
    return adw_style_manager_get_dark(adw_style_manager_get_default())
        ? O2SESSION_ACCENT_RGB : O2SESSION_ACCENT_DARK_RGB;
}

/* Text centred in a cell, in the widget's font (monospace when asked). */
static void draw_centred(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                         double cw, double ch, gboolean mono)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
    int tw, th;
    if (mono) {
        PangoFontDescription *fd = pango_font_description_from_string("Monospace 10");
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);
    }
    pango_layout_get_pixel_size(layout, &tw, &th);
    cairo_move_to(cr, x + (cw - tw) / 2.0, y + (ch - th) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* Messages from Load Symbols and Save land in the prompt's transcript and,
 * since the prompt may not be the tab showing, in a toast. */
static void prompt_say(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->prompt_out, w->prompt_scroll, line);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    o2debug_cpu c;
    char buf[300];
    int i;
    o2debug_cpu_get(w->dbg, &c);
    {
        int vals[NREGS];
        for (i = 0; i < 16; i++)
            vals[i] = c.r[i];
        vals[16] = c.pc; vals[17] = c.d; vals[18] = c.df; vals[19] = c.p;
        vals[20] = c.x; vals[21] = c.t; vals[22] = c.b; vals[23] = c.ie; vals[24] = c.q;
        for (i = 0; i < NREGS; i++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[i].digits, (unsigned)vals[i]);
            set_entry(w, w->reg[i], buf);
        }
    }
    /* EF1-EF4: the 1861's EFx is EF1, the keypads EF3 and EF4 */
    for (i = 0; i < 4; i++) {
        if (c.ef[i]) {
            gtk_widget_add_css_class(w->ef[i], "o2-accent");
            gtk_widget_remove_css_class(w->ef[i], "dim-label");
        } else {
            gtk_widget_remove_css_class(w->ef[i], "o2-accent");
            gtk_widget_add_css_class(w->ef[i], "dim-label");
        }
    }
    g_snprintf(buf, sizeof buf,
               "state %s   mode %s   INT %s   DMA OUT %s   line %d   dot %d   frame %u   "
               "cycle %llu   instruction %llu",
               c.state_name ? c.state_name : "?", c.mode_name ? c.mode_name : "?",
               c.int_line ? "asserted" : "-", c.dma_line ? "asserted" : "-",
               c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles,
               (unsigned long long)c.instructions);
    gtk_label_set_text(w->beam, buf);
}

static void refresh_ram(DbgWin *w)
{
    static uint8_t ram[RAM_SIZE];
    static char text[RAM_SIZE * 4 + 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->ram_view);
    int len = 0, row, col;
    o2debug_ram_get(w->dbg, ram);
    len += g_snprintf(text + len, sizeof text - len,
                      "       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < RAM_SIZE / 16; row++) {
        len += g_snprintf(text + len, sizeof text - len, "$%04X: ", RAM_BASE + row * 16);
        for (col = 0; col < 16; col++)
            len += g_snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += g_snprintf(text + len, sizeof text - len, "\n");
    }
    set_text(w->ram_view, text);

    /* the row Go to found stays marked across refreshes */
    if (w->ram_mark_row >= 0) {
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(b, &s, w->ram_mark_row + 1);
        gtk_text_buffer_get_iter_at_line(b, &e, w->ram_mark_row + 2);
        gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
    }
}

static void refresh_disasm(DbgWin *w)
{
    static o2debug_line lines[DISASM_WINDOW];
    static char text[DISASM_WINDOW * 220];
    int n, pc_line, i, len = 0;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkTextIter s, e;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        o2debug_cpu c;
        o2debug_cpu_get(w->dbg, &c);
        w->disasm_top = o2debug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }

    n = o2debug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        w->line_addr[i] = lines[i].address;
        len += g_snprintf(text + len, sizeof text - len, "%c%c %04X  %-9s %-14s %s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ',
                          lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label,
                          lines[i].disasm, lines[i].comment[0] ? "  ; " : "",
                          lines[i].comment);
    }
    if (n == 0)
        len += g_snprintf(text + len, sizeof text - len, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, text, -1);

    /* the PC line in the accent colour */
    if (pc_line >= 0 && pc_line < n) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_line);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
}

static void show_view(DbgWin *w)
{
    int view = (int)gtk_drop_down_get_selected(w->video_view);
    int vw = 0, vh = 0, x, y, ow, oh, sx, sy;
    GBytes *bytes;
    GdkTexture *tex;

    if (view < 0 || view >= (int)G_N_ELEMENTS(k_view_scale) ||
        !o2debug_video_view(w->dbg, view, w->view_px, &vw, &vh) ||
        vw <= 0 || vh <= 0 || vw * vh > O2DEBUG_VIEW_MAX_PIXELS)
        return;
    sx = k_view_scale[view].sx;
    sy = k_view_scale[view].sy;
    ow = vw * sx;
    oh = vh * sy;
    if (ow * oh > O2DEBUG_VIEW_MAX_PIXELS * VIEW_SCALE_MAX)
        return;
    for (y = 0; y < oh; y++)
        for (x = 0; x < ow; x++)
            w->view_px2[y * ow + x] = w->view_px[(y / sy) * vw + x / sx] | 0xFF000000u;
    bytes = g_bytes_new(w->view_px2, (gsize)ow * oh * 4);
    tex = gdk_memory_texture_new(ow, oh, GDK_MEMORY_B8G8R8A8, bytes, (gsize)ow * 4);
    gtk_picture_set_paintable(w->video_pic, GDK_PAINTABLE(tex));
    gtk_widget_set_size_request(GTK_WIDGET(w->video_pic), ow, oh);
    g_object_unref(tex);
    g_bytes_unref(bytes);
}

static void refresh_video(DbgWin *w)
{
    o2debug_video v;
    static char text[2048];

    o2debug_video_get(w->dbg, &v);
    /* lines short enough for the 420-pixel pane */
    g_snprintf(text, sizeof text,
        "CDP1861 (the Pixie)\n\n"
        "Display       %s\n"
        "INP 1         %u strobes (display on)\n"
        "Beam          line %d   dot %d\n"
        "              %s\n"
        "INT           %s   next on line %d\n"
        "EFx (EF1)     %s   next on line %d\n"
        "DMA OUT       %s\n"
        "DMA pointer   R0 = $%04X\n"
        "Last fetch    $%04X\n"
        "DMA bytes     %llu since power-on\n"
        "Short lines   %u\n"
        "              (DMA bursts under 8: a $Cx\n"
        "              opcode was in flight)\n"
        "Next INT      clock %llu\n"
        "Next EFx      clock %llu\n"
        "Next DMA      clock %llu\n"
        "Frame         %u\n",
        v.display_on ? "on" : "off", v.inp1_count,
        v.vpos, v.hpos, v.in_display ? "in the picture (lines 80-207)" : "in the blanking",
        v.int_line ? "asserted" : "-", v.next_int_line,
        v.efx_line ? "asserted" : "-", v.next_efx_line,
        v.dma_line ? "asserted" : "-",
        v.dma_pointer, v.last_dma_addr, (unsigned long long)v.dma_bytes,
        v.short_lines,
        (unsigned long long)v.next_int_clock, (unsigned long long)v.next_efx_clock,
        (unsigned long long)v.next_dma_clock, v.frame);
    set_text(w->video_text, text);
    show_view(w);
}

/* The keys a keypad's held keys name, "1 5 0", or "(nothing held)". */
static void keys_text(unsigned mask, char *dst, int dstsz)
{
    int k, len = 0;
    dst[0] = '\0';
    for (k = 1; k <= 10; k++) {
        int key = k % 10;      /* 1-9, then 0, as the keypad reads */
        if (mask & (1u << key))
            len += g_snprintf(dst + len, dstsz - len, "%d ", key);
    }
    if (!len) g_snprintf(dst, dstsz, "(nothing held)");
}

static void refresh_io(DbgWin *w)
{
    o2debug_io io;
    static char text[2048];
    char pad[2][40];
    int i;

    o2debug_io_get(w->dbg, &io);
    keys_text(io.keys[0], pad[0], sizeof pad[0]);
    keys_text(io.keys[1], pad[1], sizeof pad[1]);
    g_snprintf(text, sizeof text,
        "Beeper        Q %s   %d Hz\n"
        "Key latch     %u   (OUT 2 picks the key EF3 and EF4 read)\n"
        "EF3           %d   keypad A: the latched key %s\n"
        "EF4           %d   keypad B: the latched key %s\n"
        "Keypad A      %s\n"
        "Keypad B      %s\n"
        "CLEAR         %s\n"
        "Last OUT      1:$%02X  2:$%02X  3:$%02X  4:$%02X  5:$%02X  6:$%02X  7:$%02X\n"
        "INP 1         %u   (display-on strobes)\n"
        "BIOS          %s\n",
        io.q ? "on " : "off", io.beep_hz,
        io.keylatch,
        io.ef3, io.ef3 ? "is held" : "is up",
        io.ef4, io.ef4 ? "is held" : "is up",
        pad[0], pad[1],
        io.clear_held ? "held" : "-",
        io.last_out[1], io.last_out[2], io.last_out[3], io.last_out[4],
        io.last_out[5], io.last_out[6], io.last_out[7],
        io.inp1_count,
        io.have_bios ? "imported (the real BIOS runs)"
        : io.stub_parked ? "the app's stand-in -- parked: it met a game, which needs the BIOS"
                         : "the app's stand-in (CONFIG and FujiNet apps)");
    set_text(w->io_text, text);

    for (i = 0; i < 2; i++) {
        if (w->io_keys[i] != io.keys[i] || w->io_latch != io.keylatch) {
            w->io_keys[i] = io.keys[i];
            gtk_widget_queue_draw(w->keys_area[i]);
        }
    }
    w->io_latch = io.keylatch;
}

/* One keypad as the console sees it: the console's 3 x 4 layout, held keys
 * filled in the accent, and the key the latch selects outlined. */
#define KEY_CELL_W 44
#define KEY_CELL_H 36
#define KEY_GAP 6

static void draw_keypad(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = g_object_get_data(G_OBJECT(area), "dbg");
    int pad = GPOINTER_TO_INT(ud);
    int key;
    (void)width; (void)height;
    if (!w) return;
    for (key = 0; key <= 9; key++) {
        int col = key ? (key - 1) % 3 : 1;
        int row = key ? (key - 1) / 3 : 3;
        double x = col * (KEY_CELL_W + KEY_GAP) + 1.5;
        double y = row * (KEY_CELL_H + KEY_GAP) + 1.5;
        gboolean held = (w->io_keys[pad] >> key) & 1;
        char face[4];

        cairo_rectangle(cr, x, y, KEY_CELL_W - 3, KEY_CELL_H - 3);
        if (held) {
            set_rgb(cr, O2SESSION_ACCENT_RGB, 1.0);
            cairo_fill_preserve(cr);
        }
        if (w->io_latch == key) {
            set_rgb(cr, accent_line_rgb(), 1.0);
            cairo_set_line_width(cr, 3);
        } else {
            set_fg(cr, GTK_WIDGET(area), 0.35);
            cairo_set_line_width(cr, 1);
        }
        cairo_stroke(cr);

        g_snprintf(face, sizeof face, "%d", key);
        if (held) cairo_set_source_rgb(cr, 0, 0, 0);
        else set_fg(cr, GTK_WIDGET(area), 1.0);
        draw_centred(cr, GTK_WIDGET(area), face, x, y, KEY_CELL_W - 3, KEY_CELL_H - 3, FALSE);
    }
}

static void refresh_cart(DbgWin *w)
{
    o2debug_cart *c = &w->cart;
    char info[256];
    static char text[4096];

    o2debug_cart before = *c;

    o2debug_cart_get(w->dbg, c);
    o2debug_cart_info(w->dbg, info, sizeof info);
    /* the two drawings only when what they draw changed */
    if (before.present != c->present ||
        memcmp(before.page_type, c->page_type, sizeof c->page_type) != 0)
        gtk_widget_queue_draw(w->page_map);
    if (before.present != c->present ||
        memcmp(before.text, c->text, sizeof c->text) != 0 ||
        memcmp(before.text_inv, c->text_inv, sizeof c->text_inv) != 0 ||
        before.cursor_row != c->cursor_row || before.cursor_col != c->cursor_col)
        gtk_widget_queue_draw(w->text_grid);
    if (!c->present) {
        set_text(w->cart_text, info);
        return;
    }
    g_snprintf(text, sizeof text,
        "%s\n\n"
        "Mode         %s (%d)%s%s\n"
        "Link         %s%s\n"
        "Mailbox      %s\n"
        "Live image   %u bytes   CRC-32 %08X   %s   %d blocks%s\n"
        "Staged       %s   claim %d   CONFIG %d   armed %d   CRC-32 %08X\n"
        "Swaps        %d\n"
        "Boot         state %d   %d%%   error %d   %u / %u bytes\n"
        "Mailbox regs ACKSEQ $%02X   STATUS $%02X   last error %u   reply cmd $%02X   "
        "RXLEN %u\n"
        "Raster       %u frames served (mod 256)   %u short lines\n"
        "Queue        %u hotspot events waiting\n"
        "Text engine  cursor row %d col %d   inverse %s\n",
        info,
        c->mode_name ? c->mode_name : "?", c->mode,
        c->booted_game ? "   game booted" : "", c->direct ? "   (opened file)" : "",
        c->link_up ? "up" : "down", c->busy ? "   (transaction in flight)" : "",
        c->mailbox_mode ? "open (a socket and fujimail)" : "closed (a game runs alone)",
        c->live_size, c->live_crc, c->live_st2 ? ".st2" : "raw",
        c->live_blocks, c->live_mailbox ? "   claims the mailbox" : "",
        c->staged ? "yes" : "no", c->staged_claim, c->staged_config, c->armed,
        c->staged_crc,
        c->swaps,
        c->boot_state, c->boot_pct, c->boot_err, c->boot_got, c->boot_total,
        c->ackseq, c->status, c->last_error, c->reply_cmd, c->rxlen,
        c->frames, c->short_lines,
        c->queue_depth,
        c->cursor_row, c->cursor_col, c->inverse ? "on" : "off");
    set_text(w->cart_text, text);
}

/* The page map: what the cartridge serves in each 256-byte page, 16 pages
 * ($1000) to a row, with a legend under it. */
#define PAGE_CELL 16
#define PAGE_LEFT 50
#define PAGE_TOP 18
#define PAGE_MAP_W (PAGE_LEFT + 16 * PAGE_CELL + 2)
#define PAGE_MAP_H (PAGE_TOP + 16 * PAGE_CELL + 30)

static void draw_page_map(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    static const char *const names[4] = { "none", "image", "hotspot", "raster" };
    const guint32 colours[4] = { 0, O2SESSION_ACCENT_RGB, PAGE_HOT_RGB, PAGE_RASTER_RGB };
    GtkWidget *wg = GTK_WIDGET(area);
    int r, c, i;
    double lx;
    (void)width; (void)height;

    for (c = 0; c < 16; c++) {
        char h[4];
        g_snprintf(h, sizeof h, "%X", c);
        set_fg(cr, wg, 0.6);
        draw_centred(cr, wg, h, PAGE_LEFT + c * PAGE_CELL, 0, PAGE_CELL, PAGE_TOP, TRUE);
    }
    for (r = 0; r < 16; r++) {
        char h[8];
        g_snprintf(h, sizeof h, "$%X000", r);
        set_fg(cr, wg, 0.6);
        draw_centred(cr, wg, h, 0, PAGE_TOP + r * PAGE_CELL, PAGE_LEFT - 4, PAGE_CELL, TRUE);
        for (c = 0; c < 16; c++) {
            int t = w->cart.present ? w->cart.page_type[r * 16 + c] : 0;
            double x = PAGE_LEFT + c * PAGE_CELL + 0.5, y = PAGE_TOP + r * PAGE_CELL + 0.5;
            cairo_rectangle(cr, x, y, PAGE_CELL - 1, PAGE_CELL - 1);
            if (t >= 1 && t <= 3) {
                set_rgb(cr, colours[t], 1.0);
                cairo_fill_preserve(cr);
            }
            set_fg(cr, wg, 0.2);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
    }
    /* the legend */
    lx = 0;
    for (i = 0; i < 4; i++) {
        double y = PAGE_TOP + 16 * PAGE_CELL + 10;
        PangoLayout *layout = gtk_widget_create_pango_layout(wg, names[i]);
        int tw, th;
        cairo_rectangle(cr, lx + 0.5, y + 0.5, 11, 11);
        if (i) {
            set_rgb(cr, colours[i], 1.0);
            cairo_fill_preserve(cr);
        }
        set_fg(cr, wg, 0.3);
        cairo_stroke(cr);
        pango_layout_get_pixel_size(layout, &tw, &th);
        set_fg(cr, wg, 0.8);
        cairo_move_to(cr, lx + 16, y + 6 - th / 2.0);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        lx += 16 + tw + 12;
    }
}

/* A text-engine cell as text: printable ASCII as itself, the engine's card
 * and arrow symbols as their Unicode look-alikes, a program-defined glyph
 * ($80-$9F) as a dotted box. */
static const char *cell_text(uint8_t ch, char buf[4])
{
    static const char *const symbols[12] = {
        " ", "\xe2\x99\xa0", "\xe2\x99\xa5", "\xe2\x99\xa6", "\xe2\x99\xa3", "\xe2\x96\x88",
        "\xe2\x96\xb6", "\xe2\x97\x80", "\xe2\x96\xb2", "\xe2\x96\xbc", "\xe2\x92\x91",
        "\xe2\x96\x92",
    };
    if (ch >= 0x20 && ch < 0x7F) {
        buf[0] = (char)ch;
        buf[1] = '\0';
        return buf;
    }
    if (ch < G_N_ELEMENTS(symbols))
        return symbols[ch];
    if (ch >= 0x80 && ch < 0xA0)
        return "\xe2\xac\x9a";   /* a custom glyph */
    return "\xc2\xb7";
}

/* The text engine's 16 x 10 screen as the cartridge drew it: inverse cells
 * filled in the accent, the cursor underlined. */
#define TEXT_CELL_W 14
#define TEXT_CELL_H 22
#define TEXT_GRID_W (16 * TEXT_CELL_W + 2)
#define TEXT_GRID_H (10 * TEXT_CELL_H + 2)

static void draw_text_grid(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *wg = GTK_WIDGET(area);
    int r, c;
    (void)width; (void)height;

    cairo_rectangle(cr, 0.5, 0.5, 16 * TEXT_CELL_W + 1, 10 * TEXT_CELL_H + 1);
    set_fg(cr, wg, 0.3);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (!w->cart.present)
        return;
    for (r = 0; r < 10; r++) {
        for (c = 0; c < 16; c++) {
            double x = 1 + c * TEXT_CELL_W, y = 1 + r * TEXT_CELL_H;
            gboolean inv = w->cart.text_inv[r][c] != 0;
            char buf[4];
            if (inv) {
                cairo_rectangle(cr, x, y, TEXT_CELL_W, TEXT_CELL_H);
                set_rgb(cr, O2SESSION_ACCENT_RGB, 1.0);
                cairo_fill(cr);
                cairo_set_source_rgb(cr, 0, 0, 0);
            } else {
                set_fg(cr, wg, 1.0);
            }
            draw_centred(cr, wg, cell_text(w->cart.text[r][c], buf), x, y,
                         TEXT_CELL_W, TEXT_CELL_H, TRUE);
        }
    }
    if (w->cart.cursor_row >= 0 && w->cart.cursor_row < 10 &&
        w->cart.cursor_col >= 0 && w->cart.cursor_col < 16) {
        double x = 1 + w->cart.cursor_col * TEXT_CELL_W;
        double y = 1 + (w->cart.cursor_row + 1) * TEXT_CELL_H - 3;
        cairo_rectangle(cr, x + 2, y, TEXT_CELL_W - 4, 2);
        set_rgb(cr, accent_line_rgb(), 1.0);
        cairo_fill(cr);
    }
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static o2debug_fuji_packet p[TRACE_MAX];
    static char state[2048];
    char note[200], status[128];
    GString *t;
    int n = o2debug_fujinet_trace(w->dbg, p, TRACE_MAX), i;

    if (o2session_fujinet_external(w->session)) {
        o2session_cart_status(w->session, status, sizeof status);
        g_snprintf(note, sizeof note, "External FujiNet at %s. Cartridge: %s.",
                   o2session_boip_endpoint(w->session), status);
    } else if (!o2session_fujinet_running(w->session)) {
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    } else {
        o2session_cart_status(w->session, status, sizeof status);
        g_snprintf(note, sizeof note, "FujiNet is on. Cartridge: %s.", status);
    }
    gtk_label_set_text(w->fuji_note, note);
    o2debug_fujinet_state(w->dbg, state, sizeof state);
    set_text(w->fuji_state, state);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) g_string_append_printf(t, " \xc3\x97%u", p[i].repeat);
        g_string_append(t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    if (type & O2DEBUG_BP_EXEC) return "Execute";
    if ((type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE)) ==
        (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE))
        return "Read/Write";
    if (type & O2DEBUG_BP_READ) return "Read";
    if (type & O2DEBUG_BP_WRITE) return "Write";
    if ((type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) ==
        (O2DEBUG_BP_IN | O2DEBUG_BP_OUT))
        return "In/Out";
    if (type & O2DEBUG_BP_IN) return "Port In";
    return "Port Out";
}

static void refresh_bps(DbgWin *w)
{
    o2debug_breakpoint bps[128];
    GtkWidget *child;
    int n = o2debug_breakpoint_list(w->dbg, bps, 128), i;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Click a disassembly line, or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[320], range[32];
        /* the 1802's ports are 1-7: N in the instruction, no address */
        const gboolean port = (bps[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
        if (port && bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, "ports %u-%u", bps[i].start, bps[i].end);
        else if (port)
            g_snprintf(range, sizeof range, "port %u", bps[i].start);
        else if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, "$%04X-$%04X", bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, "$%04X", bps[i].start);
        g_snprintf(text, sizeof text, "#%-3d %-10s %-12s hits %-6u %s%s", bps[i].id,
                   bp_type_name(bps[i].type), range, bps[i].hits,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = o2debug_is_stopped(w->dbg) != 0;
    o2debug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped)
        g_snprintf(text, sizeof text, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "o2-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "o2-accent");
}

static void refresh_all(DbgWin *w)
{
    refresh_status(w);
    refresh_cpu(w);
    refresh_ram(w);
    refresh_disasm(w);
    refresh_video(w);
    refresh_io(w);
    refresh_bps(w);
    refresh_cart(w);
    refresh_fujinet(w, FALSE);
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    /* Opened before the session started (O2_OPEN_DEBUGGER), or showing
     * across a session restart: the engine comes up with the machine. */
    if (!o2debug_is_attached(w->dbg) && o2session_is_running(w->session)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = o2debug_generation(w->dbg);
    stopped = o2debug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->seen_generation = gen;
        w->was_stopped = stopped;
        refresh_all(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_status(w);
        refresh_cpu(w);
        refresh_video(w);
        refresh_io(w);
        refresh_cart(w);
        if (gtk_notebook_get_current_page(w->notebook) == TAB_FUJINET) refresh_fujinet(w, FALSE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (o2debug_is_stopped(w->dbg)) o2debug_resume(w->dbg);
    else o2debug_stop(w->dbg);
    refresh_all(w);
}

static void on_step(GtkButton *b, gpointer ud) { (void)b; o2debug_step(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_over(GtkButton *b, gpointer ud) { (void)b; o2debug_step_over(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_step_out(GtkButton *b, gpointer ud) { (void)b; o2debug_step_out(((DbgWin *)ud)->dbg); refresh_all(ud); }
static void on_scanline(GtkButton *b, gpointer ud) { (void)b; o2debug_scanline(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }
static void on_frame(GtkButton *b, gpointer ud) { (void)b; o2debug_frame(((DbgWin *)ud)->dbg, 1); refresh_all(ud); }

static void on_prompt(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->prompt_out, w->prompt_scroll, line);
    o2debug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->prompt_out, w->prompt_scroll, out);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->prompt_out, w->prompt_scroll, "\n");
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_all(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_prompt_key(GtkEventControllerKey *c, guint keyval, guint code,
                              GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->prompt_in));
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    n = o2debug_completions(w->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->prompt_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->prompt_in), -1);
    } else if (n > 1) {
        append_text(w->prompt_out, w->prompt_scroll, comps);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->prompt_out, w->prompt_scroll, "\n");
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        o2debug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    refresh_all(w);
}

static void on_ram_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a, v;
    (void)entry;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->ram_addr)), &a)
        && parse_num(gtk_editable_get_text(GTK_EDITABLE(w->ram_val)), &v))
        o2debug_write(w->dbg, (uint16_t)a, (uint8_t)v);
    refresh_all(w);
}

/* Go to: the RAM row holding an address (or a label), scrolled to and
 * marked. */
static void on_ram_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->ram_view);
    GtkTextMark *mark;
    GtkTextIter it;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a) ||
        a < RAM_BASE || a >= RAM_BASE + RAM_SIZE) {
        w->ram_mark_row = -1;
        refresh_ram(w);
        return;
    }
    w->ram_mark_row = (int)(a - RAM_BASE) / 16;
    refresh_ram(w);
    gtk_text_buffer_get_iter_at_line(b, &it, w->ram_mark_row + 1);
    mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
    gtk_text_view_scroll_to_mark(w->ram_view, mark, 0.0, TRUE, 0.0, 0.3);
    gtk_text_buffer_delete_mark(b, mark);
}

static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it;
    int bx, by, line;
    (void)g; (void)n;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(w->disasm, &it, bx, by);
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count) return;
    o2debug_breakpoint_toggle(w->dbg, w->line_addr[line]);
    refresh_disasm(w);
    refresh_bps(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a)) return;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)(a & 0xFFFF);
    refresh_disasm(w);
}

static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = o2debug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps;
    show_view(ud);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    o2debug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                               gtk_check_button_get_active(b));
    refresh_disasm(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    o2debug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_all(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[6] = { O2DEBUG_BP_EXEC, O2DEBUG_BP_READ,
                                  O2DEBUG_BP_WRITE,
                                  O2DEBUG_BP_READ | O2DEBUG_BP_WRITE,
                                  O2DEBUG_BP_IN, O2DEBUG_BP_OUT };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a, b;
    int id;
    gboolean port;
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    port = (types[sel] & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a)) {
        gtk_label_set_text(w->bp_msg, port ? "Not a port (1-7)" : "Not an address or a label");
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b)) {
        gtk_label_set_text(w->bp_msg, port ? "The end is not a port (1-7)"
                                           : "The end is not an address or a label");
        return;
    }
    if (port && (a < 1 || a > 7 || b < a || b > 7)) {
        gtk_label_set_text(w->bp_msg, "The 1802's ports are 1-7");
        return;
    }
    id = o2debug_breakpoint_add(w->dbg, types[sel], (uint16_t)a, (uint16_t)b,
                                 gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)));
    if (id < 0) {
        gtk_label_set_text(w->bp_msg, "The condition does not parse");
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_all(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    o2debug_breakpoint_clear(w->dbg);
    refresh_all(w);
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512];
    o2debug_load_symbols(w->dbg, path, msg, sizeof msg);
    prompt_say(w, msg);
    refresh_all(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    load_symbols(w, path);
}

static void on_symbols(GtkWidget *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    const char *cart = o2session_cart_path(w->session);
    (void)b;

    gtk_file_filter_set_name(syms, "Symbol files (*.map, *.lst, *.sym, *.txt)");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "lst");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (AS .map / .lst, WLA-DX .sym, AAAA name)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    /* a program's symbols usually sit beside it */
    if (cart && *cart) {
        g_autofree char *dir = g_path_get_dirname(cart);
        g_autoptr(GFile) folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, folder);
    }
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* <cart>.map/.lst/.sym next to the opened cartridge, no picker needed. */
static void on_symbols_beside(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    load_symbols(w, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    o2debug_save(w->dbg, kind, path, msg, sizeof msg);
    prompt_say(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    char title[80];
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    o2debug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    (void)nb; (void)page;
    if (num == TAB_FUJINET) refresh_fujinet(ud, FALSE);
}

static void hide_window(DbgWin *w)
{
    o2debug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7: on_step(NULL, w); return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *sym, *sympop, *beside, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the BIOS and the cartridge's code)", "disassembly.asm" },
        { "ram", "Console RAM ($0800-$09FF)", "ram.bin" },
        { "arena", "Mailbox Arena (2K)", "arena.bin" },
        { "raster", "Cartridge Raster (1K)", "raster.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned i;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Scanline+1", on_scanline);
    TB("Frame+1", on_frame);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Load Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.map, .lst, .sym)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    return bar;
}

static GtkWidget *build_prompt(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->prompt_out, FALSE);
    GtkEventController *keys;

    w->prompt_scroll = GTK_SCROLLED_WINDOW(scroll);
    gtk_text_view_set_wrap_mode(w->prompt_out, GTK_WRAP_WORD_CHAR);
    w->prompt_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->prompt_in,
        "Command (help, step, break, bpw, bpout, print, mem, poke, video, io, cart, text, "
        "disasm, ...) \xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->prompt_in), TRUE);
    g_signal_connect(w->prompt_in, "activate", G_CALLBACK(on_prompt), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_prompt_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->prompt_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->prompt_in));
    set_text(w->prompt_out, "Magnavox Odyssey 2 debugger prompt (CDP1802 and CDP1861). "
                            "Type 'help' for every command.\n");
    return padded(box);
}

static GtkWidget *reg_entry(DbgWin *w, int i)
{
    w->reg[i] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(w->reg[i], 6);
    gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
    gtk_widget_add_css_class(GTK_WIDGET(w->reg[i]), "monospace");
    g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
    g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
    return GTK_WIDGET(w->reg[i]);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    GtkWidget *rgrid = gtk_grid_new();
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *others = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *efs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ram = mono_view(&w->ram_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hint, *title, *eflabel;
    int i;

    /* R0-RF in two columns of eight, as the 1802 numbers them */
    gtk_grid_set_row_spacing(GTK_GRID(rgrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(rgrid), 16);
    for (i = 0; i < 16; i++) {
        GtkWidget *cell = labeled(k_regs[i].name, reg_entry(w, i));
        gtk_widget_set_halign(cell, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(rgrid), cell, i / 8, i % 8, 1, 1);
    }
    /* then R(P) and the rest of the CPU, and the four flag inputs */
    for (i = 16; i < NREGS; i++)
        gtk_box_append(GTK_BOX(others), labeled(k_regs[i].name, reg_entry(w, i)));
    eflabel = gtk_label_new("Flag inputs");
    gtk_widget_add_css_class(eflabel, "dim-label");
    gtk_box_append(GTK_BOX(efs), eflabel);
    for (i = 0; i < 4; i++) {
        char name[8];
        g_snprintf(name, sizeof name, "EF%d", i + 1);
        w->ef[i] = gtk_label_new(name);
        gtk_widget_add_css_class(w->ef[i], "o2-pill");
        gtk_widget_add_css_class(w->ef[i], "dim-label");
        gtk_box_append(GTK_BOX(efs), w->ef[i]);
    }
    hint = gtk_label_new("EF1 is the 1861's EFx (the frame's top and bottom); EF3 and "
                         "EF4 read keypads A and B at the latched key. Enter in a "
                         "register writes it.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_width_chars(GTK_LABEL(hint), 40);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 60);
    gtk_box_append(GTK_BOX(side), others);
    gtk_box_append(GTK_BOX(side), efs);
    gtk_box_append(GTK_BOX(side), hint);
    gtk_box_append(GTK_BOX(top), rgrid);
    gtk_box_append(GTK_BOX(top), side);

    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_label_set_xalign(w->beam, 0);
    gtk_label_set_ellipsize(w->beam, PANGO_ELLIPSIZE_END);

    w->ram_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_goto), 10);
    gtk_entry_set_placeholder_text(w->ram_goto, "$0900 / label");
    g_signal_connect(w->ram_goto, "activate", G_CALLBACK(on_ram_goto), w);
    w->ram_addr = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_addr), 10);
    gtk_entry_set_placeholder_text(w->ram_addr, "$0800");
    w->ram_val = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->ram_val), 4);
    gtk_entry_set_placeholder_text(w->ram_val, "$00");
    g_signal_connect(w->ram_val, "activate", G_CALLBACK(on_ram_write), w);
    hint = gtk_label_new("(any address or label: console RAM takes it; a page the "
                         "cartridge serves takes it in the cartridge's own buffer, "
                         "never as a hotspot)");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    /* a floor under its width, or the window's minimum height is this
     * label wrapped a word to a line */
    gtk_label_set_width_chars(GTK_LABEL(hint), 30);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(w->ram_goto)));
    gtk_box_append(GTK_BOX(edit), labeled("Write address", GTK_WIDGET(w->ram_addr)));
    gtk_box_append(GTK_BOX(edit), labeled("value", GTK_WIDGET(w->ram_val)));
    gtk_box_append(GTK_BOX(edit), hint);

    title = gtk_label_new("Console RAM ($0800-$09FF; the BIOS's display page is $0900-$09FF)");
    gtk_box_append(GTK_BOX(box), top);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->beam));
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), ram);
    gtk_box_append(GTK_BOX(box), edit);
    accent_tag(w->ram_view, "mark");
    w->ram_mark_row = -1;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkWidget *hint;

    accent_tag(w->disasm, "pc");

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0400 or label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 14);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click a line to toggle its breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* No vertical scrollbar (the wheel walks the disassembly instead), and
     * EXTERNAL rather than NEVER: NEVER would make the window's minimum
     * height all 48 lines. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0x0000;
    return padded(box);
}

static GtkWidget *build_video(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *picscroll = gtk_scrolled_window_new();
    GtkWidget *regs = mono_view(&w->video_text, FALSE);
    GtkWidget *hint;
    static const char *const views[] = { "Screen (112 x 262)", "Cartridge raster (64 x 128)",
                                         "BIOS display page (64 x 32)", NULL };

    gtk_widget_set_size_request(left, 420, -1);
    gtk_widget_set_hexpand(left, FALSE);
    gtk_box_append(GTK_BOX(left), regs);

    w->video_view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    g_signal_connect(w->video_view, "notify::selected", G_CALLBACK(on_view_changed), w);
    gtk_box_append(GTK_BOX(row), labeled("View", GTK_WIDGET(w->video_view)));
    hint = gtk_label_new("Screen: the whole raster, blanking dimmed, the visible area "
                         "outlined, the beam crossed");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(hint), 30);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(row), hint);

    w->video_pic = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_can_shrink(w->video_pic, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(w->video_pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(w->video_pic), GTK_ALIGN_START);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(picscroll), GTK_WIDGET(w->video_pic));
    gtk_widget_set_vexpand(picscroll, TRUE);
    gtk_widget_set_hexpand(picscroll, TRUE);

    gtk_box_append(GTK_BOX(right), row);
    gtk_box_append(GTK_BOX(right), picscroll);

    gtk_box_append(GTK_BOX(box), left);
    gtk_box_append(GTK_BOX(box), right);
    w->view_px = g_new0(guint32, O2DEBUG_VIEW_MAX_PIXELS);
    w->view_px2 = g_new0(guint32, (gsize)O2DEBUG_VIEW_MAX_PIXELS * VIEW_SCALE_MAX);
    return padded(box);
}

static GtkWidget *keypad_diagram(DbgWin *w, int pad)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *area = gtk_drawing_area_new();
    char name[16];

    g_snprintf(name, sizeof name, "Keypad %c", 'A' + pad);
    gtk_box_append(GTK_BOX(box), heading(name));
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area), 3 * (KEY_CELL_W + KEY_GAP));
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area), 4 * (KEY_CELL_H + KEY_GAP));
    g_object_set_data(G_OBJECT(area), "dbg", w);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_keypad,
                                   GINT_TO_POINTER(pad), NULL);
    gtk_widget_set_halign(area, GTK_ALIGN_START);
    w->keys_area[pad] = area;
    gtk_box_append(GTK_BOX(box), area);
    return box;
}

/* The text, and beside it what the console sees of both keypads. */
static GtkWidget *build_io(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *pads = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    GtkWidget *hint = gtk_label_new("Held keys are filled; the key the latch selects "
                                    "(OUT 2) is outlined.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_width_chars(GTK_LABEL(hint), 30);
    gtk_label_set_max_width_chars(GTK_LABEL(hint), 36);
    gtk_box_append(GTK_BOX(box), mono_view(&w->io_text, FALSE));
    gtk_box_append(GTK_BOX(pads), keypad_diagram(w, 0));
    gtk_box_append(GTK_BOX(pads), keypad_diagram(w, 1));
    gtk_box_append(GTK_BOX(right), pads);
    gtk_box_append(GTK_BOX(right), hint);
    gtk_box_append(GTK_BOX(box), right);
    w->io_latch = -1;
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write",
                                         "Port In", "Port Out", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$0400 / port 1-7 / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 20);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition (d == $FF && [$0810] > 3) \xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear all");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "o2-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    return padded(box);
}

/* The state as text on the left; the page map and the text engine's screen
 * on the right. */
static GtkWidget *build_cart(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    w->page_map = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->page_map), PAGE_MAP_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->page_map), PAGE_MAP_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->page_map), draw_page_map, w, NULL);
    gtk_widget_set_halign(w->page_map, GTK_ALIGN_START);

    w->text_grid = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->text_grid), TEXT_GRID_W);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->text_grid), TEXT_GRID_H);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->text_grid), draw_text_grid, w, NULL);
    gtk_widget_set_halign(w->text_grid, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(right), heading("Pages the cartridge serves"));
    gtk_box_append(GTK_BOX(right), w->page_map);
    gtk_box_append(GTK_BOX(right), heading("Text engine (16 x 10)"));
    gtk_box_append(GTK_BOX(right), w->text_grid);

    gtk_box_append(GTK_BOX(box), mono_view(&w->cart_text, FALSE));
    gtk_box_append(GTK_BOX(box), right);
    return padded(box);
}

/* The packets left (a note and Clear over them), the cartridge's mailbox right. */
static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");

    w->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->fuji_note), "dim-label");
    gtk_label_set_wrap(w->fuji_note, TRUE);
    gtk_label_set_width_chars(w->fuji_note, 30);
    gtk_label_set_xalign(w->fuji_note, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), heading("FujiBus packets (SLIP-decoded)"));
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text, FALSE));
    make_log(w->fuji_text);

    gtk_box_append(GTK_BOX(right), heading("The cartridge's mailbox"));
    gtk_box_append(GTK_BOX(right), mono_view(&w->fuji_state, FALSE));

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(left, 620, -1);
    gtk_widget_set_size_request(right, 320, -1);
    w->fuji_count = -1;
    return padded(paned);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    if (w->timer) g_source_remove(w->timer);
    o2debug_detach(w->dbg);
    g_free(w->view_px);
    g_free(w->view_px2);
    if (w->history) g_ptr_array_unref(w->history);
    if (g_win == w) g_win = NULL;
    g_free(w);
}

static void attach_and_refresh(DbgWin *w)
{
    o2debug_attach(w->dbg);     /* stops the machine */
    w->seen_generation = o2debug_generation(w->dbg);
    w->was_stopped = o2debug_is_stopped(w->dbg) != 0;
    refresh_all(w);
}

/* O2_DEBUGGER_TAB: a page number, or a tab's name (or the start of one). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("O2_DEBUGGER_TAB");
    char *end;
    long n;
    unsigned i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < (long)G_N_ELEMENTS(k_tab_names)) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    return -1;
}

void o2_debugger_show(GtkWindow *parent, o2session *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gtk_window_present(g_win->win);
        attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = o2session_debugger(session);
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_default_size(w->win, 1150, 820);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_application(w->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_prompt(w), gtk_label_new("Prompt"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cpu(w), gtk_label_new("CPU & RAM"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_disasm(w), gtk_label_new("Disassembly"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_video(w), gtk_label_new("Video"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_io(w), gtk_label_new("Sound & I/O"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_breaks(w), gtk_label_new("Breakpoints"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_cart(w), gtk_label_new("Cart"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_fujinet(w), gtk_label_new("FujiNet"));
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
}

void o2_debugger_toggle(GtkWindow *parent, o2session *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        o2_debugger_show(parent, session);
}

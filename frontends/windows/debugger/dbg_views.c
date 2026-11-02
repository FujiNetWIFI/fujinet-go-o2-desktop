/*
 * The debugger window's hand-drawn views (Win32): the disassembly, the 8244's
 * picture and object tables, the grid, the sound shifter, the EF9340's text
 * page and the keyboard matrix. Each is a small window class that paints
 * what dbg_window.c last read from the engine, double-buffered; none of them
 * reads the engine on its own except to name the object under the pointer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windowsx.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "dbg_internal.h"

#define COMMENT_COL 50       /* where a disassembly line's comment starts, in characters */
#define OBJ_ROWS 39          /* 3 headings, 4 sprites, 12 singles, 4 quads of (1 + 4) */

/* ---- drawing helpers --------------------------------------------------------------- */

/* A hand-drawn view paints into a memory DC and blits once (they refresh
 * live while the machine runs). */
typedef void (*paint_fn)(debugger *d, HWND hwnd, HDC dc, const RECT *c);

static COLORREF xrgb_ref(uint32_t px)
{
    return RGB((px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff);
}

static void fill_rgb(HDC dc, const RECT *r, COLORREF colour)
{
    HBRUSH b = CreateSolidBrush(colour);
    FillRect(dc, r, b);
    DeleteObject(b);
}

/* UTF-8 through TextOutW at a point. */
static void text_at(HDC dc, int x, int y, const char *utf8, int n)
{
    wchar_t w[512];
    char tmp[512];
    if (n < 0 || n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
    snprintf(tmp, sizeof tmp, "%.*s", n, utf8);
    dbg_wide_from(tmp, w, 512);
    TextOutW(dc, x, y, w, (int)wcslen(w));
}

/* UTF-8 into a rectangle through DrawTextW. */
static void text_in(HDC dc, const char *utf8, RECT *r, UINT fmt)
{
    wchar_t w[512];
    dbg_wide_from(utf8, w, 512);
    DrawTextW(dc, w, -1, r, fmt | DT_NOPREFIX);
}

/* A top-down 32-bit DIB onto dc, every pixel scale x scale, nearest. */
static void blit(HDC dc, const image *im, int x, int y, int scale)
{
    BITMAPINFO bmi;
    if (!im->px || im->w <= 0 || im->h <= 0) return;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = im->w;
    bmi.bmiHeader.biHeight = -im->h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x, y, im->w * scale, im->h * scale, 0, 0, im->w, im->h, im->px, &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
}

static void paint_buffered(debugger *d, HWND hwnd, paint_fn fn)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT c;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ oldbmp, oldfont;
    GetClientRect(hwnd, &c);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, c.right > 0 ? c.right : 1, c.bottom > 0 ? c.bottom : 1);
    oldbmp = SelectObject(mem, bmp);
    oldfont = SelectObject(mem, d->mono);
    SetBkMode(mem, TRANSPARENT);
    fn(d, hwnd, mem, &c);
    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldfont);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* The 8 colours of the EF9340's attributes in the Teletext order (red 1,
 * green 2, blue 4); black is drawn grey so it shows on the dark panel. This
 * is the decode view's palette -- the true colours are in the rendered
 * layer beside it. */
static COLORREF ef_colour(int c)
{
    static const COLORREF table[8] = {
        RGB(0x6a, 0x6a, 0x6a), RGB(0xe0, 0x30, 0x30), RGB(0x30, 0xd0, 0x30), RGB(0xe8, 0xe0, 0x30),
        RGB(0x50, 0x60, 0xf0), RGB(0xd0, 0x40, 0xd0), RGB(0x30, 0xd8, 0xd8), RGB(0xf0, 0xf0, 0xf0) };
    return table[c & 7];
}

/* The box of an object in frame pixels, for the highlight. */
static int object_box(const debugger *d, int kind, int idx, RECT *r)
{
    const i8244_snapshot *s = &d->snap;
    int x, y, w, h;
    if (kind == 0 && idx >= 0 && idx < 4) {
        x = s->sprites[idx].x; y = s->sprites[idx].y; w = s->sprites[idx].width; h = s->sprites[idx].height;
    } else if (kind == 1 && idx >= 0 && idx < 12) {
        x = s->singles[idx].x; y = s->singles[idx].y; w = s->singles[idx].width; h = s->singles[idx].height;
    } else if (kind == 2 && idx >= 0 && idx < 16) {
        const i8244_char_info *o = &s->quads[idx / 4].chars[idx % 4];
        x = o->x; y = o->y; w = o->width; h = o->height;
    } else {
        return 0;
    }
    SetRect(r, x, y, x + w, y + h);
    return 1;
}


/* ---- the disassembly ---------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter, address, bytes,
 * label, instruction, and the comment (a branch target's label, a BIOS
 * entry's name) in grey. The PC's line is filled with the accent, white text
 * on it. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int gutter = S(18);
    int i;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    if (d->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_at(dc, gutter, 0, "(no disassembly)", -1);
        return;
    }
    for (i = 0; i < d->line_count; i++) {
        const o2debug_line *l = &d->lines[i];
        const int y = i * d->mono_h;
        RECT row;
        char text[240];
        if (y >= c->bottom) break;
        SetRect(&row, 0, y, c->right, y + d->mono_h);
        if (l->is_pc) FillRect(dc, &row, d->accent);
        SetTextColor(dc, l->is_pc ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT));
        if (l->has_breakpoint) {
            const int size = d->mono_h - S(6) > 4 ? d->mono_h - S(6) : 4;
            HGDIOBJ oldb = SelectObject(dc, l->is_pc ? (HGDIOBJ)GetStockObject(WHITE_BRUSH)
                                                     : (HGDIOBJ)d->accent_dark);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, S(4), y + S(3), S(4) + size + 1, y + S(3) + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        snprintf(text, sizeof text, "%03X  %-6.6s  %-14.14s %s", l->address & 0xfff, l->bytes, l->label,
                 l->disasm);
        text_at(dc, gutter, y, text, -1);
        if (l->comment[0]) {
            snprintf(text, sizeof text, "; %s", l->comment);
            if (!l->is_pc) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_at(dc, gutter + d->mono_w * COMMENT_COL, y, text, -1);
        }
    }
}

/* A click toggles the breakpoint on that line; the wheel and the keys
 * browse (and turn Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_disasm);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd && o2debug_is_attached(d->dbg)) dbg_refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: {
        const int line = d->mono_h > 0 ? GET_Y_LPARAM(lp) / d->mono_h : -1;
        SetFocus(hwnd);
        if (line >= 0 && line < d->line_count) {
            o2debug_breakpoint_toggle(d->dbg, d->lines[line].address);
            dbg_refresh_disasm(d);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageW(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = o2debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        dbg_refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            RECT c;
            int page, rows;
            GetClientRect(hwnd, &c);
            page = d->mono_h > 0 ? c.bottom / d->mono_h / 2 : 1;
            if (page < 1) page = 1;
            rows = wp == VK_PRIOR ? -page : wp == VK_NEXT ? page : wp == VK_UP ? -1 : 1;
            SendMessageW(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = o2debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
            dbg_refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- pictures ------------------------------------------------------------------------ */

/* Where the Video picture sits in its window: the largest whole scale that
 * fits, centred, so the chip's pixels stay whole. */
static void vdc_geom(const debugger *d, const RECT *c, int *x0, int *y0, int *scale)
{
    int s = 1;
    const int w = d->vimg.w, h = d->vimg.h;
    if (w > 0 && h > 0) {
        const int sx = c->right / w, sy = c->bottom / h;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
    }
    *scale = s;
    *x0 = (c->right - w * s) / 2 > 0 ? (c->right - w * s) / 2 : 0;
    *y0 = (c->bottom - h * s) / 2 > 0 ? (c->bottom - h * s) / 2 : 0;
}

/* The picture with a highlight round the object under the pointer: white
 * outside the accent so it shows on anything. */
static void paint_vpic(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int x0, y0, s;
    (void)hwnd;
    FillRect(dc, c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    vdc_geom(d, c, &x0, &y0, &s);
    blit(dc, &d->vimg, x0, y0, s);
    if (d->hover_valid && d->nhits > 0) {
        RECT box;
        if (object_box(d, d->hits[0].kind, d->hits[0].index, &box)) {
            RECT r;
            SetRect(&r, x0 + box.left * s, y0 + box.top * s, x0 + box.right * s, y0 + box.bottom * s);
            InflateRect(&r, 1, 1);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
            InflateRect(&r, -1, -1);
            FrameRect(dc, &r, d->accent);
        }
    }
}

static LRESULT CALLBACK vpic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_vpic);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        RECT c;
        TRACKMOUSEEVENT tme;
        int x0, y0, s, fx, fy;
        GetClientRect(hwnd, &c);
        vdc_geom(d, &c, &x0, &y0, &s);
        fx = GET_X_LPARAM(lp) - x0;
        fy = GET_Y_LPARAM(lp) - y0;
        if (fx >= 0 && fy >= 0 && fx < d->vimg.w * s && fy < d->vimg.h * s) {
            d->hover_valid = 1;
            d->hover_x = fx / s;
            d->hover_y = fy / s;
        } else {
            d->hover_valid = 0;
        }
        dbg_update_hover(d);
        memset(&tme, 0, sizeof tme);
        tme.cbSize = sizeof tme;
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        d->hover_valid = 0;
        dbg_update_hover(d);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A picture the engine drew, as large as fits (a whole scale) when centred,
 * else at the top left at 1:1: the sheets and the EF9340's layer. */
static void paint_pic(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const picview *pv = (const picview *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    int s = 1, x = 0, y = 0;
    FillRect(dc, c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    (void)d;
    if (!pv || pv->img.w <= 0 || pv->img.h <= 0) return;
    if (pv->center) {
        const int sx = c->right / pv->img.w, sy = c->bottom / pv->img.h;
        s = sx < sy ? sx : sy;
        if (s < 1) s = 1;
        x = (c->right - pv->img.w * s) / 2 > 0 ? (c->right - pv->img.w * s) / 2 : 0;
        y = (c->bottom - pv->img.h * s) / 2 > 0 ? (c->bottom - pv->img.h * s) / 2 : 0;
    }
    blit(dc, &pv->img, x, y, s);
}

static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_pic); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the object tables ----------------------------------------------------------------- */

typedef struct { int kind; int idx; } objrow_t;     /* kind: 0 heading, 1 sprite, 2 single, 3 quad, 4 quad char */
static objrow_t g_objrows[OBJ_ROWS];

static void init_objrows(void)
{
    int n = 0, i, q, j;
    g_objrows[n].kind = 0; g_objrows[n++].idx = 0;
    for (i = 0; i < 4; i++) { g_objrows[n].kind = 1; g_objrows[n++].idx = i; }
    g_objrows[n].kind = 0; g_objrows[n++].idx = 1;
    for (i = 0; i < 12; i++) { g_objrows[n].kind = 2; g_objrows[n++].idx = i; }
    g_objrows[n].kind = 0; g_objrows[n++].idx = 2;
    for (q = 0; q < 4; q++) {
        g_objrows[n].kind = 3; g_objrows[n++].idx = q;
        for (j = 0; j < 4; j++) { g_objrows[n].kind = 4; g_objrows[n++].idx = q * 4 + j; }
    }
}

/* The bitmap slot of a row, or -1. */
static int obj_slot(const objrow_t *r)
{
    return r->kind == 1 ? r->idx : r->kind == 2 ? 4 + r->idx : r->kind == 4 ? 16 + r->idx : -1;
}

/* Screen pixels per bitmap pixel: the bitmaps are drawn at true size, so a
 * high-DPI display doubles them whole. */
static int obj_bscale(const debugger *d)
{
    const int s = (d->dpi + 48) / 96;
    return s < 1 ? 1 : s;
}

static int obj_row_height(const debugger *d, const objrow_t *r)
{
    const int slot = obj_slot(r);
    const int bm = slot >= 0 ? d->obj_h[slot] * obj_bscale(d) : 0;
    int text;
    switch (r->kind) {
    case 0: return S(24);
    case 1: text = 3 * d->mono_h; break;
    case 3: text = d->mono_h; break;
    default: text = 2 * d->mono_h; break;
    }
    return (text > bm ? text : bm) + S(10);
}

static int obj_total_height(const debugger *d)
{
    int r, h = 0;
    for (r = 0; r < OBJ_ROWS; r++) h += obj_row_height(d, &g_objrows[r]);
    return h;
}

/* The width of the bitmap column: the widest bitmap there is. */
static int obj_bitmap_column(const debugger *d)
{
    int k, w = 16;
    for (k = 0; k < OBJ_COUNT; k++)
        if (d->obj_w[k] > w) w = d->obj_w[k];
    return w * obj_bscale(d) + S(14);
}

static const char *visibility(int visible, int clipped, int hidden)
{
    return hidden ? "hidden (NTSC Y < 14)" : !visible ? "off screen" : clipped ? "clipped" : "visible";
}

/* The words of a row: up to three lines. Returns how many. */
static int obj_lines(const debugger *d, const objrow_t *r, char lines[3][112])
{
    const i8244_snapshot *s = &d->snap;
    if (r->kind == 0) {
        static const char *const heads[3] = { "Sprites  (the minor system: 8 x 8, zoomable)",
                                              "Single characters  (12)", "Quads  (4 x 4 characters)" };
        snprintf(lines[0], 112, "%s", heads[r->idx]);
        return 1;
    }
    if (r->kind == 1) {
        const i8244_sprite_info *o = &s->sprites[r->idx];
        snprintf(lines[0], 112, "Sprite %d   x=%d y=%d   %s", r->idx, o->x, o->y, i8244_colour_name(o->color));
        snprintf(lines[1], 112, "zoom x%d  S=%d X9=%d   %s", o->zoom ? 2 : 1, o->s, o->x9,
                 visibility(o->visible, o->clipped, 0));
        snprintf(lines[2], 112, "shape %02X %02X %02X %02X %02X %02X %02X %02X", o->shape[0], o->shape[1],
                 o->shape[2], o->shape[3], o->shape[4], o->shape[5], o->shape[6], o->shape[7]);
        return 3;
    }
    if (r->kind == 3) {
        const i8244_quad_info *q = &s->quads[r->idx];
        snprintf(lines[0], 112, "Quad %d   x=%d y=%d   %d rows", r->idx, q->x, q->y, q->rows);
        return 1;
    }
    {
        const i8244_char_info *o = r->kind == 2 ? &s->singles[r->idx] : &s->quads[r->idx / 4].chars[r->idx % 4];
        if (r->kind == 2)
            snprintf(lines[0], 112, "Single %d   x=%d y=%d   %s", r->idx, o->x, o->y, i8244_colour_name(o->color));
        else
            snprintf(lines[0], 112, "  char %d   x=%d y=%d   %s", r->idx % 4, o->x, o->y,
                     i8244_colour_name(o->color));
        snprintf(lines[1], 112, "%sptr $%02X  glyph %d  bit8 %d  %d rows  %s", r->kind == 2 ? "" : "  ",
                 o->reg_ptr, o->glyph, o->bit8, o->rows, visibility(o->visible, o->clipped, o->ntsc_hidden));
        return 2;
    }
}

static void paint_objects(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int col = obj_bitmap_column(d), bs = obj_bscale(d);
    int r, y = -d->obj_scroll;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    for (r = 0; r < OBJ_ROWS; r++) {
        const objrow_t *row = &g_objrows[r];
        const int h = obj_row_height(d, row);
        if (y + h > 0 && y < c->bottom) {
            char lines[3][112];
            const int n = obj_lines(d, row, lines);
            const int slot = obj_slot(row);
            int i;
            if (row->kind == 0) {
                RECT band;
                SetRect(&band, 0, y, c->right, y + h);
                FillRect(dc, &band, GetSysColorBrush(COLOR_BTNFACE));
                SelectObject(dc, d->bold);
                SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
                text_at(dc, S(8), y + (h - d->ui_h) / 2, lines[0], -1);
                SelectObject(dc, d->mono);
            } else {
                RECT line;
                if (slot >= 0 && d->obj_bm[slot] && d->obj_w[slot] > 0) {
                    image im;
                    im.px = d->obj_bm[slot];
                    im.w = d->obj_w[slot];
                    im.h = d->obj_h[slot];
                    im.cap = OBJ_BM_PX;
                    blit(dc, &im, S(8), y + (h - im.h * bs) / 2, bs);
                }
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                for (i = 0; i < n; i++) {
                    if (i > 0) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                    text_at(dc, S(8) + col, y + S(5) + i * d->mono_h, lines[i], -1);
                }
                SetRect(&line, 0, y + h - 1, c->right, y + h);
                FillRect(dc, &line, GetSysColorBrush(COLOR_BTNFACE));
            }
        }
        y += h;
    }
}

/* Keeps the scroll bar in step with the table's height and the window's. */
void dbg_objects_scroll_update(debugger *d)
{
    RECT c;
    SCROLLINFO si;
    int total, max_scroll;
    GetClientRect(d->objects, &c);
    total = obj_total_height(d);
    max_scroll = total - c.bottom > 0 ? total - c.bottom : 0;
    if (d->obj_scroll > max_scroll) d->obj_scroll = max_scroll;
    if (d->obj_scroll < 0) d->obj_scroll = 0;
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total > 0 ? total - 1 : 0;
    si.nPage = (UINT)c.bottom;
    si.nPos = d->obj_scroll;
    SetScrollInfo(d->objects, SB_VERT, &si, TRUE);
}

static LRESULT CALLBACK objects_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_objects);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        dbg_objects_scroll_update(d);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si;
        int pos = d->obj_scroll;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= d->mono_h; break;
        case SB_LINEDOWN: pos += d->mono_h; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = obj_total_height(d); break;
        default: break;
        }
        d->obj_scroll = pos;
        dbg_objects_scroll_update(d);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        d->obj_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * d->mono_h * 3 / WHEEL_DELTA;
        dbg_objects_scroll_update(d);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the grid -------------------------------------------------------------------------- */

/* The 9 x 8 cells of the grid as the chip holds them: a line present is
 * thick in the grid colour, one absent a faint guide. Horizontal lines are
 * 9 columns of 9 positions, vertical lines 10 positions of 8 rows. */
static void paint_grid(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const i8244_snapshot *s = &d->snap;
    const int bright = s->grid_lum || s->p17_lum;
    const COLORREF line_col = xrgb_ref(d->pal[(bright ? 8 : 0) + (s->grid_color & 7)]);
    const COLORREF bg_col = xrgb_ref(d->pal[(s->p17_lum ? 8 : 0) + (s->bg_color & 7)]);
    const int m = S(24), head = 4 * d->mono_h + S(10);
    int cw, ch, gx, gy, col, row, thick;
    char t[256];
    RECT area;
    HBRUSH line_br, guide_br;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    snprintf(t, sizeof t, "Grid %s   dots %s   wide verticals %s", s->grid_en ? "on" : "off",
             s->dots_en ? "on" : "off", s->fill ? "on" : "off");
    text_at(dc, S(8), S(4), t, -1);
    snprintf(t, sizeof t, "Colour %d %s, %s   background %d %s", s->grid_color, i8244_colour_name(s->grid_color),
             bright ? "bright" : "dim", s->bg_color, i8244_colour_name(s->bg_color));
    text_at(dc, S(8), S(4) + d->mono_h, t, -1);
    snprintf(t, sizeof t, "Cells %d x %d pixels from x=%d y=%d  (9 x 8 cells)", s->cell_w, s->cell_h,
             s->grid_x0 * 2, s->grid_y0);
    text_at(dc, S(8), S(4) + 2 * d->mono_h, t, -1);
    if (!s->grid_en) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_at(dc, S(8), S(4) + 3 * d->mono_h, "(drawn as it is held: the grid is switched off)", -1);
    }

    cw = (c->right - 2 * m) / 9;
    if (cw > S(44)) cw = S(44);
    ch = cw * (s->cell_w > 0 ? s->cell_h : 24) / (s->cell_w > 0 ? s->cell_w : 16);
    if (8 * ch > c->bottom - head - m) {
        ch = (c->bottom - head - m) / 8;
        cw = ch * (s->cell_h > 0 ? s->cell_w : 16) / (s->cell_h > 0 ? s->cell_h : 24);
    }
    if (cw < 6 || ch < 6) return;
    gx = (c->right - 9 * cw) / 2;
    gy = head;
    SetRect(&area, gx - S(6), gy - S(6), gx + 9 * cw + S(6), gy + 8 * ch + S(6));
    fill_rgb(dc, &area, bg_col);
    thick = S(3);
    line_br = CreateSolidBrush(line_col);
    guide_br = CreateSolidBrush(RGB(0x80, 0x80, 0x80));
    for (row = 0; row < 9; row++)
        for (col = 0; col < 9; col++) {
            const int on = (s->hgrid[col] >> row) & 1;
            RECT r;
            SetRect(&r, gx + col * cw, gy + row * ch - (on ? thick / 2 : 0), gx + (col + 1) * cw + 1,
                    gy + row * ch + (on ? (thick + 1) / 2 : 1));
            FillRect(dc, &r, on ? line_br : guide_br);
        }
    for (col = 0; col < 10; col++)
        for (row = 0; row < 8; row++) {
            const int on = (s->vgrid[col] >> row) & 1;
            const int wide = on && s->fill ? 2 * thick : thick;
            RECT r;
            SetRect(&r, gx + col * cw - (on ? wide / 2 : 0), gy + row * ch,
                    gx + col * cw + (on ? (wide + 1) / 2 : 1), gy + (row + 1) * ch + 1);
            FillRect(dc, &r, on ? line_br : guide_br);
        }
    if (s->dots_en) {
        for (row = 0; row < 9; row++)
            for (col = 0; col < 10; col++) {
                RECT r;
                SetRect(&r, gx + col * cw - thick, gy + row * ch - thick, gx + col * cw + thick + 1,
                        gy + row * ch + thick + 1);
                FillRect(dc, &r, line_br);
            }
    }
    DeleteObject(line_br);
    DeleteObject(guide_br);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    for (col = 0; col < 9; col++) {
        RECT r;
        char n[4];
        snprintf(n, sizeof n, "%d", col);
        SetRect(&r, gx + col * cw, gy - m + S(2), gx + (col + 1) * cw, gy - S(8));
        text_in(dc, n, &r, DT_CENTER | DT_SINGLELINE);
    }
    for (row = 0; row < 9; row++) {
        RECT r;
        char n[4];
        snprintf(n, sizeof n, "%d", row);
        SetRect(&r, gx - m, gy + row * ch - d->mono_h / 2, gx - S(8), gy + row * ch + d->mono_h / 2);
        text_in(dc, n, &r, DT_RIGHT | DT_SINGLELINE);
    }
}

static LRESULT CALLBACK grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_grid); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the sound shifter ------------------------------------------------------------------ */

/* The 24 bits of the shift register, bit 23 first; a set bit in the accent. */
static void paint_sound(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const i8244_snapshot *s = &d->snap;
    const int m = S(8);
    int cell = (c->right - 2 * m) / 24, i;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    if (cell > S(28)) cell = S(28);
    if (cell < 6) return;
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_at(dc, m, S(2), "Shift register  (bit 23 first)", -1);
    for (i = 0; i < 24; i++) {
        const int bit = 23 - i, on = (int)((s->shift >> bit) & 1);
        RECT r;
        char n[4];
        SetRect(&r, m + i * cell, S(4) + d->mono_h, m + (i + 1) * cell, S(4) + d->mono_h + cell);
        FillRect(dc, &r, on ? d->accent : GetSysColorBrush(COLOR_WINDOW));
        FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
        SetTextColor(dc, on ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT));
        text_in(dc, on ? "1" : "0", &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (bit % 4 == 0) {
            snprintf(n, sizeof n, "%d", bit);
            SetRect(&r, m + i * cell, r.bottom + 1, m + (i + 1) * cell, r.bottom + 1 + d->mono_h);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            text_in(dc, n, &r, DT_CENTER | DT_SINGLELINE);
        }
    }
}

static LRESULT CALLBACK sound_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_sound); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the EF9340's text page --------------------------------------------------------------- */

/* The cell size of the decode grid. */
void dbg_ef_cell(const debugger *d, int *cw, int *ch)
{
    *cw = d->mono_w + S(4);
    *ch = d->mono_h + S(2);
}

/* The 40 x 24 page as the chip holds it, the service row under it: the
 * character in its colour; a graphics cell as a filled block; a delimiter
 * as a bar in its background; inverse swaps the two. */
static void paint_ef_grid(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const ef9340_snapshot_t *e = &d->ef;
    int cw, ch, row, col;
    (void)hwnd;
    dbg_ef_cell(d, &cw, &ch);
    fill_rgb(dc, c, RGB(0x18, 0x18, 0x18));
    for (row = 0; row <= EF9340_ROWS; row++) {
        const int y = row * ch + (row == EF9340_ROWS ? S(6) : 0);
        for (col = 0; col < EF9340_COLUMNS; col++) {
            const ef9340_cell_t *cell = row == EF9340_ROWS ? &e->service[col] : &e->page[row][col];
            RECT r;
            wchar_t glyph[2] = { L' ', 0 };
            COLORREF fg = ef_colour(cell->fg), bg = RGB(0x18, 0x18, 0x18);
            SetRect(&r, col * cw, y, (col + 1) * cw, y + ch);
            switch (cell->kind) {
            case EF9340_CELL_GRAPHIC:
                bg = ef_colour(cell->bg);
                fill_rgb(dc, &r, bg);
                InflateRect(&r, -S(2), -S(2));
                fill_rgb(dc, &r, fg);
                continue;
            case EF9340_CELL_ILLEGAL:
                fill_rgb(dc, &r, fg);
                continue;
            case EF9340_CELL_DELIMITER:
                bg = ef_colour(cell->bg);
                fill_rgb(dc, &r, bg);
                glyph[0] = L'|';
                fg = RGB(0x20, 0x20, 0x20);
                break;
            default:
                if (cell->flags & EF9340_ATTR_INVERTED) { bg = fg; fg = RGB(0x18, 0x18, 0x18); fill_rgb(dc, &r, bg); }
                if (cell->flags & EF9340_ATTR_EXTENDED) glyph[0] = L'#';
                else if (cell->b >= 0x20 && cell->b < 0x7f) glyph[0] = (wchar_t)cell->b;
                else glyph[0] = cell->b ? L'.' : L' ';
                break;
            }
            SetTextColor(dc, fg);
            DrawTextW(dc, glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    if (e->cursor_on && e->cursor_visible && e->x < EF9340_COLUMNS && e->y < EF9340_ROWS) {
        RECT r;
        SetRect(&r, e->x * cw, e->y * ch, (e->x + 1) * cw, (e->y + 1) * ch);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        InflateRect(&r, -1, -1);
        FrameRect(dc, &r, d->accent);
    }
}

static LRESULT CALLBACK ef_grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_ef_grid);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        static const char *const kinds[4] = { "alphanumeric", "graphics", "delimiter", "illegal" };
        int cw, ch, col, row, y = GET_Y_LPARAM(lp);
        const ef9340_cell_t *cell;
        char t[200];
        dbg_ef_cell(d, &cw, &ch);
        col = GET_X_LPARAM(lp) / cw;
        if (y >= EF9340_ROWS * ch + S(6)) y -= S(6);          /* the gap above the service row */
        row = y / ch;
        if (col < 0 || col >= EF9340_COLUMNS || row < 0 || row > EF9340_ROWS) return 0;
        cell = row == EF9340_ROWS ? &d->ef.service[col] : &d->ef.page[row][col];
        snprintf(t, sizeof t, "%s row %d col %d: A=$%02X B=$%02X  %s  fg %d bg %d  glyph %d%s",
                 row == EF9340_ROWS ? "Service" : "Page", row == EF9340_ROWS ? 31 : row, col, cell->a, cell->b,
                 kinds[cell->kind & 3], cell->fg, cell->bg, cell->glyph,
                 (cell->flags & EF9340_ATTR_EXTENDED) ? "  (extended RAM)" : "");
        dbg_label_set(d->ef_status, t);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the keyboard matrix and the sticks ------------------------------------------------------- */

#define MX_LABEL_W 34
#define MX_CELL_W 58
#define MX_CELL_H 26
#define MX_HEAD_H 22

/* The size the matrix view wants. */
void dbg_matrix_size(const debugger *d, int *w, int *h)
{
    *w = S(MX_LABEL_W) + 8 * S(MX_CELL_W) + S(8);
    *h = S(MX_HEAD_H) + 8 * S(MX_CELL_H) + S(16) + 3 * (S(MX_CELL_H) + S(8)) + S(8);
}

/* A held key or stick contact in the accent, white text on it. */
static void matrix_cell(debugger *d, HDC dc, RECT r, const char *text, int held, int scanned)
{
    FillRect(dc, &r, held ? d->accent : GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
    if (scanned) {
        InflateRect(&r, -1, -1);
        FrameRect(dc, &r, d->accent_dark);
        InflateRect(&r, 1, 1);
    }
    SetTextColor(dc, held ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT));
    InflateRect(&r, -2, 0);
    text_in(dc, text, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* The 8 x 8 matrix as the console scans it (the row P20-P22 selects is
 * outlined), then both sticks and RESET. */
static void paint_matrix(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    static const char *const dirs[5] = { "UP", "RIGHT", "DOWN", "LEFT", "FIRE" };
    const o2debug_io *io = &d->io;
    int row, col, s, i, y;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    SelectObject(dc, d->ui);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    for (col = 0; col < 8; col++) {
        RECT r;
        char n[8];
        snprintf(n, sizeof n, "col %d", col);
        SetRect(&r, S(MX_LABEL_W) + col * S(MX_CELL_W), 0, S(MX_LABEL_W) + (col + 1) * S(MX_CELL_W), S(MX_HEAD_H));
        text_in(dc, n, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (row = 0; row < 8; row++) {
        RECT r;
        char n[8];
        const int top = S(MX_HEAD_H) + row * S(MX_CELL_H);
        snprintf(n, sizeof n, "row %d", row);
        SetRect(&r, 0, top, S(MX_LABEL_W) - S(4), top + S(MX_CELL_H));
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_in(dc, n, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        for (col = 0; col < 8; col++) {
            const int key = row * 8 + col;
            const char *label = o2session_key_label(io->machine, key);
            const int held = (int)((io->keys >> key) & 1);
            SetRect(&r, S(MX_LABEL_W) + col * S(MX_CELL_W), top, S(MX_LABEL_W) + (col + 1) * S(MX_CELL_W),
                    top + S(MX_CELL_H));
            matrix_cell(d, dc, r, label ? label : "", held, io->kbd_enable && row == io->kbd_row);
        }
    }
    y = S(MX_HEAD_H) + 8 * S(MX_CELL_H) + S(16);
    for (s = 0; s < 2; s++) {
        RECT r;
        char n[16];
        SetRect(&r, 0, y, S(MX_LABEL_W) + S(8), y + S(MX_CELL_H));
        snprintf(n, sizeof n, "Stick %d", s + 1);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        r.right = S(MX_LABEL_W) + S(24);
        text_in(dc, n, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (i = 0; i < 5; i++) {
            const int w = S(52);
            SetRect(&r, S(70) + i * (w + S(5)), y, S(70) + i * (w + S(5)) + w, y + S(MX_CELL_H));
            matrix_cell(d, dc, r, dirs[i], (io->sticks[s] >> i) & 1, 0);
        }
        y += S(MX_CELL_H) + S(8);
    }
    {
        RECT r;
        SetRect(&r, 0, y, S(70), y + S(MX_CELL_H));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_in(dc, "Console", &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetRect(&r, S(70), y, S(70) + S(MX_CELL_W) + S(10), y + S(MX_CELL_H));
        matrix_cell(d, dc, r, "RESET", io->reset_held, 0);
    }
}

static LRESULT CALLBACK matrix_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_debugger;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_matrix); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the classes ----------------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const wchar_t *name, WNDPROC proc)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

/* Registers the window classes of the views, once. */
void dbg_views_register(HINSTANCE inst)
{
    register_class(inst, L"O2DbgVdc", vpic_proc);
    register_class(inst, L"O2DbgPic", pic_proc);
    register_class(inst, L"O2DbgDisasm", disasm_proc);
    register_class(inst, L"O2DbgObjects", objects_proc);
    register_class(inst, L"O2DbgGrid", grid_proc);
    register_class(inst, L"O2DbgSound", sound_proc);
    register_class(inst, L"O2DbgEfGrid", ef_grid_proc);
    register_class(inst, L"O2DbgMatrix", matrix_proc);
    init_objrows();
}

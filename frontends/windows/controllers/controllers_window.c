/*
 * The Win32 Controllers window: the Odyssey 2's membrane keyboard drawn from
 * the session's layout table (so every frontend shows the same keys: the
 * Videopac+ G7400's bigger one when that machine is selected), both
 * joysticks drawn like the console's, the gamepads and which stick each one
 * drives, and Map mode. The GNOME window's behaviour, in GDI.
 *
 * Keys and stick controls are driven by WM_LBUTTONDOWN/WM_LBUTTONUP rather
 * than by BN_CLICKED: the console samples its keyboard and sticks once per
 * frame, so a press present only for the instant of a click falls between
 * frames and a polling program never sees it. A key is HELD while the mouse
 * button is down, like the real one. The mouse is captured on press; letting
 * go anywhere, dragging off the cap or losing the capture releases it, so the
 * console is never left holding one.
 *
 * What the console sees held -- host keyboard, gamepads and this window
 * together -- is filled with the accent, so the window doubles as an input
 * tester for a new gamepad.
 *
 * FIXED SIZE: laid out for the Odyssey 2's keyboard and three gamepad rows
 * whatever is fitted, so neither the machine, Map mode nor a gamepad resizes
 * it (the G7400's wider keyboard is drawn smaller inside the same plate).
 *
 * MAP MODE: Map arms a two-step rebind -- click a key cap, a stick control or
 * a session action to pick the target, then press a host key OR a gamepad
 * button (Esc cancels). While armed the caps show what drives them, typing
 * does not reach the console, and rebinding steals the key from whatever held
 * it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help/help_window.h"
#include "../key_forward.h"
#include "../win_common.h"

#define PANEL_CLASS   "FujiNetGoO2Controllers"
#define PANEL_STYLE   (WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN)
#define PANEL_EXSTYLE WS_EX_TOOLWINDOW

/* Sizes at 96 dpi; S() scales them to the window's. */
#define MARGIN      12
#define SILVER_PAD  10      /* the console's silver around the plate */
#define PLATE_PAD   14
#define PLATE_W     500
#define KBD_W       (PLATE_W - 2 * PLATE_PAD)
#define KBD_UNIT_O2 43      /* key units to pixels: the Odyssey 2's 10.9-wide board */
#define KBD_ROWS_U  6.5f    /* the Odyssey 2's keyboard height, in units */
#define STICK_W     200
#define STICK_H     150
#define STICK_GAP   24
#define TITLE_H     20
#define HEAD_H      22
#define ROW_H       26      /* one gamepad's row */
#define PAD_ROWS     3      /* gamepad rows the window has room for */
#define SEG_W       64      /* Automatic / Stick 1 / Stick 2 */
#define BTN_H       28
#define MAP_W       64
#define DEFAULTS_W  80
#define ACT_W      118
#define HELP_W      64

#define IDT_CAPTURE 1
#define IDT_HELD    2
#define IDT_PADS    3

#define MAX_CONTROLS 128

enum { CT_KEY, CT_DIR, CT_FIRE, CT_ACTION };

typedef struct {
    RECT rc;
    int kind;
    int target;             /* what Map rebinds, or -1 */
    int key;                /* CT_KEY: matrix position, -1 for RESET */
    int port, act;          /* CT_DIR / CT_FIRE */
    int cap_kind;           /* CT_KEY: o2_keycap_kind */
    const char *label;      /* static strings */
    int lit;
} control;

typedef struct {
    unsigned generation;
    int count, assign[PAD_ROWS], effective[PAD_ROWS];
} pads_state;

static HWND g_panel, g_parent;
static o2session *g_session;
static int g_dpi = 96;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static WPARAM g_swallow_vk;     /* the key Map took: its repeats and release too */
static char g_hint[300];

static control g_ctl[MAX_CONTROLS];
static int g_nctl;
static int g_mouse_ctl = -1;    /* the control the captured mouse holds */
static int g_built_machine = -1;
static int g_unit;              /* the keyboard's key unit, in 96-dpi pixels */

static pads_state g_pads_shown;

static int g_total_w, g_total_h;
static RECT g_silver_rc, g_plate_rc, g_stick_rc[2], g_stick_title_rc[2];
static RECT g_gp_rc, g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static RECT g_map_rc, g_defaults_rc, g_help_rc, g_hint_rc;

static HBRUSH g_accent_brush;
static HFONT g_font, g_bold, g_small, g_cap, g_cap_in, g_cap_tiny, g_sym;

static void refresh_held(void);

static int S(int v) { return o2_scale(v, g_dpi); }

/* ---- fonts ------------------------------------------------------------------ */

static void free_fonts(void)
{
    HFONT *f[] = { &g_font, &g_bold, &g_small, &g_cap, &g_cap_in, &g_cap_tiny, &g_sym };
    size_t i;
    for (i = 0; i < sizeof f / sizeof f[0]; i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static int font_has(HDC dc, HFONT f, const wchar_t *w, int n)
{
    WORD idx[32];
    HGDIOBJ old;
    int i, ok = 1;
    if (!f || n <= 0 || n > 32) return 0;
    old = SelectObject(dc, f);
    if (GetGlyphIndicesW(dc, w, n, idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) ok = 0;
    for (i = 0; ok && i < n; i++)
        if (idx[i] == 0xffff) ok = 0;
    SelectObject(dc, old);
    return ok;
}

/* The UI fonts at this DPI, and the keys' legends sized by the key unit. The
 * G7400's arrows and the x and divide signs may not be in the UI font: the
 * first symbol face that has them draws those. */
static void make_fonts(void)
{
    static const char *const symbol_faces[] = { "Segoe UI Symbol", "DejaVu Sans", "Arial Unicode MS" };
    HDC dc;
    size_t i;

    free_fonts();
    g_font = o2_make_font(g_dpi, 0, FW_NORMAL, NULL);
    g_bold = o2_make_font(g_dpi, 0, FW_BOLD, NULL);
    g_small = o2_make_font(g_dpi, 10, FW_NORMAL, NULL);
    g_cap = o2_make_font(g_dpi, g_unit * 36 / 100, FW_BOLD, NULL);
    g_cap_in = o2_make_font(g_dpi, g_unit * 21 / 100, FW_BOLD, NULL);
    g_cap_tiny = o2_make_font(g_dpi, 9, FW_NORMAL, NULL);
    dc = GetDC(NULL);
    for (i = 0; i < sizeof symbol_faces / sizeof symbol_faces[0] && !g_sym; i++) {
        HFONT f = o2_make_font(g_dpi, g_unit * 36 / 100, FW_BOLD, symbol_faces[i]);
        if (font_has(dc, f, L"\x00d7\x00f7\x2191\x2190", 4)) g_sym = f;
        else if (f) DeleteObject(f);
    }
    ReleaseDC(NULL, dc);
}

/* ---- text -------------------------------------------------------------------- */

/* UTF-8 (key legends, gamepad names) through DrawTextW. */
static int text_out(HDC dc, const char *utf8, RECT *rc, UINT fmt)
{
    wchar_t w[256];
    if (!utf8 || !*utf8) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0])) <= 0) return 0;
    return DrawTextW(dc, w, -1, rc, fmt | DT_NOPREFIX);
}

static void text_line(HDC dc, const char *utf8, const RECT *rc, UINT fmt)
{
    RECT r = *rc;
    text_out(dc, utf8, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* Centred both ways, wrapped (a Map legend is a key and a pad button). */
static void text_block(HDC dc, const char *utf8, const RECT *rc)
{
    RECT m = *rc;
    int h;
    InflateRect(&m, -S(2), 0);
    h = text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    m.left = rc->left + S(2);
    m.right = rc->right - S(2);
    m.top = rc->top + ((rc->bottom - rc->top) - h) / 2;
    if (m.top < rc->top) m.top = rc->top;
    m.bottom = rc->bottom;
    text_out(dc, utf8, &m, DT_CENTER | DT_WORDBREAK);
}

/* ---- pressing ----------------------------------------------------------------- */

static int control_target(const control *c)
{
    if (c->kind == CT_KEY)
        return c->key >= 0 ? O2_TARGET_KEY(c->key) : O2_TARGET_SWITCH(O2_SW_RESET);
    return c->target;
}

/* Lets go of what the mouse holds in this window (only that: the main
 * window releases its own keys). A session action fires on release inside
 * its button, like any button. */
static void mouse_release(int fire_action)
{
    const int i = g_mouse_ctl;
    control *c;
    if (i < 0) return;
    g_mouse_ctl = -1;
    if (i >= g_nctl) return;
    c = &g_ctl[i];
    if (c->kind == CT_ACTION) {
        if (fire_action) o2session_sysaction_post(g_session, c->act);
    } else {
        o2session_press(g_session, control_target(c), 0);
    }
    if (g_panel) InvalidateRect(g_panel, &c->rc, FALSE);
}

static void control_press(int i)
{
    control *c = &g_ctl[i];
    mouse_release(0);
    g_mouse_ctl = i;
    if (c->kind != CT_ACTION)
        o2session_press(g_session, control_target(c), 1);
    InvalidateRect(g_panel, &c->rc, FALSE);
}

static void set_hint(const char *text)
{
    snprintf(g_hint, sizeof g_hint, "%s", text);
    if (g_panel) InvalidateRect(g_panel, &g_hint_rc, FALSE);
}

static void set_map_state(int state)
{
    int i;
    g_map_state = state;
    if (state < 0) {
        if (g_panel) KillTimer(g_panel, IDT_CAPTURE);
        o2session_gamepad_capture_cancel(g_session);
    }
    if (state == -2) {
        set_hint("Click and hold to press. Map rebinds a key, a stick control or a gamepad button.");
    } else if (state == -1) {
        set_hint("Click a key or a stick control to remap. Esc finishes.");
    } else {
        char msg[200];
        mouse_release(0);
        snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s. Esc cancels.", o2_target_name(state));
        set_hint(msg);
        o2session_gamepad_capture_begin(g_session);
        if (g_panel) SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    for (i = 0; i < g_nctl; i++)
        g_ctl[i].lit = 0;           /* refresh_held relights them when idle */
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* After a rebind: what it is now, and whom it was taken from. */
static void bound_hint(const char *what, const char *stolen)
{
    char msg[260];
    if (stolen[0]) snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else snprintf(msg, sizeof msg, "Bound %s", what);
    set_hint(msg);
}

/* ---- layout --------------------------------------------------------------------- */

static int machine_now(void)
{
    const int m = o2session_get_int(g_session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

/* The silver console with its black plate, the two sticks, the gamepad rows
 * and the Map row: the window is this size for every machine. */
static void layout_static(void)
{
    const int left = S(MARGIN);
    const int plate_h = (int)((KBD_ROWS_U * KBD_UNIT_O2) * (float)g_dpi / 96.0f + 0.5f) + 2 * S(PLATE_PAD);
    const int content_w = S(PLATE_W) + 2 * S(SILVER_PAD);
    const int sticks_w = 2 * S(STICK_W) + S(STICK_GAP);
    int y = S(MARGIN), i;

    g_total_w = 2 * left + content_w;
    SetRect(&g_silver_rc, left, y, left + content_w, y + plate_h + 2 * S(SILVER_PAD));
    SetRect(&g_plate_rc, g_silver_rc.left + S(SILVER_PAD), g_silver_rc.top + S(SILVER_PAD),
            g_silver_rc.right - S(SILVER_PAD), g_silver_rc.bottom - S(SILVER_PAD));
    y = g_silver_rc.bottom + S(12);

    for (i = 0; i < 2; i++) {
        const int x = left + (content_w - sticks_w) / 2 + i * (S(STICK_W) + S(STICK_GAP));
        SetRect(&g_stick_title_rc[i], x, y, x + S(STICK_W), y + S(TITLE_H));
        SetRect(&g_stick_rc[i], x, y + S(TITLE_H), x + S(STICK_W), y + S(TITLE_H) + S(STICK_H));
    }
    y = g_stick_rc[0].bottom + S(12);

    SetRect(&g_gp_head_rc, left, y, left + content_w, y + S(HEAD_H));
    y += S(HEAD_H);
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_gp_row_rc[i], left, y, left + content_w, y + S(ROW_H));
        y += S(ROW_H) + S(2);
    }
    SetRect(&g_gp_rc, left, g_gp_head_rc.top, left + content_w, y);
    y += S(8);

    SetRect(&g_map_rc, left, y, left + S(MAP_W), y + S(BTN_H));
    SetRect(&g_defaults_rc, g_map_rc.right + S(6), y, g_map_rc.right + S(6) + S(DEFAULTS_W), y + S(BTN_H));
    SetRect(&g_help_rc, left + content_w - S(HELP_W), y, left + content_w, y + S(BTN_H));
    y += S(BTN_H) + S(6);
    SetRect(&g_hint_rc, left, y, left + content_w, y + S(20));
    g_total_h = y + S(20) + S(MARGIN);
}

/* The Automatic / Stick 1 / Stick 2 choice at the right of a gamepad row. */
static void segment_rect(int row, int seg, RECT *rc)
{
    const RECT *r = &g_gp_row_rc[row];
    const int x = r->right - (3 - seg) * (S(SEG_W) + S(4)) + S(4);
    SetRect(rc, x, r->top + 1, x + S(SEG_W), r->bottom - 1);
}

static void gp_name_rect(int row, RECT *rc)
{
    RECT seg0;
    segment_rect(row, 0, &seg0);
    *rc = g_gp_row_rc[row];
    rc->right = seg0.left - S(110);
}

static control *add_control(int kind)
{
    control *c;
    if (g_nctl >= MAX_CONTROLS) return NULL;
    c = &g_ctl[g_nctl++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->key = -1;
    c->target = -1;
    c->port = -1;
    c->act = -1;
    return c;
}

static int px(float units)
{
    return (int)(units * (float)S(g_unit) + 0.5f);
}

static void build_keyboard(void)
{
    int n = 0, i, x0, y0;
    float wu = 0, hu = 0;
    const o2_keycap *caps = o2session_keyboard_layout(machine_now(), &n, &wu, &hu);

    /* the O2's board at 43 px a unit; a wider board is drawn smaller so it
     * still fits the plate */
    g_unit = (int)((float)KBD_W / wu);
    if (g_unit > KBD_UNIT_O2) g_unit = KBD_UNIT_O2;
    x0 = g_plate_rc.left + ((g_plate_rc.right - g_plate_rc.left) - px(wu)) / 2;
    y0 = g_plate_rc.top + ((g_plate_rc.bottom - g_plate_rc.top) - px(hu)) / 2;
    for (i = 0; i < n; i++) {
        control *c = add_control(CT_KEY);
        if (!c) break;
        c->rc.left = x0 + px(caps[i].x);
        c->rc.top = y0 + px(caps[i].y);
        c->rc.right = x0 + px(caps[i].x + caps[i].w);
        c->rc.bottom = y0 + px(caps[i].y + caps[i].h);
        c->key = caps[i].target;
        c->cap_kind = caps[i].kind;
        c->label = caps[i].label ? caps[i].label : "";
        c->target = control_target(c);
    }
}

/* Where a stick's controls sit inside its base: the fire button at the back
 * left corner as on the console, the four direction pads round the stick. */
static void stick_geometry(int port, RECT *fire, RECT dir[4], POINT *knob)
{
    const RECT *b = &g_stick_rc[port];
    const int cx = b->left + S(124), cy = b->top + S(80);
    SetRect(fire, b->left + S(10), b->top + S(10), b->left + S(70), b->top + S(48));
    SetRect(&dir[O2_ACT_UP],    cx - S(21), cy - S(64), cx + S(21), cy - S(30));
    SetRect(&dir[O2_ACT_RIGHT], cx + S(30), cy - S(21), cx + S(66), cy + S(21));
    SetRect(&dir[O2_ACT_DOWN],  cx - S(21), cy + S(30), cx + S(21), cy + S(64));
    SetRect(&dir[O2_ACT_LEFT],  cx - S(66), cy - S(21), cx - S(30), cy + S(21));
    knob->x = cx;
    knob->y = cy;
}

static void build_sticks(void)
{
    int port, act;
    for (port = 0; port < 2; port++) {
        RECT fire, dir[4];
        POINT knob;
        control *c;
        stick_geometry(port, &fire, dir, &knob);
        for (act = 0; act < 4; act++) {
            c = add_control(CT_DIR);
            if (!c) return;
            c->rc = dir[act];
            c->port = port;
            c->act = act;
            c->target = O2_TARGET_PORT(port, act);
        }
        c = add_control(CT_FIRE);
        if (!c) return;
        c->rc = fire;
        c->port = port;
        c->act = O2_ACT_FIRE;
        c->target = O2_TARGET_PORT(port, O2_ACT_FIRE);
        c->label = "FIRE";
    }
}

/* The two session actions beside Map and Defaults: pressed like any button,
 * and rebindable like any key. */
static void build_actions(void)
{
    static const struct { int act; const char *label; } acts[] = {
        { O2_SYSACT_RESET_CONFIG, "Reset to CONFIG" }, { O2_SYSACT_DEBUG_STOP, "Debugger Stop" },
    };
    int i, x = g_defaults_rc.right + S(14);
    for (i = 0; i < 2; i++) {
        control *c = add_control(CT_ACTION);
        if (!c) return;
        SetRect(&c->rc, x, g_map_rc.top, x + S(ACT_W), g_map_rc.bottom);
        c->act = acts[i].act;
        c->target = O2_TARGET_SYSACT(acts[i].act);
        c->label = acts[i].label;
        x += S(ACT_W) + S(6);
    }
}

static void rebuild(void)
{
    mouse_release(0);
    if (g_panel && GetCapture() == g_panel) ReleaseCapture();
    g_nctl = 0;
    g_built_machine = machine_now();
    build_keyboard();
    make_fonts();               /* the caps' size follows the unit */
    build_sticks();
    build_actions();
    if (g_panel) {
        InvalidateRect(g_panel, NULL, FALSE);
        refresh_held();
    }
}

/* ---- painting ----------------------------------------------------------------------- */

static void draw_face(HDC dc, const RECT *rc, int pushed, int accent, int inactive)
{
    RECT r = *rc;
    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(255, 255, 255));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0)
                                             | (inactive ? DFCS_INACTIVE : 0));
        SetTextColor(dc, GetSysColor(inactive ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    }
}

static COLORREF rgb_of(unsigned v)
{
    return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

static void round_rect(HDC dc, const RECT *r, int radius, COLORREF fill, COLORREF line)
{
    HGDIOBJ oldb = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldp = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, line);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(dc, oldp);
    SelectObject(dc, oldb);
}

static void disc(HDC dc, int cx, int cy, int rad, COLORREF fill, COLORREF line)
{
    HGDIOBJ oldb = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldp = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, line);
    Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
    SelectObject(dc, oldp);
    SelectObject(dc, oldb);
}

/* In Map mode a target shows what drives it: the key, then the pad button;
 * whole = the full "Name: key, button" line. */
static void binding_label(int target, char *out, int outsz)
{
    const o2_binding bind = o2session_binding_get(g_session, target);
    char key[32];
    key[0] = '\0';
    o2session_keysym_name(bind.keysym, key, sizeof key);
    if (bind.button != O2_PAD_BTN_NONE)
        snprintf(out, (size_t)outsz, "%s\n%s", key[0] ? key : "\xe2\x80\x94", o2_pad_button_name(bind.button));
    else
        snprintf(out, (size_t)outsz, "%s", key[0] ? key : "\xe2\x80\x94");
}

/* A key cap: a cream face over a shadow edge on the black plate; the function
 * row's legends are red, RESET is a red cap, SPACE a plain bar. */
static void paint_key(HDC dc, const control *c, int mapping)
{
    const int armed = mapping && g_map_state >= 0 && c->target == g_map_state;
    const int accent = mapping ? armed : c->lit;
    const int radius = S(g_unit) / 4 + 2;
    RECT face = c->rc, shadow = c->rc, t;
    COLORREF fill = RGB(0xEC, 0xE9, 0xE0), text = RGB(0x1e, 0x1e, 0x1e);

    if (c->cap_kind == O2_KEYCAP_RESET) { fill = rgb_of(O2SESSION_ACCENT_DARK_RGB); text = RGB(255, 255, 255); }
    if (c->cap_kind == O2_KEYCAP_FUNCTION) text = rgb_of(O2SESSION_ACCENT_DARK_RGB);
    if (accent) { fill = rgb_of(O2SESSION_ACCENT_RGB); text = RGB(255, 255, 255); }
    OffsetRect(&shadow, 0, S(2));
    face.bottom -= S(1);
    round_rect(dc, &shadow, radius, RGB(0x6a, 0x66, 0x60), RGB(0x6a, 0x66, 0x60));
    round_rect(dc, &face, radius, fill, fill);
    SetTextColor(dc, text);

    if (mapping && c->target >= 0) {
        char legend[96];
        binding_label(c->target, legend, sizeof legend);
        SelectObject(dc, g_cap_tiny);
        text_block(dc, legend, &face);
        return;
    }
    switch (c->cap_kind) {
    case O2_KEYCAP_INPUT: SelectObject(dc, g_cap_in); break;
    case O2_KEYCAP_RESET:
    case O2_KEYCAP_SPACE: SelectObject(dc, g_cap_in); break;
    default: SelectObject(dc, g_cap); break;
    }
    if (c->cap_kind == O2_KEYCAP_SPACE && !accent) SetTextColor(dc, RGB(0x70, 0x6c, 0x64));
    if (g_sym && (unsigned char)c->label[0] >= 0x80) {
        wchar_t w[16];
        const int n = MultiByteToWideChar(CP_UTF8, 0, c->label, -1, w, 16) - 1;
        if (n > 0 && !font_has(dc, g_cap, w, n)) SelectObject(dc, g_sym);
    }
    SetRect(&t, face.left + 1, face.top, face.right - 1, face.bottom);
    text_line(dc, c->label, &t, DT_CENTER | DT_END_ELLIPSIS);
}

/* A direction pad: a dark wedge with a triangle, the accent while held. */
static void draw_arrow(HDC dc, const RECT *r, int act, COLORREF col)
{
    const int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    const int a = S(8), b = S(6);
    POINT pt[3];
    HGDIOBJ oldb, oldp;
    switch (act) {
    case O2_ACT_UP:
        pt[0].x = cx; pt[0].y = cy - b; pt[1].x = cx + a; pt[1].y = cy + b; pt[2].x = cx - a; pt[2].y = cy + b;
        break;
    case O2_ACT_DOWN:
        pt[0].x = cx; pt[0].y = cy + b; pt[1].x = cx - a; pt[1].y = cy - b; pt[2].x = cx + a; pt[2].y = cy - b;
        break;
    case O2_ACT_LEFT:
        pt[0].x = cx - b; pt[0].y = cy; pt[1].x = cx + b; pt[1].y = cy - a; pt[2].x = cx + b; pt[2].y = cy + a;
        break;
    default:
        pt[0].x = cx + b; pt[0].y = cy; pt[1].x = cx - b; pt[1].y = cy + a; pt[2].x = cx - b; pt[2].y = cy - a;
        break;
    }
    oldb = SelectObject(dc, GetStockObject(DC_BRUSH));
    oldp = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, col);
    SetDCPenColor(dc, col);
    Polygon(dc, pt, 3);
    SelectObject(dc, oldp);
    SelectObject(dc, oldb);
}

static void paint_dir(HDC dc, const control *c, int mapping)
{
    const int armed = mapping && g_map_state >= 0 && c->target == g_map_state;
    const int accent = mapping ? armed : c->lit;
    const COLORREF fill = accent ? rgb_of(O2SESSION_ACCENT_RGB) : RGB(0x3c, 0x3c, 0x3c);
    round_rect(dc, &c->rc, S(8), fill, RGB(0x5a, 0x5a, 0x5a));
    if (mapping) {
        char legend[96];
        binding_label(c->target, legend, sizeof legend);
        SetTextColor(dc, RGB(255, 255, 255));
        SelectObject(dc, g_cap_tiny);
        text_block(dc, legend, &c->rc);
    } else {
        draw_arrow(dc, &c->rc, c->act, accent ? RGB(255, 255, 255) : RGB(0xd0, 0xd0, 0xd0));
    }
}

static void paint_fire(HDC dc, const control *c, int mapping)
{
    const int armed = mapping && g_map_state >= 0 && c->target == g_map_state;
    const int lit = mapping ? armed : c->lit;
    const COLORREF fill = lit ? RGB(0xff, 0x52, 0x4c) : rgb_of(O2SESSION_ACCENT_RGB);
    RECT t = c->rc;
    round_rect(dc, &c->rc, S(6), fill, RGB(0x6a, 0x0c, 0x10));
    SetTextColor(dc, RGB(255, 255, 255));
    if (mapping) {
        char legend[96];
        binding_label(c->target, legend, sizeof legend);
        SelectObject(dc, g_cap_tiny);
        text_block(dc, legend, &c->rc);
    } else {
        SelectObject(dc, g_small);
        text_line(dc, c->label, &t, DT_CENTER);
    }
}

/* The stick itself, in its black base: the knob leans the way it is held. */
static void paint_stick(HDC dc, int port)
{
    RECT fire, dir[4];
    POINT knob;
    int dx, dy;
    const unsigned held = o2session_buttons_held(g_session, port);
    RECT title = g_stick_title_rc[port];
    char text[24];

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    snprintf(text, sizeof text, "Stick %d", port + 1);
    text_line(dc, text, &title, DT_LEFT);

    round_rect(dc, &g_stick_rc[port], S(14), RGB(0x22, 0x22, 0x22), RGB(0x0a, 0x0a, 0x0a));
    stick_geometry(port, &fire, dir, &knob);
    dx = (int)((held >> O2_ACT_RIGHT) & 1) - (int)((held >> O2_ACT_LEFT) & 1);
    dy = (int)((held >> O2_ACT_DOWN) & 1) - (int)((held >> O2_ACT_UP) & 1);
    /* the ring the stick moves in, then the stick */
    disc(dc, knob.x, knob.y, S(25), RGB(0x16, 0x16, 0x16), RGB(0x4a, 0x4a, 0x4a));
    disc(dc, knob.x + dx * S(5), knob.y + dy * S(5), S(15), RGB(0x30, 0x30, 0x30), RGB(0x66, 0x66, 0x66));
    disc(dc, knob.x + dx * S(5) - S(4), knob.y + dy * S(5) - S(4), S(4), RGB(0x6a, 0x6a, 0x6a), RGB(0x6a, 0x6a, 0x6a));
}

static void paint_control(HDC dc, const control *c)
{
    const int mapping = g_map_state != -2;
    switch (c->kind) {
    case CT_KEY: paint_key(dc, c, mapping); break;
    case CT_DIR: paint_dir(dc, c, mapping); break;
    case CT_FIRE: paint_fire(dc, c, mapping); break;
    default: {
        const int armed = mapping && g_map_state >= 0 && c->target == g_map_state;
        draw_face(dc, &c->rc, c->lit, armed, 0);
        SelectObject(dc, g_font);
        text_line(dc, c->label, &c->rc, DT_CENTER);
        break;
    }
    }
}

/* One row per gamepad: its name, the stick it drives now, and the Automatic
 * / Stick 1 / Stick 2 choice, the current one pushed in. */
static void paint_gamepads(HDC dc)
{
    static const char *const segs[3] = { "Automatic", "Stick 1", "Stick 2" };
    const int n = o2session_gamepad_count(g_session);
    int i, s;

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    text_line(dc, "Gamepads", &g_gp_head_rc, DT_LEFT);
    SelectObject(dc, g_font);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    if (n == 0) {
        text_line(dc, "No gamepads connected", &g_gp_row_rc[0], DT_LEFT);
        return;
    }
    if (n > PAD_ROWS) {
        char more[48];
        snprintf(more, sizeof more, "%d more not shown", n - PAD_ROWS);
        text_line(dc, more, &g_gp_head_rc, DT_RIGHT);
    }
    for (i = 0; i < n && i < PAD_ROWS; i++) {
        const int assign = o2session_gamepad_assignment(g_session, i);
        const int eff = o2session_gamepad_effective_port(g_session, i);
        const int on = assign < 0 || assign >= 2 ? 0 : assign + 1;
        char name[128], line[32];
        RECT name_rc, eff_rc, seg0;

        gp_name_rect(i, &name_rc);
        segment_rect(i, 0, &seg0);
        o2session_gamepad_name(g_session, i, name, sizeof name);
        SelectObject(dc, g_font);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        text_line(dc, name, &name_rc, DT_LEFT | DT_END_ELLIPSIS);
        if (eff >= 0) snprintf(line, sizeof line, "drives stick %d", eff + 1);
        else snprintf(line, sizeof line, "drives nothing");
        SetRect(&eff_rc, name_rc.right + S(8), name_rc.top, seg0.left - S(8), name_rc.bottom);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        text_line(dc, line, &eff_rc, DT_RIGHT);
        for (s = 0; s < 3; s++) {
            RECT r;
            segment_rect(i, s, &r);
            draw_face(dc, &r, s == on, 0, 0);
            text_line(dc, segs[s], &r, DT_CENTER);
        }
    }
}

static void paint_panel(HDC dc)
{
    RECT client;
    int i;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);

    /* the console: silver body, black keyboard plate */
    round_rect(dc, &g_silver_rc, S(16), RGB(0xC3, 0xC0, 0xBF), RGB(0x8e, 0x8b, 0x8a));
    round_rect(dc, &g_plate_rc, S(10), RGB(0x14, 0x14, 0x14), RGB(0x05, 0x05, 0x05));
    paint_stick(dc, 0);
    paint_stick(dc, 1);
    for (i = 0; i < g_nctl; i++)
        paint_control(dc, &g_ctl[i]);
    paint_gamepads(dc);

    SelectObject(dc, g_font);
    draw_face(dc, &g_map_rc, 0, g_map_state != -2, 0);
    text_line(dc, g_map_state == -2 ? "Map" : "Done", &g_map_rc, DT_CENTER);
    draw_face(dc, &g_defaults_rc, 0, 0, 0);
    text_line(dc, "Defaults", &g_defaults_rc, DT_CENTER);
    draw_face(dc, &g_help_rc, 0, 0, 0);
    text_line(dc, "Help", &g_help_rc, DT_CENTER);
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    text_line(dc, g_hint, &g_hint_rc, DT_LEFT | DT_END_ELLIPSIS);
}

/* ---- polling -------------------------------------------------------------------------- */

/* What the console sees held, lit. */
static void refresh_held(void)
{
    unsigned held[2];
    int i, port, redraw_sticks = 0;
    if (!g_panel || g_map_state != -2) return;
    for (port = 0; port < 2; port++)
        held[port] = o2session_buttons_held(g_session, port);
    for (i = 0; i < g_nctl; i++) {
        control *c = &g_ctl[i];
        int on;
        if (c->kind == CT_KEY)
            on = c->key >= 0 ? o2session_key_held(g_session, c->key) != 0
                             : o2session_switch_held(g_session, O2_SW_RESET) != 0;
        else if (c->kind == CT_DIR || c->kind == CT_FIRE)
            on = (held[c->port] >> c->act) & 1;
        else
            on = i == g_mouse_ctl;
        if (on != c->lit) {
            c->lit = on;
            InvalidateRect(g_panel, &c->rc, FALSE);
            if (c->kind == CT_DIR) redraw_sticks |= 1 << c->port;
        }
    }
    /* a stick's knob leans with its directions */
    for (port = 0; port < 2; port++)
        if (redraw_sticks & (1 << port)) InvalidateRect(g_panel, &g_stick_rc[port], FALSE);
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in
 * Preferences. */
static void refresh_pads(void)
{
    pads_state now;
    int i;
    memset(&now, 0, sizeof now);
    now.generation = o2session_gamepad_generation(g_session);
    now.count = o2session_gamepad_count(g_session);
    for (i = 0; i < PAD_ROWS && i < now.count; i++) {
        now.assign[i] = o2session_gamepad_assignment(g_session, i);
        now.effective[i] = o2session_gamepad_effective_port(g_session, i);
    }
    if (memcmp(&now, &g_pads_shown, sizeof now) != 0) {
        g_pads_shown = now;
        InvalidateRect(g_panel, &g_gp_rc, FALSE);
    }
    if (g_built_machine != machine_now())
        rebuild();
}

/* ---- keys ------------------------------------------------------------------------------ */

static void hide_panel(void)
{
    set_map_state(-2);
    mouse_release(0);
    if (GetCapture() == g_panel) ReleaseCapture();
    g_swallow_vk = 0;
    ShowWindow(g_panel, SW_HIDE);
}

/* A key while Map is armed: 1 if Map took it. Waiting for a target, a key is
 * not typing; waiting for a binding, the key is the binding (not F9, F11 or
 * F12, which are the app's). */
int o2_controllers_map_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;

    if (!g_panel) return 0;
    if (g_swallow_vk && vk == g_swallow_vk) {
        if (!down) g_swallow_vk = 0;
        return 1;
    }
    if (g_map_state == -2) return 0;
    if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 0;
    if (msg == WM_SYSKEYDOWN && vk == VK_F4) return 0;
    if (!down || (lp & (1 << 30))) return 1;
    if (vk == VK_ESCAPE) {
        set_map_state(g_map_state >= 0 ? -1 : -2);
        g_swallow_vk = vk;
        return 1;
    }
    if (g_map_state >= 0) {
        char stolen[128], name[32];
        const int target = g_map_state;
        const uint32_t ks = o2_keysym_from_msg(vk, lp);
        if (!ks) return 1;              /* nothing to call it: keep waiting */
        stolen[0] = '\0';
        o2session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        o2session_keysym_name(ks, name, sizeof name);
        g_swallow_vk = vk;
        set_map_state(-1);              /* stay armed: remapping several in a row is normal */
        bound_hint(name, stolen);
    }
    return 1;
}

/* A key with the window focused: 1 if taken. */
static int panel_key(UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    uint32_t ks;
    int sa;

    if (o2_controllers_map_key(msg, vk, lp)) return 1;
    if (vk == VK_F9) {
        if (down && !(lp & (1 << 30))) hide_panel();
        return 1;
    }
    if (o2_app_key(vk)) {
        /* the main window's shortcuts, as if it had the focus */
        if (g_parent && down) SendMessageA(g_parent, msg, vk, lp);
        return 1;
    }
    ks = o2_keysym_from_msg(vk, lp);
    if (down && msg == WM_KEYDOWN && ks && (sa = o2session_key_sysaction(g_session, ks)) >= 0) {
        /* the main window runs these */
        if (!(lp & (1 << 30))) o2session_sysaction_post(g_session, sa);
        return 1;
    }
    return o2_forward_key(g_session, msg, vk, lp);
}

/* Exactly the laid-out client size, whatever the frame is at this DPI. */
static void fit_client(void)
{
    RECT wr, cr;
    GetWindowRect(g_panel, &wr);
    GetClientRect(g_panel, &cr);
    if (cr.right == g_total_w && cr.bottom == g_total_h) return;
    SetWindowPos(g_panel, NULL, 0, 0, (wr.right - wr.left) - cr.right + g_total_w,
                 (wr.bottom - wr.top) - cr.bottom + g_total_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The window's outer size for the laid-out client. */
static void window_size(HWND hwnd, SIZE *sz)
{
    RECT wr, cr;
    if (hwnd && GetWindowRect(hwnd, &wr) && GetClientRect(hwnd, &cr) && cr.right > 0) {
        sz->cx = (wr.right - wr.left) - cr.right + g_total_w;
        sz->cy = (wr.bottom - wr.top) - cr.bottom + g_total_h;
    } else {
        SetRect(&wr, 0, 0, g_total_w, g_total_h);
        AdjustWindowRectEx(&wr, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
        sz->cx = wr.right - wr.left;
        sz->cy = wr.bottom - wr.top;
    }
}

static void rescale(int dpi)
{
    g_dpi = dpi;
    layout_static();
    rebuild();
}

/* ---- the window ----------------------------------------------------------------------- */

static int control_at(POINT p)
{
    int i;
    for (i = 0; i < g_nctl; i++)
        if (PtInRect(&g_ctl[i].rc, p)) return i;
    return -1;
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 25 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp, oldfont;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        oldfont = SelectObject(mem, g_font);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldfont);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        POINT p;
        int i, row, seg;
        p.x = GET_X_LPARAM(lp);
        p.y = GET_Y_LPARAM(lp);
        SetFocus(hwnd);
        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_help_rc, p)) { o2_help_show(hwnd, "controllers"); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            o2session_bindings_reset(g_session);
            set_hint("Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (row = 0; row < PAD_ROWS && row < o2session_gamepad_count(g_session); row++)
            for (seg = 0; seg < 3; seg++) {
                RECT r;
                segment_rect(row, seg, &r);
                if (PtInRect(&r, p)) {
                    /* 0 = automatic (connection order), 1-2 = that stick */
                    o2session_gamepad_assign(g_session, row, seg - 1);
                    refresh_pads();
                    InvalidateRect(hwnd, &g_gp_rc, FALSE);
                    return 0;
                }
            }
        i = control_at(p);
        if (i < 0) return 0;
        if (g_map_state != -2) {
            /* armed: picks (or changes) the target */
            if (g_ctl[i].target >= 0) set_map_state(g_ctl[i].target);
            return 0;
        }
        control_press(i);
        SetCapture(hwnd);
        refresh_held();
        return 0;
    }
    case WM_MOUSEMOVE:
        /* dragged off what it holds: let go, as a button does */
        if (g_mouse_ctl >= 0) {
            POINT p;
            p.x = GET_X_LPARAM(lp);
            p.y = GET_Y_LPARAM(lp);
            if (g_mouse_ctl >= g_nctl || !PtInRect(&g_ctl[g_mouse_ctl].rc, p)) {
                mouse_release(0);
                if (GetCapture() == hwnd) ReleaseCapture();
                refresh_held();
            }
        }
        return 0;
    case WM_LBUTTONUP:
        /* released wherever the mouse ended up: capture means this arrives
         * even if the pointer left the cap */
        if (g_mouse_ctl >= 0) {
            mouse_release(1);
            refresh_held();
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        /* lost the mouse some other way (Alt+Tab mid-press): let go */
        if ((HWND)lp != hwnd && g_mouse_ctl >= 0) {
            mouse_release(0);
            refresh_held();
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_CAPTURE) {
            int button;
            if (g_map_state < 0) {
                KillTimer(hwnd, IDT_CAPTURE);
            } else if (o2session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                o2session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(o2_pad_button_name(button), stolen);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        } else if (wp == IDT_PADS) {
            refresh_pads();
        }
        return 0;

    /* WM_SYSKEY* too: a bound Alt is the console's */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        if (panel_key(msg, wp, lp)) return 0;
        break;
    case WM_SYSCHAR:
        /* Alt+letter: the main window's menu mnemonics, as when it has the focus */
        if (wp != ' ' && g_parent) { SendMessageA(g_parent, msg, wp, lp); return 0; }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            const HWND other = (HWND)lp;
            mouse_release(0);
            if (GetCapture() == hwnd) ReleaseCapture();
            g_swallow_vk = 0;       /* its release goes elsewhere */
            /* Leaving the app: nothing typed here stays held (within it, the
             * window that gets the key-up lets go). */
            if (!other || GetWindowThreadProcessId(other, NULL) != GetCurrentThreadId())
                o2session_release_all(g_session);
            refresh_held();
        }
        break;                      /* DefWindowProc gives the window the focus */
    case WM_SHOWWINDOW:
        if (wp) {
            refresh_held();
            SetTimer(hwnd, IDT_HELD, 40, NULL);
            SetTimer(hwnd, IDT_PADS, 250, NULL);
        } else {
            KillTimer(hwnd, IDT_HELD);
            KillTimer(hwnd, IDT_PADS);
        }
        break;
    case WM_GETMINMAXINFO:
        /* Fixed: the size limits say so too, so neither maximizing nor a
         * compositor (Wine passes them on) makes it any other size. */
        if (g_total_w) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            SIZE sz;
            window_size(hwnd, &sz);
            mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = sz.cx;
            mm->ptMinTrackSize.y = mm->ptMaxTrackSize.y = mm->ptMaxSize.y = sz.cy;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        rescale(HIWORD(wp));
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        fit_client();
        return 0;
    }
    case WM_CLOSE:
        /* Hide, do not destroy: a remap and the position survive closing it. */
        hide_panel();
        return 0;
    case WM_DESTROY:
        /* with the main window: the next toggle starts afresh */
        mouse_release(0);
        set_map_state(-2);
        g_panel = NULL;
        g_nctl = 0;
        g_built_machine = -1;
        free_fonts();
        break;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_panel(void)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT want;

    if (!registered) {
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = panel_proc;    /* no CS_DBLCLKS: fast clicks stay press/release pairs */
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PANEL_CLASS;
        registered = RegisterClassExA(&wc) != 0;
    }
    if (!g_accent_brush)
        g_accent_brush = CreateSolidBrush(rgb_of(O2SESSION_ACCENT_RGB));
    g_dpi = o2_window_dpi(g_parent);
    layout_static();

    SetRect(&want, 0, 0, g_total_w, g_total_h);
    /* A tool window with a caption and no thick frame or maximize box: it is
     * exactly the size of its contents, and stays off the taskbar. */
    AdjustWindowRectEx(&want, PANEL_STYLE, FALSE, PANEL_EXSTYLE);
    g_panel = CreateWindowExA(PANEL_EXSTYLE, PANEL_CLASS, "Controllers", PANEL_STYLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left, want.bottom - want.top,
                              g_parent, NULL, inst, NULL);
    if (!g_panel) return 0;
    o2_help_set_context(g_panel, "controllers", 0);    /* F1 is the console's RESET here */
    rescale(o2_window_dpi(g_panel));    /* placed on another monitor than the main window */
    fit_client();
    memset(&g_pads_shown, 0, sizeof g_pads_shown);
    set_map_state(-2);
    return 1;
}

static void show_panel(void)
{
    if (g_built_machine != machine_now())
        rebuild();
    refresh_pads();
    ShowWindow(g_panel, SW_SHOW);
    SetForegroundWindow(g_panel);
}

void o2_controllers_window_toggle(HWND parent, o2session *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_panel && !create_panel())
        return;
    if (IsWindowVisible(g_panel))
        hide_panel();
    else
        show_panel();
}

int o2_controllers_window_visible(void)
{
    return g_panel && IsWindowVisible(g_panel);
}

void o2_controllers_window_refresh(void)
{
    if (!g_panel)
        return;
    if (IsWindowVisible(g_panel)) {
        refresh_pads();             /* rebuilds if the machine changed */
        InvalidateRect(g_panel, NULL, FALSE);
    } else {
        g_built_machine = -1;       /* rebuilt when next shown */
    }
}

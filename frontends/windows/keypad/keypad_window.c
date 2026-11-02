/*
 * The Win32 Keypad window: the Odyssey 2's two ten-key keypads side by side,
 * laid out as on the console (1 2 3 / 4 5 6 / 7 8 9 / 0 under the 8), each
 * with the gamepad driving it, then the console's CLEAR button and the
 * session's resets, then the Map row, then the gamepads and which keypad
 * each one drives.
 *
 * A key lights in the accent colour whenever the console sees it held, from
 * whatever source (keyboard, gamepad, or a click here), so the window doubles
 * as an input tester.
 *
 * Keys are driven by WM_LBUTTONDOWN/WM_LBUTTONUP on the window rather than by
 * BN_CLICKED: a keypad key is HELD, and the BIOS scans the keypads by
 * latching one key at a time and reading EF3/EF4, so a value present only for
 * the instant of a click falls between scans. The mouse is captured on press
 * and released on button-up wherever that happens, so dragging off a key
 * cannot strand the machine with a key held forever.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

/* GET_X_LPARAM / GET_Y_LPARAM live here, not in windows.h. */
#include <windowsx.h>

#include <stdio.h>
#include <string.h>

#include "../key_forward.h"

#define PAD_CLASS "FujiNetGoO2Keypad"

#define KEY_W    72         /* wide enough for a Map-mode label ("Keypad 7" over "D-pad Up") */
#define KEY_H    44
#define GAP       6
#define MARGIN   12
#define PAD_W   (MARGIN + 3 * KEY_W + 2 * GAP + MARGIN)
#define HEAD_H   22
#define PADS_H   20
#define WIDE_W  120
#define WIDE_H   40
#define ROW_H    26         /* one gamepad's row */
#define PAD_ROWS  4         /* gamepad rows the window has room for */
#define SEG_W    76         /* Automatic / Keypad A / Keypad B */
#define EFF_W   110         /* "drives keypad A" */

#define IDT_CAPTURE 1
#define IDT_HELD    2

typedef struct {
    RECT rc;
    int target;
    int key;                 /* a keypad key: its digit drawn large */
    char face[20];
} pad_button;

static HWND g_panel;
static o2session *g_session;
static pad_button g_btn[O2_TARGET_COUNT];
static int g_nbtn;
static int g_held = -1;          /* button index under the captured mouse */
static int g_map_state = -2;     /* -2 idle, -1 armed, >=0 awaiting a key/button */
static RECT g_map_rc, g_defaults_rc, g_hint_rc;
static RECT g_head_rc[2], g_pads_rc[2], g_box_rc[2];
static RECT g_gp_head_rc, g_gp_row_rc[PAD_ROWS];
static unsigned g_shown_held[2];
static int g_shown_sw[O2_SW_COUNT];
static char g_hint[200];
static HBRUSH g_accent_brush, g_body_brush;
static HFONT g_bold, g_digit;
static int g_total_w, g_total_h;

static void add_button(const char *face, int target, int x, int y, int w, int h, int key)
{
    pad_button *b;
    if (g_nbtn >= O2_TARGET_COUNT) return;
    b = &g_btn[g_nbtn++];
    SetRect(&b->rc, x, y, x + w, y + h);
    b->target = target;
    b->key = key;
    snprintf(b->face, sizeof b->face, "%s", face);
}

/* One keypad, laid out like the console's: 1-9 in three rows of three and
 * 0 alone under the 8. Returns the y below it. */
static int build_keypad(int port, int x0, int y0)
{
    int y = y0, i;

    SetRect(&g_head_rc[port], x0, y, x0 + PAD_W, y + HEAD_H);
    y += HEAD_H;
    SetRect(&g_pads_rc[port], x0, y, x0 + PAD_W, y + PADS_H);
    y += PADS_H + GAP;

    SetRect(&g_box_rc[port], x0, y, x0 + PAD_W, y + 4 * KEY_H + 3 * GAP + 2 * MARGIN);
    y += MARGIN;
    for (i = 0; i < 10; i++) {
        /* 1-9 in reading order, then 0 in the middle column of the last row */
        const int cell = i == 0 ? 10 : i - 1;
        const char face[2] = { (char)('0' + i), '\0' };
        add_button(face, O2_TARGET_PORT(port, O2_ACT_0 + i),
                   x0 + MARGIN + (cell % 3) * (KEY_W + GAP), y + (cell / 3) * (KEY_H + GAP),
                   KEY_W, KEY_H, 1);
    }
    return g_box_rc[port].bottom;
}

static void layout(void)
{
    const int x1 = MARGIN + PAD_W + 2 * MARGIN;
    int y, cx, i;

    g_nbtn = 0;
    y = build_keypad(0, MARGIN, MARGIN);
    build_keypad(1, x1, MARGIN);
    g_total_w = x1 + PAD_W + MARGIN;

    /* The console's button and the session's resets, centred. Debugger
     * Stop is here so it can be mapped like everything else. */
    y += MARGIN + 4;
    cx = (g_total_w - (3 * WIDE_W + 2 * GAP)) / 2;
    add_button("CLEAR", O2_TARGET_SWITCH(O2_SW_CLEAR), cx, y, WIDE_W, WIDE_H, 0);
    add_button("Reset to CONFIG", O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG), cx + (WIDE_W + GAP), y,
               WIDE_W, WIDE_H, 0);
    add_button("Debugger Stop", O2_TARGET_SYSACT(O2_SYSACT_DEBUG_STOP), cx + 2 * (WIDE_W + GAP), y,
               WIDE_W, WIDE_H, 0);
    y += WIDE_H + MARGIN;

    SetRect(&g_map_rc, MARGIN, y, MARGIN + 70, y + 30);
    SetRect(&g_defaults_rc, MARGIN + 76, y, MARGIN + 76 + 84, y + 30);
    SetRect(&g_hint_rc, MARGIN + 76 + 84 + 10, y, g_total_w - MARGIN, y + 30);
    y += 30 + MARGIN + 4;

    /* The gamepads: a fixed number of rows, so the window never resizes
     * when one is plugged in. */
    SetRect(&g_gp_head_rc, MARGIN, y, g_total_w - MARGIN, y + HEAD_H);
    y += HEAD_H + 2;
    for (i = 0; i < PAD_ROWS; i++) {
        SetRect(&g_gp_row_rc[i], MARGIN, y, g_total_w - MARGIN, y + ROW_H);
        y += ROW_H + 2;
    }
    g_total_h = y + MARGIN;
}

/* The three assignment segments at the right of a gamepad row. */
static void segment_rect(int row, int seg, RECT *rc)
{
    const RECT *r = &g_gp_row_rc[row];
    const int x = r->right - (3 - seg) * (SEG_W + 4) + 4;
    SetRect(rc, x, r->top + 1, x + SEG_W, r->bottom - 1);
}

/* The session's actions, from a click here or a key: Debugger Stop has to
 * open the debugger window, which is the main window's business, so it goes
 * through the session's sysaction queue that the main window polls. */
static void run_sysaction(int sa)
{
    if (sa == O2_SYSACT_DEBUG_STOP) o2session_sysaction_post(g_session, sa);
    else o2session_sysaction(g_session, sa);
}

static void press_target(int target, int down)
{
    if (target >= O2_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. */
        if (!down) run_sysaction(target - O2_TARGET_SYSACT(0));
        return;
    }
    /* A key, or CLEAR: held for as long as the mouse is down. */
    o2session_press(g_session, target, down);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, p)) return i;
    return -1;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        KillTimer(g_panel, IDT_CAPTURE);
        o2session_gamepad_capture_cancel(g_session);
        g_hint[0] = '\0';
    } else if (state == -1) {
        KillTimer(g_panel, IDT_CAPTURE);
        o2session_gamepad_capture_cancel(g_session);
        snprintf(g_hint, sizeof g_hint, "Click a control to remap it");
    } else {
        snprintf(g_hint, sizeof g_hint, "Press a key or gamepad button for %s", o2_target_name(state));
        o2session_gamepad_capture_begin(g_session);
        SetTimer(g_panel, IDT_CAPTURE, 50, NULL);
    }
    InvalidateRect(g_panel, NULL, TRUE);
}

/* After a rebind: name what it now does, and what it was taken from. */
static void bound_hint(int target, const char *what, const char *stolen)
{
    char name[64];
    snprintf(name, sizeof name, "%s", o2_target_name(target));   /* a static buffer */
    if (stolen[0])
        snprintf(g_hint, sizeof g_hint, "%s: %s (taken from %s)", name, what, stolen);
    else
        snprintf(g_hint, sizeof g_hint, "%s: %s", name, what);
}

static void button_label(const pad_button *b, char *out, int outsz)
{
    if (g_map_state != -2) {
        const o2_binding bind = o2session_binding_get(g_session, b->target);
        char key[32];
        o2session_keysym_name(bind.keysym, key, sizeof key);
        if (bind.button != O2_PAD_BTN_NONE)
            snprintf(out, outsz, "%s\n%s", key[0] ? key : "-", o2_pad_button_name(bind.button));
        else
            snprintf(out, outsz, "%s", key[0] ? key : "-");
    } else {
        snprintf(out, outsz, "%s", b->face);
    }
}

static void draw_label(HDC dc, const RECT *r, const char *label, int pushed)
{
    RECT text = *r;
    int h;
    /* Vertically centred, word-broken on the newline a Map-mode label
     * carries between its key and its pad button. */
    InflateRect(&text, -2, 0);
    h = DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    text.left = r->left + 2;
    text.right = r->right - 2;
    text.top = r->top + ((r->bottom - r->top) - h) / 2 + (pushed ? 1 : 0);
    text.bottom = r->bottom;
    DrawTextA(dc, label, -1, &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* A held or Map-target control wears the tan accent with dark text on it:
 * the tan is too pale to carry white. */
static void draw_button(HDC dc, const RECT *rc, const char *label, int pushed, int accent)
{
    RECT r = *rc;

    if (accent) {
        FillRect(dc, &r, g_accent_brush);
        FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(0, 0, 0));
    } else {
        DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH | (pushed ? DFCS_PUSHED : 0));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    }
    draw_label(dc, &r, label, pushed);
}

/* The gamepads driving a keypad, as one line. */
static void pads_line(int port, char *out, int outsz)
{
    int i, n = o2session_gamepad_count(g_session), len = 0;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        char name[96];
        if (o2session_gamepad_effective_port(g_session, i) != port) continue;
        o2session_gamepad_name(g_session, i, name, sizeof name);
        len += snprintf(out + len, (size_t)(outsz - len), "%s%s", len ? ", " : "Gamepad: ", name);
        if (len >= outsz) break;
    }
    if (!out[0]) snprintf(out, (size_t)outsz, "No gamepad (keyboard only)");
}

/* One row per gamepad: its name, which keypad it drives now, and the
 * Automatic / Keypad A / Keypad B choice, the current one pushed in. */
static void paint_gamepads(HDC dc, HFONT font)
{
    static const char *const segs[3] = { "Automatic", "Keypad A", "Keypad B" };
    const int n = o2session_gamepad_count(g_session);
    int i, s;

    SelectObject(dc, g_bold);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    DrawTextA(dc, "Gamepads", -1, &g_gp_head_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, font);
    if (n == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, "No gamepads connected", -1, &g_gp_row_rc[0],
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    for (i = 0; i < n && i < PAD_ROWS; i++) {
        char name[128], line[64];
        RECT name_rc = g_gp_row_rc[i], eff_rc;
        const int assign = o2session_gamepad_assignment(g_session, i);
        const int eff = o2session_gamepad_effective_port(g_session, i);
        RECT seg0;

        segment_rect(i, 0, &seg0);
        o2session_gamepad_name(g_session, i, name, sizeof name);
        name_rc.right = seg0.left - EFF_W - 8;
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, name, -1, &name_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetRect(&eff_rc, name_rc.right + 8, name_rc.top, seg0.left - 8, name_rc.bottom);
        if (eff >= 0) snprintf(line, sizeof line, "drives keypad %c", 'A' + eff);
        else snprintf(line, sizeof line, "drives nothing");
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &eff_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (s = 0; s < 3; s++) {
            RECT r;
            segment_rect(i, s, &r);
            draw_button(dc, &r, segs[s], assign + 1 == s, 0);
        }
    }
}

static void paint_panel(HDC dc)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ old = SelectObject(dc, font);
    RECT client;
    int i, port;

    GetClientRect(g_panel, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_BTNFACE + 1));
    SetBkMode(dc, TRANSPARENT);

    for (port = 0; port < 2; port++) {
        char line[200];
        RECT box = g_box_rc[port];
        SelectObject(dc, g_bold);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, port ? "Keypad B" : "Keypad A", -1, &g_head_rc[port],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, font);
        pads_line(port, line, sizeof line);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, line, -1, &g_pads_rc[port], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* the keypad's dark recess in the console's top, so the tan of a
         * held key stands out */
        FillRect(dc, &box, g_body_brush);
        FrameRect(dc, &box, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
    }

    for (i = 0; i < g_nbtn; i++) {
        char label[64];
        const int t = g_btn[i].target;
        int lit = 0, accent;
        const int clicked = (i == g_held) && g_map_state == -2;
        if (t < 2 * O2_ACT_PER_PORT)
            lit = (g_shown_held[t / O2_ACT_PER_PORT] >> (t % O2_ACT_PER_PORT)) & 1;
        else if (t < O2_TARGET_SYSACT(0))
            lit = g_shown_sw[t - O2_TARGET_SWITCH(0)];
        button_label(&g_btn[i], label, sizeof label);
        accent = (g_map_state == -2 && (lit || clicked)) || (g_map_state >= 0 && t == g_map_state);
        /* the digits large, as printed on the keys; Map-mode labels small */
        SelectObject(dc, g_btn[i].key && g_map_state == -2 ? g_digit : font);
        draw_button(dc, &g_btn[i].rc, label, clicked, accent);
    }
    SelectObject(dc, font);

    draw_button(dc, &g_map_rc, g_map_state == -2 ? "Map" : "Done", 0, g_map_state != -2);
    draw_button(dc, &g_defaults_rc, "Defaults", 0, 0);

    if (g_hint[0]) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, g_hint, -1, &g_hint_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    paint_gamepads(dc, font);
    SelectObject(dc, old);
}

static void refresh_held(void)
{
    int port, sw, changed = 0;
    for (port = 0; port < 2; port++) {
        unsigned now = o2session_buttons_held(g_session, port);
        if (now != g_shown_held[port]) { g_shown_held[port] = now; changed = 1; }
    }
    for (sw = 0; sw < O2_SW_COUNT; sw++) {
        const int now = o2session_switch_held(g_session, sw);
        if (now != g_shown_sw[sw]) { g_shown_sw[sw] = now; changed = 1; }
    }
    if (changed) InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        /* Off-screen first: the held highlights repaint at 20 Hz. */
        RECT c;
        HDC mem;
        HBITMAP bmp;
        HGDIOBJ oldbmp;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        oldbmp = SelectObject(mem, bmp);
        paint_panel(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT p = { x, y };
        int i = hit(x, y), row, seg;

        if (PtInRect(&g_map_rc, p)) { set_map_state(g_map_state == -2 ? -1 : -2); return 0; }
        if (PtInRect(&g_defaults_rc, p)) {
            o2session_bindings_reset(g_session);
            snprintf(g_hint, sizeof g_hint, "Every key and button is back to its default");
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        /* A gamepad row's keypad choice. */
        for (row = 0; row < PAD_ROWS && row < o2session_gamepad_count(g_session); row++) {
            for (seg = 0; seg < 3; seg++) {
                RECT r;
                segment_rect(row, seg, &r);
                if (PtInRect(&r, p)) {
                    /* 0 = automatic (connection order), 1 = keypad A, 2 = keypad B */
                    o2session_gamepad_assign(g_session, row, seg - 1);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        if (i < 0) return 0;

        if (g_map_state == -1) { set_map_state(g_btn[i].target); return 0; }
        if (g_map_state >= 0) return 0;

        g_held = i;
        SetCapture(hwnd);
        press_target(g_btn[i].target, 1);
        InvalidateRect(hwnd, &g_btn[i].rc, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        /* Release wherever the mouse ended up: capture means this arrives
         * even if the pointer left the button. */
        if (g_held >= 0) {
            RECT r = g_btn[g_held].rc;
            int t = g_btn[g_held].target;
            g_held = -1;
            ReleaseCapture();
            press_target(t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        /* Lost the mouse some other way (Alt+Tab mid-press): let go, but
         * without firing a system action nobody finished clicking. */
        if (g_held >= 0 && (HWND)lp != hwnd) {
            RECT r = g_btn[g_held].rc;
            const int t = g_btn[g_held].target;
            g_held = -1;
            if (t < O2_TARGET_SYSACT(0)) o2session_press(g_session, t, 0);
            InvalidateRect(hwnd, &r, FALSE);
        }
        break;

    case WM_TIMER:
        if (wp == IDT_CAPTURE && g_map_state >= 0) {
            int button;
            if (o2session_gamepad_capture_poll(g_session, &button)) {
                char stolen[128];
                const int target = g_map_state;
                stolen[0] = '\0';
                o2session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(target, o2_pad_button_name(button), stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } else if (wp == IDT_HELD) {
            refresh_held();
        }
        return 0;

    /* WM_SYSKEY* too: Left Alt is keypad A's 0 by default (MAME's). */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const uint32_t ks = o2_keysym_from_msg(wp, lp);
        int sa;
        if (lp & (1 << 30)) return 0;   /* auto-repeat */
        if (g_map_state >= 0) {
            if (wp == VK_ESCAPE && !(GetKeyState(VK_SHIFT) & 0x8000)) { set_map_state(-1); return 0; }
            if (ks) {
                char stolen[128], name[32];
                const int target = g_map_state;
                stolen[0] = '\0';
                o2session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
                o2session_keysym_name(ks, name, sizeof name);
                set_map_state(-1);   /* stay armed: remapping several in a row is normal */
                bound_hint(target, name, stolen);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        if (g_map_state == -1) {
            if (wp == VK_ESCAPE) set_map_state(-2);
            return 0;
        }
        if (wp == VK_F9) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (!ks) break;
        /* not from a system key: Alt+Esc is Windows', not Reset to CONFIG */
        sa = msg == WM_KEYDOWN ? o2session_key_sysaction(g_session, ks) : -1;
        if (sa >= 0) { run_sysaction(sa); return 0; }
        if (o2session_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        const uint32_t ks = o2_keysym_from_msg(wp, lp);
        if (g_map_state != -2) return 0;
        /* A bound Alt's release stops here, so it does not open the
         * system menu as well. */
        if (ks && o2session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) o2session_release_all(g_session);
        return 0;
    case WM_SHOWWINDOW:
        if (wp) { refresh_held(); SetTimer(hwnd, IDT_HELD, 50, NULL); }
        else KillTimer(hwnd, IDT_HELD);
        break;
    case WM_CLOSE:
        /* Hide, do not destroy: the window's position survives closing it. */
        set_map_state(-2);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void o2_keypad_window_toggle(HWND parent, o2session *session)
{
    g_session = session;

    if (!g_panel) {
        WNDCLASSEXA wc;
        RECT want;
        LOGFONTA lf;

        layout();
        g_accent_brush = CreateSolidBrush(RGB((O2SESSION_ACCENT_RGB >> 16) & 0xff,
                                              (O2SESSION_ACCENT_RGB >> 8) & 0xff,
                                              O2SESSION_ACCENT_RGB & 0xff));
        g_body_brush = CreateSolidBrush(RGB(0x3a, 0x34, 0x2e));
        GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        g_bold = CreateFontIndirectA(&lf);
        lf.lfHeight = lf.lfHeight * 16 / 10;
        g_digit = CreateFontIndirectA(&lf);

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = PAD_CLASS;
        RegisterClassExA(&wc);

        SetRect(&want, 0, 0, g_total_w, g_total_h);
        /* A tool window with a caption and no thick frame or maximize box:
         * it is exactly the size of its controls. WS_EX_TOOLWINDOW also
         * keeps it off the taskbar. */
        AdjustWindowRectEx(&want, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, FALSE, WS_EX_TOOLWINDOW);
        g_panel = CreateWindowExA(WS_EX_TOOLWINDOW, PAD_CLASS, "Keypad",
                                  WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  want.right - want.left, want.bottom - want.top,
                                  parent, NULL, wc.hInstance, NULL);
        if (!g_panel) return;
        g_shown_held[0] = g_shown_held[1] = 0;
        memset(g_shown_sw, 0, sizeof g_shown_sw);
    }

    if (IsWindowVisible(g_panel)) {
        set_map_state(-2);
        ShowWindow(g_panel, SW_HIDE);
    } else {
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
    }
}

void o2_keypad_window_gamepads_changed(void)
{
    if (g_panel && IsWindowVisible(g_panel)) InvalidateRect(g_panel, NULL, FALSE);
}

int o2_keypad_pretranslate(MSG *msg)
{
    /* The window has no child controls, so its own window proc sees every
     * key; nothing to steal here. Kept for symmetry with the debugger. */
    (void)msg;
    return 0;
}

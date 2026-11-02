/*
 * FujiNet Go Odyssey 2 -- the Windows (Win32 + GDI) frontend.
 *
 * No toolkit: a plain window, a menu bar, a status bar and StretchDIBits.
 * That is enough for the Odyssey 2's 100x246 picture, and it keeps the
 * artifact a folder you copy rather than a runtime hunt.
 *
 * Two Windows-specific things are load bearing:
 *
 *   DwmFlush() on a present thread is this platform's frame clock. There is
 *   no GdkFrameClock here, and a plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away with a keypad key
 *   down and coming back to a game that still sees it held is the classic
 *   symptom of not doing this, and Windows is where it happens most, because
 *   the WM eats the key-up.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2session.h"
#include "debugger/dbg_window.h"
#include "key_forward.h"
#include "keypad/keypad_window.h"
#include "resource.h"

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define WIN_CLASS "FujiNetGoO2"
#define APP_TITLE "FujiNet Go Odyssey 2"
#define APP_TITLE_W L"FujiNet Go Odyssey 2"

/* How long a gamepad connect/disconnect notice stays in the status bar. */
#define PAD_NOTICE_MS 4000

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

static o2session *g_session;
static HWND g_hwnd;
static HWND g_statusbar;
static uint32_t *g_fb;
static int g_have_frame;
static uint64_t g_serial;
static BITMAPINFO g_bmi;
static CRITICAL_SECTION g_fb_lock;
static volatile LONG g_running = 1;
static HANDLE g_present_thread;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;   /* the TV's 4:3 on */
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[O2_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_pad_notice[160];
static DWORD g_pad_notice_until;
static HBRUSH g_accent_brush;
static int g_bios_alert;         /* needs_bios as last seen: alert once per rise */

/* ---- the present thread: this platform's frame clock ---------------------- */

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        LARGE_INTEGER t, f;
        /* Blocks until the compositor's next vblank; falls through at once
         * if the DWM is off, and the core's wall-clock pacing takes over --
         * which is the whole reason notify_vsync is advisory. */
        if (FAILED(DwmFlush())) Sleep(8);
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        o2session_notify_vsync(g_session,
                                (int64_t)(t.QuadPart * 1000000000LL / f.QuadPart));
        EnterCriticalSection(&g_fb_lock);
        if (o2session_copy_frame(g_session, g_fb, &g_serial)) {
            g_have_frame = 1;
            LeaveCriticalSection(&g_fb_lock);
            InvalidateRect(g_hwnd, NULL, FALSE);
        } else {
            LeaveCriticalSection(&g_fb_lock);
        }
    }
    return 0;
}

/* ---- painting ------------------------------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The picture's shape: the 1861's 100x246 visible area filled the
 * television's 4:3, so "TV" shows it at 4:3; "square" shows the
 * framebuffer's own pixels, a very tall 100:246. */
static double picture_aspect(int tv)
{
    return tv ? 4.0 / 3.0 : (double)O2SESSION_FB_WIDTH / (double)O2SESSION_FB_HEIGHT;
}

static void paint(HDC dc)
{
    RECT rc;
    double want, w, h, sw, sh;

    GetClientRect(g_hwnd, &rc);
    rc.bottom -= statusbar_height();
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (w <= 0 || h <= 0) return;

    EnterCriticalSection(&g_fb_lock);
    if (!g_have_frame) { LeaveCriticalSection(&g_fb_lock); return; }

    want = picture_aspect(g_aspect);
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    /* The session's pixels are 0x00RRGGBB: exactly a 32-bit BI_RGB DIB. */
    SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
    StretchDIBits(dc, (int)((w - sw) / 2), (int)((h - sh) / 2), (int)sw, (int)sh,
                  0, 0, O2SESSION_FB_WIDTH, O2SESSION_FB_HEIGHT, g_fb, &g_bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    LeaveCriticalSection(&g_fb_lock);
}

/* ---- helpers -------------------------------------------------------------- */

static void update_status(void);

/* Power-cycle with the current settings: the session decides whether that
 * is a power cycle of the console alone or a restart of FujiNet, audio and
 * the gamepads with it. */
static void restart_session(void)
{
    if (o2session_restart(g_session) != 0)
        MessageBoxA(g_hwnd, o2session_last_error(g_session), "Could not start",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void run_sysaction(int sa)
{
    switch (sa) {
    case O2_SYSACT_RESET_CONFIG:
        o2session_sysaction(g_session, sa);
        break;
    case O2_SYSACT_DEBUG_STOP:
        /* Showing the debugger attaches it, and attaching stops the machine. */
        o2_debugger_show(g_hwnd, g_session);
        break;
    default: break;
    }
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    char title[512], st[160], line[400];
    const char *cart = o2session_cart_path(g_session);

    if (!o2session_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
    } else {
        o2session_cart_status(g_session, st, sizeof st);
        if (cart && cart[0])
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 %s", base_name(cart));
        else if (o2session_cart_booted_game(g_session))
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 booted from FujiNet");
        else
            snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 CONFIG");
    }
    set_text_utf8(g_hwnd, title, 0);

    if (!g_statusbar) return;
    /* Part 0 is the link dot, drawn in WM_DRAWITEM from this item data. */
    SendMessageA(g_statusbar, SB_SETTEXTA, SBT_OWNERDRAW | 0,
                 (LPARAM)(o2session_cart_link_up(g_session) == 1));
    if (g_pad_notice[0] && GetTickCount() < g_pad_notice_until) {
        snprintf(line, sizeof line, "%s", g_pad_notice);
    } else {
        g_pad_notice[0] = '\0';
        snprintf(line, sizeof line, "FujiNet: %s%s", st,
                 o2session_fujinet_external(g_session) ? " (external)"
                 : o2session_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while the cartridge's link to FujiNet is up,
 * grey otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = 10;
    RECT r = di->rcItem;
    const int x = r.left + ((r.right - r.left) - size) / 2;
    const int y = r.top + ((r.bottom - r.top) - size) / 2;
    HGDIOBJ oldb = SelectObject(di->hDC, up ? (HGDIOBJ)g_accent_brush : GetStockObject(GRAY_BRUSH));
    HGDIOBJ oldp = SelectObject(di->hDC, GetStockObject(NULL_PEN));
    Ellipse(di->hDC, x, y, x + size + 1, y + size + 1);
    SelectObject(di->hDC, oldp);
    SelectObject(di->hDC, oldb);
}

/* Gamepad hot-plug: the session bumps a generation on every add/remove and
 * keeps the last event as text; show it for a few seconds. */
static void poll_gamepads(void)
{
    const unsigned gen = o2session_gamepad_generation(g_session);
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (o2session_gamepad_last_event(g_session, g_pad_notice, sizeof g_pad_notice) > 0) {
        g_pad_notice_until = GetTickCount() + PAD_NOTICE_MS;
        update_status();
    }
    o2_keypad_window_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------- */

/* A Machine menu item's label with the key it is bound to after the tab --
 * read from the bindings each time the menu opens, so a remap in the Keypad
 * window shows here. The key is only a label: there is no accelerator
 * table, so F3 and Escape reach the console through the bindings rather
 * than being eaten by the menu. */
static void machine_item(HMENU m, UINT id, const char *text, int target, int enabled)
{
    char key[32], label[96];
    const o2_binding b = o2session_binding_get(g_session, target);
    o2session_keysym_name(b.keysym, key, sizeof key);
    if (key[0]) snprintf(label, sizeof label, "%s\t%s", text, key);
    else snprintf(label, sizeof label, "%s", text);
    ModifyMenuA(m, id, MF_BYCOMMAND | MF_STRING, id, label);
    EnableMenuItem(m, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
}

/* The console's one button, CLEAR, and the session's power cycle, each with
 * its bound key. */
static void sync_machine_menu(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    machine_item(m, IDM_CLEAR, "C&LEAR", O2_TARGET_SWITCH(O2_SW_CLEAR), 1);
    machine_item(m, IDM_RESET_CONFIG, "Reset to &CONFIG", O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG), 1);
}

static void build_menu(HWND hwnd)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU fuji = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

    AppendMenuA(machine, MF_STRING, IDM_OPEN, "&Open Cartridge...\tCtrl+O");
    AppendMenuA(machine, MF_STRING, IDM_EJECT, "&Eject Cartridge");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_SD, "&Import Cartridge to SD...");
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_BIOS, "Import &BIOS...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_CLEAR, "C&LEAR");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "Se&ttings...\tCtrl+,");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenuA(view, MF_STRING, IDM_KEYPAD, "&Keypad\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING | (g_aspect ? MF_CHECKED : 0), IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(view, MF_STRING | (g_smooth ? MF_CHECKED : 0), IDM_SMOOTH, "&Smooth Scaling");
    AppendMenuA(view, MF_STRING, IDM_FULLSCREEN, "&Fullscreen\tF11");

    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_CONFIG, "&Web UI");
    AppendMenuA(fuji, MF_STRING, IDM_FUJINET_LOG, "Console &Log");

    AppendMenuA(help, MF_STRING, IDM_ABOUT, "&About " APP_TITLE);

    AppendMenuA(bar, MF_POPUP, (UINT_PTR)machine, "&Machine");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)view, "&View");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)fuji, "&FujiNet");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    SetMenu(hwnd, bar);
    sync_machine_menu();
}

static void open_cart(const char *path)
{
    if (o2session_load_cart(g_session, path) != 0)
        MessageBoxA(g_hwnd, o2session_last_error(g_session), "Could not open",
                    MB_ICONWARNING | MB_OK);
    update_status();
}

static void settings_bios_imported(void);

/* Import BIOS: the session's message always says what happened -- which
 * image it was, whether the console can run it yet, the custom-CRC warning.
 * Returns 1 when the console's system ROM is now there. */
static int import_bios(HWND owner, const char *path)
{
    char msg[512];
    const int idx = o2session_import_bios(g_session, path, msg, sizeof msg);
    MessageBoxA(owner, msg[0] ? msg : o2session_last_error(g_session), "Import BIOS",
                (idx < 0 ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_OK);
    settings_bios_imported();
    return idx >= 0 && o2session_have_bios(g_session);
}

/* A dropped file, or one named on the command line: a BIOS image is
 * imported, a cartridge opened, anything else copied to FujiNet's SD
 * folder for CONFIG to mount. */
static void load_media(const char *path)
{
    char dest[1024];
    if (o2session_media_is_bios(path)) { import_bios(g_hwnd, path); return; }
    if (o2session_media_is_cartridge(path)) { open_cart(path); return; }
    if (o2session_import_media(g_session, path, dest, sizeof dest) != 0) {
        MessageBoxA(g_hwnd, o2session_last_error(g_session), "Import failed",
                    MB_ICONWARNING | MB_OK);
        return;
    }
    MessageBoxA(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.",
                "Imported", MB_ICONINFORMATION | MB_OK);
}

static int pick_cart(const char *title, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Odyssey 2 cartridges (*.st2;*.bin;*.rom)\0*.st2;*.bin;*.rom\0"
                      "All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* The user's own dump of the BIOS: the 2K set, the 1K system ROM, or one of
 * MAME's four 512-byte ROMs. */
static int pick_bios(HWND owner, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Odyssey 2 BIOS images (*.bin;*.rom;*.ic11;*.ic12;*.ic13;*.ic14)\0"
                      "*.bin;*.rom;*.ic11;*.ic12;*.ic13;*.ic14\0"
                      "All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = "Import BIOS";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

/* The stand-in met a game (CONFIG booted one from the network with no BIOS
 * imported): say so once per occurrence, and offer the import, after which
 * Reset to CONFIG boots the console on the real BIOS. Polled from the
 * status timers; the latch keeps the alert from coming back while it is
 * still up (MessageBox runs a modal loop, and the timers keep firing). */
static void poll_needs_bios(void)
{
    const int need = o2session_needs_bios(g_session) != 0;
    char path[MAX_PATH];
    if (!need) { g_bios_alert = 0; return; }
    if (g_bios_alert) return;
    g_bios_alert = 1;
    if (MessageBoxA(g_hwnd,
                    "This cartridge needs the Magnavox Odyssey 2 BIOS.\n\n"
                    "Odyssey 2 games are written in the BIOS's byte-code, so only the real BIOS "
                    "runs them; CONFIG and FujiNet apps run without it.\n\n"
                    "Import your own dump of the BIOS now?",
                    "BIOS needed", MB_ICONINFORMATION | MB_YESNO) != IDYES)
        return;
    if (pick_bios(g_hwnd, path, sizeof path) && import_bios(g_hwnd, path)) {
        o2session_reset_to_config(g_session);
        update_status();
    }
}

/* ---- settings window -------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The picture, the volume,
 * the sticks and the gamepad assignments apply live; the machine (which BIOS
 * runs) and the host options are applied when the window closes, by one
 * power cycle or restart for the lot.
 */

static HWND g_settings_window;
static int g_settings_dirty;
static HWND g_pad_list, g_pad_port, g_bios_status;

static const char *aspect_name(int i)
{
    static const char *const names[] = { "TV (4:3)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static void settings_apply_checkbox(HWND hwnd, int id, const char *key, int def)
{
    int on = SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (o2session_get_int(g_session, key, def) != on) {
        o2session_set_int(g_session, key, on);
        g_settings_dirty = 1;
    }
}

/* What is imported, in the words of the import: the whole 2K set, the 1K
 * system ROM, or MAME's 512-byte ROMs (the console runs ic11 and ic12). */
static void bios_status_text(char *out, int outsz)
{
    const int set = o2session_bios_available(g_session, 0);
    const int sys = o2session_bios_available(g_session, 1);
    const int ic11 = o2session_bios_available(g_session, 2);
    const int ic12 = o2session_bios_available(g_session, 3);
    if (set) snprintf(out, (size_t)outsz, "Imported (2K set)");
    else if (sys) snprintf(out, (size_t)outsz, "Imported (1K system ROM)");
    else if (ic11 && ic12) snprintf(out, (size_t)outsz, "Imported (MAME's 512-byte ROMs)");
    else if (ic11 || ic12)
        snprintf(out, (size_t)outsz, "Partly imported \xe2\x80\x94 the console needs both "
                                     "84932.ic11 and 84933.ic12");
    else
        snprintf(out, (size_t)outsz, "Not imported \xe2\x80\x94 CONFIG and FujiNet apps run without it; "
                                     "games need it");
}

/* An import from the menu or the window's own button: the status line
 * follows. */
static void settings_bios_imported(void)
{
    char line[200];
    if (!g_settings_window || !g_bios_status) return;
    bios_status_text(line, sizeof line);
    set_text_utf8(g_bios_status, line, 0);
}

static void refresh_pad_list(void)
{
    int i, n, sel;
    if (!g_pad_list) return;
    sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
    SendMessageA(g_pad_list, LB_RESETCONTENT, 0, 0);
    n = o2session_gamepad_count(g_session);
    if (n == 0) {
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)"(no gamepads connected)");
    }
    for (i = 0; i < n; i++) {
        char name[128], line[200];
        int eff = o2session_gamepad_effective_port(g_session, i);
        o2session_gamepad_name(g_session, i, name, sizeof name);
        if (eff >= 0)
            snprintf(line, sizeof line, "%s  [keypad %c%s]", name, 'A' + eff,
                     o2session_gamepad_assignment(g_session, i) < 0 ? ", automatic" : "");
        else
            snprintf(line, sizeof line, "%s  [unused]", name);
        SendMessageA(g_pad_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < n) SendMessageA(g_pad_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void open_web_ui(HWND owner)
{
    if (o2session_fujinet_external(g_session)) {
        MessageBoxA(owner, "FujiNet is external: use the web UI of the instance this app dials.",
                    "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!o2session_fujinet_running(g_session)) {
        MessageBoxA(owner, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ShellExecuteA(owner, "open", o2session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SET_IMPORT_BIOS: {
            char path[MAX_PATH];
            const int had = o2session_have_bios(g_session);
            if (!pick_bios(hwnd, path, sizeof path)) return 0;
            import_bios(hwnd, path);
            /* A BIOS the console can run now boots at the power cycle this
             * window ends with: a machine change like any other here. */
            if (o2session_have_bios(g_session) != had) g_settings_dirty = 1;
            return 0;
        }
        case IDC_SET_USE_BIOS: settings_apply_checkbox(hwnd, IDC_SET_USE_BIOS, "use_bios", 1); return 0;
        case IDC_SET_ASPECT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                HMENU m = GetMenu(g_hwnd);
                /* row 0 is the TV, row 1 square pixels */
                g_aspect = SendMessageA(GetDlgItem(hwnd, IDC_SET_ASPECT), CB_GETCURSEL, 0, 0) == 0;
                o2session_set_int(g_session, "aspect", g_aspect ? 0 : 1);
                if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
                InvalidateRect(g_hwnd, NULL, TRUE);
            }
            return 0;
        case IDC_SET_AN_JOY:
            o2session_set_analog(g_session,
                SendMessageA(GetDlgItem(hwnd, IDC_SET_AN_JOY), BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_PAD_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_pad_port, CB_SETCURSEL,
                             (WPARAM)(o2session_gamepad_assignment(g_session, sel) + 1), 0);
            }
            return 0;
        case IDC_SET_PAD_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageA(g_pad_list, LB_GETCURSEL, 0, 0);
                int choice = (int)SendMessageA(g_pad_port, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < o2session_gamepad_count(g_session))
                    o2session_gamepad_assign(g_session, sel, choice - 1);
                refresh_pad_list();
                o2_keypad_window_gamepads_changed();
            }
            return 0;
        case IDC_SET_FUJINET: settings_apply_checkbox(hwnd, IDC_SET_FUJINET, "enable_fujinet", 1); return 0;
        case IDC_SET_EXTERNAL:
            settings_apply_checkbox(hwnd, IDC_SET_EXTERNAL, "fujinet_external", 0);
            EnableWindow(GetDlgItem(hwnd, IDC_SET_EXT_ADDR),
                         SendDlgItemMessageA(hwnd, IDC_SET_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        case IDC_SET_WEBUI: open_web_ui(hwnd); return 0;
        case IDC_SET_AUDIO: settings_apply_checkbox(hwnd, IDC_SET_AUDIO, "enable_audio", 1); return 0;
        case IDC_SET_GAMEPAD: settings_apply_checkbox(hwnd, IDC_SET_GAMEPAD, "enable_gamepad", 1); return 0;
        default: break;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_SET_VOLUME))
            o2session_set_volume(g_session, (int)SendMessageA((HWND)lp, TBM_GETPOS, 0, 0));
        return 0;
    case WM_TIMER:
        if (wp == IDT_SETTINGS_PADS) {
            static unsigned seen;
            unsigned gen = o2session_gamepad_generation(g_session);
            if (gen != seen) { seen = gen; refresh_pad_list(); }
        }
        return 0;
    case WM_CLOSE: {
        /* The external address is saved here, and only if it is one. */
        char addr[96], host[64];
        const char *old;
        int port;
        {
            char *first = addr, *last;
            GetDlgItemTextA(hwnd, IDC_SET_EXT_ADDR, addr, sizeof addr);
            while (*first == ' ' || *first == '\t') first++;
            memmove(addr, first, strlen(first) + 1);
            last = addr + strlen(addr);
            while (last > addr && (last[-1] == ' ' || last[-1] == '\t')) *--last = '\0';
        }
        if (SendDlgItemMessageA(hwnd, IDC_SET_EXTERNAL, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            o2session_parse_hostport(addr, host, sizeof host, &port) != 0) {
            MessageBoxA(hwnd, "That is not an address:port.\n\nExample: 127.0.0.1:11520",
                        "External FujiNet", MB_ICONWARNING);
            SetFocus(GetDlgItem(hwnd, IDC_SET_EXT_ADDR));
            return 0;
        }
        old = o2session_get_str(g_session, "fujinet_external_addr", "");
        if (o2session_parse_hostport(addr, host, sizeof host, &port) == 0 && strcmp(old, addr) != 0) {
            o2session_set_str(g_session, "fujinet_external_addr", addr);
            g_settings_dirty = 1;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, IDT_SETTINGS_PADS);
        g_settings_window = NULL;
        g_pad_list = g_pad_port = g_bios_status = NULL;
        if (g_settings_dirty) {
            g_settings_dirty = 0;
            restart_session();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND settings_checkbox(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w, int checked)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, 22, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_label(HWND parent, HINSTANCE inst, const char *text, int x, int y, int w, int h)
{
    HWND l = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent,
                             NULL, inst, NULL);
    SendMessageA(l, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return l;
}

/* A section heading, in bold like the Keypad window's. */
static HWND settings_heading(HWND parent, HINSTANCE inst, HFONT bold, const char *text, int y)
{
    HWND l = settings_label(parent, inst, text, 16, y, 440, 18);
    if (bold) SendMessageA(l, WM_SETFONT, (WPARAM)bold, TRUE);
    return l;
}

static HWND settings_button(HWND parent, HINSTANCE inst, const char *text, int id, int x, int y, int w)
{
    HWND h = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             x, y, w, 26, parent, (HMENU)(INT_PTR)id, inst, NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static HWND settings_combo(HWND parent, HINSTANCE inst, int id, int x, int y, int w,
                           const char *(*names)(int), int sel)
{
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, 200, parent, (HMENU)(INT_PTR)id, inst, NULL);
    int i;
    if (names)
        for (i = 0; names(i); i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)names(i));
    SendMessageA(c, CB_SETCURSEL, (WPARAM)sel, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void show_settings(HINSTANCE inst)
{
    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    static HFONT bold;
    HWND h;
    RECT want;
    char line[200];
    int y = 12;

    if (g_settings_window) { SetForegroundWindow(g_settings_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            LOGFONTA lf;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = settings_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "O2SettingsWindow";
            RegisterClassA(&wc);
            GetObjectA(font, sizeof lf, &lf);
            lf.lfWeight = FW_BOLD;
            bold = CreateFontIndirectA(&lf);
            registered = 1;
        }
    }
    g_settings_window = CreateWindowA("O2SettingsWindow", "Settings", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 700, NULL, NULL, inst, NULL);

    settings_heading(g_settings_window, inst, bold, "Machine", y); y += 20;
    settings_label(g_settings_window, inst, "Applied by a power cycle when this window closes.",
                   16, y, 440, 18); y += 26;
    settings_label(g_settings_window, inst, "BIOS:", 16, y, 60, 18);
    g_bios_status = settings_label(g_settings_window, inst, "", 80, y, 264, 32);
    settings_bios_imported();
    settings_button(g_settings_window, inst, "Import BIOS...", IDC_SET_IMPORT_BIOS, 352, y + 2, 100);
    y += 38;
    settings_checkbox(g_settings_window, inst, "Use the imported BIOS (off: always the stand-in)", IDC_SET_USE_BIOS,
                      80, y, 370, o2session_get_int(g_session, "use_bios", 1));
    y += 34;

    settings_heading(g_settings_window, inst, bold, "Display", y); y += 22;
    settings_label(g_settings_window, inst, "Picture:", 16, y + 3, 60, 18);
    settings_combo(g_settings_window, inst, IDC_SET_ASPECT, 80, y, 220, aspect_name, g_aspect ? 0 : 1);
    y += 40;

    settings_heading(g_settings_window, inst, bold, "Audio", y); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable audio (applied when this window closes)", IDC_SET_AUDIO,
                      16, y, 400, o2session_get_int(g_session, "enable_audio", 1));
    y += 28;
    settings_label(g_settings_window, inst, "Volume:", 16, y + 3, 60, 18);
    h = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                        80, y, 300, 28, g_settings_window, (HMENU)(INT_PTR)IDC_SET_VOLUME, inst, NULL);
    SendMessageA(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageA(h, TBM_SETTICFREQ, 10, 0);
    SendMessageA(h, TBM_SETPOS, TRUE, (LPARAM)o2session_get_int(g_session, "volume", 100));
    y += 40;

    settings_heading(g_settings_window, inst, bold, "Input", y); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable gamepads (applied when this window closes)", IDC_SET_GAMEPAD,
                      16, y, 400, o2session_get_int(g_session, "enable_gamepad", 1));
    y += 26;
    settings_checkbox(g_settings_window, inst, "Analog sticks steer too", IDC_SET_AN_JOY, 16, y, 300,
                      o2session_get_int(g_session, "analog_joystick", 1));
    y += 30;
    settings_label(g_settings_window, inst, "Gamepads (select one, then choose its keypad):", 16, y, 440, 18); y += 22;
    g_pad_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 16, y, 316, 70,
        g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_LIST, inst, NULL);
    SendMessageA(g_pad_list, WM_SETFONT, (WPARAM)font, TRUE);
    g_pad_port = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        342, y, 110, 200, g_settings_window, (HMENU)(INT_PTR)IDC_SET_PAD_PORT, inst, NULL);
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Automatic");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Keypad A");
    SendMessageA(g_pad_port, CB_ADDSTRING, 0, (LPARAM)"Keypad B");
    SendMessageA(g_pad_port, CB_SETCURSEL, 0, 0);
    SendMessageA(g_pad_port, WM_SETFONT, (WPARAM)font, TRUE);
    refresh_pad_list();
    y += 80;

    settings_heading(g_settings_window, inst, bold, "FujiNet", y); y += 22;
    settings_checkbox(g_settings_window, inst, "Enable FujiNet (applied when this window closes)", IDC_SET_FUJINET,
                      16, y, 400, o2session_get_int(g_session, "enable_fujinet", 1));
    y += 28;
    settings_checkbox(g_settings_window, inst, "Use external FujiNet (applied when this window closes)", IDC_SET_EXTERNAL,
                      16, y, 440, o2session_get_int(g_session, "fujinet_external", 0));
    y += 28;
    settings_label(g_settings_window, inst, "Address:port:", 16, y + 3, 90, 18);
    {
        HWND edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", o2session_get_str(g_session, "fujinet_external_addr", ""),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 110, y, 220, 22,
                                    g_settings_window, (HMENU)(INT_PTR)IDC_SET_EXT_ADDR, inst, NULL);
        SendMessageA(edit, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageA(edit, EM_SETCUEBANNER, FALSE, (LPARAM)L"127.0.0.1:11520");
        EnableWindow(edit, o2session_get_int(g_session, "fujinet_external", 0) != 0);
    }
    y += 30;
    if (o2session_fujinet_external(g_session))
        snprintf(line, sizeof line, "Web UI: FujiNet is external");
    else
        snprintf(line, sizeof line, "Web UI: %s", o2session_fujinet_webui_url(g_session));
    settings_label(g_settings_window, inst, line, 16, y + 5, 320, 18);
    settings_button(g_settings_window, inst, "Open", IDC_SET_WEBUI, 352, y, 100);
    y += 38;

    /* Exactly tall enough for what is in it. */
    SetRect(&want, 0, 0, 468, y);
    AdjustWindowRectEx(&want, style, FALSE, 0);
    SetWindowPos(g_settings_window, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    SetTimer(g_settings_window, IDT_SETTINGS_PADS, 500, NULL);
    ShowWindow(g_settings_window, SW_SHOW);
}

/* ---- FujiNet console log --------------------------------------------------- */

static HWND g_log_window;
static HWND g_log_edit;

static void log_refresh(void)
{
    static char buf[128 * 1024];
    int n;
    DWORD first, last, lines;
    if (!g_log_edit) return;

    first = (DWORD)SendMessageA(g_log_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    lines = (DWORD)SendMessageA(g_log_edit, EM_GETLINECOUNT, 0, 0);
    {
        RECT rc;
        HDC dc = GetDC(g_log_edit);
        TEXTMETRICA tm;
        int visible = 1;
        GetClientRect(g_log_edit, &rc);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, (HGDIOBJ)SendMessageA(g_log_edit, WM_GETFONT, 0, 0));
            if (GetTextMetricsA(dc, &tm) && tm.tmHeight > 0)
                visible = (rc.bottom - rc.top) / tm.tmHeight;
            SelectObject(dc, of);
            ReleaseDC(g_log_edit, dc);
        }
        last = first + (DWORD)(visible > 0 ? visible : 1);
    }
    n = o2session_fujinet_copy_log(g_session, buf, sizeof buf);
    SetWindowTextA(g_log_edit, n > 0 ? buf : "(no FujiNet output yet)");
    if (last >= lines) {
        int len = GetWindowTextLengthA(g_log_edit);
        SendMessageA(g_log_edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageA(g_log_edit, EM_SCROLLCARET, 0, 0);
    }
}

static LRESULT CALLBACK log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_log_edit) MoveWindow(g_log_edit, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_LOG_REFRESH) log_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_LOG_REFRESH);
        g_log_window = NULL;
        g_log_edit = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void show_fujinet_log(HINSTANCE inst)
{
    RECT rc;
    if (o2session_fujinet_external(g_session)) {
        MessageBoxA(g_hwnd, "FujiNet is external: its console output is in the process that is running it.",
                    "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (g_log_window) { SetForegroundWindow(g_log_window); return; }
    {
        static int registered;
        if (!registered) {
            WNDCLASSA wc;
            memset(&wc, 0, sizeof wc);
            wc.lpfnWndProc = log_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "O2FujiNetLogWindow";
            RegisterClassA(&wc);
            registered = 1;
        }
    }
    g_log_window = CreateWindowA("O2FujiNetLogWindow", "FujiNet Console Log", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 860, 600, NULL, NULL, inst, NULL);
    GetClientRect(g_log_window, &rc);
    g_log_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, rc.right - rc.left, rc.bottom - rc.top, g_log_window, (HMENU)(INT_PTR)IDC_LOG_EDIT, inst, NULL);
    SendMessageA(g_log_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    SetTimer(g_log_window, IDT_LOG_REFRESH, 1000, NULL);
    log_refresh();
    ShowWindow(g_log_window, SW_SHOW);
}

/* ---- window --------------------------------------------------------------- */

static void toggle_fullscreen(HWND hwnd)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        g_placement.length = sizeof g_placement;
        GetWindowPlacement(hwnd, &g_placement);
        if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetMenu(hwnd, NULL);
            ShowWindow(g_statusbar, SW_HIDE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = 0;
    }
    InvalidateRect(hwnd, NULL, TRUE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_INITMENUPOPUP:
        sync_machine_menu();
        break;
    case WM_ENTERMENULOOP:
        /* The menu takes the keyboard: whatever was held lets go now
         * rather than staying down until the key is pressed again. */
        o2session_release_all(g_session);
        memset(g_sysact_down, 0, sizeof g_sysact_down);
        break;

    /* WM_SYSKEY* as well: Left Alt is keypad A's 0 by default (MAME's), and
     * Windows reports Alt -- and every key pressed while it is down -- as a
     * system key. A system key that drives the console is the console's;
     * the rest (Alt+F4, the menu mnemonics, Alt+Esc) stay Windows'. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_F9) { o2_keypad_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { o2_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_SYSKEYDOWN) {
            ks = o2_keysym_from_msg(wp, lp);
            if (wp != VK_F4 && ks && o2session_key_sysaction(g_session, ks) < 0 &&
                o2session_key(g_session, ks, 1))
                return 0;
            break;
        }
        if (ctrl) {
            if (wp == 'O') { PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0); return 0; }
            if (wp == VK_OEM_COMMA) { PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0); return 0; }
            break;
        }
        if (lp & (1 << 30)) return 0;  /* auto-repeat: the key is already held */
        ks = o2_keysym_from_msg(wp, lp);
        if (!ks) break;
        /* The session's own actions (Escape) first; then the console: the
         * keypads and CLEAR (F3), through the bindings like every other
         * key. */
        sa = o2session_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!g_sysact_down[sa]) {
                g_sysact_down[sa] = 1;
                run_sysaction(sa);
                update_status();
            }
            return 0;
        }
        if (o2session_key(g_session, ks, 1)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks = o2_keysym_from_msg(wp, lp);
        int sa;
        if (!ks) break;
        sa = o2session_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        /* A bound Alt's release stops here, so it does not also put the
         * menu bar into keyboard mode. */
        if (o2session_key(g_session, ks, 0)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            o2session_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) update_status();
        else if (wp == IDT_SYSACT) {
            int sa;
            while (o2session_sysaction_take(g_session, &sa)) {
                run_sysaction(sa);
                update_status();
            }
            poll_gamepads();
            poll_needs_bios();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN: {
            char path[MAX_PATH];
            if (pick_cart("Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: o2session_eject(g_session); update_status(); return 0;
        case IDM_IMPORT_SD: {
            char path[MAX_PATH], dest[1024], note[1200];
            if (!pick_cart("Import Cartridge to SD", path, sizeof path)) return 0;
            if (o2session_import_cart_to_sd(g_session, path, dest, sizeof dest) != 0) {
                MessageBoxA(hwnd, o2session_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
                return 0;
            }
            snprintf(note, sizeof note, "%s is on the SD host. Boot it from the CONFIG client.", base_name(dest));
            MessageBoxA(hwnd, note, "Imported", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_IMPORT_BIOS: {
            char path[MAX_PATH];
            if (pick_bios(hwnd, path, sizeof path)) import_bios(hwnd, path);
            return 0;
        }
        /* CLEAR from the menu: the session holds it for a few frames, as a
         * finger would -- the console samples it, so an instant's press
         * would fall between two looks. */
        case IDM_CLEAR: o2session_clear(g_session); update_status(); return 0;
        case IDM_RESET_CONFIG: run_sysaction(O2_SYSACT_RESET_CONFIG); update_status(); return 0;
        case IDM_KEYPAD: o2_keypad_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: o2_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT: {
            HMENU m = GetMenu(hwnd);
            g_aspect = !g_aspect;
            if (m) CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
            o2session_set_int(g_session, "aspect", g_aspect ? 0 : 1);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SMOOTH: {
            HMENU m = GetMenu(hwnd);
            g_smooth = !g_smooth;
            if (m) CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
            o2session_set_int(g_session, "smooth", g_smooth);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case IDM_SETTINGS: show_settings((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_LOG: show_fujinet_log((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE)); return 0;
        case IDM_FUJINET_CONFIG: open_web_ui(hwnd); return 0;
        case IDM_ABOUT: {
            wchar_t text[1024];
            MultiByteToWideChar(CP_UTF8, 0,
                APP_TITLE " " O2_VERSION_STRING "\n"
                "Magnavox Odyssey 2 with built-in FujiNet\n\n"
                "The emulator is MAME's Magnavox Odyssey 2, CDP1802 and CDP1861\n"
                "(Curt Coder, BSD-3-Clause), transposed to C. The FujiNet\n"
                "firmware and the cartridge: Thomas Cherryhomes and the\n"
                "FujiNet contributors.\n\n"
                "Copyright \xc2\xa9 2026 Thomas Cherryhomes \xe2\x80\x94 GPL-3.0-or-later\n"
                "https://fujinet.online/\n"
                "Issues: https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues",
                -1, text, 1024);
            MessageBoxW(hwnd, text, L"About " APP_TITLE_W, MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        char path[MAX_PATH];
        HDROP drop = (HDROP)wp;
        if (DragQueryFileA(drop, 0, path, sizeof path)) load_media(path);
        DragFinish(drop);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSEX wc;
    MSG msg;
    o2session_start_opts opts;
    RECT want;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    int parts[2] = { DOT_PART_W, -1 };
    char media[MAX_PATH];
    const char *later = NULL;   /* a non-cartridge path from the command line */
    (void)prev;

    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_fb_lock);

    g_session = o2session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_fb = calloc((size_t)O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT, sizeof *g_fb);
    if (!g_fb) return 1;
    g_accent_brush = CreateSolidBrush(RGB((O2SESSION_ACCENT_RGB >> 16) & 0xff,
                                          (O2SESSION_ACCENT_RGB >> 8) & 0xff,
                                          O2SESSION_ACCENT_RGB & 0xff));

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof g_bmi.bmiHeader;
    g_bmi.bmiHeader.biWidth = O2SESSION_FB_WIDTH;
    g_bmi.bmiHeader.biHeight = -O2SESSION_FB_HEIGHT;
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;

    /* "aspect": 0 (the default) the television's pixel aspect, 1 square --
     * the family's meaning, shared with the other frontends */
    g_aspect = o2session_get_int(g_session, "aspect", 0) == 0;
    g_smooth = o2session_get_int(g_session, "smooth", 0);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN_CLASS;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassEx(&wc);

    /* Three times the console's picture in height, at the chosen shape
     * (984 x 738 at the TV's 4:3), plus chrome. */
    SetRect(&want, 0, 0, (int)(O2SESSION_FB_HEIGHT * 3 * picture_aspect(g_aspect) + 0.5),
            O2SESSION_FB_HEIGHT * 3);
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + 24, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)parts);
    ShowWindow(g_hwnd, show);

    o2session_default_opts(g_session, &opts);
    if (cmdline && *cmdline) {
        /* a path on the command line, quotes and all: a cartridge boots in
         * place of the remembered one; a BIOS or anything else is routed
         * like a dropped file once the machine is up */
        const char *p = cmdline;
        size_t n;
        if (*p == '"') p++;
        snprintf(media, sizeof media, "%s", p);
        n = strlen(media);
        while (n && (media[n - 1] == '"' || media[n - 1] == ' ')) media[--n] = '\0';
        if (media[0]) {
            if (!o2session_media_is_bios(media) && o2session_media_is_cartridge(media))
                opts.cart_path = media;
            else
                later = media;
        }
    }
    if (o2session_start(g_session, &opts) != 0)
        MessageBoxA(g_hwnd, o2session_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);
    if (later) load_media(later);

    g_pad_generation = o2session_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_status();

    if (env_on("O2_OPEN_KEYPAD")) o2_keypad_window_toggle(g_hwnd, g_session);
    if (env_on("O2_OPEN_DEBUGGER")) o2_debugger_show(g_hwnd, g_session);
    if (env_on("O2_OPEN_SETTINGS")) show_settings(inst);

    g_present_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (o2_debugger_pretranslate(&msg)) continue;
        if (o2_keypad_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    InterlockedExchange(&g_running, 0);
    if (g_present_thread) {
        WaitForSingleObject(g_present_thread, 2000);
        CloseHandle(g_present_thread);
    }
    o2session_stop(g_session);
    o2session_free(g_session);
    free(g_fb);
    DeleteObject(g_accent_brush);
    DeleteCriticalSection(&g_fb_lock);
    return (int)msg.wParam;
}

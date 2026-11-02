/*
 * FujiNet Go Odyssey 2 -- the Windows (Win32) frontend.
 *
 * No toolkit: a plain window, a menu bar and a status bar, with the picture a
 * child window drawn by display.c (Direct3D 11, GDI as the fallback). That
 * keeps the artifact a folder you copy rather than a runtime hunt.
 *
 * Three Windows-specific things are load bearing:
 *
 *   The display's render thread is this platform's frame clock: it feeds each
 *   vblank to the core (display.c). There is no GdkFrameClock here, and a
 *   plain timer would beat against the panel.
 *
 *   WM_ACTIVATE releases every held key. Alt-tabbing away mid-game and coming
 *   back to a stick still pushed is the classic symptom of not doing this, and
 *   Windows is where it happens most, because the WM eats the key-up.
 *
 *   The Odyssey 2 has no Alt key, so a system key (WM_SYSKEYDOWN) is passed to
 *   the session and swallowed only when it is bound: Alt+F4, the menu
 *   mnemonics and Alt+Esc stay Windows'.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2session.h"
#include "controllers/controllers_window.h"
#include "debugger/dbg_window.h"
#include "display.h"
#include "fujilog.h"
#include "import.h"
#include "key_forward.h"
#include "prefs.h"
#include "resource.h"
#include "win_common.h"

#define WIN_CLASS "FujiNetGoO2"
#define APP_TITLE "FujiNet Go Odyssey 2"
#define APP_TITLE_W L"FujiNet Go Odyssey 2"
#define INSTANCE_MUTEX "Local\\FujiNetGoO2.Instance"

/* How long a notice (a gamepad hot-plug, Preferences applying) stays in the
 * status bar. */
#define NOTICE_MS 4000

/* The status bar's first part: the FujiNet link dot. */
#define DOT_PART_W 22

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static o2session *g_session;
static HWND g_hwnd, g_display, g_statusbar, g_banner_text, g_banner_import;
static int g_aspect = 1, g_smooth = 0, g_fullscreen = 0;    /* the TV's 4:3 on */
static int g_banner_shown;
static WINDOWPLACEMENT g_placement;
static int g_sysact_down[O2_SYSACT_COUNT];
static unsigned g_pad_generation;
static char g_notice[160];
static DWORD g_notice_until;
static HBRUSH g_accent_brush;
static int g_bios_alert;         /* needs_bios as last seen: alert once per rise */

static void update_status(void);
static void sync_menu_checks(void);
static void layout_children(void);

/* ---- small helpers -------------------------------------------------------- */

static int S(int v) { return o2_scale(v, o2_window_dpi(g_hwnd)); }

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '\\');
    const char *t = strrchr(p, '/');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : p;
}

static int env_on(const char *name)
{
    const char *env = getenv(name);
    return env && *env && *env != '0';
}

static void notice(const char *text)
{
    snprintf(g_notice, sizeof g_notice, "%s", text);
    g_notice_until = GetTickCount() + NOTICE_MS;
    update_status();
}

/* A question with two buttons of our own words. Task dialogs need the
 * version 6 common controls (the manifest asks for them); looked up rather
 * than imported, so a process without them still gets a plain Yes/No. 1 for
 * the first button. */
static int ask(HWND owner, const char *text, const char *title, const char *yes, const char *no)
{
    typedef HRESULT (WINAPI *task_fn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);
    HMODULE cc = LoadLibraryA("comctl32.dll");
    task_fn fn = cc ? (task_fn)(void (*)(void))GetProcAddress(cc, "TaskDialogIndirect") : NULL;
    if (fn) {
        wchar_t *wt = o2_wide_dup(text), *wc = o2_wide_dup(title);
        wchar_t *wy = o2_wide_dup(yes), *wn = o2_wide_dup(no);
        TASKDIALOG_BUTTON buttons[2];
        TASKDIALOGCONFIG cfg;
        int chosen = 0;
        HRESULT hr;
        memset(&cfg, 0, sizeof cfg);
        buttons[0].nButtonID = 1001;
        buttons[0].pszButtonText = wy;
        buttons[1].nButtonID = 1002;
        buttons[1].pszButtonText = wn;
        cfg.cbSize = sizeof cfg;
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        cfg.pszWindowTitle = wc;
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszContent = wt;
        cfg.cButtons = 2;
        cfg.pButtons = buttons;
        cfg.nDefaultButton = 1001;
        hr = fn(&cfg, &chosen, NULL, NULL);
        free(wt); free(wc); free(wy); free(wn);
        if (SUCCEEDED(hr))
            return chosen == 1001;
    }
    return o2_message(owner, text, title, MB_ICONINFORMATION | MB_YESNO) == IDYES;
}

/* ---- layout: banner, picture, status bar --------------------------------------- */

static int statusbar_height(void)
{
    RECT r;
    if (!g_statusbar || !IsWindowVisible(g_statusbar)) return 0;
    GetWindowRect(g_statusbar, &r);
    return r.bottom - r.top;
}

/* The banner over the picture, the picture above the status bar. */
static void layout_children(void)
{
    RECT rc;
    int top = 0, bottom;
    if (!g_display) return;
    GetClientRect(g_hwnd, &rc);
    bottom = rc.bottom - statusbar_height();
    if (bottom < 0) bottom = 0;
    if (g_banner_shown && !g_fullscreen) {
        const int bh = S(34), bw = S(150);
        MoveWindow(g_banner_text, S(10), 0, rc.right - bw - S(24) > 40 ? rc.right - bw - S(24) : 40, bh, TRUE);
        MoveWindow(g_banner_import, rc.right - bw - S(8), S(4), bw, bh - S(8), TRUE);
        top = bh;
    }
    MoveWindow(g_display, 0, top, rc.right, bottom > top ? bottom - top : 0, TRUE);
}

/* What is missing for the console as it runs now: the 8244's character set
 * (text is blank), or the speech ROMs. A game that needs the BIOS is an
 * alert, not a banner. */
static void update_banner(void)
{
    const unsigned missing = o2session_missing(g_session);
    const char *text = NULL;
    int show;
    if (missing & O2_MISSING_FONT)
        text = "Character set not imported \xe2\x80\x94 text will be blank. Import BIOS...";
    else if (missing & O2_MISSING_VOICE)
        text = "The Voice's speech ROMs are not imported \xe2\x80\x94 speech is silent. Import BIOS...";
    show = text != NULL;
    if (text) {
        wchar_t cur[256], *w = o2_wide_dup(text);
        GetWindowTextW(g_banner_text, cur, 256);
        if (w && wcscmp(cur, w) != 0) SetWindowTextW(g_banner_text, w);
        free(w);
    }
    if (show == g_banner_shown) return;
    g_banner_shown = show;
    ShowWindow(g_banner_text, show && !g_fullscreen ? SW_SHOW : SW_HIDE);
    ShowWindow(g_banner_import, show && !g_fullscreen ? SW_SHOW : SW_HIDE);
    layout_children();
}

/* ---- the machine ----------------------------------------------------------------- */

/* Power-cycle with the current settings: the session decides whether that is
 * a power cycle of the console alone or a restart of FujiNet, audio and the
 * gamepads with it. */
static void restart_session(void)
{
    if (o2session_restart(g_session) != 0)
        o2_message(g_hwnd, o2session_last_error(g_session), "Could not start", MB_ICONWARNING | MB_OK);
    o2_controllers_window_refresh();
    update_status();
    update_banner();
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

/* The ROMs changed under a console that was missing some: power-cycle it so
 * the new ones are used -- the cartridge it runs again, or CONFIG. */
static void apply_new_roms(unsigned was_missing)
{
    o2_prefs_roms_changed();
    if (was_missing) {
        const char *cart = o2session_cart_path(g_session);
        if (cart && *cart) {
            char again[1024];
            snprintf(again, sizeof again, "%s", cart);
            o2session_load_cart(g_session, again);
        } else {
            o2session_reset_to_config(g_session);
        }
    }
    o2_controllers_window_refresh();
    update_status();
    update_banner();
}

/* Import BIOS from the menu, the banner or an alert: files or a folder.
 * Returns 1 when something was imported. */
static int import_bios(HWND owner, int folder)
{
    const unsigned was_missing = o2session_missing(g_session);
    const int n = o2_import_bios(owner, g_session, folder);
    if (n > 0) apply_new_roms(was_missing);
    return n > 0;
}

/* A game or a Voice game the console cannot run yet: the session says so
 * and the message names Import BIOS when importing fixes it. */
static int import_would_fix(const char *err)
{
    return err && strstr(err, "Import BIOS") != NULL;
}

static void open_cart(const char *path)
{
    char again[1024];
    int tries;
    snprintf(again, sizeof again, "%s", path);
    for (tries = 0; tries < 3; tries++) {
        char msg[700];
        if (o2session_load_cart(g_session, again) == 0) break;
        snprintf(msg, sizeof msg, "%s", o2session_last_error(g_session));
        if (!import_would_fix(msg)) {
            o2_message(g_hwnd, msg, "Could not open", MB_ICONWARNING | MB_OK);
            break;
        }
        /* the refused cartridge is tried again once the ROMs are in */
        if (!ask(g_hwnd, msg, "BIOS needed", "Import BIOS...", "Not Now") || !import_bios(g_hwnd, 0))
            break;
    }
    update_status();
    update_banner();
}

/* A dropped file, or one named on the command line: a ROM (a BIOS, a
 * character set, a zip of them) is imported, a cartridge opened, anything
 * else copied to FujiNet's SD folder for CONFIG to mount. */
static void load_media(const char *path)
{
    char dest[1024];
    if (o2session_media_is_rom(path)) {
        const unsigned was_missing = o2session_missing(g_session);
        const char *one[1];
        one[0] = path;
        if (o2_import_roms(g_hwnd, g_session, one, 1) > 0) apply_new_roms(was_missing);
        return;
    }
    if (o2session_media_is_cartridge(path)) { open_cart(path); return; }
    if (o2session_import_media(g_session, path, dest, sizeof dest) != 0) {
        o2_message(g_hwnd, o2session_last_error(g_session), "Import failed", MB_ICONWARNING | MB_OK);
        return;
    }
    o2_message(g_hwnd, "Copied to FujiNet's SD folder. Mount it from the CONFIG client.", "Imported",
               MB_ICONINFORMATION | MB_OK);
}

/* The stand-in met a game (CONFIG booted one from the network with no BIOS
 * imported): say so once per occurrence, and offer the import, after which
 * the console boots on the real BIOS. Polled from the status timers; the
 * latch keeps the alert from coming back while it is still up (a modal loop
 * lets the timers keep firing). */
static void poll_needs_bios(void)
{
    if (!o2session_needs_bios(g_session)) { g_bios_alert = 0; return; }
    if (g_bios_alert) return;
    g_bios_alert = 1;
    if (!ask(g_hwnd,
             "This cartridge needs the Odyssey 2 BIOS.\n\n"
             "Odyssey 2 games call routines in the BIOS, so only the real one runs them; "
             "CONFIG and FujiNet apps run without it.\n\n"
             "Machine > Import BIOS... adds your own dump.",
             "BIOS needed", "Import BIOS...", "Not Now"))
        return;
    import_bios(g_hwnd, 0);
}

/* ---- status ------------------------------------------------------------------------- */

static void set_text_utf8(HWND h, const char *text, int statusbar)
{
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512);
    if (statusbar) SendMessageW(h, SB_SETTEXTW, 1, (LPARAM)w);   /* part 1: the text */
    else SetWindowTextW(h, w);
}

static void update_status(void)
{
    char title[512], st[160], line[400], type[96];
    const char *cart = o2session_cart_path(g_session);

    if (!o2session_is_running(g_session)) {
        snprintf(title, sizeof title, APP_TITLE " \xe2\x80\x94 stopped");
        snprintf(st, sizeof st, "stopped");
        type[0] = '\0';
    } else {
        o2session_cart_status(g_session, st, sizeof st);
        o2session_cart_type_name(g_session, type, sizeof type);
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
    if (g_notice[0] && GetTickCount() < g_notice_until) {
        snprintf(line, sizeof line, "%s", g_notice);
    } else {
        g_notice[0] = '\0';
        snprintf(line, sizeof line, "%s%sFujiNet: %s%s", type, type[0] ? " \xe2\x80\x94 " : "", st,
                 o2session_fujinet_external(g_session) ? " (external)"
                 : o2session_fujinet_running(g_session) ? "" : " (runtime not running)");
    }
    set_text_utf8(g_statusbar, line, 1);
}

/* The link dot: the accent while the cartridge's link to FujiNet is up, grey
 * otherwise. */
static void draw_status_dot(const DRAWITEMSTRUCT *di)
{
    const int up = di->itemData != 0;
    const int size = S(10);
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
    char text[160];
    if (gen == g_pad_generation) return;
    g_pad_generation = gen;
    if (o2session_gamepad_last_event(g_session, text, sizeof text) > 0)
        notice(text);
    o2_controllers_window_refresh();
    o2_prefs_gamepads_changed();
}

/* ---- menu ----------------------------------------------------------------------------- */

/* A menu item's label with the key it is bound to after the tab -- read from
 * the bindings each time the menu opens, so a remap in the Controllers window
 * shows here. The key is only a label: there is no accelerator table, so F1
 * and Escape reach the console through the bindings rather than being eaten by
 * the menu. */
static void bound_item(HMENU m, UINT id, const char *text, int target)
{
    char key[32], label[96];
    const o2_binding b = o2session_binding_get(g_session, target);
    key[0] = '\0';
    o2session_keysym_name(b.keysym, key, sizeof key);
    if (key[0]) snprintf(label, sizeof label, "%s\t%s", text, key);
    else snprintf(label, sizeof label, "%s", text);
    ModifyMenuA(m, id, MF_BYCOMMAND | MF_STRING, id, label);
}

/* The console's own button, RESET, with its bound key; Pause with its check. */
static void sync_machine_menu(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    bound_item(m, IDM_RESET, "&Reset", O2_TARGET_SWITCH(O2_SW_RESET));
    CheckMenuItem(m, IDM_PAUSE, MF_BYCOMMAND | (o2session_is_paused(g_session) ? MF_CHECKED : MF_UNCHECKED));
}

static void sync_menu_checks(void)
{
    HMENU m = GetMenu(g_hwnd);
    if (!m) return;
    CheckMenuItem(m, IDM_TV_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_SMOOTH, MF_BYCOMMAND | (g_smooth ? MF_CHECKED : MF_UNCHECKED));
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
    AppendMenuA(machine, MF_STRING, IDM_IMPORT_FOLDER, "Import BIOS from a &Folder...");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_PAUSE, "&Pause");
    AppendMenuA(machine, MF_STRING, IDM_RESET, "&Reset");
    AppendMenuA(machine, MF_STRING, IDM_RESET_CONFIG, "Reset to &CONFIG\tCtrl+R");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_SETTINGS, "Pre&ferences...\tCtrl+,");
    AppendMenuA(machine, MF_SEPARATOR, 0, NULL);
    AppendMenuA(machine, MF_STRING, IDM_EXIT, "E&xit");

    AppendMenuA(view, MF_STRING, IDM_CONTROLLERS, "&Controllers\tF9");
    AppendMenuA(view, MF_STRING, IDM_DEBUGGER, "&Debugger\tF12");
    AppendMenuA(view, MF_SEPARATOR, 0, NULL);
    AppendMenuA(view, MF_STRING, IDM_TV_ASPECT, "&TV Aspect");
    AppendMenuA(view, MF_STRING, IDM_SMOOTH, "&Smooth Scaling");
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
    sync_menu_checks();
}

/* ---- Preferences glue -------------------------------------------------------------------- */

/* The picture options live in the settings: Preferences writes them and says
 * so. */
static void read_display_settings(void)
{
    g_aspect = o2session_get_int(g_session, "aspect", 0) == 0;
    g_smooth = o2session_get_int(g_session, "smooth", 0) != 0;
    o2_display_set_options(g_aspect, g_smooth);
    sync_menu_checks();
}

static void prefs_roms_changed(void)
{
    update_banner();
}

static void prefs_closed(int dirty)
{
    if (!dirty) return;
    restart_session();
    notice("Preferences applied: the console was power-cycled");
}

static void show_prefs(void)
{
    static const o2_prefs_hooks hooks = { read_display_settings, prefs_roms_changed, prefs_closed };
    o2_prefs_show(g_hwnd, g_session, &hooks);
}

static void open_web_ui(HWND owner)
{
    if (o2session_fujinet_external(g_session)) {
        o2_message(owner, "FujiNet is external: use the web UI of the instance this app dials.", "FujiNet",
                   MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (!o2session_fujinet_running(g_session)) {
        o2_message(owner, "FujiNet is not running.", "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    ShellExecuteA(owner, "open", o2session_fujinet_webui_url(g_session), NULL, NULL, SW_SHOWNORMAL);
}

static void show_log(void)
{
    if (o2session_fujinet_external(g_session)) {
        o2_message(g_hwnd, "FujiNet is external: its console output is in the process that is running it.",
                   "FujiNet", MB_ICONINFORMATION | MB_OK);
        return;
    }
    o2_fujilog_show(g_hwnd, g_session);
}

static void show_about(void)
{
    o2_message(g_hwnd,
        APP_TITLE " " O2_VERSION_STRING "\n"
        "Magnavox Odyssey 2 and Philips Videopac with built-in FujiNet\n\n"
        "The emulator is MAME's Odyssey 2 / Videopac (Intel 8048, the 8244/8245 video\n"
        "chip, the EF9340/41 and The Voice's SP0256; BSD-3-Clause), transposed to C.\n"
        "The FujiNet firmware and the cartridge: Thomas Cherryhomes and the FujiNet\n"
        "contributors.\n\n"
        "Copyright \xc2\xa9 2026 Thomas Cherryhomes \xe2\x80\x94 GPL-3.0-or-later\n"
        "https://fujinet.online/\n"
        "Issues: https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues",
        "About " APP_TITLE, MB_ICONINFORMATION | MB_OK);
}

/* ---- window ---------------------------------------------------------------------------------- */

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
            ShowWindow(g_banner_text, SW_HIDE);
            ShowWindow(g_banner_import, SW_HIDE);
            g_fullscreen = 1;
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        build_menu(hwnd);
        ShowWindow(g_statusbar, SW_SHOW);
        g_fullscreen = 0;
        if (g_banner_shown) {
            ShowWindow(g_banner_text, SW_SHOW);
            ShowWindow(g_banner_import, SW_SHOW);
        }
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout_children();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_statusbar) SendMessageA(g_statusbar, WM_SIZE, 0, 0);
        layout_children();
        /* minimized: nothing to draw, so the render thread sleeps (the
         * machine, the audio and FujiNet run on) */
        o2_display_set_active(wp != SIZE_MINIMIZED);
        return 0;
    case WM_DRAWITEM:
        if (wp == IDC_STATUSBAR) { draw_status_dot((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_banner_text) {
            SetTextColor((HDC)wp, RGB(255, 255, 255));
            SetBkColor((HDC)wp, RGB((O2SESSION_ACCENT_RGB >> 16) & 0xff, (O2SESSION_ACCENT_RGB >> 8) & 0xff,
                                    O2SESSION_ACCENT_RGB & 0xff));
            return (LRESULT)g_accent_brush;
        }
        break;
    case WM_INITMENUPOPUP:
        sync_machine_menu();
        break;
    case WM_ENTERMENULOOP:
        /* The menu takes the keyboard: whatever was held lets go now rather
         * than staying down until the key is pressed again. */
        o2session_release_all(g_session);
        memset(g_sysact_down, 0, sizeof g_sysact_down);
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    /* WM_SYSKEY* as well: Windows reports Alt -- and every key pressed while
     * it is down -- as a system key. One that drives the console (a bound
     * key) is the console's; the rest (Alt+F4, the menu mnemonics, Alt+Esc)
     * stay Windows'. */
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        uint32_t ks;
        int sa;
        if (o2_controllers_map_key(msg, wp, lp)) return 0;
        if (wp == VK_F9) { o2_controllers_window_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F12) { o2_debugger_toggle(hwnd, g_session); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(hwnd); return 0; }
        if (msg == WM_KEYDOWN && o2_app_key(wp)) {
            if (wp == 'O') PostMessage(hwnd, WM_COMMAND, IDM_OPEN, 0);
            else if (wp == 'R') PostMessage(hwnd, WM_COMMAND, IDM_RESET_CONFIG, 0);
            else if (wp == VK_OEM_COMMA) PostMessage(hwnd, WM_COMMAND, IDM_SETTINGS, 0);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) {
            /* the session's own actions never fire from a system key: Alt+Esc
             * is Windows', not Reset to CONFIG */
            if (o2_forward_key(g_session, msg, wp, lp)) return 0;
            break;
        }
        ks = o2_keysym_from_msg(wp, lp);
        if (!ks) break;
        /* The session's own actions (Escape) first; then the console, through
         * the bindings like every other key. */
        sa = o2session_key_sysaction(g_session, ks);
        if (sa >= 0) {
            if (!(lp & (1 << 30)) && !g_sysact_down[sa]) {
                g_sysact_down[sa] = 1;
                run_sysaction(sa);
                update_status();
            }
            return 0;
        }
        if (o2_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: {
        uint32_t ks;
        int sa;
        if (o2_controllers_map_key(msg, wp, lp)) return 0;
        ks = o2_keysym_from_msg(wp, lp);
        if (!ks) break;
        sa = o2session_key_sysaction(g_session, ks);
        if (sa >= 0) { g_sysact_down[sa] = 0; return 0; }
        /* A bound Alt's release stops here, so it does not also put the menu
         * bar into keyboard mode. */
        if (o2_forward_key(g_session, msg, wp, lp)) return 0;
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            o2session_release_all(g_session);
            memset(g_sysact_down, 0, sizeof g_sysact_down);
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_STATUS) {
            update_status();
            update_banner();
        } else if (wp == IDT_SYSACT) {
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
            if (o2_pick_cart(hwnd, "Open Cartridge", path, sizeof path)) open_cart(path);
            return 0;
        }
        case IDM_EJECT: o2session_eject(g_session); update_status(); update_banner(); return 0;
        case IDM_IMPORT_SD: o2_import_carts(hwnd, g_session); return 0;
        case IDM_IMPORT_BIOS:
        case IDC_BANNER_IMPORT: import_bios(hwnd, 0); return 0;
        case IDM_IMPORT_FOLDER: import_bios(hwnd, 1); return 0;
        case IDM_PAUSE: o2session_set_paused(g_session, !o2session_is_paused(g_session)); return 0;
        /* the console's RESET key: the session holds it for a few frames, as
         * a finger would -- the console samples it once a frame */
        case IDM_RESET: o2session_reset(g_session); update_status(); return 0;
        case IDM_RESET_CONFIG: run_sysaction(O2_SYSACT_RESET_CONFIG); update_status(); update_banner(); return 0;
        case IDM_CONTROLLERS: o2_controllers_window_toggle(hwnd, g_session); return 0;
        case IDM_DEBUGGER: o2_debugger_toggle(hwnd, g_session); return 0;
        case IDM_FULLSCREEN: toggle_fullscreen(hwnd); return 0;
        case IDM_TV_ASPECT:
            g_aspect = !g_aspect;
            o2session_set_int(g_session, "aspect", g_aspect ? 0 : 1);
            o2_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SMOOTH:
            g_smooth = !g_smooth;
            o2session_set_int(g_session, "smooth", g_smooth);
            o2_display_set_options(g_aspect, g_smooth);
            sync_menu_checks();
            return 0;
        case IDM_SETTINGS: show_prefs(); return 0;
        case IDM_FUJINET_LOG: show_log(); return 0;
        case IDM_FUJINET_CONFIG: open_web_ui(hwnd); return 0;
        case IDM_ABOUT: show_about(); return 0;
        case IDM_EXIT: PostMessage(hwnd, WM_CLOSE, 0, 0); return 0;
        default: break;
        }
        break;

    case WM_DROPFILES: {
        const UINT n = DragQueryFileA((HDROP)wp, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        for (i = 0; i < n; i++) {
            char path[MAX_PATH];
            if (DragQueryFileA((HDROP)wp, i, path, sizeof path)) load_media(path);
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    /* a second launch hands its file to this one */
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == 1 && cd->lpData && cd->cbData > 1 && cd->cbData < MAX_PATH) {
            char path[MAX_PATH];
            memcpy(path, cd->lpData, cd->cbData);
            path[sizeof path - 1] = '\0';
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            load_media(path);
        }
        return TRUE;
    }

    case WM_CLOSE:
        o2_prefs_abandon();
        break;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_STATUS);
        KillTimer(hwnd, IDT_SYSACT);
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* The path on the command line, quotes and all. */
static int command_line_path(const char *cmdline, char *out, size_t outsz)
{
    const char *p = cmdline;
    size_t n;
    if (!cmdline || !*cmdline) return 0;
    if (*p == '"') p++;
    snprintf(out, outsz, "%s", p);
    n = strlen(out);
    while (n && (out[n - 1] == '"' || out[n - 1] == ' ')) out[--n] = '\0';
    return out[0] != '\0';
}

/* One copy of the app: a second launch brings the first forward and hands it
 * the file it was started with. 1 when this process should exit. */
static int hand_over_to_running_instance(const char *path)
{
    HANDLE mutex = CreateMutexA(NULL, FALSE, INSTANCE_MUTEX);
    HWND other;
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    other = FindWindowA(WIN_CLASS, NULL);
    if (!other)
        return 0;
    if (path && path[0]) {
        COPYDATASTRUCT cd;
        cd.dwData = 1;
        cd.cbData = (DWORD)strlen(path) + 1;
        cd.lpData = (void *)path;
        SendMessageA(other, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd);
    }
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return 1;
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
    int have_media, dpi;
    (void)prev;

    have_media = command_line_path(cmdline, media, sizeof media);
    if (hand_over_to_running_instance(have_media ? media : NULL))
        return 0;

    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);     /* the folder browser */

    g_session = o2session_new(NULL);
    if (!g_session) {
        MessageBoxA(NULL, "Could not create the session (unusable config or data directories?)",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_accent_brush = CreateSolidBrush(RGB((O2SESSION_ACCENT_RGB >> 16) & 0xff,
                                          (O2SESSION_ACCENT_RGB >> 8) & 0xff,
                                          O2SESSION_ACCENT_RGB & 0xff));

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
     * (972 x 729 at the TV's 4:3), plus chrome, at the screen's DPI. */
    dpi = o2_system_dpi();
    SetRect(&want, 0, 0,
            o2_scale((int)(O2SESSION_FB_HEIGHT * 3 * (g_aspect ? 4.0 / 3.0
                                  : (double)O2SESSION_FB_WIDTH / O2SESSION_FB_HEIGHT) + 0.5), dpi),
            o2_scale(O2SESSION_FB_HEIGHT * 3, dpi));
    AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, TRUE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WIN_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                            want.bottom - want.top + o2_scale(24, dpi), NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    build_menu(g_hwnd);
    g_statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_STATUSBAR, inst, NULL);
    {
        int scaled[2] = { o2_scale(parts[0], o2_window_dpi(g_hwnd)), -1 };
        SendMessageA(g_statusbar, SB_SETPARTS, 2, (LPARAM)scaled);
    }
    g_banner_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                                    0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_BANNER_TEXT, inst, NULL);
    g_banner_import = CreateWindowExW(0, L"BUTTON", L"Import BIOS...", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_BANNER_IMPORT, inst, NULL);
    {
        HFONT ui = o2_make_font(o2_window_dpi(g_hwnd), 0, FW_NORMAL, NULL);
        SendMessageW(g_banner_text, WM_SETFONT, (WPARAM)ui, TRUE);
        SendMessageW(g_banner_import, WM_SETFONT, (WPARAM)ui, TRUE);
    }
    g_display = o2_display_create(g_hwnd, g_session);
    o2_display_set_options(g_aspect, g_smooth);
    layout_children();
    ShowWindow(g_hwnd, show);

    o2session_default_opts(g_session, &opts);
    if (have_media) {
        /* a cartridge boots in place of the remembered one; a ROM or anything
         * else is routed like a dropped file once the machine is up */
        if (!o2session_media_is_rom(media) && o2session_media_is_cartridge(media))
            opts.cart_path = media;
        else
            later = media;
    }
    if (o2session_start(g_session, &opts) != 0)
        o2_message(g_hwnd, o2session_last_error(g_session), APP_TITLE, MB_ICONWARNING | MB_OK);
    else if (opts.cart_path && !*o2session_cart_path(g_session))
        later = media;      /* the session booted CONFIG instead: say why, and offer the import */
    if (later) load_media(later);

    g_pad_generation = o2session_gamepad_generation(g_session);
    SetTimer(g_hwnd, IDT_STATUS, 1000, NULL);
    SetTimer(g_hwnd, IDT_SYSACT, 100, NULL);
    update_status();
    update_banner();

    if (env_on("O2_OPEN_CONTROLLERS")) o2_controllers_window_toggle(g_hwnd, g_session);
    if (env_on("O2_OPEN_DEBUGGER")) o2_debugger_show(g_hwnd, g_session);
    if (env_on("O2_OPEN_SETTINGS")) show_prefs();

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        /* Before TranslateMessage, so the sub-windows' keys reach them
         * regardless of which child control has the focus. */
        if (o2_debugger_pretranslate(&msg)) continue;
        if (o2_prefs_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    o2_display_destroy();
    o2session_stop(g_session);
    o2session_free(g_session);
    DeleteObject(g_accent_brush);
    CoUninitialize();
    return (int)msg.wParam;
}

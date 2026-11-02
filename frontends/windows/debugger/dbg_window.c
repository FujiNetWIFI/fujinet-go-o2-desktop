/*
 * Debugger window (Win32) over the CDP1802 / CDP1861 debugger engine, via
 * core/include/o2debug.h. Mirrors the GTK and Qt debuggers tab for tab --
 * Prompt, CPU & RAM, Disassembly, Video, Sound & I/O, Breakpoints, Cart,
 * FujiNet -- built from plain common controls, with the disassembly, the
 * 1861's pictures, the keypad diagrams, the cartridge's page map and its
 * text engine drawn by hand.
 *
 * The engine only exists while this window is showing: showing it attaches
 * (which stops the machine, as on every sibling) and hiding it detaches,
 * which lets the machine run on at full speed. The window refreshes on a
 * timer keyed to the engine's generation counter rather than on every tick.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2debug.h"

#define DISASM_MAX 96        /* rows fetched at most; the view shows what fits */
#define MAX_BPS 128
#define HISTORY_MAX 64
#define TRACE_MAX 512        /* FujiNet packets shown */
#define GUTTER_W 18          /* the disassembly's breakpoint column */
#define COMMENT_COL 50       /* where a disassembly line's comment starts, in characters */

#define RAM_BASE 0x0800      /* the console's 512 bytes */
#define RAM_SIZE 512

#define WM_DBG_ACCEPT  (WM_APP + 2)   /* wp: control id -- Enter in an edit */
#define WM_DBG_TAB     (WM_APP + 3)   /* Tab in the prompt: complete */
#define WM_DBG_HISTORY (WM_APP + 4)   /* wp: 0 Up (older), 1 Down (newer) in the prompt */

#define TIMER_REFRESH 1

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_VIDEO, PAGE_IO, PAGE_BREAKS, PAGE_CART,
    PAGE_FUJINET, PAGE_COUNT
};

#define NREGS  21            /* R0-RF, then D P X T B */
#define NFLAGS 3             /* DF IE Q: editable */
#define NEF    4             /* EF1-EF4: what the CPU sees, read-only */
#define NSAVE  6

enum {
    IDC_RUN = 1000, IDC_STEP, IDC_OVER, IDC_OUT, IDC_SCAN, IDC_FRAME,
    IDC_STATUS, IDC_TABS,
    IDC_PROMPT_OUT, IDC_PROMPT_IN, IDC_LOAD_SYMBOLS, IDC_SAVE,
    IDC_REG0, IDC_REG_LAST = IDC_REG0 + NREGS - 1,
    IDC_FLAG0, IDC_FLAG_LAST = IDC_FLAG0 + NFLAGS - 1,
    IDC_EF0, IDC_EF_LAST = IDC_EF0 + NEF - 1,
    IDC_CPU_STATE, IDC_RAM_VIEW, IDC_RAM_GOTO, IDC_RAM_ADDR, IDC_RAM_VAL,
    IDC_FOLLOW_PC, IDC_JUMP, IDC_DISASM,
    IDC_VIDEO_TEXT, IDC_VIDEO_VIEW, IDC_VIDEO_PIC,
    IDC_IO_TEXT, IDC_IO_KEYS,
    IDC_BP_LIST, IDC_BP_TYPE, IDC_BP_START, IDC_BP_END, IDC_BP_COND, IDC_BP_ADD,
    IDC_BP_REMOVE, IDC_BP_ENABLE, IDC_BP_CLEAR,
    IDC_CART, IDC_CART_MAP, IDC_CART_GRID,
    IDC_FUJI_NOTE, IDC_FUJI_CLEAR, IDC_FUJI_TEXT, IDC_FUJI_STATE,
    IDC_SAVE_KIND0, IDC_SAVE_KIND_LAST = IDC_SAVE_KIND0 + NSAVE - 1,
    IDC_SYMS_CART, IDC_SYMS_FILE,
    IDC_LABEL_FIRST
};

typedef struct {
    HWND hwnd;
    HWND tabs;
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, status;

    HWND prompt_out, prompt_in, load_symbols, save;

    HWND reg_label[NREGS], reg_edit[NREGS], flag[NFLAGS], ef[NEF], cpu_state, ram_label, ram_view;
    HWND ram_goto_label, ram_goto, ram_addr_label, ram_addr, ram_val_label, ram_val;

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND video_text, video_view_label, video_view, video_pic;

    HWND io_text, io_keys_label, io_keys;

    HWND bp_list, bp_type, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart, cart_map_label, cart_map, cart_grid_label, cart_grid;

    HWND fuji_title, fuji_note, fuji_clear, fuji_text, state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    HFONT mono, ui;
    int mono_w, mono_h;        /* the mono font's cell, for the hand-drawn views */
    HACCEL accel;
    HBRUSH accent, accent_dark;

    o2session *session;
    o2debug *dbg;

    int page;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    int disasm_top;            /* address of the first disassembly line */
    o2debug_line lines[DISASM_MAX];
    int line_count;
    int pc_line;

    o2debug_breakpoint bps[MAX_BPS];
    int nbps;

    o2debug_io io;        /* the last I/O snapshot, for the keypad diagrams */
    o2debug_cart cart_state;   /* the last cartridge snapshot, for the map and the text */

    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    uint32_t *pic_px;
    int pic_w, pic_h;
} debugger;

static debugger *g_dbg;

static const char *const kSaveKinds[NSAVE] = { "dis", "ram", "arena", "raster", "screen", "regs" };
static const char *const kSaveTitles[NSAVE] = { "Disassembly (BIOS and cartridge)...", "Console RAM...",
                                                "Mailbox arena...", "Cart raster...", "Screen (PPM)...",
                                                "Registers (text)..." };

/* The editable registers, in the order the CPU page lays them out: the
 * sixteen scratchpad registers in two columns of eight, then the rest. */
static const char *const kRegNames[NREGS] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
                                              "R8", "R9", "RA", "RB", "RC", "RD", "RE", "RF",
                                              "D", "P", "X", "T", "B" };
static const int kRegIds[NREGS] = {
    O2_REG_R0 + 0, O2_REG_R0 + 1, O2_REG_R0 + 2, O2_REG_R0 + 3,
    O2_REG_R0 + 4, O2_REG_R0 + 5, O2_REG_R0 + 6, O2_REG_R0 + 7,
    O2_REG_R0 + 8, O2_REG_R0 + 9, O2_REG_R0 + 10, O2_REG_R0 + 11,
    O2_REG_R0 + 12, O2_REG_R0 + 13, O2_REG_R0 + 14, O2_REG_R0 + 15,
    O2_REG_D, O2_REG_P, O2_REG_X, O2_REG_T, O2_REG_B };
static const char *const kFlagNames[NFLAGS] = { "DF", "IE", "Q" };
static const int kFlagIds[NFLAGS] = { O2_REG_DF, O2_REG_IE, O2_REG_Q };

/* The page names O2_DEBUGGER_TAB accepts, by page. */
static const char *const kPageNames[PAGE_COUNT] = { "prompt", "cpu", "disassembly", "video", "io",
                                                    "breakpoints", "cart", "fujinet" };

/* The cartridge's page map: what it serves per 256-byte page (S2PG_*). */
static const char *const kPageKinds[4] = { "none", "image ROM", "hotspot", "raster (DMA)" };
static const COLORREF kPageColours[4] = { RGB(0xf4, 0xf4, 0xf4), RGB(0x5b, 0x8d, 0xd9), RGB(0xe0, 0x7a, 0x2e),
                                          RGB(0x5a, 0xa8, 0x5a) };

/* ---- helpers ---------------------------------------------------------------- */

static void set_text(HWND h, const char *text) { SetWindowTextA(h, text); }

/* EDIT controls want CRLF and render control characters as boxes. */
static char *to_crlf(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    return buf;
}

static void set_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    set_text(h, buf ? buf : text);
    free(buf);
}

/* For the live views: replace the text but keep the scroll position. */
static void set_text_lf_keep(HWND h, const char *text)
{
    const int first = (int)SendMessageA(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    set_text_lf(h, text);
    SendMessageA(h, EM_LINESCROLL, 0, first);
}

static void append_text_lf(HWND h, const char *text)
{
    char *buf = to_crlf(text);
    int len;
    if (!buf) return;
    len = GetWindowTextLengthA(h);
    SendMessageA(h, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(h, EM_REPLACESEL, FALSE, (LPARAM)buf);
    SendMessageA(h, EM_SCROLLCARET, 0, 0);
    free(buf);
}

static void edit_text(HWND h, char *out, int outsz) { GetWindowTextA(h, out, outsz); }

static size_t addf(char *s, size_t len, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (len >= cap) return len;
    va_start(ap, fmt);
    n = vsnprintf(s + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return len;
    return len + (size_t)n < cap ? len + (size_t)n : cap - 1;
}

/* The FujiNet page shows the engine's UTF-8 (arrows, the x of a repeat,
 * an ellipsis): its note and log are Unicode controls, set as wide text. */
static wchar_t *wide_view_text(const char *text)
{
    size_t n = strlen(text), i, o = 0;
    char *buf = malloc(n * 2 + 1);
    wchar_t *w = NULL;
    int wn;
    if (!buf) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[o++] = '\r'; buf[o++] = '\n'; }
        else if (c >= 0x20 || c == '\t') buf[o++] = (char)c;
    }
    buf[o] = '\0';
    wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    if (wn > 0 && (w = malloc((size_t)wn * sizeof *w)) != NULL)
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, w, wn);
    free(buf);
    return w;
}

/* A label set only when it changed (no flicker on the live refresh). */
static void set_label(HWND h, const char *utf8)
{
    wchar_t cur[512], w[512];
    w[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 512);
    w[511] = 0;
    GetWindowTextW(h, cur, 512);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
}

/* Whether a log's reader is at its end (it then follows new lines). */
static int edit_at_end(debugger *d, HWND h)
{
    RECT c;
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    const int count = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    GetClientRect(h, &c);
    return first + (d->mono_h > 0 && c.bottom > d->mono_h ? c.bottom / d->mono_h : 1) >= count;
}

/* Replaces a log's text where the reader left it, or, following, at its
 * end. */
static void set_text_at(HWND h, const char *text, int follow)
{
    const int first = (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0);
    wchar_t *w = wide_view_text(text);
    if (!w) return;
    SetWindowTextW(h, w);
    if (follow) SendMessageW(h, EM_LINESCROLL, 0, SendMessageW(h, EM_GETLINECOUNT, 0, 0));
    else SendMessageW(h, EM_LINESCROLL, 0, first - (int)SendMessageW(h, EM_GETFIRSTVISIBLELINE, 0, 0));
    free(w);
}

/* $hex, 0xhex, #dec, or bare hex, as the prompt reads numbers. */
static int parse_num(const char *text, long *out)
{
    char buf[64];
    const char *p;
    char *end;
    long v;
    int base = 16;

    snprintf(buf, sizeof buf, "%s", text);
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '$') p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    else if (*p == '#') { p++; base = 10; }
    if (!*p) return 0;
    v = strtol(p, &end, base);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

static int resolve_addr(debugger *d, const char *text)
{
    long v;
    int addr = -1;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return -1;
    if (*p != '$' && *p != '#' && !(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
        addr = o2debug_label_address(d->dbg, p);
    if (addr < 0 && parse_num(p, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

/* The disassembly rows the view has room for. */
static int disasm_rows(debugger *d)
{
    RECT c;
    int rows;
    if (!d->disasm || d->mono_h <= 0) return 1;
    GetClientRect(d->disasm, &c);
    rows = (c.bottom - c.top) / d->mono_h;
    if (rows < 1) rows = 1;
    if (rows > DISASM_MAX) rows = DISASM_MAX;
    return rows;
}

static HBRUSH make_brush(uint32_t rgb)
{
    return CreateSolidBrush(RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
}

/* A hand-drawn view paints into a memory DC and blits once (they refresh
 * live while the machine runs). */
typedef void (*paint_fn)(debugger *d, HWND hwnd, HDC dc, const RECT *c);

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

static void fill_rgb(HDC dc, const RECT *r, COLORREF colour)
{
    HBRUSH b = CreateSolidBrush(colour);
    FillRect(dc, r, b);
    DeleteObject(b);
}

/* ---- refreshers -------------------------------------------------------------- */

static void refresh_status(debugger *d)
{
    char reason[160], buf[220];
    int addr;
    const int stopped = o2debug_is_stopped(d->dbg);
    o2debug_stop_reason(d->dbg, reason, sizeof reason, &addr);
    if (stopped) snprintf(buf, sizeof buf, "Stopped%s%s", reason[0] ? ": " : "", reason);
    else snprintf(buf, sizeof buf, "Running");
    set_text(d->status, buf);
    set_text(d->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    InvalidateRect(d->run_btn, NULL, FALSE);
}

static void refresh_cpu(debugger *d)
{
    o2debug_cpu c;
    char buf[400];
    int i;
    o2debug_cpu_get(d->dbg, &c);
    {
        const int v[NREGS] = { c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7],
                               c.r[8], c.r[9], c.r[10], c.r[11], c.r[12], c.r[13], c.r[14], c.r[15],
                               c.d, c.p, c.x, c.t, c.b };
        for (i = 0; i < NREGS; i++) {
            if (GetFocus() == d->reg_edit[i]) continue;   /* do not fight the user's typing */
            /* the sixteen registers as four digits, D, T and B as two, P and
             * X (register numbers) as one */
            snprintf(buf, sizeof buf, i < 16 ? "%04X" : (i == 17 || i == 18) ? "%X" : "%02X", v[i]);
            set_text(d->reg_edit[i], buf);
        }
        /* which scratchpad registers are the program counter and the index */
        for (i = 0; i < 16; i++) {
            char name[16];
            snprintf(name, sizeof name, "%s%s%s", kRegNames[i], i == (c.p & 15) ? " P" : "",
                     i == (c.x & 15) ? " X" : "");
            set_label(d->reg_label[i], name);
        }
    }
    {
        const int flags[NFLAGS] = { c.df, c.ie, c.q };
        for (i = 0; i < NFLAGS; i++)
            SendMessageA(d->flag[i], BM_SETCHECK, flags[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        for (i = 0; i < NEF; i++)
            SendMessageA(d->ef[i], BM_SETCHECK, c.ef[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    snprintf(buf, sizeof buf,
             "PC $%04X (R%X)   R(X) $%04X (R%X)\r\n"
             "State %s   mode %s\r\n"
             "1861 INT %s   DMA OUT %s\r\n"
             "Beam line %d  dot %d   frame %u\r\n"
             "Cycles %llu   instructions %llu",
             c.pc & 0xffff, c.p & 15, c.r[c.x & 15] & 0xffff, c.x & 15,
             c.state_name ? c.state_name : "?", c.mode_name ? c.mode_name : "?",
             c.int_line ? "asserted" : "clear", c.dma_line ? "asserted" : "clear",
             c.vpos, c.hpos, c.frame,
             (unsigned long long)c.cycles, (unsigned long long)c.instructions);
    set_text(d->cpu_state, buf);
}

static void refresh_ram(debugger *d)
{
    static uint8_t ram[RAM_SIZE];
    static char text[RAM_SIZE / 16 * 64 + 128];
    size_t len = 0;
    int row, col;
    o2debug_ram_get(d->dbg, ram);
    len += (size_t)snprintf(text + len, sizeof text - len,
                            "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\r\n");
    for (row = 0; row < RAM_SIZE / 16; row++) {
        len += (size_t)snprintf(text + len, sizeof text - len, "$%04X: ", RAM_BASE + row * 16);
        for (col = 0; col < 16; col++)
            len += (size_t)snprintf(text + len, sizeof text - len, "%02X ", ram[row * 16 + col]);
        len += (size_t)snprintf(text + len, sizeof text - len, "\r\n");
    }
    {
        /* keep the scroll position across a refresh */
        int first = (int)SendMessageA(d->ram_view, EM_GETFIRSTVISIBLELINE, 0, 0);
        set_text(d->ram_view, text);
        SendMessageA(d->ram_view, EM_LINESCROLL, 0, first);
    }
}

static void refresh_disasm(debugger *d)
{
    o2debug_cpu c;
    int pc_line = -1;
    const int rows = disasm_rows(d);
    const int follow = SendMessageA(d->follow_pc, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (follow) {
        /* the PC a third of the way down, so what led to it shows too */
        o2debug_cpu_get(d->dbg, &c);
        d->disasm_top = o2debug_row_address(d->dbg, (uint16_t)c.pc, -(rows / 3));
    }
    d->line_count = o2debug_disassemble(d->dbg, (uint16_t)d->disasm_top, d->lines, rows, &pc_line);
    d->pc_line = pc_line;
    InvalidateRect(d->disasm, NULL, FALSE);
}

static void refresh_video(debugger *d)
{
    o2debug_video v;
    static char s[2048];
    size_t len = 0;
    int view;

    o2debug_video_get(d->dbg, &v);
    len = addf(s, len, sizeof s,
        "CDP1861 display   %s   (INP 1 strobes: %u)\n"
        "Beam              line %d  dot %d   %s\n"
        "Lines now         INT %d   EFx %d   DMA OUT %d\n"
        "Next events       INT at line %d   EFx at line %d\n"
        "DMA pointer (R0)  $%04X   last DMA $%04X\n"
        "DMA bytes         %llu since power-on\n"
        "Short bursts      %u   (DMA under 8 bytes: a $Cx opcode in flight)\n"
        "Frame             %u\n\n",
        v.display_on ? "on" : "off", v.inp1_count,
        v.vpos, v.hpos, v.in_display ? "in the picture (lines 80-207)" : "in blanking",
        v.int_line, v.efx_line, v.dma_line,
        v.next_int_line, v.next_efx_line,
        v.dma_pointer, v.last_dma_addr,
        (unsigned long long)v.dma_bytes,
        v.short_lines,
        v.frame);
    len = addf(s, len, sizeof s,
        "Views\n"
        "  Screen             the whole 112x262 raster: blanking\n"
        "                     dimmed, the visible area outlined in\n"
        "                     tan, the beam's line and dot crossed\n"
        "  Cart raster        the 64x128 picture the cartridge\n"
        "                     serves, on screen or not\n"
        "  BIOS display page  $0900-$09FF as the BIOS draws it\n");
    set_text_lf_keep(d->video_text, s);

    view = (int)SendMessageA(d->video_view, CB_GETCURSEL, 0, 0);
    if (view < 0) view = 0;
    if (!o2debug_video_view(d->dbg, view, d->pic_px, &d->pic_w, &d->pic_h))
        d->pic_w = d->pic_h = 0;
    InvalidateRect(d->video_pic, NULL, FALSE);
}

static void refresh_io(debugger *d)
{
    o2debug_io *io = &d->io;
    static char s[2048];
    size_t len = 0;
    int k, i;

    o2debug_io_get(d->dbg, io);
    len = addf(s, len, sizeof s, "Beeper (Q)      %s", io->q ? "on" : "off");
    if (io->q && io->beep_hz > 0) len = addf(s, len, sizeof s, ", %d Hz", io->beep_hz);
    len = addf(s, len, sizeof s,
        "\nKey latch       %d   (OUT 2 selects the key to read)\n"
        "EF3 / EF4       %d / %d   (the latched key on keypad A / B)\n"
        "CLEAR           %s\n"
        "Last OUT       ",
        io->keylatch, io->ef3, io->ef4, io->clear_held ? "held" : "up");
    for (i = 1; i < 8; i++) len = addf(s, len, sizeof s, " %d=$%02X", i, io->last_out[i]);
    len = addf(s, len, sizeof s,
        "\nINP 1           %u   (display-on strobes)\n"
        "BIOS            %s\n\n",
        io->inp1_count,
        io->have_bios ? "imported" : io->stub_parked ? "the stand-in, parked: a game needs the BIOS"
                                                     : "the stand-in (CONFIG and FujiNet apps)");
    for (k = 0; k < 2; k++) {
        len = addf(s, len, sizeof s, "Keypad %c held  ", 'A' + k);
        for (i = 0; i < 10; i++)
            if (io->keys[k] & (1u << i)) len = addf(s, len, sizeof s, " %d", i);
        len = addf(s, len, sizeof s, "%s\n", io->keys[k] & 0x3ff ? "" : " (none)");
    }
    set_text_lf_keep(d->io_text, s);
    InvalidateRect(d->io_keys, NULL, FALSE);
}

static void refresh_bps(debugger *d)
{
    int i, sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
    d->nbps = o2debug_breakpoint_list(d->dbg, d->bps, MAX_BPS);
    SendMessageA(d->bp_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < d->nbps; i++) {
        const o2debug_breakpoint *b = &d->bps[i];
        const int port = (b->type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
        char line[256], kind[8] = "", range[24];
        if (b->type & O2DEBUG_BP_EXEC) strcat(kind, "x");
        if (b->type & O2DEBUG_BP_READ) strcat(kind, "r");
        if (b->type & O2DEBUG_BP_WRITE) strcat(kind, "w");
        if (b->type & O2DEBUG_BP_IN) strcat(kind, "i");
        if (b->type & O2DEBUG_BP_OUT) strcat(kind, "o");
        /* ports are 1-7, addresses a word */
        if (port && b->start == b->end) snprintf(range, sizeof range, "port %u", b->start);
        else if (port) snprintf(range, sizeof range, "ports %u-%u", b->start, b->end);
        else if (b->start == b->end) snprintf(range, sizeof range, "$%04X", b->start);
        else snprintf(range, sizeof range, "$%04X-$%04X", b->start, b->end);
        snprintf(line, sizeof line, "%c %3d  %-3s  %-12s  hits %-6u %s%s", b->enabled ? '+' : '-',
                 b->id, kind, range, b->hits, b->condition[0] ? "if " : "", b->condition);
        SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (d->nbps == 0) SendMessageA(d->bp_list, LB_ADDSTRING, 0, (LPARAM)"(no breakpoints -- click a disassembly line, or add one below)");
    if (sel >= 0 && sel < d->nbps) SendMessageA(d->bp_list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void refresh_cart(debugger *d)
{
    o2debug_cart *c = &d->cart_state;
    char info[256];
    static char s[4096];
    size_t len = 0;
    o2debug_cart_get(d->dbg, c);
    o2debug_cart_info(d->dbg, info, sizeof info);
    InvalidateRect(d->cart_map, NULL, FALSE);
    InvalidateRect(d->cart_grid, NULL, FALSE);
    if (!c->present) { set_text_lf(d->cart, "No FujiNet cartridge is running."); return; }
    len = addf(s, len, sizeof s,
        "%s\n\n"
        "Mode            %s   (%s)\n"
        "Link            %s%s\n"
        "Booted          %s\n"
        "Live image      %s, %u bytes, %d blocks, CRC-32 %08X%s\n",
        info,
        c->mode_name ? c->mode_name : "?", c->mailbox_mode ? "a socket and fujimail" : "a game alone",
        c->link_up ? "up" : "down", c->busy ? " (transaction in flight)" : "",
        c->direct ? "an opened cartridge file" : c->booted_game ? "a game from the network" : "CONFIG",
        c->live_st2 ? "ST2" : "raw", c->live_size, c->live_blocks, c->live_crc,
        c->live_mailbox ? ", claims the mailbox" : "");
    if (c->staged)
        len = addf(s, len, sizeof s, "Staged          %s%s, CRC-32 %08X\n",
                   c->staged_config ? "CONFIG" : c->staged_claim ? "an app" : "a game",
                   c->armed ? " (armed)" : "", c->staged_crc);
    else
        len = addf(s, len, sizeof s, "Staged          nothing\n");
    len = addf(s, len, sizeof s,
        "Swaps           %d\n"
        "Boot            state %d   %d%%   error %d   %u of %u bytes\n\n"
        "Mailbox         ACKSEQ $%02X   STATUS $%02X   last error %u\n"
        "                reply cmd $%02X   RXLEN %u\n"
        "Queue           %u deep\n"
        "Raster          %u frames served (mod 256)   %u short bursts\n"
        "Text cursor     row %d  col %d%s\n",
        c->swaps,
        c->boot_state, c->boot_pct, c->boot_err, c->boot_got, c->boot_total,
        c->ackseq, c->status, c->last_error, c->reply_cmd, c->rxlen,
        c->queue_depth,
        c->frames, c->short_lines,
        c->cursor_row, c->cursor_col, c->inverse ? "   (writing inverse)" : "");
    set_text_lf_keep(d->cart, s);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(debugger *d, int force)
{
    static o2debug_fuji_packet p[TRACE_MAX];
    static char t[TRACE_MAX * 320 + 128];
    static char state[2048];
    char note[200], status[128];
    size_t len = 0;
    int n = o2debug_fujinet_trace(d->dbg, p, TRACE_MAX), i, follow;

    if (o2session_fujinet_external(d->session)) {
        o2session_cart_status(d->session, status, sizeof status);
        snprintf(note, sizeof note, "External FujiNet at %s. Cartridge: %s.",
                 o2session_boip_endpoint(d->session), status);
    } else if (!o2session_fujinet_running(d->session)) {
        snprintf(note, sizeof note, "FujiNet is off (Settings \xe2\x96\xb8 FujiNet).");
    } else {
        o2session_cart_status(d->session, status, sizeof status);
        snprintf(note, sizeof note, "FujiNet is on. Cartridge: %s.", status);
    }
    set_label(d->fuji_note, note);
    o2debug_fujinet_state(d->dbg, state, sizeof state);
    set_text_lf_keep(d->fuji_state, state);
    if (n < 0) n = 0;

    if (!force && n == d->fuji_count && (n == 0 || p[n - 1].serial == d->fuji_last)) return;
    follow = force || d->fuji_count < 0 || edit_at_end(d, d->fuji_text);
    d->fuji_count = n;
    d->fuji_last = n > 0 ? p[n - 1].serial : 0;
    t[0] = '\0';
    for (i = 0; i < n; i++) {
        int k;
        len = addf(t, len, sizeof t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) len = addf(t, len, sizeof t, " \xc3\x97%u", p[i].repeat);
        len = addf(t, len, sizeof t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            len = addf(t, len, sizeof t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) len = addf(t, len, sizeof t, "\xe2\x80\xa6");
    }
    if (n == 0) len = addf(t, len, sizeof t, "(no FujiBus packets since the trace was cleared)");
    set_text_at(d->fuji_text, t, follow);
}

static void refresh_all(debugger *d)
{
    refresh_status(d);
    refresh_cpu(d);
    refresh_ram(d);
    refresh_disasm(d);
    refresh_video(d);
    refresh_io(d);
    refresh_bps(d);
    refresh_cart(d);
    refresh_fujinet(d, 0);
}

/* ---- actions ------------------------------------------------------------------ */

static void toggle_run(debugger *d)
{
    if (o2debug_is_stopped(d->dbg)) o2debug_resume(d->dbg);
    else o2debug_stop(d->dbg);
    refresh_all(d);
}

static void run_prompt(debugger *d)
{
    static char out[65536];
    char cmd[256], echo[300];
    edit_text(d->prompt_in, cmd, sizeof cmd);
    if (!cmd[0]) return;
    /* history: the newest last, no repeats of the one before */
    if (d->nhistory == 0 || strcmp(d->history[d->nhistory - 1], cmd) != 0) {
        if (d->nhistory == HISTORY_MAX) {
            memmove(d->history[0], d->history[1], sizeof d->history[0] * (HISTORY_MAX - 1));
            d->nhistory--;
        }
        snprintf(d->history[d->nhistory++], sizeof d->history[0], "%s", cmd);
    }
    d->history_pos = d->nhistory;
    snprintf(echo, sizeof echo, "> %s\n", cmd);
    append_text_lf(d->prompt_out, echo);
    o2debug_command(d->dbg, cmd, out, sizeof out);
    append_text_lf(d->prompt_out, out);
    append_text_lf(d->prompt_out, "\n");
    set_text(d->prompt_in, "");
    refresh_all(d);
}

/* Up / Down in the prompt walk the commands already run; past the newest is
 * an empty line again. */
static void history_step(debugger *d, int newer)
{
    int len;
    if (d->nhistory == 0) return;
    d->history_pos += newer ? 1 : -1;
    if (d->history_pos < 0) d->history_pos = 0;
    if (d->history_pos >= d->nhistory) {
        d->history_pos = d->nhistory;
        set_text(d->prompt_in, "");
        return;
    }
    set_text(d->prompt_in, d->history[d->history_pos]);
    len = GetWindowTextLengthA(d->prompt_in);
    SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

static void complete_prompt(debugger *d)
{
    char text[256], comps[4096];
    const char *word;
    char *sp;
    int n;
    edit_text(d->prompt_in, text, sizeof text);
    sp = strrchr(text, ' ');
    word = sp ? sp + 1 : text;
    n = o2debug_completions(d->dbg, word, comps, sizeof comps);
    if (n == 1) {
        char line[256], joined[520];
        char *nl;
        snprintf(line, sizeof line, "%.255s", comps);
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (sp) sp[1] = '\0'; else text[0] = '\0';
        snprintf(joined, sizeof joined, "%s%s ", text, line);
        set_text(d->prompt_in, joined);
        SendMessageA(d->prompt_in, EM_SETSEL, (WPARAM)strlen(joined), (LPARAM)strlen(joined));
    } else if (n > 1) {
        append_text_lf(d->prompt_out, comps);
    }
}

static void jump_to(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    if (addr < 0) {
        append_text_lf(d->prompt_out, "No such label or address.\n");
        return;
    }
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
    d->disasm_top = addr;
    refresh_disasm(d);
}

/* Scroll the RAM dump to an address (or label) in console RAM. */
static void ram_goto(debugger *d, const char *text)
{
    const int addr = resolve_addr(d, text);
    int line, first;
    if (addr < RAM_BASE || addr >= RAM_BASE + RAM_SIZE) {
        append_text_lf(d->prompt_out, addr < 0 ? "No such label or address.\n"
                                               : "Console RAM is $0800-$09FF.\n");
        return;
    }
    line = (addr - RAM_BASE) / 16 + 1;   /* + the column header */
    first = (int)SendMessageA(d->ram_view, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageA(d->ram_view, EM_LINESCROLL, 0, line - first);
}

static int pick_file(debugger *d, int save, const char *title, const char *filter, char *path, DWORD pathsz)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    path[0] = '\0';
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = d->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = pathsz;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) ? 1 : 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
}

static void save_file(debugger *d, int which)
{
    char path[MAX_PATH], msg[512], title[64];
    snprintf(title, sizeof title, "Save %s", kSaveTitles[which]);
    if (!pick_file(d, 1, title, "All files\0*.*\0\0", path, sizeof path)) return;
    o2debug_save(d->dbg, kSaveKinds[which], path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
}

static void popup_below(debugger *d, HWND button, HMENU m)
{
    RECT r;
    GetWindowRect(button, &r);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, d->hwnd, NULL);
    DestroyMenu(m);
}

static void save_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    int i;
    for (i = 0; i < NSAVE; i++) AppendMenuA(m, MF_STRING, (UINT_PTR)(IDC_SAVE_KIND0 + i), kSaveTitles[i]);
    popup_below(d, d->save, m);
}

/* Symbols come from a file the user picks, or from the one beside the
 * opened cartridge (game.map / .lst / .sym next to game.st2). */
static void symbols_menu(debugger *d)
{
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, IDC_SYMS_FILE, "From a file...");
    AppendMenuA(m, MF_STRING, IDC_SYMS_CART, "Beside the cartridge");
    popup_below(d, d->load_symbols, m);
}

static void load_symbols(debugger *d, int beside_cart)
{
    char path[MAX_PATH], msg[512];
    if (!beside_cart &&
        !pick_file(d, 0, "Load symbols",
                   "Symbol files (*.map;*.lst;*.sym;*.txt)\0*.map;*.lst;*.sym;*.txt\0All files\0*.*\0\0",
                   path, sizeof path))
        return;
    o2debug_load_symbols(d->dbg, beside_cart ? NULL : path, msg, sizeof msg);
    append_text_lf(d->prompt_out, msg);
    append_text_lf(d->prompt_out, "\n");
    refresh_all(d);
}

static void add_breakpoint(debugger *d)
{
    char a[64], b[64], cond[256];
    static const int types[6] = { O2DEBUG_BP_EXEC, O2DEBUG_BP_READ, O2DEBUG_BP_WRITE,
                                  O2DEBUG_BP_READ | O2DEBUG_BP_WRITE, O2DEBUG_BP_IN,
                                  O2DEBUG_BP_OUT };
    int start, end, sel, id, port;
    edit_text(d->bp_start, a, sizeof a);
    edit_text(d->bp_end, b, sizeof b);
    edit_text(d->bp_cond, cond, sizeof cond);
    sel = (int)SendMessageA(d->bp_type, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel > 5) sel = 0;
    port = (types[sel] & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
    start = resolve_addr(d, a);
    if (start < 0) { MessageBoxA(d->hwnd, "The start is not an address or label.", "Breakpoint", MB_ICONWARNING); return; }
    end = b[0] ? resolve_addr(d, b) : start;
    if (end < start) end = start;
    /* the 1802's N lines: INP 1-7 and OUT 1-7 */
    if (port && (start < 1 || end > 7)) { MessageBoxA(d->hwnd, "An I/O port is 1-7.", "Breakpoint", MB_ICONWARNING); return; }
    id = o2debug_breakpoint_add(d->dbg, types[sel], (uint16_t)start, (uint16_t)end, cond);
    if (id < 0) { MessageBoxA(d->hwnd, "The condition does not parse.", "Breakpoint", MB_ICONWARNING); return; }
    set_text(d->bp_start, "");
    set_text(d->bp_end, "");
    set_text(d->bp_cond, "");
    refresh_bps(d);
    refresh_disasm(d);
}

static void on_accept(debugger *d, int id)
{
    char buf[512];
    long v;

    if (id >= IDC_REG0 && id <= IDC_REG_LAST) {
        edit_text(d->reg_edit[id - IDC_REG0], buf, sizeof buf);
        if (parse_num(buf, &v)) o2debug_cpu_set(d->dbg, kRegIds[id - IDC_REG0], (int)v);
        SetFocus(d->hwnd);
        refresh_all(d);
        return;
    }
    switch (id) {
    case IDC_PROMPT_IN: run_prompt(d); break;
    case IDC_RAM_GOTO:
        edit_text(d->ram_goto, buf, sizeof buf);
        ram_goto(d, buf);
        break;
    case IDC_RAM_ADDR:
    case IDC_RAM_VAL: {
        char abuf[64];
        int a;
        edit_text(d->ram_addr, abuf, sizeof abuf);
        edit_text(d->ram_val, buf, sizeof buf);
        a = resolve_addr(d, abuf);
        if (a >= 0 && parse_num(buf, &v)) o2debug_write(d->dbg, (uint16_t)a, (uint8_t)v);
        refresh_all(d);
        break;
    }
    case IDC_JUMP:
        edit_text(d->jump, buf, sizeof buf);
        jump_to(d, buf);
        break;
    case IDC_BP_START:
    case IDC_BP_END:
    case IDC_BP_COND:
        add_breakpoint(d);
        break;
    default: break;
    }
}

/* ---- the 1861's pictures ------------------------------------------------------------ */

/* Nearest-neighbour at the largest whole scale that fits, centred: the
 * Pixie's pixels stay whole. */
static LRESULT CALLBACK pic_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect(dc, &c, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        if (d && d->pic_px && d->pic_w > 0 && d->pic_h > 0) {
            BITMAPINFO bmi;
            int sx = c.right / d->pic_w, sy = c.bottom / d->pic_h, scale, w, h;
            scale = sx < sy ? sx : sy;
            if (scale < 1) scale = 1;
            w = d->pic_w * scale;
            h = d->pic_h * scale;
            memset(&bmi, 0, sizeof bmi);
            bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
            bmi.bmiHeader.biWidth = d->pic_w;
            bmi.bmiHeader.biHeight = -d->pic_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, (c.right - w) / 2 > 0 ? (c.right - w) / 2 : 0,
                          (c.bottom - h) / 2 > 0 ? (c.bottom - h) / 2 : 0, w, h,
                          0, 0, d->pic_w, d->pic_h, d->pic_px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the keypad diagrams ------------------------------------------------------------- */

/* Both keypads as 3x4 grids in the console's layout (1 2 3 / 4 5 6 /
 * 7 8 9 / _ 0 _): a key the console sees held is filled with the accent,
 * the key OUT 2 has latched is outlined in the dark accent on both. */
#define KEY_CELL_W 36
#define KEY_CELL_H 28
#define KEY_GAP     4
#define KEYPAD_W   (3 * KEY_CELL_W + 2 * KEY_GAP)
#define KEYPADS_GAP 28

static void paint_keys(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int k, i;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    SelectObject(dc, d->ui);
    for (k = 0; k < 2; k++) {
        const int x0 = k * (KEYPAD_W + KEYPADS_GAP);
        RECT head;
        char title[16];
        SetRect(&head, x0, 0, x0 + KEYPAD_W, 18);
        snprintf(title, sizeof title, "Keypad %c", 'A' + k);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, title, -1, &head, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (i = 0; i < 10; i++) {
            const int cell = i == 0 ? 10 : i - 1;
            const int held = (d->io.keys[k] >> i) & 1;
            RECT r;
            char face[2] = { (char)('0' + i), '\0' };
            SetRect(&r, x0 + (cell % 3) * (KEY_CELL_W + KEY_GAP), 22 + (cell / 3) * (KEY_CELL_H + KEY_GAP),
                    x0 + (cell % 3) * (KEY_CELL_W + KEY_GAP) + KEY_CELL_W,
                    22 + (cell / 3) * (KEY_CELL_H + KEY_GAP) + KEY_CELL_H);
            FillRect(dc, &r, held ? d->accent : GetSysColorBrush(COLOR_WINDOW));
            FrameRect(dc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
            if (d->io.keylatch == i) {
                RECT in = r;
                FrameRect(dc, &in, d->accent_dark);
                InflateRect(&in, -1, -1);
                FrameRect(dc, &in, d->accent_dark);
            }
            SetTextColor(dc, held ? RGB(0, 0, 0) : GetSysColor(COLOR_WINDOWTEXT));
            DrawTextA(dc, face, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    {
        RECT legend;
        SetRect(&legend, 0, 22 + 4 * (KEY_CELL_H + KEY_GAP) + 4, c->right, c->bottom);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, "Filled: held as the console sees it.  Outlined: the key OUT 2 latched.", -1, &legend,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
}

static LRESULT CALLBACK keys_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_keys); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the cartridge's page map and text engine ------------------------------------------ */

/* 256 pages in 16 rows of 16 ($0000 at the top left), each in the colour of
 * what the cartridge serves there, with a legend under them. */
static void paint_cart_map(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const int label_w = d->mono_w * 6 + 4, head_h = d->mono_h + 2;
    int cw, ch, row, col, i, x;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_BTNFACE));
    cw = (c->right - label_w) / 16;
    ch = (c->bottom - head_h - d->mono_h - 8) / 16;
    if (cw < 6) cw = 6;
    if (ch < 6) ch = 6;
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    for (col = 0; col < 16; col++) {
        RECT r;
        char hex[2] = { "0123456789ABCDEF"[col], '\0' };
        SetRect(&r, label_w + col * cw, 0, label_w + col * cw + cw, head_h);
        DrawTextA(dc, hex, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (row = 0; row < 16; row++) {
        RECT r;
        char addr[8];
        snprintf(addr, sizeof addr, "$%X000", row);
        SetRect(&r, 0, head_h + row * ch, label_w - 4, head_h + row * ch + ch);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        DrawTextA(dc, addr, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        for (col = 0; col < 16; col++) {
            const uint8_t t = d->cart_state.page_type[row * 16 + col];
            SetRect(&r, label_w + col * cw, head_h + row * ch, label_w + col * cw + cw, head_h + row * ch + ch);
            fill_rgb(dc, &r, kPageColours[t < 4 ? t : 0]);
            FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
        }
    }
    /* the legend */
    x = label_w;
    for (i = 0; i < 4; i++) {
        RECT sw, text;
        SIZE sz;
        const int y = head_h + 16 * ch + 6;
        SetRect(&sw, x, y + 2, x + d->mono_h - 4, y + d->mono_h - 2);
        fill_rgb(dc, &sw, kPageColours[i]);
        FrameRect(dc, &sw, GetSysColorBrush(COLOR_BTNSHADOW));
        GetTextExtentPoint32A(dc, kPageKinds[i], (int)strlen(kPageKinds[i]), &sz);
        SetRect(&text, sw.right + 4, y, sw.right + 4 + sz.cx, y + d->mono_h);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, kPageKinds[i], -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        x = text.right + 14;
    }
}

/* The cartridge's 16x10 text screen as its engine last drew it: inverse
 * cells dark, the cursor's cell outlined in the dark accent. */
static void paint_cart_grid(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    const o2debug_cart *k = &d->cart_state;
    int cw = c->right / 16, ch = c->bottom / 10, row, col;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    if (cw < 4) cw = 4;
    if (ch < 4) ch = 4;
    for (row = 0; row < 10; row++)
        for (col = 0; col < 16; col++) {
            const uint8_t chr = k->text[row][col];
            const int inv = k->text_inv[row][col] != 0;
            char glyph[2];
            RECT r;
            SetRect(&r, col * cw, row * ch, col * cw + cw, row * ch + ch);
            if (inv) FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
            FrameRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
            /* the prompt's 'text' rendering: printable ASCII as is, any
             * other drawn glyph as # */
            glyph[0] = (char)(chr >= 0x20 && chr < 0x7f ? chr : chr ? '#' : ' ');
            glyph[1] = '\0';
            SetTextColor(dc, inv ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT));
            DrawTextA(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (k->present && row == k->cursor_row && col == k->cursor_col) {
                RECT in = r;
                FrameRect(dc, &in, d->accent_dark);
                InflateRect(&in, -1, -1);
                FrameRect(dc, &in, d->accent_dark);
            }
        }
}

static LRESULT CALLBACK cart_map_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_cart_map); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK cart_grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (msg == WM_PAINT && d) { paint_buffered(d, hwnd, paint_cart_grid); return 0; }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- the disassembly ---------------------------------------------------------------- */

/* Drawn by hand, one row per line: the breakpoint gutter, address, bytes,
 * label, instruction, and the comment (a branch target's label, a port's
 * name) in grey. The PC's line is filled with the accent, dark text on it. */
static void paint_disasm(debugger *d, HWND hwnd, HDC dc, const RECT *c)
{
    int i;
    (void)hwnd;
    FillRect(dc, c, GetSysColorBrush(COLOR_WINDOW));
    if (d->line_count == 0) {
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        TextOutA(dc, GUTTER_W, 0, "(no disassembly)", 16);
        return;
    }
    for (i = 0; i < d->line_count; i++) {
        const o2debug_line *l = &d->lines[i];
        const int y = i * d->mono_h;
        RECT row;
        char text[200];
        int len;
        if (y >= c->bottom) break;
        SetRect(&row, 0, y, c->right, y + d->mono_h);
        if (l->is_pc) FillRect(dc, &row, d->accent);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        if (l->has_breakpoint) {
            const int size = d->mono_h - 6 > 4 ? d->mono_h - 6 : 4;
            HGDIOBJ oldb = SelectObject(dc, d->accent_dark);
            HGDIOBJ oldp = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, 4, y + 3, 4 + size + 1, y + 3 + size + 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
        }
        len = snprintf(text, sizeof text, "%04X  %-9.9s %-16.16s %s", l->address, l->bytes, l->label, l->disasm);
        if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
        TextOutA(dc, GUTTER_W, y, text, len);
        if (l->comment[0]) {
            len = snprintf(text, sizeof text, "; %s", l->comment);
            if (len > (int)sizeof text - 1) len = (int)sizeof text - 1;
            if (!l->is_pc) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            TextOutA(dc, GUTTER_W + d->mono_w * COMMENT_COL, y, text, len);
        }
    }
}

/* A click toggles the breakpoint on that line; the wheel and the keys
 * browse (and turn Follow PC off, as in the other frontends). */
static LRESULT CALLBACK disasm_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT:
        paint_buffered(d, hwnd, paint_disasm);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        /* more or fewer rows fit now */
        if (d->disasm == hwnd) refresh_disasm(d);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_LBUTTONDOWN: {
        const int line = d->mono_h > 0 ? GET_Y_LPARAM(lp) / d->mono_h : -1;
        SetFocus(hwnd);
        if (line >= 0 && line < d->line_count) {
            o2debug_breakpoint_toggle(d->dbg, d->lines[line].address);
            refresh_disasm(d);
            refresh_bps(d);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int rows = -GET_WHEEL_DELTA_WPARAM(wp) / 40;
        SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
        if (rows) d->disasm_top = o2debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
        refresh_disasm(d);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_UP || wp == VK_DOWN) {
            const int page = disasm_rows(d) / 2 > 0 ? disasm_rows(d) / 2 : 1;
            int rows = wp == VK_PRIOR ? -page : wp == VK_NEXT ? page : wp == VK_UP ? -1 : 1;
            SendMessageA(d->follow_pc, BM_SETCHECK, BST_UNCHECKED, 0);
            d->disasm_top = o2debug_row_address(d->dbg, (uint16_t)d->disasm_top, rows);
            refresh_disasm(d);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- edit subclassing ------------------------------------------------------------- */

static WNDPROC g_edit_proc;

/* Enter in a single-line field means "apply this value"; Tab in the prompt
 * completes and Up/Down recall. None may beep its way through the default
 * handler. */
static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    const int id = (int)GetWindowLongPtrA(hwnd, GWLP_ID);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_ACCEPT, (WPARAM)id, 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && wp == VK_TAB) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_TAB, 0, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
        PostMessageA(GetAncestor(hwnd, GA_ROOT), WM_DBG_HISTORY, wp == VK_DOWN, 0);
        return 0;
    }
    if (id == IDC_PROMPT_IN && msg == WM_CHAR && wp == VK_TAB) return 0;
    if (id == IDC_PROMPT_IN && msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcA(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- construction ------------------------------------------------------------------ */

static HWND child(debugger *d, const char *cls, const char *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, d->hwnd,
                             (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE), NULL);
    SendMessageA(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

static HWND mono_view(debugger *d, int id, int wrap)
{
    return child(d, "EDIT", "",
                 WS_BORDER | WS_VSCROLL | (wrap ? 0 : WS_HSCROLL) | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 id, d->mono);
}

static HWND field(debugger *d, int id, HFONT font)
{
    HWND h = child(d, "EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id, font);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)edit_subclass);
    return h;
}

static HWND label(debugger *d, const char *text)
{
    static int next_id;
    return child(d, "STATIC", text, SS_LEFT, IDC_LABEL_FIRST + next_id++, d->ui);
}

static HWND button(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, id, d->ui);
}

static HWND checkbox(debugger *d, const char *text, int id)
{
    return child(d, "BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, d->ui);
}

static HWND combo(debugger *d, int id, const char *const *items, int n)
{
    HWND c = child(d, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, d->ui);
    int i;
    for (i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageA(c, CB_SETCURSEL, 0, 0);
    return c;
}

static void build_controls(debugger *d)
{
    TCITEMA item;
    static const char *const tabs[PAGE_COUNT] = { "Prompt", "CPU && RAM", "Disassembly", "Video",
                                                  "Sound && I/O", "Breakpoints", "Cart", "FujiNet" };
    static const char *const ef_names[NEF] = { "EF1", "EF2", "EF3", "EF4" };
    static const char *const views[3] = { "Screen (112 x 262)", "Cart raster (64 x 128)",
                                          "BIOS display page (64 x 32)" };
    static const char *const bp_types[6] = { "Execute", "Read", "Write", "Read/Write", "Port in", "Port out" };
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(d->hwnd, GWLP_HINSTANCE);
    int i;

    d->tabs = child(d, WC_TABCONTROLA, "", WS_VISIBLE | WS_CLIPSIBLINGS, IDC_TABS, d->ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < PAGE_COUNT; i++) {
        item.pszText = (char *)tabs[i];
        SendMessageA(d->tabs, TCM_INSERTITEMA, (WPARAM)i, (LPARAM)&item);
    }

    /* Toolbar: the stepping buttons, the symbol and save menus (they
     * matter on every page), then the stop reason. Run/Stop is owner-drawn
     * so it can wear the accent colour while the machine is stopped. */
    d->run_btn = child(d, "BUTTON", "Stop (F5)", BS_OWNERDRAW | WS_TABSTOP, IDC_RUN, d->ui);
    d->step_btn = button(d, "Step (F7)", IDC_STEP);
    d->over_btn = button(d, "Over (F8)", IDC_OVER);
    d->out_btn = button(d, "Out (Shift+F8)", IDC_OUT);
    d->scan_btn = button(d, "Scanline+1", IDC_SCAN);
    d->frame_btn = button(d, "Frame+1", IDC_FRAME);
    d->load_symbols = button(d, "Load symbols...", IDC_LOAD_SYMBOLS);
    d->save = button(d, "Save...", IDC_SAVE);
    d->status = child(d, "STATIC", "Running", SS_RIGHT | SS_ENDELLIPSIS, IDC_STATUS, d->ui);
    {
        HWND bar[9] = { d->run_btn, d->step_btn, d->over_btn, d->out_btn, d->scan_btn, d->frame_btn,
                        d->load_symbols, d->save, d->status };
        for (i = 0; i < 9; i++) ShowWindow(bar[i], SW_SHOW);
    }

    /* Prompt */
    d->prompt_out = mono_view(d, IDC_PROMPT_OUT, 1);
    set_text(d->prompt_out, "Magnavox Odyssey 2 debugger prompt (CDP1802 / CDP1861). Type 'help' for every command.\r\n");
    d->prompt_in = field(d, IDC_PROMPT_IN, d->mono);
    SendMessageA(d->prompt_in, EM_SETCUEBANNER, TRUE,
                 (LPARAM)L"command (help, step, break, bpw, print, mem, runto, ...) - Tab completes, Up/Down recall");

    /* CPU & RAM */
    for (i = 0; i < NREGS; i++) {
        d->reg_label[i] = label(d, kRegNames[i]);
        d->reg_edit[i] = field(d, IDC_REG0 + i, d->mono);
        SendMessageA(d->reg_edit[i], EM_SETLIMITTEXT, 4, 0);
    }
    for (i = 0; i < NFLAGS; i++) d->flag[i] = checkbox(d, kFlagNames[i], IDC_FLAG0 + i);
    for (i = 0; i < NEF; i++) {
        /* inputs the CPU samples, not state it keeps: shown, not set */
        d->ef[i] = checkbox(d, ef_names[i], IDC_EF0 + i);
        EnableWindow(d->ef[i], FALSE);
    }
    d->cpu_state = child(d, "STATIC", "", SS_LEFT, IDC_CPU_STATE, d->mono);
    d->ram_label = label(d, "Console RAM ($0800-$09FF)");
    d->ram_view = mono_view(d, IDC_RAM_VIEW, 0);
    d->ram_goto_label = label(d, "Go to");
    d->ram_goto = field(d, IDC_RAM_GOTO, d->mono);
    d->ram_addr_label = label(d, "Write address");
    d->ram_addr = field(d, IDC_RAM_ADDR, d->mono);
    d->ram_val_label = label(d, "value");
    d->ram_val = field(d, IDC_RAM_VAL, d->mono);

    /* Disassembly */
    d->follow_pc = checkbox(d, "Follow PC", IDC_FOLLOW_PC);
    SendMessageA(d->follow_pc, BM_SETCHECK, BST_CHECKED, 0);
    d->jump_label = label(d, "Jump to");
    d->jump = field(d, IDC_JUMP, d->mono);
    d->disasm_hint = label(d, "Click a line to toggle its breakpoint; scroll to browse");
    d->disasm = CreateWindowExA(WS_EX_CLIENTEDGE, "O2DbgDisasm", "", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10,
                                d->hwnd, (HMENU)(INT_PTR)IDC_DISASM, inst, NULL);

    /* Video */
    d->video_text = mono_view(d, IDC_VIDEO_TEXT, 0);
    d->video_view_label = label(d, "View");
    d->video_view = combo(d, IDC_VIDEO_VIEW, views, 3);
    d->video_pic = child(d, "O2DbgPixels", "", 0, IDC_VIDEO_PIC, d->ui);

    /* Sound & I/O */
    d->io_text = mono_view(d, IDC_IO_TEXT, 0);
    d->io_keys_label = label(d, "The keypads as the console sees them");
    d->io_keys = child(d, "O2DbgKeys", "", 0, IDC_IO_KEYS, d->ui);

    /* Breakpoints */
    d->bp_list = child(d, "LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, IDC_BP_LIST, d->mono);
    d->bp_type = combo(d, IDC_BP_TYPE, bp_types, 6);
    d->bp_start_label = label(d, "Address");
    d->bp_start = field(d, IDC_BP_START, d->mono);
    d->bp_end_label = label(d, "to");
    d->bp_end = field(d, IDC_BP_END, d->mono);
    d->bp_cond_label = label(d, "if");
    d->bp_cond = field(d, IDC_BP_COND, d->mono);
    SendMessageA(d->bp_cond, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional condition, e.g. value == $FF");
    d->bp_add = button(d, "Add", IDC_BP_ADD);
    d->bp_remove = button(d, "Remove", IDC_BP_REMOVE);
    d->bp_enable = button(d, "Enable/Disable", IDC_BP_ENABLE);
    d->bp_clear = button(d, "Clear all", IDC_BP_CLEAR);

    /* Cart */
    d->cart = mono_view(d, IDC_CART, 0);
    d->cart_map_label = label(d, "Pages: what the cartridge serves per 256 bytes");
    d->cart_map = child(d, "O2DbgCartMap", "", 0, IDC_CART_MAP, d->mono);
    d->cart_grid_label = label(d, "The text engine, as drawn (16 x 10)");
    d->cart_grid = child(d, "O2DbgCartGrid", "", 0, IDC_CART_GRID, d->mono);

    /* FujiNet: the note and the log are Unicode (see wide_view_text) */
    d->fuji_title = label(d, "FujiBus packets (SLIP-decoded)");
    d->fuji_note = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, d->hwnd,
                                   (HMENU)(INT_PTR)IDC_FUJI_NOTE, inst, NULL);
    SendMessageW(d->fuji_note, WM_SETFONT, (WPARAM)d->ui, TRUE);
    d->fuji_clear = button(d, "Clear", IDC_FUJI_CLEAR);
    d->fuji_text = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                       ES_AUTOVSCROLL,
                                   0, 0, 10, 10, d->hwnd, (HMENU)(INT_PTR)IDC_FUJI_TEXT, inst, NULL);
    SendMessageW(d->fuji_text, WM_SETFONT, (WPARAM)d->mono, TRUE);
    SendMessageW(d->fuji_text, EM_SETLIMITTEXT, 0, 0);
    d->state_title = label(d, "The cartridge's mailbox");
    d->fuji_state = mono_view(d, IDC_FUJI_STATE, 0);

    /* The tab control was created first, so every page control sits below
     * it in z-order and its body would paint over labels that have not
     * repainted since; at the bottom (with WS_CLIPSIBLINGS) it never does. */
    SetWindowPos(d->tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void show_page(debugger *d, int page)
{
    HWND prompt[] = { d->prompt_out, d->prompt_in };
    HWND cpu[] = { d->cpu_state, d->ram_label, d->ram_view, d->ram_goto_label, d->ram_goto,
                   d->ram_addr_label, d->ram_addr, d->ram_val_label, d->ram_val };
    HWND dis[] = { d->follow_pc, d->jump_label, d->jump, d->disasm_hint, d->disasm };
    HWND video[] = { d->video_text, d->video_view_label, d->video_view, d->video_pic };
    HWND io[] = { d->io_text, d->io_keys_label, d->io_keys };
    HWND brk[] = { d->bp_list, d->bp_type, d->bp_start_label, d->bp_start, d->bp_end_label, d->bp_end,
                   d->bp_cond_label, d->bp_cond, d->bp_add, d->bp_remove, d->bp_enable, d->bp_clear };
    HWND cart[] = { d->cart, d->cart_map_label, d->cart_map, d->cart_grid_label, d->cart_grid };
    HWND fuji[] = { d->fuji_title, d->fuji_note, d->fuji_clear, d->fuji_text, d->state_title, d->fuji_state };
    size_t i;
    d->page = page;
#define SHOW(arr, p) for (i = 0; i < sizeof(arr) / sizeof((arr)[0]); i++) ShowWindow((arr)[i], page == (p) ? SW_SHOW : SW_HIDE)
    SHOW(prompt, PAGE_PROMPT);
    SHOW(cpu, PAGE_CPU);
    SHOW(d->reg_label, PAGE_CPU);
    SHOW(d->reg_edit, PAGE_CPU);
    SHOW(d->flag, PAGE_CPU);
    SHOW(d->ef, PAGE_CPU);
    SHOW(dis, PAGE_DISASM);
    SHOW(video, PAGE_VIDEO);
    SHOW(io, PAGE_IO);
    SHOW(brk, PAGE_BREAKS);
    SHOW(cart, PAGE_CART);
    SHOW(fuji, PAGE_FUJINET);
#undef SHOW
}

static void layout(debugger *d)
{
    RECT client, page;
    int y = 8, bx = 8, px, py, pw, ph, i;

    GetClientRect(d->hwnd, &client);

    MoveWindow(d->run_btn, bx, y, 96, 26, TRUE);     bx += 102;
    MoveWindow(d->step_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->over_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->out_btn, bx, y, 110, 26, TRUE);    bx += 116;
    MoveWindow(d->scan_btn, bx, y, 90, 26, TRUE);    bx += 96;
    MoveWindow(d->frame_btn, bx, y, 80, 26, TRUE);   bx += 92;
    MoveWindow(d->load_symbols, bx, y, 110, 26, TRUE); bx += 116;
    MoveWindow(d->save, bx, y, 80, 26, TRUE);        bx += 86;
    MoveWindow(d->status, bx, y + 5, client.right - bx - 8 > 40 ? client.right - bx - 8 : 40, 20, TRUE);

    MoveWindow(d->tabs, 8, 40, client.right - 16, client.bottom - 48, TRUE);
    page = (RECT){ 8, 40, client.right - 8, client.bottom - 8 };
    SendMessageA(d->tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    px = page.left + 4; py = page.top + 4;
    pw = page.right - page.left - 8; ph = page.bottom - page.top - 8;
    if (pw < 200) pw = 200;
    if (ph < 200) ph = 200;

    /* Prompt */
    MoveWindow(d->prompt_out, px, py, pw, ph - 32, TRUE);
    MoveWindow(d->prompt_in, px, py + ph - 26, pw, 24, TRUE);

    /* CPU & RAM: R0-R7 and R8-RF in two columns, then D P X T B, then the
     * flags and the EF inputs; the machine's state under them; the RAM to
     * the right. */
    {
        const int col_w = 112, row_h = 28, left_w = 4 * col_w + 16;
        int ry, rx;
        for (i = 0; i < NREGS; i++) {
            const int col = i < 16 ? i / 8 : 2, row = i < 16 ? i % 8 : i - 16;
            const int x = px + col * col_w;
            ry = py + row * row_h;
            MoveWindow(d->reg_label[i], x, ry + 4, 46, 18, TRUE);
            MoveWindow(d->reg_edit[i], x + 48, ry, 54, 24, TRUE);
        }
        for (i = 0; i < NFLAGS; i++) MoveWindow(d->flag[i], px + 3 * col_w, py + i * row_h, 80, 22, TRUE);
        for (i = 0; i < NEF; i++) MoveWindow(d->ef[i], px + 3 * col_w, py + (NFLAGS + i) * row_h + 6, 80, 22, TRUE);
        ry = py + 8 * row_h + 10;
        MoveWindow(d->cpu_state, px, ry, left_w, 5 * d->mono_h + 8, TRUE);

        rx = px + left_w + 12;
        MoveWindow(d->ram_label, rx, py, pw - (rx - px), 18, TRUE);
        MoveWindow(d->ram_view, rx, py + 20, pw - (rx - px), ph - 20 - 34, TRUE);
        ry = py + ph - 26;
        MoveWindow(d->ram_goto_label, rx, ry + 4, 36, 18, TRUE);
        MoveWindow(d->ram_goto, rx + 40, ry, 100, 24, TRUE);
        MoveWindow(d->ram_addr_label, rx + 160, ry + 4, 80, 18, TRUE);
        MoveWindow(d->ram_addr, rx + 244, ry, 80, 24, TRUE);
        MoveWindow(d->ram_val_label, rx + 334, ry + 4, 40, 18, TRUE);
        MoveWindow(d->ram_val, rx + 376, ry, 60, 24, TRUE);
    }

    /* Disassembly */
    MoveWindow(d->follow_pc, px, py + 2, 90, 22, TRUE);
    MoveWindow(d->jump_label, px + 96, py + 4, 50, 18, TRUE);
    MoveWindow(d->jump, px + 148, py, 150, 24, TRUE);
    MoveWindow(d->disasm_hint, px + 308, py + 4, pw - 308 > 40 ? pw - 308 : 40, 18, TRUE);
    MoveWindow(d->disasm, px, py + 30, pw, ph - 30, TRUE);

    /* Video: the 1861's state left; the view and its picture right */
    {
        const int lw = pw * 45 / 100, rx = px + lw + 10, rw = pw - lw - 10;
        MoveWindow(d->video_text, px, py, lw, ph, TRUE);
        MoveWindow(d->video_view_label, rx, py + 4, 40, 18, TRUE);
        MoveWindow(d->video_view, rx + 42, py, 220, 200, TRUE);
        MoveWindow(d->video_pic, rx, py + 30, rw, ph - 30, TRUE);
    }

    /* Sound & I/O: the text left, the keypad diagrams right */
    {
        const int kw = 2 * KEYPAD_W + KEYPADS_GAP + 4;
        const int lw = pw - kw - 16 > 300 ? pw - kw - 16 : 300, rx = px + lw + 16;
        MoveWindow(d->io_text, px, py, lw, ph, TRUE);
        MoveWindow(d->io_keys_label, rx, py, kw + 40, 18, TRUE);
        MoveWindow(d->io_keys, rx, py + 24, kw, 22 + 4 * (KEY_CELL_H + KEY_GAP) + 44, TRUE);
    }

    /* Breakpoints */
    {
        int ry = py + ph - 62;
        MoveWindow(d->bp_list, px, py, pw, ry - py - 6, TRUE);
        MoveWindow(d->bp_type, px, ry, 100, 200, TRUE);
        MoveWindow(d->bp_start_label, px + 108, ry + 4, 50, 18, TRUE);
        MoveWindow(d->bp_start, px + 160, ry, 90, 24, TRUE);
        MoveWindow(d->bp_end_label, px + 256, ry + 4, 18, 18, TRUE);
        MoveWindow(d->bp_end, px + 276, ry, 90, 24, TRUE);
        MoveWindow(d->bp_cond_label, px + 374, ry + 4, 14, 18, TRUE);
        MoveWindow(d->bp_cond, px + 390, ry, pw - 390 - 70 > 80 ? pw - 390 - 70 : 80, 24, TRUE);
        MoveWindow(d->bp_add, px + pw - 64, ry - 1, 64, 26, TRUE);
        ry += 32;
        MoveWindow(d->bp_remove, px, ry, 90, 26, TRUE);
        MoveWindow(d->bp_enable, px + 96, ry, 120, 26, TRUE);
        MoveWindow(d->bp_clear, px + 222, ry, 90, 26, TRUE);
    }

    /* Cart: the state left; the page map and the text engine right (wide
     * enough for the map's legend on one line) */
    {
        const int label_w = d->mono_w * 6 + 4;
        const int grid_w = 16 * (d->mono_w + 10), grid_h = 10 * (d->mono_h + 8);
        const int rw = label_w + grid_w > 58 * d->mono_w ? label_w + grid_w : 58 * d->mono_w;
        const int lw = pw - rw - 16 > 300 ? pw - rw - 16 : 300, rx = px + lw + 16;
        const int map_h = d->mono_h + 2 + 16 * (d->mono_h + 2) + d->mono_h + 10;
        MoveWindow(d->cart, px, py, lw, ph, TRUE);
        MoveWindow(d->cart_map_label, rx, py, rw, 18, TRUE);
        MoveWindow(d->cart_map, rx, py + 22, rw, map_h, TRUE);
        MoveWindow(d->cart_grid_label, rx, py + 22 + map_h + 12, rw, 18, TRUE);
        /* under the map's cells */
        MoveWindow(d->cart_grid, rx + label_w + (rw - label_w - grid_w) / 2, py + 22 + map_h + 34,
                   grid_w, grid_h, TRUE);
    }

    /* FujiNet: the packets left (a note and Clear over them), the mailbox
     * right */
    {
        const int rw = pw / 3 > 320 ? 320 : pw / 3 > 220 ? pw / 3 : 220;
        const int lw = pw - rw - 12;
        MoveWindow(d->fuji_title, px, py, lw, 18, TRUE);
        MoveWindow(d->fuji_clear, px + lw - 70, py + 22, 70, 26, TRUE);
        MoveWindow(d->fuji_note, px, py + 26, lw - 78, 18, TRUE);
        MoveWindow(d->fuji_text, px, py + 54, lw, ph - 54, TRUE);
        MoveWindow(d->state_title, px + lw + 12, py, rw, 18, TRUE);
        MoveWindow(d->fuji_state, px + lw + 12, py + 22, rw, ph - 22, TRUE);
    }
}

/* ---- attach / detach -------------------------------------------------------------- */

static void attach(debugger *d)
{
    o2debug_attach(d->dbg);
    d->seen_gen = o2debug_generation(d->dbg);
    d->was_stopped = 1;
    refresh_all(d);
    SetTimer(d->hwnd, TIMER_REFRESH, 100, NULL);
}

static void detach_and_hide(debugger *d)
{
    KillTimer(d->hwnd, TIMER_REFRESH);
    o2debug_detach(d->dbg);
    ShowWindow(d->hwnd, SW_HIDE);
}

/* ---- window proc -------------------------------------------------------------------- */

/* Stopped, Run wears the accent -- with dark text: the tan is too pale to
 * carry white. */
static void draw_run_button(debugger *d, const DRAWITEMSTRUCT *di)
{
    char text[32];
    const int stopped = o2debug_is_stopped(d->dbg);
    RECT r = di->rcItem;
    if (stopped) {
        FillRect(di->hDC, &r, d->accent);
        FrameRect(di->hDC, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(di->hDC, RGB(0, 0, 0));
    } else {
        DrawFrameControl(di->hDC, &r, DFC_BUTTON, DFCS_BUTTONPUSH | ((di->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0));
        SetTextColor(di->hDC, GetSysColor(COLOR_BTNTEXT));
    }
    SetBkMode(di->hDC, TRANSPARENT);
    GetWindowTextA(di->hwndItem, text, sizeof text);
    DrawTextA(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(di->hDC, &r);
    }
}

static LRESULT CALLBACK dbg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debugger *d = g_dbg;
    if (!d || d->hwnd != hwnd) return DefWindowProcA(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        layout(d);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd)) {
            const unsigned gen = o2debug_generation(d->dbg);
            const int stopped = o2debug_is_stopped(d->dbg);
            if (gen != d->seen_gen || stopped != d->was_stopped) {
                d->seen_gen = gen;
                d->was_stopped = stopped;
                refresh_all(d);
            } else if (!stopped && ++d->running_ticks >= 5) {
                /* running: the live pages twice a second (every o2debug
                 * call takes the host's run lock, so this is safe) */
                d->running_ticks = 0;
                refresh_status(d);
                if (d->page == PAGE_CPU) { refresh_cpu(d); refresh_ram(d); }
                else if (d->page == PAGE_DISASM) refresh_disasm(d);
                else if (d->page == PAGE_VIDEO) refresh_video(d);
                else if (d->page == PAGE_IO) refresh_io(d);
                else if (d->page == PAGE_BREAKS) refresh_bps(d);
                else if (d->page == PAGE_CART) refresh_cart(d);
                else if (d->page == PAGE_FUJINET) refresh_fujinet(d, 0);
            }
        }
        return 0;
    case WM_DBG_ACCEPT:
        on_accept(d, (int)wp);
        return 0;
    case WM_DBG_TAB:
        complete_prompt(d);
        return 0;
    case WM_DBG_HISTORY:
        history_step(d, (int)wp);
        return 0;
    case WM_NOTIFY:
        if (((LPNMHDR)lp)->code == (UINT)TCN_SELCHANGE) {
            show_page(d, (int)SendMessageA(d->tabs, TCM_GETCURSEL, 0, 0));
            layout(d);
            if (d->page == PAGE_FUJINET) refresh_fujinet(d, 0);
            return 0;
        }
        break;
    case WM_DRAWITEM:
        if (wp == IDC_RUN) { draw_run_button(d, (const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d->status || (HWND)lp == d->disasm_hint || (HWND)lp == d->fuji_note) {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_FLAG0 && id <= IDC_FLAG_LAST && HIWORD(wp) == BN_CLICKED) {
            if (o2debug_is_stopped(d->dbg))
                o2debug_cpu_set(d->dbg, kFlagIds[id - IDC_FLAG0],
                                     SendMessageA(d->flag[id - IDC_FLAG0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            refresh_cpu(d);
            return 0;
        }
        if (id >= IDC_SAVE_KIND0 && id <= IDC_SAVE_KIND_LAST) { save_file(d, id - IDC_SAVE_KIND0); return 0; }
        switch (id) {
        case IDC_RUN: toggle_run(d); return 0;
        case IDC_STEP: o2debug_step(d->dbg); refresh_all(d); return 0;
        case IDC_OVER: o2debug_step_over(d->dbg); refresh_all(d); return 0;
        case IDC_OUT: o2debug_step_out(d->dbg); refresh_all(d); return 0;
        case IDC_SCAN: o2debug_scanline(d->dbg, 1); refresh_all(d); return 0;
        case IDC_FRAME: o2debug_frame(d->dbg, 1); refresh_all(d); return 0;
        case IDC_LOAD_SYMBOLS: symbols_menu(d); return 0;
        case IDC_SYMS_FILE: load_symbols(d, 0); return 0;
        case IDC_SYMS_CART: load_symbols(d, 1); return 0;
        case IDC_SAVE: save_menu(d); return 0;
        case IDC_FOLLOW_PC: refresh_disasm(d); return 0;
        case IDC_VIDEO_VIEW:
            if (HIWORD(wp) == CBN_SELCHANGE) refresh_video(d);
            return 0;
        case IDC_BP_ADD: add_breakpoint(d); return 0;
        case IDC_BP_REMOVE:
        case IDC_BP_ENABLE: {
            int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < d->nbps) {
                if (id == IDC_BP_REMOVE) o2debug_breakpoint_remove(d->dbg, d->bps[sel].id);
                else o2debug_breakpoint_enable(d->dbg, d->bps[sel].id, !d->bps[sel].enabled);
                refresh_bps(d);
                refresh_disasm(d);
            }
            return 0;
        }
        case IDC_BP_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                int sel = (int)SendMessageA(d->bp_list, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < d->nbps &&
                    !(d->bps[sel].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT))) {
                    char a[16];
                    snprintf(a, sizeof a, "$%04X", d->bps[sel].start);
                    TabCtrl_SetCurSel(d->tabs, PAGE_DISASM);
                    show_page(d, PAGE_DISASM);
                    layout(d);
                    jump_to(d, a);
                }
            }
            return 0;
        case IDC_BP_CLEAR:
            o2debug_breakpoint_clear(d->dbg);
            refresh_bps(d);
            refresh_disasm(d);
            return 0;
        case IDC_FUJI_CLEAR:
            o2debug_fujinet_trace_clear(d->dbg);
            refresh_fujinet(d, 1);
            return 0;
        case IDCANCEL:
            detach_and_hide(d);
            return 0;
        default: break;
        }
        break;
    }
    case WM_CLOSE:
        /* Hide and let the machine run; F12 brings it back, stopped. */
        detach_and_hide(d);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        DeleteObject(d->mono);
        DeleteObject(d->accent);
        DeleteObject(d->accent_dark);
        DestroyAcceleratorTable(d->accel);
        free(d->pic_px);
        free(d);
        g_dbg = NULL;
        return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- entry points ---------------------------------------------------------------------- */

static void register_class(HINSTANCE inst, const char *name, WNDPROC proc, HBRUSH background)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = background;
    wc.lpszClassName = name;
    RegisterClassA(&wc);
}

/* O2_DEBUGGER_TAB: a page number, or its name (prompt, cpu,
 * disassembly, video, io, breakpoints, cart, fujinet). */
static int page_from_env(void)
{
    const char *tab = getenv("O2_DEBUGGER_TAB");
    int i;
    if (!tab || !*tab) return PAGE_PROMPT;
    if (tab[0] >= '0' && tab[0] <= '9') {
        i = atoi(tab);
        return (i >= 0 && i < PAGE_COUNT) ? i : PAGE_PROMPT;
    }
    for (i = 0; i < PAGE_COUNT; i++)
        if (lstrcmpiA(tab, kPageNames[i]) == 0) return i;
    return PAGE_PROMPT;
}

void o2_debugger_show(HWND parent, o2session *session)
{
    HINSTANCE inst;
    debugger *d;
    ACCEL accels[6];

    if (g_dbg) {
        if (!IsWindowVisible(g_dbg->hwnd)) {
            ShowWindow(g_dbg->hwnd, SW_SHOW);
            attach(g_dbg);
        } else {
            o2debug_stop(g_dbg->dbg);
            refresh_all(g_dbg);
        }
        SetForegroundWindow(g_dbg->hwnd);
        return;
    }

    inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    register_class(inst, "O2DebuggerWindow", dbg_proc, (HBRUSH)(COLOR_BTNFACE + 1));
    register_class(inst, "O2DbgPixels", pic_proc, NULL);
    register_class(inst, "O2DbgDisasm", disasm_proc, NULL);
    register_class(inst, "O2DbgKeys", keys_proc, NULL);
    register_class(inst, "O2DbgCartMap", cart_map_proc, NULL);
    register_class(inst, "O2DbgCartGrid", cart_grid_proc, NULL);

    d = calloc(1, sizeof *d);
    if (!d) return;
    d->session = session;
    d->dbg = o2session_debugger(session);
    d->fuji_count = -1;
    d->pic_px = calloc(O2DEBUG_VIEW_MAX_PIXELS, sizeof *d->pic_px);
    g_dbg = d;

    d->hwnd = CreateWindowExA(0, "O2DebuggerWindow", "Debugger", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820, NULL, NULL, inst, NULL);
    if (!d->hwnd) { free(d->pic_px); free(d); g_dbg = NULL; return; }

    d->ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    d->mono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    {
        HDC dc = GetDC(d->hwnd);
        TEXTMETRICA tm;
        HGDIOBJ old = SelectObject(dc, d->mono);
        d->mono_w = 7;
        d->mono_h = 15;
        if (GetTextMetricsA(dc, &tm)) {
            d->mono_w = tm.tmAveCharWidth;
            d->mono_h = tm.tmHeight;
        }
        SelectObject(dc, old);
        ReleaseDC(d->hwnd, dc);
    }
    d->accent = make_brush(O2SESSION_ACCENT_RGB);
    d->accent_dark = make_brush(O2SESSION_ACCENT_DARK_RGB);

    build_controls(d);

    /* F5/F7/F8/Shift+F8/F12 work wherever the focus is inside the window. */
    accels[0].fVirt = FVIRTKEY;          accels[0].key = VK_F5;  accels[0].cmd = IDC_RUN;
    accels[1].fVirt = FVIRTKEY;          accels[1].key = VK_F7;  accels[1].cmd = IDC_STEP;
    accels[2].fVirt = FVIRTKEY;          accels[2].key = VK_F8;  accels[2].cmd = IDC_OVER;
    accels[3].fVirt = FVIRTKEY | FSHIFT; accels[3].key = VK_F8;  accels[3].cmd = IDC_OUT;
    accels[4].fVirt = FVIRTKEY;          accels[4].key = VK_F12; accels[4].cmd = IDCANCEL;
    accels[5].fVirt = FVIRTKEY | FSHIFT; accels[5].key = VK_F7;  accels[5].cmd = IDC_FRAME;
    d->accel = CreateAcceleratorTableA(accels, 6);

    d->page = page_from_env();
    SendMessageA(d->tabs, TCM_SETCURSEL, (WPARAM)d->page, 0);
    show_page(d, d->page);
    layout(d);

    ShowWindow(d->hwnd, SW_SHOW);
    attach(d);
}

int o2_debugger_pretranslate(MSG *msg)
{
    if (!g_dbg || !g_dbg->accel) return 0;
    if (msg->hwnd != g_dbg->hwnd && !IsChild(g_dbg->hwnd, msg->hwnd)) return 0;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_F12) { detach_and_hide(g_dbg); return 1; }
    return TranslateAcceleratorA(g_dbg->hwnd, g_dbg->accel, msg) ? 1 : 0;
}

int o2_debugger_visible(void)
{
    return g_dbg && IsWindowVisible(g_dbg->hwnd);
}

void o2_debugger_toggle(HWND parent, o2session *session)
{
    if (o2_debugger_visible()) detach_and_hide(g_dbg);
    else o2_debugger_show(parent, session);
}

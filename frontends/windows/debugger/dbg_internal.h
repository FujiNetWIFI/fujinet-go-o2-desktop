/*
 * What the debugger window's two files share: the window's state and the
 * views that dbg_views.c draws by hand. Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include <stdint.h>

#include "o2debug.h"
#include "../win_common.h"

#define DISASM_MAX 128       /* rows fetched at most; the view shows what fits */
#define MAX_BPS 128
#define HISTORY_MAX 64
#define MAX_CTL 256
#define OBJ_COUNT 32         /* bitmaps: 4 sprites, 12 singles, 16 quad characters */
#define OBJ_BM_PX (64 * 64)  /* the biggest object bitmap (a zoomed sprite at scale 2) */

enum {
    PAGE_PROMPT = 0, PAGE_CPU, PAGE_DISASM, PAGE_VIDEO, PAGE_EF, PAGE_IO, PAGE_BREAKS, PAGE_CART,
    PAGE_FUJINET, PAGE_COUNT
};

/* The Video tab's sections (the strip beside the picture). */
enum { SEC_OBJECTS = 0, SEC_GRID, SEC_REGS, SEC_CTRL, SEC_SOUND, SEC_SHEETS, SEC_COUNT };

/* The EF9340 tab shows either its decode or the "G7400 only" notice. */
enum { EFSEC_PRESENT = 0, EFSEC_ABSENT };

#define NFIELDS 9            /* A PC MB PSW SP Timer P1 P2 BUS */
#define NFLAGS 5             /* C AC F0 F1 BS: editable */
#define NINPUTS 7            /* TF, EN TCNTI, EN I, in interrupt, T0, T1, INT: read-only */
#define NLAYERS 7
#define NOVERLAYS 6
#define NSAVE 7
#define NSHEETS 3

/* An XRGB picture the engine drew (0x00RRGGBB, top-down: a 32-bit BI_RGB DIB as it is). */
typedef struct {
    uint32_t *px;
    size_t cap;
    int w, h;
} image;

/* What a pixel window (O2DbgPic) shows; its GWLP_USERDATA points at one. */
typedef struct {
    image img;
    int center;               /* centred and as large as fits, else top-left at 1:1 */
} picview;

/* Every control the window owns, with the page (and section) it belongs to,
 * so a page change is one loop and a DPI change remakes every font. */
typedef struct {
    HWND h;
    int page;
    int sec;                  /* -1: the whole page; else the Video section / EF9340 state */
    int mono;
} ctl_t;

typedef struct {
    HWND hwnd;
    HWND tabs;
    HINSTANCE inst;
    HWND run_btn, step_btn, over_btn, out_btn, scan_btn, frame_btn, load_symbols, save, status;

    HWND prompt_out, prompt_in;

    HWND fld_label[NFIELDS], fld[NFIELDS], flag[NFLAGS], input[NINPUTS];
    HWND bank_head[2], bank_label[16], bank_edit[16], cpu_state;
    HWND iram_label, iram_view, xram_label, xram_view, ram_space, ram_goto_label, ram_goto;
    HWND ram_addr_label, ram_addr, ram_val_label, ram_val;

    HWND follow_pc, jump_label, jump, disasm_hint, disasm;

    HWND view_label, view_mode, layers_label, layer[NLAYERS], overlays_label, overlay[NOVERLAYS];
    HWND vpic, vstatus, vtabs;
    HWND objects, grid, regs_list, reg_addr_label, reg_addr, reg_val_label, reg_val, reg_write;
    HWND ctrl_text, sound_view, sound_text, sheet_label[NSHEETS], sheet[NSHEETS];

    HWND ef_none, ef_text, ef_grid, ef_pic, ef_status;

    HWND io_text, io_matrix;

    HWND bp_list, bp_type, bp_space, bp_start_label, bp_start, bp_end_label, bp_end, bp_cond_label, bp_cond;
    HWND bp_add, bp_remove, bp_enable, bp_clear;

    HWND cart_text, cart_mailbox_label, cart_mailbox;

    HWND fuji_title, fuji_note, fuji_clear, fuji_text, state_title, fuji_state;
    uint64_t fuji_last;
    int fuji_count;

    ctl_t ctl[MAX_CTL];
    int nctl;

    HFONT ui, bold, mono;
    int dpi;
    int ui_h;                 /* the UI font's line height */
    int row_h;                /* an edit / button / combo row */
    int mono_w, mono_h;       /* the mono font's cell, for the hand-drawn views */
    HACCEL accel;
    HBRUSH accent, accent_dark;

    o2session *session;
    o2debug *dbg;

    int page, vsec, ef_sec;
    unsigned seen_gen;
    int was_stopped;
    int running_ticks;

    /* the disassembly */
    int disasm_top;
    o2debug_line lines[DISASM_MAX];
    int line_count;
    int pc_line;

    /* breakpoints */
    o2debug_breakpoint bps[MAX_BPS];
    int nbps;

    /* the CPU, the I/O and the cartridge as last read */
    o2debug_cpu cpu;
    o2debug_io io;
    o2debug_cart cart_state;

    /* the prompt's history */
    char history[HISTORY_MAX][256];
    int nhistory, history_pos;

    /* the 8244 */
    i8244_snapshot snap;
    image vimg;               /* the Video tab's picture (scale 1) */
    uint32_t pal[16];         /* the 16 palette colours, sampled from the palette strip */
    image palimg;
    uint32_t *obj_bm[OBJ_COUNT];
    int obj_w[OBJ_COUNT], obj_h[OBJ_COUNT];
    int obj_scroll;
    int hover_valid, hover_x, hover_y;
    o2debug_hit hits[8];
    int nhits;
    char vdc_regname[256][48];
    char vdc_regtext[256][112];
    picview sheet_pv[NSHEETS];
    int sheet_scale[NSHEETS];

    /* the EF9340 */
    ef9340_snapshot_t ef;
    picview ef_pv;
    int ef_hover_row, ef_hover_col;
    char ef_hover_text[160];

} debugger;

/* The one window, for the views' window procs. */
extern debugger *g_debugger;

/* Lays out in 96-dpi units scaled by the window's DPI. */
#define S(v) o2_scale((v), d->dpi)

/* In dbg_window.c, for the views. */
void dbg_wide_from(const char *utf8, wchar_t *w, int n);
void dbg_label_set(HWND h, const char *utf8);
void dbg_refresh_disasm(debugger *d);
void dbg_update_hover(debugger *d);

/* In dbg_views.c, for the window. */
void dbg_views_register(HINSTANCE inst);
void dbg_objects_scroll_update(debugger *d);
void dbg_ef_cell(const debugger *d, int *cw, int *ch);
void dbg_matrix_size(const debugger *d, int *w, int *h);

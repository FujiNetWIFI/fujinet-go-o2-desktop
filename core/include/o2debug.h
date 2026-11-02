/*
 * o2debug -- the debugger contract: a CDP1802 / CDP1861 debugger engine
 * over the core (core/debugger/) behind a C API the four native debugger
 * windows share. Same shape as the SMS sibling's smsdebug.h, so the windows
 * carry over; the engine is that one with the Z80 and the VDP traded for
 * the COSMAC, the Pixie and the FujiNet cartridge's text engine.
 *
 * The engine only engages while a window holds it attached:
 * o2debug_attach() when the window opens -- which also stops the
 * machine, as on every sibling -- and o2debug_detach() when it closes,
 * which lets the machine run on at full speed with no hook installed. The
 * FujiNet trace is the exception: it records from session start, so it shows
 * what happened before anyone opened a window.
 *
 * Calls are safe from the UI thread at any time. Inspection is meant for the
 * stopped state; while running it answers with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2DEBUG_H
#define O2DEBUG_H

#include <stdint.h>

#include "o2session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lazily created; lives as long as the session. */
o2debug *o2debug_get(o2session *s);

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. Idempotent. */
void o2debug_attach(o2debug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. */
void o2debug_detach(o2debug *d);
int  o2debug_is_attached(o2debug *d);

int  o2debug_is_stopped(o2debug *d);
void o2debug_stop(o2debug *d);         /* break at the next instruction */
void o2debug_resume(o2debug *d);       /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $0412",
 * "write $0900 = $12", "step", "frame", ...) and the PC, -1 if none.
 * Returns length. */
int  o2debug_stop_reason(o2debug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned o2debug_generation(o2debug *d);

/* ---- the prompt ------------------------------------------------------------ */
/* Run one command; "help" lists them: step [n], over, out, frame [n],
 * scanline [n], run, stop, runto <addr>, break <addr> [cond], bpr/bpw
 * <addr>[-<end>] [cond], bpin/bpout <port>[-<end>] [cond], delete <id|addr>,
 * enable/disable <id>, clear, breaks, print <expr>, regs, set <reg> <expr>,
 * mem <addr> [len], poke <addr> <val...>, video, io, keys, cart, mailbox,
 * text, label <addr> <name>, labels [file], disasm [addr] [n], trace
 * [on|off|n], save <kind> <file>. Expressions: numbers ($0400, 0x400, 400h,
 * 1024), registers (r0-r15, their halves r3.0 / r3.1, pc d b t p x df ie q,
 * ef1-ef4), [addr] byte and {addr} word reads (high byte first, as the
 * 1802 keeps them), labels, vpos hpos frame cycles keylatch, and in a
 * breakpoint condition `value` and `addr` for the access; C operators with C
 * precedence. Output text into dst; returns its length. */
int  o2debug_command(o2debug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands, registers and labels), one per line.
 * Returns the count. */
int  o2debug_completions(o2debug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
void o2debug_step(o2debug *d);          /* F7: one instruction */
/* F8: over a subroutine call -- a SEP to another register, run until the
 * caller's P comes back with R(P) just past it (or past an SCRT call's
 * inline DW target, +3) */
void o2debug_step_over(o2debug *d);
/* Shift+F8: to the routine's return -- with P = 3 under the standard call
 * technique (SCRT), to R6's return address with P = 3 again; otherwise to
 * the first SEP, RET or DIS that changes P */
void o2debug_step_out(o2debug *d);
void o2debug_scanline(o2debug *d, int n);
void o2debug_frame(o2debug *d, int n);
/* Run until PC == addr. */
void o2debug_run_to(o2debug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int r[16];
    int pc;                                /* R(P): the instruction about to run */
    int p, x, d, b, t;
    int df, ie, q;
    int ef[4];                             /* EF1-EF4 as the CPU sees them now */
    int state;                             /* the COSMAC state machine */
    const char *state_name;                /* "fetch", "execute", "DMA out", ... */
    int mode;                              /* CLEAR/WAIT: load, reset, pause, run */
    const char *mode_name;
    int int_line, dma_line;                /* the 1861's INT and DMA OUT */
    uint64_t cycles;                       /* clocks since power-on */
    uint64_t instructions;
    int vpos, hpos;                        /* where the beam is */
    uint32_t frame;
} o2debug_cpu;
void o2debug_cpu_get(o2debug *d, o2debug_cpu *out);
typedef enum {
    O2_REG_R0 = 0,        /* ... O2_REG_R0 + 15 */
    O2_REG_PC = 16,       /* R(P) */
    O2_REG_P, O2_REG_X, O2_REG_D, O2_REG_B, O2_REG_T,
    O2_REG_DF, O2_REG_IE, O2_REG_Q,
    O2_REG_COUNT
} o2debug_reg;
void o2debug_cpu_set(o2debug *d, int reg, int value);

/* ---- video: the CDP1861 ------------------------------------------------------ */
typedef struct {
    int display_on;               /* INP 1 turned it on */
    int int_line, efx_line, dma_line;
    int vpos, hpos;
    int in_display;               /* the beam is on lines 80-207 */
    uint16_t dma_pointer;         /* R0: the next DMA byte's address */
    uint16_t last_dma_addr;
    uint64_t dma_bytes;           /* bytes fetched since power-on */
    uint32_t inp1_count;          /* display-on strobes */
    int next_int_line, next_efx_line;   /* where the 1861's next events fall */
    uint64_t next_int_clock, next_efx_clock, next_dma_clock;
    uint32_t frame;
    uint32_t short_lines;         /* DMA bursts under 8 (a $Cx opcode in flight) */
} o2debug_video;
void o2debug_video_get(o2debug *d, o2debug_video *out);

/* The video views, as XRGB images. Returns 1 when drawn.
 *   SCREEN:  112x262, the whole raster MAME's bitmap holds (blanking
 *            included) with the visible area and the beam marked.
 *   RASTER:  64x128, the cartridge's served raster (what a FujiNet app
 *            draws through the text engine), whether or not it is on screen.
 *   RAM:     64x32, the BIOS's display page ($0900-$09FF) as the BIOS
 *            draws it, 4 lines a row. */
typedef enum { O2DEBUG_VIEW_SCREEN = 0, O2DEBUG_VIEW_RASTER,
               O2DEBUG_VIEW_RAM } o2debug_view;
#define O2DEBUG_VIEW_MAX_PIXELS (112 * 262)
int  o2debug_video_view(o2debug *d, int view, uint32_t *dst, int *width, int *height);

/* ---- sound and I/O ------------------------------------------------------------ */
typedef struct {
    int q;                        /* the beeper is on (Q) */
    int beep_hz;
    uint8_t keylatch;             /* the key OUT 2 selected (0-15) */
    uint16_t keys[2];             /* keypads A and B as latched, bit n = key n */
    int ef3, ef4;                 /* what the latched key reads on each */
    int clear_held;
    uint8_t last_out[8];          /* the last byte OUT to each port */
    uint32_t inp1_count;
    int have_bios;                /* else the stand-in */
    int stub_parked;              /* the stand-in met a game */
} o2debug_io;
void o2debug_io_get(o2debug *d, o2debug_io *out);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads without side effects (no hotspot fires; the raster reads 0). */
int  o2debug_read(o2debug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit: console RAM takes it; a page the cartridge serves takes
 * it in the cartridge's own buffer (the image or the mailbox arena), never
 * as a hotspot. */
void o2debug_write(o2debug *d, uint16_t addr, uint8_t value);
/* The console's 512 bytes of RAM ($0800-$09FF). */
void o2debug_ram_get(o2debug *d, uint8_t out[512]);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;
    int length;
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];        /* a branch target's label, a port's name */
} o2debug_line;
/* Up to `max` lines starting at `addr`. Returns the count; *pc_line is the
 * index of the PC's line in `out`, -1 when not among them. */
int  o2debug_disassemble(o2debug *d, uint16_t addr, o2debug_line *out,
                              int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down). Backwards
 * is a best guess. */
int  o2debug_row_address(o2debug *d, uint16_t addr, int rows);
/* Address of a label, or -1. Label of an address into dst (may be empty). */
int  o2debug_label_address(o2debug *d, const char *label);
int  o2debug_address_label(o2debug *d, uint16_t addr, char *dst, int dstsz);
int  o2debug_set_label(o2debug *d, uint16_t addr, const char *label);
/* Load a symbol file: Macroassembler AS (-g MAP's .map, or the -L listing's
 * symbol table), WLA-DX .sym, or a plain "AAAA name" list. With path NULL,
 * <cart>.map/.lst/.sym next to the opened cartridge. Message into msg;
 * returns labels loaded or -1. */
int  o2debug_load_symbols(o2debug *d, const char *path, char *msg, int msgsz);

/* ---- the cartridge ------------------------------------------------------------ */
typedef struct {
    int present, link_up, busy, direct, booted_game;
    int mailbox_mode;        /* a socket and fujimail (else a game alone) */
    int mode;                /* FN_MODE_*: 0 CONFIG, 2 game, 3 app (mailbox live) */
    const char *mode_name;
    int live_mailbox, live_st2, live_blocks;
    uint32_t live_size, live_crc;
    int staged, staged_claim, staged_config, armed;
    uint32_t staged_crc;
    int swaps;
    int nbanks, bank;        /* the bank window at $C400: banks, the one shown */
    uint8_t page_type[256];  /* S2PG_*: what the cartridge serves per page */
    int boot_state, boot_pct, boot_err;
    uint32_t boot_got, boot_total;
    uint8_t ackseq, status, last_error, reply_cmd;
    uint16_t rxlen;
    uint8_t frames;          /* raster frames served, mod 256 */
    uint32_t short_lines;
    uint32_t queue_depth;
    /* the text engine as drawn */
    uint8_t text[10][16];
    uint8_t text_inv[10][16];
    int cursor_row, cursor_col, inverse;
} o2debug_cart;
void o2debug_cart_get(o2debug *d, o2debug_cart *out);
/* One line: "FujiNet cartridge: CONFIG, link up". */
int  o2debug_cart_info(o2debug *d, char *dst, int dstsz);

/* ---- FujiNet: the cartridge's socket, as FujiBus packets ------------------------ */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> Odyssey 2, 0: Odyssey 2 -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;
    uint8_t  data[64];      /* the decoded packet's first bytes */
    char     text[96];      /* "Odyssey 2 -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;        /* the same exchange back to back, shown "xN" */
} o2debug_fuji_packet;
/* Recorded from session start, attached or not. The newest `max`, oldest
 * first; returns the count. */
int  o2debug_fujinet_trace(o2debug *d, o2debug_fuji_packet *out, int max);
void o2debug_fujinet_trace_clear(o2debug *d);
/* The cartridge's link and mailbox, as text lines. Returns length. */
int  o2debug_fujinet_state(o2debug *d, char *dst, int dstsz);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    O2DEBUG_BP_EXEC = 1, O2DEBUG_BP_READ = 2, O2DEBUG_BP_WRITE = 4,
    O2DEBUG_BP_IN = 8, O2DEBUG_BP_OUT = 16
} o2debug_bp_type;
typedef struct {
    int id;
    int type;                /* o2debug_bp_type bits */
    uint16_t start, end;     /* a port range for IN/OUT (1-7) */
    int enabled;
    unsigned hits;
    char condition[128];
} o2debug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  o2debug_breakpoint_toggle(o2debug *d, uint16_t addr);
int  o2debug_breakpoint_check(o2debug *d, uint16_t addr);
/* Returns the new id, or -1 if the condition does not parse. */
int  o2debug_breakpoint_add(o2debug *d, int type, uint16_t start, uint16_t end,
                                 const char *condition);
void o2debug_breakpoint_remove(o2debug *d, int id);
void o2debug_breakpoint_enable(o2debug *d, int id, int enabled);
int  o2debug_breakpoint_list(o2debug *d, o2debug_breakpoint *out, int max);
void o2debug_breakpoint_clear(o2debug *d);

/* ---- the trace ring ----------------------------------------------------------- */
typedef struct {
    uint16_t pc;
    uint8_t p, x, d, df, ie, q;
    uint16_t rx;             /* R(X) */
    uint8_t len, bytes[3];
    uint64_t cycles;
    int16_t vpos, hpos;
} o2debug_trace;
void o2debug_trace_enable(o2debug *d, int on);
int  o2debug_trace_enabled(o2debug *d);
/* Newest first; returns entries copied. */
int  o2debug_trace_read(o2debug *d, o2debug_trace *out, int max);
void o2debug_trace_clear(o2debug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the BIOS
 * and every page the cartridge serves as code, as mapped now), "ram" (the
 * console's 512 bytes), "arena" (the mailbox, 2K), "raster" (the cart's
 * 64x128 raster, 1K), "screen" (the 1861's picture as a PPM), "regs" (CPU
 * and video state as text). Message into msg; returns 0 or -1. */
int  o2debug_save(o2debug *d, const char *kind, const char *path,
                       char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* O2DEBUG_H */

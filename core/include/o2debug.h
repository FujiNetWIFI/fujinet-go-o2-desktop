/*
 * o2debug -- the debugger contract: an MCS-48 / 8244 debugger engine over the
 * core (core/debugger/) behind a C API the four native debugger windows share.
 * Same shape as the Studio II and SMS siblings' debug headers, so the windows
 * carry over; the engine is that one with the COSMAC and the Pixie traded for
 * the 8048, the 8244/8245 VDC (and the G7400's EF9340, The Voice's SP0256) and
 * the FujiNet cartridge's mailbox.
 *
 * The engine only engages while a window holds it attached:
 * o2debug_attach() when the window opens -- which also stops the machine, as
 * on every sibling -- and o2debug_detach() when it closes, which lets the
 * machine run on at full speed with no hook installed. The FujiNet trace is
 * the exception: it records from session start, so it shows what happened
 * before anyone opened a window.
 *
 * Calls are safe from the UI thread at any time. Inspection is meant for the
 * stopped state; while running it answers with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2DEBUG_H
#define O2DEBUG_H

#include <stddef.h>
#include <stdint.h>

#include "o2session.h"
#include "i8244.h"      /* the VDC snapshot, layers and flags (core/o2) */
#include "ef9340.h"     /* the G7400's text chip snapshot */
#include "sp0256.h"     /* The Voice's chip state */

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
 * "write xdata $E3 = $12", "step", "frame", ...) and the PC, -1 if none.
 * Returns length. */
int  o2debug_stop_reason(o2debug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped/resumed, so a window
 * knows when to refresh. */
unsigned o2debug_generation(o2debug *d);

/* ---- the prompt ------------------------------------------------------------ */
/* Run one command; "help" lists them: step [n], over, out, frame [n],
 * scanline [n], run, stop, runto <addr>, break <addr> [cond], bpr/bpw
 * <addr>[-<end>] [cond] (program data), bpxr/bpxw (external data),
 * bpir/bpiw (internal RAM), bpin/bpout <p1|p2|bus> [cond], delete <id|addr>,
 * enable/disable <id>, clear, breaks, print <expr>, regs, set <reg> <expr>,
 * mem <space> <addr> [len] (prog, xdata, iram, vdc), poke <space> <addr>
 * <val...>, vdc, objects, ef, voice, io, keys, cart, mailbox, label <addr>
 * <name>, labels [file], disasm [addr] [n], trace [on|off|n], save <kind>
 * <file>. Expressions: numbers ($0400, 0x400, 400h, 1024), registers (a pc
 * psw sp r0-r7 r0'-r7' t cnt p1 p2 bus c ac f0 f1 bs mb), [addr] program-byte
 * reads and the spaces' x[addr] i[addr] v[addr], labels, vpos hpos frame
 * cycles, and in a breakpoint condition `value` and `addr` for the access;
 * C operators with C precedence. Output text into dst; returns its length. */
int  o2debug_command(o2debug *d, const char *command, char *dst, int dstsz);
/* Completions for a prefix (commands, registers and labels), one per line.
 * Returns the count. */
int  o2debug_completions(o2debug *d, const char *prefix, char *dst, int dstsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
void o2debug_step(o2debug *d);          /* F7: one instruction */
/* F8: over a CALL (run until the return address is reached again, with the
 * stack back where it was) */
void o2debug_step_over(o2debug *d);
/* Shift+F8: to the routine's RET / RETR */
void o2debug_step_out(o2debug *d);
void o2debug_scanline(o2debug *d, int n);
void o2debug_frame(o2debug *d, int n);
/* Run until PC == addr (12-bit program address). */
void o2debug_run_to(o2debug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int a;
    int pc;                                /* 12 bits: the instruction about to run */
    int mb;                                /* the A11 latch (SEL MB0/MB1) */
    int psw;                               /* raw: C AC F0 BS 1 SP2 SP1 SP0 as the 8048 keeps it */
    int cy, ac, f0, f1, bs, sp;            /* decoded; bs = register bank in use, sp = 0..7 */
    uint8_t iram[64];                      /* the 8048's internal RAM (both register banks first) */
    uint8_t r[2][8];                       /* R0-R7 of bank 0 and bank 1 */
    int timer, prescaler;                  /* the timer/counter and its /32 prescaler */
    int timer_mode;                        /* 0 stopped, 1 timer, 2 event counter (T1) */
    int tf;                                /* the timer-overflow flag (JTF) */
    int tcnti_en, ext_int_en;              /* EN TCNTI / EN I */
    int in_interrupt;                      /* an interrupt routine is running */
    int irq_pending, timer_irq_pending;
    int t0, t1, int_line;                  /* the input pins */
    int p1, p2, bus;                       /* the output latches */
    uint64_t cycles;                       /* machine cycles since power-on */
    uint64_t instructions;
    int vpos, hpos;                        /* where the beam is (NTSC/PAL lines, pixels) */
    uint32_t frame;
    int machine;                           /* o2_machine */
} o2debug_cpu;
void o2debug_cpu_get(o2debug *d, o2debug_cpu *out);
typedef enum {
    O2_REG_A = 0, O2_REG_PC, O2_REG_PSW, O2_REG_SP, O2_REG_T, O2_REG_CNT,
    O2_REG_P1, O2_REG_P2, O2_REG_BUS,
    O2_REG_C, O2_REG_AC, O2_REG_F0, O2_REG_F1, O2_REG_BS, O2_REG_MB,
    O2_REG_R0,            /* ... O2_REG_R0 + 7: the bank in use */
    O2_REG_R0_B1 = O2_REG_R0 + 8,   /* ... + 7: bank 1 */
    O2_REG_COUNT = O2_REG_R0_B1 + 8
} o2debug_reg;
void o2debug_cpu_set(o2debug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* The 8048's address spaces. Reads have no side effects (no mailbox hotspot,
 * no VDC read clearing a collision flag). */
typedef enum {
    O2_SPACE_PROG = 0,   /* 12-bit: BIOS or stand-in $000-$3FF, the cartridge $400-$FFF */
    O2_SPACE_XDATA,      /* external data $00-$FF: 128 bytes of RAM, the cart's registers, the VDC */
    O2_SPACE_IRAM,       /* internal RAM $00-$3F */
    O2_SPACE_VDC         /* the 8244's register file $00-$FF, raw */
} o2debug_space;
int  o2debug_read(o2debug *d, int space, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit: RAM takes it; a page the cartridge serves takes it in the
 * cartridge's own buffer (the image or the mailbox page), never as a hotspot;
 * the VDC takes it with MAME's data masks and ignores the write lockouts. */
void o2debug_write(o2debug *d, int space, uint16_t addr, uint8_t value);

/* ---- disassembly -------------------------------------------------------------- */
typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;
    int length;
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];        /* a branch target's label, a BIOS entry's name, a mailbox register */
} o2debug_line;
/* Up to `max` lines starting at `addr` (program space). Returns the count;
 * *pc_line is the index of the PC's line in `out`, -1 when not among them. */
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
 * <cart>.map/.lst/.sym next to the opened cartridge. Built-in labels: the BIOS
 * entry points (readkey, getjoystick, ...), the interrupt vectors and the
 * FujiNet mailbox registers and status page. Message into msg; returns labels
 * loaded or -1. */
int  o2debug_load_symbols(o2debug *d, const char *path, char *msg, int msgsz);

/* ---- video: the 8244/8245 and the G7400's EF9340 -------------------------------- */
/* Everything the chip knows: registers decoded, the four sprites, twelve
 * singles and four quads with their positions, the grid, collisions, sound
 * (i8244.h). */
void o2debug_vdc_get(o2debug *d, i8244_snapshot *out);
/* A register's name and a decoded value line (live). */
void o2debug_vdc_describe(o2debug *d, unsigned offset, char *name, size_t name_n,
                          char *text, size_t text_n);
/* A debugger store into the VDC (the machine must be stopped). */
void o2debug_vdc_write(o2debug *d, unsigned offset, uint8_t value);

/* The VDC views, as XRGB images (every one ends with the pixel size in *w,*h;
 * dst NULL asks for the size only). Returns 1 when drawn. layers is I8244_L_*
 * and flags I8244_F_* (boxes, collisions, overlaps, ...).
 *   FRAME:    the last frame the machine finished (what the display shows);
 *   RENDER:   the CURRENT register state re-rendered with the chosen layers
 *             and overlays (works while stopped, whatever the beam is doing);
 *   PROGRESS: the frame as far as the beam has got, the rest dimmed, with
 *             I8244_F_BEAM a cross-hair at the beam;
 *   FONT:     the 512-byte character generator as 64 glyphs;
 *   SHAPES:   the four sprite shape tables, in their colours;
 *   PALETTE:  the 16 colours. */
typedef enum {
    O2DEBUG_VDC_FRAME = 0, O2DEBUG_VDC_RENDER, O2DEBUG_VDC_PROGRESS,
    O2DEBUG_VDC_FONT, O2DEBUG_VDC_SHAPES, O2DEBUG_VDC_PALETTE
} o2debug_vdc_view_kind;
#define O2DEBUG_VDC_VIEW_MAX_PIXELS (I8244_WIDTH * I8244_HEIGHT)
int  o2debug_vdc_view(o2debug *d, int view, unsigned layers, unsigned flags,
                      int scale, uint32_t *dst, size_t cap, int *w, int *h);
/* One object as the chip draws it, for the tables: kind 0 sprite (0..3), 1
 * single (0..11), 2 quad member (0..15), 3 font glyph (0..63). */
int  o2debug_vdc_object(o2debug *d, int kind, int index, int scale, uint32_t bg,
                        uint32_t *dst, size_t cap, int *w, int *h);
/* What object (if any) draws at a pixel of the 360x243 frame: kind as above
 * (-1 none), index, topmost first, written into out[] (max entries). */
typedef struct { int kind, index; } o2debug_hit;
int  o2debug_vdc_hit_test(o2debug *d, int x, int y, o2debug_hit *out, int max);

/* The Videopac+ G7400's EF9340/41 text chip. present = 0 on the other
 * machines (a window shows "Videopac+ G7400 only"). */
int  o2debug_ef_present(o2debug *d);
void o2debug_ef_get(o2debug *d, ef9340_snapshot_t *out);
/* The text layer rendered alone (XRGB, W x H in *w,*h). */
int  o2debug_ef_view(o2debug *d, uint32_t *dst, size_t cap, int *w, int *h);

/* ---- sound and I/O ------------------------------------------------------------ */
typedef struct {
    int machine;
    int have_bios, have_font;            /* else the stand-in / a blank font */
    int stub_running;                    /* the stand-in is the BIOS now */
    int missing;                         /* O2_MISSING_* */
    /* P1 / P2 decoded by function (odyssey2.cpp) */
    int p1, p2;
    int cart_bank;                       /* P10, P11 */
    int kbd_enable;                      /* P12 low: the decoder and stick buffer on */
    int vdc_select;                      /* P13 low */
    int ram_select;                      /* P14 low: external RAM and the cartridge */
    int ef_select;                       /* P15 low (G7400) */
    int copy_mode;                       /* P16 high: external writes and VDC reads gated off */
    int lum;                             /* P17 low: every pixel brightened */
    int kbd_row;                         /* P20-P22 */
    /* inputs as the machine has them */
    uint64_t keys;                       /* the matrix, bit row*8+col */
    uint8_t sticks[2];                   /* bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 BUTTON */
    int reset_held;
    /* the 8244's sound is in the VDC snapshot; The Voice's: */
    int voice_present;
    sp0256_debug_t voice;
    int t0;                              /* the cart's T0 (The Voice's !LRQ) */
    uint64_t audio_samples;
} o2debug_io;
void o2debug_io_get(o2debug *d, o2debug_io *out);

/* ---- the cartridge ------------------------------------------------------------ */
typedef struct {
    int present;
    int type;                            /* enum o2_cart_type (carts.h) */
    const char *type_name;               /* "FujiNet", "Standard", "4-in-1", ... */
    int voice_in_front;
    /* a mapper cartridge */
    uint32_t rom_size;
    int bank, control;
    /* the FujiNet cartridge (type FujiNet) */
    int link_up, link_off, mailbox, app_live, booted_game, armed;
    int staged_app, staged_mailbox;
    uint32_t live_size, live_crc, staged_size, staged_crc;
    int swaps;
    int nbanks, fbank;                   /* legacy banks / APPBANK pages, and the one shown */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, last_error, reply_cmd;
    uint16_t rxlen;
    uint32_t queue_depth;
    uint8_t mailbox_page[0xE0];          /* $F20-$FFF as the console reads it */
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
    int space;               /* o2debug_space for READ/WRITE; for IN/OUT: 1 = P1, 2 = P2, 0 = BUS */
    uint16_t start, end;     /* an address range */
    int enabled;
    unsigned hits;
    char condition[128];     /* may use value, addr, registers, flags, vpos, hpos, the VDC's status */
} o2debug_breakpoint;
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  o2debug_breakpoint_toggle(o2debug *d, uint16_t addr);
int  o2debug_breakpoint_check(o2debug *d, uint16_t addr);
/* Returns the new id, or -1 if the condition does not parse. */
int  o2debug_breakpoint_add(o2debug *d, int type, int space, uint16_t start,
                            uint16_t end, const char *condition);
void o2debug_breakpoint_remove(o2debug *d, int id);
void o2debug_breakpoint_enable(o2debug *d, int id, int enabled);
int  o2debug_breakpoint_list(o2debug *d, o2debug_breakpoint *out, int max);
void o2debug_breakpoint_clear(o2debug *d);

/* ---- the trace ring ----------------------------------------------------------- */
typedef struct {
    uint16_t pc;
    uint8_t a, psw, sp, r0, r1;
    uint8_t len, bytes[2];
    uint64_t cycles;
    int16_t vpos, hpos;
} o2debug_trace;
void o2debug_trace_enable(o2debug *d, int on);
int  o2debug_trace_enabled(o2debug *d);
/* Newest first; returns entries copied. */
int  o2debug_trace_read(o2debug *d, o2debug_trace *out, int max);
void o2debug_trace_clear(o2debug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the BIOS and
 * the cartridge as code, as mapped now), "ram" (the 64 internal and 128
 * external bytes), "mailbox" (the cart's $F20-$FFF page), "vdc" (the 256
 * register bytes), "font" (the character generator), "screen" (the frame as a
 * PPM), "regs" (CPU, VDC and I/O state as text). Message into msg; returns 0
 * or -1. */
int  o2debug_save(o2debug *d, const char *kind, const char *path,
                       char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif /* O2DEBUG_H */

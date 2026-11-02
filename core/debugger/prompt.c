/*
 * prompt.c -- the debugger's command line (o2debug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "debugger_internal.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (o->len >= o->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(o->dst + o->len, (size_t)(o->cap - o->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        o->len += n < o->cap - o->len ? n : o->cap - o->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
    "break", "bpr", "bpw", "bpxr", "bpxw", "bpir", "bpiw", "bpvr", "bpvw", "bpin", "bpout", "delete",
    "enable", "disable", "clear", "breaks", "print", "regs", "set", "mem", "poke", "vdc",
    "objects", "ef", "voice", "io", "keys", "cart", "mailbox", "label", "labels",
    "disasm", "trace", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a CALL (or a DJNZ loop) (F8)\n"
    "out                    run to the routine's RET / RETR (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]   run until the beam has moved n lines / n frames\n"
    "break <addr>[-<end>] [cond]            execute breakpoint\n"
    "bpr | bpw <addr>[-<end>] [cond]        program-data read / write\n"
    "bpxr | bpxw <addr>[-<end>] [cond]      external data (MOVX) read / write\n"
    "bpir | bpiw <addr>[-<end>] [cond]      internal RAM read / write\n"
    "bpvr | bpvw <addr>[-<end>] [cond]      VDC register read / write (VDC selected)\n"
    "bpin | bpout <p1|p2|bus> [cond]        IN / OUT on a port\n"
    "delete <id|addr> | enable <id> | disable <id> | clear | breaks\n"
    "print <expr>           e.g. print r3, print x[$E3], print v[$A1] & 8\n"
    "regs                   CPU registers\n"
    "set <reg> <expr>       e.g. set pc $0412, set r2 $09, set mb 1, set f1 1\n"
    "mem [space] <addr> [len]    dump memory (side-effect free); space is\n"
    "                       prog (default), xdata, iram or vdc\n"
    "poke [space] <addr> <v...>  write memory (prog is read-only)\n"
    "vdc | objects          the 8244/8245: registers; sprites, characters, quads\n"
    "ef | voice             the G7400's EF9340; The Voice's SP0256\n"
    "io | keys              ports decoded; the keyboard matrix and the sticks\n"
    "cart | mailbox         the cartridge; the FujiNet mailbox page\n"
    "label <addr> [name] | labels [file]\n"
    "disasm [addr] [n]\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "save <dis|ram|mailbox|vdc|font|screen|regs> <file>\n"
    "Expressions: $0400 0x400 400h 1024 %1010; registers a pc psw sp r0-r7 (the\n"
    "bank in use) r0'-r7' (bank 1) t (timer) cnt (prescaler) p1 p2 bus c ac f0 f1\n"
    "bs mb; [addr] a program byte; x[addr] external data, i[addr] internal RAM,\n"
    "v[addr] the VDC (v[$A1] is its status); labels; vpos hpos frame cycles; in a\n"
    "condition, value and addr; C operators.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
    {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(o2debug *d, outbuf *o, const char *text, long *v)
{
    char err[96];
    if (o2debug_eval(d, text, v, err, sizeof err) != 0)
    {
        out(o, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(o2debug *d, outbuf *o, char *tok, long *a, long *b)
{
    char *dash = strchr(tok + 1, '-');
    if (dash)
    {
        *dash = '\0';
        if (eval(d, o, tok, a) || eval(d, o, dash + 1, b))
            return -1;
        return 0;
    }
    if (eval(d, o, tok, a))
        return -1;
    *b = *a;
    return 0;
}

/* prog, xdata, iram, vdc (and the short forms), else -1 */
static int space_by_name(const char *n)
{
    static const struct { const char *name; int space; } map[] = {
        {"prog", O2_SPACE_PROG}, {"p", O2_SPACE_PROG}, {"program", O2_SPACE_PROG},
        {"xdata", O2_SPACE_XDATA}, {"x", O2_SPACE_XDATA}, {"ext", O2_SPACE_XDATA},
        {"iram", O2_SPACE_IRAM}, {"i", O2_SPACE_IRAM}, {"ram", O2_SPACE_IRAM},
        {"vdc", O2_SPACE_VDC}, {"v", O2_SPACE_VDC},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].space;
    return -1;
}

static const char *const space_label[4] = { "prog", "xdata", "iram", "vdc" };

static void show_regs(o2debug *d, outbuf *o)
{
    o2debug_cpu c;
    o2debug_cpu_get(d, &c);
    out(o, "A=%02X  PC=%03X  MB=%d  PSW=%02X [C=%d AC=%d F0=%d F1=%d BS=%d SP=%d]\n", c.a, c.pc,
        c.mb, c.psw, c.cy, c.ac, c.f0, c.f1, c.bs, c.sp);
    for (int b = 0; b < 2; b++)
    {
        out(o, "bank %d%s:", b, b == c.bs ? " (in use)" : "");
        for (int i = 0; i < 8; i++)
            out(o, " R%d=%02X", i, c.r[b][i]);
        out(o, "\n");
    }
    out(o, "T=%02X (%s)  prescaler=%02X  TF=%d  TCNTI=%d  EN I=%d  in ISR=%d%s%s\n", c.timer,
        c.timer_mode == 1 ? "timer" : c.timer_mode == 2 ? "event counter" : "stopped", c.prescaler,
        c.tf, c.tcnti_en, c.ext_int_en, c.in_interrupt, c.irq_pending ? "  INT pending" : "",
        c.timer_irq_pending ? "  timer IRQ pending" : "");
    out(o, "P1=%02X P2=%02X BUS=%02X   T0=%d T1=%d INT=%d\n", c.p1, c.p2, c.bus, c.t0, c.t1,
        c.int_line);
    out(o, "line %d dot %d  frame %u  cycles %llu  instructions %llu\n", c.vpos, c.hpos, c.frame,
        (unsigned long long)c.cycles, (unsigned long long)c.instructions);
}

static void dump(outbuf *o, int space, uint16_t addr, const uint8_t *b, int n)
{
    const int w = space == O2_SPACE_PROG ? 3 : 2;
    const unsigned mask = space == O2_SPACE_PROG ? 0xfff : space == O2_SPACE_IRAM ? 0x3f : 0xff;

    for (int i = 0; i < n; i += 16)
    {
        out(o, "%0*X ", w, (addr + i) & mask);
        for (int j = 0; j < 16; j++)
            out(o, j < n - i ? " %02X" : "   ", b[i + j]);
        out(o, "  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

/* the registers the chips and the cartridge decode in a dumped range */
static void dump_names(o2debug *d, outbuf *o, int space, uint16_t addr, int n)
{
    if (space == O2_SPACE_XDATA)
    {
        for (int i = 0; i < n; i++)
        {
            const char *nm = o2_xdata_name((uint8_t)(addr + i));
            if (nm)
                out(o, "  %02X %s\n", (addr + i) & 0xff, nm);
        }
    }
    else if (space == O2_SPACE_VDC)
    {
        for (int i = 0; i < n; i++)
        {
            char name[48], text[160];
            const unsigned a = (addr + (unsigned)i) & 0xff;
            /* the control and status registers, where the byte means something
             * the hex does not say */
            if (a >= 0xa0 && a <= 0xaa)
            {
                o2debug_vdc_describe(d, a, name, sizeof name, text, sizeof text);
                out(o, "  %02X %-10s %s\n", a, name, text);
            }
        }
    }
}

static const char *space_name(int space)
{
    return space >= 0 && space < 4 ? space_label[space] : "?";
}

static void list_breaks(o2debug *d, outbuf *o)
{
    o2debug_breakpoint bp[128];
    int n = o2debug_breakpoint_list(d, bp, 128);
    if (!n)
    {
        out(o, "no breakpoints\n");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        char kinds[16] = "";
        if (bp[i].type & O2DEBUG_BP_EXEC)  strcat(kinds, "x");
        if (bp[i].type & O2DEBUG_BP_READ)  strcat(kinds, "r");
        if (bp[i].type & O2DEBUG_BP_WRITE) strcat(kinds, "w");
        if (bp[i].type & O2DEBUG_BP_IN)    strcat(kinds, "i");
        if (bp[i].type & O2DEBUG_BP_OUT)   strcat(kinds, "o");
        out(o, "#%d %-3s ", bp[i].id, kinds);
        if (bp[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT))
            out(o, "%s", bp[i].space == 1 ? "P1" : bp[i].space == 2 ? "P2" : "BUS");
        else
        {
            if (bp[i].type & (O2DEBUG_BP_READ | O2DEBUG_BP_WRITE))
                out(o, "%s ", space_name(bp[i].space));
            if (bp[i].start == bp[i].end)
                out(o, "$%03X", bp[i].start);
            else
                out(o, "$%03X-$%03X", bp[i].start, bp[i].end);
        }
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

/* break / bpr / bpw ... : "<addr>[-<end>] [condition]" in `space` */
static void add_bp(o2debug *d, outbuf *o, int type, int space, char *args)
{
    char *t = next_tok(&args);
    long a, b;
    int id;

    if (!t)
    {
        out(o, "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, o, t, &a, &b))
        return;
    if (a < 0 || b < 0 || a > 0xfff || b > 0xfff)
    {
        out(o, "error: address out of range\n");
        return;
    }
    id = o2debug_breakpoint_add(d, type, space, (uint16_t)a, (uint16_t)b, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse, or the address is outside %s space\n",
            space_name(space));
    else
    {
        out(o, "breakpoint #%d\n", id);
        if (space == O2_SPACE_PROG && (type & O2DEBUG_BP_WRITE))
            out(o, "(nothing writes program space; this one will not fire)\n");
    }
}

/* bpin / bpout <p1|p2|bus> [condition] */
static void add_io_bp(o2debug *d, outbuf *o, int type, char *args)
{
    char *t = next_tok(&args);
    int port = -1, id;

    if (t)
    {
        if (!strcasecmp(t, "p1") || !strcmp(t, "1")) port = 1;
        else if (!strcasecmp(t, "p2") || !strcmp(t, "2")) port = 2;
        else if (!strcasecmp(t, "bus") || !strcmp(t, "0")) port = 0;
    }
    if (port < 0)
    {
        out(o, "usage: %s <p1|p2|bus> [condition]\n", type == O2DEBUG_BP_IN ? "bpin" : "bpout");
        return;
    }
    id = o2debug_breakpoint_add(d, type, port, 0, 0, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse\n");
    else
        out(o, "breakpoint #%d\n", id);
}

/* a register for `set`; -1 if unknown */
static int reg_by_name(const char *n, int bs, int *iram_addr)
{
    static const struct { const char *name; int reg; } map[] = {
        {"a", O2_REG_A}, {"pc", O2_REG_PC}, {"psw", O2_REG_PSW}, {"sp", O2_REG_SP},
        {"t", O2_REG_T}, {"cnt", O2_REG_CNT}, {"p1", O2_REG_P1}, {"p2", O2_REG_P2},
        {"bus", O2_REG_BUS}, {"c", O2_REG_C}, {"ac", O2_REG_AC}, {"f0", O2_REG_F0},
        {"f1", O2_REG_F1}, {"bs", O2_REG_BS}, {"mb", O2_REG_MB},
    };
    (void)bs;
    *iram_addr = -1;
    if (n[0] == 'r' && n[1] >= '0' && n[1] <= '7')
    {
        if (n[2] == '\0')
            return O2_REG_R0 + (n[1] - '0');
        if (n[2] == '\'' && n[3] == '\0')
            return O2_REG_R0_B1 + (n[1] - '0');
    }
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(map[i].name, n))
            return map[i].reg;
    return -1;
}

static void show_io(o2debug *d, outbuf *o)
{
    o2debug_io io;
    o2debug_io_get(d, &io);
    out(o, "BIOS: %s%s   character set: %s\n", io.have_bios ? "imported" : "the stand-in",
        io.missing & O2_MISSING_BIOS ? " (parked: a game needs the BIOS)" : "",
        io.have_font ? "imported" : "not imported");
    out(o, "P1=%02X: bank P10/P11=%d  keyboard %s  VDC %s  RAM/cart %s  EF9340 %s  copy mode %s"
           "  LUM %s\n",
        io.p1, io.cart_bank, io.kbd_enable ? "on" : "off", io.vdc_select ? "selected" : "off",
        io.ram_select ? "selected" : "off", io.ef_select ? "selected" : "off",
        io.copy_mode ? "on" : "off", io.lum ? "on" : "off");
    out(o, "P2=%02X: keyboard row %d   T0 %d\n", io.p2, io.kbd_row, io.t0);
    out(o, "RESET %s   audio samples %llu   Voice %s\n", io.reset_held ? "held" : "up",
        (unsigned long long)io.audio_samples, io.voice_present ? "present" : "absent");
}

static void show_keys(o2debug *d, outbuf *o)
{
    o2debug_io io;
    o2debug_io_get(d, &io);
    for (int r = 0; r < 8; r++)
    {
        const unsigned row = (unsigned)(io.keys >> (r * 8)) & 0xff;
        if (r >= 6 && !row)
            continue;
        out(o, "row %d:", r);
        for (int c = 0; c < 8; c++)
            if (row & (1u << c))
                out(o, " %d", r * 8 + c);
        out(o, "%s\n", row ? "" : " (none)");
    }
    for (int s = 0; s < 2; s++)
    {
        const uint8_t j = io.sticks[s];
        out(o, "stick %d:%s%s%s%s%s%s\n", s + 1, j & 1 ? " UP" : "", j & 2 ? " RIGHT" : "",
            j & 4 ? " DOWN" : "", j & 8 ? " LEFT" : "", j & 16 ? " BUTTON" : "", j ? "" : " (centred)");
    }
    out(o, "keyboard decoder %s, row %d\n", io.kbd_enable ? "on" : "off", io.kbd_row);
}

int o2debug_command(o2debug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd;
    outbuf o = { dst, dstsz, 0 };
    long v, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (char *c = cmd; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        out(&o, "%s", help_text);
    else if (!strcmp(cmd, "step") || !strcmp(cmd, "s"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!o2debug_is_stopped(d))
            out(&o, "the machine is running (stop first)\n");
        else
            o2debug_step_n(d, (int)v);
    }
    else if (!strcmp(cmd, "over") || !strcmp(cmd, "o"))
        o2debug_step_over(d);
    else if (!strcmp(cmd, "out"))
        o2debug_step_out(d);
    else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g"))
        o2debug_resume(d);
    else if (!strcmp(cmd, "stop"))
        o2debug_stop(d);
    else if (!strcmp(cmd, "runto"))
    {
        char *t = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        o2debug_run_to(d, (uint16_t)v);
    }
    else if (!strcmp(cmd, "scanline") || !strcmp(cmd, "frame"))
    {
        char *t = next_tok(&p);
        v = 1;
        if (t && eval(d, &o, t, &v))
            return o.len;
        if (!strcmp(cmd, "frame"))
            o2debug_frame(d, (int)v);
        else
            o2debug_scanline(d, (int)v);
    }
    else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp"))
        add_bp(d, &o, O2DEBUG_BP_EXEC, O2_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpr"))
        add_bp(d, &o, O2DEBUG_BP_READ, O2_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpw"))
        add_bp(d, &o, O2DEBUG_BP_WRITE, O2_SPACE_PROG, p);
    else if (!strcmp(cmd, "bpxr"))
        add_bp(d, &o, O2DEBUG_BP_READ, O2_SPACE_XDATA, p);
    else if (!strcmp(cmd, "bpxw"))
        add_bp(d, &o, O2DEBUG_BP_WRITE, O2_SPACE_XDATA, p);
    else if (!strcmp(cmd, "bpir"))
        add_bp(d, &o, O2DEBUG_BP_READ, O2_SPACE_IRAM, p);
    else if (!strcmp(cmd, "bpiw"))
        add_bp(d, &o, O2DEBUG_BP_WRITE, O2_SPACE_IRAM, p);
    else if (!strcmp(cmd, "bpvr"))
        add_bp(d, &o, O2DEBUG_BP_READ, O2_SPACE_VDC, p);
    else if (!strcmp(cmd, "bpvw"))
        add_bp(d, &o, O2DEBUG_BP_WRITE, O2_SPACE_VDC, p);
    else if (!strcmp(cmd, "bpin"))
        add_io_bp(d, &o, O2DEBUG_BP_IN, p);
    else if (!strcmp(cmd, "bpout"))
        add_io_bp(d, &o, O2DEBUG_BP_OUT, p);
    else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable"))
    {
        char *t = next_tok(&p);
        if (!t)
            out(&o, "usage: %s <id>\n", cmd);
        else
        {
            o2debug_breakpoint bp[128];
            const int n = o2debug_breakpoint_list(d, bp, 128);
            const int is_id = t[0] == '#';
            long id = strtol(is_id ? t + 1 : t, NULL, 0);
            int found = 0;

            /* "#3" is an id; a plain number is one when a breakpoint has it,
             * else an address (delete only) */
            for (int i = 0; i < n; i++)
                if (bp[i].id == id)
                    found = 1;
            if (!found && !is_id && !strcmp(cmd, "delete") && eval(d, &o, t, &v) == 0)
            {
                for (int i = 0; i < n; i++)
                    if ((bp[i].type & O2DEBUG_BP_EXEC) && v >= bp[i].start && v <= bp[i].end)
                    {
                        o2debug_breakpoint_remove(d, bp[i].id);
                        found = 1;
                    }
                if (!found)
                    out(&o, "no breakpoint #%ld or at $%03lX\n", id, v & 0xfff);
            }
            else if (!found)
                out(&o, "no breakpoint #%ld\n", id);
            else if (!strcmp(cmd, "delete"))
                o2debug_breakpoint_remove(d, (int)id);
            else
                o2debug_breakpoint_enable(d, (int)id, !strcmp(cmd, "enable"));
        }
    }
    else if (!strcmp(cmd, "clear"))
        o2debug_breakpoint_clear(d);
    else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl"))
        list_breaks(d, &o);
    else if (!strcmp(cmd, "print") || !strcmp(cmd, "p"))
    {
        if (eval(d, &o, rest_of(p), &v) == 0)
        {
            char bin[17];
            const int bits = (v >= 0 && v < 256) ? 8 : 16;
            for (int i = 0; i < bits; i++)
                bin[i] = (char)('0' + ((v >> (bits - 1 - i)) & 1));
            bin[bits] = '\0';
            out(&o, v >= 0 && v < 256 ? "$%02lX  %ld  %%%s\n" : "$%04lX  %ld  %%%s\n",
                (unsigned long)v & 0xffff, v, bin);
        }
    }
    else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r"))
        show_regs(d, &o);
    else if (!strcmp(cmd, "set"))
    {
        char *r = next_tok(&p);
        int ia, reg = r ? reg_by_name(r, 0, &ia) : -1;
        if (reg < 0)
            out(&o, "usage: set <reg> <expr>   (a pc psw sp r0-r7 r0'-r7' t cnt p1 p2 bus c ac f0 f1 "
                    "bs mb)\n");
        else if (eval(d, &o, rest_of(p), &v) == 0)
        {
            if (!o2debug_is_stopped(d))
                out(&o, "the machine is running (stop first)\n");
            else
            {
                o2debug_cpu_set(d, reg, (int)v);
                show_regs(d, &o);
            }
        }
    }
    else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m"))
    {
        char *t = next_tok(&p), *l;
        uint8_t buf[256];
        int space = O2_SPACE_PROG;
        if (t && space_by_name(t) >= 0)
        {
            space = space_by_name(t);
            t = next_tok(&p);
        }
        l = next_tok(&p);
        w = 64;
        if (!t || eval(d, &o, t, &v) || (l && eval(d, &o, l, &w)))
        {
            if (!t)
                out(&o, "usage: mem [prog|xdata|iram|vdc] <addr> [len]\n");
            return o.len;
        }
        if (w > 256)
            w = 256;
        if (w < 1)
            w = 1;
        o2debug_read(d, space, (uint16_t)v, buf, (int)w);
        out(&o, "%s space\n", space_label[space]);
        dump(&o, space, (uint16_t)v, buf, (int)w);
        dump_names(d, &o, space, (uint16_t)v, (int)w);
    }
    else if (!strcmp(cmd, "poke"))
    {
        char *t = next_tok(&p), *b;
        int n = 0, space = O2_SPACE_PROG;
        if (t && space_by_name(t) >= 0)
        {
            space = space_by_name(t);
            t = next_tok(&p);
        }
        if (!t || eval(d, &o, t, &v))
        {
            if (!t)
                out(&o, "usage: poke [xdata|iram|vdc] <addr> <v...>\n");
            return o.len;
        }
        if (space == O2_SPACE_PROG)
        {
            out(&o, "program space is read-only: the BIOS and the cartridge's image cannot be "
                    "edited from here\n");
            return o.len;
        }
        while ((b = next_tok(&p)) != NULL)
        {
            if (eval(d, &o, b, &w))
                return o.len;
            if (space == O2_SPACE_VDC)
                o2debug_vdc_write(d, (unsigned)(v + n), (uint8_t)w);
            else
                o2debug_write(d, space, (uint16_t)(v + n), (uint8_t)w);
            n++;
        }
        out(&o, "%d byte%s at %s $%02lX\n", n, n == 1 ? "" : "s", space_label[space],
            (unsigned long)v & 0xff);
    }
    else if (!strcmp(cmd, "vdc"))
        o.len += o2debug_text_vdc(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "objects"))
        o.len += o2debug_text_objects(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "ef"))
        o.len += o2debug_text_ef(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "voice"))
        o.len += o2debug_text_voice(dst + o.len, dstsz - o.len);
    else if (!strcmp(cmd, "io"))
        show_io(d, &o);
    else if (!strcmp(cmd, "keys"))
        show_keys(d, &o);
    else if (!strcmp(cmd, "cart") || !strcmp(cmd, "mailbox"))
    {
        o2debug_cart c;
        char info[200];
        o2debug_cart_get(d, &c);
        o2debug_cart_info(d, info, sizeof info);
        out(&o, "%s\n", info);
        if (!strcmp(cmd, "cart"))
        {
            out(&o, "type %s%s, image %u bytes", c.type_name ? c.type_name : "?",
                c.voice_in_front ? " behind The Voice" : "", c.rom_size);
            if (c.type == O2_CART_FUJINET)
                out(&o, ", CRC %08X%s\n", c.live_crc,
                    c.mailbox ? ", claims the mailbox" : ", no mailbox");
            else
                out(&o, ", bank %d, control $%02X\n", c.bank, c.control);
            if (c.type == O2_CART_FUJINET)
            {
                out(&o, "%s: %d %s, %d in\n", c.app_live ? "APPBANK pages" : "banks", c.nbanks,
                    c.app_live ? "pages" : "legacy banks", c.fbank);
                out(&o, "staged: %s%s  swaps %d\n",
                    c.staged_size ? (c.staged_mailbox ? "an app" : "a game") : "nothing",
                    c.armed ? " (armed)" : "", c.swaps);
            }
        }
        else if (c.type == O2_CART_FUJINET)
        {
            out(&o, "ACKSEQ %02X  status %02X  err %d  reply %02X  rxlen %u  queue %u\n", c.ackseq,
                c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth);
            out(&o, "boot state %d %d%% err %d\n", c.boot_state, c.boot_pct, c.boot_err);
        }
        if (!strcmp(cmd, "mailbox"))
        {
            out(&o, "$F20-$FFF as the console reads it:\n");
            dump(&o, O2_SPACE_PROG, 0xf20, c.mailbox_page, 0x50);
        }
    }
    else if (!strcmp(cmd, "label"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        o2debug_set_label(d, (uint16_t)(v & 0xfff), n);
    }
    else if (!strcmp(cmd, "labels"))
    {
        char msg[256];
        char *f = rest_of(p);
        o2debug_load_symbols(d, *f ? f : NULL, msg, sizeof msg);
        out(&o, "%s\n", msg);
    }
    else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        o2debug_line lines[64];
        o2debug_cpu c;
        int pcl, cnt;
        o2debug_cpu_get(d, &c);
        v = c.pc;
        w = 16;
        if ((t && eval(d, &o, t, &v)) || (n && eval(d, &o, n, &w)))
            return o.len;
        if (w > 64)
            w = 64;
        if (w < 1)
            w = 1;
        cnt = o2debug_disassemble(d, (uint16_t)v, lines, (int)w, &pcl);
        for (int i = 0; i < cnt; i++)
        {
            if (lines[i].label[0])
                out(&o, "%s:\n", lines[i].label);
            out(&o, "%c%c%03X  %-6s %-20s%s%s\n", lines[i].is_pc ? '>' : ' ',
                lines[i].has_breakpoint ? '*' : ' ', lines[i].address, lines[i].bytes,
                lines[i].disasm, lines[i].comment[0] ? "; " : "", lines[i].comment);
        }
    }
    else if (!strcmp(cmd, "trace"))
    {
        char *t = next_tok(&p);
        if (t && !strcmp(t, "on"))
            o2debug_trace_enable(d, 1);
        else if (t && !strcmp(t, "off"))
            o2debug_trace_enable(d, 0);
        else if (t && !strcmp(t, "clear"))
            o2debug_trace_clear(d);
        else
        {
            o2debug_trace tr[64];
            int n;
            w = 16;
            if (t && eval(d, &o, t, &w))
                return o.len;
            if (w > 64)
                w = 64;
            if (w < 1)
                w = 1;
            n = o2debug_trace_read(d, tr, (int)w);
            if (!n)
                out(&o, "trace is %s and empty\n", o2debug_trace_enabled(d) ? "on" : "off");
            for (int i = n - 1; i >= 0; i--)
            {
                o2debug_line l;
                int pcl;
                o2debug_disassemble(d, tr[i].pc, &l, 1, &pcl);
                out(&o, "%10llu %3d:%3d  %03X  %-18s A=%02X PSW=%02X SP=%d R0=%02X R1=%02X\n",
                    (unsigned long long)tr[i].cycles, tr[i].vpos, tr[i].hpos, tr[i].pc, l.disasm,
                    tr[i].a, tr[i].psw, tr[i].sp, tr[i].r0, tr[i].r1);
            }
        }
    }
    else if (!strcmp(cmd, "save"))
    {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f)
            out(&o, "usage: save <dis|ram|mailbox|vdc|font|screen|regs> <file>\n");
        else
        {
            o2debug_save(d, k, f, msg, sizeof msg);
            out(&o, "%s\n", msg);
        }
    }
    else
        out(&o, "unknown command '%s' (try help)\n", cmd);

    return o.len;
}

int o2debug_completions(o2debug *d, const char *prefix, char *dst, int dstsz)
{
    outbuf o = { dst, dstsz, 0 };
    const char *word;
    size_t n;
    int count = 0;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    n = strlen(word);

    if (first_word)
    {
        for (int i = 0; commands[i]; i++)
            if (!strncmp(commands[i], word, n))
            {
                out(&o, "%s\n", commands[i]);
                count++;
            }
        return count;
    }
    if (!strncmp(prefix, "save ", 5) && !strchr(prefix + 5, ' '))
    {
        static const char *const kinds[] = { "dis", "ram", "mailbox", "vdc", "font", "screen", "regs", NULL };
        for (int i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n))
            {
                out(&o, "%s\n", kinds[i]);
                count++;
            }
        return count;
    }
    if ((!strncmp(prefix, "mem ", 4) && !strchr(prefix + 4, ' ')) ||
        (!strncmp(prefix, "poke ", 5) && !strchr(prefix + 5, ' ')))
    {
        static const char *const spaces[] = { "prog", "xdata", "iram", "vdc", NULL };
        for (int i = 0; spaces[i]; i++)
            if (!strncmp(spaces[i], word, n))
            {
                out(&o, "%s\n", spaces[i]);
                count++;
            }
        return count;
    }
    if ((!strncmp(prefix, "bpin ", 5) && !strchr(prefix + 5, ' ')) ||
        (!strncmp(prefix, "bpout ", 6) && !strchr(prefix + 6, ' ')))
    {
        static const char *const ports[] = { "p1", "p2", "bus", NULL };
        for (int i = 0; ports[i]; i++)
            if (!strncmp(ports[i], word, n))
            {
                out(&o, "%s\n", ports[i]);
                count++;
            }
        return count;
    }
    for (int i = 0; o2debug_reg_names[i]; i++)
        if (n && !strncmp(o2debug_reg_names[i], word, n))
        {
            out(&o, "%s\n", o2debug_reg_names[i]);
            count++;
        }
    {
        symtab *t = o2debug_symbols(d);
        const char *name;
        o2_host_lock();
        for (int i = 0; (name = symtab_name_at(t, i)) != NULL && count < 200; i++)
            if (n && !strncasecmp(name, word, n))
            {
                out(&o, "%s\n", name);
                count++;
            }
        o2_host_unlock();
    }
    return count;
}

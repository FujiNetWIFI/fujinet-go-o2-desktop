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
    "break", "bpr", "bpw", "bpin", "bpout", "delete", "enable", "disable", "clear",
    "breaks", "print", "regs", "set", "mem", "poke", "video", "io", "keys", "cart",
    "mailbox", "text", "label", "labels", "disasm", "trace", "save", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a SEP call, or an SCRT call (F8)\n"
    "out                    run to the routine's return (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "scanline [n] | frame [n]\n"
    "break <addr>[-<end>] [cond]     execute breakpoint\n"
    "bpr | bpw <addr>[-<end>] [cond] read / write breakpoint\n"
    "bpin | bpout <port>[-<end>] [cond]  INP / OUT breakpoint (ports 1-7)\n"
    "delete <id> | enable <id> | disable <id> | clear | breaks\n"
    "print <expr>           e.g. print r3, print [$0900], print {r6}\n"
    "regs                   CPU registers\n"
    "set <reg> <expr>       e.g. set pc $0402, set r2 $09ff, set df 1\n"
    "mem <addr> [len]       dump memory (side-effect free)\n"
    "poke <addr> <v...>     write memory\n"
    "video | io | keys      the CDP1861, the I/O, the keypads\n"
    "cart | mailbox | text  the FujiNet cartridge, its mailbox, its text engine\n"
    "label <addr> <name> | labels [file]\n"
    "disasm [addr] [n]\n"
    "trace [on|off|n]       record / show the last n instructions\n"
    "save <dis|ram|arena|raster|screen|regs> <file>\n"
    "Expressions: $0400 0x400 400h 1024 %1010; registers r0-r15 (r3.0 r3.1 the\n"
    "bytes) pc d b t p x df ie q ef1-ef4; [addr] {addr} (high byte first);\n"
    "labels; vpos hpos frame cycles keylatch; in a condition, value and addr;\n"
    "C operators.\n";

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

static void show_regs(o2debug *d, outbuf *o)
{
    o2debug_cpu c;
    o2debug_cpu_get(d, &c);
    for (int r = 0; r < 16; r++)
        out(o, "R%-2d=%04X%s", r, c.r[r], r % 8 == 7 ? "\n" : " ");
    out(o, "P=%X X=%X D=%02X DF=%d B=%02X T=%02X IE=%d Q=%d  EF1-4 %d%d%d%d  PC=%04X\n",
        c.p, c.x, c.d, c.df, c.b, c.t, c.ie, c.q, c.ef[0], c.ef[1], c.ef[2], c.ef[3], c.pc);
    out(o, "%s, %s  INT %d DMA %d  line %d dot %d  frame %u\n", c.state_name, c.mode_name,
        c.int_line, c.dma_line, c.vpos, c.hpos, c.frame);
}

static void dump(outbuf *o, uint16_t addr, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i += 16)
    {
        out(o, "%04X ", (uint16_t)(addr + i));
        for (int j = 0; j < 16; j++)
            out(o, j < n - i ? " %02X" : "   ", b[i + j]);
        out(o, "  ");
        for (int j = 0; j < 16 && i + j < n; j++)
            out(o, "%c", (b[i + j] >= 0x20 && b[i + j] < 0x7f) ? b[i + j] : '.');
        out(o, "\n");
    }
}

static int reg_by_name(const char *n)
{
    static const struct { const char *name; int reg; } map[] = {
        {"pc", O2_REG_PC}, {"p", O2_REG_P}, {"x", O2_REG_X}, {"d", O2_REG_D},
        {"b", O2_REG_B}, {"t", O2_REG_T}, {"df", O2_REG_DF}, {"ie", O2_REG_IE},
        {"q", O2_REG_Q},
    };
    if (n[0] == 'r' && n[1] >= '0' && n[1] <= '9')
    {
        char *end;
        long r = strtol(n + 1, &end, 10);
        if (*end == '\0' && r >= 0 && r <= 15)
            return O2_REG_R0 + (int)r;
    }
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(map[i].name, n))
            return map[i].reg;
    return -1;
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
        if (bp[i].start == bp[i].end)
            out(o, "#%d %-3s $%04X", bp[i].id, kinds, bp[i].start);
        else
            out(o, "#%d %-3s $%04X-$%04X", bp[i].id, kinds, bp[i].start, bp[i].end);
        out(o, "%s%s%s  hits %u\n", bp[i].enabled ? "" : " (disabled)",
            bp[i].condition[0] ? " if " : "", bp[i].condition, bp[i].hits);
    }
}

static void add_bp(o2debug *d, outbuf *o, int type, char *args, int io)
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
    if (io && (a < 1 || a > 7 || b < 1 || b > 7))
    {
        out(o, "error: ports are 1-7 (INP 1 is display on, OUT 2 the key latch)\n");
        return;
    }
    id = o2debug_breakpoint_add(d, type, (uint16_t)a, (uint16_t)b, rest_of(args));
    if (id < 0)
        out(o, "error: the condition does not parse\n");
    else
        out(o, "breakpoint #%d\n", id);
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
        else if (v <= 1)
            o2debug_step(d);
        else
        {
            /* step n: the hook counts them down */
            o2debug_step(d);
            for (long i = 1; i < v; i++)
            {
                int tries = 0;
                while (!o2debug_is_stopped(d) && tries++ < 2000)
                {
                    struct timespec ts = { 0, 100000L };
                    nanosleep(&ts, NULL);
                }
                o2debug_step(d);
            }
        }
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
        add_bp(d, &o, O2DEBUG_BP_EXEC, p, 0);
    else if (!strcmp(cmd, "bpr"))
        add_bp(d, &o, O2DEBUG_BP_READ, p, 0);
    else if (!strcmp(cmd, "bpw"))
        add_bp(d, &o, O2DEBUG_BP_WRITE, p, 0);
    else if (!strcmp(cmd, "bpin"))
        add_bp(d, &o, O2DEBUG_BP_IN, p, 1);
    else if (!strcmp(cmd, "bpout"))
        add_bp(d, &o, O2DEBUG_BP_OUT, p, 1);
    else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable"))
    {
        char *t = next_tok(&p);
        if (!t)
            out(&o, "usage: %s <id>\n", cmd);
        else
        {
            long id = strtol(t[0] == '#' ? t + 1 : t, NULL, 10);
            if (!strcmp(cmd, "delete"))
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
            for (int i = 0; i < 16; i++)
                bin[i] = (char)('0' + ((v >> (15 - i)) & 1));
            bin[16] = '\0';
            out(&o, "$%04lX  %ld  %%%s\n", (unsigned long)v & 0xffff, v, bin);
        }
    }
    else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r"))
        show_regs(d, &o);
    else if (!strcmp(cmd, "set"))
    {
        char *r = next_tok(&p);
        int reg = r ? reg_by_name(r) : -1;
        if (reg < 0)
            out(&o, "usage: set <reg> <expr>\n");
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
        char *t = next_tok(&p), *l = next_tok(&p);
        uint8_t buf[256];
        w = 64;
        if (!t || eval(d, &o, t, &v) || (l && eval(d, &o, l, &w)))
            return o.len;
        if (w > 256)
            w = 256;
        o2debug_read(d, (uint16_t)v, buf, (int)w);
        dump(&o, (uint16_t)v, buf, (int)w);
    }
    else if (!strcmp(cmd, "poke"))
    {
        char *t = next_tok(&p), *b;
        int n = 0;
        if (!t || eval(d, &o, t, &v))
            return o.len;
        while ((b = next_tok(&p)) != NULL)
        {
            if (eval(d, &o, b, &w))
                return o.len;
            o2debug_write(d, (uint16_t)(v + n), (uint8_t)w);
            n++;
        }
        out(&o, "%d byte%s at $%04lX\n", n, n == 1 ? "" : "s", (unsigned long)v & 0xffff);
    }
    else if (!strcmp(cmd, "video"))
    {
        o2debug_video vd;
        o2debug_video_get(d, &vd);
        out(&o, "CDP1861: display %s  INT %d  EFx %d  DMA OUT %d\n", vd.display_on ? "on" : "off",
            vd.int_line, vd.efx_line, vd.dma_line);
        out(&o, "beam line %d dot %d%s  frame %u\n", vd.vpos, vd.hpos,
            vd.in_display ? " (display lines)" : "", vd.frame);
        out(&o, "DMA pointer R0=$%04X  last $%04X  %llu bytes  short bursts %u\n", vd.dma_pointer,
            vd.last_dma_addr, (unsigned long long)vd.dma_bytes, vd.short_lines);
        out(&o, "next INT event line %d, next EFx event line %d  display-on strobes %u\n",
            vd.next_int_line, vd.next_efx_line, vd.inp1_count);
    }
    else if (!strcmp(cmd, "io") || !strcmp(cmd, "keys"))
    {
        o2debug_io io;
        o2debug_io_get(d, &io);
        if (!strcmp(cmd, "keys"))
        {
            for (int k = 0; k < 2; k++)
            {
                out(&o, "keypad %c:", k ? 'B' : 'A');
                for (int i = 0; i < 10; i++)
                    if (io.keys[k] & (1u << i))
                        out(&o, " %d", i);
                out(&o, "%s\n", io.keys[k] ? "" : " (none)");
            }
            out(&o, "latched key %d: EF3 %d EF4 %d  CLEAR %s\n", io.keylatch, io.ef3, io.ef4,
                io.clear_held ? "held" : "up");
        }
        else
        {
            out(&o, "Q %d (beeper %s, %d Hz)  key latch %d  CLEAR %s\n", io.q, io.q ? "on" : "off",
                io.beep_hz, io.keylatch, io.clear_held ? "held" : "up");
            out(&o, "last OUT:");
            for (int i = 1; i < 8; i++)
                out(&o, " %d=$%02X", i, io.last_out[i]);
            out(&o, "\nINP 1 (display on) %u times  BIOS: %s%s\n", io.inp1_count,
                io.have_bios ? "imported" : "the stand-in",
                io.stub_parked ? " (parked: a game needs the BIOS)" : "");
        }
    }
    else if (!strcmp(cmd, "cart") || !strcmp(cmd, "mailbox") || !strcmp(cmd, "text"))
    {
        o2debug_cart c;
        char info[200];
        o2debug_cart_get(d, &c);
        o2debug_cart_info(d, info, sizeof info);
        out(&o, "%s\n", info);
        if (!strcmp(cmd, "cart"))
        {
            static const char kind[] = ".RHD";   /* none, ROM, hotspot, raster (DMA) */
            out(&o, "image: %s, %u bytes, %d blocks, CRC %08X%s\n", c.live_st2 ? "ST2" : "raw",
                c.live_size, c.live_blocks, c.live_crc, c.live_mailbox ? ", claims the mailbox" : "");
            if (c.nbanks)
                out(&o, "bank window $C400: %d banks, bank %d in\n", c.nbanks, c.bank);
            out(&o, "staged: %s%s  swaps %d  raster frames %u  short bursts %u\n",
                c.staged ? (c.staged_config ? "CONFIG" : c.staged_claim ? "an app" : "a game") : "nothing",
                c.armed ? " (armed)" : "", c.swaps, c.frames, c.short_lines);
            out(&o, "pages (. none, R image, H hotspot, D raster):\n");
            for (int hi = 0; hi < 16; hi++)
            {
                out(&o, "  %X000 ", hi);
                for (int lo = 0; lo < 16; lo++)
                {
                    uint8_t t = c.page_type[hi * 16 + lo];
                    out(&o, "%c", t < 4 ? kind[t] : '?');
                }
                out(&o, "\n");
            }
        }
        else if (!strcmp(cmd, "mailbox"))
        {
            out(&o, "ACKSEQ %d  status %02X  err %d  reply %02X  rxlen %u  queue %u\n", c.ackseq,
                c.status, c.last_error, c.reply_cmd, c.rxlen, c.queue_depth);
            out(&o, "boot state %d %d%% err %d (%u/%u)\n", c.boot_state, c.boot_pct, c.boot_err,
                c.boot_got, c.boot_total);
        }
        else
        {
            for (int r = 0; r < 10; r++)
            {
                out(&o, "%c|", r == c.cursor_row ? '>' : ' ');
                for (int col = 0; col < 16; col++)
                {
                    uint8_t ch = c.text[r][col];
                    out(&o, "%c", ch >= 0x20 && ch < 0x7f ? ch : ch ? '#' : ' ');
                }
                out(&o, "|\n");
            }
            out(&o, "cursor row %d col %d%s\n", c.cursor_row, c.cursor_col,
                c.inverse ? ", inverse" : "");
        }
    }
    else if (!strcmp(cmd, "label"))
    {
        char *t = next_tok(&p), *n = next_tok(&p);
        if (!t || eval(d, &o, t, &v))
            return o.len;
        o2debug_set_label(d, (uint16_t)v, n);
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
        cnt = o2debug_disassemble(d, (uint16_t)v, lines, (int)w, &pcl);
        for (int i = 0; i < cnt; i++)
        {
            if (lines[i].label[0])
                out(&o, "%s:\n", lines[i].label);
            out(&o, "%c%c%04X  %-10s %-20s%s%s\n", lines[i].is_pc ? '>' : ' ',
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
            n = o2debug_trace_read(d, tr, (int)w);
            if (!n)
                out(&o, "trace is %s and empty\n", o2debug_trace_enabled(d) ? "on" : "off");
            for (int i = n - 1; i >= 0; i--)
            {
                o2debug_line l;
                int pcl;
                o2debug_disassemble(d, tr[i].pc, &l, 1, &pcl);
                out(&o, "%10llu %3d:%3d  %04X  %-18s P=%X X=%X D=%02X DF=%d R(X)=%04X\n",
                    (unsigned long long)tr[i].cycles, tr[i].vpos, tr[i].hpos, tr[i].pc,
                    l.disasm, tr[i].p, tr[i].x, tr[i].d, tr[i].df, tr[i].rx);
            }
        }
    }
    else if (!strcmp(cmd, "save"))
    {
        char *k = next_tok(&p);
        char *f = rest_of(p);
        char msg[300];
        if (!k || !*f)
            out(&o, "usage: save <dis|ram|arena|raster|screen|regs> <file>\n");
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
        static const char *const kinds[] = { "dis", "ram", "arena", "raster", "screen", "regs", NULL };
        for (int i = 0; kinds[i]; i++)
            if (!strncmp(kinds[i], word, n))
            {
                out(&o, "%s\n", kinds[i]);
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
        for (int i = 0; (name = symtab_name_at(t, i)) != NULL && count < 200; i++)
            if (n && !strncasecmp(name, word, n))
            {
                out(&o, "%s\n", name);
                count++;
            }
    }
    return count;
}

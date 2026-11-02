/*
 * debug_test -- the debugger contract against a running machine: attach
 * stops it, registers, step / run-to / scanline / frame, disassembly with
 * the PC line and port names, breakpoints of every class (execute, memory
 * read and write, INP and OUT) with conditions, memory, the video views,
 * sound and I/O, the cartridge tab, built-in and loaded symbols in every
 * format, the trace ring, Save, the prompt and its completion, and detach
 * lets it run again. The breakpoints survive a power cycle. The program is
 * the tests' counter app (test_rom.h), on the stand-in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "o2session.h"
#include "o2debug.h"
#include "test_tmpdir.h"
#include "test_rom.h"

/* the loop, as counter.asm assembles it */
#define A_LDN   TEST_COUNTER_LOOP               /* LDN R8 */
#define A_ADI   (TEST_COUNTER_LOOP + 1)         /* ADI #01 */
#define A_STR   (TEST_COUNTER_LOOP + 3)         /* STR R8 */
#define A_SEX3  (TEST_COUNTER_LOOP + 4)         /* SEX R3 */
#define A_OUT   (TEST_COUNTER_LOOP + 5)         /* OUT 2; DB 01 */
#define A_SEX2  (TEST_COUNTER_LOOP + 7)         /* SEX R2 */
#define A_BR    (TEST_COUNTER_UP + 1)           /* BR loop */

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(o2debug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!o2debug_is_stopped(d) == want) return 1;
        sleep_ms(5); waited += 5;
    }
    return 0;
}

/* Resume and wait for the next stop. */
static int run_until_stop(o2debug *d, int timeout_ms)
{
    unsigned g = o2debug_generation(d);
    int waited = 0;
    o2debug_resume(d);
    while (waited < timeout_ms) {
        if (o2debug_is_stopped(d) && o2debug_generation(d) != g) return 1;
        sleep_ms(2); waited += 2;
    }
    return 0;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int cpu_pc(o2debug *d)
{
    o2debug_cpu c;
    o2debug_cpu_get(d, &c);
    return c.pc;
}

static int in_program(int pc)
{
    return pc >= TEST_COUNTER_LOOP && pc <= TEST_COUNTER_ISRW + 2;
}

int main(void)
{
    char cfg[512], data[512], rom[700], out[8192], path[800], msg[256];
    o2session_paths p;
    o2session *s;
    o2session_start_opts o;
    o2debug *d;
    o2debug_cpu c, c2;
    o2debug_line lines[48];
    int pc_line = -1, n, i, w, h;
    static uint32_t img[O2DEBUG_VIEW_MAX_PIXELS];

    test_tmpdir(cfg, sizeof cfg, "dcfg");
    test_tmpdir(data, sizeof data, "ddata");
    memset(&p, 0, sizeof p);
    p.config_dir = cfg; p.data_dir = data; p.fujinet_lib = "";
    snprintf(rom, sizeof rom, "%s/counter.bin", cfg);
    test_rom_write(rom, test_counter_app, sizeof test_counter_app);

    s = o2session_new(&p);
    if (!s) return 1;
    o2session_default_opts(s, &o);
    o.enable_fujinet = 0; o.enable_audio = 0; o.enable_gamepad = 0;
    o.cart_path = rom;
    check(o2session_start(s, &o) == 0, "session starts with the counter app");
    sleep_ms(300);

    d = o2session_debugger(s);
    check(d != NULL, "debugger handle");
    o2debug_attach(d);
    check(o2debug_is_attached(d), "attached");
    check(wait_stopped(d, 1, 3000), "attaching stops the machine");

    o2debug_cpu_get(d, &c);
    check(in_program(c.pc), "the PC is in the counter's loop or its interrupt");
    check(c.p == 3 || c.p == 1, "P is the program's (3) or the interrupt's (1)");
    check(c.r[2] >= 0x09f0 && c.r[2] <= 0x09ff, "the stack is the program's own, at RAM's top");
    check(c.r[8] == TEST_RAM_COUNTER, "R8 points at the counter");
    check(c.vpos >= 0 && c.vpos < 262 && c.hpos >= 0 && c.hpos < 112, "the beam position is reported");
    check(strcmp(c.mode_name, "run") == 0, "the CPU is in RUN");

    o2debug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    {
        int waited = 0;
        o2debug_cpu_get(d, &c2);
        while (c2.pc == c.pc && waited < 2000) {
            sleep_ms(5); waited += 5;
            o2debug_cpu_get(d, &c2);
        }
    }
    check(c2.pc != c.pc, "a step moves the PC");
    check(c2.cycles > c.cycles, "and the cycle count");

    /* run to an address */
    o2debug_run_to(d, A_LDN);
    check(wait_stopped(d, 1, 3000) && cpu_pc(d) == A_LDN, "run to the loop's top");

    /* disassembly around the PC */
    n = o2debug_disassemble(d, (uint16_t)o2debug_row_address(d, A_LDN, -4), lines, 48, &pc_line);
    check(n > 8, "disassembly lines");
    check(pc_line >= 0 && lines[pc_line].address == A_LDN && lines[pc_line].is_pc, "the PC line is marked");
    n = o2debug_disassemble(d, A_LDN, lines, 5, NULL);
    check(n == 5 && strcmp(lines[0].disasm, "LDN R8") == 0 && strcmp(lines[1].disasm, "ADI #01") == 0,
          "the loop starts LDN R8 / ADI #01");
    check(strcmp(lines[2].disasm, "STR R8") == 0 && strcmp(lines[4].disasm, "OUT 2") == 0,
          "then STR R8 ... OUT 2");
    check(strcmp(lines[4].comment, "KEYLATCH") == 0, "and OUT 2 is named");
    n = o2debug_disassemble(d, 0x0402, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "s2_entry") == 0, "the clients' native entry is labelled");

    /* an execute breakpoint hits */
    check(o2debug_breakpoint_toggle(d, A_BR) == 1, "breakpoint set on the BR");
    check(o2debug_breakpoint_check(d, A_BR), "and reported");
    check(run_until_stop(d, 3000), "the breakpoint hit");
    check(cpu_pc(d) == A_BR, "stopped at the breakpoint's address");
    {
        char why[128]; int addr;
        o2debug_stop_reason(d, why, sizeof why, &addr);
        check(strstr(why, "reakpoint") != NULL && addr == A_BR, "the stop reason says breakpoint");
    }
    n = o2debug_disassemble(d, TEST_COUNTER_UP, lines, 2, NULL);
    check(n == 2 && lines[1].address == A_BR && lines[1].has_breakpoint, "the line shows the breakpoint");
    check(o2debug_breakpoint_toggle(d, A_BR) == 0, "toggled off");

    /* conditions */
    check(o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, A_STR, A_STR, "this bogus ((") == -1,
          "an unparsable condition is refused");
    {
        int id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, A_STR, A_STR, "r8 == $1234");
        check(id > 0, "a conditional breakpoint is added");
        o2debug_resume(d);
        sleep_ms(200);
        check(!o2debug_is_stopped(d), "a false condition never stops");
        o2debug_stop(d);
        check(wait_stopped(d, 1, 3000), "stop");
        o2debug_breakpoint_remove(d, id);
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, A_STR, A_STR,
                                         "r8 == $0810 && [$0810] == $40 && r8.1 == 8");
        check(id > 0 && run_until_stop(d, 5000), "a true condition stops");
        {
            uint8_t v = 0;
            o2debug_read(d, TEST_RAM_COUNTER, &v, 1);
            check(cpu_pc(d) == A_STR && v == 0x40, "with the memory the condition named");
        }
        o2debug_breakpoint_clear(d);
    }

    /* memory and I/O breakpoints */
    {
        o2debug_breakpoint list[8];
        int id = o2debug_breakpoint_add(d, O2DEBUG_BP_WRITE, TEST_RAM_COUNTER, TEST_RAM_COUNTER, "");
        check(id > 0, "write breakpoint added");
        check(o2debug_breakpoint_list(d, list, 8) == 1 && list[0].type == O2DEBUG_BP_WRITE, "and listed");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_SEX3, "the write breakpoint hits after STR R8");
        o2debug_breakpoint_enable(d, id, 0);
        check(o2debug_breakpoint_list(d, list, 8) == 1 && !list[0].enabled && list[0].hits >= 1,
              "disabled, with its hit count");
        o2debug_breakpoint_clear(d);

        o2debug_breakpoint_add(d, O2DEBUG_BP_READ, TEST_RAM_COUNTER, TEST_RAM_COUNTER, "");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_ADI, "the read breakpoint hits after LDN R8");
        o2debug_breakpoint_clear(d);

        o2debug_breakpoint_add(d, O2DEBUG_BP_OUT, 2, 2, "value == 1");
        check(run_until_stop(d, 3000) && cpu_pc(d) == A_SEX2, "the OUT 2 breakpoint hits past its inline byte");
        o2debug_breakpoint_clear(d);

        o2debug_breakpoint_add(d, O2DEBUG_BP_IN, 1, 7, "");
        o2debug_resume(d);
        sleep_ms(150);
        check(!o2debug_is_stopped(d), "the program never INPs: the INP breakpoint stays quiet");
        o2debug_stop(d);
        wait_stopped(d, 1, 3000);
        o2debug_breakpoint_clear(d);
        check(o2debug_breakpoint_list(d, list, 8) == 0, "cleared");
        check(o2debug_breakpoint_add(d, O2DEBUG_BP_IN, 0, 0, "") > 0, "the API takes any range");
        o2debug_breakpoint_clear(d);
    }

    /* memory: read, and debugger writes */
    {
        uint8_t ram[512], v = 0, claim[4];
        o2debug_write(d, 0x0900, 0x5A);
        o2debug_ram_get(d, ram);
        check(ram[0x100] == 0x5A, "a RAM write lands");
        o2debug_read(d, 0x07FC, claim, 4);
        check(memcmp(claim, "FUJI", 4) == 0, "the claim reads through the cartridge");
        o2debug_read(d, 0x0420, &v, 1);
        o2debug_write(d, 0x0420, 0xEA);
        {
            uint8_t back = 0;
            o2debug_read(d, 0x0420, &back, 1);
            check(back == 0xEA, "a debugger write reaches the cartridge's image");
        }
        o2debug_write(d, 0x0420, v);
    }

    /* registers */
    o2debug_cpu_set(d, O2_REG_D, 0x42);
    o2debug_cpu_set(d, O2_REG_DF, 1);
    o2debug_cpu_set(d, O2_REG_R0 + 5, 0x1234);
    o2debug_cpu_get(d, &c);
    check(c.d == 0x42 && c.df == 1, "D and DF are editable");
    check(c.r[5] == 0x1234, "the scratchpad registers too");

    /* video */
    {
        o2debug_video v;
        o2debug_video_get(d, &v);
        check(v.display_on, "the 1861 is on");
        check(v.dma_bytes > 0, "and has fetched by DMA");
        check(v.next_int_line == 78 || v.next_int_line == 80, "its next INT event falls on line 78 or 80");
        check(o2debug_video_view(d, O2DEBUG_VIEW_SCREEN, img, &w, &h) && w == 112 && h == 262,
              "the whole-raster view");
        check(o2debug_video_view(d, O2DEBUG_VIEW_RASTER, img, &w, &h) && w == 64 && h == 128,
              "the cartridge's raster view");
        check(o2debug_video_view(d, O2DEBUG_VIEW_RAM, img, &w, &h) && w == 64 && h == 32,
              "the BIOS display page view");
    }

    /* sound and I/O */
    {
        o2debug_io io;
        o2debug_io_get(d, &io);
        check(!io.have_bios && !io.stub_parked, "the stand-in, not parked");
        check(io.keylatch == 1, "the program latched key 1");
        check(io.last_out[2] == 1, "OUT 2's last byte");
        check(io.beep_hz == 300 && !io.q, "the 300 Hz beeper, off");
        check(!io.clear_held && io.keys[0] == 0 && io.keys[1] == 0, "nothing held");
    }

    /* the cartridge */
    {
        o2debug_cart cart;
        o2debug_cart_get(d, &cart);
        check(cart.present && cart.direct && cart.booted_game, "cartridge tab: an opened image");
        check(cart.mailbox_mode && cart.live_mailbox, "an app: the mailbox is live");
        check(!cart.live_st2 && cart.live_size == 1024, "a raw 1K image");
        check(cart.page_type[0x04] == 1 && cart.page_type[0xF4] == 3 && cart.page_type[0x09] == 0,
              "the page map: the image, the raster, and console RAM left alone");
        check(o2debug_cart_info(d, out, sizeof out) > 0 && strstr(out, "FujiNet"), "cart info line");
        check(o2debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Link"), "the FujiNet state text");
    }

    /* symbols: every format, and the built-in tables */
    check(o2debug_label_address(d, "FN_ACKSEQ") == 0xE800, "built-in mailbox labels (from fuji_mailbox.h)");
    check(o2debug_label_address(d, "FN_RASTER") == 0xF400, "and the raster");
    check(o2debug_label_address(d, "S2T_PUTC") == 0xED00, "and the text engine (from s2_text.h)");
    snprintf(path, sizeof path, "%s/t.map", cfg);
    write_text(path, "Symbols in Segment NOTHING\nR8                 Int    8   -1  1 0\n\n"
                     "Symbols in Segment CODE\nloop               Int    42D -1  1 0\n");
    check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1, "an AS -g MAP .map loads (code only)");
    check(o2debug_label_address(d, "loop") == A_LDN, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.lst", cfg);
    write_text(path, "  Symbol Table (* = unused):\n  --------------------------\n\n"
                     " RA :                            A - | *isrw :                     451 C |\n"
                     " up :                          43B C |\n");
    check(o2debug_load_symbols(d, path, msg, sizeof msg) == 2, "an AS listing's symbol table loads");
    check(o2debug_label_address(d, "isrw") == TEST_COUNTER_ISRW, "unused labels too");
    snprintf(path, sizeof path, "%s/t.sym", cfg);
    write_text(path, "; WLA-DX\n[labels]\n00:0430 bump\n");
    check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1, "a WLA-DX .sym loads");
    check(o2debug_label_address(d, "bump") == A_STR, "and its labels resolve");
    snprintf(path, sizeof path, "%s/t.txt", cfg);
    write_text(path, "0811 key1\n");
    check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1, "a plain list loads");
    o2debug_address_label(d, TEST_RAM_KEY1, out, sizeof out);
    check(strcmp(out, "key1") == 0, "and the address finds its label");
    n = o2debug_disassemble(d, A_STR, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "bump") == 0, "loaded labels show in the disassembly");
    n = o2debug_disassemble(d, A_BR, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].comment, "loop") == 0, "a branch names its target");

    /* the trace ring */
    {
        o2debug_trace tr[16];
        o2debug_trace_enable(d, 1);
        for (i = 0; i < 10; i++) {
            o2debug_step(d);
            wait_stopped(d, 1, 2000);
        }
        n = o2debug_trace_read(d, tr, 16);
        check(n >= 10, "the trace holds the stepped instructions");
        check(n > 0 && in_program(tr[0].pc), "newest first, in the program");
        o2debug_trace_enable(d, 0);
        o2debug_trace_clear(d);
    }

    /* the beam: a scanline, a frame */
    {
        o2debug_cpu a, b;
        o2debug_cpu_get(d, &a);
        o2debug_scanline(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        o2debug_cpu_get(d, &b);
        check(b.vpos == (a.vpos + 1) % 262, "scanline+1 advances the beam one line");
        o2debug_frame(d, 1);
        wait_stopped(d, 1, 3000);
        sleep_ms(20);
        o2debug_cpu_get(d, &a);
        check(a.frame == b.frame + 1, "frame+1 advances one frame");
    }

    /* Save */
    {
        static const struct { const char *kind; long size; } kinds[] = {
            { "ram", 512 }, { "arena", 2048 }, { "raster", 1024 },
        };
        for (i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
            char what[96];
            snprintf(path, sizeof path, "%s/save.%s", cfg, kinds[i].kind);
            snprintf(what, sizeof what, "save %s writes %ld bytes", kinds[i].kind, kinds[i].size);
            check(o2debug_save(d, kinds[i].kind, path, msg, sizeof msg) == 0 &&
                  file_size(path) == kinds[i].size, what);
        }
        snprintf(path, sizeof path, "%s/save.ppm", cfg);
        check(o2debug_save(d, "screen", path, msg, sizeof msg) == 0 &&
              file_size(path) > 100 * 246 * 3, "save screen (a PPM of the picture)");
        snprintf(path, sizeof path, "%s/save.regs", cfg);
        check(o2debug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 100, "save regs");
        snprintf(path, sizeof path, "%s/save.dis", cfg);
        check(o2debug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 1000, "save dis");
        check(o2debug_save(d, "nonsense", path, msg, sizeof msg) == -1, "an unknown kind is refused");
    }

    /* the prompt */
    o2debug_cpu_set(d, O2_REG_D, 0x66);
    o2debug_command(d, "print d", out, sizeof out);
    check(strstr(out, "66") != NULL, "print evaluates an expression");
    o2debug_command(d, "print {$07FC}", out, sizeof out);
    check(strstr(out, "4655") != NULL, "{addr} reads high byte first, as the 1802 keeps them");
    snprintf(path, sizeof path, "mem $%04X 4", A_LDN);
    o2debug_command(d, path, out, sizeof out);
    check(strstr(out, "08 FC 01 58") != NULL, "mem dumps CPU memory");
    o2debug_command(d, "label $0431 latch", out, sizeof out);
    check(o2debug_label_address(d, "latch") == A_SEX3, "label sets one");
    snprintf(path, sizeof path, "disasm $%04X 2", A_LDN);
    o2debug_command(d, path, out, sizeof out);
    check(strstr(out, "LDN R8") != NULL, "disasm");
    o2debug_command(d, "regs", out, sizeof out);
    check(strstr(out, "R8 =0810") != NULL && strstr(out, "P=") != NULL, "regs");
    o2debug_command(d, "video", out, sizeof out);
    check(strstr(out, "CDP1861") != NULL, "video");
    o2debug_command(d, "io", out, sizeof out);
    check(strstr(out, "key latch 1") != NULL, "io shows the key latch");
    o2debug_command(d, "keys", out, sizeof out);
    check(strstr(out, "keypad A") != NULL, "keys");
    o2debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet") != NULL && strstr(out, "pages") != NULL, "cart, with its page map");
    o2debug_command(d, "text", out, sizeof out);
    check(strstr(out, "cursor") != NULL, "text");
    o2debug_command(d, "set r9 $0811", out, sizeof out);
    check(strstr(out, "R9 =0811") != NULL, "set a register by name");
    snprintf(path, sizeof path, "break $%04X", A_STR);
    o2debug_command(d, path, out, sizeof out);
    check(o2debug_breakpoint_check(d, A_STR), "break sets a breakpoint");
    o2debug_command(d, "bpout 9", out, sizeof out);
    check(strstr(out, "1-7") != NULL, "an INP/OUT breakpoint outside ports 1-7 is refused");
    o2debug_command(d, "clear", out, sizeof out);
    check(!o2debug_breakpoint_check(d, A_STR), "clear removes them");
    o2debug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") != NULL, "help lists the commands");
    o2debug_completions(d, "ru", out, sizeof out);
    check(strstr(out, "run") != NULL && strstr(out, "runto") != NULL, "Tab completion");
    o2debug_completions(d, "print r1", out, sizeof out);
    check(strstr(out, "r10") != NULL, "registers complete");
    o2debug_command(d, "frame", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "frame stops again");

    /* step over / out do not wedge the machine */
    o2debug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over");

    /* detach: running again */
    o2debug_detach(d);
    check(!o2debug_is_attached(d), "detached");
    {
        uint8_t a = 0, b = 0;
        sleep_ms(100);
        o2debug_attach(d);
        wait_stopped(d, 1, 3000);
        o2debug_read(d, TEST_RAM_COUNTER, &a, 1);
        o2debug_resume(d);
        sleep_ms(200);
        o2debug_read(d, TEST_RAM_COUNTER, &b, 1);
        check(a != b, "the machine runs after resume");
        o2debug_detach(d);
    }

    /* the debugger survives a power cycle */
    o2debug_attach(d);
    o2debug_breakpoint_toggle(d, A_STR);
    check(o2session_reset_to_config(s) == 0, "reset to CONFIG with the debugger attached");
    check(o2session_load_cart(s, rom) == 0, "reopen the cartridge");
    /* the power cycle re-attaches: it stops on the new machine's first
     * instruction, and from there the breakpoint must still be set */
    check(wait_stopped(d, 1, 3000), "the new machine stops on attach");
    check(cpu_pc(d) == 0x0000, "at the stand-in's first instruction");
    check(run_until_stop(d, 3000) && cpu_pc(d) == A_STR, "the breakpoint survives the power cycle");
    o2debug_breakpoint_clear(d);
    o2debug_detach(d);

    /* attached before the session starts (a window opened first): the
     * machine stops as soon as it exists */
    o2session_stop(s);
    o2debug_attach(d);
    o.cart_path = rom;
    check(o2session_start(s, &o) == 0, "restart with the debugger already attached");
    check(wait_stopped(d, 1, 3000), "the new machine stops in the debugger at once");
    o2debug_detach(d);
    check(!o2debug_is_stopped(d), "and runs when it detaches");

    o2session_stop(s);
    o2session_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

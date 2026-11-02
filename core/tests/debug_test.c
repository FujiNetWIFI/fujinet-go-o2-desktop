/*
 * debug_test -- the debugger contract (core/include/o2debug.h) against a
 * running machine: attach stops it (before the session starts, too), the CPU
 * view and its edits, step / step over / step out / run to / scanline /
 * frame, disassembly with labels and the PC line, breakpoints of every class
 * (execute, program-data, external-data, VDC, internal-RAM, IN, OUT) with
 * conditions, memory in all four spaces, the VDC views / objects / hit test /
 * EF9340 absence / I/O / cartridge, symbols in every format, the trace ring,
 * Save, the prompt and its completion, the FujiNet trace (wired through the
 * cartridge's real transport to a stand-in FujiNet on loopback), detach, and
 * a power cycle with the debugger attached.
 *
 * The machine is the host's (core/o2/host.h) on the no-BIOS stand-in running
 * the baked CONFIG client, so it needs no ROM and no session; it does not
 * depend on pixels, only on structure.
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

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "dasm48.h"
#include "debugger_internal.h"
#include "fuji_mailbox.h"
#include "fujitrace.h"
#include "host.h"
#include "test_tmpdir.h"

static int failures;
static int last_ok = 1;
static void check_(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
    last_ok = ok;
}
#define check(ok, what) check_((ok) != 0, what)
/* what the last failed check saw */
#define show(fmt, ...) do { if (!last_ok) printf("      " fmt "\n", __VA_ARGS__); } while (0)

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
        sleep_ms(2); waited += 2;
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
        sleep_ms(1); waited += 1;
    }
    return 0;
}

static int stop_now(o2debug *d)
{
    o2debug_stop(d);
    return wait_stopped(d, 1, 3000);
}

static o2debug_cpu cpu(o2debug *d)
{
    o2debug_cpu c;
    o2debug_cpu_get(d, &c);
    return c;
}

static char reason[128];
static int why(o2debug *d)
{
    int addr = -1;
    o2debug_stop_reason(d, reason, sizeof reason, &addr);
    return addr;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int find_line(const o2debug_line *l, int n, const char *prefix)
{
    for (int i = 0; i < n; i++)
        if (strncmp(l[i].disasm, prefix, strlen(prefix)) == 0) return i;
    return -1;
}

/* ---- a stand-in FujiNet on loopback (POSIX) ----------------------------------- */

#ifndef _WIN32
typedef struct { int listen_fd, fd, frames; } fake_fujinet;

/* A SLIP-framed FujiBus ACK from the FUJI device with one payload byte. */
static size_t ack_frame(uint8_t *out)
{
    uint8_t pkt[7] = { 0x70, 0x06, 7, 0, 0, 0, 0 };
    uint16_t ck = 0;
    size_t o = 0;
    for (int i = 0; i < 7; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (int i = 0; i < 7; i++) out[o++] = pkt[i];
    out[o++] = 0xC0;
    return o;
}

/* Answers every request frame with an ACK until the console hangs up. */
static void *fake_main(void *arg)
{
    fake_fujinet *f = arg;
    uint8_t c, reply[16];
    int ends = 0;

    f->fd = accept(f->listen_fd, NULL, NULL);
    if (f->fd < 0) return NULL;
    while (recv(f->fd, &c, 1, 0) == 1) {
        if (c != 0xC0) continue;
        if (++ends == 2) {          /* a frame: leading and trailing delimiter */
            send(f->fd, reply, ack_frame(reply), 0);
            f->frames++;
            ends = 0;
        }
    }
    close(f->fd);
    return NULL;
}
#endif

/* ---- a game cartridge, to run known code ---------------------------------------- */

/* 2K at $400: a little program with one of everything the breakpoints watch.
 *   400 anl p1,#$AF        RAM select on, external writes on (P14, P16 low)
 *   402 mov r0,#$10 / 404 mov a,#$77
 *   406 movx @r0,a         external write $10
 *   407 movx a,@r0         external read $10
 *   408 mov @r0,a          internal write $10
 *   409 mov a,@r0          internal read $10
 *   40A call $420          $420: inc r0, call $428 ($428: inc r1, ret), ret
 *   40C mov r1,#3
 *   40E djnz r1,$40E       three trips
 *   410 in a,p1 / 411 outl p2,a / 412 ins a,bus
 *   413 mov a,#$40 / 415 movp a,@a      reads $440 ($5A)
 *   416 jmp $416           idles
 */
static void build_game(uint8_t *img)
{
    static const uint8_t code[] = {
        0x99, 0xAF, 0xB8, 0x10, 0x23, 0x77, 0x90, 0x80, 0xA0, 0xF0, 0x94, 0x20,
        0xB9, 0x03, 0xE9, 0x0E, 0x09, 0x3A, 0x08, 0x23, 0x40, 0xA3, 0x84, 0x16,
    };
    memset(img, 0, 2048);
    memcpy(img, code, sizeof code);
    img[0x20] = 0x18;                       /* 420 inc r0 */
    img[0x21] = 0x94; img[0x22] = 0x28;     /* 421 call $428 */
    img[0x23] = 0x83;                       /* 423 ret */
    img[0x28] = 0x19; img[0x29] = 0x83;     /* 428 inc r1 / ret */
    img[0x40] = 0x5A;                       /* 440 */
}

static void game_tests(o2debug *d, const char *tmp)
{
    char out[8192], path[800], msg[256];
    o2debug_cpu c, c2;
    o2debug_breakpoint bps[16];
    o2debug_line lines[32];
    o2debug_cart cart;
    i8244_snapshot snap;
    int n, id, i, vw, vh;

    /* the cartridge */
    o2debug_cart_get(d, &cart);
    check(cart.present && cart.type == O2_CART_STD && strcmp(cart.type_name, "Standard") == 0 && cart.rom_size == 2048,
          "cartridge: a standard 2K game");
    check(!cart.mailbox && !cart.link_up && !cart.voice_in_front, "with no FujiNet behind it");
    o2debug_cart_info(d, out, sizeof out);
    check(strcmp(out, "Standard cartridge, 2048 bytes") == 0, "its one line");
    o2debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "Standard") && strstr(out, "bank"), "the prompt's cart for a mapper");
    check(o2debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "not the FujiNet"), "and no FujiNet state");
    o2debug_save(d, "mailbox", (snprintf(path, sizeof path, "%s/game.mailbox", tmp), path), msg, sizeof msg);
    check(file_size(path) == 0xE0, "a mapper cartridge's $F20-$FFF page saves from what the console reads");

    /* the program, disassembled */
    n = o2debug_disassemble(d, 0x400, lines, 24, NULL);
    check(n == 24 && strncmp(lines[0].disasm, "anl", 3) == 0 && strstr(lines[0].disasm, "p1,#$AF") &&
          strstr(lines[1].disasm, "mov  r0,#$10") && strncmp(lines[3].disasm, "movx @r0,a", 10) == 0,
          "the game's first instructions");
    i = find_line(lines, n, "call");
    check(i >= 0 && lines[i].address == 0x40A && strstr(lines[i].disasm, "$420") && lines[i].comment[0] == '\0',
          "a CALL in the lower half names its 11-bit target");
    i = find_line(lines, n, "djnz");
    check(i >= 0 && lines[i].address == 0x40E && strstr(lines[i].disasm, "r1,$40E"), "DJNZ shows its target");

    /* breakpoints on the accesses, in program order */
    {
        static const struct { int type, space; int lo, hi; const char *cond, *reason; int pc; } bp[] = {
            { O2DEBUG_BP_WRITE, O2_SPACE_XDATA, 0x10, 0x10, "value == $77", "write xdata $10 = $77", 0x407 },
            { O2DEBUG_BP_READ, O2_SPACE_XDATA, 0x10, 0x1F, "", "read xdata $10 = $77", 0x408 },
            { O2DEBUG_BP_WRITE, O2_SPACE_IRAM, 0x10, 0x10, "", "write iram $10 = $77", 0x409 },
            { O2DEBUG_BP_READ, O2_SPACE_IRAM, 0x10, 0x10, "", "read iram $10 = $77", 0x40A },
        };
        for (i = 0; i < 4; i++)
            check(o2debug_breakpoint_add(d, bp[i].type, bp[i].space, (uint16_t)bp[i].lo, (uint16_t)bp[i].hi, bp[i].cond) > 0,
                  "an access breakpoint is added");
        for (i = 0; i < 4; i++) {
            char what[96];
            snprintf(what, sizeof what, "stops at $%03X: %s", bp[i].pc, bp[i].reason);
            check(run_until_stop(d, 3000) && cpu(d).pc == bp[i].pc && strcmp((why(d), reason), bp[i].reason) == 0, what);
            show("pc %03X reason '%s'", cpu(d).pc, reason);
        }
        n = o2debug_breakpoint_list(d, bps, 16);
        check(n == 4 && bps[0].hits == 1 && bps[1].hits == 1 && bps[2].hits == 1 && bps[3].hits == 1, "each hit exactly once");
        c = cpu(d);
        check(c.a == 0x77 && c.r[0][0] == 0x10 && c.p1 == 0xAF && c.sp == 0, "A, R0 and P1 are what the program made them");
        o2debug_breakpoint_clear(d);
    }

    /* step over a CALL: the routine, a nested call, both returns -- six instructions */
    c = cpu(d);
    check(c.pc == 0x40A, "at the CALL");
    o2debug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over stops");
    c2 = cpu(d);
    check(c2.pc == 0x40C && c2.sp == 0 && c2.instructions == c.instructions + 6 && c2.r[0][0] == 0x11 && c2.r[0][1] == 0x01,
          "step over runs the call (inc r0, call, inc r1, ret, ret) and stops after it");
    check(strcmp((why(d), reason), "step over") == 0, "reason 'step over'");

    /* DJNZ is stepped over like a call: the whole loop */
    o2debug_run_to(d, 0x40E);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x40E && cpu(d).r[0][1] == 3, "run to the DJNZ");
    c = cpu(d);
    o2debug_step_over(d);
    check(wait_stopped(d, 1, 3000), "step over a DJNZ stops");
    c2 = cpu(d);
    check(c2.pc == 0x410 && c2.instructions == c.instructions + 3 && c2.r[0][1] == 0, "the loop ran its three trips");

    /* step into a CALL, then out of it; and out of a nested one */
    o2debug_cpu_set(d, O2_REG_PC, 0x40A);
    o2debug_step(d);
    check(wait_stopped(d, 1, 3000), "step into stops");
    c = cpu(d);
    check(c.pc == 0x420 && c.sp == 1, "a step into a CALL is in the routine, one deeper");
    o2debug_step_out(d);
    check(wait_stopped(d, 1, 3000), "step out stops");
    c2 = cpu(d);
    check(c2.pc == 0x40C && c2.sp == 0 && strcmp((why(d), reason), "step out") == 0 &&
          c2.instructions == c.instructions + 5, "step out runs to the routine's RET (the nested call and its RET included)");
    o2debug_cpu_set(d, O2_REG_PC, 0x421);
    o2debug_step(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x428 && cpu(d).sp == 1, "into the nested routine");
    o2debug_step_out(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x423 && cpu(d).sp == 0, "out of it, one level");
    o2debug_cpu_set(d, O2_REG_PC, 0x421);
    o2debug_step_over(d);
    check(wait_stopped(d, 1, 3000) && cpu(d).pc == 0x423, "step over the nested CALL");

    /* IN, OUT and MOVP breakpoints */
    o2debug_cpu_set(d, O2_REG_PC, 0x410);
    id = o2debug_breakpoint_add(d, O2DEBUG_BP_IN, 1, 0, 0, "");
    check(id > 0 && o2debug_breakpoint_add(d, O2DEBUG_BP_OUT, 2, 0, 0, "value == $AF") > 0 &&
          o2debug_breakpoint_add(d, O2DEBUG_BP_IN, 0, 0, 0, "") > 0 &&
          o2debug_breakpoint_add(d, O2DEBUG_BP_READ, O2_SPACE_PROG, 0x440, 0x440, "value == $5A") > 0, "port and MOVP breakpoints");
    check(run_until_stop(d, 3000) && cpu(d).pc == 0x411 && strcmp((why(d), reason), "IN p1 -> $AF") == 0, "IN A,P1");
    check(run_until_stop(d, 3000) && cpu(d).pc == 0x412 && strcmp((why(d), reason), "OUT p2 <- $AF") == 0, "OUTL P2,A");
    check(run_until_stop(d, 3000) && cpu(d).pc == 0x413 && strncmp((why(d), reason), "IN bus -> $", 11) == 0, "INS A,BUS");
    check(run_until_stop(d, 3000) && cpu(d).pc == 0x416 && strcmp((why(d), reason), "read prog $440 = $5A") == 0, "MOVP A,@A");
    check(cpu(d).a == 0x5A, "and A holds what it read");
    o2debug_breakpoint_clear(d);
    o2debug_resume(d);
    sleep_ms(50);
    check(!o2debug_is_stopped(d), "the program idles at $416");
    check(stop_now(d) && cpu(d).pc == 0x416, "stopped in its idle loop");

    /* PAL: the beam, scanlines, frames */
    o2debug_vdc_get(d, &snap);
    check(snap.is_pal && snap.frame_hz > 49.0 && snap.frame_hz < 50.5, "the 8245 at 49.7 Hz");
    c = cpu(d);
    check(c.vpos >= 0 && c.vpos < 313 && c.hpos >= 0 && c.hpos < 456, "the beam is in the PAL raster");
    o2debug_scanline(d, 1);
    check(wait_stopped(d, 1, 3000) && cpu(d).vpos == (c.vpos + 1) % 313, "scanline+1 on a 313-line frame");
    c = cpu(d);
    o2debug_scanline(d, 313);
    check(wait_stopped(d, 1, 3000) && cpu(d).vpos == c.vpos && cpu(d).frame == c.frame + 1, "313 scanlines is one frame");
    c = cpu(d);
    o2debug_frame(d, 2);
    check(wait_stopped(d, 1, 3000) && cpu(d).frame == c.frame + 2, "frame+2");

    /* the G7400's EF9340 and The Voice, put in place by hand (the machine is parked) */
    {
        o2_machine_t *m = o2debug_machine();
        ef9340_snapshot_t ef;
        o2debug_io io;
        static uint32_t efimg[360 * 243];

        o2_host_lock();
        ef9340_init(&m->ef, NULL);
        ef9340_reset(&m->ef, 0);
        m->have_ef = true;
        o2_host_unlock();
        check(o2debug_ef_present(d), "an EF9340 present");
        o2debug_ef_get(d, &ef);
        check(!ef.font_present && ef.x == 0, "its snapshot");
        check(o2debug_ef_view(d, efimg, 360 * 243, &vw, &vh) && vw == 360 && vh == 243, "its text layer view");
        check(!o2debug_ef_view(d, efimg, 10, &vw, &vh), "which refuses a buffer too small");
        o2debug_command(d, "ef", out, sizeof out);
        check(strstr(out, "EF9340: R=") && strstr(out, " 0 |"), "and its page at the prompt");
        o2_host_lock();
        m->have_ef = false;
        o2_host_unlock();

        o2_host_lock();
        sp0256_init(&m->speech, NULL, NULL, 0);
        m->have_voice = true;
        o2_host_unlock();
        o2debug_io_get(d, &io);
        check(io.voice_present && io.voice.clock_hz == 3120000 && io.voice.rate_hz == 10000, "The Voice's chip state in the I/O view");
        o2debug_cart_get(d, &cart);
        check(cart.voice_in_front, "and the cartridge sits behind it");
        o2debug_command(d, "voice", out, sizeof out);
        check(strstr(out, "SP0256: LRQ") && strstr(out, "3120000 Hz clock"), "and at the prompt");
        o2_host_lock();
        m->have_voice = false;
        o2_host_unlock();
    }
    (void)c2;
}

/* ---- the program under test --------------------------------------------------- */

int main(void)
{
    static o2debug_fuji_packet pk[FUJITRACE_MAX];
    char tmp[512], out[16384], path[800], msg[300], boip[64] = "";
    static uint32_t img[O2DEBUG_VDC_VIEW_MAX_PIXELS * 4], img2[O2DEBUG_VDC_VIEW_MAX_PIXELS * 4];
    o2_host_opts_t ho;
    o2debug *d;
    o2debug_cpu c, c2, saved;
    o2debug_line lines[64];
    o2debug_breakpoint bps[16];
    int n, w, h, pc_line = -1, id, id2, i, fresh;
#ifndef _WIN32
    fake_fujinet fake;
    pthread_t fake_th;
    int fake_up = 0;
#endif

    test_tmpdir(tmp, sizeof tmp, "dbg");

    d = o2debug_create();
    check(d != NULL, "an engine");
    if (!d) return 1;

    /* ---- with no machine at all ---- */
    check(!o2debug_is_attached(d) && !o2debug_is_stopped(d), "a new engine is detached");
    c = cpu(d);
    check(c.pc == 0 && c.cycles == 0, "no machine: a zero CPU view");
    check(o2debug_read(d, O2_SPACE_PROG, 0, (uint8_t *)out, 4) == 0, "no machine: no memory");
    check(!o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 1, NULL, 0, &w, &h), "no machine: no VDC view");
    check(!o2debug_ef_present(d), "no machine: no EF9340");
    check(o2debug_cart_info(d, out, sizeof out) > 0, "no machine: a cartridge line");
    o2debug_command(d, "regs", out, sizeof out);
    check(out[0] != '\0', "no machine: the prompt still answers");
    check(o2debug_label_address(d, "readkey") == 0x0B0, "built-in labels need no machine");

    /* ---- the FujiNet trace is installed first, from session start ---- */
    o2debug_fujinet_install(d);
#ifndef _WIN32
    {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        int one = 1;

        memset(&fake, 0, sizeof fake);
        fake.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        setsockopt(fake.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (fake.listen_fd >= 0 && bind(fake.listen_fd, (struct sockaddr *)&a, sizeof a) == 0 &&
            listen(fake.listen_fd, 1) == 0 &&
            getsockname(fake.listen_fd, (struct sockaddr *)&a, &al) == 0 &&
            pthread_create(&fake_th, NULL, fake_main, &fake) == 0) {
            snprintf(boip, sizeof boip, "127.0.0.1:%d", ntohs(a.sin_port));
            fake_up = 1;
        }
    }
#endif

    /* ---- attached before the machine exists: it stops on its first instruction ---- */
    o2debug_attach(d);
    check(o2debug_is_attached(d), "attached with no machine to hook yet");
    id = o2debug_breakpoint_add(d, O2DEBUG_BP_WRITE, O2_SPACE_XDATA, 0xA0, 0xA0, "value == $20 && addr == $A0");
    id2 = o2debug_breakpoint_add(d, O2DEBUG_BP_WRITE, O2_SPACE_VDC, 0xA3, 0xA3, "");
    check(id > 0 && id2 > 0, "write breakpoints on external data and on the VDC, set before the machine");

    memset(&ho, 0, sizeof ho);
    ho.machine = O2_MACHINE_ODYSSEY2;
    ho.boip_hostport = boip[0] ? boip : NULL;
    if (o2_host_start(&ho) != 0) {
        printf("FAIL: the host does not start: %s\n", o2_host_last_error());
        return 1;
    }
    o2debug_power_up(d);
    check(wait_stopped(d, 1, 3000), "the new machine stops in the debugger at once");
    c = cpu(d);
    /* The host's thread starts running as soon as o2_host_start returns, and the
     * hook goes in a moment later: on a slow or loaded machine the console may
     * have run some instructions first. Say so, and carry on from the reset
     * vector (what is skipped is the boot-time part). */
    fresh = c.pc == 0 && c.instructions == 0;
    if (!fresh) {
        printf("note: the machine ran before the hook went in (pc %03X, %llu instructions): "
               "the boot-time checks are skipped\n", c.pc, (unsigned long long)c.instructions);
        o2debug_cpu_set(d, O2_REG_PC, 0);
        c = cpu(d);
    }
    check(c.pc == 0 && c.machine == O2_MACHINE_ODYSSEY2, "on the reset vector");
    if (fresh) {
        check(c.instructions == 0 && c.cycles == 0, "before its first instruction");
        check(c.vpos == 243 && c.hpos == 0 && c.frame == 0, "the beam starts at the vertical blank");
    }
    check(why(d) == 0 && strcmp(reason, "stopped") == 0, "the stop reason is 'stopped', at the PC");
    n = o2debug_disassemble(d, 0, lines, 3, &pc_line);
    check(n == 3 && pc_line == 0 && lines[0].is_pc, "disassembly marks the PC's line");
    check(strncmp(lines[0].disasm, "jmp", 3) == 0 && strstr(lines[0].disasm, "$400"),
          "the stand-in's reset vector is jmp $400");
    check(strcmp(lines[0].label, "reset") == 0 && strcmp(lines[0].comment, "cart_start") == 0,
          "labelled 'reset', branching to 'cart_start'");
    check(strcmp(lines[1].label, "") == 0 && lines[1].address == 2, "the next line follows the instruction");
    o2debug_step(d);
    check(wait_stopped(d, 1, 2000), "stopped after a step");
    c2 = cpu(d);
    check(c2.pc == 0x400 && c2.instructions == c.instructions + 1 && c2.cycles == c.cycles + 2,
          "a step runs jmp $400 (two cycles)");
    check(why(d) == 0x400 && strcmp(reason, "step") == 0, "the reason is 'step', at the new PC");

    /* the breakpoints set before the machine existed fire during CONFIG's boot */
    if (fresh) {
        int saw_xdata = 0, saw_vdc = 0;
        for (i = 0; i < 6 && !(saw_xdata && saw_vdc); i++) {
            if (!run_until_stop(d, 5000)) break;
            why(d);
            if (strstr(reason, "write xdata $A0 = $20")) saw_xdata = 1;
            if (strstr(reason, "write vdc $A3 = $")) saw_vdc = 1;
        }
        check(saw_xdata, "an external-data write breakpoint with `value` and `addr` conditions");
        check(saw_vdc, "a VDC-space write breakpoint");
        n = o2debug_breakpoint_list(d, bps, 16);
        check(n == 2 && bps[0].hits >= 1 && bps[1].hits >= 1 && bps[0].type == O2DEBUG_BP_WRITE &&
              bps[0].space == O2_SPACE_XDATA && bps[1].space == O2_SPACE_VDC && bps[0].start == 0xA0,
              "listed with their hit counts, space and range");
        {
            uint8_t v = 0;
            o2debug_read(d, O2_SPACE_VDC, 0xA0, &v, 1);
            check(saw_xdata && v == 0x20, "the VDC holds what the breakpoint stopped on");
        }
    }
    o2debug_breakpoint_clear(d);
    check(o2debug_breakpoint_list(d, bps, 16) == 0, "cleared");

    /* let CONFIG settle, then stop wherever it is */
    o2debug_resume(d);
    {
        int waited = 0;
        while (o2_host_frame_count() < 90 && waited < 5000) { sleep_ms(10); waited += 10; }
        check(o2_host_frame_count() >= 90, "the machine runs with the debugger attached");
    }
    check(stop_now(d), "stop");
    check(why(d) >= 0 && strcmp(reason, "stopped") == 0, "stopped on request");

    /* ---- the CPU view ---- */
    c = cpu(d);
    check(c.pc >= 0 && c.pc < 0x1000 && c.mb >= 0 && c.mb <= 1, "PC is 12 bits, MB a latch");
    check((c.psw & 0x08) && c.sp == (c.psw & 7) && c.cy == ((c.psw >> 7) & 1) &&
          c.ac == ((c.psw >> 6) & 1) && c.f0 == ((c.psw >> 5) & 1) && c.bs == ((c.psw >> 4) & 1),
          "PSW and its decode agree");
    check(c.frame >= 80 && c.cycles > 100000 && c.instructions > 10000, "time has passed");
    check(c.vpos >= 0 && c.vpos < 263 && c.hpos >= 0 && c.hpos < 455, "the beam is in the NTSC raster");
    check(c.r[c.bs][2] == c.iram[(c.bs ? 24 : 0) + 2], "R2 of the bank in use is its internal RAM byte");
    check(c.timer_mode >= 0 && c.timer_mode <= 2 && (c.t1 == 0 || c.t1 == 1) && (c.t0 == 0 || c.t0 == 1),
          "timer mode and the T0/T1 pins");
    check(c.tcnti_en == 1 && c.ext_int_en == 1, "CONFIG enabled both interrupts");
    check(c.p1 == (int)cpu(d).p1 && c.p1 != 0, "the port latches");

    /* ---- register edits (restored afterwards) ---- */
    saved = c;
    o2debug_cpu_set(d, O2_REG_A, 0x5A);
    o2debug_cpu_set(d, O2_REG_R0 + 3, 0xC3);
    o2debug_cpu_set(d, O2_REG_R0_B1 + 6, 0x66);
    o2debug_cpu_set(d, O2_REG_T, 0x12);
    o2debug_cpu_set(d, O2_REG_CNT, 0x07);
    o2debug_cpu_set(d, O2_REG_F1, !saved.f1);
    o2debug_cpu_set(d, O2_REG_MB, !saved.mb);
    o2debug_cpu_set(d, O2_REG_C, !saved.cy);
    o2debug_cpu_set(d, O2_REG_PC, 0x321);
    c2 = cpu(d);
    check(c2.a == 0x5A && c2.r[c.bs][3] == 0xC3 && c2.r[1][6] == 0x66, "A and registers (the bank in use, and bank 1) take edits");
    check(c2.timer == 0x12 && c2.prescaler == 0x07 && c2.f1 == !saved.f1 && c2.mb == !saved.mb &&
          c2.cy == !saved.cy && c2.psw == (saved.psw ^ 0x80), "T, the prescaler, F1, MB and C too");
    check(c2.pc == 0x321, "PC takes an edit");
    {
        /* BS switches the bank the R registers mean */
        o2debug_cpu_set(d, O2_REG_BS, !saved.bs);
        c2 = cpu(d);
        check(c2.bs == !saved.bs && c2.r[c2.bs][3] == (c2.bs == saved.bs ? 0xC3 : c2.r[c2.bs][3]),
              "BS flips the register bank in use");
        o2debug_cpu_set(d, O2_REG_R0 + 1, 0x99);
        check(cpu(d).r[!saved.bs][1] == 0x99, "R0+n then means the other bank");
    }
    o2debug_cpu_set(d, O2_REG_PSW, 0xFF);
    c2 = cpu(d);
    check(c2.psw == 0xFF && c2.sp == 7 && c2.cy && c2.ac && c2.f0 && c2.bs, "PSW takes a whole byte");
    o2debug_cpu_set(d, O2_REG_SP, 2);
    check(cpu(d).sp == 2, "SP");
    /* restore */
    for (i = 0; i < 64; i++) o2debug_write(d, O2_SPACE_IRAM, (uint16_t)i, saved.iram[i]);
    o2debug_cpu_set(d, O2_REG_PSW, saved.psw);
    o2debug_cpu_set(d, O2_REG_A, saved.a);
    o2debug_cpu_set(d, O2_REG_T, saved.timer);
    o2debug_cpu_set(d, O2_REG_CNT, saved.prescaler);
    o2debug_cpu_set(d, O2_REG_F1, saved.f1);
    o2debug_cpu_set(d, O2_REG_MB, saved.mb);
    o2debug_cpu_set(d, O2_REG_PC, saved.pc);
    c2 = cpu(d);
    check(c2.pc == saved.pc && c2.psw == saved.psw && c2.a == saved.a &&
          memcmp(c2.iram, saved.iram, 64) == 0 && c2.mb == saved.mb, "the CPU is as it was");

    /* ---- the disassembly ---- */
    n = o2debug_disassemble(d, 0x0B0, lines, 12, NULL);
    check(n == 12 && strcmp(lines[0].label, "readkey") == 0 && strncmp(lines[0].disasm, "dis", 3) == 0 &&
          strstr(lines[0].disasm, " i") && lines[0].length == 1 && strcmp(lines[0].bytes, "15") == 0,
          "the stand-in's readkey is labelled and starts DIS I");
    check(strstr(lines[1].disasm, "sel") && strstr(lines[1].disasm, "rb0") && lines[1].address == 0x0B1,
          "then SEL RB0");
    check(strstr(lines[2].disasm, "in") && strstr(lines[2].disasm, "a,p1"), "then IN A,P1");
    i = find_line(lines, n, "anl");
    check(i >= 0 && lines[i].length == 2 && strstr(lines[i].disasm, "#$FB") && strcmp(lines[i].bytes, "53 FB") == 0,
          "a two-byte instruction shows both bytes");
    n = o2debug_disassemble(d, 0x38F, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "getjoystick") == 0, "getjoystick is labelled");
    n = o2debug_disassemble(d, 3, lines, 1, NULL);
    check(n == 1 && strcmp(lines[0].label, "int_vector") == 0 && strcmp(lines[0].comment, "cart_irq") == 0,
          "the interrupt vector branches to cart_irq");
    check(o2debug_row_address(d, 0x0B4, -3) == 0x0B1, "three rows before $0B4 is $0B1");
    check(o2debug_row_address(d, 0x0B4, 1) == 0x0B6, "one row after the two-byte instruction at $0B4 is $0B6");
    check(o2debug_row_address(d, 0x0B4, 0) == 0x0B4, "no rows is the same address");
    check(o2debug_row_address(d, 0x002, -1) == 0x000, "backwards from $002 lands on the vector");
    n = o2debug_disassemble(d, (uint16_t)o2debug_row_address(d, saved.pc, -4), lines, 16, &pc_line);
    check(pc_line >= 0 && lines[pc_line].address == saved.pc && lines[pc_line].is_pc, "the PC's line is found among the rows");
    check(o2debug_label_address(d, "getjoystick") == 0x38F && o2debug_label_address(d, "nonesuch") == -1, "label lookup");
    check(o2debug_label_address(d, "FN_R_ACKSEQ") == 0xF20 && o2debug_label_address(d, "FN_R_DATA") == 0xF30,
          "the mailbox status page's labels come from fuji_mailbox.h");
    check(o2debug_label_address(d, "waitvsync") == 0x176 && o2debug_label_address(d, "printchar") == 0x3EA &&
          o2debug_label_address(d, "timer_vector") == 7, "the BIOS entry points and vectors");
    o2debug_address_label(d, 0x38F, out, sizeof out);
    check(strcmp(out, "getjoystick") == 0, "address to label");

    /* ---- memory in the four spaces ---- */
    {
        uint8_t b[8], v;
        check(o2debug_read(d, O2_SPACE_PROG, 0, b, 5) == 5 && b[0] == 0x84 && b[1] == 0x00 && b[3] == 0x84 && b[4] == 0x02,
              "program space: the vectors (jmp $400, jmp $402)");
        check(o2debug_read(d, O2_SPACE_PROG, 0x0FFE, b, 4) == 4, "program space wraps at 4K");
        o2debug_read(d, O2_SPACE_PROG, 0x400, b, 4);
        check(o2debug_read(d, O2_SPACE_PROG, FN_R_MAGIC0, b, 3) == 3 && b[0] == 'F' && b[1] == 'N' && b[2] == FN_PROTO_VER,
              "the cartridge's page reads through the FujiNet cartridge ('FN', protocol)");
        o2debug_write(d, O2_SPACE_IRAM, 0x3E, 0xAB);
        o2debug_read(d, O2_SPACE_IRAM, 0x3E, &v, 1);
        check(v == 0xAB, "internal RAM takes a write");
        o2debug_read(d, O2_SPACE_IRAM, 0x7E, &v, 1);
        check(v == 0xAB, "and wraps at 64 bytes");
        o2debug_write(d, O2_SPACE_IRAM, 0x3E, saved.iram[0x3E]);

        /* the 6810 RAM, with the cartridge/RAM select on and the VDC off */
        {
            const int p1 = c.p1;
            o2debug_cpu_set(d, O2_REG_P1, (p1 | 0x08) & ~0x10);
            o2debug_read(d, O2_SPACE_XDATA, 0x1F, &v, 1);
            b[7] = v;
            o2debug_write(d, O2_SPACE_XDATA, 0x1F, 0x77);
            o2debug_read(d, O2_SPACE_XDATA, 0x1F, &v, 1);
            check(v == 0x77, "external data: the RAM takes a write");
            o2debug_write(d, O2_SPACE_XDATA, 0x1F, b[7]);
            o2debug_write(d, O2_SPACE_XDATA, 0x9F, 0x11);
            o2debug_cpu_set(d, O2_REG_P1, p1);
            check(cpu(d).p1 == p1, "P1 is restored");
        }
        o2debug_read(d, O2_SPACE_VDC, 0x80, &v, 1);
        b[6] = v;
        o2debug_vdc_write(d, 0x80, 0x5A);
        o2debug_read(d, O2_SPACE_VDC, 0x80, &v, 1);
        check(v == 0x5A, "the VDC takes a write (shape row 0 of sprite 0)");
        o2debug_vdc_write(d, 0x80, b[6]);
        o2debug_read(d, O2_SPACE_PROG, 0x420, &b[5], 1);
        o2debug_write(d, O2_SPACE_PROG, 0x420, (uint8_t)(b[5] ^ 0xFF));
        o2debug_read(d, O2_SPACE_PROG, 0x420, &v, 1);
        check(v == b[5], "program space is read-only (the cartridge has no poke API)");
    }

    /* ---- step over, step out, run to ---- */
    {
        o2debug_trace tr[4096];
        int call_pc = -1, tries;

        o2debug_trace_enable(d, 1);
        check(o2debug_trace_enabled(d), "trace on");
        o2debug_frame(d, 1);
        check(wait_stopped(d, 1, 3000), "stopped after a frame");
        n = o2debug_trace_read(d, tr, 4096);
        for (i = n - 1; i >= 0 && call_pc < 0; i--)
            if ((tr[i].bytes[0] & 0x1f) == 0x14 && tr[i].pc >= 0x400)
                call_pc = tr[i].pc;
        o2debug_trace_enable(d, 0);
        check(call_pc >= 0, "CONFIG calls a routine within a frame (found in the trace)");

        for (tries = 0; tries < 8 && call_pc >= 0; tries++) {
            int ret = (call_pc + 2) & 0xfff, sp0, indirq;
            id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, (uint16_t)call_pc, (uint16_t)call_pc, "");
            if (!run_until_stop(d, 5000)) break;
            o2debug_breakpoint_remove(d, id);
            c = cpu(d);
            if (c.pc != call_pc) continue;
            sp0 = c.sp;
            indirq = c.in_interrupt;
            if (tries == 0) {
                check(strstr((why(d), reason), "breakpoint: exec") != NULL, "an execute breakpoint, with its reason");
                n = o2debug_disassemble(d, (uint16_t)call_pc, lines, 1, NULL);
                check(n == 1 && lines[0].has_breakpoint == 0 && strncmp(lines[0].disasm, "call", 4) == 0,
                      "the instruction is a CALL (and the breakpoint was removed)");
            }
            /* step over: back after the call with the stack where it was */
            o2debug_step_over(d);
            check(wait_stopped(d, 1, 5000), "step over stops");
            c2 = cpu(d);
            check(c2.pc == ret && c2.sp == sp0 && (why(d), strcmp(reason, "step over") == 0),
                  "step over a CALL returns to the next instruction, same stack");
            check(c2.cycles > c.cycles + 2, "having run the routine");
            /* step into, then out */
            id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, (uint16_t)call_pc, (uint16_t)call_pc, "");
            if (!run_until_stop(d, 5000)) break;
            o2debug_breakpoint_remove(d, id);
            c = cpu(d);
            if (c.pc != call_pc || c.in_interrupt != indirq) continue;
            sp0 = c.sp;
            o2debug_step(d);
            check(wait_stopped(d, 1, 2000), "step into stops");
            c2 = cpu(d);
            if (c2.in_interrupt != indirq) continue;     /* an interrupt took the step: try again */
            check(c2.sp == sp0 + 1 && c2.pc != ret, "a step into a CALL is in the routine, one deeper");
            o2debug_step_out(d);
            check(wait_stopped(d, 1, 5000), "step out stops");
            c2 = cpu(d);
            check(c2.pc == ret && c2.sp == sp0 && (why(d), strcmp(reason, "step out") == 0),
                  "step out returns to the caller's next instruction");
            break;
        }
        check(tries < 8, "step over / step into / step out ran on a CALL");

        /* run to: inside readkey, past its DIS I, where no interrupt can intervene */
        o2debug_trace_clear(d);
        o2debug_trace_enable(d, 1);
        o2debug_run_to(d, 0x0B2);
        check(wait_stopped(d, 1, 5000) && cpu(d).pc == 0x0B2 && (why(d), strcmp(reason, "run to") == 0),
              "run to $0B2 in readkey");
        c = cpu(d);
        o2debug_step_over(d);       /* not a CALL: a plain step */
        check(wait_stopped(d, 1, 2000) && cpu(d).instructions == c.instructions + 1 && cpu(d).pc == 0x0B3,
              "step over something that is not a CALL is one step");
        o2debug_command(d, "step 5", out, sizeof out);
        check(wait_stopped(d, 1, 2000) && cpu(d).instructions == c.instructions + 6, "`step 5` runs five instructions");
        check(strcmp((why(d), reason), "step") == 0, "and stops as 'step'");
        check(o2debug_trace_read(d, tr, 16) >= 6 && tr[0].pc == cpu(d).pc, "the trace ring's newest entry is the PC");
        o2debug_trace_enable(d, 0);
        o2debug_trace_clear(d);
        check(o2debug_trace_read(d, tr, 1) == 0, "trace clear");
    }

    /* ---- scanline and frame ---- */
    {
        o2debug_cpu a, b;
        a = cpu(d);
        o2debug_scanline(d, 1);
        check(wait_stopped(d, 1, 3000), "scanline stops");
        b = cpu(d);
        check(b.vpos == (a.vpos + 1) % 263, "scanline+1 advances the beam one line");
        check(strcmp((why(d), reason), "scanline") == 0, "reason 'scanline'");
        a = b;
        o2debug_command(d, "scanline 7", out, sizeof out);
        check(wait_stopped(d, 1, 3000), "scanline 7 stops");
        b = cpu(d);
        check(b.vpos == (a.vpos + 7) % 263, "scanline+7 advances seven lines");
        a = b;
        o2debug_frame(d, 1);
        check(wait_stopped(d, 1, 3000), "frame stops");
        b = cpu(d);
        check(b.frame == a.frame + 1 && strcmp((why(d), reason), "frame") == 0, "frame+1 advances one frame");
        a = b;
        o2debug_command(d, "frame 3", out, sizeof out);
        check(wait_stopped(d, 1, 3000) && cpu(d).frame == a.frame + 3, "frame 3 advances three");
    }

    /* ---- breakpoints of every class ---- */
    {
        o2debug_line ln[32];
        int k;

        check(o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, 0xB0, 0xB0, "this bogus ((") == -1,
              "an unparsable condition is refused");
        check(o2debug_breakpoint_add(d, 0, 0, 0, 0, "") == -1, "no type is refused");
        check(o2debug_breakpoint_add(d, O2DEBUG_BP_READ, 9, 0, 0, "") == -1, "a bad space is refused");
        check(o2debug_breakpoint_add(d, O2DEBUG_BP_OUT, 3, 0, 0, "") == -1, "a bad port is refused");
        check(o2debug_breakpoint_toggle(d, 0x0B0) == 1 && o2debug_breakpoint_check(d, 0x0B0), "toggle sets an execute breakpoint");
        n = o2debug_disassemble(d, 0x0B0, ln, 1, NULL);
        check(n == 1 && ln[0].has_breakpoint, "the line shows it");
        check(o2debug_breakpoint_toggle(d, 0x0B0) == 0 && !o2debug_breakpoint_check(d, 0x0B0), "toggle again removes it");

        /* a false condition never stops */
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, 0x0B0, 0x0B0, "sp == 31");
        o2debug_resume(d);
        sleep_ms(400);
        check(!o2debug_is_stopped(d), "a false condition never stops");
        check(stop_now(d), "stop");
        o2debug_breakpoint_remove(d, id);
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_EXEC, O2_SPACE_PROG, 0x0B0, 0x0B0, "sp < 8 && frame > 5 && a == a");
        check(id > 0 && run_until_stop(d, 5000) && cpu(d).pc == 0x0B0, "a true condition stops at readkey");
        o2debug_breakpoint_enable(d, id, 0);
        n = o2debug_breakpoint_list(d, bps, 16);
        check(n == 1 && !bps[0].enabled && bps[0].hits >= 1 && strcmp(bps[0].condition, "sp < 8 && frame > 5 && a == a") == 0,
              "disabled, with its hit count and condition");
        o2debug_resume(d);
        sleep_ms(300);
        check(!o2debug_is_stopped(d), "a disabled breakpoint never stops");
        check(stop_now(d), "stop");
        o2debug_breakpoint_clear(d);

        /* IN / OUT on the ports: readkey and getjoystick run once a frame */
        n = o2debug_disassemble(d, 0x0B0, ln, 24, NULL);
        {
            /* each runs from wherever the machine is: the access is the
             * one-byte instruction just before the PC (the ISR does some
             * itself, so which routine it is does not matter) */
            struct { const char *cmd, *pre; uint8_t opcode; } t[] = {
                { "bpin p1", "IN p1 -> $", 0x09 },
                { "bpin p2", "IN p2 -> $", 0x0A },
                { "bpin bus", "IN bus -> $", 0x08 },
                { "bpout p1", "OUT p1 <- $", 0x39 },
                { "bpout p2", "OUT p2 <- $", 0x3A },
            };
            for (k = 0; k < 5; k++) {
                uint8_t before = 0;
                char what[96];
                snprintf(what, sizeof what, "`%s` stops right after the instruction, naming the access", t[k].cmd);
                o2debug_command(d, t[k].cmd, out, sizeof out);
                check(strstr(out, "breakpoint #") && run_until_stop(d, 5000), what);
                c = cpu(d);
                o2debug_read(d, O2_SPACE_PROG, (uint16_t)(c.pc - 1), &before, 1);
                check(strncmp((why(d), reason), t[k].pre, strlen(t[k].pre)) == 0 && before == t[k].opcode, what);
                show("pc %03X opcode before %02X reason '%s'", c.pc, before, reason);
                o2debug_breakpoint_clear(d);
            }
            (void)n;
        }
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_OUT, 2, 0, 0, "value == $F0 && addr == 2");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "OUT p2 <- $F0", 13) == 0,
              "an OUT condition on `value` and `addr` (the port)");
        o2debug_breakpoint_clear(d);

        /* program-data reads: CONFIG's MOVP table copy */
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_READ, O2_SPACE_PROG, 0x600, 0x6FF, "addr == $64B");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "read prog $64B = $", 18) == 0,
              "a program-data (MOVP) read breakpoint with an address condition");
        o2debug_breakpoint_clear(d);
        /* external-data reads: CONFIG's MOVX A,@R0 of RAM $17 */
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_READ, O2_SPACE_XDATA, 0x10, 0x1F, "");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "read xdata $", 12) == 0,
              "an external-data read breakpoint");
        o2debug_breakpoint_clear(d);
        /* internal RAM, judged after the instruction: a write to R7 of bank 0, a read of $3C */
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_WRITE, O2_SPACE_IRAM, 0, 0x3F, "addr == 7");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "write iram $07 = $", 18) == 0,
              "an internal-RAM write breakpoint (R7 of bank 0)");
        o2debug_breakpoint_clear(d);
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_READ, O2_SPACE_IRAM, 0x3C, 0x3C, "");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "read iram $3C = $", 17) == 0,
              "an internal-RAM read breakpoint (@R1)");
        o2debug_breakpoint_clear(d);
        id = o2debug_breakpoint_add(d, O2DEBUG_BP_WRITE, O2_SPACE_IRAM, 0x08, 0x17, "");
        check(id > 0 && run_until_stop(d, 5000) && strncmp((why(d), reason), "write iram $", 12) == 0,
              "a write to the stack bytes (a CALL or an interrupt entry)");
        o2debug_breakpoint_clear(d);
        /* with the iram watch gone, nothing is armed */
        o2debug_resume(d);
        sleep_ms(100);
        check(!o2debug_is_stopped(d), "and breakpoints that are cleared stop nothing");
        check(stop_now(d), "stop");
    }

    /* ---- the VDC ---- */
    {
        i8244_snapshot s;
        char name[64], text[200];
        int vis = -1, hit_kind = -1, hit_index = -1, bx = 0, by = 0, bw = 0, bh = 0;
        o2debug_hit hits[16];

        o2debug_vdc_get(d, &s);
        check(!s.is_pal && s.regs[0xA0] == s.control && s.sprites[0].reg_x == s.regs[1] &&
              s.sprites[1].reg_y == s.regs[4] && s.singles[0].reg_y == s.regs[0x10] &&
              s.quads[0].chars[0].index == 0 && s.quads[3].chars[3].index == 15,
              "the VDC snapshot: registers and objects agree");
        check(s.fg_en && s.grid_x0 == 13 && s.cell_w == 16, "CONFIG has the foreground on");
        for (i = 0; i < 4 && vis < 0; i++)
            if (s.sprites[i].visible) { vis = 0; hit_kind = 0; hit_index = i;
                bx = s.sprites[i].x; by = s.sprites[i].y; bw = s.sprites[i].width; bh = s.sprites[i].height; }
        for (i = 0; i < 12 && vis < 0; i++)
            if (s.singles[i].visible) { vis = 0; hit_kind = 1; hit_index = i;
                bx = s.singles[i].x; by = s.singles[i].y; bw = s.singles[i].width; bh = s.singles[i].height; }
        check(vis == 0, "CONFIG's screen has a visible object");
        o2debug_vdc_describe(d, 0xA0, name, sizeof name, text, sizeof text);
        check(name[0] != '\0', "register names");
        o2debug_vdc_describe(d, 0xA1, name, sizeof name, text, sizeof text);
        check(name[0] != '\0' && text[0] != '\0', "register text");
        o2debug_vdc_describe(d, 0x1234, name, sizeof name, text, sizeof text);
        check(name[0] != '\0', "an offset past $FF folds to a register");

        /* views */
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 1, NULL, 0, &w, &h) && w == 360 && h == 243,
              "FRAME: asking for the size");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 2, NULL, 0, &w, &h) && w == 720 && h == 486,
              "at scale 2 it is 720x486");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 1, img, 100, &w, &h) == 0 && w == 360,
              "a buffer too small draws nothing");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 1, img, sizeof img / sizeof img[0], &w, &h) &&
              w == 360 && h == 243, "FRAME draws");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FRAME, I8244_L_ALL, 0, 2, img2, sizeof img2 / sizeof img2[0], &w, &h) &&
              w == 720 && h == 486, "FRAME at scale 2 draws");
        {
            int same = 1;
            for (int y = 0; y < 243 && same; y += 7)
                for (int x = 0; x < 360 && same; x += 5) {
                    const uint32_t p = img[y * 360 + x];
                    same = img2[(2 * y) * 720 + 2 * x] == p && img2[(2 * y) * 720 + 2 * x + 1] == p &&
                           img2[(2 * y + 1) * 720 + 2 * x] == p && img2[(2 * y + 1) * 720 + 2 * x + 1] == p;
                }
            check(same, "the scaled frame is the frame, each pixel a 2x2 block");
        }
        {
            uint32_t bg_only[360 * 243], with_sprites[360 * 243];
            int uniform = 1, differ = 0;

            check(o2debug_vdc_view(d, O2DEBUG_VDC_RENDER, I8244_L_BG, 0, 1, bg_only, 360 * 243, &w, &h) &&
                  o2debug_vdc_view(d, O2DEBUG_VDC_RENDER, I8244_L_BG | I8244_L_SPRITES, 0, 1, with_sprites, 360 * 243, &w, &h),
                  "RENDER with layer masks");
            for (i = 1; i < 360 * 243; i++)
                if (bg_only[i] != bg_only[0]) uniform = 0;
            check(uniform, "the background layer alone is one colour");
            for (i = 0; i < 360 * 243; i++)
                if (bg_only[i] != with_sprites[i]) differ = 1;
            if (s.sprites[0].visible)
                check(differ, "a visible sprite is drawn on the sprite layer");
            check(o2debug_vdc_view(d, O2DEBUG_VDC_RENDER, I8244_L_ALL, I8244_F_BOXES | I8244_F_COLLISION | I8244_F_OVERLAP,
                                   1, img, sizeof img / sizeof img[0], &w, &h) && w == 360, "RENDER with every overlay");
        }
        check(o2debug_vdc_view(d, O2DEBUG_VDC_PROGRESS, 0, I8244_F_BEAM, 1, img, sizeof img / sizeof img[0], &w, &h) &&
              w == 360 && h == 243, "PROGRESS (the frame to the beam) draws");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_FONT, 0, 0, 2, NULL, 0, &w, &h) && w > 0 && h > 0, "FONT sheet size");
        {
            int fw = w;
            check(o2debug_vdc_view(d, O2DEBUG_VDC_FONT, 0, 0, 2, img, sizeof img / sizeof img[0], &w, &h) && w == fw,
                  "FONT draws (blank glyphs without the character set)");
        }
        check(o2debug_vdc_view(d, O2DEBUG_VDC_SHAPES, 0, 0, 3, img, sizeof img / sizeof img[0], &w, &h) && w > 0 && h > 0,
              "SHAPES draws");
        check(o2debug_vdc_view(d, O2DEBUG_VDC_PALETTE, 0, 0, 1, img, sizeof img / sizeof img[0], &w, &h) && w == 128 && h == 32,
              "PALETTE: 8x2 cells of 16");
        {
            uint32_t pal[16];
            int distinct = 0;
            (void)pal;
            for (i = 1; i < 16; i++)
                if (img[(i / 8) * 16 * 128 + (i % 8) * 16] != img[0]) distinct++;
            check(distinct >= 8, "the palette's colours differ");
        }
        check(!o2debug_vdc_view(d, 99, 0, 0, 1, img, sizeof img / sizeof img[0], &w, &h), "an unknown view is refused");

        /* object bitmaps */
        check(o2debug_vdc_object(d, 0, 0, 2, 0xFF000000u, img, 1024, &w, &h) && w == 16 && h == 16, "sprite 0, 2x");
        {
            int lit = 0, bgp = 0;
            for (i = 0; i < 256; i++) { if (img[i] == 0xFF000000u) bgp++; else lit++; }
            check(lit > 0 && bgp > 0, "a sprite's bitmap has lit pixels in its colour and background around");
        }
        check(o2debug_vdc_object(d, 1, 0, 3, 0, NULL, 0, &w, &h) && w == 24 && h > 0, "a single character's size");
        check(o2debug_vdc_object(d, 2, 5, 1, 0, img, 1024, &w, &h) && w == 8, "a quad member");
        check(o2debug_vdc_object(d, 3, 12, 2, 0, img, 1024, &w, &h) && w == 16 && h == 16, "a font glyph");
        check(!o2debug_vdc_object(d, 0, 4, 2, 0, img, 1024, &w, &h) && !o2debug_vdc_object(d, 3, 64, 2, 0, img, 1024, &w, &h),
              "a bad index is refused");
        check(!o2debug_vdc_object(d, 0, 0, 2, 0, img, 4, &w, &h), "a buffer too small");

        /* hit test */
        n = o2debug_vdc_hit_test(d, bx + bw / 2, by + bh / 2, hits, 16);
        {
            int found = 0;
            for (i = 0; i < n; i++)
                if (hits[i].kind == hit_kind && hits[i].index == hit_index) found = 1;
            check(n >= 1 && found, "the pointer over an object's box finds it");
        }
        check(o2debug_vdc_hit_test(d, -40, -40, hits, 16) == 0 && o2debug_vdc_hit_test(d, 400, 300, hits, 16) == 0,
              "and nothing outside the picture");
        check(o2debug_vdc_hit_test(d, bx + bw / 2, by + bh / 2, hits, 0) == 0, "max 0 asks for nothing");
        {
            /* overlap: stack sprite 1 on sprite 0 and a single character on both; sprite 0 is topmost */
            uint8_t save[0x14], lit_x = 0, lit_y = 0;
            int row, col, found_lit = 0;

            o2debug_read(d, O2_SPACE_VDC, 0, save, 0x14);
            o2debug_vdc_get(d, &s);
            for (row = 0; row < 8 && !found_lit; row++)
                for (col = 0; col < 8 && !found_lit; col++)
                    if ((s.sprites[0].shape[row] >> col) & 1) {
                        const int zpx = s.sprites[0].zoom ? 4 : 2;
                        lit_x = (uint8_t)(col * zpx + 1);
                        lit_y = (uint8_t)(row * zpx + 1);
                        found_lit = (s.sprites[0].s | s.sprites[0].x9) == 0;
                    }
            if (found_lit) {
                const int px = s.sprites[0].x + lit_x, py = s.sprites[0].y + lit_y;
                /* sprite 1: same place; single 0: same place */
                o2debug_vdc_write(d, 0x04, s.regs[0]);
                o2debug_vdc_write(d, 0x05, s.regs[1]);
                o2debug_vdc_write(d, 0x10, s.regs[0]);
                o2debug_vdc_write(d, 0x11, s.regs[1]);
                n = o2debug_vdc_hit_test(d, px, py, hits, 16);
                check(n >= 3 && hits[0].kind == 0 && hits[0].index == 0 && hits[1].kind == 0 && hits[1].index == 1,
                      "overlapping objects come back topmost first: sprite 0, sprite 1 ...");
                {
                    int single_after = 0;
                    for (i = 2; i < n; i++) if (hits[i].kind == 1 && hits[i].index == 0) single_after = 1;
                    check(single_after, "... and a single character under the sprites");
                }
                for (i = 0; i < 0x14; i++) o2debug_vdc_write(d, (unsigned)i, save[i]);
            } else
                printf("note: CONFIG's sprite 0 has no plain lit pixel: the overlap order was not exercised\n");
        }

        /* edits */
        {
            uint8_t v, orig;
            o2debug_read(d, O2_SPACE_VDC, 0x81, &orig, 1);
            o2debug_vdc_write(d, 0x81, 0xA5);
            o2debug_vdc_get(d, &s);
            check(s.sprites[0].shape[1] == 0xA5, "a VDC edit shows in the snapshot");
            o2debug_vdc_write(d, 0x81, orig);
            o2debug_read(d, O2_SPACE_VDC, 0x81, &v, 1);
            check(v == orig, "and is undone");
        }
    }

    /* ---- the EF9340 (G7400 only), I/O, the cartridge ---- */
    {
        ef9340_snapshot_t ef;
        o2debug_io io;
        o2debug_cart cart;

        check(!o2debug_ef_present(d) && !o2debug_ef_view(d, img, 100, &w, &h), "no EF9340 on the Odyssey 2");
        o2debug_ef_get(d, &ef);
        check(ef.r == 0 && !ef.display_on, "an empty EF9340 snapshot");
        o2debug_command(d, "ef", out, sizeof out);
        check(strstr(out, "G7400"), "the prompt says where the EF9340 is");
        o2debug_command(d, "voice", out, sizeof out);
        check(strstr(out, "not present"), "and that The Voice is absent");

        o2debug_io_get(d, &io);
        check(io.machine == O2_MACHINE_ODYSSEY2 && !io.have_bios && io.stub_running && !io.have_font,
              "I/O: the stand-in, no character set");
        check((io.missing & O2_MISSING_FONT) && !(io.missing & O2_MISSING_BIOS), "and the missing part is the font");
        check(io.cart_bank == (io.p1 & 3) && io.kbd_enable == !(io.p1 & 4) && io.vdc_select == !(io.p1 & 8) &&
              io.ram_select == !(io.p1 & 16) && io.copy_mode == !!(io.p1 & 0x40) && io.lum == !(io.p1 & 0x80) &&
              io.kbd_row == (io.p2 & 7), "P1 and P2 decoded by function");
        check(!io.voice_present && io.keys == 0 && io.sticks[0] == 0 && io.sticks[1] == 0 && !io.reset_held,
              "nothing held, no Voice");
        check(io.audio_samples > 10000, "audio samples have been made");

        o2debug_cart_get(d, &cart);
        check(cart.present && cart.type == O2_CART_FUJINET && strcmp(cart.type_name, "FujiNet") == 0, "cartridge: the FujiNet");
        check(cart.mailbox && cart.app_live && !cart.booted_game && cart.live_size == 25600 && cart.live_crc != 0,
              "running the baked CONFIG, mailbox live");
        check(cart.mailbox_page[0x09] == 'F' && cart.mailbox_page[0x0A] == 'N' && cart.mailbox_page[0x0B] == FN_PROTO_VER,
              "the mailbox page carries 'FN' and the protocol version");
        check(cart.nbanks == 12, "CONFIG has twelve APPBANK pages");
        o2debug_cart_info(d, out, sizeof out);
        check(strncmp(out, "FujiNet cartridge: CONFIG", 25) == 0, "the cartridge's one line");
        check(o2debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Link") && strstr(out, "Mailbox"), "the FujiNet state text");
#ifndef _WIN32
        if (fake_up) {
            int waited = 0;
            while (!cart.link_up && waited < 5000) { sleep_ms(50); waited += 50; o2debug_cart_get(d, &cart); }
            check(cart.link_up && !cart.link_off, "the cartridge's link comes up to the stand-in FujiNet");
        }
#endif
    }

    /* ---- the FujiNet trace, from session start, attached or not ---- */
#ifndef _WIN32
    if (fake_up) {
        int waited = 0, dirs[2] = { 0, 0 };
        o2debug_fujinet_trace_clear(d);
        /* CONFIG asks the FujiNet things as it runs; wait for one exchange */
        o2debug_resume(d);
        n = 0;
        while (waited < 8000) {
            n = o2debug_fujinet_trace(d, pk, 512);
            dirs[0] = dirs[1] = 0;
            for (i = 0; i < n; i++) dirs[pk[i].to_host ? 1 : 0] = 1;
            if (dirs[0] && dirs[1]) break;
            sleep_ms(50); waited += 50;
        }
        check(dirs[0] && dirs[1], "the trace saw packets both ways on the cartridge's own socket");
        if (n > 0)
            check(strstr(pk[0].text, "Odyssey 2") && pk[0].nbytes >= 6 && pk[0].serial > 0, "described, with the FujiBus header");
        o2debug_fujinet_trace_clear(d);
        check(o2debug_fujinet_trace(d, pk, 512) == 0, "clear empties the ring");
        check(stop_now(d), "stop again");
    } else
        printf("SKIP: no loopback listener, the FujiNet trace is untested here\n");
#else
    printf("SKIP: the loopback stand-in FujiNet is POSIX only\n");
#endif

    /* ---- symbols: every format, and the built-in tables ---- */
    {
        snprintf(path, sizeof path, "%s/t.map", tmp);
        write_text(path, "Symbols in Segment NOTHING\nR8                 Int    8   -1  1 0\n\n"
                         "Symbols in Segment CODE\nloop               Int    42D -1  1 0\n");
        check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1, "an AS -g MAP .map loads (code only)");
        check(o2debug_label_address(d, "loop") == 0x42D && o2debug_label_address(d, "R8") == -1, "its labels resolve");
        snprintf(path, sizeof path, "%s/t.lst", tmp);
        write_text(path, "  Symbol Table (* = unused):\n  --------------------------\n\n"
                         " RA :                            A - | *isrw :                     451 C |\n"
                         " up :                          43B C |\n");
        check(o2debug_load_symbols(d, path, msg, sizeof msg) == 2, "an AS listing's symbol table loads");
        check(o2debug_label_address(d, "isrw") == 0x451, "unused labels too");
        snprintf(path, sizeof path, "%s/t.sym", tmp);
        write_text(path, "; WLA-DX\n[labels]\n00:0430 bump\n");
        check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1 && o2debug_label_address(d, "bump") == 0x430,
              "a WLA-DX .sym loads");
        snprintf(path, sizeof path, "%s/t.txt", tmp);
        write_text(path, "0811 key1\n");
        check(o2debug_load_symbols(d, path, msg, sizeof msg) == 1, "a plain list loads");
        o2debug_address_label(d, 0x811, out, sizeof out);
        check(strcmp(out, "key1") == 0, "and the address finds its label");
        check(o2debug_load_symbols(d, "/nonexistent/file.map", msg, sizeof msg) == -1, "a missing file is refused");

        n = o2debug_disassemble(d, 0x42D, lines, 1, NULL);
        check(n == 1 && strcmp(lines[0].label, "loop") == 0, "loaded labels show in the disassembly");
        o2debug_set_label(d, 0x400, "myentry");
        n = o2debug_disassemble(d, 0, lines, 1, NULL);
        check(strcmp(lines[0].comment, "myentry") == 0, "a branch names its target (a user label beats the built-in)");
        o2debug_set_label(d, 0x400, NULL);
        n = o2debug_disassemble(d, 0, lines, 1, NULL);
        check(strcmp(lines[0].comment, "cart_start") == 0, "and removing it brings the built-in back");

        snprintf(path, sizeof path, "%s/game.bin", tmp);
        o2debug_set_cart_path(d, path);
        check(o2debug_load_symbols(d, NULL, msg, sizeof msg) == -1, "no symbol file beside the cartridge");
        snprintf(path, sizeof path, "%s/game.sym", tmp);
        write_text(path, "[labels]\n00:0440 beside\n");
        check(o2debug_load_symbols(d, NULL, msg, sizeof msg) == 1 && o2debug_label_address(d, "beside") == 0x440,
              "symbols beside the cartridge load with no path");
        o2debug_set_cart_path(d, "");
    }

    /* ---- the trace ring ---- */
    {
        o2debug_trace tr[16];
        o2debug_cpu a = cpu(d);
        o2debug_trace_clear(d);
        o2debug_trace_enable(d, 1);
        for (i = 0; i < 10; i++) {
            o2debug_step(d);
            wait_stopped(d, 1, 2000);
        }
        n = o2debug_trace_read(d, tr, 16);
        check(n == 10, "the trace holds one entry per boundary stepped to");
        c = cpu(d);
        check(n >= 3 && tr[0].pc == c.pc && tr[0].cycles == c.cycles && tr[0].cycles > tr[1].cycles && tr[1].cycles > tr[2].cycles,
              "newest first, cycles falling, the newest being the PC");
        check(n == 10 && tr[9].cycles > a.cycles && tr[9].cycles <= a.cycles + 4, "the oldest is the first step's result");
        {
            uint8_t b = 0;
            o2debug_read(d, O2_SPACE_PROG, tr[3].pc, &b, 1);
            check(tr[3].bytes[0] == b && tr[3].len == dasm48_length(b) && tr[3].vpos >= 0 && tr[3].vpos < 263 &&
                  tr[3].hpos >= 0 && tr[3].hpos < 455, "an entry: opcode, length and beam");
        }
        o2debug_trace_enable(d, 0);
        o2debug_trace_clear(d);
    }

    /* ---- Save ---- */
    {
        static const struct { const char *kind; long size; } kinds[] = {
            { "ram", 64 + 128 }, { "mailbox", 0xE0 }, { "vdc", 256 },
        };
        for (i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++) {
            char what[96];
            snprintf(path, sizeof path, "%s/save.%s", tmp, kinds[i].kind);
            snprintf(what, sizeof what, "save %s writes %ld bytes", kinds[i].kind, kinds[i].size);
            check(o2debug_save(d, kinds[i].kind, path, msg, sizeof msg) == 0 && file_size(path) == kinds[i].size, what);
        }
        snprintf(path, sizeof path, "%s/save.font", tmp);
        check(o2debug_save(d, "font", path, msg, sizeof msg) == -1 && strstr(msg, "character set"),
              "save font says there is no character set imported");
        snprintf(path, sizeof path, "%s/save.ppm", tmp);
        check(o2debug_save(d, "screen", path, msg, sizeof msg) == 0 && file_size(path) == 15 + 360 * 243 * 3,
              "save screen (a PPM of the frame)");
        snprintf(path, sizeof path, "%s/save.regs", tmp);
        check(o2debug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 300, "save regs");
        snprintf(path, sizeof path, "%s/save.dis", tmp);
        check(o2debug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 8000, "save dis (the BIOS and the cartridge)");
        check(o2debug_save(d, "nonsense", path, msg, sizeof msg) == -1 && strstr(msg, "Unknown"), "an unknown kind is refused");
        check(o2debug_save(d, "ram", "/nonexistent/dir/x", msg, sizeof msg) == -1 && strstr(msg, "Cannot"), "an unwritable path is refused");
    }

    /* ---- the prompt ---- */
    o2debug_cpu_set(d, O2_REG_A, 0x66);
    o2debug_command(d, "print a", out, sizeof out);
    check(strstr(out, "$66") && strstr(out, "102") && strstr(out, "%01100110"), "print evaluates a register");
    o2debug_cpu_set(d, O2_REG_A, saved.a);
    o2debug_command(d, "print [0] == $84 && [$402] != 0 && x[$E0] >= 0 && i[8] >= 0 && v[$A1] >= 0", out, sizeof out);
    check(strstr(out, "1  "), "[addr] reads program space; x[] i[] v[] the other spaces");
    o2debug_command(d, "print readkey + 2", out, sizeof out);
    check(strstr(out, "$B2"), "labels in expressions");
    o2debug_command(d, "print r0' + vpos * 0 + (frame > 0) + (cycles > 0) + (hpos >= 0)", out, sizeof out);
    check(strstr(out, "error") == NULL, "r0', vpos, hpos, frame and cycles are names");
    o2debug_command(d, "print nonesuch", out, sizeof out);
    check(strstr(out, "unknown name"), "an unknown name is an error");
    o2debug_command(d, "print 1 +", out, sizeof out);
    check(strstr(out, "error"), "so is a syntax error");
    o2debug_command(d, "print 3 << 2 | 1", out, sizeof out);
    check(strstr(out, "$0D"), "C precedence: 3 << 2 | 1 is 13");
    o2debug_command(d, "mem 0 5", out, sizeof out);
    check(strstr(out, "84 00") && strstr(out, "84 02") && strstr(out, "prog"), "mem dumps program space");
    o2debug_command(d, "mem iram 0 16", out, sizeof out);
    check(strstr(out, "iram space"), "mem iram");
    o2debug_command(d, "mem xdata $E0 4", out, sizeof out);
    check(strstr(out, "FN_W_REGSEL") && strstr(out, "FN_W_SEQ"), "mem xdata names the mailbox registers");
    o2debug_command(d, "mem vdc $A0 4", out, sizeof out);
    check(strstr(out, "CONTROL") && strstr(out, "STATUS"), "mem vdc describes the control registers");
    o2debug_command(d, "poke iram $3E $11 $22", out, sizeof out);
    {
        uint8_t v[2];
        o2debug_read(d, O2_SPACE_IRAM, 0x3E, v, 2);
        check(v[0] == 0x11 && v[1] == 0x22 && strstr(out, "2 bytes"), "poke iram");
        o2debug_write(d, O2_SPACE_IRAM, 0x3E, saved.iram[0x3E]);
        o2debug_write(d, O2_SPACE_IRAM, 0x3F, saved.iram[0x3F]);
    }
    o2debug_command(d, "poke $420 1", out, sizeof out);
    check(strstr(out, "read-only"), "poke prog is refused with a reason");
    o2debug_command(d, "poke vdc $81 $3C", out, sizeof out);
    {
        uint8_t v = 0;
        o2debug_read(d, O2_SPACE_VDC, 0x81, &v, 1);
        check(v == 0x3C, "poke vdc");
        o2debug_vdc_write(d, 0x81, 0);
    }
    o2debug_command(d, "label $0431 latch", out, sizeof out);
    check(o2debug_label_address(d, "latch") == 0x431, "label sets one");
    o2debug_command(d, "label $0431", out, sizeof out);
    check(o2debug_label_address(d, "latch") == -1, "and no name removes it");
    o2debug_command(d, "disasm $0B0 3", out, sizeof out);
    check(strstr(out, "readkey:") && strstr(out, "0B0") && strstr(out, "dis"), "disasm");
    o2debug_command(d, "regs", out, sizeof out);
    check(strstr(out, "PC=") && strstr(out, "bank 0") && strstr(out, "bank 1"), "regs");
    o2debug_command(d, "vdc", out, sizeof out);
    check(strstr(out, "8244") && strstr(out, "$A0") && strstr(out, "COLLISION"), "vdc");
    o2debug_command(d, "objects", out, sizeof out);
    check(strstr(out, "sprite 0") && strstr(out, "single 11") && strstr(out, "quad 3"), "objects");
    o2debug_command(d, "io", out, sizeof out);
    check(strstr(out, "stand-in") && strstr(out, "P1=") && strstr(out, "P2="), "io");
    o2debug_command(d, "keys", out, sizeof out);
    check(strstr(out, "stick 1") && strstr(out, "centred"), "keys");
    o2debug_command(d, "cart", out, sizeof out);
    check(strstr(out, "FujiNet") && strstr(out, "APPBANK"), "cart");
    o2debug_command(d, "mailbox", out, sizeof out);
    check(strstr(out, "ACKSEQ") && strstr(out, "F20"), "mailbox");
    o2debug_command(d, "trace on", out, sizeof out);
    o2debug_command(d, "step 3", out, sizeof out);
    wait_stopped(d, 1, 2000);
    o2debug_command(d, "trace 3", out, sizeof out);
    check(o2debug_trace_enabled(d) && strstr(out, "A=") && strstr(out, "SP="), "trace lists instructions");
    o2debug_command(d, "trace off", out, sizeof out);
    o2debug_command(d, "trace clear", out, sizeof out);
    check(!o2debug_trace_enabled(d), "trace off");
    snprintf(path, sizeof path, "break $%03X", 0xB0);
    o2debug_command(d, path, out, sizeof out);
    check(o2debug_breakpoint_check(d, 0xB0), "break sets a breakpoint");
    o2debug_command(d, "bpxw $A0-$A3 value == 0", out, sizeof out);
    o2debug_command(d, "bpiw 0-7", out, sizeof out);
    o2debug_command(d, "bpvw $A0-$A3 (v[$A1] & 8) == 0", out, sizeof out);
    o2debug_command(d, "bpr $600-$6ff", out, sizeof out);
    o2debug_command(d, "bpin p2", out, sizeof out);
    o2debug_command(d, "bpout bus", out, sizeof out);
    o2debug_command(d, "breaks", out, sizeof out);
    check(strstr(out, "xdata $0A0-$0A3 if value == 0") && strstr(out, "iram $000-$007") && strstr(out, "prog $600-$6FF") &&
          strstr(out, "P2") && strstr(out, "BUS") && strstr(out, "vdc $0A0-$0A3 if (v[$A1] & 8) == 0"),
          "breaks lists every class");
    show("%s", out);
    o2debug_command(d, "bpin px", out, sizeof out);
    check(strstr(out, "usage"), "a bad port is refused");
    o2debug_command(d, "bpxw $A0 ((", out, sizeof out);
    check(strstr(out, "error"), "a bad condition is refused");
    o2debug_command(d, "delete $0B0", out, sizeof out);
    check(!o2debug_breakpoint_check(d, 0xB0), "delete by address");
    n = o2debug_breakpoint_list(d, bps, 16);
    check(n == 6, "six breakpoints remain");
    id = bps[0].id;
    snprintf(path, sizeof path, "disable #%d", id);
    o2debug_command(d, path, out, sizeof out);
    o2debug_breakpoint_list(d, bps, 16);
    check(!bps[0].enabled, "disable by id");
    snprintf(path, sizeof path, "enable %d", id);
    o2debug_command(d, path, out, sizeof out);
    o2debug_breakpoint_list(d, bps, 16);
    check(bps[0].enabled, "enable by (plain) id");
    snprintf(path, sizeof path, "delete #%d", id);
    o2debug_command(d, path, out, sizeof out);
    check(o2debug_breakpoint_list(d, bps, 16) == n - 1, "delete by id");
    o2debug_command(d, "delete 999", out, sizeof out);
    check(strstr(out, "no breakpoint"), "an unknown id is said so");
    o2debug_command(d, "clear", out, sizeof out);
    check(o2debug_breakpoint_list(d, bps, 16) == 0, "clear removes them");
    o2debug_command(d, "set a $42", out, sizeof out);
    check(cpu(d).a == 0x42 && strstr(out, "A=42"), "set a register by name");
    o2debug_command(d, "set a ", out, sizeof out);
    o2debug_cpu_set(d, O2_REG_A, saved.a);
    o2debug_command(d, "set bogus 1", out, sizeof out);
    check(strstr(out, "usage"), "set with an unknown register");
    o2debug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") && strstr(out, "bpxw") && strstr(out, "bpin") && strstr(out, "save"), "help lists the commands");
    o2debug_command(d, "frobnicate", out, sizeof out);
    check(strstr(out, "unknown command"), "an unknown command");
    check(o2debug_completions(d, "ru", out, sizeof out) == 2 && strstr(out, "run") && strstr(out, "runto"), "Tab completion");
    check(o2debug_completions(d, "print r1", out, sizeof out) == 2 && strstr(out, "r1'") && strstr(out, "r1\n"), "registers complete");
    check(o2debug_completions(d, "print wait", out, sizeof out) == 2 && strstr(out, "waitvsync") && strstr(out, "waitforkey"),
          "labels complete");
    check(o2debug_completions(d, "mem x", out, sizeof out) == 1 && strstr(out, "xdata"), "spaces complete");
    check(o2debug_completions(d, "save r", out, sizeof out) == 2 && strstr(out, "ram") && strstr(out, "regs"), "save kinds complete");
    o2debug_command(d, "runto $0B0", out, sizeof out);
    check(wait_stopped(d, 1, 5000) && cpu(d).pc == 0x0B0, "the prompt's runto");
    o2debug_command(d, "over", out, sizeof out);
    check(wait_stopped(d, 1, 2000), "the prompt's over");
    o2debug_command(d, "out", out, sizeof out);
    check(wait_stopped(d, 1, 5000) && strcmp((why(d), reason), "step out") == 0, "the prompt's out (readkey's RET)");
    o2debug_command(d, "run", out, sizeof out);
    sleep_ms(50);
    check(!o2debug_is_stopped(d), "the prompt's run");
    o2debug_command(d, "stop", out, sizeof out);
    check(wait_stopped(d, 1, 3000), "the prompt's stop");

    /* ---- detach lets it run ---- */
    o2debug_detach(d);
    check(!o2debug_is_attached(d) && !o2debug_is_stopped(d), "detached");
    {
        const uint64_t f0 = o2_host_frame_count();
        sleep_ms(200);
        check(o2_host_frame_count() > f0 + 3, "the machine runs at full speed again");
    }
    o2debug_attach(d);
    check(wait_stopped(d, 1, 3000), "attaching again stops it");
    o2debug_detach(d);

    /* ---- a power cycle with the debugger attached: a Videopac G7000 (PAL) and a game cartridge ---- */
    o2debug_attach(d);
    check(wait_stopped(d, 1, 3000), "stopped");
    o2debug_breakpoint_toggle(d, 0x400);
    o2debug_power_down(d);
    o2_host_stop();
    check(o2debug_machine() == NULL, "the host is stopped");
    c = cpu(d);
    check(c.cycles == 0 && !o2debug_is_stopped(d), "no machine: a zero view again");
    {
        static uint8_t game[2048];
        build_game(game);
        ho.machine = O2_MACHINE_VIDEOPAC;
        ho.cart = game;
        ho.cart_size = sizeof game;
        ho.cart_type = O2_CART_STD;
        ho.cart_app = false;
        ho.boip_hostport = NULL;
        check(o2_host_start(&ho) == 0, "the host starts again with a game cartridge in the slot");
    }
    o2debug_power_up(d);
    check(wait_stopped(d, 1, 3000), "the new machine stops in the debugger at once");
    if (cpu(d).pc != 0 || cpu(d).instructions != 0) {      /* the race described above */
        printf("note: the machine ran before the hook went in\n");
        o2debug_cpu_set(d, O2_REG_PC, 0);
    }
    check(cpu(d).pc == 0 && cpu(d).machine == O2_MACHINE_VIDEOPAC, "on the reset vector of the Videopac G7000");
    check(o2debug_breakpoint_check(d, 0x400), "the breakpoint survived the power cycle");
    check(run_until_stop(d, 5000) && cpu(d).pc == 0x400 && strstr((why(d), reason), "breakpoint: exec $0400"),
          "and fires on the new machine");
    o2debug_breakpoint_clear(d);
    game_tests(d, tmp);

    o2debug_detach(d);
    sleep_ms(100);
    check(o2_host_frame_count() > 2, "and the new machine runs");

    o2debug_power_down(d);
    o2_host_stop();
    o2debug_free(d);
#ifndef _WIN32
    if (fake_up) {
        shutdown(fake.listen_fd, SHUT_RDWR);
        close(fake.listen_fd);
        if (fake.fd > 0) shutdown(fake.fd, SHUT_RDWR);
        pthread_join(fake_th, NULL);
    }
#endif
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

/* fujinet_cart.c -- the FujiNet Odyssey 2 cartridge. See fujinet_cart.h.
 *
 * Transposed from the firmware's MAME device (fujinet-firmware
 * pico/o2/emu/fujinet.cpp, BSD-3-Clause, copyright-holders Thomas
 * Cherryhomes) onto the staged cartridge sources. The names follow it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujinet_cart.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the cartridge firmware's own sources (core/o2/fuji-generated) */
#include "fuji_mailbox.h"
#include "fujibus.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "o2app.h"
#include "o2map.h"
#include "fujiconfigrom.h"      /* _configrom, FUJI_CONFIGROM_SIZE */

#include "fujitcp_host.h"

#define QUEUE_SIZE 1024         /* console register writes waiting for the worker */
#define MAX_POKES  4096

struct o2_fujinet {
    o2_cart_t base;             /* the slot interface; first */

    /* Two tables of legacy images (or, for an APPBANK image, just its mailbox
     * bytes in table [run][0]): tbl[run] is the one served, the other is where
     * a push lands. Each bank is the console's 4K program window. */
    uint8_t (*tbl)[O2MAP_BANKS][O2MAP_BANK_BYTES];   /* [2] */
    unsigned run;

    /* Protocol 2: the APPBANK image served through o2app.h, and the staged one */
    uint8_t *app;
    uint32_t app_len;
    uint8_t *app_next;
    uint32_t app_next_len;
    unsigned app_next_pages;
    o2app_t appst;
    bool app_live;

    uint8_t p1;                 /* P10, P11, P14 as the machine passes them */
    atomic_bool mailbox;        /* the running image owns the mailbox page */
    bool powered;

    /* the image served first (the baked CONFIG or the opened app) and the
     * most recent plan, for the debugger */
    uint32_t live_size, live_crc;
    unsigned nbanks, cur_pages;

    /* A staged image, built by whichever thread runs the transaction and taken
     * by the console's thread. loadlock covers it and the swap. */
    pthread_mutex_t loadlock;
    bool next_is_app;
    atomic_bool boot_armed;
    bool stage_mailbox;
    uint32_t staged_size, staged_crc;
    unsigned swaps;
    bool served_other;          /* anything but the baked CONFIG has run */

    /* The worker owns fujimail and the socket; the console's thread owns the
     * tables, the swap and the app state. The worker's pokes reach the tables
     * through pokes[], applied before the next fetch; the console's register
     * writes reach the worker through writes[]. */
    bool async;
    bool worker_running;
    pthread_t worker;
    pthread_mutex_t qlock;
    pthread_cond_t qcv;
    uint8_t wq_addr[QUEUE_SIZE], wq_data[QUEUE_SIZE];
    unsigned qhead, qtail;
    atomic_bool stop;
    pthread_mutex_t pklock;
    uint16_t pk_addr[MAX_POKES];
    uint8_t pk_data[MAX_POKES];
    unsigned npokes;
    atomic_bool poked;

    /* the link */
    char boip[96];
    bool have_boip, link_off;
    bool debug;
    unsigned pace_ms;
};

/* fujimail's port is C function pointers with no context argument; one slot,
 * one cart, as on the hardware. */
static o2_fujinet_t *s_cart = NULL;

/* set on the worker thread: its pokes queue for the console's thread */
static _Thread_local bool t_on_worker = false;

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void apply_poke(o2_fujinet_t *c, unsigned addr, uint8_t value);

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void poke(o2_fujinet_t *c, unsigned addr, uint8_t value)
{
    if (!t_on_worker) {
        apply_poke(c, addr, value);
        return;
    }
    pthread_mutex_lock(&c->pklock);
    if (c->npokes < MAX_POKES) {
        c->pk_addr[c->npokes] = (uint16_t)addr;
        c->pk_data[c->npokes] = value;
        c->npokes++;
    }
    atomic_store_explicit(&c->poked, true, memory_order_release);
    pthread_mutex_unlock(&c->pklock);
}

static void c_poke(unsigned addr, uint8_t value) { poke(s_cart, addr, value); }
static bool c_link_up(void) { return fujitcp_active(); }

/* Dial again if the link has dropped: MAME's device connects once and never
 * again; a FujiNet that restarts or a transaction that was cut short should
 * not leave the console without one. */
static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    o2_fujinet_t *c = s_cart;
    fb_status_t st;

    if (!c->link_off && !fujitcp_active())
        fujitcp_init(c->have_boip ? c->boip : NULL);
    st = fujitcp_transact(device, command, params, nparams, payload, payload_len,
                          timeout_ms, reply);
    if (st == FB_ENOLINK)
        fujitcp_close();        /* the next transaction reconnects */
    return st;
}

static void c_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t len)
{
    o2_fujinet_t *c = s_cart;

    if (c->pace_ms && device == FUJI_DEVICEID_DBC && command == CMD_FUJI_ACK)
        sleep_ms(c->pace_ms);
    fujitcp_send_bare(device, command, payload, len);
}

/* Try to connect for up to `ms`: the listener can be a moment behind the
 * console on a first start (the session waits for it, but not for ever). */
static void c_wait_link(uint32_t ms)
{
    o2_fujinet_t *c = s_cart;
    const uint64_t deadline = now_ms() + ms;

    while (!c->link_off && !fujitcp_active() && !atomic_load(&c->stop)) {
        if (fujitcp_init(c->have_boip ? c->boip : NULL) == 0)
            break;
        if (now_ms() >= deadline)
            break;
        sleep_ms(100);
    }
    poke(c, FN_R_STATUS, fujitcp_active() ? FN_R_STATUS_LINK : 0);
}

static void c_stream_end(int stream, const uint8_t *data, unsigned len, bool aborted);

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;

    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++) {
        uint8_t ch = t->rx[k];
        if (ch == 0)
            break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static const fujimail_port_t debug_port = {
    c_poke, c_link_up, c_transact, c_send_bare, c_stream_end,
    c_wait_link, NULL, c_on_txn, c_on_dbc,
};

static const fujimail_port_t quiet_port = {
    c_poke, c_link_up, c_transact, c_send_bare, c_stream_end,
    c_wait_link, NULL, NULL, NULL,
};

/*-------------------------------------------------
    the tables
-------------------------------------------------*/

/* The running image's table only: a staged game that does not claim the
 * mailbox has its own bytes at $F20, and one that does gets the page at the
 * swap. */
static void apply_poke(o2_fujinet_t *c, unsigned addr, uint8_t value)
{
    if (!atomic_load(&c->mailbox))
        return;
    for (unsigned b = 0; b < O2MAP_BANKS; b++)
        c->tbl[c->run][b][addr & 0xfff] = value;
}

static void drain_pokes(o2_fujinet_t *c)
{
    uint16_t addr[MAX_POKES];
    uint8_t data[MAX_POKES];
    unsigned n;

    if (!atomic_load_explicit(&c->poked, memory_order_acquire))
        return;
    pthread_mutex_lock(&c->pklock);
    n = c->npokes;
    memcpy(addr, c->pk_addr, n * sizeof addr[0]);
    memcpy(data, c->pk_data, n);
    c->npokes = 0;
    atomic_store_explicit(&c->poked, false, memory_order_relaxed);
    pthread_mutex_unlock(&c->pklock);
    for (unsigned i = 0; i < n; i++)
        apply_poke(c, addr[i], data[i]);
}

/* fuji_cart_swap(): the staged image becomes the running one. The cart gives a
 * claiming image the running status bytes by poking both tables; here they
 * are copied across at the swap, which the console cannot tell apart. */
static void take_staged(o2_fujinet_t *c)
{
    pthread_mutex_lock(&c->loadlock);
    if (!atomic_load(&c->boot_armed)) {
        pthread_mutex_unlock(&c->loadlock);
        return;
    }
    const unsigned stage = c->run ^ 1;
    if (c->stage_mailbox) {
        for (unsigned b = 0; b < O2MAP_BANKS; b++)
            memcpy(&c->tbl[stage][b][FN_R_ACKSEQ], &c->tbl[c->run][0][FN_R_ACKSEQ],
                   0x1000 - FN_R_ACKSEQ);
    }
    if (c->next_is_app) {
        uint8_t *old = c->app;
        c->app = c->app_next;
        c->app_len = c->app_next_len;
        c->app_next = NULL;
        c->app_next_len = 0;
        free(old);
        o2app_init(&c->appst, c->app, c->app_next_pages);
        c->app_live = true;
        c->cur_pages = c->app_next_pages;
    } else {
        c->app_live = false;
    }
    atomic_store(&c->mailbox, c->stage_mailbox);
    c->run = stage;
    c->swaps++;
    c->served_other = true;
    c->live_size = c->staged_size;
    c->live_crc = c->staged_crc;
    atomic_store(&c->boot_armed, false);
    fprintf(stderr, "fujinet: swapped in the staged image (%s); mailbox %s\n",
            c->next_is_app ? "APPBANK" : "legacy", c->stage_mailbox ? "kept" : "off");
    pthread_mutex_unlock(&c->loadlock);
}

/*-------------------------------------------------
    the slot interface
-------------------------------------------------*/

/* The bus loop, picopac_cart.c core1_main: bank = 3 - (P10 + 2*P11) for a
 * legacy image, o2app_read for an APPBANK one, and a staged image taking over
 * on the console's fetch of the cartridge reset vector. */
static uint8_t fj_read(o2_cart_t *base, uint16_t addr, int peek)
{
    o2_fujinet_t *c = (o2_fujinet_t *)base;

    if (c->async)
        drain_pokes(c);
    if (!peek && addr == 0x400 && atomic_load_explicit(&c->boot_armed, memory_order_acquire))
        take_staged(c);

    if (c->app_live)
        return o2app_read(&c->appst, addr, c->tbl[c->run][0], fujimail_rx, fujimail_rxn);
    return c->tbl[c->run][3 - ((c->p1 & 1) + (c->p1 & 2))][addr];
}

static uint8_t fj_io_read(o2_cart_t *base, uint8_t off, int peek)
{
    (void)base; (void)off; (void)peek;
    return 0xff;
}

/* Console MOVX writes, qualified the way the cartridge qualifies them: P14 low
 * is its chip select (the machine has already dropped writes in copy mode). */
static void fj_io_write(o2_cart_t *base, uint8_t off, uint8_t data)
{
    o2_fujinet_t *c = (o2_fujinet_t *)base;

    if ((c->p1 & 0x10) || off < FN_W_REGSEL || off > FN_W_SEQ || !atomic_load(&c->mailbox))
        return;
    if (c->app_live && o2app_write(&c->appst, off, data))
        return;
    if (!c->async) {
        fujimail_write(off, data);
        return;
    }
    pthread_mutex_lock(&c->qlock);
    unsigned next = (c->qhead + 1) % QUEUE_SIZE;
    if (next != c->qtail) {
        c->wq_addr[c->qhead] = off;
        c->wq_data[c->qhead] = data;
        c->qhead = next;
    }
    pthread_cond_signal(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
}

static void fj_write_p1(o2_cart_t *base, uint8_t data) { ((o2_fujinet_t *)base)->p1 = data; }
static void fj_write_p2(o2_cart_t *base, uint8_t data) { (void)base; (void)data; }
static uint8_t fj_bus_read(o2_cart_t *base) { (void)base; return 0xff; }
static void fj_bus_write(o2_cart_t *base, uint8_t data) { (void)base; (void)data; }
static int fj_t0(o2_cart_t *base) { (void)base; return 0; }

static const o2_cart_ops_t fujinet_ops = {
    fj_read, fj_io_read, fj_io_write, fj_write_p1, fj_write_p2,
    fj_bus_read, fj_bus_write, fj_t0,
};

/*-------------------------------------------------
    the worker
-------------------------------------------------*/

static void *worker_main(void *arg)
{
    o2_fujinet_t *c = arg;

    t_on_worker = true;
    for (;;) {
        uint8_t addr, data;

        pthread_mutex_lock(&c->qlock);
        while (c->qhead == c->qtail && !atomic_load(&c->stop))
            pthread_cond_wait(&c->qcv, &c->qlock);
        if (atomic_load(&c->stop)) {
            pthread_mutex_unlock(&c->qlock);
            break;
        }
        addr = c->wq_addr[c->qtail];
        data = c->wq_data[c->qtail];
        c->qtail = (c->qtail + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&c->qlock);

        /* writes queued before a swap are the old image's */
        if (atomic_load(&c->mailbox))
            fujimail_write(addr, data);
    }
    return NULL;
}

static void worker_start(o2_fujinet_t *c)
{
    atomic_store(&c->stop, false);
    c->qhead = c->qtail = 0;
    c->worker_running = false;
    if (!c->async)
        return;
    if (pthread_create(&c->worker, NULL, worker_main, c) == 0) {
        c->worker_running = true;
    } else {
        fprintf(stderr, "fujinet: no mailbox worker; servicing inline\n");
        c->async = false;
    }
}

/* A transaction in flight is cut short, so a power-off or exit never waits out
 * its timeout. */
static void worker_stop(o2_fujinet_t *c)
{
    if (!c->worker_running)
        return;
    pthread_mutex_lock(&c->qlock);
    atomic_store(&c->stop, true);
    pthread_cond_broadcast(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
    fujitcp_abort();
    pthread_join(c->worker, NULL);
    c->worker_running = false;
    c->qhead = c->qtail = 0;
    pthread_mutex_lock(&c->pklock);
    c->npokes = 0;
    atomic_store(&c->poked, false);
    pthread_mutex_unlock(&c->pklock);
}

/* fujinet.c's port_stream_end, with the swap left to fj_read(). */
static void c_stream_end(int stream, const uint8_t *data, unsigned len, bool aborted)
{
    o2_fujinet_t *c = s_cart;
    o2map_plan_t plan;

    if (aborted || stream != 0 || len == 0)
        return;
    if (o2map_plan(data, len, &plan) != O2MAP_OK) {
        poke(c, FN_R_BOOT_STATE, FN_BOOT_FAILED);
        poke(c, FN_R_BOOT_ERR, FN_BOOT_ERR_NOMAP);
        return;
    }
    pthread_mutex_lock(&c->loadlock);
    if (plan.appbank) {
        uint8_t *copy = malloc(len);
        if (!copy) {
            pthread_mutex_unlock(&c->loadlock);
            poke(c, FN_R_BOOT_STATE, FN_BOOT_FAILED);
            poke(c, FN_R_BOOT_ERR, FN_BOOT_ERR_TOOBIG);
            return;
        }
        memcpy(copy, data, len);
        free(c->app_next);
        c->app_next = copy;
        c->app_next_len = len;
        c->app_next_pages = plan.npages;
        c->next_is_app = true;
        fujimail_stage_live = true;
        c->stage_mailbox = true;
    } else {
        /* the table the console is not running */
        o2map_apply(data, &plan, c->tbl[c->run ^ 1]);
        c->next_is_app = false;
        c->stage_mailbox = plan.mailbox_ok;
        c->nbanks = plan.nbanks;
    }
    c->staged_size = len;
    c->staged_crc = o2_crc32(data, len);
    atomic_store_explicit(&c->boot_armed, true, memory_order_release);
    pthread_mutex_unlock(&c->loadlock);
    fprintf(stderr, "fujinet: staged %u bytes (%s, mailbox %s); takes over at the next reset\n",
            len, plan.appbank ? "APPBANK" : "legacy", c->stage_mailbox ? "claimed" : "not claimed");
}

/*-------------------------------------------------
    create / power
-------------------------------------------------*/

o2_fujinet_t *o2_fujinet_create(void)
{
    o2_fujinet_t *c;

    if (s_cart)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->tbl = malloc(2 * sizeof c->tbl[0]);
    if (!c->tbl) {
        free(c);
        return NULL;
    }
    memset(c->tbl, 0xff, 2 * sizeof c->tbl[0]);
    c->base.ops = &fujinet_ops;
    c->base.type = O2_CART_FUJINET;
    c->base.priv = c;
    c->p1 = 0x13;
    atomic_init(&c->mailbox, true);
    atomic_init(&c->boot_armed, false);
    atomic_init(&c->stop, false);
    atomic_init(&c->poked, false);
    pthread_mutex_init(&c->loadlock, NULL);
    pthread_mutex_init(&c->qlock, NULL);
    pthread_cond_init(&c->qcv, NULL);
    pthread_mutex_init(&c->pklock, NULL);
    s_cart = c;
    return c;
}

void o2_fujinet_destroy(o2_fujinet_t *c)
{
    if (!c)
        return;
    o2_fujinet_power_off(c);
    if (s_cart == c)
        s_cart = NULL;
    free(c->tbl);
    free(c->app);
    free(c->app_next);
    pthread_mutex_destroy(&c->loadlock);
    pthread_mutex_destroy(&c->qlock);
    pthread_cond_destroy(&c->qcv);
    pthread_mutex_destroy(&c->pklock);
    free(c);
}

o2_cart_t *o2_fujinet_cart(o2_fujinet_t *c)
{
    return c ? &c->base : NULL;
}

static const char *plan_error(int err)
{
    return o2map_strerror((o2map_err_t)err);
}

static void set_why(char *why, int whysz, const char *text)
{
    if (why && whysz > 0)
        snprintf(why, (size_t)whysz, "%s", text);
}

int o2_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                           char *why, int whysz)
{
    o2map_plan_t plan;
    o2map_err_t err;

    if (claims)
        *claims = false;
    if (!image || !len) {
        set_why(why, whysz, "the cartridge image is empty");
        return 0;
    }
    err = o2map_plan(image, len, &plan);
    if (err != O2MAP_OK) {
        set_why(why, whysz, plan_error(err));
        return 0;
    }
    if (claims)
        *claims = plan.appbank ? true : plan.mailbox_ok;
    return 1;
}

int o2_fujinet_power_on(o2_fujinet_t *c, const uint8_t *image, uint32_t len,
                        const char *boip, bool sync, char *why, int whysz)
{
    o2map_plan_t plan;
    const char *env;

    if (!c)
        return -1;
    o2_fujinet_power_off(c);

    c->debug = getenv("FUJINET_DEBUG") != NULL;
    env = getenv("FUJINET_PACE_MS");
    c->pace_ms = env ? (unsigned)atoi(env) : 0;
    memset(c->tbl, 0xff, 2 * sizeof c->tbl[0]);
    free(c->app);
    free(c->app_next);
    c->app = c->app_next = NULL;
    c->app_len = c->app_next_len = 0;
    c->app_live = false;
    c->run = 0;
    c->p1 = 0x13;
    atomic_store(&c->mailbox, true);
    atomic_store(&c->boot_armed, false);
    c->swaps = 0;
    c->served_other = false;
    c->staged_size = c->staged_crc = 0;
    c->nbanks = 0;
    c->cur_pages = 0;

    if (!image) {
        image = _configrom;
        len = FUJI_CONFIGROM_SIZE;
    } else {
        c->served_other = true;
    }
    c->live_size = len;
    c->live_crc = o2_crc32(image, len);

    /* The loaded image stands in for the baked-in CONFIG, so the mailbox is
     * live whatever the image claims, and it can push: an APPBANK image is
     * served from this device's own copy, not the stage buffer that
     * fujimail_stage_live guards. */
    if (o2map_plan(image, len, &plan) != O2MAP_OK) {
        set_why(why, whysz, "the image is not a cartridge this FujiNet cart can map");
        return -1;
    }
    if (plan.appbank) {
        c->app = malloc(len);
        if (!c->app) {
            set_why(why, whysz, "out of memory");
            return -1;
        }
        memcpy(c->app, image, len);
        c->app_len = len;
        o2app_init(&c->appst, c->app, plan.npages);
        c->app_live = true;
        c->cur_pages = plan.npages;
        fprintf(stderr, "fujinet: APPBANK image, %u pages, as the baked CONFIG\n", plan.npages);
    } else {
        o2map_apply(image, &plan, c->tbl[0]);
        c->nbanks = plan.nbanks;
        fprintf(stderr, "fujinet: %u-byte image as %u x %uK bank(s), as the baked CONFIG\n",
                (unsigned)len, plan.nbanks, plan.bank_bytes / 1024);
    }

    /* the link: "" = FujiNet switched off */
    c->have_boip = boip && boip[0];
    c->link_off = !c->have_boip;
    snprintf(c->boip, sizeof c->boip, "%s", c->have_boip ? boip : "");
    fujimail_stage_live = false;
    fujimail_init(c->debug ? &debug_port : &quiet_port);
    fujitcp_close();                    /* fujinet-pc's BoIP port takes one client */
    if (!c->link_off)
        fujitcp_init(c->boip);          /* a miss is retried by the transaction */
    fujimail_paint();
    poke(c, FN_R_STATUS, fujitcp_active() ? FN_R_STATUS_LINK : 0);

    c->async = !sync;
    {
        const char *async = getenv("FUJINET_ASYNC");
        if (async)
            c->async = atoi(async) != 0;
    }
    c->powered = true;
    worker_start(c);
    return 0;
}

void o2_fujinet_power_off(o2_fujinet_t *c)
{
    if (!c || !c->powered)
        return;
    worker_stop(c);
    fujitcp_close();
    c->powered = false;
}

void o2_fujinet_frame(o2_fujinet_t *c)
{
    if (c && c->async)
        drain_pokes(c);
}

/*-------------------------------------------------
    status
-------------------------------------------------*/

void o2_fujinet_status(o2_fujinet_t *c, o2_cart_status_t *out)
{
    const uint8_t *mb;

    memset(out, 0, sizeof *out);
    if (!c)
        return;
    out->powered = c->powered;
    out->mailbox = atomic_load(&c->mailbox);
    out->link_up = fujitcp_active();
    out->link_off = c->link_off;
    out->app_live = c->app_live;
    out->booted_game = c->served_other;
    out->armed = atomic_load(&c->boot_armed);
    out->staged_app = c->next_is_app;
    out->staged_mailbox = c->stage_mailbox;
    out->live_size = c->live_size;
    out->live_crc = c->live_crc;
    out->staged_size = c->staged_size;
    out->staged_crc = c->staged_crc;
    out->swaps = c->swaps;
    out->p1 = c->p1;
    if (c->app_live) {
        out->nbanks = c->cur_pages;
        out->bank = c->appst.pagenum;
    } else {
        out->nbanks = c->nbanks;
        out->bank = 3 - ((c->p1 & 1) + (c->p1 & 2));
    }
    mb = c->tbl[c->run][0];
    out->ackseq = mb[FN_R_ACKSEQ];
    out->status = mb[FN_R_STATUS];
    out->err = mb[FN_R_ERR];
    out->reply_cmd = mb[FN_R_REPLY_CMD];
    out->rxlen = (uint16_t)(mb[FN_R_RXLEN_LO] | mb[FN_R_RXLEN_HI] << 8);
    out->boot_state = mb[FN_R_BOOT_STATE];
    out->boot_pct = mb[FN_R_BOOT_PCT];
    out->boot_err = mb[FN_R_BOOT_ERR];
    pthread_mutex_lock(&c->qlock);
    out->queue = (c->qhead + QUEUE_SIZE - c->qtail) % QUEUE_SIZE;
    pthread_mutex_unlock(&c->qlock);
}

void o2_fujinet_mailbox(o2_fujinet_t *c, uint8_t out[0xE0])
{
    if (!c) {
        memset(out, 0xff, 0xE0);
        return;
    }
    memcpy(out, &c->tbl[c->run][0][FN_R_PAGE + 0x20], 0xE0);
}

uint32_t o2_fujinet_copy_image(o2_fujinet_t *c, uint8_t *dst, uint32_t max)
{
    uint32_t n = 0;

    if (!c || !dst)
        return 0;
    if (c->app_live && c->app) {
        n = c->app_len < max ? c->app_len : max;
        memcpy(dst, c->app, n);
    } else {
        /* the running legacy table's first bank, from $400 */
        n = O2MAP_BANK_BYTES - O2MAP_CART_BASE;
        if (n > max)
            n = max;
        memcpy(dst, &c->tbl[c->run][0][O2MAP_CART_BASE], n);
    }
    return n;
}

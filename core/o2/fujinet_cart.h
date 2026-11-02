/* fujinet_cart.h -- the FujiNet Odyssey 2 cartridge, the one cart with a
 * network behind it.
 *
 * A C transposition of the firmware's own MAME device model (fujinet-firmware
 * pico/o2/emu/fujinet.cpp, BSD-3-Clause, Thomas Cherryhomes): an RP2354B on
 * the cartridge connector that serves the console's program fetches from $400
 * and decodes the mailbox registers the console writes at $E0-$E3. The image
 * planner (o2map.c), the APPBANK serving (o2app.h), the mailbox protocol
 * (fujimail.c) and the wire codec (fujibus.c) are the cartridge firmware's own
 * sources, staged verbatim into fuji-generated/ by cmake/StageFujiProto.cmake;
 * this device is only the port.
 *
 * It sits in the cartridge slot through the o2_cart_t interface (carts.h). What
 * it serves at power-on is the baked CONFIG, or an opened FujiNet app, with the
 * mailbox live; the image the client mounts is staged beside it and takes over,
 * as on the cart, at the console's next fetch of $400 -- which is what the
 * BIOS does after the client's `jmp $000`.
 *
 * Like the cartridge, the mailbox service runs on its own thread (the
 * RP2354B's core0) while the bus is served on the emulation thread (core1), so
 * a slow network transaction never stalls the picture; a synchronous mode
 * (FUJINET_ASYNC=0, or the sync flag) runs it inline instead, which is
 * deterministic for tests.
 *
 * Unlike MAME's device it reconnects: a link that drops (FujiNet restarted, a
 * transaction cut short) is dialled again before the next transaction.
 *
 * Single instance per process: fujimail's port interface is C function
 * pointers with no context argument (the constraint is real hardware's too --
 * one cart slot, one cart).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_FUJINET_CART_H
#define O2_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

#include "carts.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct o2_fujinet o2_fujinet_t;

/* Create the (single) cart; NULL if one already exists. */
o2_fujinet_t *o2_fujinet_create(void);
void o2_fujinet_destroy(o2_fujinet_t *c);

/* The slot interface (valid until destroy). */
o2_cart_t *o2_fujinet_cart(o2_fujinet_t *c);

/* Power on, as MAME's device_reset with the image it was given:
 *   - `image` NULL: the baked CONFIG client (fujiconfigrom.h), mailbox live;
 *   - an image: served in its place, mailbox live whatever it claims, as the
 *     MAME device serves its -cart image (a FujiNet app opened from disk).
 * `boip` is fujinet-pc's BoIP listener ("host:port"), or "" for none at all
 * (FujiNet switched off: nothing is dialled, every transaction fails at once).
 * Returns 0, or -1 with the reason (an image the cartridge cannot plan) in
 * `why`. */
int o2_fujinet_power_on(o2_fujinet_t *c, const uint8_t *image, uint32_t len,
                        const char *boip, bool sync, char *why, int whysz);
/* Power off: stop the mailbox worker, close the link. */
void o2_fujinet_power_off(o2_fujinet_t *c);

/* Bring what the mailbox worker published into the tables (also done on every
 * fetch); the host calls it once a frame. */
void o2_fujinet_frame(o2_fujinet_t *c);

/* ---- status, for the session and the debugger ---- */
typedef struct {
    bool powered;
    bool mailbox;             /* the running image owns the mailbox page */
    bool link_up;             /* the BoIP socket is connected */
    bool link_off;            /* FujiNet is switched off: nothing is dialled */
    bool app_live;            /* the running image is an APPBANK one */
    bool booted_game;         /* anything other than the baked CONFIG has been served */
    bool armed;               /* an image is staged and takes over at the next $400 fetch */
    bool staged_app;          /* ... and it is an APPBANK one */
    bool staged_mailbox;      /* ... and it claims the mailbox */
    uint32_t live_size, live_crc;
    uint32_t staged_size, staged_crc;
    unsigned swaps;           /* images swapped in since power-on */
    unsigned nbanks;          /* legacy: the banks served; APPBANK: pages */
    unsigned bank;            /* legacy: the one P10/P11 shows; APPBANK: CODEBANK */
    uint8_t p1;               /* P1 as the cart sees it (P10, P11, P14) */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, err, reply_cmd;
    uint16_t rxlen;
    unsigned queue;           /* register writes waiting for the worker */
} o2_cart_status_t;

void o2_fujinet_status(o2_fujinet_t *c, o2_cart_status_t *out);

/* The mailbox page ($F20-$FFF) as the console would read it, and the running
 * image's bytes at a console address ($400-$FFF) with no side effects (the
 * debugger's Cart tab and disassembly). */
void o2_fujinet_mailbox(o2_fujinet_t *c, uint8_t out[0xE0]);

/* The image served at power-on or by the last swap, as the debugger's Save
 * wants it. Returns bytes copied. */
uint32_t o2_fujinet_copy_image(o2_fujinet_t *c, uint8_t *dst, uint32_t max);

/* 1 if `image` (len bytes) can run on the cartridge, with *claims set when it
 * is a FujiNet app (claims the mailbox); else 0 and why. */
int o2_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                           char *why, int whysz);

#ifdef __cplusplus
}
#endif

#endif /* O2_FUJINET_CART_H */

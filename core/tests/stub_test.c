/*
 * stub_test -- the no-BIOS stand-in's two routines, the way the FujiNet apps
 * call them: readkey and getjoystick. A cartridge of the tests' own
 * (roms/inputprobe.a48, in test_rom.h) calls both every pass and leaves what
 * they return in the console's external RAM; the test presses every key of the
 * matrix and every stick direction and reads the answer. Also that the RESET
 * key stops the 8048 while it is held and lets it run again from $000 when it
 * is let go, with the RAM kept. No ROM, no FujiNet.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "o2_internal.h"
#include "test_rom.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void frames(o2_machine_t *m, int n)
{
    while (n-- > 0)
        o2_machine_run_frame(m);
}

int main(void)
{
    static o2_machine_t m;
    o2_machine_opts_t mo;
    o2_cart_t cart;
    const char *err = NULL;
    int key, ok_keys = 0, total_keys = 0;
    unsigned counter;

    if (o2_cart_init(&cart, O2_CART_STD, test_rom_inputprobe, sizeof test_rom_inputprobe, &err) != 0) {
        printf("FAIL: %s\n", err);
        return 1;
    }
    memset(&mo, 0, sizeof mo);
    mo.machine = O2_MACHINE_ODYSSEY2;
    mo.cart = &cart;
    o2_machine_init(&m, &mo);
    check(!m.have_bios, "the stand-in is the BIOS");

    frames(&m, 5);
    check(m.ram[0] == 0xff, "with nothing pressed readkey says $FF");
    check(m.ram[1] == 0 && m.ram[2] == 0 && m.ram[3] == 0, "and getjoystick says centred, no button");
    check(m.ram[4] != 0, "the probe is running");

    /* every key of the Odyssey 2's matrix, one at a time */
    for (key = 0; key < 48; key++) {
        if (key == 10 || key == 11)
            continue;                     /* not wired on the Odyssey 2 */
        total_keys++;
        m.keys_live = 1ull << key;
        frames(&m, 2);
        if (m.ram[0] == (uint8_t)key)
            ok_keys++;
        else
            printf("FAIL: key %d read back as %02X\n", key, m.ram[0]);
        m.keys_live = 0;
        frames(&m, 2);
    }
    check(ok_keys == total_keys, "readkey reports every one of the 46 wired keys");

    /* the sticks: bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 BUTTON */
    m.joy_live[0] = 0x01; frames(&m, 2);
    check(m.ram[1] == 0xff && m.ram[2] == 0, "stick up: R3 = $FF");
    m.joy_live[0] = 0x04; frames(&m, 2);
    check(m.ram[1] == 0x01 && m.ram[2] == 0, "stick down: R3 = 1");
    m.joy_live[0] = 0x08; frames(&m, 2);
    check(m.ram[2] == 0xff && m.ram[1] == 0, "stick left: R2 = $FF");
    m.joy_live[0] = 0x02; frames(&m, 2);
    check(m.ram[2] == 0x01 && m.ram[1] == 0, "stick right: R2 = 1");
    m.joy_live[0] = 0x10; frames(&m, 2);
    check(m.ram[3] == 1 && m.ram[1] == 0 && m.ram[2] == 0, "the button: F0");
    m.joy_live[0] = 0x01 | 0x08 | 0x10; frames(&m, 2);
    check(m.ram[1] == 0xff && m.ram[2] == 0xff && m.ram[3] == 1, "up-left with the button");
    m.joy_live[0] = 0; frames(&m, 2);
    check(m.ram[1] == 0 && m.ram[2] == 0 && m.ram[3] == 0, "released: centred again");
    m.joy_live[1] = 0x1f; frames(&m, 2);
    check(m.ram[1] == 0 && m.ram[2] == 0 && m.ram[3] == 0, "the second stick is not the first");
    m.joy_live[1] = 0;

    /* RESET resets the 8048 only: it holds it while down, the RAM survives */
    counter = m.ram[4];
    m.reset_live = true;
    frames(&m, 3);
    {
        unsigned held = m.ram[4];

        frames(&m, 3);
        check(m.ram[4] == held, "RESET held: the 8048 does not run");
    }
    m.reset_live = false;
    frames(&m, 3);
    check(m.ram[4] != counter && m.ram[4] != 0, "RESET let go: it runs again from $000 with its RAM");
    check(m.vdc.frames > 50, "and the VDC never stopped");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

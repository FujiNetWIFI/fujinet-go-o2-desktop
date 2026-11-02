/*
 * no_sdl_link_test -- the core links and runs on its own: no SDL, no
 * session, no frontend. Links o2_core alone (a session symbol would fail
 * the link), runs the staged wire codec's self test, plans the baked CONFIG
 * client with the staged planner, and boots a machine for a frame on the
 * no-BIOS stand-in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "o2_internal.h"
#include "fujibus.h"
#include "o2map.h"
#include "fujiconfigrom.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    static o2_machine_t m;
    o2map_plan_t plan;
    o2_machine_opts_t mo;
    char why[160];
    o2_fujinet_t *fj;

    check(fujibus_selftest(), "the staged fujibus codec passes its self test");
    check(o2map_plan(_configrom, FUJI_CONFIGROM_SIZE, &plan) == O2MAP_OK,
          "the staged o2map plans the baked CONFIG client");
    check(plan.appbank && plan.npages == 12, "CONFIG is a 12-page APPBANK image");
    {
        bool claims = false;

        check(o2_fujinet_check_image(_configrom, FUJI_CONFIGROM_SIZE, &claims, why, sizeof why) &&
              claims, "and a FujiNet app (it claims the mailbox)");
    }

    fj = o2_fujinet_create();
    check(fj != NULL, "the cartridge device creates");
    check(o2_fujinet_power_on(fj, NULL, 0, "", true, why, sizeof why) == 0,
          "it powers on with CONFIG");
    memset(&mo, 0, sizeof mo);
    mo.machine = O2_MACHINE_ODYSSEY2;
    mo.cart = o2_fujinet_cart(fj);
    mo.fujinet = fj;
    o2_machine_init(&m, &mo);
    o2_machine_run_frame(&m);
    check(m.frame_count == 1, "a BIOS-less Odyssey 2 runs a frame");
    check(m.vdc.frames == 1 && m.cpu.instructions > 1000, "and the 8048 ran");
    o2_fujinet_destroy(fj);

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

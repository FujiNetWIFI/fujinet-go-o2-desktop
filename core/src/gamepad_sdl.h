/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef O2_GAMEPAD_SDL_H
#define O2_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define O2_DIR_UP    1
#define O2_DIR_DOWN  2
#define O2_DIR_LEFT  4
#define O2_DIR_RIGHT 8

/* The stick directions a set of direction bits holds, as a mask over
 * o2_action (bit n = action n: UP RIGHT DOWN LEFT). Opposite directions
 * cancel, so a stick pushed both ways holds neither. Pure. */
unsigned o2_acts_from_dir(int dir);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `exit` (both 0..1 of full deflection),
 * so a thumb resting near the threshold cannot flutter. `prev` is the
 * previous result. Pure. */
int o2_dir_from_stick(float x, float y, int prev, float enter, float exit);

/* Which pad drives `port`: an explicit assignment wins; among the rest,
 * connection order (first pad to port 0, second to port 1). `assign[i]` is
 * pad i's assignment (-1 = automatic). Returns the pad index or -1. Pure. */
int o2_pad_for_port(const int *assign, int npads, int port);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int o2_pad_button_from_sdl(uint8_t button);
int o2_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* O2_GAMEPAD_SDL_H */

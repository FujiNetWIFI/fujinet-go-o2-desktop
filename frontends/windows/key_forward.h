/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift or the layout is doing --
 * and the rule every window of the app shares for handing a key to the
 * console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "o2session.h"

static inline uint32_t o2_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned scancode = (unsigned)((lp >> 16) & 0xff);
    const int extended = (lp >> 24) & 1;
    uint32_t ks = o2session_keysym_from_win_scancode(scancode, extended);
    if (ks) return ks;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vk) {
    case VK_UP: return O2_KEYSYM_UP;
    case VK_DOWN: return O2_KEYSYM_DOWN;
    case VK_LEFT: return O2_KEYSYM_LEFT;
    case VK_RIGHT: return O2_KEYSYM_RIGHT;
    case VK_ESCAPE: return O2_KEYSYM_ESCAPE;
    case VK_RETURN: return O2_KEYSYM_RETURN;
    case VK_SPACE: return O2_KEYSYM_SPACE;
    case VK_TAB: return O2_KEYSYM_TAB;
    case VK_BACK: return O2_KEYSYM_BACKSPACE;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return O2_KEYSYM_F1 + (uint32_t)(vk - VK_F1);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    /* The numeric keypad keeps its own keysyms: stick 2 lives there. */
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return O2_KEYSYM_KP_0 + (uint32_t)(vk - VK_NUMPAD0);
    return 0;
}

/* The app's own keys, never the console's: F9, F11, F12, and Ctrl+O (open),
 * Ctrl+R (reset to CONFIG) and Ctrl+, (Preferences). Ctrl itself is the
 * console's: Right Ctrl is stick 1's fire button. */
static inline int o2_app_key(WPARAM vk)
{
    if (vk == VK_F9 || vk == VK_F11 || vk == VK_F12) return 1;
    return (GetKeyState(VK_CONTROL) & 0x8000) && (vk == 'O' || vk == 'R' || vk == VK_OEM_COMMA);
}

/* A key press or release for the console from any of the app's windows: 1
 * when the console took it (it is bound). Windows reports Alt, and every key
 * pressed while Alt is down, as a system key; a bound one is the console's
 * and the rest (Alt+F4, the menu mnemonics, Alt+Esc) stay Windows'. An
 * auto-repeat of a bound key is swallowed without being pressed again. The
 * session's own actions (Escape) are the caller's. */
static inline int o2_forward_key(o2session *s, UINT msg, WPARAM vk, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const uint32_t ks = o2_keysym_from_msg(vk, lp);
    if (!ks) return 0;
    if (!down)
        return o2session_key(s, ks, 0) != 0;     /* always: a key let go after a shortcut must not stick */
    if (o2_app_key(vk) || (msg == WM_SYSKEYDOWN && vk == VK_F4)) return 0;
    if (lp & (1 << 30)) return o2session_target_for_key(s, ks) >= 0;
    return o2session_key(s, ks, 1) != 0;
}

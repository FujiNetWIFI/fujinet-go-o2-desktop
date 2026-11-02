/*
 * keyboard -- the on-screen membrane keyboard: the key caps as they are
 * printed on the console, and the label every matrix position has.
 *
 * The matrix positions are MAME's (src/mame/philips/odyssey2.cpp,
 * INPUT_PORTS_START(o2) and (vpp)): position = row * 8 + column, KEY.0 to
 * KEY.7. The Odyssey 2 and the Videopac G7000 have rows 0-5 (48 keys; the two
 * bits of row 1 that are IPT_UNUSED are positions 10 and 11); the Videopac+
 * G7400 adds row 6 (Ret, Lock, : *, | @, ] [, ¨ ^, , /, < >) and row 7
 * (Shift, Break, Cntl, Esc) and prints a second legend on most keys.
 *
 *   row 0   0 1 2 3 4 5 6 7           row 3   Q S D F G H J K
 *   row 1   8 9 - - SPACE ? L P       row 4   A Z X C V B M .
 *   row 2   + W E R T U I O           row 5   - x / = Y N CLEAR ENTER
 *
 * Y and YES are two printed caps wired to the same matrix position (44), and
 * so are N and NO (45): both caps press the one switch, as they do on the
 * console.
 *
 * Geometry: the Odyssey 2's from the console photograph -- the numeral row
 * (the 0 first), the function row (+ - x / = in red, then YES NO CLEAR ENTER
 * and the red RESET at the right), QWERTYUIOP, ASDFGHJKL indented half a key,
 * ZXCVBNM.? indented one key, the SPACE bar centred below. The Videopac+
 * G7400's geometry is APPROXIMATE: there is no photograph of that console to
 * trace, so its caps follow a plausible typewriter arrangement of MAME's
 * labels (a main block, and the arrows, YES/NO, CLEAR and ENTER in a block
 * to the right). Every key is reachable; where each sits is a guess.
 *
 * Units are key caps: a letter key is 1.0 x 1.0, with the small gap between
 * caps (0.1) already in the coordinates, so the pitch is 1.1.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>

#include "o2session.h"

#define GAP   0.1f
#define PITCH (1.0f + GAP)

/* A cap `wd` pitches wide at column `col` of row `row` (both in pitches). */
#define KEY(lbl, tgt, col, row, wd, kind) \
    { lbl, tgt, (col) * PITCH, (row) * PITCH, (wd) * PITCH - GAP, 1.0f, kind }
#define NRM O2_KEYCAP_NORMAL
#define FUN O2_KEYCAP_FUNCTION
#define INP O2_KEYCAP_INPUT

#define TIMES "\xc3\x97"      /* U+00D7 multiplication sign */
#define DIVIDE "\xc3\xb7"     /* U+00F7 division sign */

/* ---- the Odyssey 2 and the Videopac G7000 ----------------------------------- */

static const o2_keycap o2_caps[] = {
    /* the numerals: 0 first */
    KEY("0", 0, 0, 0, 1, NRM), KEY("1", 1, 1, 0, 1, NRM), KEY("2", 2, 2, 0, 1, NRM),
    KEY("3", 3, 3, 0, 1, NRM), KEY("4", 4, 4, 0, 1, NRM), KEY("5", 5, 5, 0, 1, NRM),
    KEY("6", 6, 6, 0, 1, NRM), KEY("7", 7, 7, 0, 1, NRM), KEY("8", 8, 8, 0, 1, NRM),
    KEY("9", 9, 9, 0, 1, NRM),

    /* function (red legends), input, and RESET */
    KEY("+", 16, 0, 1, 1, FUN), KEY("-", 40, 1, 1, 1, FUN), KEY(TIMES, 41, 2, 1, 1, FUN),
    KEY(DIVIDE, 42, 3, 1, 1, FUN), KEY("=", 43, 4, 1, 1, FUN),
    KEY("YES", 44, 5, 1, 1, INP), KEY("NO", 45, 6, 1, 1, INP),
    KEY("CLEAR", 46, 7, 1, 1, INP), KEY("ENTER", 47, 8, 1, 1, INP),
    KEY("RESET", -1, 9, 1, 1, O2_KEYCAP_RESET),

    /* alpha */
    KEY("Q", 24, 0, 2, 1, NRM), KEY("W", 17, 1, 2, 1, NRM), KEY("E", 18, 2, 2, 1, NRM),
    KEY("R", 19, 3, 2, 1, NRM), KEY("T", 20, 4, 2, 1, NRM), KEY("Y", 44, 5, 2, 1, NRM),
    KEY("U", 21, 6, 2, 1, NRM), KEY("I", 22, 7, 2, 1, NRM), KEY("O", 23, 8, 2, 1, NRM),
    KEY("P", 15, 9, 2, 1, NRM),

    KEY("A", 32, 0.5f, 3, 1, NRM), KEY("S", 25, 1.5f, 3, 1, NRM), KEY("D", 26, 2.5f, 3, 1, NRM),
    KEY("F", 27, 3.5f, 3, 1, NRM), KEY("G", 28, 4.5f, 3, 1, NRM), KEY("H", 29, 5.5f, 3, 1, NRM),
    KEY("J", 30, 6.5f, 3, 1, NRM), KEY("K", 31, 7.5f, 3, 1, NRM), KEY("L", 14, 8.5f, 3, 1, NRM),

    KEY("Z", 33, 1, 4, 1, NRM), KEY("X", 34, 2, 4, 1, NRM), KEY("C", 35, 3, 4, 1, NRM),
    KEY("V", 36, 4, 4, 1, NRM), KEY("B", 37, 5, 4, 1, NRM), KEY("N", 45, 6, 4, 1, NRM),
    KEY("M", 38, 7, 4, 1, NRM), KEY(".", 39, 8, 4, 1, NRM), KEY("?", 13, 9, 4, 1, NRM),

    /* the SPACE bar, centred under the letters */
    KEY("SPACE", 12, 2, 5, 6, O2_KEYCAP_SPACE),
};

#define O2_WIDTH  (9 * PITCH + 1.0f)
#define O2_HEIGHT (5 * PITCH + 1.0f)

/* ---- the Videopac+ G7400 (approximate geometry: see the header) ----------- */

static const o2_keycap g7400_caps[] = {
    /* the main block */
    KEY("Esc", 63, 0, 0, 1, NRM), KEY("1 !", 1, 1, 0, 1, NRM),
    KEY("2 \"", 2, 2, 0, 1, NRM), KEY("3 \xc2\xa3", 3, 3, 0, 1, NRM),
    KEY("4 $", 4, 4, 0, 1, NRM), KEY("5 %", 5, 5, 0, 1, NRM),
    KEY("6 &", 6, 6, 0, 1, NRM), KEY("7 '", 7, 7, 0, 1, NRM),
    KEY("8 (", 8, 8, 0, 1, NRM), KEY("9 )", 9, 9, 0, 1, NRM),
    KEY("0 #", 0, 10, 0, 1, NRM),

    KEY("Q", 24, 0.5f, 1, 1, NRM), KEY("W", 17, 1.5f, 1, 1, NRM), KEY("E", 18, 2.5f, 1, 1, NRM),
    KEY("R", 19, 3.5f, 1, 1, NRM), KEY("T", 20, 4.5f, 1, 1, NRM), KEY("Y", 44, 5.5f, 1, 1, NRM),
    KEY("U", 21, 6.5f, 1, 1, NRM), KEY("I", 22, 7.5f, 1, 1, NRM), KEY("O", 23, 8.5f, 1, 1, NRM),
    KEY("P", 15, 9.5f, 1, 1, NRM), KEY("] [", 52, 10.5f, 1, 1, NRM),

    KEY("Lock", 49, 0.75f, 2, 1, NRM), KEY("A", 32, 1.75f, 2, 1, NRM), KEY("S", 25, 2.75f, 2, 1, NRM),
    KEY("D", 26, 3.75f, 2, 1, NRM), KEY("F", 27, 4.75f, 2, 1, NRM), KEY("G", 28, 5.75f, 2, 1, NRM),
    KEY("H", 29, 6.75f, 2, 1, NRM), KEY("J", 30, 7.75f, 2, 1, NRM), KEY("K", 31, 8.75f, 2, 1, NRM),
    KEY("L", 14, 9.75f, 2, 1, NRM), KEY(": *", 50, 10.75f, 2, 1, NRM),

    KEY("Shift", 56, 1.25f, 3, 1, NRM), KEY("Z", 33, 2.25f, 3, 1, NRM), KEY("X", 34, 3.25f, 3, 1, NRM),
    KEY("C", 35, 4.25f, 3, 1, NRM), KEY("V", 36, 5.25f, 3, 1, NRM), KEY("B", 37, 6.25f, 3, 1, NRM),
    KEY("N", 45, 7.25f, 3, 1, NRM), KEY("M", 38, 8.25f, 3, 1, NRM), KEY(", /", 54, 9.25f, 3, 1, NRM),
    KEY(".", 39, 10.25f, 3, 1, NRM), KEY("?", 13, 11.25f, 3, 1, NRM),

    KEY("Cntl", 58, 0, 4, 1, NRM), KEY("Break", 57, 1, 4, 1, NRM), KEY("| @", 51, 2, 4, 1, NRM),
    KEY("\xc2\xa8 ^", 53, 3, 4, 1, NRM), KEY("SPACE", 12, 4, 4, 5, O2_KEYCAP_SPACE),
    KEY("< >", 55, 9, 4, 1, NRM),

    /* the arrows and operators, YES/NO, CLEAR and ENTER, and RESET */
    KEY("+ \xe2\x86\x91", 16, 13, 0, 1, FUN), KEY("- \xe2\x86\x93", 40, 14, 0, 1, FUN),
    KEY("RESET", -1, 15, 0, 1, O2_KEYCAP_RESET),
    KEY(TIMES " \xe2\x86\x96", 41, 13, 1, 1, FUN), KEY(DIVIDE " \xe2\x86\x90", 42, 14, 1, 1, FUN),
    KEY("= \xe2\x86\x92", 43, 15, 1, 1, FUN),
    KEY("YES", 44, 13, 2, 1, INP), KEY("NO", 45, 14, 2, 1, INP), KEY("Ret", 48, 15, 2, 1, INP),
    KEY("CLEAR ;", 46, 13, 3, 1, INP), KEY("ENTER _", 47, 14, 3, 2, INP),
};

#define G7400_WIDTH  (15 * PITCH + 1.0f)
#define G7400_HEIGHT (4 * PITCH + 1.0f)

#define COUNT(a) ((int)(sizeof (a) / sizeof (a)[0]))

const o2_keycap *o2session_keyboard_layout(int machine, int *n, float *width, float *height)
{
    if (machine == O2_MACHINE_VIDEOPAC_PLUS) {
        if (n) *n = COUNT(g7400_caps);
        if (width) *width = G7400_WIDTH;
        if (height) *height = G7400_HEIGHT;
        return g7400_caps;
    }
    if (n) *n = COUNT(o2_caps);
    if (width) *width = O2_WIDTH;
    if (height) *height = O2_HEIGHT;
    return o2_caps;
}

/* ---- the label of each matrix position ------------------------------------- */

/* The Odyssey 2 / G7000: one legend per key. Y and N share their position with
 * YES and NO. */
static const char *const o2_labels[48] = {
    "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", NULL, NULL, "SPACE", "?", "L", "P",
    "+", "W", "E", "R", "T", "U", "I", "O",
    "Q", "S", "D", "F", "G", "H", "J", "K",
    "A", "Z", "X", "C", "V", "B", "M", ".",
    "-", TIMES, DIVIDE, "=", "Y / YES", "N / NO", "CLEAR", "ENTER",
};

/* The Videopac+ G7400: MAME's own port names (the vpp block). */
static const char *const g7400_labels[64] = {
    "0 #", "1 !", "2 \"", "3 \xc2\xa3", "4 $", "5 %", "6 &", "7 '",
    "8 (", "9 )", NULL, NULL, "SPACE", "?", "L", "P",
    "+ \xe2\x86\x91", "W", "E", "R", "T", "U", "I", "O",
    "Q", "S", "D", "F", "G", "H", "J", "K",
    "A", "Z", "X", "C", "V", "B", "M", ".",
    "- \xe2\x86\x93", TIMES " \xe2\x86\x96", DIVIDE " \xe2\x86\x90", "= \xe2\x86\x92",
    "Y / YES", "N / NO", "CLEAR ;", "ENTER _",
    "Ret", "Lock", ": *", "| @", "] [", "\xc2\xa8 ^", ", /", "< >",
    "Shift", "Break", "Cntl", NULL, NULL, NULL, NULL, "Esc",
};

const char *o2session_key_label(int machine, int key)
{
    if (key < 0)
        return NULL;
    if (machine == O2_MACHINE_VIDEOPAC_PLUS)
        return key < 64 ? g7400_labels[key] : NULL;
    return key < 48 ? o2_labels[key] : NULL;
}

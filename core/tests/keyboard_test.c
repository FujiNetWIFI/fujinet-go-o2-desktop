/*
 * keyboard_test -- the on-screen keyboard's layout tables, pure: for the
 * Odyssey 2 / Videopac G7000 and for the Videopac+ G7400, no key caps
 * overlap, every matrix position the console has is on a cap (and the ones it
 * does not have are not), the extent covers every cap, the labels are unique
 * and agree with o2session_key_label(), Y/YES and N/NO share their positions,
 * and the Odyssey 2's caps sit where the console's photograph has them.
 * No machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "o2session.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

#define EPS 1e-3f

static const o2_keycap *find(const o2_keycap *c, int n, const char *label)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(c[i].label, label)) return &c[i];
    return NULL;
}

/* A well-formed UTF-8 string (the labels carry the multiplication and division
 * signs, arrows and the pound sign). */
static int utf8_ok(const char *s)
{
    while (*s) {
        unsigned char b = (unsigned char)*s++;
        int more = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (more < 0) return 0;
        while (more--)
            if (((unsigned char)*s++ & 0xc0) != 0x80) return 0;
    }
    return 1;
}

/* A label of the cap is the position's label, or one of the two names a
 * shared position has ("Y / YES"). */
static int label_agrees(const char *cap, const char *key)
{
    return key && (!strcmp(cap, key) || strstr(key, cap) != NULL);
}

static void check_layout(int machine, const char *name, int top, const int *unreachable)
{
    int n = 0, i, j, bad = 0, resets = 0;
    float w = 0, h = 0;
    const o2_keycap *c = o2session_keyboard_layout(machine, &n, &w, &h);
    int hit[64] = { 0 };
    char what[160];

    snprintf(what, sizeof what, "%s: a layout with caps and an extent", name);
    check(c && n > 0 && w > 1 && h > 1, what);

    /* the caps: valid, inside the extent, not overlapping */
    for (i = 0; i < n; i++) {
        if (!c[i].label || !c[i].label[0] || !utf8_ok(c[i].label)) bad++;
        if (c[i].target < -1 || c[i].target >= top) bad++;
        if (c[i].w <= 0 || c[i].h <= 0 || c[i].x < -EPS || c[i].y < -EPS) bad++;
        if (c[i].x + c[i].w > w + EPS || c[i].y + c[i].h > h + EPS) bad++;
        if (c[i].kind < O2_KEYCAP_NORMAL || c[i].kind > O2_KEYCAP_SPACE) bad++;
        if (c[i].target >= 0) hit[c[i].target]++;
        if (c[i].target == -1) {
            resets++;
            if (c[i].kind != O2_KEYCAP_RESET) bad++;
        } else if (c[i].kind == O2_KEYCAP_RESET) {
            bad++;
        }
    }
    snprintf(what, sizeof what, "%s: every cap is well formed and inside the extent", name);
    check(!bad, what);
    snprintf(what, sizeof what, "%s: exactly one RESET cap, with no matrix position", name);
    check(resets == 1, what);

    bad = 0;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            const int ox = c[i].x < c[j].x + c[j].w - EPS && c[j].x < c[i].x + c[i].w - EPS;
            const int oy = c[i].y < c[j].y + c[j].h - EPS && c[j].y < c[i].y + c[i].h - EPS;
            if (ox && oy) {
                printf("  (%s overlaps %s)\n", c[i].label, c[j].label);
                bad++;
            }
        }
    snprintf(what, sizeof what, "%s: no two caps overlap", name);
    check(!bad, what);

    /* the extent is tight: something reaches each edge */
    {
        float maxx = 0, maxy = 0;
        for (i = 0; i < n; i++) {
            if (c[i].x + c[i].w > maxx) maxx = c[i].x + c[i].w;
            if (c[i].y + c[i].h > maxy) maxy = c[i].y + c[i].h;
        }
        snprintf(what, sizeof what, "%s: the extent is the caps' (%.2f x %.2f)", name, w, h);
        check(fabsf(maxx - w) < EPS && fabsf(maxy - h) < EPS, what);
    }

    /* every position the console has is reachable; the others are not */
    bad = 0;
    for (i = 0; i < top; i++) {
        int absent = 0;
        for (j = 0; unreachable[j] >= 0; j++)
            if (unreachable[j] == i) absent = 1;
        if (absent ? hit[i] != 0 : hit[i] == 0) {
            printf("  (matrix position %d: %d caps)\n", i, hit[i]);
            bad++;
        }
    }
    snprintf(what, sizeof what, "%s: every matrix position the console has is on a cap", name);
    check(!bad, what);

    /* labels: unique across the layout, and the position's own */
    bad = 0;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (!strcmp(c[i].label, c[j].label)) {
                printf("  (two caps say %s)\n", c[i].label);
                bad++;
            }
    snprintf(what, sizeof what, "%s: no two caps carry the same label", name);
    check(!bad, what);

    bad = 0;
    for (i = 0; i < n; i++)
        if (c[i].target >= 0 && !label_agrees(c[i].label, o2session_key_label(machine, c[i].target))) {
            printf("  (cap %s is position %d, labelled %s)\n", c[i].label, c[i].target,
                   o2session_key_label(machine, c[i].target) ? o2session_key_label(machine, c[i].target) : "nothing");
            bad++;
        }
    snprintf(what, sizeof what, "%s: each cap's label agrees with its position's", name);
    check(!bad, what);

    /* the position labels: present exactly where there is a key, unique, valid UTF-8 */
    bad = 0;
    for (i = 0; i < top; i++) {
        int absent = 0;
        const char *l = o2session_key_label(machine, i);
        for (j = 0; unreachable[j] >= 0; j++)
            if (unreachable[j] == i) absent = 1;
        if (absent ? l != NULL : (!l || !utf8_ok(l))) bad++;
        if (l)
            for (j = i + 1; j < top; j++) {
                const char *m = o2session_key_label(machine, j);
                if (m && !strcmp(l, m)) bad++;
            }
    }
    snprintf(what, sizeof what, "%s: a unique label for each position that has a key, none for the rest", name);
    check(!bad, what);
    check(o2session_key_label(machine, -1) == NULL && o2session_key_label(machine, 64) == NULL
          && o2session_key_label(machine, 1000) == NULL, "positions out of range have no label");
}

int main(void)
{
    static const int o2_absent[] = { 10, 11, -1 };
    static const int g7400_absent[] = { 10, 11, 59, 60, 61, 62, -1 };
    int n, nv, np;
    float w, h, wv, hv, wp, hp;
    const o2_keycap *c, *cv, *cp;
    const o2_keycap *k, *q;

    check_layout(O2_MACHINE_ODYSSEY2, "Odyssey 2", 48, o2_absent);
    check_layout(O2_MACHINE_VIDEOPAC_PLUS, "Videopac+ G7400", 64, g7400_absent);

    c = o2session_keyboard_layout(O2_MACHINE_ODYSSEY2, &n, &w, &h);
    cv = o2session_keyboard_layout(O2_MACHINE_VIDEOPAC, &nv, &wv, &hv);
    cp = o2session_keyboard_layout(O2_MACHINE_VIDEOPAC_PLUS, &np, &wp, &hp);
    check(c == cv && n == nv && w == wv && h == hv, "the Videopac G7000 has the Odyssey 2's keyboard");
    check(c != cp, "and the G7400 its own");
    check(o2session_keyboard_layout(O2_MACHINE_ODYSSEY2, NULL, NULL, NULL) == c, "the out parameters are optional");
    check(o2session_keyboard_layout(-1, &nv, NULL, NULL) == c && o2session_keyboard_layout(99, &nv, NULL, NULL) == c,
          "an unknown machine gets the Odyssey 2's");

    /* ---- the Odyssey 2, as the console photograph has it ---- */
    check(n == 10 + 10 + 10 + 9 + 9 + 1, "49 caps: 10 numerals, the function row and RESET, three letter rows, SPACE");

    /* the numeral row: 0 first, then 1 to 9 */
    {
        int ok = 1;
        char l[2] = "0";
        const o2_keycap *prev = NULL;
        for (int d = 0; d < 10; d++) {
            l[0] = (char)('0' + d);
            k = find(c, n, l);
            if (!k || k->target != d || k->y != c[0].y || (prev && k->x <= prev->x)) ok = 0;
            prev = k;
        }
        check(ok && find(c, n, "0")->x < find(c, n, "1")->x, "the numerals run 0 to 9 along the top, the 0 first");
    }
    /* the function row: + - x / = (red), YES NO CLEAR ENTER, then RESET at the right */
    {
        static const char *const row[10] = { "+", "-", "\xc3\x97", "\xc3\xb7", "=", "YES", "NO", "CLEAR", "ENTER", "RESET" };
        int ok = 1;
        const o2_keycap *prev = NULL;
        for (int i = 0; i < 10; i++) {
            k = find(c, n, row[i]);
            if (!k || k->y <= find(c, n, "0")->y || (prev && (k->x <= prev->x || k->y != prev->y))) ok = 0;
            prev = k;
        }
        check(ok, "below the numerals: + - x / = YES NO CLEAR ENTER and RESET, in a row");
    }
    check(find(c, n, "+")->kind == O2_KEYCAP_FUNCTION && find(c, n, "-")->kind == O2_KEYCAP_FUNCTION
          && find(c, n, "\xc3\x97")->kind == O2_KEYCAP_FUNCTION && find(c, n, "\xc3\xb7")->kind == O2_KEYCAP_FUNCTION
          && find(c, n, "=")->kind == O2_KEYCAP_FUNCTION, "the function keys have the red legends");
    check(find(c, n, "YES")->kind == O2_KEYCAP_INPUT && find(c, n, "NO")->kind == O2_KEYCAP_INPUT
          && find(c, n, "CLEAR")->kind == O2_KEYCAP_INPUT && find(c, n, "ENTER")->kind == O2_KEYCAP_INPUT,
          "YES NO CLEAR ENTER are the input keys");
    check(find(c, n, "RESET")->target == -1 && find(c, n, "RESET")->kind == O2_KEYCAP_RESET
          && find(c, n, "RESET")->x > find(c, n, "ENTER")->x, "the red RESET is at the right of the row");
    check(find(c, n, "Q")->kind == O2_KEYCAP_NORMAL && find(c, n, "?")->kind == O2_KEYCAP_NORMAL
          && find(c, n, "5")->kind == O2_KEYCAP_NORMAL, "the rest are plain keys");

    /* Y and YES are two caps on one switch, and so are N and NO */
    check(find(c, n, "Y")->target == 44 && find(c, n, "YES")->target == 44, "Y and YES are position 44");
    check(find(c, n, "N")->target == 45 && find(c, n, "NO")->target == 45, "N and NO are position 45");
    check(!strcmp(o2session_key_label(O2_MACHINE_ODYSSEY2, 44), "Y / YES")
          && !strcmp(o2session_key_label(O2_MACHINE_ODYSSEY2, 45), "N / NO"), "and say so");

    /* the matrix, from MAME's INPUT_PORTS: row * 8 + column */
    check(find(c, n, "Q")->target == 24 && find(c, n, "A")->target == 32 && find(c, n, "Z")->target == 33
          && find(c, n, "W")->target == 17 && find(c, n, "P")->target == 15 && find(c, n, "L")->target == 14
          && find(c, n, "+")->target == 16 && find(c, n, "ENTER")->target == 47
          && find(c, n, "CLEAR")->target == 46 && find(c, n, "SPACE")->target == 12
          && find(c, n, "?")->target == 13 && find(c, n, ".")->target == 39 && find(c, n, "-")->target == 40
          && find(c, n, "=")->target == 43, "the matrix positions are MAME's");

    /* the letter rows: QWERTYUIOP, ASDFGHJKL indented half a key, ZXCVBNM.? a key */
    {
        static const char *const r1[10] = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" };
        static const char *const r2[9] = { "A", "S", "D", "F", "G", "H", "J", "K", "L" };
        static const char *const r3[9] = { "Z", "X", "C", "V", "B", "N", "M", ".", "?" };
        int ok = 1, i;
        for (i = 1; i < 10; i++) if (find(c, n, r1[i])->x <= find(c, n, r1[i - 1])->x) ok = 0;
        for (i = 1; i < 9; i++) if (find(c, n, r2[i])->x <= find(c, n, r2[i - 1])->x) ok = 0;
        for (i = 1; i < 9; i++) if (find(c, n, r3[i])->x <= find(c, n, r3[i - 1])->x) ok = 0;
        check(ok, "the three letter rows run left to right in order");
        q = find(c, n, "Q");
        check(find(c, n, "Y")->y == q->y && find(c, n, "A")->y > q->y && find(c, n, "Z")->y > find(c, n, "A")->y,
              "QWERTYUIOP above ASDFGHJKL above ZXCVBNM.?");
        check(fabsf((find(c, n, "A")->x - q->x) - 0.5f * (find(c, n, "W")->x - q->x)) < 0.01f,
              "ASDFGHJKL is indented half a key");
        check(fabsf((find(c, n, "Z")->x - q->x) - (find(c, n, "W")->x - q->x)) < 0.01f,
              "ZXCVBNM.? is indented a whole key");
        check(fabsf(find(c, n, "Q")->w - 1.0f) < EPS && fabsf(find(c, n, "Q")->h - 1.0f) < EPS,
              "a letter key is 1.0 x 1.0, the gap between caps being in the coordinates");
    }
    /* the SPACE bar: wide, below, centred */
    k = find(c, n, "SPACE");
    check(k->kind == O2_KEYCAP_SPACE && k->w > 5.0f && k->y > find(c, n, "Z")->y
          && fabsf((k->x + k->w / 2) - w / 2) < 0.05f, "SPACE is a wide bar centred below the letters");

    /* ---- the Videopac+ G7400's own rows ---- */
    {
        static const char *const extra[] = { "Ret", "Lock", ": *", "| @", "] [", "\xc2\xa8 ^", ", /", "< >",
                                             "Shift", "Break", "Cntl", "Esc" };
        static const int pos[] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 63 };
        int ok = 1;
        for (int i = 0; i < 12; i++) {
            k = find(cp, np, extra[i]);
            if (!k || k->target != pos[i]) { ok = 0; printf("  (%s)\n", extra[i]); }
        }
        check(ok, "the G7400 adds rows 6 and 7: Ret Lock : * | @ ] [ diaeresis ^ , / < > and Shift Break Cntl Esc");
    }
    check(find(cp, np, "0 #") && find(cp, np, "0 #")->target == 0 && find(cp, np, "3 \xc2\xa3")
          && find(cp, np, "+ \xe2\x86\x91")->target == 16 && find(cp, np, "ENTER _")->target == 47
          && find(cp, np, "CLEAR ;")->target == 46, "and prints MAME's second legends on the others");
    check(find(cp, np, "RESET") && find(cp, np, "YES")->target == 44 && find(cp, np, "NO")->target == 45
          && find(cp, np, "Y")->target == 44 && find(cp, np, "N")->target == 45,
          "the G7400 has RESET, and Y/YES and N/NO share a position there too");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

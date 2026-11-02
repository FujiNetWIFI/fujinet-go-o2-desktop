/*
 * File and folder choosers for cartridges and the BIOS -- see import.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef O2_WIN_IMPORT_H
#define O2_WIN_IMPORT_H

#include <windows.h>

#include "o2session.h"

/* One cartridge (.bin .rom .o2) for Open Cartridge; 1 if one was chosen. */
int o2_pick_cart(HWND owner, const char *title, char *path, DWORD pathsz);

/* Asks for BIOS files (several, zips included) or a folder, imports each,
 * and says in one message box what came of it. Returns how many ROM items
 * were imported (0: nothing recognised), or -1 when the chooser was
 * cancelled. */
int o2_import_bios(HWND owner, o2session *session, int folder);

/* Imports dropped or named files and folders the same way, one message box
 * for the lot. Returns how many ROM items came in. */
int o2_import_roms(HWND owner, o2session *session, const char *const *paths, int n);

/* Asks for cartridge images (several) and copies them onto FujiNet's SD
 * host, then says so in one message box. Returns how many were copied, or
 * -1 when the chooser was cancelled. */
int o2_import_carts(HWND owner, o2session *session);

#endif

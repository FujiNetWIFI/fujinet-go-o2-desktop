/*
 * The FujiNet console log window and the FujiNet configuration window, and
 * the accent colour helpers every window shares.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

extern "C" {
#include "o2session.h"
}

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, o2session *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, o2session *session);

/* The accent colour: the Odyssey 2's power-button red. It carries WHITE text
 * (a held key, the debugger's current line, the Map target). */
QColor o2AccentColor();
/* Its darker variant, the console's reset key: outlines and text on the
 * light themes, and the RESET cap. */
QColor o2AccentDarkColor();
/* The accent for TEXT (and text-like marks: the status dot, a breakpoint
 * dot): the red on a dark theme, the darker variant on a light one, where
 * the red is too strong to read as a mark. */
QColor o2AccentTextColor(const QWidget *w);
/* A style sheet that fills a button (or a label) in the accent with white
 * text. */
QString o2AccentFill();

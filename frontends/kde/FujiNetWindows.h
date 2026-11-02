/*
 * The FujiNet console log window and the FujiNet configuration window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

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

/* The accent colour (the Odyssey 2 case's tan), for highlights and for the
 * fills that carry dark text. */
QColor o2AccentColor();
/* The accent for TEXT (and text-like marks: the status dot, a breakpoint
 * dot): the tan on a dark theme, its darker variant on a light one, where
 * the tan is too pale to read. */
QColor o2AccentTextColor(const QWidget *w);

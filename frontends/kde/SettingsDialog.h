/*
 * SettingsDialog -- the Preferences dialog. The analog stick switch, the
 * gamepad assignments, the picture and the volume apply live; the BIOS
 * (imported, or switched on or off) and the host options need a power
 * cycle, which the caller does when run() says one of those changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>
#include <QString>

extern "C" {
#include "o2session.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a restart option changed. */
    static bool run(QWidget *parent, o2session *session);
    /* Import BIOS: the user's own Odyssey 2 BIOS (the 2K set, the 1K system
     * ROM, or one of MAME's four 512-byte ROMs), from `path` or, when it is
     * empty, a file chooser. Shows the session's message, which always says
     * what happened. Returns the image's index, or -1 (also when the
     * chooser was cancelled). */
    static int importBios(QWidget *parent, o2session *session, const QString &path = QString());
};

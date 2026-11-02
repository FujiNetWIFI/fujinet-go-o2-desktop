/*
 * SettingsDialog -- the Preferences dialog, and the Import BIOS flow it
 * shares with the main window.
 *
 * Every row applies as it is changed: the picture, the volume, the analog
 * sticks and the gamepad assignments take effect at once, and the machine,
 * BIOS, Voice, audio, gamepad and FujiNet rows are applied by power-cycling
 * the console when the dialog closes (run() says so).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QString>
#include <QWidget>

extern "C" {
#include "o2session.h"
}

namespace SettingsDialog {

/* Runs the dialog. Returns true when something that needs a power cycle
 * changed (or a ROM was imported), for the caller to restart the session. */
bool run(QWidget *parent, o2session *session);

/* Import BIOS: the user's own ROM dumps (the console BIOS, the video chips'
 * character sets, The Voice's speech ROMs), from `path` or, when it is
 * empty, from a file chooser (several files at once, zips included) or,
 * with `folder`, from a folder picked with one. Shows what the session says
 * happened for every input. Returns how many ROMs were imported, or -1 when
 * the chooser was cancelled. They take effect at the next power cycle. */
int importRoms(QWidget *parent, o2session *session, bool folder, const QString &path = QString());

} // namespace SettingsDialog

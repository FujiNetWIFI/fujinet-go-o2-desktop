/*
 * The debugger's chip tabs: EF9340 (the Videopac+ G7400's text chip), Sound &
 * I/O (the ports decoded by function, the keyboard matrix and sticks live,
 * the 8244's sound, The Voice) and Cart (the cartridge's mapper or the
 * FujiNet cartridge's state and mailbox page).
 *
 * Each is refreshed by the debugger window while it shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>
#include <memory>

#include "o2debug.h"

class EfPageWidget;
class KeyMatrixWidget;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class StickDiagram;

class EfTab : public QWidget {
public:
    explicit EfTab(o2debug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    o2debug *m_dbg;
    std::unique_ptr<ef9340_snapshot_t> m_snap;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_text = nullptr;
    EfPageWidget *m_page = nullptr;
    QLabel *m_layer = nullptr;
    QLabel *m_note = nullptr;
};

class IoTab : public QWidget {
public:
    IoTab(o2debug *debugger, o2session *session, QWidget *parent = nullptr);
    void refresh();

private:
    o2debug *m_dbg;
    o2session *m_session;
    QPlainTextEdit *m_text = nullptr;
    KeyMatrixWidget *m_matrix = nullptr;
    StickDiagram *m_stick[2] = {};
    QLabel *m_reset = nullptr;
};

class CartTab : public QWidget {
public:
    explicit CartTab(o2debug *debugger, QWidget *parent = nullptr);
    void refresh();

private:
    o2debug *m_dbg;
    QPlainTextEdit *m_text = nullptr;
    QPlainTextEdit *m_mailbox = nullptr;
};

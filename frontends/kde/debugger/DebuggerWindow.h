/*
 * Debugger window (Qt6 Widgets) over the CDP1802 / CDP1861 debugger engine,
 * via core/include/o2debug.h. Mirrors the GNOME one tab for tab.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "o2debug.h"
#include "o2session.h"
}

/* The painted panels, defined in DebuggerWindow.cpp: a keypad as the
 * console sees it (Sound & I/O), the cartridge's page map and its text
 * engine's character grid (Cart). */
class KeypadDiagram;
class PageMapWidget;
class TextGridWidget;

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, o2session *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, o2session *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(o2session *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildVideo();
    QWidget *buildIo();
    QWidget *buildBreaks();
    QWidget *buildCart();
    QWidget *buildFujinet();

    void refreshAll();
    void refreshStatus();
    void refreshCpu();
    void refreshRam();
    void refreshDisasm();
    void refreshVideo();
    void refreshIo();
    void refreshBps();
    void refreshCart();
    void refreshFujinet(bool force);
    void tick();
    void toggleRun();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(o2debug *));
    void loadSymbols(bool fromFile);
    void setRegister(int reg, QLineEdit *edit);
    bool addressOf(const QString &text, long *out);

    o2session *m_session;
    o2debug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    QLineEdit *m_reg[O2_REG_COUNT] = {};
    QLabel *m_regLabel[16] = {};
    QCheckBox *m_ef[4] = {};
    QLabel *m_cpuInfo = nullptr;
    QPlainTextEdit *m_ram = nullptr;
    QLineEdit *m_ramGoto = nullptr;
    QLineEdit *m_ramAddr = nullptr, *m_ramVal = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    std::vector<uint16_t> m_lineAddr;

    QPlainTextEdit *m_videoText = nullptr;
    QComboBox *m_videoView = nullptr;
    QLabel *m_videoPic = nullptr;
    QLabel *m_videoNote = nullptr;
    std::vector<uint32_t> m_videoPx;

    QPlainTextEdit *m_io = nullptr;
    KeypadDiagram *m_keypad[2] = {};

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QPlainTextEdit *m_cart = nullptr;
    PageMapWidget *m_pageMap = nullptr;
    TextGridWidget *m_textGrid = nullptr;
    QLabel *m_textNote = nullptr;

    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};

/*
 * Debugger window (Qt6 Widgets) over the MCS-48 / 8244 debugger engine, via
 * core/include/o2debug.h. Mirrors the GNOME one tab for tab: Prompt, CPU &
 * RAM, Disassembly, Video (the 8244), EF9340, Sound & I/O, Breakpoints, Cart,
 * FujiNet.
 *
 * The engine is attached while the window is shown (which stops the machine,
 * as on every sibling) and detached when it is hidden, which lets the
 * machine run on. A timer runs only while it is shown, and refreshes only the
 * tab that shows.
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

class CartTab;
class EfTab;
class IoTab;
class VideoTab;

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
    QWidget *buildBreaks();
    QWidget *buildFujinet();
    QTableWidget *hexTable(int space, int size);
    QLineEdit *regEdit(int reg, int digits, const QString &tip);

    void refreshAll();
    void refreshTab(int index);
    void refreshStatus();
    void refreshCpu();
    void refreshRam();
    void refreshDisasm();
    void refreshBps();
    void refreshFujinet(bool force);
    void tick();
    void toggleRun();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(o2debug *));
    void loadSymbols(bool fromFile);
    void setRegister(int reg, QLineEdit *edit);
    void setBankRegister(int bank, int n, QLineEdit *edit);
    void syncBpForm();
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
    QLineEdit *m_bankReg[2][8] = {};
    QLabel *m_bankHead[2] = {};
    QCheckBox *m_state[9] = {};
    QLabel *m_cpuInfo = nullptr;
    QTableWidget *m_hex[2] = {};            /* the internal and the external RAM */
    std::vector<uint8_t> m_hexPrev[2];
    bool m_hexFilling = false;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    std::vector<uint16_t> m_lineAddr;

    VideoTab *m_video = nullptr;
    EfTab *m_ef = nullptr;
    IoTab *m_io = nullptr;
    CartTab *m_cart = nullptr;

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr, *m_bpSpace = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QLabel *m_fujiNote = nullptr;
    QPlainTextEdit *m_fujiText = nullptr, *m_fujiState = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;
};

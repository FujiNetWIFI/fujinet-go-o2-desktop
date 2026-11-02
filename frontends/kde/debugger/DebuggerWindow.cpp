/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPointer>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"
#include "ChipTabs.h"
#include "DebugUi.h"
#include "VideoTab.h"

#define DISASM_WINDOW 48

using dbgui::atEnd;
using dbgui::hx;
using dbgui::monoView;
using dbgui::setText;
using dbgui::stripControl;
using dbgui::titleLabel;

namespace {

/* $0400, 0x0400, 400h, #1024; a bare number is hex, as everywhere in an 8048
 * debugger. */
bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else if (s.endsWith('h', Qt::CaseInsensitive)) v = s.chopped(1).toLong(&ok, 16);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

/* The order of the tabs, and their names for O2_DEBUGGER_TAB. */
enum { kTabPrompt, kTabCpu, kTabDisasm, kTabVideo, kTabEf, kTabIo, kTabBreaks, kTabCart, kTabFujinet };
const char *const kTabNames[] = { "prompt", "cpu", "disassembly", "video", "ef9340", "io", "breakpoints", "cart", "fujinet" };
/* other names a tab answers to */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "ram", kTabCpu }, { "8244", kTabVideo }, { "vdc", kTabVideo }, { "ef", kTabEf }, { "sound", kTabIo },
    { "voice", kTabIo }, { "keys", kTabIo }, { "mailbox", kTabCart },
};

QPointer<DebuggerWindow> s_win;

/* The external-data and program spaces' names for the breakpoint list. */
const char *spaceName(int space)
{
    switch (space) {
    case O2_SPACE_PROG: return "program";
    case O2_SPACE_XDATA: return "external data";
    case O2_SPACE_IRAM: return "internal RAM";
    default: return "8244 registers";
    }
}

} // namespace

void DebuggerWindow::showFor(QWidget *parent, o2session *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    /* Opening the debugger stops the machine, as on every sibling. */
    o2debug_attach(s_win->m_dbg);
    s_win->refreshAll();
}

void DebuggerWindow::toggleFor(QWidget *parent, o2session *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(o2session *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(o2debug_get(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1400, 900);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = m_tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    m_video = new VideoTab(m_dbg);
    tabs->addTab(m_video, QStringLiteral("Video"));
    m_ef = new EfTab(m_dbg);
    tabs->addTab(m_ef, QStringLiteral("EF9340"));
    m_io = new IoTab(m_dbg, m_session);
    tabs->addTab(m_io, QStringLiteral("Sound && I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    m_cart = new CartTab(m_dbg);
    tabs->addTab(m_cart, QStringLiteral("Cart"));
    tabs->addTab(buildFujinet(), QStringLiteral("FujiNet"));
    /* O2_DEBUGGER_TAB: an index, or a tab's name (or the start of one) */
    if (qEnvironmentVariableIsSet("O2_DEBUGGER_TAB")) {
        const QString want = qEnvironmentVariable("O2_DEBUGGER_TAB").trimmed().toLower();
        bool ok = false;
        int n = want.toInt(&ok);
        if (!ok) {
            n = -1;
            for (int i = 0; i < (int)(sizeof kTabNames / sizeof kTabNames[0]) && !want.isEmpty(); ++i)
                if (QString::fromUtf8(kTabNames[i]).startsWith(want)) { n = i; break; }
            for (const auto &a : kTabAliases)
                if (n < 0 && !want.isEmpty() && QString::fromUtf8(a.name).startsWith(want)) n = a.tab;
        }
        if (n >= 0 && n < tabs->count()) tabs->setCurrentIndex(n);
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this](int page) { refreshTab(page); });
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    /* started on show, stopped on hide: no wakeups while the window is away */
}

/* ---- the toolbar --------------------------------------------------------------- */

void DebuggerWindow::stepAnd(void (*fn)(o2debug *))
{
    fn(m_dbg);
    refreshAll();
}

void DebuggerWindow::toggleRun()
{
    if (o2debug_is_stopped(m_dbg)) o2debug_resume(m_dbg); else o2debug_stop(m_dbg);
    refreshAll();
}

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), [this] { stepAnd(o2debug_step); });
    add(QStringLiteral("Over (F8)"), [this] { stepAnd(o2debug_step_over); });
    add(QStringLiteral("Out (Shift+F8)"), [this] { stepAnd(o2debug_step_out); });
    add(QStringLiteral("Scanline+1"), [this] { o2debug_scanline(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Frame+1"), [this] { o2debug_frame(m_dbg, 1); refreshAll(); });

    auto *sym = new QPushButton(QStringLiteral("Load Symbols..."));
    sym->setFocusPolicy(Qt::NoFocus);
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File..."), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Beside the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    h->addWidget(sym);

    auto *save = new QPushButton(QStringLiteral("Save..."));
    save->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly (the BIOS and the cartridge, as mapped)", "disassembly.asm" },
        { "ram", "RAM (64 internal and 128 external bytes)", "ram.bin" },
        { "mailbox", "Mailbox page ($F20-$FFF)", "mailbox.bin" },
        { "vdc", "8244 registers (256 bytes)", "vdc.bin" },
        { "font", "Character set (the 8244's font)", "font.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "regs", "CPU, video and I/O state", "registers.txt" } };
    for (const auto &s : saves) {
        const char *kind = s.kind;
        const char *title = s.title;
        const char *file = s.file;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title, file] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)),
                                                              QString::fromUtf8(file));
            if (path.isEmpty()) return;
            char msg[512];
            o2debug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            const QString text = stripControl(msg);
            appendPrompt(text + QLatin1Char('\n'));
            statusBar()->showMessage(text, 6000);
        });
    }
    save->setMenu(menu);
    h->addWidget(save);

    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

/* A symbol file the user picks, or (fromFile false) the <cart>.map/.lst/.sym
 * beside the opened cartridge. */
void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(o2session_cart_path(m_session));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.map *.lst *.sym *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512];
    const QByteArray p = path.toLocal8Bit();
    o2debug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    const QString text = stripControl(msg);
    appendPrompt(text + QLatin1Char('\n'));
    statusBar()->showMessage(text, 6000);
    refreshAll();
}

/* ---- Prompt ----------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Debugger prompt. Type 'help' for every command.\n"));
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, runto, disasm, vdc, objects, io, keys, cart ...)"
                                                  " — Tab completes, Up/Down recall"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    v->addWidget(m_promptOut, 1);
    v->addWidget(m_promptIn);
    return w;
}

/* ---- CPU & RAM -------------------------------------------------------------------------- */

QLineEdit *DebuggerWindow::regEdit(int reg, int digits, const QString &tip)
{
    auto *e = m_reg[reg] = new QLineEdit;
    e->setMaxLength(digits);
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits + 1, QLatin1Char('0'))) + 14);
    e->setToolTip(tip);
    connect(e, &QLineEdit::returnPressed, this, [this, reg, e] { setRegister(reg, e); });
    return e;
}

/* A hex editor over one of the 8048's RAMs: sixteen to a row, the byte
 * a double-click (or a typed digit) edits, the ones that changed since the
 * last look in the accent. */
QTableWidget *DebuggerWindow::hexTable(int space, int size)
{
    const int rows = size / 16;
    auto *t = new QTableWidget(rows, 16);
    QStringList cols, rowNames;
    for (int c = 0; c < 16; ++c) cols << hx(c, 1);
    for (int r = 0; r < rows; ++r) rowNames << QStringLiteral("$") + hx(r * 16, 2);
    t->setHorizontalHeaderLabels(cols);
    t->setVerticalHeaderLabels(rowNames);
    t->setFont(dbgui::monoFont(t->font()));
    const QFontMetrics fm(t->font());
    const int cw = fm.horizontalAdvance(QStringLiteral("00")) + 14, ch = fm.height() + 8;
    t->horizontalHeader()->setDefaultSectionSize(cw);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    t->verticalHeader()->setDefaultSectionSize(ch);
    t->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    const int vw = fm.horizontalAdvance(QStringLiteral("$00")) + 20;
    t->verticalHeader()->setFixedWidth(vw);
    t->setFixedSize(vw + 16 * cw + 2 * t->frameWidth() + 2,
                    t->horizontalHeader()->sizeHint().height() + rows * ch + 2 * t->frameWidth() + 2);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < 16; ++c) {
            auto *it = new QTableWidgetItem(QStringLiteral("00"));
            it->setTextAlignment(Qt::AlignCenter);
            t->setItem(r, c, it);
        }
    connect(t, &QTableWidget::itemChanged, this, [this, space](QTableWidgetItem *it) {
        if (m_hexFilling) return;
        bool ok = false;
        const unsigned v = it->text().trimmed().toUInt(&ok, 16);
        if (ok && v <= 0xFF) o2debug_write(m_dbg, space, (uint16_t)(it->row() * 16 + it->column()), (uint8_t)v);
        /* once the editor has closed, so the byte shows as two digits */
        QTimer::singleShot(0, this, [this] { refreshRam(); });
    });
    return t;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;

    /* the registers, three columns of label and field; an edit goes in with
     * Enter, while the machine is stopped */
    auto *regs = new QGroupBox(QStringLiteral("Registers"));
    auto *g = new QGridLayout(regs);
    struct Field { int reg, digits; const char *name, *tip; };
    static const Field cols[3][6] = {
        { { O2_REG_A, 2, "A", "The accumulator" },
          { O2_REG_PC, 3, "PC", "The program counter (12 bits): the instruction about to run" },
          { O2_REG_MB, 1, "MB", "The memory-bank latch (A11) that SEL MB0 and MB1 set" },
          { O2_REG_PSW, 2, "PSW", "The program status word as the 8048 keeps it: C AC F0 BS 1 SP2 SP1 SP0" },
          { -1, 0, nullptr, nullptr } },
        { { O2_REG_C, 1, "C", "The carry (0 or 1)" },
          { O2_REG_AC, 1, "AC", "The auxiliary carry (0 or 1)" },
          { O2_REG_F0, 1, "F0", "Flag 0 (a PSW bit)" },
          { O2_REG_F1, 1, "F1", "Flag 1" },
          { O2_REG_BS, 1, "BS", "The register bank in use (0 or 1)" },
          { O2_REG_SP, 1, "SP", "The stack pointer, 0-7 (two bytes of internal RAM a level)" } },
        { { O2_REG_T, 2, "T", "The timer / event counter" },
          { O2_REG_CNT, 2, "CNT", "The timer's /32 prescaler count" },
          { O2_REG_P1, 2, "P1", "The P1 output latch" },
          { O2_REG_P2, 2, "P2", "The P2 output latch" },
          { O2_REG_BUS, 2, "BUS", "The BUS output latch" } },
    };
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 6; ++row) {
            const Field &f = cols[col][row];
            if (!f.name) continue;
            auto *label = new QLabel(QString::fromUtf8(f.name));
            label->setToolTip(QString::fromUtf8(f.tip));
            g->addWidget(label, row, col * 3, Qt::AlignRight);
            g->addWidget(regEdit(f.reg, f.digits, QString::fromUtf8(f.tip)), row, col * 3 + 1);
        }
    g->setColumnMinimumWidth(2, 16);
    g->setColumnMinimumWidth(5, 16);
    g->setColumnStretch(8, 1);
    left->addWidget(regs);

    /* R0-R7 of both banks: they are the first and fourth eight bytes of the
     * internal RAM, so an edit is a store there */
    auto *banks = new QGroupBox(QStringLiteral("Register banks (R0-R7)"));
    auto *bg = new QGridLayout(banks);
    for (int b = 0; b < 2; ++b) {
        m_bankHead[b] = new QLabel(QStringLiteral("Bank %1").arg(b));
        m_bankHead[b]->setAlignment(Qt::AlignCenter);
        bg->addWidget(m_bankHead[b], 0, 1 + b);
        for (int n = 0; n < 8; ++n) {
            auto *e = m_bankReg[b][n] = new QLineEdit;
            e->setMaxLength(2);
            e->setFixedWidth(e->fontMetrics().horizontalAdvance(QStringLiteral("000")) + 14);
            e->setToolTip(QStringLiteral("R%1 of bank %2: internal RAM $%3").arg(n).arg(b).arg(hx(b * 24 + n, 2)));
            connect(e, &QLineEdit::returnPressed, this, [this, b, n, e] { setBankRegister(b, n, e); });
            if (b == 0) bg->addWidget(new QLabel(QStringLiteral("R%1").arg(n)), 1 + n, 0, Qt::AlignRight);
            bg->addWidget(e, 1 + n, 1 + b, Qt::AlignCenter);
        }
    }
    bg->setColumnStretch(3, 1);
    left->addWidget(banks);

    /* the timer, the interrupts and the pins: what the CPU reads, not
     * editable */
    auto *stateBox = new QGroupBox(QStringLiteral("Timer, interrupts and pins"));
    auto *sg = new QGridLayout(stateBox);
    static const struct { const char *name, *tip; } states[9] = {
        { "TF", "The timer-overflow flag (JTF tests and clears it)" },
        { "EN TCNTI", "The timer/counter interrupt is enabled" },
        { "EN I", "The external interrupt is enabled" },
        { "In interrupt", "An interrupt routine is running" },
        { "IRQ pending", "The external interrupt is waiting" },
        { "Timer IRQ pending", "The timer interrupt is waiting" },
        { "T0", "The T0 input: the cartridge's (The Voice's LRQ)" },
        { "T1", "The T1 input: vertical or horizontal blank" },
        { "INT", "The INT input: the 8244's interrupt line" },
    };
    for (int i = 0; i < 9; ++i) {
        m_state[i] = new QCheckBox(QString::fromUtf8(states[i].name));
        m_state[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_state[i]->setFocusPolicy(Qt::NoFocus);
        m_state[i]->setToolTip(QString::fromUtf8(states[i].tip));
        sg->addWidget(m_state[i], i / 3, i % 3);
    }
    left->addWidget(stateBox);
    m_cpuInfo = dbgui::dimLabel();
    m_cpuInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left->addWidget(m_cpuInfo);
    left->addStretch();
    h->addLayout(left);

    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Internal RAM ($00-$3F): the register banks, the stack and your variables")));
    m_hex[0] = hexTable(O2_SPACE_IRAM, 64);
    right->addWidget(m_hex[0]);
    right->addSpacing(8);
    right->addWidget(titleLabel(QStringLiteral("External RAM ($00-$7F, through MOVX)")));
    m_hex[1] = hexTable(O2_SPACE_XDATA, 128);
    right->addWidget(m_hex[1]);
    right->addWidget(dbgui::dimLabel(QStringLiteral("Double-click a byte to edit it (two hex digits, Enter). "
                                                    "Changed since the last look: red.")));
    right->addStretch();
    h->addLayout(right, 1);
    return w;
}

/* A register typed in, with Enter: taken while the machine is stopped (a
 * running CPU would overwrite it at once). */
void DebuggerWindow::setRegister(int reg, QLineEdit *edit)
{
    long val;
    if (!o2debug_is_stopped(m_dbg))
        statusBar()->showMessage(QStringLiteral("Stop the machine to edit a register"), 4000);
    else if (parseNum(edit->text(), &val))
        o2debug_cpu_set(m_dbg, reg, (int)val);
    edit->clearFocus();
    refreshAll();
}

void DebuggerWindow::setBankRegister(int bank, int n, QLineEdit *edit)
{
    long val;
    if (!o2debug_is_stopped(m_dbg))
        statusBar()->showMessage(QStringLiteral("Stop the machine to edit a register"), 4000);
    else if (parseNum(edit->text(), &val))
        o2debug_write(m_dbg, O2_SPACE_IRAM, (uint16_t)(bank * 24 + n), (uint8_t)val);
    edit->clearFocus();
    refreshAll();
}

void DebuggerWindow::refreshCpu()
{
    o2debug_cpu c;
    o2debug_cpu_get(m_dbg, &c);
    auto set = [&](QLineEdit *e, unsigned v, int digits) {
        if (!e || e->hasFocus()) return;
        const QString t = hx(v, digits);
        if (e->text() != t) e->setText(t);
    };
    set(m_reg[O2_REG_A], (unsigned)c.a & 0xFF, 2);
    set(m_reg[O2_REG_PC], (unsigned)c.pc & 0xFFF, 3);
    set(m_reg[O2_REG_MB], (unsigned)c.mb & 1, 1);
    set(m_reg[O2_REG_PSW], (unsigned)c.psw & 0xFF, 2);
    set(m_reg[O2_REG_C], (unsigned)c.cy & 1, 1);
    set(m_reg[O2_REG_AC], (unsigned)c.ac & 1, 1);
    set(m_reg[O2_REG_F0], (unsigned)c.f0 & 1, 1);
    set(m_reg[O2_REG_F1], (unsigned)c.f1 & 1, 1);
    set(m_reg[O2_REG_BS], (unsigned)c.bs & 1, 1);
    set(m_reg[O2_REG_SP], (unsigned)c.sp & 7, 1);
    set(m_reg[O2_REG_T], (unsigned)c.timer & 0xFF, 2);
    set(m_reg[O2_REG_CNT], (unsigned)c.prescaler & 0xFF, 2);
    set(m_reg[O2_REG_P1], (unsigned)c.p1 & 0xFF, 2);
    set(m_reg[O2_REG_P2], (unsigned)c.p2 & 0xFF, 2);
    set(m_reg[O2_REG_BUS], (unsigned)c.bus & 0xFF, 2);
    for (int b = 0; b < 2; ++b) {
        for (int n = 0; n < 8; ++n) set(m_bankReg[b][n], c.r[b][n], 2);
        m_bankHead[b]->setText(c.bs == b ? QStringLiteral("Bank %1 (in use)").arg(b) : QStringLiteral("Bank %1").arg(b));
    }
    const bool flags[9] = { c.tf != 0, c.tcnti_en != 0, c.ext_int_en != 0, c.in_interrupt != 0,
                            c.irq_pending != 0, c.timer_irq_pending != 0, c.t0 != 0, c.t1 != 0, c.int_line != 0 };
    for (int i = 0; i < 9; ++i) m_state[i]->setChecked(flags[i]);
    static const char *const modes[3] = { "stopped", "timer (/32 of the machine cycle)", "event counter (T1)" };
    m_cpuInfo->setText(QStringLiteral("Timer: %1. Cycles %2, instructions %3, beam line %4 dot %5, frame %6. %7.")
                           .arg(QString::fromUtf8(modes[c.timer_mode < 0 || c.timer_mode > 2 ? 0 : c.timer_mode]))
                           .arg((qulonglong)c.cycles).arg((qulonglong)c.instructions).arg(c.vpos).arg(c.hpos)
                           .arg(c.frame).arg(QString::fromUtf8(o2_machine_name(c.machine))));
}

void DebuggerWindow::refreshRam()
{
    static const struct { int space, size; } spaces[2] = { { O2_SPACE_IRAM, 64 }, { O2_SPACE_XDATA, 128 } };
    for (int k = 0; k < 2; ++k) {
        QTableWidget *t = m_hex[k];
        if (!t || dbgui::editing(t)) continue;
        uint8_t buf[128] = {};
        o2debug_read(m_dbg, spaces[k].space, 0, buf, spaces[k].size);
        const bool first = m_hexPrev[k].empty();
        m_hexFilling = true;
        for (int i = 0; i < spaces[k].size; ++i) {
            QTableWidgetItem *it = t->item(i / 16, i % 16);
            const QString text = hx(buf[i], 2);
            if (it->text() != text) it->setText(text);
            const bool changed = !first && m_hexPrev[k][(size_t)i] != buf[i];
            it->setForeground(changed ? QBrush(o2AccentTextColor(t)) : QBrush());
        }
        m_hexFilling = false;
        m_hexPrev[k].assign(buf, buf + spaces[k].size);
    }
}

/* ---- Disassembly -------------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::toggled, this, [this](bool on) { if (on) refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Go to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to toggle its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

void DebuggerWindow::refreshDisasm()
{
    static o2debug_line lines[DISASM_WINDOW];
    if (m_followPc->isChecked()) {
        o2debug_cpu c;
        o2debug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)o2debug_row_address(m_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int pcLine = -1;
    const int n = o2debug_disassemble(m_dbg, m_disasmTop, lines, DISASM_WINDOW, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[i].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, " %c %03X  %-6s %-*s %-22s", lines[i].is_pc ? '>' : ' ', lines[i].address,
                 lines[i].bytes, labelWidth, label, lines[i].disasm);
        text += lines[i].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
        text += QLatin1Char('\n');
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)\n");
    m_disasm->setPlainText(text);
    for (int i = 0; i < n; ++i) {
        if (i == pcLine) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.select(QTextCursor::LineUnderCursor);
            QTextCharFormat fmt;
            fmt.setBackground(o2AccentColor());
            fmt.setForeground(Qt::white);
            cur.setCharFormat(fmt);
        } else if (lines[i].has_breakpoint) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            fmt.setForeground(o2AccentTextColor(m_disasm));
            cur.setCharFormat(fmt);
        }
    }
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    if (!addressOf(text, &a)) { statusBar()->showMessage(QStringLiteral("No such address or label"), 4000); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)o2debug_row_address(m_dbg, (uint16_t)(a & 0xFFF), -(DISASM_WINDOW / 3));
    refreshDisasm();
}

/* ---- Breakpoints ------------------------------------------------------------------------------- */

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Exec"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("In"), QStringLiteral("Out") });
    m_bpType->setToolTip(QStringLiteral("Exec stops before an instruction runs; Read and Write on an access to "
                                        "memory; In and Out on a read or write of a port"));
    m_bpSpace = new QComboBox;
    m_bpStart = new QLineEdit;
    m_bpStart->setPlaceholderText(QStringLiteral("start ($400 or label)"));
    m_bpStart->setMinimumWidth(m_bpStart->fontMetrics().horizontalAdvance(m_bpStart->placeholderText()) + 16);
    m_bpStart->setMaximumWidth(200);
    m_bpEnd = new QLineEdit;
    m_bpEnd->setPlaceholderText(QStringLiteral("end (optional)"));
    m_bpEnd->setMaximumWidth(140);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == $FF or value & $80)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[5] = { O2DEBUG_BP_EXEC, O2DEBUG_BP_READ, O2DEBUG_BP_WRITE, O2DEBUG_BP_IN, O2DEBUG_BP_OUT };
        const int type = m_bpType->currentIndex();
        const bool port = type >= 3;
        long a = 0, b = 0;
        if (!port) {
            if (m_bpStart->text().trimmed().isEmpty()) return;
            if (!addressOf(m_bpStart->text(), &a)) { statusBar()->showMessage(QStringLiteral("Bad start address"), 4000); return; }
            b = a;
            if (!m_bpEnd->text().trimmed().isEmpty() && !addressOf(m_bpEnd->text(), &b)) {
                statusBar()->showMessage(QStringLiteral("Bad end address"), 4000);
                return;
            }
        }
        /* a memory breakpoint names its space; a port one the port (BUS 0, P1 1, P2 2) */
        const int space = type == 0 ? O2_SPACE_PROG : m_bpSpace->currentData().toInt();
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (o2debug_breakpoint_add(m_dbg, types[type], space, (uint16_t)a, (uint16_t)b, cond.constData()) < 0) {
            statusBar()->showMessage(QStringLiteral("The condition does not parse"), 6000);
            return;
        }
        m_bpStart->clear();
        m_bpEnd->clear();
        m_bpCond->clear();
        refreshAll();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_bpStart, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpEnd, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpCond, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpType, &QComboBox::currentIndexChanged, this, [this] { syncBpForm(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpSpace);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);
    syncBpForm();

    m_bpTable = new QTableWidget(0, 7);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Where"), QStringLiteral("Range"), QStringLiteral("Hits"),
                                           QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setStretchLastSection(true);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        o2debug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    v->addWidget(m_bpTable, 1);

    auto *btns = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bpTable->currentRow();
        if (r < 0) return;
        o2debug_breakpoint_remove(m_dbg, m_bpTable->item(r, 0)->data(Qt::UserRole).toInt());
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        o2debug_breakpoint_clear(m_dbg);
        refreshAll();
    });
    btns->addWidget(remove);
    btns->addWidget(clear);
    btns->addStretch();
    v->addLayout(btns);
    return w;
}

/* The space (or the port) a breakpoint watches, and whether it takes an
 * address, follow its type. */
void DebuggerWindow::syncBpForm()
{
    const int type = m_bpType->currentIndex();
    m_bpSpace->clear();
    if (type == 0) {
        m_bpSpace->addItem(QStringLiteral("program"), O2_SPACE_PROG);
    } else if (type == 1 || type == 2) {
        m_bpSpace->addItem(QStringLiteral("external data"), O2_SPACE_XDATA);
        m_bpSpace->addItem(QStringLiteral("internal RAM"), O2_SPACE_IRAM);
        m_bpSpace->addItem(QStringLiteral("program"), O2_SPACE_PROG);
        m_bpSpace->addItem(QStringLiteral("8244 registers"), O2_SPACE_VDC);
    } else {
        m_bpSpace->addItem(QStringLiteral("BUS"), 0);
        m_bpSpace->addItem(QStringLiteral("P1"), 1);
        m_bpSpace->addItem(QStringLiteral("P2"), 2);
    }
    m_bpSpace->setEnabled(type != 0);
    m_bpStart->setEnabled(type < 3);
    m_bpEnd->setEnabled(type < 3);
}

void DebuggerWindow::refreshBps()
{
    o2debug_breakpoint bps[256];
    const int n = o2debug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QString::number(bps[i].id)));
        QStringList type;
        if (bps[i].type & O2DEBUG_BP_EXEC) type << QStringLiteral("Exec");
        if (bps[i].type & O2DEBUG_BP_READ) type << QStringLiteral("Read");
        if (bps[i].type & O2DEBUG_BP_WRITE) type << QStringLiteral("Write");
        if (bps[i].type & O2DEBUG_BP_IN) type << QStringLiteral("In");
        if (bps[i].type & O2DEBUG_BP_OUT) type << QStringLiteral("Out");
        m_bpTable->setItem(i, 2, new QTableWidgetItem(type.join(QLatin1Char('/'))));
        const bool port = (bps[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
        static const char *const ports[3] = { "BUS", "P1", "P2" };
        m_bpTable->setItem(i, 3, new QTableWidgetItem(port ? QString::fromLatin1(ports[bps[i].space < 0 || bps[i].space > 2 ? 0 : bps[i].space])
                                                           : QString::fromUtf8(spaceName(bps[i].space))));
        QString range = port ? QStringLiteral("—") : QStringLiteral("$") + hx(bps[i].start, 3);
        if (!port && bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, 3);
        m_bpTable->setItem(i, 4, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 5, new QTableWidgetItem(QString::number(bps[i].hits)));
        m_bpTable->setItem(i, 6, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    if (n > 0) m_bpTable->resizeColumnsToContents();
    m_bpFilling = false;
}

/* ---- FujiNet ------------------------------------------------------------------------------------- */

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = dbgui::dimLabel();
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        o2debug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView(false);
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("The cartridge's mailbox")));
    m_fujiState = monoView(false);
    rv->addWidget(m_fujiState, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static o2debug_fuji_packet p[512];
    static char state[2048];
    const int n = o2debug_fujinet_trace(m_dbg, p, 512);
    char status[128];
    o2session_cart_status(m_session, status, sizeof status);
    if (o2session_fujinet_external(m_session))
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1. Cartridge: %2.")
                                .arg(QString::fromUtf8(o2session_boip_endpoint(m_session)), QString::fromUtf8(status)));
    else if (!o2session_fujinet_running(m_session))
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences > FujiNet)."));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on. Cartridge: %1.").arg(QString::fromUtf8(status)));
    state[0] = '\0';
    o2debug_fujinet_state(m_dbg, state, sizeof state);
    setText(m_fujiState, QString::fromUtf8(state));

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        if (i) t += QLatin1Char('\n');
        t += QString::fromUtf8(p[i].text);
        if (p[i].repeat > 1) t += QStringLiteral(" ×%1").arg(p[i].repeat);
        t += QStringLiteral("\n     ");
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            t += QStringLiteral("%1 ").arg(hx(p[i].data[k], 2));
        if (p[i].nbytes > (int)sizeof p[i].data) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, t, force || atEnd(m_fujiText));
}

/* ---- refreshing ------------------------------------------------------------------------------------ */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = o2debug_is_stopped(m_dbg) != 0;
    o2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    /* the engine says "stopped" when nothing more can be said */
    const bool why = reason[0] && strcmp(reason, "stopped") != 0;
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(why ? ": " : "", why ? QString::fromUtf8(reason) : QString())
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? o2AccentFill() : QString());
}

/* The tab that shows; the others catch up when they are shown. */
void DebuggerWindow::refreshTab(int index)
{
    switch (index) {
    case kTabCpu: refreshCpu(); refreshRam(); break;
    case kTabDisasm: refreshDisasm(); break;
    case kTabVideo: m_video->refresh(); break;
    case kTabEf: m_ef->refresh(); break;
    case kTabIo: m_io->refresh(); break;
    case kTabBreaks: refreshBps(); break;
    case kTabCart: m_cart->refresh(); break;
    case kTabFujinet: refreshFujinet(false); break;
    default: break;
    }
}

void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshTab(m_tabs->currentIndex());
}

/* Every o2debug call takes the host's run lock, so the live tabs are safe to
 * refresh while the machine runs: the picture ten times a second, the rest
 * twice a second. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = o2debug_generation(m_dbg);
    const bool stopped = o2debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen;
        m_wasStopped = stopped;
        refreshAll();
        return;
    }
    if (stopped) return;
    const int tab = m_tabs->currentIndex();
    if (tab == kTabPrompt) return;
    if (tab == kTabVideo || ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshAll();
    }
}

/* ---- the prompt and the rest ----------------------------------------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = m_history.size();
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    o2debug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

/* A label, or a number as parseNum reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = o2debug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return parseNum(text, out);
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = o2debug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
        /* the history: Up for older, Down back towards an empty line */
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_promptIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                o2debug_breakpoint_toggle(m_dbg, m_lineAddr[(size_t)line]);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            const int rows = -we->angleDelta().y() / 40;
            if (rows != 0) {
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)o2debug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); return;
    case Qt::Key_F7: stepAnd(o2debug_step); return;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? o2debug_step_out : o2debug_step_over);
        return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. The close button is a plain close, which
 * only hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. */
void DebuggerWindow::showEvent(QShowEvent *e)
{
    m_timer.start(100);
    QMainWindow::showEvent(e);
}

void DebuggerWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    o2debug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}

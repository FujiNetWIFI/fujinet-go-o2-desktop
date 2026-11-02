/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 48

namespace {

QPlainTextEdit *monoView(bool editable)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

/* $0400, 0x0400, 0400h, #1024; a bare number is hex, as everywhere in an
 * 1802 debugger. */
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

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QString yesNo(int v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); }

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

/* The order of the tabs, and their names for O2_DEBUGGER_TAB. */
const char *const kTabNames[] = { "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet" };
const int kTabVideo = 3;
const int kTabFujinet = 7;   /* refreshed live only while it shows */

QPointer<DebuggerWindow> s_win;

QFont monoFont(const QFont &base)
{
    QFont f = base;
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    return f;
}

/* What the cartridge serves in a page (S2PG_*: none, image, hotspot,
 * raster), as a colour and a name. */
const char *const kPageNames[4] = { "none", "image ROM", "hotspot", "raster (DMA)" };
QColor pageColour(int type, const QPalette &pal)
{
    switch (type) {
    case 1: return o2AccentColor();
    case 2: return QColor(0xC0, 0x50, 0x4D);
    case 3: return QColor(0x4F, 0x81, 0xBD);
    default: return pal.color(QPalette::Base);
    }
}

} // namespace

/* ---- the painted panels ------------------------------------------------------ */

/* One keypad as the console sees it, laid out as on the console (1 2 3 /
 * 4 5 6 / 7 8 9 / 0 under 8): the held keys filled in the accent colour,
 * the key OUT 2 has latched outlined. */
class KeypadDiagram : public QWidget {
public:
    explicit KeypadDiagram(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(3 * kCell, 4 * kCell);
    }
    void set(unsigned held, int latch)
    {
        if (held == m_held && latch == m_latch) return;
        m_held = held;
        m_latch = latch;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        static const int keys[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, 0, -1 };
        QPainter p(this);
        for (int i = 0; i < 12; ++i) {
            const int k = keys[i];
            if (k < 0) continue;
            const QRect r((i % 3) * kCell + 1, (i / 3) * kCell + 1, kCell - 4, kCell - 4);
            const bool on = (m_held >> k) & 1u;
            p.fillRect(r, on ? o2AccentColor() : palette().color(QPalette::Base));
            p.setPen(k == m_latch ? QPen(o2AccentTextColor(this), 2) : QPen(palette().color(QPalette::Mid)));
            p.drawRect(r);
            /* the tan is pale: dark text on it, whatever the theme */
            p.setPen(on ? QColor(Qt::black) : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, QString::number(k));
        }
    }

private:
    static constexpr int kCell = 30;
    unsigned m_held = 0;
    int m_latch = -1;
};

/* The 256 pages of the address space, 4K to a row, coloured by what the
 * cartridge serves there; hover a cell for its range. */
class PageMapWidget : public QWidget {
public:
    explicit PageMapWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFont(monoFont(font()));
        const QFontMetrics fm(font());
        m_label = fm.horizontalAdvance(QStringLiteral("$F000 "));
        m_cw = qMax(16, fm.horizontalAdvance(QLatin1Char('0')) + 8);
        m_ch = qMax(14, fm.height());
        m_head = fm.height() + 2;
        int legend = 0;
        for (const char *n : kPageNames) legend += m_ch + 4 + fm.horizontalAdvance(QString::fromUtf8(n)) + 12;
        setFixedSize(qMax(m_label + 16 * m_cw + 1, legend), m_head + 16 * m_ch + 1 + 8 + m_ch);
    }
    void set(const uint8_t types[256])
    {
        if (!memcmp(types, m_types, sizeof m_types)) return;
        memcpy(m_types, types, sizeof m_types);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette &pal = palette();
        p.setPen(pal.color(QPalette::Text));
        for (int col = 0; col < 16; ++col)
            p.drawText(QRect(m_label + col * m_cw, 0, m_cw, m_head), Qt::AlignCenter, hx(col, 1));
        for (int row = 0; row < 16; ++row) {
            p.setPen(pal.color(QPalette::Text));
            p.drawText(QRect(0, m_head + row * m_ch, m_label, m_ch), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("$%1").arg(hx(row * 0x1000, 4)));
            for (int col = 0; col < 16; ++col)
                p.fillRect(m_label + col * m_cw, m_head + row * m_ch, m_cw, m_ch,
                           pageColour(m_types[row * 16 + col], pal));
        }
        p.setPen(pal.color(QPalette::Mid));
        for (int i = 0; i <= 16; ++i) {
            p.drawLine(m_label + i * m_cw, m_head, m_label + i * m_cw, m_head + 16 * m_ch);
            p.drawLine(m_label, m_head + i * m_ch, m_label + 16 * m_cw, m_head + i * m_ch);
        }
        /* the legend */
        int x = 0;
        const int y = m_head + 16 * m_ch + 8;
        for (int t = 0; t < 4; ++t) {
            const QString name = QString::fromUtf8(kPageNames[t]);
            p.fillRect(x, y, m_ch, m_ch, pageColour(t, pal));
            p.setPen(pal.color(QPalette::Mid));
            p.drawRect(x, y, m_ch, m_ch);
            p.setPen(pal.color(QPalette::Text));
            const int w = QFontMetrics(font()).horizontalAdvance(name);
            p.drawText(QRect(x + m_ch + 4, y, w + 2, m_ch), Qt::AlignLeft | Qt::AlignVCenter, name);
            x += m_ch + 4 + w + 12;
        }
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int col = (he->pos().x() - m_label) / m_cw, row = (he->pos().y() - m_head) / m_ch;
            if (he->pos().x() >= m_label && he->pos().y() >= m_head && col < 16 && row < 16) {
                const int page = row * 16 + col;
                QToolTip::showText(he->globalPos(), QStringLiteral("$%1-$%2: %3")
                                       .arg(hx(page << 8, 4), hx((page << 8) | 0xFF, 4),
                                            QString::fromUtf8(kPageNames[m_types[page] & 3])), this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    uint8_t m_types[256] = {};
    int m_label = 0, m_cw = 0, m_ch = 0, m_head = 0;
};

/* The cartridge's text engine as it has drawn: ten rows of sixteen cells,
 * white on black as on the television, the inverse cells inverted and the
 * cursor outlined in the accent colour. */
class TextGridWidget : public QWidget {
public:
    explicit TextGridWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFont(monoFont(font()));
        const QFontMetrics fm(font());
        m_cw = fm.horizontalAdvance(QLatin1Char('M')) + 6;
        m_ch = fm.height() + 2;
        setFixedSize(16 * m_cw + 8, 10 * m_ch + 8);
    }
    void set(const o2debug_cart &c)
    {
        if (!memcmp(c.text, m_text, sizeof m_text) && !memcmp(c.text_inv, m_inv, sizeof m_inv)
            && c.cursor_row == m_row && c.cursor_col == m_col && c.present == m_present)
            return;
        memcpy(m_text, c.text, sizeof m_text);
        memcpy(m_inv, c.text_inv, sizeof m_inv);
        m_row = c.cursor_row;
        m_col = c.cursor_col;
        m_present = c.present;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!m_present) return;
        for (int r = 0; r < 10; ++r) {
            for (int c = 0; c < 16; ++c) {
                const QRect cell(4 + c * m_cw, 4 + r * m_ch, m_cw, m_ch);
                const bool inv = m_inv[r][c] != 0;
                if (inv) p.fillRect(cell, Qt::white);
                p.setPen(inv ? Qt::black : Qt::white);
                p.drawText(cell, Qt::AlignCenter, glyph(m_text[r][c]));
            }
        }
        if (m_row >= 0 && m_row < 10 && m_col >= 0 && m_col < 16) {
            p.setPen(QPen(o2AccentColor(), 2));
            p.drawRect(QRect(4 + m_col * m_cw, 4 + m_row * m_ch, m_cw, m_ch).adjusted(1, 1, -1, -1));
        }
    }

private:
    /* The engine's character set: $20-$7F its font, $01-$0B its symbols,
     * $80-$9F glyphs the app defined, $A0 a mosaic cell. */
    static QString glyph(uint8_t ch)
    {
        static const char16_t symbols[12] = { u' ', u'♠', u'♥', u'♦', u'♣', u'█', u'→',
                                              u'←', u'↑', u'↓', u'⑩', u'▒' };
        if (ch >= 0x20 && ch < 0x7F) return QString(QChar(ch));
        if (ch >= 0x01 && ch <= 0x0B) return QString(QChar(symbols[ch]));
        if (ch >= 0x80 && ch <= 0x9F) return QStringLiteral("□");
        if (ch == 0xA0) return QStringLiteral("▚");
        return QString();
    }

    uint8_t m_text[10][16] = {};
    uint8_t m_inv[10][16] = {};
    int m_row = -1, m_col = -1, m_present = 0;
    int m_cw = 0, m_ch = 0;
};

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
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(o2session_debugger(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1180, 820);
    m_videoPx.resize(O2DEBUG_VIEW_MAX_PIXELS);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = m_tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    tabs->addTab(buildVideo(), QStringLiteral("Video"));
    tabs->addTab(buildIo(), QStringLiteral("Sound && I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    tabs->addTab(buildCart(), QStringLiteral("Cart"));
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
        }
        if (n >= 0 && n < tabs->count()) tabs->setCurrentIndex(n);
    }
    connect(tabs, &QTabWidget::currentChanged, this, [this](int page) {
        if (page == kTabFujinet) refreshFujinet(false);
        /* the view is scaled to the room it has, known once it shows */
        if (page == kTabVideo) refreshVideo();
    });
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    /* started on show, stopped on hide: no wakeups while the window is away */
}

/* ---- building ------------------------------------------------------------- */

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
        { "dis", "Disassembly (the BIOS and the cartridge's pages, as mapped)", "disassembly.asm" },
        { "ram", "Console RAM ($0800-$09FF)", "ram.bin" },
        { "arena", "Mailbox Arena (2K)", "arena.bin" },
        { "raster", "Cartridge Raster (64x128, 1K)", "raster.bin" },
        { "screen", "Screen (PPM)", "screen.ppm" },
        { "regs", "CPU and Video State", "registers.txt" } };
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

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Debugger prompt. Type 'help' for every command.\n"));
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, runto, disasm, video, keys, cart ...)"
                                                  " — Tab completes, Up/Down recall"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    v->addWidget(m_promptOut, 1);
    v->addWidget(m_promptIn);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    /* the sixteen scratchpad registers in two columns of eight, then R(P)
     * and the rest of the CPU's state in a third; edits go in with Enter,
     * while the machine is stopped */
    auto *regs = new QGridLayout;
    auto addReg = [&](int reg, QLabel *label, int row, int col) {
        regs->addWidget(label, row, col, Qt::AlignRight);
        auto *e = m_reg[reg] = new QLineEdit;
        e->setMaxLength(4);
        e->setMaximumWidth(64);
        connect(e, &QLineEdit::returnPressed, this, [this, reg, e] { setRegister(reg, e); });
        regs->addWidget(e, row, col + 1);
    };
    /* "RF (P,X)": the label names the registers P and X select */
    const int labelWidth = fontMetrics().horizontalAdvance(QStringLiteral("RF (P, X)")) + 4;
    for (int n = 0; n < 16; ++n) {
        m_regLabel[n] = new QLabel(QStringLiteral("R%1").arg(hx(n, 1)));
        m_regLabel[n]->setMinimumWidth(labelWidth);
        m_regLabel[n]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        addReg(O2_REG_R0 + n, m_regLabel[n], n % 8, (n / 8) * 2);
    }
    static const struct { int reg; const char *name, *tip; } others[] = {
        { O2_REG_PC, "PC", "R(P): the instruction about to run" },
        { O2_REG_D, "D", "The accumulator" },
        { O2_REG_DF, "DF", "The carry (0 or 1)" },
        { O2_REG_P, "P", "Which register is the program counter" },
        { O2_REG_X, "X", "Which register is the data pointer" },
        { O2_REG_T, "T", "X and P as an interrupt saved them" },
        { O2_REG_B, "B", "The auxiliary register" },
        { O2_REG_IE, "IE", "Interrupts enabled (0 or 1)" },
        { O2_REG_Q, "Q", "The Q output: the Odyssey 2's beeper (0 or 1)" },
    };
    for (int i = 0; i < (int)(sizeof others / sizeof others[0]); ++i) {
        auto *l = new QLabel(QString::fromUtf8(others[i].name));
        l->setToolTip(QString::fromUtf8(others[i].tip));
        addReg(others[i].reg, l, i, 4);
    }
    regs->setColumnMinimumWidth(4, 40);
    regs->setColumnStretch(6, 1);
    v->addLayout(regs);
    /* the four external flag inputs, read-only: on the Odyssey 2 EF1 is the
     * 1861's display-area signal, EF3 and EF4 the keypads */
    auto *flags = new QHBoxLayout;
    for (int i = 0; i < 4; ++i) {
        m_ef[i] = new QCheckBox(QStringLiteral("EF%1").arg(i + 1));
        m_ef[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_ef[i]->setFocusPolicy(Qt::NoFocus);
        m_ef[i]->setToolTip(QStringLiteral("An input: what the CPU reads on EF%1 now").arg(i + 1));
        flags->addWidget(m_ef[i]);
    }
    flags->addStretch();
    v->addLayout(flags);
    m_cpuInfo = new QLabel;
    m_cpuInfo->setStyleSheet(QStringLiteral("color: gray;"));
    m_cpuInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_cpuInfo->setWordWrap(true);
    v->addWidget(m_cpuInfo);

    auto *head = new QHBoxLayout;
    head->addWidget(new QLabel(QStringLiteral("Console RAM ($0800-$09FF)")));
    head->addStretch();
    head->addWidget(new QLabel(QStringLiteral("Go to")));
    m_ramGoto = new QLineEdit;
    m_ramGoto->setPlaceholderText(QStringLiteral("$0900 or label"));
    m_ramGoto->setMaximumWidth(140);
    connect(m_ramGoto, &QLineEdit::returnPressed, this, [this] {
        long a;
        if (!addressOf(m_ramGoto->text(), &a) || a < 0x0800 || a > 0x09FF) {
            statusBar()->showMessage(QStringLiteral("Not a RAM address ($0800-$09FF)"), 4000);
            return;
        }
        /* line 0 is the column header */
        m_ram->verticalScrollBar()->setValue(1 + (int)((a - 0x0800) / 16));
    });
    head->addWidget(m_ramGoto);
    v->addLayout(head);
    m_ram = monoView(false);
    v->addWidget(m_ram, 1);
    auto *edit = new QHBoxLayout;
    m_ramAddr = new QLineEdit; m_ramAddr->setPlaceholderText(QStringLiteral("$0800")); m_ramAddr->setMaximumWidth(140);
    m_ramVal = new QLineEdit; m_ramVal->setPlaceholderText(QStringLiteral("$00")); m_ramVal->setMaximumWidth(60);
    auto write = [this] {
        long a, val;
        if (!addressOf(m_ramAddr->text(), &a) || !parseNum(m_ramVal->text(), &val)) {
            statusBar()->showMessage(QStringLiteral("Bad address or value"), 4000);
            return;
        }
        o2debug_write(m_dbg, (uint16_t)a, (uint8_t)val);
        refreshAll();
    };
    connect(m_ramVal, &QLineEdit::returnPressed, this, write);
    connect(m_ramAddr, &QLineEdit::returnPressed, this, write);
    edit->addWidget(new QLabel(QStringLiteral("Poke address (RAM, or a page the cartridge serves)")));
    edit->addWidget(m_ramAddr);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_ramVal);
    edit->addStretch();
    v->addLayout(edit);
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
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
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

QWidget *DebuggerWindow::buildVideo()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_videoText = monoView(false);
    h->addWidget(m_videoText, 1);

    auto *right = new QVBoxLayout;
    auto *sel = new QHBoxLayout;
    m_videoView = new QComboBox;
    m_videoView->addItems({ QStringLiteral("Screen (112×262, the whole raster)"),
                            QStringLiteral("Cart raster (64×128)"),
                            QStringLiteral("BIOS display page (64×32, $0900)") });
    connect(m_videoView, &QComboBox::currentIndexChanged, this, [this](int) { refreshVideo(); });
    sel->addWidget(m_videoView);
    sel->addStretch();
    right->addLayout(sel);
    m_videoNote = new QLabel;
    m_videoNote->setStyleSheet(QStringLiteral("color: gray;"));
    m_videoNote->setWordWrap(true);
    right->addWidget(m_videoNote);
    m_videoPic = new QLabel;
    m_videoPic->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto *scroll = new QScrollArea;
    scroll->setWidget(m_videoPic);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(540, 540);
    right->addWidget(scroll, 1);
    h->addLayout(right);
    return w;
}

/* The state as text on the left; the two keypads as the console sees them
 * on the right. */
QWidget *DebuggerWindow::buildIo()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_io = monoView(false);
    h->addWidget(m_io, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Keypads as the console sees them")));
    auto *pads = new QHBoxLayout;
    for (int k = 0; k < 2; ++k) {
        auto *col = new QVBoxLayout;
        auto *name = new QLabel(k ? QStringLiteral("Keypad B (EF4)") : QStringLiteral("Keypad A (EF3)"));
        name->setAlignment(Qt::AlignHCenter);
        col->addWidget(name);
        m_keypad[k] = new KeypadDiagram;
        col->addWidget(m_keypad[k], 0, Qt::AlignHCenter);
        pads->addLayout(col);
    }
    right->addLayout(pads);
    auto *legend = new QLabel(QStringLiteral("Filled: held. Outlined: the key OUT 2 has latched."));
    legend->setStyleSheet(QStringLiteral("color: gray;"));
    legend->setWordWrap(true);
    right->addWidget(legend);
    right->addStretch();
    h->addLayout(right);
    return w;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Exec"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("In"), QStringLiteral("Out") });
    m_bpType->setToolTip(QStringLiteral("In and Out take a port (1-7)"));
    m_bpStart = new QLineEdit;
    m_bpStart->setPlaceholderText(QStringLiteral("start ($0400 or label)"));
    m_bpStart->setMinimumWidth(m_bpStart->fontMetrics().horizontalAdvance(m_bpStart->placeholderText()) + 16);
    m_bpStart->setMaximumWidth(200);
    m_bpEnd = new QLineEdit;
    m_bpEnd->setPlaceholderText(QStringLiteral("end (optional)"));
    m_bpEnd->setMaximumWidth(140);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. d == $FF or value & $80)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[5] = { O2DEBUG_BP_EXEC, O2DEBUG_BP_READ, O2DEBUG_BP_WRITE,
                                      O2DEBUG_BP_IN, O2DEBUG_BP_OUT };
        if (m_bpStart->text().trimmed().isEmpty()) return;
        long a, b;
        if (!addressOf(m_bpStart->text(), &a)) { statusBar()->showMessage(QStringLiteral("Bad start address"), 4000); return; }
        b = a;
        if (!m_bpEnd->text().trimmed().isEmpty() && !addressOf(m_bpEnd->text(), &b)) {
            statusBar()->showMessage(QStringLiteral("Bad end address"), 4000);
            return;
        }
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (o2debug_breakpoint_add(m_dbg, types[m_bpType->currentIndex()], (uint16_t)a, (uint16_t)b,
                                    cond.constData()) < 0) {
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
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Range"), QStringLiteral("Hits"),
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

/* The cartridge's state as text on the left; its page map and its text
 * engine's screen on the right. */
QWidget *DebuggerWindow::buildCart()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_cart = monoView(false);
    h->addWidget(m_cart, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(titleLabel(QStringLiteral("Pages the cartridge serves")));
    m_pageMap = new PageMapWidget;
    right->addWidget(m_pageMap);
    right->addSpacing(12);
    right->addWidget(titleLabel(QStringLiteral("Text engine")));
    m_textGrid = new TextGridWidget;
    right->addWidget(m_textGrid);
    m_textNote = new QLabel;
    m_textNote->setStyleSheet(QStringLiteral("color: gray;"));
    right->addWidget(m_textNote);
    right->addStretch();
    h->addLayout(right);
    return w;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = new QLabel;
    m_fujiNote->setStyleSheet(QStringLiteral("color: gray;"));
    m_fujiNote->setWordWrap(true);
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

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = o2debug_is_stopped(m_dbg) != 0;
    o2debug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(reason[0] ? ": " : "", QString::fromUtf8(reason))
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    /* the tan is pale: dark text on it, whatever the theme */
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: black;").arg(o2AccentColor().name()) : QString());
}

void DebuggerWindow::refreshCpu()
{
    o2debug_cpu c;
    o2debug_cpu_get(m_dbg, &c);
    for (int reg = 0; reg < O2_REG_COUNT; ++reg) {
        QLineEdit *e = m_reg[reg];
        if (!e || e->hasFocus()) continue;
        int v;
        /* the 16-bit registers as four digits, P and X as one, D, B and T
         * as two, the flags as 0 or 1 */
        switch (reg) {
        case O2_REG_PC: e->setText(hx((unsigned)c.pc & 0xFFFF, 4)); continue;
        case O2_REG_P: e->setText(hx((unsigned)c.p & 0xF, 1)); continue;
        case O2_REG_X: e->setText(hx((unsigned)c.x & 0xF, 1)); continue;
        case O2_REG_D: v = c.d; break;
        case O2_REG_B: v = c.b; break;
        case O2_REG_T: v = c.t; break;
        case O2_REG_DF: e->setText(c.df ? QStringLiteral("1") : QStringLiteral("0")); continue;
        case O2_REG_IE: e->setText(c.ie ? QStringLiteral("1") : QStringLiteral("0")); continue;
        case O2_REG_Q: e->setText(c.q ? QStringLiteral("1") : QStringLiteral("0")); continue;
        default: e->setText(hx((unsigned)c.r[reg - O2_REG_R0] & 0xFFFF, 4)); continue;
        }
        e->setText(hx((unsigned)v & 0xFF, 2));
    }
    for (int n = 0; n < 16; ++n) {
        QString name = QStringLiteral("R%1").arg(hx(n, 1));
        const bool p = (c.p & 0xF) == n, x = (c.x & 0xF) == n;
        if (p && x) name += QStringLiteral(" (P, X)");
        else if (p) name += QStringLiteral(" (P)");
        else if (x) name += QStringLiteral(" (X)");
        m_regLabel[n]->setText(name);
    }
    for (int i = 0; i < 4; ++i) m_ef[i]->setChecked(c.ef[i] != 0);
    m_cpuInfo->setText(QStringLiteral("state %1   mode %2   INT %3   DMA OUT %4   cycles %5   instructions %6   "
                                      "beam line %7 dot %8   frame %9")
                           .arg(QString::fromUtf8(c.state_name ? c.state_name : "?"),
                                QString::fromUtf8(c.mode_name ? c.mode_name : "?"))
                           .arg(c.int_line).arg(c.dma_line)
                           .arg((qulonglong)c.cycles).arg((qulonglong)c.instructions)
                           .arg(c.vpos).arg(c.hpos).arg(c.frame));
}

void DebuggerWindow::refreshRam()
{
    static uint8_t ram[512];
    static char buf[64 + 32 * 64];
    o2debug_ram_get(m_dbg, ram);
    int n = snprintf(buf, sizeof buf, "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int row = 0; row < 32; ++row) {
        n += snprintf(buf + n, sizeof buf - n, "$%04X: ", 0x0800 + row * 16);
        for (int col = 0; col < 16; ++col)
            n += snprintf(buf + n, sizeof buf - n, "%02X ", ram[row * 16 + col]);
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    const int scroll = m_ram->verticalScrollBar()->value();
    m_ram->setPlainText(QString::fromLatin1(buf, n));
    m_ram->verticalScrollBar()->setValue(scroll);
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
        snprintf(buf, sizeof buf, " %c %04X  %-11s %-*s %-22s", lines[i].is_pc ? '>' : ' ', lines[i].address,
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
            /* the tan is pale: dark text on it, whatever the theme */
            fmt.setBackground(o2AccentColor());
            fmt.setForeground(Qt::black);
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

void DebuggerWindow::refreshVideo()
{
    o2debug_video v;
    o2debug_video_get(m_dbg, &v);
    QString s;
    s += QStringLiteral("CDP1861       display %1\n").arg(v.display_on ? "on" : "off");
    s += QStringLiteral("Lines         INT %1   EFx %2   DMA OUT %3\n").arg(v.int_line).arg(v.efx_line).arg(v.dma_line);
    s += QStringLiteral("Beam          line %1   dot %2   %3\n").arg(v.vpos).arg(v.hpos)
        .arg(v.in_display ? QStringLiteral("(display lines 80-207)") : QStringLiteral("(blanking)"));
    s += QStringLiteral("Frame         %1\n\n").arg(v.frame);
    s += QStringLiteral("DMA pointer   R0 = $%1   last fetch $%2\n").arg(hx(v.dma_pointer, 4), hx(v.last_dma_addr, 4));
    s += QStringLiteral("DMA bytes     %1 since power-on\n").arg((qulonglong)v.dma_bytes);
    s += QStringLiteral("Short bursts  %1 (under 8 bytes: a $Cx opcode in flight)\n").arg(v.short_lines);
    s += QStringLiteral("INP 1         %1 display-on strobes\n\n").arg(v.inp1_count);
    s += QStringLiteral("Next INT      line %1   clock %2\n").arg(v.next_int_line).arg((qulonglong)v.next_int_clock);
    s += QStringLiteral("Next EFx      line %1   clock %2\n").arg(v.next_efx_line).arg((qulonglong)v.next_efx_clock);
    s += QStringLiteral("Next DMA      clock %1\n").arg((qulonglong)v.next_dma_clock);
    setText(m_videoText, s);

    static const char *const notes[3] = {
        "The whole raster the 1861 scans, blanking dimmed, the visible area outlined and the beam marked.",
        "The cartridge's 64×128 raster: what a FujiNet app draws through the text engine, on screen or not.",
        "The BIOS's display page, $0900-$09FF, as the BIOS draws it: four lines a row.",
    };
    const int view = m_videoView->currentIndex();
    m_videoNote->setText(QString::fromUtf8(notes[view < 0 || view > 2 ? 0 : view]));
    int w = 0, h = 0;
    if (o2debug_video_view(m_dbg, view, m_videoPx.data(), &w, &h) && w > 0 && h > 0) {
        QImage img(reinterpret_cast<const uchar *>(m_videoPx.data()), w, h, w * 4, QImage::Format_RGB32);
        /* nearest-neighbour, a whole number of times, as large as the view
         * holds (and never under 560 pixels' worth) */
        const QSize room = m_videoPic->parentWidget()->size().expandedTo(QSize(560, 560));
        const int scale = qMax(1, qMin(room.width() / w, room.height() / h));
        m_videoPic->setPixmap(QPixmap::fromImage(img.scaled(w * scale, h * scale, Qt::IgnoreAspectRatio,
                                                            Qt::FastTransformation)));
    }
}

void DebuggerWindow::refreshIo()
{
    o2debug_io a;
    o2debug_io_get(m_dbg, &a);
    QString s;
    s += QStringLiteral("Beeper        Q = %1   %2\n").arg(a.q)
        .arg(a.q ? QStringLiteral("sounding, %1 Hz").arg(a.beep_hz) : QStringLiteral("silent"));
    s += QStringLiteral("Key latch     %1 (OUT 2)\n").arg(a.keylatch);
    s += QStringLiteral("EF3           %1   (keypad A, the latched key)\n").arg(a.ef3);
    s += QStringLiteral("EF4           %1   (keypad B, the latched key)\n").arg(a.ef4);
    s += QStringLiteral("CLEAR         %1\n\n").arg(a.clear_held ? "held" : "up");
    for (int k = 0; k < 2; ++k) {
        QString keys;
        for (int i = 0; i < 10; ++i)
            if (a.keys[k] & (1u << i)) keys += QStringLiteral(" %1").arg(i);
        s += QStringLiteral("Keypad %1      %2\n").arg(QChar(k ? 'B' : 'A'))
            .arg(keys.isEmpty() ? QStringLiteral("(none held)") : keys.mid(1));
    }
    s += QStringLiteral("\nLast OUT     ");
    for (int port = 1; port < 8; ++port) s += QStringLiteral(" %1=$%2").arg(port).arg(hx(a.last_out[port], 2));
    s += QStringLiteral("\nINP 1         %1 (display on)\n\n").arg(a.inp1_count);
    s += QStringLiteral("BIOS          %1\n")
        .arg(a.have_bios ? QStringLiteral("imported")
                         : a.stub_parked ? QStringLiteral("the stand-in, parked: a game needs the BIOS")
                                         : QStringLiteral("the stand-in (CONFIG and FujiNet apps)"));
    setText(m_io, s);
    const int latch = a.keylatch < 10 ? a.keylatch : -1;
    for (int k = 0; k < 2; ++k) m_keypad[k]->set(a.keys[k], latch);
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
        /* a port range for In/Out (1-7) is one digit */
        const int digits = (bps[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) ? 1 : 4;
        QString range = QStringLiteral("$") + hx(bps[i].start, digits);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, digits);
        m_bpTable->setItem(i, 3, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 4, new QTableWidgetItem(QString::number(bps[i].hits)));
        m_bpTable->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    if (n > 0) m_bpTable->resizeColumnsToContents();
    m_bpFilling = false;
}

void DebuggerWindow::refreshCart()
{
    o2debug_cart c;
    char info[256];
    o2debug_cart_get(m_dbg, &c);
    o2debug_cart_info(m_dbg, info, sizeof info);
    m_pageMap->set(c.page_type);
    m_textGrid->set(c);
    QString s = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) {
        m_textNote->clear();
        setText(m_cart, s);
        return;
    }
    m_textNote->setText(QStringLiteral("cursor row %1 column %2%3").arg(c.cursor_row).arg(c.cursor_col)
                            .arg(c.inverse ? QStringLiteral(", drawing inverse") : QString()));
    s += QStringLiteral("Mode          %1 (%2)%3\n").arg(QString::fromUtf8(c.mode_name ? c.mode_name : "?")).arg(c.mode)
        .arg(c.direct ? QStringLiteral(", an opened cartridge file") : QString());
    s += QStringLiteral("Booted game   %1\n").arg(yesNo(c.booted_game));
    s += QStringLiteral("Mailbox mode  %1\n").arg(c.mailbox_mode ? QStringLiteral("yes (a socket and fujimail)")
                                                                 : QStringLiteral("no (a game alone)"));
    s += QStringLiteral("Link          %1%2\n\n").arg(c.link_up ? "up" : "down",
                                                     c.busy ? QStringLiteral(" (transaction in flight)") : QString());
    s += QStringLiteral("Live image    %1, %2 bytes, %3 blocks\n              CRC-32 %4%5\n")
        .arg(c.live_st2 ? QStringLiteral("ST2") : QStringLiteral("raw")).arg(c.live_size).arg(c.live_blocks)
        .arg(hx(c.live_crc, 8), c.live_mailbox ? QStringLiteral(", claims the mailbox") : QString());
    s += QStringLiteral("Staged        %1%2").arg(c.staged ? (c.staged_config ? QStringLiteral("CONFIG")
                                                                 : c.staged_claim ? QStringLiteral("an app")
                                                                                  : QStringLiteral("a game"))
                                                           : QStringLiteral("nothing"),
                                                  c.armed ? QStringLiteral(" (armed)") : QString());
    if (c.staged) s += QStringLiteral(", CRC-32 %1").arg(hx(c.staged_crc, 8));
    s += QStringLiteral("   swaps %1\n\n").arg(c.swaps);
    s += QStringLiteral("Mailbox       ACKSEQ $%1   STATUS $%2   last error %3\n              reply cmd $%4   RXLEN %5\n")
        .arg(hx(c.ackseq, 2), hx(c.status, 2)).arg(c.last_error).arg(hx(c.reply_cmd, 2)).arg(c.rxlen);
    s += QStringLiteral("Boot          state %1   %2%   error %3   %4 of %5 bytes\n")
        .arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err).arg(c.boot_got).arg(c.boot_total);
    s += QStringLiteral("Raster        %1 frames served (mod 256)   short bursts %2\n").arg(c.frames).arg(c.short_lines);
    s += QStringLiteral("Queue depth   %1\n").arg(c.queue_depth);
    setText(m_cart, s);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
void DebuggerWindow::refreshFujinet(bool force)
{
    static o2debug_fuji_packet p[512];
    static char state[2048];
    const int n = o2debug_fujinet_trace(m_dbg, p, 512);
    if (o2session_fujinet_external(m_session)) {
        char status[128];
        o2session_cart_status(m_session, status, sizeof status);
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1. Cartridge: %2.")
                                .arg(QString::fromUtf8(o2session_boip_endpoint(m_session)), QString::fromUtf8(status)));
    } else if (!o2session_fujinet_running(m_session)) {
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)."));
    } else {
        char status[128];
        o2session_cart_status(m_session, status, sizeof status);
        m_fujiNote->setText(QStringLiteral("FujiNet is on. Cartridge: %1.").arg(QString::fromUtf8(status)));
    }
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

void DebuggerWindow::refreshAll()
{
    refreshStatus(); refreshCpu(); refreshRam(); refreshDisasm(); refreshVideo(); refreshIo();
    refreshBps(); refreshCart(); refreshFujinet(false);
}

/* Every o2debug call takes the host's run lock, so the live tabs are safe
 * to refresh while the machine runs: twice a second. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = o2debug_generation(m_dbg);
    const bool stopped = o2debug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen; m_wasStopped = stopped;
        refreshAll();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(); refreshCpu(); refreshRam(); refreshVideo(); refreshIo(); refreshCart();
        if (m_tabs->currentIndex() == kTabFujinet) refreshFujinet(false);
    }
}

/* ---- the prompt and the rest ----------------------------------------------- */

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

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    if (!addressOf(text, &a)) { statusBar()->showMessage(QStringLiteral("No such address or label"), 4000); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)o2debug_row_address(m_dbg, (uint16_t)(a & 0xFFFF), -(DISASM_WINDOW / 3));
    refreshDisasm();
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
                o2debug_breakpoint_toggle(m_dbg, m_lineAddr[line]);
                refreshDisasm();
                refreshBps();
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

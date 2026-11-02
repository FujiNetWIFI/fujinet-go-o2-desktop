/*
 * ChipTabs -- see ChipTabs.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ChipTabs.h"

#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QToolTip>
#include <QVBoxLayout>
#include <cstring>
#include <vector>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;
using dbgui::yesNo;

namespace {

/* The text chip's colour: bit 0 red, bit 1 green, bit 2 blue. */
QColor chipColour(int c) { return QColor((c & 1) ? 255 : 0, (c & 2) ? 255 : 0, (c & 4) ? 255 : 0); }

} // namespace

/* ---- EF9340 ------------------------------------------------------------------------ */

/* The text page as the chip holds it: 40 columns by 24 rows under the service
 * row, each cell's character in its colours (graphics cells and delimiters
 * marked); hover a cell for its attribute and code. */
class EfPageWidget : public QWidget {
public:
    EfPageWidget()
    {
        setFont(dbgui::monoFont(font()));
        const QFontMetrics fm(font());
        m_cw = fm.horizontalAdvance(QLatin1Char('M')) + 2;
        m_ch = fm.height() + 2;
        setFixedSize(EF9340_COLUMNS * m_cw + 8, (EF9340_ROWS + 1) * m_ch + 8 + 6);
        setMouseTracking(true);
    }
    void set(const ef9340_snapshot_t &s)
    {
        memcpy(m_page, s.page, sizeof m_page);
        memcpy(m_service, s.service, sizeof m_service);
        m_cursorX = s.cursor_visible ? s.x : -1;
        m_cursorY = s.cursor_visible ? s.y : -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        for (int x = 0; x < EF9340_COLUMNS; ++x) paintCell(p, m_service[x], x, 0);
        p.setPen(QColor(0x40, 0x40, 0x40));
        p.drawLine(4, 4 + m_ch + 3, width() - 4, 4 + m_ch + 3);
        for (int y = 0; y < EF9340_ROWS; ++y)
            for (int x = 0; x < EF9340_COLUMNS; ++x) paintCell(p, m_page[y][x], x, y + 1);
        if (m_cursorX >= 0 && m_cursorX < EF9340_COLUMNS && m_cursorY >= 0 && m_cursorY < EF9340_ROWS) {
            p.setPen(QPen(o2AccentColor(), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(cellRect(m_cursorX, m_cursorY + 1).adjusted(1, 1, -1, -1));
        }
    }
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            int cx = -1, row = -1;
            for (int r = 0; r <= EF9340_ROWS && cx < 0; ++r)
                for (int x = 0; x < EF9340_COLUMNS; ++x)
                    if (cellRect(x, r).contains(he->pos())) {
                        cx = x;
                        row = r;
                        break;
                    }
            if (cx < 0) {
                QToolTip::hideText();
                return true;
            }
            const ef9340_cell_t &c = row == 0 ? m_service[cx] : m_page[row - 1][cx];
            static const char *const kinds[4] = { "alphanumeric", "graphics", "delimiter", "illegal graphics" };
            QToolTip::showText(he->globalPos(),
                               QStringLiteral("%1 column %2: attribute $%3, code $%4, %5, foreground %6, background %7")
                                   .arg(row == 0 ? QStringLiteral("service row") : QStringLiteral("row %1").arg(row - 1))
                                   .arg(cx).arg(hx(c.a, 2), hx(c.b, 2), QString::fromLatin1(kinds[c.kind & 3]))
                                   .arg((int)c.fg).arg((int)c.bg), this);
            return true;
        }
        return QWidget::event(e);
    }

private:
    QRect cellRect(int x, int row) const
    {
        /* the service row sits above the page, with a gap */
        return QRect(4 + x * m_cw, 4 + row * m_ch + (row > 0 ? 6 : 0), m_cw, m_ch);
    }
    void paintCell(QPainter &p, const ef9340_cell_t &c, int x, int row)
    {
        const QRect r = cellRect(x, row);
        QColor fg = chipColour(c.fg), bg = chipColour(c.bg);
        QString glyph;
        switch (c.kind) {
        case EF9340_CELL_GRAPHIC:
            glyph = QStringLiteral("▚");
            break;
        case EF9340_CELL_DELIMITER:
            fg = QColor(0x60, 0x60, 0x60);
            bg = Qt::black;
            glyph = QStringLiteral("·");
            break;
        case EF9340_CELL_ILLEGAL:
            glyph = QStringLiteral("█");
            break;
        default:
            bg = Qt::black;
            if (c.flags & EF9340_ATTR_EXTENDED) glyph = QStringLiteral("▣");
            else if (c.b > 0x20 && c.b < 0x7F) glyph = QString(QChar(c.b));
            if (c.flags & EF9340_ATTR_INVERTED) std::swap(fg, bg);
            break;
        }
        if (c.kind == EF9340_CELL_ALNUM && fg == Qt::black && !(c.flags & EF9340_ATTR_INVERTED)) fg = QColor(0x50, 0x50, 0x50);
        if (bg != QColor(Qt::black)) p.fillRect(r, bg);
        if (glyph.isEmpty()) return;
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, glyph);
    }

    ef9340_cell_t m_page[EF9340_ROWS][EF9340_COLUMNS] = {};
    ef9340_cell_t m_service[EF9340_COLUMNS] = {};
    int m_cw = 0, m_ch = 0, m_cursorX = -1, m_cursorY = -1;
};

EfTab::EfTab(o2debug *debugger, QWidget *parent)
    : QWidget(parent), m_dbg(debugger), m_snap(new ef9340_snapshot_t)
{
    memset(m_snap.get(), 0, sizeof *m_snap);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget;
    outer->addWidget(m_stack);

    auto *absent = new QWidget;
    auto *av = new QVBoxLayout(absent);
    auto *msg = new QLabel(QStringLiteral("Videopac+ G7400 only"));
    QFont f = msg->font();
    f.setPointSizeF(f.pointSizeF() * 1.6);
    msg->setFont(f);
    msg->setAlignment(Qt::AlignCenter);
    auto *hint = dbgui::dimLabel(QStringLiteral("The EF9340/EF9341 text chip is in the Videopac+ G7400 only. "
                                                "Choose it in Preferences > Machine."));
    hint->setAlignment(Qt::AlignCenter);
    av->addStretch();
    av->addWidget(msg);
    av->addWidget(hint);
    av->addStretch();
    m_stack->addWidget(absent);

    auto *present = new QWidget;
    auto *h = new QHBoxLayout(present);
    auto *left = new QVBoxLayout;
    left->addWidget(dbgui::titleLabel(QStringLiteral("Registers")));
    m_text = dbgui::monoView();
    left->addWidget(m_text, 1);
    left->addWidget(dbgui::titleLabel(QStringLiteral("The text layer, rendered alone")));
    m_layer = new QLabel;
    m_layer->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_layer->setMinimumSize(360, 243);
    left->addWidget(m_layer);
    h->addLayout(left, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(dbgui::titleLabel(QStringLiteral("The text page: service row, then 24 rows of 40")));
    m_page = new EfPageWidget;
    right->addWidget(m_page);
    m_note = dbgui::dimLabel();
    right->addWidget(m_note);
    right->addStretch();
    h->addLayout(right);
    m_stack->addWidget(present);
}

void EfTab::refresh()
{
    if (!o2debug_ef_present(m_dbg)) {
        m_stack->setCurrentIndex(0);
        return;
    }
    m_stack->setCurrentIndex(1);
    o2debug_ef_get(m_dbg, m_snap.get());
    const ef9340_snapshot_t &s = *m_snap;
    QString t;
    t += QStringLiteral("EF9341 (the CPU's side)\n  TA $%1   TB $%2   %3\n\n")
             .arg(hx(s.ta, 2), hx(s.tb, 2), s.busy ? "busy" : "ready");
    t += QStringLiteral("EF9340 (the video chip)\n");
    t += QStringLiteral("  position   X %1  Y %2\n").arg((int)s.x).arg((int)s.y);
    t += QStringLiteral("  R   $%1\n").arg(hx(s.r, 2));
    t += QStringLiteral("      display %1, boxed %2, conceal %3, service row %4,\n"
                        "      cursor %5, %6 lines, blink %7\n")
             .arg(s.display_on ? "on" : "off", yesNo(s.box_mode), s.conceal_enabled ? "on" : "off",
                  s.service_row ? "shown" : "off", s.cursor_on ? "on" : "off", s.lines625 ? "625" : "525",
                  s.blink_enabled ? "on" : "off");
    t += QStringLiteral("  Y0  $%1   first row %2, %3\n").arg(hx(s.y0, 2)).arg((int)s.first_row)
             .arg(s.zoom ? "double-height rows" : "normal rows");
    t += QStringLiteral("  M   $%1   %2, slice %3\n").arg(hx(s.m, 2), QString::fromUtf8(s.mode_name ? s.mode_name : "?"))
             .arg((int)s.slice);
    t += QStringLiteral("  blink      %1 (prescaler %2)\n").arg(s.blink ? "off phase" : "on phase").arg(s.blink_prescaler);
    t += QStringLiteral("  cursor     %1\n").arg(s.cursor_visible ? "on a cell" : "not on a cell");
    t += QStringLiteral("\nCharacter set   %1\nLines drawn     %2\n")
             .arg(s.font_present ? "imported" : "not imported (blank glyphs)").arg((qulonglong)s.lines_drawn);
    dbgui::setText(m_text, t);

    int w = 0, hgt = 0;
    static std::vector<uint32_t> px(EF9340_LAYER_STRIDE * EF9340_LAYER_ROWS);
    if (o2debug_ef_view(m_dbg, px.data(), px.size(), &w, &hgt) && w > 0 && hgt > 0) {
        const QImage img(reinterpret_cast<const uchar *>(px.data()), w, hgt, w * 4, QImage::Format_RGB32);
        m_layer->setPixmap(QPixmap::fromImage(img.copy()));
    }
    m_page->set(s);
    m_note->setText(QStringLiteral("Graphics cells ▚, delimiters ·, extended glyphs ▣. The cursor is outlined. "
                                   "Glyph shapes are approximated by the character code."));
}

/* ---- Sound & I/O ------------------------------------------------------------------------ */

/* The keyboard matrix as the console scans it: eight rows of eight, a key
 * held lit, the row the CPU has selected (P20-P22) outlined. */
class KeyMatrixWidget : public QWidget {
public:
    KeyMatrixWidget()
    {
        setFixedSize(kHead + 8 * kCellW + 2, kHead + 8 * kCellH + 2);
    }
    void set(uint64_t keys, int row, int machine)
    {
        if (keys == m_keys && row == m_row && machine == m_machine) return;
        m_keys = keys;
        m_row = row;
        m_machine = machine;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QFont small = [&] { QFont f = font(); f.setPointSizeF(f.pointSizeF() * 0.8); return f; }();
        p.setFont(small);
        p.setPen(palette().color(QPalette::Text));
        for (int c = 0; c < 8; ++c)
            p.drawText(QRect(kHead + c * kCellW, 0, kCellW, kHead), Qt::AlignCenter, QStringLiteral("KEY.%1").arg(c));
        for (int r = 0; r < 8; ++r) {
            p.setPen(r == m_row ? o2AccentTextColor(this) : palette().color(QPalette::Text));
            p.drawText(QRect(0, kHead + r * kCellH, kHead, kCellH), Qt::AlignCenter, QString::number(r));
            for (int c = 0; c < 8; ++c) {
                const int key = r * 8 + c;
                const char *name = o2session_key_label(m_machine, key);
                const bool held = (m_keys >> key) & 1u;
                const QRect cell(kHead + c * kCellW + 1, kHead + r * kCellH + 1, kCellW - 2, kCellH - 2);
                p.setPen(r == m_row ? QPen(o2AccentTextColor(this), 2) : QPen(palette().color(QPalette::Mid)));
                p.setBrush(held ? o2AccentColor() : name ? palette().color(QPalette::Base) : palette().color(QPalette::Window));
                p.drawRoundedRect(cell, 3, 3);
                p.setPen(held ? QColor(Qt::white) : palette().color(name ? QPalette::Text : QPalette::Mid));
                p.drawText(cell, Qt::AlignCenter,
                           name ? QString::fromUtf8(name).section(QLatin1Char(' '), 0, 0) : QStringLiteral("·"));
            }
        }
    }

private:
    static constexpr int kHead = 34, kCellW = 54, kCellH = 28;
    uint64_t m_keys = 0;
    int m_row = -1, m_machine = 0;
};

/* One joystick as the console reads it: the four directions round the button. */
class StickDiagram : public QWidget {
public:
    StickDiagram() { setFixedSize(3 * kCell + 2, 3 * kCell + 2); }
    void set(unsigned bits)
    {
        if (bits == m_bits) return;
        m_bits = bits;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        /* bit 0 UP, 1 RIGHT, 2 DOWN, 3 LEFT, 4 BUTTON */
        static const struct { int bit, x, y; const char *label; } pads[5] = {
            { 0, 1, 0, "▲" }, { 1, 2, 1, "▶" }, { 2, 1, 2, "▼" }, { 3, 0, 1, "◀" }, { 4, 1, 1, "●" },
        };
        for (const auto &pd : pads) {
            const QRect r(1 + pd.x * kCell, 1 + pd.y * kCell, kCell - 3, kCell - 3);
            const bool on = (m_bits >> pd.bit) & 1u;
            p.setPen(palette().color(QPalette::Mid));
            p.setBrush(on ? o2AccentColor() : palette().color(QPalette::Base));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(on ? QColor(Qt::white) : palette().color(QPalette::Text));
            p.drawText(r, Qt::AlignCenter, QString::fromUtf8(pd.label));
        }
    }

private:
    static constexpr int kCell = 30;
    unsigned m_bits = 0;
};

IoTab::IoTab(o2debug *debugger, o2session *session, QWidget *parent)
    : QWidget(parent), m_dbg(debugger), m_session(session)
{
    auto *h = new QHBoxLayout(this);
    m_text = dbgui::monoView();
    h->addWidget(m_text, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(dbgui::titleLabel(QStringLiteral("Keyboard matrix as the console scans it")));
    m_matrix = new KeyMatrixWidget;
    right->addWidget(m_matrix);
    right->addWidget(dbgui::dimLabel(QStringLiteral("Filled: held. Outlined: the row P20-P22 has selected. "
                                                    "Rows 6 and 7 exist on the Videopac+ G7400 only.")));
    right->addSpacing(8);
    right->addWidget(dbgui::titleLabel(QStringLiteral("Joysticks")));
    auto *sticks = new QHBoxLayout;
    for (int i = 0; i < 2; ++i) {
        auto *col = new QVBoxLayout;
        auto *name = new QLabel(QStringLiteral("Stick %1").arg(i + 1));
        name->setAlignment(Qt::AlignHCenter);
        col->addWidget(name);
        m_stick[i] = new StickDiagram;
        col->addWidget(m_stick[i], 0, Qt::AlignHCenter);
        sticks->addLayout(col);
    }
    m_reset = new QLabel;
    sticks->addWidget(m_reset, 1);
    right->addLayout(sticks);
    right->addStretch();
    h->addLayout(right);
}

void IoTab::refresh()
{
    o2debug_io io;
    i8244_snapshot vdc;
    o2debug_io_get(m_dbg, &io);
    o2debug_vdc_get(m_dbg, &vdc);
    const bool g7400 = io.machine == O2_MACHINE_VIDEOPAC_PLUS;
    auto bits8 = [](unsigned v) {
        QString b;
        for (int i = 7; i >= 0; --i) b += (v >> i) & 1 ? QLatin1Char('1') : QLatin1Char('0');
        return b;
    };
    QString t;
    t += QStringLiteral("P1 = $%1  %2\n").arg(hx(io.p1, 2), bits8(io.p1));
    t += QStringLiteral("  P10-P11  cartridge bank       %1\n").arg(io.cart_bank);
    t += QStringLiteral("  P12      keyboard and sticks  %1 (the decoder and stick buffer)\n").arg(io.kbd_enable ? "on" : "off");
    t += QStringLiteral("  P13      8244 video chip      %1\n").arg(io.vdc_select ? "selected" : "not selected");
    t += QStringLiteral("  P14      external RAM, cart   %1\n").arg(io.ram_select ? "selected" : "not selected");
    t += QStringLiteral("  P15      EF9341 text chip     %1%2\n").arg(io.ef_select ? "selected" : "not selected",
                                                                     g7400 ? "" : " (G7400 only)");
    t += QStringLiteral("  P16      copy mode            %1 (external writes and VDC reads gated off)\n").arg(io.copy_mode ? "on" : "off");
    t += QStringLiteral("  P17      luminance            %1\n").arg(io.lum ? "every pixel brightened" : "normal");
    t += QStringLiteral("P2 = $%1  %2\n").arg(hx(io.p2, 2), bits8(io.p2));
    t += QStringLiteral("  P20-P22  keyboard row         %1 (also the high address lines of cartridge reads)\n").arg(io.kbd_row);
    t += QStringLiteral("T0 (the cartridge's: The Voice's LRQ)  %1\n").arg(io.t0);
    t += QStringLiteral("Reset key  %1\n\n").arg(io.reset_held ? "held" : "up");

    t += QStringLiteral("8244 sound\n");
    t += QStringLiteral("  %1, %2, volume %3, shift $%4\n").arg(vdc.snd_enable ? "on" : "off",
                                                                 vdc.snd_noise ? "noise" : "loop")
             .arg(vdc.snd_volume).arg(hx(vdc.shift & 0xFFFFFF, 6));
    if (!vdc.snd_noise && vdc.freq_hz > 0)
        t += QStringLiteral("  tone %1 Hz (the 24 bits repeat every %2)\n").arg(vdc.freq_hz, 0, 'f', 1).arg(vdc.pattern_period);
    t += QStringLiteral("  audio samples made  %1\n\n").arg((qulonglong)io.audio_samples);

    t += QStringLiteral("BIOS            %1\n").arg(io.stub_running ? "the stand-in (CONFIG and FujiNet apps; games need the BIOS)"
                                                                    : "the imported BIOS");
    t += QStringLiteral("BIOS imported   %1\nCharacter set   %2\n").arg(yesNo(io.have_bios), io.have_font ? "imported" : "not imported (text is blank)");
    if (io.missing) {
        QStringList m;
        if (io.missing & O2_MISSING_BIOS) m << QStringLiteral("BIOS");
        if (io.missing & O2_MISSING_FONT) m << QStringLiteral("character set");
        if (io.missing & O2_MISSING_VOICE) m << QStringLiteral("The Voice's ROMs");
        t += QStringLiteral("Missing         %1\n").arg(m.join(QStringLiteral(", ")));
    }

    t += QStringLiteral("\nThe Voice (SP0256)  %1\n").arg(io.voice_present ? "present" : "not fitted");
    if (io.voice_present) {
        const sp0256_debug_t &v = io.voice;
        t += QStringLiteral("  halted %1   LRQ %2   SBY %3\n").arg(v.halted).arg(v.lrq).arg(v.sby);
        t += QStringLiteral("  allophone %1   pending %2   last %3\n").arg(v.allophone).arg(v.pending).arg(v.last_allophone);
        t += QStringLiteral("  period %1   amplitude %2   interpolation %3   %4%5\n").arg(v.period).arg(v.amp).arg(v.interp)
                 .arg(v.silent ? "silent" : "speaking", v.noise ? ", noise" : "");
        t += QStringLiteral("  ROM pc $%1.%2   FIFO %3 decles%4\n").arg(hx(v.rom_addr, 4)).arg(v.rom_bit).arg(v.fifo_count)
                 .arg(v.fifo_sel ? " (fetching from the FIFO)" : "");
        t += QStringLiteral("  clock %1 Hz   sample rate %2 Hz   samples %3   dropped %4\n").arg(v.clock_hz).arg(v.rate_hz)
                 .arg((qulonglong)v.samples_made).arg((qulonglong)v.alds_dropped);
    }
    dbgui::setText(m_text, t);

    m_matrix->set(io.keys, io.kbd_row, io.machine);
    for (int i = 0; i < 2; ++i) m_stick[i]->set(io.sticks[i]);
    m_reset->setText(QStringLiteral("RESET key: %1").arg(io.reset_held ? QStringLiteral("held") : QStringLiteral("up")));
}

/* ---- Cart ----------------------------------------------------------------------------------- */

CartTab::CartTab(o2debug *debugger, QWidget *parent) : QWidget(parent), m_dbg(debugger)
{
    auto *h = new QHBoxLayout(this);
    m_text = dbgui::monoView();
    h->addWidget(m_text, 1);
    auto *right = new QVBoxLayout;
    right->addWidget(dbgui::titleLabel(QStringLiteral("The cartridge's mailbox page, $F20-$FFF")));
    m_mailbox = dbgui::monoView();
    m_mailbox->setMinimumWidth(m_mailbox->fontMetrics().horizontalAdvance(QString(80, QLatin1Char('0'))));
    right->addWidget(m_mailbox, 1);
    h->addLayout(right);
}

void CartTab::refresh()
{
    o2debug_cart c;
    char info[256] = "";
    o2debug_cart_get(m_dbg, &c);
    o2debug_cart_info(m_dbg, info, sizeof info);
    QString t = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) {
        dbgui::setText(m_text, t);
        dbgui::setText(m_mailbox, QString());
        return;
    }
    const QString type = QString::fromUtf8(c.type_name ? c.type_name : "?");
    const bool fuji = type.contains(QStringLiteral("FujiNet"), Qt::CaseInsensitive);
    t += QStringLiteral("Cartridge      %1\n").arg(type);
    t += QStringLiteral("The Voice      %1\n").arg(c.voice_in_front ? "in front of the cartridge" : "not fitted");
    if (!fuji) {
        t += QStringLiteral("ROM            %1 bytes\n").arg(c.rom_size);
        t += QStringLiteral("Bank           %1   control $%2\n").arg(c.bank).arg(hx((unsigned)c.control, 2));
        dbgui::setText(m_text, t);
        dbgui::setText(m_mailbox, QStringLiteral("(not a FujiNet cartridge: it has no mailbox)"));
        return;
    }
    t += QStringLiteral("Link           %1%2\n").arg(c.link_up ? "up" : "down", c.link_off ? " (switched off)" : "");
    t += QStringLiteral("Mailbox        %1\n").arg(c.mailbox ? "open (a FujiNet app: a socket and fujimail)" : "closed");
    t += QStringLiteral("App live       %1   booted game %2\n").arg(yesNo(c.app_live), yesNo(c.booted_game));
    t += QStringLiteral("Live image     %1 bytes, CRC-32 %2\n").arg(c.live_size).arg(hx(c.live_crc, 8));
    t += QStringLiteral("Staged         %1%2\n").arg(c.staged_size ? QStringLiteral("%1 bytes, CRC-32 %2, %3")
                                                                       .arg(c.staged_size).arg(hx(c.staged_crc, 8),
                                                                            c.staged_app ? QStringLiteral("an app%1")
                                                                                               .arg(c.staged_mailbox ? QStringLiteral(" claiming the mailbox") : QString())
                                                                                         : QStringLiteral("a game"))
                                                                 : QStringLiteral("nothing"),
                                               c.armed ? "  (armed: swaps at the next fetch of $400)" : "");
    t += QStringLiteral("Swaps          %1\n").arg(c.swaps);
    t += QStringLiteral("Banks          %1 (showing %2)\n").arg(c.nbanks).arg(c.fbank);
    t += QStringLiteral("Boot           state %1, %2%, error %3\n").arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err);
    t += QStringLiteral("\nMailbox status page\n");
    t += QStringLiteral("  ACKSEQ $%1   STATUS $%2   last error $%3\n").arg(hx(c.ackseq, 2), hx(c.status, 2), hx(c.last_error, 2));
    t += QStringLiteral("  reply command $%1   RXLEN %2   queue depth %3\n").arg(hx(c.reply_cmd, 2)).arg(c.rxlen).arg(c.queue_depth);
    dbgui::setText(m_text, t);

    QString dump;
    for (unsigned row = 0; row < sizeof c.mailbox_page / 16; ++row) {
        dump += QStringLiteral("$%1  ").arg(hx(0xF20 + row * 16, 3));
        QString ascii;
        for (unsigned col = 0; col < 16; ++col) {
            const uint8_t b = c.mailbox_page[row * 16 + col];
            dump += hx(b, 2) + QLatin1Char(' ');
            ascii += b >= 0x20 && b < 0x7F ? QChar(b) : QLatin1Char('.');
        }
        dump += QStringLiteral(" ") + ascii + QLatin1Char('\n');
    }
    dbgui::setText(m_mailbox, dump);
}

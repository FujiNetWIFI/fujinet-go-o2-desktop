/*
 * VideoTab -- see VideoTab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "VideoTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cstring>
#include <functional>

#include "../FujiNetWindows.h"
#include "DebugUi.h"

using dbgui::hx;

namespace {

constexpr int kZoom = 2;   /* the picture is shown at 2x, nearest neighbour */

QString colourText(int c)
{
    return QStringLiteral("%1 %2").arg(c).arg(QString::fromUtf8(i8244_colour_name(c)));
}

QString shapeText(const uint8_t shape[8])
{
    QStringList b;
    for (int i = 0; i < 8; ++i) b << hx(shape[i], 2);
    return b.join(QLatin1Char(' '));
}

QColor fromXrgb(uint32_t v) { return QColor((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

/* Sets a table cell's text (creating the item), touching it only when it
 * changed. */
void setCell(QTableWidget *t, int row, int col, const QString &text)
{
    QTableWidgetItem *it = t->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        t->setItem(row, col, it);
    }
    if (it->text() != text) it->setText(text);
}

QTableWidget *objectTable(const QStringList &headers, int rows, int rowHeight)
{
    auto *t = new QTableWidget(rows, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(rowHeight);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setFocusPolicy(Qt::NoFocus);
    t->setFont(dbgui::monoFont(t->font()));
    t->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setIconSize(QSize(rowHeight - 4, rowHeight - 4));
    /* tall enough for every row, so the section scrolls, not the table */
    t->setFixedHeight(t->horizontalHeader()->sizeHint().height() + rows * rowHeight + 4);
    return t;
}

} // namespace

/* ---- the picture ---------------------------------------------------------------- */

/* The chip's picture at 2x, nearest neighbour, with a box round the object
 * under the pointer. */
class VdcImage : public QWidget {
public:
    std::function<void(int, int)> onHover;   /* frame pixel, or -1 -1 when the pointer leaves */

    VdcImage()
    {
        setMouseTracking(true);
        setFixedSize(I8244_WIDTH * kZoom, I8244_HEIGHT * kZoom);
        setCursor(Qt::CrossCursor);
    }
    void setImage(const QImage &img)
    {
        m_img = img;
        update();
    }
    void setHighlight(const QRect &r)
    {
        if (r == m_hl) return;
        m_hl = r;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (m_img.isNull()) return;
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(rect(), m_img);
        if (!m_hl.isNull()) {
            const QRect r(m_hl.x() * kZoom, m_hl.y() * kZoom, m_hl.width() * kZoom, m_hl.height() * kZoom);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 3));
            p.drawRect(r.adjusted(-1, -1, 1, 1));
            p.setPen(QPen(Qt::white, 1));
            p.drawRect(r.adjusted(-1, -1, 1, 1));
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (onHover) onHover(qBound(0, (int)e->position().x() / kZoom, I8244_WIDTH - 1),
                             qBound(0, (int)e->position().y() / kZoom, I8244_HEIGHT - 1));
    }
    void leaveEvent(QEvent *) override
    {
        if (onHover) onHover(-1, -1);
    }

private:
    QImage m_img;
    QRect m_hl;
};

/* ---- rows of labelled bits ------------------------------------------------------------ */

/* Rows of bit cells under a header: the shift register (24 cells) and the
 * collision matrix. Bit j of a row's value is the cell at column j, or, with
 * msbFirst, at column n-1-j. */
class BitsWidget : public QWidget {
public:
    BitsWidget(const QStringList &columns, const QStringList &rows, int cellW, bool msbFirst = false)
        : m_cols(columns), m_rows(rows), m_cw(cellW), m_msbFirst(msbFirst), m_values(rows.size(), 0)
    {
        QFontMetrics fm(font());
        for (const QString &r : rows) m_labelW = qMax(m_labelW, fm.horizontalAdvance(r) + 10);
        m_ch = fm.height() + 8;
        setFixedSize(m_labelW + columns.size() * m_cw + 2, (rows.size() + 1) * (m_ch + 2) + 2);
    }
    void set(int row, uint32_t value)
    {
        if (m_values[(size_t)row] == value) return;
        m_values[(size_t)row] = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int n = m_cols.size();
        p.setPen(palette().color(QPalette::Text));
        for (int j = 0; j < n; ++j)
            p.drawText(QRect(m_labelW + j * m_cw, 0, m_cw, m_ch), Qt::AlignCenter, m_cols[j]);
        for (int r = 0; r < m_rows.size(); ++r) {
            const int y = (r + 1) * (m_ch + 2);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRect(0, y, m_labelW - 6, m_ch), Qt::AlignRight | Qt::AlignVCenter, m_rows[r]);
            for (int j = 0; j < n; ++j) {
                const int bit = m_msbFirst ? n - 1 - j : j;
                const bool on = (m_values[(size_t)r] >> bit) & 1u;
                const QRect c(m_labelW + j * m_cw + 1, y, m_cw - 2, m_ch);
                p.setPen(palette().color(QPalette::Mid));
                p.setBrush(on ? o2AccentColor() : palette().color(QPalette::Base));
                p.drawRoundedRect(c, 3, 3);
                p.setPen(on ? QColor(Qt::white) : palette().color(QPalette::Text));
                p.drawText(c, Qt::AlignCenter, on ? QStringLiteral("1") : QStringLiteral("0"));
            }
        }
    }

private:
    QStringList m_cols, m_rows;
    int m_cw, m_ch = 0, m_labelW = 0;
    bool m_msbFirst;
    std::vector<uint32_t> m_values;
};

/* ---- the grid ------------------------------------------------------------------------- */

/* The grid as the chip holds it: nine columns by eight rows of cells, ten
 * vertical and nine horizontal line positions, each segment lit or not, and
 * the dots where the lines meet. */
class GridDiagram : public QWidget {
public:
    GridDiagram() { setFixedSize(kMargin * 2 + 9 * kCellW, kMargin * 2 + 8 * kCellH); }
    void set(const i8244_snapshot &s, const QColor &colour)
    {
        m_colour = colour;
        m_on = s.grid_en != 0;
        m_dots = s.dots_en != 0;
        m_fill = s.fill != 0;
        for (int i = 0; i < 9; ++i) m_h[i] = s.hgrid[i];
        for (int i = 0; i < 10; ++i) m_v[i] = s.vgrid[i];
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        const QColor lit = m_on ? m_colour : m_colour.darker(300);
        const QColor dim(0x30, 0x30, 0x30);
        const int thick = 3;
        /* horizontal segments: line y (0-8) of column i (0-8) */
        for (int i = 0; i < 9; ++i)
            for (int y = 0; y < 9; ++y) {
                const bool on = (m_h[i] >> y) & 1;
                p.fillRect(kMargin + i * kCellW, kMargin + y * kCellH - thick / 2, kCellW, thick, on ? lit : dim);
            }
        /* vertical segments: line i (0-9) of row y (0-7); FILL makes them as wide as a cell */
        for (int i = 0; i < 10; ++i)
            for (int y = 0; y < 8; ++y) {
                const bool on = (m_v[i] >> y) & 1;
                const int w = m_fill && on ? kCellW - 2 : thick;
                p.fillRect(kMargin + i * kCellW - (m_fill && on ? 0 : thick / 2), kMargin + y * kCellH, w, kCellH,
                           on ? lit : dim);
            }
        if (m_dots) {
            p.setPen(Qt::NoPen);
            p.setBrush(lit);
            for (int i = 0; i < 10; ++i)
                for (int y = 0; y < 9; ++y)
                    p.drawEllipse(QPointF(kMargin + i * kCellW, kMargin + y * kCellH), 3.5, 3.5);
        }
    }

private:
    static constexpr int kMargin = 10, kCellW = 26, kCellH = 36;
    uint16_t m_h[9] = {};
    uint8_t m_v[10] = {};
    QColor m_colour = Qt::white;
    bool m_on = true, m_dots = false, m_fill = false;
};

/* ---- the tab --------------------------------------------------------------------------- */

VideoTab::VideoTab(o2debug *debugger, QWidget *parent)
    : QWidget(parent), m_dbg(debugger), m_px(1u << 18)
{
    memset(&m_snap, 0, sizeof m_snap);
    loadPalette();

    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QScrollArea;
    left->setWidget(buildLeft());
    left->setFrameShape(QFrame::NoFrame);
    split->addWidget(left);
    m_sections = new QTabWidget;
    m_sections->addTab(buildObjects(), QStringLiteral("Objects"));
    m_sections->addTab(buildGrid(), QStringLiteral("Grid"));
    m_sections->addTab(buildRegisters(), QStringLiteral("Registers"));
    m_sections->addTab(buildControl(), QStringLiteral("Control/Status"));
    m_sections->addTab(buildSound(), QStringLiteral("Sound"));
    m_sections->addTab(buildSheets(), QStringLiteral("Sheets"));
    connect(m_sections, &QTabWidget::currentChanged, this, [this] { refresh(); });
    split->addWidget(m_sections);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    left->setMinimumWidth(I8244_WIDTH * kZoom + 36);
    m_sections->setMinimumWidth(460);

    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(split);
    hover(-1, -1);
}

/* The palette as the chip draws it, sampled from the palette strip (8 colours
 * dim above 8 bright), for the swatches and the grid. */
void VideoTab::loadPalette()
{
    for (int i = 0; i < 16; ++i) m_pal[i] = 0x808080;
    int w = 0, h = 0;
    if (!o2debug_vdc_view(m_dbg, O2DEBUG_VDC_PALETTE, I8244_L_ALL, 0, 1, m_px.data(), m_px.size(), &w, &h)
        || w < 8 || h < 2)
        return;
    const int cell = w / 8;
    if (cell < 1 || h < 2 * cell) return;
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 8; ++col)
            m_pal[row * 8 + col] = m_px[(size_t)(row * cell + cell / 2) * w + col * cell + cell / 2];
}

/* ---- building ------------------------------------------------------------------------------ */

QWidget *VideoTab::buildLeft()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setSizeConstraint(QLayout::SetFixedSize);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("View")));
    m_view = new QComboBox;
    m_view->addItems({ QStringLiteral("Frame (what the display shows)"),
                       QStringLiteral("Rendered from the registers"),
                       QStringLiteral("Frame in progress, up to the beam") });
    m_view->setCurrentIndex(O2DEBUG_VDC_RENDER);
    m_view->setToolTip(QStringLiteral("Rendered re-draws the chip's registers as they are now with the layers "
                                      "and overlays below, whatever the beam is doing; Frame is the last "
                                      "finished frame; Frame in progress shows what the beam has drawn so far"));
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    row->addWidget(m_view, 1);
    v->addLayout(row);

    static const char *const layerNames[7] = { "Background", "Grid", "Vertical grid", "Dots", "Sprites",
                                               "Singles", "Quads" };
    auto *lg = new QGridLayout;
    lg->addWidget(dbgui::titleLabel(QStringLiteral("Layers")), 0, 0, 1, 4);
    for (int i = 0; i < 7; ++i) {
        m_layer[i] = new QCheckBox(QString::fromLatin1(layerNames[i]));
        m_layer[i]->setChecked(true);
        m_layer[i]->setFocusPolicy(Qt::NoFocus);
        connect(m_layer[i], &QCheckBox::toggled, this, [this] { refresh(); });
        lg->addWidget(m_layer[i], 1 + i / 4, i % 4);
    }
    v->addLayout(lg);

    static const struct { const char *name, *tip; bool on; } overlays[6] = {
        { "Bounding boxes + indices", "Outline every object and number it", true },
        { "Collision highlight", "Tint the pixels that set a bit of the collision register under its mask", false },
        { "Overlap highlight", "Tint any pixel where two objects overlap", false },
        { "Show hidden", "Draw what the NTSC chip hides above line 14", false },
        { "Ignore enables", "Draw the grid and the objects although the control register has them off", false },
        { "Beam cross-hair", "Frame in progress: a cross-hair where the beam is", true },
    };
    auto *og = new QGridLayout;
    og->addWidget(dbgui::titleLabel(QStringLiteral("Overlays")), 0, 0, 1, 3);
    for (int i = 0; i < 6; ++i) {
        m_overlay[i] = new QCheckBox(QString::fromLatin1(overlays[i].name));
        m_overlay[i]->setToolTip(QString::fromLatin1(overlays[i].tip));
        m_overlay[i]->setChecked(overlays[i].on);
        m_overlay[i]->setFocusPolicy(Qt::NoFocus);
        connect(m_overlay[i], &QCheckBox::toggled, this, [this] { refresh(); });
        og->addWidget(m_overlay[i], 1 + i / 3, i % 3);
    }
    v->addLayout(og);

    m_image = new VdcImage;
    m_image->onHover = [this](int x, int y) { hover(x, y); };
    v->addWidget(m_image);
    m_hover = new QLabel;
    m_hover->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hover->setFixedWidth(I8244_WIDTH * kZoom);
    m_hover->setWordWrap(true);
    m_hover->setMinimumHeight(m_hover->fontMetrics().lineSpacing() * 2 + 4);
    m_hover->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    v->addWidget(m_hover);
    m_beam = dbgui::dimLabel();
    v->addWidget(m_beam);
    v->addStretch();
    return w;
}

QWidget *VideoTab::buildObjects()
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Sprites: four 8x8 shapes (the \"minor\" objects)")));
    m_sprites = objectTable({ QStringLiteral("#"), QStringLiteral("Bitmap"), QStringLiteral("X"), QStringLiteral("Y"),
                              QStringLiteral("Colour"), QStringLiteral("Zoom"), QStringLiteral("S"),
                              QStringLiteral("X9"), QStringLiteral("Shape rows"), QStringLiteral("State") }, 4, 28);
    v->addWidget(m_sprites);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Single characters: twelve, 16 pixels wide")));
    const QStringList cols = { QStringLiteral("#"), QStringLiteral("Bitmap"), QStringLiteral("X"), QStringLiteral("Y"),
                               QStringLiteral("Ptr"), QStringLiteral("Glyph"), QStringLiteral("Colour"),
                               QStringLiteral("Bit 8"), QStringLiteral("Rows"), QStringLiteral("State") };
    m_singles = objectTable(cols, 12, 22);
    v->addWidget(m_singles);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Quads: four of four characters, one X and Y each")));
    QStringList qcols = cols;
    qcols[0] = QStringLiteral("Q.c");
    m_quads = objectTable(qcols, 16, 22);
    v->addWidget(m_quads);
    v->addWidget(dbgui::dimLabel(QStringLiteral("X and Y are screen pixels (360x243). Shape rows are the eight "
                                                "shape bytes, bit 0 leftmost.")));
    v->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

QWidget *VideoTab::buildGrid()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    m_gridDiagram = new GridDiagram;
    auto *left = new QVBoxLayout;
    left->addWidget(dbgui::titleLabel(QStringLiteral("Grid: 9 columns x 8 rows")));
    left->addWidget(m_gridDiagram);
    left->addStretch();
    h->addLayout(left);
    m_gridText = dbgui::monoView();
    h->addWidget(m_gridText, 1);
    return w;
}

QWidget *VideoTab::buildRegisters()
{
    m_regs = new QTableWidget(256, 4);
    m_regs->setHorizontalHeaderLabels({ QStringLiteral("Reg"), QStringLiteral("Name"), QStringLiteral("Value"),
                                        QStringLiteral("Decoded") });
    m_regs->verticalHeader()->setVisible(false);
    m_regs->verticalHeader()->setDefaultSectionSize(m_regs->fontMetrics().height() + 6);
    m_regs->setFont(dbgui::monoFont(m_regs->font()));
    m_regs->setSelectionMode(QAbstractItemView::SingleSelection);
    m_regs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_regs->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_regs->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_regs->horizontalHeader()->setStretchLastSection(true);
    m_regsFilling = true;
    for (int off = 0; off < 256; ++off) {
        for (int col = 0; col < 4; ++col) {
            auto *it = new QTableWidgetItem;
            if (col != 2) it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            m_regs->setItem(off, col, it);
        }
        m_regs->item(off, 0)->setText(QStringLiteral("$") + hx(off, 2));
    }
    m_regsFilling = false;
    /* a store while the machine is stopped, as the chip's registers take it */
    connect(m_regs, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_regsFilling || it->column() != 2) return;
        bool ok = false;
        QString t = it->text().trimmed();
        if (t.startsWith(QLatin1Char('$'))) t = t.mid(1);
        const unsigned v = t.toUInt(&ok, 16);
        if (ok && v <= 0xFF && o2debug_is_stopped(m_dbg))
            o2debug_vdc_write(m_dbg, (unsigned)it->row(), (uint8_t)v);
        refresh();
    });
    return m_regs;
}

QWidget *VideoTab::buildControl()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_ctlText = dbgui::monoView();
    v->addWidget(m_ctlText, 1);
    v->addWidget(dbgui::titleLabel(QStringLiteral("Collision register $A2: the mask the program set, and what has latched")));
    m_collision = new BitsWidget({ QStringLiteral("S0"), QStringLiteral("S1"), QStringLiteral("S2"),
                                   QStringLiteral("S3"), QStringLiteral("V grid"), QStringLiteral("H grid"),
                                   QStringLiteral("CX"), QStringLiteral("Chars") },
                                 { QStringLiteral("mask"), QStringLiteral("latched") }, 52);
    v->addWidget(m_collision);
    v->addWidget(dbgui::dimLabel(QStringLiteral("S0-S3: the sprites. V grid: the vertical lines. H grid: the horizontal "
                                                "lines and the dots. CX: the external overlap input (the G7400's "
                                                "text chip). Chars: the singles and quads.")));
    return w;
}

QWidget *VideoTab::buildSound()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->addWidget(dbgui::titleLabel(QStringLiteral("The sound shift register $A7-$A9 (bit 23 left)")));
    QStringList bits;
    for (int i = 23; i >= 0; --i) bits << QString::number(i);
    m_shift = new BitsWidget(bits, { QStringLiteral("shift") }, 22, true);
    v->addWidget(m_shift);
    m_soundText = dbgui::monoView();
    v->addWidget(m_soundText, 1);
    return w;
}

QWidget *VideoTab::buildSheets()
{
    auto *content = new QWidget;
    auto *v = new QVBoxLayout(content);
    auto section = [&](const QString &title, QLabel *&label) {
        v->addWidget(dbgui::titleLabel(title));
        label = new QLabel;
        label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        v->addWidget(label);
    };
    section(QStringLiteral("Font: the 512-byte character set as 64 glyphs (top two rows: colour bit 0 = 0; "
                           "bottom two: bit 0 = 1)"), m_fontSheet);
    section(QStringLiteral("Sprite shapes: the four shape tables, in their colours"), m_shapeSheet);
    section(QStringLiteral("Palette: eight colours dim over eight bright"), m_paletteSheet);
    v->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

/* ---- refreshing --------------------------------------------------------------------------- */

unsigned VideoTab::layers() const
{
    unsigned m = 0;
    for (int i = 0; i < 7; ++i)
        if (m_layer[i]->isChecked()) m |= 1u << i;
    return m;
}

unsigned VideoTab::overlays() const
{
    static const unsigned flags[6] = { I8244_F_BOXES, I8244_F_COLLISION, I8244_F_OVERLAP, I8244_F_SHOW_HIDDEN,
                                       I8244_F_IGNORE_ENABLE, I8244_F_BEAM };
    unsigned f = 0;
    for (int i = 0; i < 6; ++i)
        if (m_overlay[i]->isChecked()) f |= flags[i];
    return f;
}

void VideoTab::refresh()
{
    o2debug_vdc_get(m_dbg, &m_snap);
    refreshPicture();
    switch (m_sections->currentIndex()) {
    case 0: refreshObjects(); break;
    case 1: refreshGrid(); break;
    case 2: refreshRegisters(); break;
    case 3: refreshControl(); break;
    case 4: refreshSound(); break;
    case 5: refreshSheets(); break;
    default: break;
    }
}

void VideoTab::refreshPicture()
{
    int w = 0, h = 0;
    if (o2debug_vdc_view(m_dbg, m_view->currentIndex(), layers(), overlays(), 1, m_px.data(), m_px.size(), &w, &h)
        && w > 0 && h > 0) {
        const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
        m_image->setImage(img.copy());   /* the buffer is reused by the next call */
    }
    m_beam->setText(QStringLiteral("Beam at line %1, dot %2 (%3), frame %4; %5 Hz.")
                        .arg(m_snap.vpos).arg(m_snap.hpos)
                        .arg(m_snap.vpos < I8244_HEIGHT ? QStringLiteral("drawing") : QStringLiteral("in vertical blank"))
                        .arg((qulonglong)m_snap.frames)
                        .arg(m_snap.frame_hz, 0, 'f', 2));
}

/* An object as the table rows draw it, as a pixmap of `scale` per pixel. */
QPixmap VideoTab::objectPixmap(int kind, int index, int scale)
{
    int w = 0, h = 0;
    if (!o2debug_vdc_object(m_dbg, kind, index, scale, 0xFF202020u, m_px.data(), m_px.size(), &w, &h) || w <= 0 || h <= 0)
        return QPixmap();
    const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
    return QPixmap::fromImage(img);   /* copies */
}

/* A sheet view scaled up by a whole number to fit maxWidth. */
QPixmap VideoTab::viewPixmap(int view, int maxWidth)
{
    int w = 0, h = 0;
    if (!o2debug_vdc_view(m_dbg, view, I8244_L_ALL, 0, 1, m_px.data(), m_px.size(), &w, &h) || w <= 0 || h <= 0)
        return QPixmap();
    const QImage img(reinterpret_cast<const uchar *>(m_px.data()), w, h, w * 4, QImage::Format_RGB32);
    const int scale = qBound(1, maxWidth / w, 8);
    return QPixmap::fromImage(img.scaled(w * scale, h * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

/* The colour cell: a swatch of the bright variant (objects always draw it)
 * and the number and name. */
static void setColourCell(QTableWidget *t, int row, int col, int colour, const uint32_t *pal)
{
    setCell(t, row, col, colourText(colour));
    QPixmap sw(10, 10);
    sw.fill(fromXrgb(pal[8 + (colour & 7)]));
    t->item(row, col)->setData(Qt::DecorationRole, sw);
}

static QString stateText(int visible, int clipped, int ntscHidden)
{
    if (ntscHidden) return QStringLiteral("hidden (NTSC, Y < 14)");
    if (!visible) return QStringLiteral("off screen");
    return clipped ? QStringLiteral("clipped") : QStringLiteral("visible");
}

void VideoTab::refreshObjects()
{
    for (int i = 0; i < 4; ++i) {
        const i8244_sprite_info &s = m_snap.sprites[i];
        setCell(m_sprites, i, 0, QString::number(i));
        const QPixmap pm = objectPixmap(0, i, 3);
        setCell(m_sprites, i, 1, QString());
        m_sprites->item(i, 1)->setData(Qt::DecorationRole, pm);
        setCell(m_sprites, i, 2, QString::number(s.x));
        setCell(m_sprites, i, 3, QString::number(s.y));
        setColourCell(m_sprites, i, 4, s.color, m_pal);
        setCell(m_sprites, i, 5, s.zoom ? QStringLiteral("4 px/bit") : QStringLiteral("2 px/bit"));
        setCell(m_sprites, i, 6, QString::number(s.s));
        setCell(m_sprites, i, 7, QString::number(s.x9));
        setCell(m_sprites, i, 8, shapeText(s.shape));
        setCell(m_sprites, i, 9, stateText(s.visible, s.clipped, 0));
    }
    auto fillChars = [&](QTableWidget *t, int row, const QString &name, const i8244_char_info &c, int kind, int index) {
        setCell(t, row, 0, name);
        setCell(t, row, 1, QString());
        t->item(row, 1)->setData(Qt::DecorationRole, objectPixmap(kind, index, 2));
        setCell(t, row, 2, QString::number(c.x));
        setCell(t, row, 3, QString::number(c.y));
        setCell(t, row, 4, QStringLiteral("$") + hx(c.reg_ptr, 2));
        setCell(t, row, 5, QStringLiteral("%1 (+%2)").arg(c.glyph).arg(c.first_row));
        setColourCell(t, row, 6, c.color, m_pal);
        setCell(t, row, 7, QString::number(c.bit8));
        setCell(t, row, 8, QString::number(c.rows));
        setCell(t, row, 9, stateText(c.visible, c.clipped, c.ntsc_hidden));
    };
    for (int i = 0; i < 12; ++i) fillChars(m_singles, i, QString::number(i), m_snap.singles[i], 1, i);
    for (int q = 0; q < 4; ++q)
        for (int j = 0; j < 4; ++j)
            fillChars(m_quads, q * 4 + j, QStringLiteral("%1.%2").arg(q).arg(j), m_snap.quads[q].chars[j], 2, q * 4 + j);
}

void VideoTab::refreshGrid()
{
    const i8244_snapshot &s = m_snap;
    m_gridDiagram->set(s, fromXrgb(m_pal[(s.grid_lum ? 8 : 0) + (s.grid_color & 7)]));
    QString t;
    t += QStringLiteral("Grid          %1 ($A0 bit 3)\n").arg(s.grid_en ? "on" : "off");
    t += QStringLiteral("Dots          %1 ($A0 bit 6)\n").arg(s.dots_en ? "on" : "off");
    t += QStringLiteral("Fill          %1 ($A0 bit 7: vertical lines a whole cell wide)\n").arg(s.fill ? "on" : "off");
    t += QStringLiteral("Colour        %1%2\n").arg(colourText(s.grid_color), s.grid_lum ? QStringLiteral(" (bright)") : QString());
    t += QStringLiteral("Origin        x %1 (x2 pixels), y %2; cells %3 x %4 pixels\n\n")
             .arg(s.grid_x0).arg(s.grid_y0).arg(s.cell_w).arg(s.cell_h);
    t += QStringLiteral("Horizontal lines ($C0-$D8: 9 columns, a line y of each in a bit)\n");
    for (int i = 0; i < 9; ++i) {
        QString bits;
        for (int y = 0; y < 9; ++y) bits += (s.hgrid[i] >> y) & 1 ? QLatin1Char('#') : QLatin1Char('.');
        t += QStringLiteral("  col %1  %2  $%3\n").arg(i).arg(bits, hx(s.hgrid[i], 3));
    }
    t += QStringLiteral("\nVertical lines ($E0-$E9: 10 positions, a row y of each in a bit)\n");
    for (int i = 0; i < 10; ++i) {
        QString bits;
        for (int y = 0; y < 8; ++y) bits += (s.vgrid[i] >> y) & 1 ? QLatin1Char('#') : QLatin1Char('.');
        t += QStringLiteral("  pos %1  %2  $%3\n").arg(i).arg(bits, hx(s.vgrid[i], 2));
    }
    t += QStringLiteral("\nWrite lockout: the grid registers read 0 and ignore writes while the grid is on: %1\n")
             .arg(s.lock_grid ? "locked now" : "open");
    dbgui::setText(m_gridText, t);
}

void VideoTab::refreshRegisters()
{
    m_regsFilling = true;
    for (int off = 0; off < 256; ++off) {
        char name[64] = "", text[200] = "";
        o2debug_vdc_describe(m_dbg, (unsigned)off, name, sizeof name, text, sizeof text);
        setCell(m_regs, off, 1, QString::fromUtf8(name));
        const QString value = QStringLiteral("$") + hx(m_snap.regs[off], 2);
        QTableWidgetItem *v = m_regs->item(off, 2);
        /* leave a value being typed alone */
        if (!(dbgui::editing(m_regs) && m_regs->currentRow() == off)) {
            /* a register that changed since the last look is shown in the accent */
            const char hot = !v->text().isEmpty() && v->text() != value;
            if (v->text() != value) v->setText(value);
            if (hot != m_regHot[(size_t)off]) {
                m_regHot[(size_t)off] = hot;
                v->setForeground(hot ? QBrush(o2AccentTextColor(this)) : QBrush());
            } else if (!hot && m_regHot[(size_t)off]) {
                m_regHot[(size_t)off] = 0;
            }
        }
        setCell(m_regs, off, 3, QString::fromUtf8(text));
    }
    m_regsFilling = false;
}

void VideoTab::refreshControl()
{
    const i8244_snapshot &s = m_snap;
    auto flag = [](int v) { return v ? QStringLiteral("1") : QStringLiteral("0"); };
    QString t;
    t += QStringLiteral("$A0 CONTROL = $%1\n").arg(hx(s.control, 2));
    t += QStringLiteral("  bit 0  (unused)                          %1\n").arg(flag(s.ctl_bit0));
    t += QStringLiteral("  bit 1  strobe: beam position latched     %1\n").arg(flag(s.strobe));
    t += QStringLiteral("  bit 2  sound interrupt enable            %1\n").arg(flag(s.sound_irq_en));
    t += QStringLiteral("  bit 3  grid enable                       %1\n").arg(flag(s.grid_en));
    t += QStringLiteral("  bit 4  external (CX) overlap interrupt   %1\n").arg(flag(s.ext_irq_en));
    t += QStringLiteral("  bit 5  foreground (objects) enable       %1\n").arg(flag(s.fg_en));
    t += QStringLiteral("  bit 6  dots                              %1\n").arg(flag(s.dots_en));
    t += QStringLiteral("  bit 7  fill (vertical lines 16 wide)     %1\n").arg(flag(s.fill));
    t += QStringLiteral("\n$A1 STATUS = $%1 (as a read would give it now; the sticky bits clear on a read)\n")
             .arg(hx(s.status, 2));
    t += QStringLiteral("  horizontal status                        %1\n").arg(flag(s.st_hstatus));
    t += QStringLiteral("  strobe                                   %1\n").arg(flag(s.st_strobe));
    t += QStringLiteral("  sound (shift register interrupt)         %1\n").arg(flag(s.st_sound));
    t += QStringLiteral("  vertical blank                           %1\n").arg(flag(s.st_vblank));
    t += QStringLiteral("  external overlap (CX)                    %1\n").arg(flag(s.st_ext_overlap));
    t += QStringLiteral("  character overlap                        %1\n").arg(flag(s.st_char_overlap));
    t += QStringLiteral("  interrupt line (_INTR)                   %1\n").arg(flag(s.irq));
    t += QStringLiteral("\n$A3 COLOURS = $%1\n").arg(hx(s.color_reg, 2));
    t += QStringLiteral("  background   %1\n").arg(colourText(s.bg_color));
    t += QStringLiteral("  grid         %1%2\n").arg(colourText(s.grid_color), s.grid_lum ? QStringLiteral(", bright") : QString());
    t += QStringLiteral("  P17 (LUM)    %1\n").arg(s.p17_lum ? "low: every pixel brightened" : "high");
    t += QStringLiteral("\nBeam $A4 (Y) / $A5 (X)\n");
    t += QStringLiteral("  live         x %1  y %2\n").arg(s.x_beam).arg(s.y_beam);
    t += QStringLiteral("  latched      x %1  y %2\n").arg((int)s.x_beam_latched).arg((int)s.y_beam_latched);
    t += QStringLiteral("\nWrite lockouts (MAME's chip: reads give 0 and writes are ignored)\n");
    t += QStringLiteral("  objects $00-$7F   %1 (foreground on)\n").arg(s.lock_objects ? "locked" : "open");
    t += QStringLiteral("  grid    $C0-$FF   %1 (grid on)\n").arg(s.lock_grid ? "locked" : "open");
    dbgui::setText(m_ctlText, t);
    m_collision->set(0, s.col_mask);
    m_collision->set(1, s.col_result);
}

void VideoTab::refreshSound()
{
    const i8244_snapshot &s = m_snap;
    m_shift->set(0, s.shift & 0xFFFFFF);
    QString t;
    t += QStringLiteral("Shift register  $%1 ($A7 high, $A8, $A9 low)\n").arg(hx(s.shift & 0xFFFFFF, 6));
    t += QStringLiteral("$AA SOUND = $%1\n").arg(hx(s.snd_reg, 2));
    t += QStringLiteral("  enable       %1\n").arg(s.snd_enable ? "on" : "off");
    t += QStringLiteral("  clock        %1 (%2 Hz)\n").arg(s.snd_fast ? "fast (line rate / 4)" : "slow (line rate / 16)")
             .arg(s.shift_clock_hz, 0, 'f', 1);
    t += QStringLiteral("  mode         %1\n").arg(s.snd_noise ? "noise" : "loop (the 24 bits repeat)");
    t += QStringLiteral("  volume       %1 of 15\n").arg(s.snd_volume);
    t += QStringLiteral("  output       level %1\n\n").arg(s.sh_output);
    if (s.snd_noise)
        t += QStringLiteral("Tone          noise: no pitch\n");
    else if (s.pattern_period > 0 && s.freq_hz > 0)
        t += QStringLiteral("Tone          the pattern repeats every %1 bits: %2 Hz\n").arg(s.pattern_period).arg(s.freq_hz, 0, 'f', 1);
    else
        t += QStringLiteral("Tone          constant: no pitch\n");
    t += QStringLiteral("Shifter       prescaler %1, count %2%3%4\n").arg(s.sh_prescaler).arg(s.sh_count)
             .arg(s.sh_pending ? ", write pending" : "", s.sh_written ? ", written" : "");
    t += QStringLiteral("Sound IRQ     %1 ($A0 bit 2)\n").arg(s.sound_irq_en ? "enabled" : "off");
    dbgui::setText(m_soundText, t);
}

void VideoTab::refreshSheets()
{
    m_fontSheet->setPixmap(viewPixmap(O2DEBUG_VDC_FONT, 520));
    m_shapeSheet->setPixmap(viewPixmap(O2DEBUG_VDC_SHAPES, 520));
    m_paletteSheet->setPixmap(viewPixmap(O2DEBUG_VDC_PALETTE, 260));
}

/* ---- under the pointer ------------------------------------------------------------------------- */

void VideoTab::hover(int x, int y)
{
    if (x < 0) {
        m_hover->setText(QStringLiteral("Move the pointer over the picture to see the object under it."));
        m_hover->setStyleSheet(QStringLiteral("color: gray;"));
        m_image->setHighlight(QRect());
        return;
    }
    m_hover->setStyleSheet(QString());
    o2debug_hit hits[8];
    const int n = o2debug_vdc_hit_test(m_dbg, x, y, hits, 8);
    QString text = QStringLiteral("x=%1 y=%2: ").arg(x).arg(y);
    QRect box;
    if (n <= 0) {
        text += QStringLiteral("no object (the background or the grid)");
    } else {
        const o2debug_hit &h = hits[0];
        auto chars = [&](const i8244_char_info &c, const QString &name) {
            box = QRect(c.x, c.y, c.width, c.height);
            return QStringLiteral("%1: x=%2 y=%3 glyph=%4 colour=%5 ptr=$%6%7").arg(name).arg(c.x).arg(c.y)
                .arg(c.glyph).arg(colourText(c.color), hx(c.reg_ptr, 2))
                .arg(c.ntsc_hidden ? QStringLiteral(" (hidden: NTSC, Y < 14)") : QString());
        };
        if (h.kind == 0 && h.index >= 0 && h.index < 4) {
            const i8244_sprite_info &s = m_snap.sprites[h.index];
            box = QRect(s.x, s.y, s.width, s.height);
            text += QStringLiteral("Sprite %1: x=%2 y=%3 colour=%4 zoom=%5").arg(h.index).arg(s.x).arg(s.y)
                        .arg(colourText(s.color)).arg(s.zoom ? QStringLiteral("4 px/bit") : QStringLiteral("2 px/bit"));
        } else if (h.kind == 1 && h.index >= 0 && h.index < 12) {
            text += chars(m_snap.singles[h.index], QStringLiteral("Single %1").arg(h.index));
        } else if (h.kind == 2 && h.index >= 0 && h.index < 16) {
            text += chars(m_snap.quads[h.index / 4].chars[h.index % 4],
                          QStringLiteral("Quad %1, character %2").arg(h.index / 4).arg(h.index % 4));
        } else {
            text += QStringLiteral("object kind %1 index %2").arg(h.kind).arg(h.index);
        }
        if (n > 1) text += QStringLiteral(" (+%1 under it)").arg(n - 1);
    }
    m_hover->setText(text);
    m_image->setHighlight(box);
}

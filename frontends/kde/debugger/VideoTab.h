/*
 * The debugger's Video tab: everything the 8244/8245 knows.
 *
 * Left, the picture the chip draws -- the last frame, the registers
 * re-rendered with the layers and overlays you choose, or the frame as far as
 * the beam has got -- at 2x with nearest-neighbour scaling, and what is under
 * the pointer. Right, the chip in sections: the objects with their positions
 * and rendered bitmaps (4 sprites, 12 single characters, 4 quads of 4), the
 * grid as a diagram, every register $00-$FF decoded (editable while the
 * machine is stopped), the control, status, collision and colour registers,
 * the sound shift register, and the font, shape and palette sheets.
 *
 * Refreshed by the debugger window: live while the machine runs (at most ten
 * times a second) and on every stop, only while this tab shows.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "o2debug.h"

class BitsWidget;
class GridDiagram;
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QTabWidget;
class QTableWidget;
class VdcImage;

class VideoTab : public QWidget {
public:
    VideoTab(o2debug *debugger, QWidget *parent = nullptr);
    /* Pulls the chip's state and redraws what shows. */
    void refresh();

private:
    QWidget *buildLeft();
    QWidget *buildObjects();
    QWidget *buildGrid();
    QWidget *buildRegisters();
    QWidget *buildControl();
    QWidget *buildSound();
    QWidget *buildSheets();

    void refreshPicture();
    void refreshObjects();
    void refreshGrid();
    void refreshRegisters();
    void refreshControl();
    void refreshSound();
    void refreshSheets();
    void hover(int x, int y);
    QPixmap objectPixmap(int kind, int index, int scale);
    QPixmap viewPixmap(int view, int maxWidth);
    void loadPalette();
    unsigned layers() const;
    unsigned overlays() const;

    o2debug *m_dbg;
    i8244_snapshot m_snap;
    std::vector<uint32_t> m_px;
    uint32_t m_pal[16] = {};          /* 8 colours dim, then 8 bright */

    QComboBox *m_view = nullptr;
    QCheckBox *m_layer[7] = {};
    QCheckBox *m_overlay[6] = {};
    VdcImage *m_image = nullptr;
    QLabel *m_hover = nullptr;
    QLabel *m_beam = nullptr;
    QTabWidget *m_sections = nullptr;

    QTableWidget *m_sprites = nullptr, *m_singles = nullptr, *m_quads = nullptr;
    GridDiagram *m_gridDiagram = nullptr;
    QPlainTextEdit *m_gridText = nullptr;
    QTableWidget *m_regs = nullptr;
    bool m_regsFilling = false;
    std::vector<char> m_regHot = std::vector<char>(256, 0);   /* changed at the last look */
    QPlainTextEdit *m_ctlText = nullptr;
    BitsWidget *m_collision = nullptr;
    BitsWidget *m_shift = nullptr;
    QPlainTextEdit *m_soundText = nullptr;
    QLabel *m_fontSheet = nullptr, *m_shapeSheet = nullptr, *m_paletteSheet = nullptr;
};

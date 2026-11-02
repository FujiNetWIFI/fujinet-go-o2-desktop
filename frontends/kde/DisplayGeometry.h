/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * The 1861 paints 100 x 246 (its 64 x 128 picture inside the blanking MAME
 * keeps) across the whole of a 4:3 television, so on the TV a Odyssey 2
 * pixel is more than three times as wide as it is tall. Square pixels show
 * the raw buffer: a very tall, narrow picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "o2session.h"

inline QRectF displayRect(QSizeF avail, bool tvAspect)
{
    const double want = tvAspect ? 4.0 / 3.0
                                 : (double)O2SESSION_FB_WIDTH / (double)O2SESSION_FB_HEIGHT;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}

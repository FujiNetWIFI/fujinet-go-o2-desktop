/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * The 8244/8245 paints 360 x 243 across a 4:3 television, so on the TV a
 * pixel is a little narrower than it is tall (0.9 : 1). Square pixels show
 * the raw buffer at 360 : 243.
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

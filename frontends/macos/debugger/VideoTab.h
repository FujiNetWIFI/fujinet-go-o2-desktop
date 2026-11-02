/*
 * The debugger's Video tab: everything the 8244/8245 knows.
 *
 * Left, the picture the chip draws -- the last frame, the registers
 * re-rendered with the layers and overlays you choose, or the frame as far as
 * the beam has got -- with the object under the pointer named and outlined.
 * Right, the chip in sections: the objects with their positions and rendered
 * bitmaps (4 sprites, 12 single characters, 4 quads of 4), the grid as a
 * diagram, every register $00-$FF decoded (editable while the machine is
 * stopped), the control, status, collision and colour registers, the sound
 * shift register, and the font, shape and palette sheets.
 *
 * Refreshed by the debugger window: live while the machine runs (at most ten
 * times a second) and on every stop, only while this tab shows.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "o2debug.h"

@interface O2VideoTab : NSObject <NSTabViewDelegate>
- (instancetype)initWithDebugger:(o2debug *)dbg;
/* The tab's content, to put in the debugger's tab view. */
@property (nonatomic, readonly) NSView *view;
/* Pulls the chip's state and redraws what shows. */
- (void)refresh;
@end

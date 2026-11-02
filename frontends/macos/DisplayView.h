/*
 * The emulator display.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "o2session.h"

@interface O2DisplayView : NSView
- (instancetype)initWithSession:(o2session *)session;
/* YES: the 360x243 picture at the television's 4:3 (setting "aspect" 0, the
 * default); NO: square pixels. */
- (void)setTvAspect:(BOOL)tv;
- (void)setSmooth:(BOOL)smooth;
/* Starts or stops the frame clock to match whether the window can be seen
 * (on screen, not occluded, not miniaturized, the app not hidden). It follows
 * the window's own notifications; call it after ordering the window in. */
- (void)updateVisibility;
- (void)stop;
@end

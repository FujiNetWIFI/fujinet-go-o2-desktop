/*
 * The keypad panel: the Odyssey 2's two 10-key keypads side by side, the
 * console's CLEAR button and the session's actions, the connected gamepads
 * and the Map row, in a fixed-size floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "o2session.h"

@interface O2KeypadWindow : NSWindowController <NSWindowDelegate>
+ (void)toggleWithSession:(o2session *)session;
+ (BOOL)isVisible;
@end

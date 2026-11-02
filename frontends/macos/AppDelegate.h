/*
 * The application delegate: the window, the menu bar, and the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "o2session.h"

@interface O2AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
/* mediaPath: a file named on the command line (a cartridge, a BIOS or
 * anything for the SD folder), or NULL. */
- (instancetype)initWithSession:(o2session *)session mediaPath:(const char *)mediaPath;
@end

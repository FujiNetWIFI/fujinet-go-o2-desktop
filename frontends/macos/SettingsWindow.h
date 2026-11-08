/*
 * The Settings window: Machine, ROMs, Display, Audio, Input and FujiNet pages,
 * the same settings (keys and defaults) as the other frontends' Preferences.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "o2session.h"

/* What the window needs the app to do for it. */
@protocol O2SettingsDelegate <NSObject>
/* Live display changes (the app's menu and view follow). */
- (void)settingsSetAspect:(int)aspect;
- (void)settingsSetSmooth:(BOOL)smooth;
/* The ROMs page's buttons: pick files (folder NO) or a folder (YES) and import. */
- (void)settingsImportRoms:(BOOL)folder;
- (void)settingsOpenWebUI;
/* The window closed. needsRestart: a machine or host option changed, so the
 * console is power-cycled (o2session_restart) and a toast says so. */
- (void)settingsClosed:(BOOL)needsRestart;
@end

@interface O2SettingsWindow : NSObject <NSWindowDelegate, NSTabViewDelegate>
- (instancetype)initWithSession:(o2session *)session delegate:(id<O2SettingsDelegate>)delegate;
- (void)show;
/* The ROM list, the machine's ROM status and the Web UI line, from the session
 * (after an import, a restart). */
- (void)refresh;
/* The View menu changed the picture: the popup and the box follow. */
- (void)syncDisplay;
@end

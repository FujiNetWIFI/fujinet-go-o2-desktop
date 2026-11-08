/*
 * O2AppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "KeyForward.h"
#import "SettingsWindow.h"
#import "controllers/ControllersWindow.h"
#import "debugger/DebuggerWindow.h"
#import "help/HelpWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef O2_VERSION_STRING
#define O2_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go Odyssey 2"

#define WEBSITE_URL "https://fujinet.online/"
#define ISSUES_URL  "https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues"

/* How long a gamepad connect/disconnect or import message stays over the
 * picture. */
#define TOAST_SECONDS 3.0

#define BANNER_H 24.0

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

/* The open panels' filters: a type per extension (a dynamic one for an
 * extension nothing has registered, which still matches by extension). */
static NSArray<UTType *> *typesForExtensions(NSArray<NSString *> *exts)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    return types;
}

@class O2AppDelegate;

/* The content view sits between AppKit and the session: it forwards keys
 * and the drop, and leaves the display purely about pixels. */
@interface O2ContentView : NSView
@property (nonatomic) o2session *session;
@property (nonatomic, weak) O2AppDelegate *owner;
@end

@interface O2AppDelegate () <O2SettingsDelegate>
- (void)runSysaction:(int)sa;
- (void)loadMediaPaths:(NSArray<NSString *> *)paths;
- (BOOL)handleAppKey:(uint32_t)keysym;
@end

@implementation O2ContentView

- (BOOL)acceptsFirstResponder { return YES; }

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    /* Cmd shortcuts are the menus'. One no menu claims still arrives here,
     * but AppKit sends no keyUp while Command is down, so forwarding it
     * would leave the machine holding the key. */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = O2KeysymFromEvent(e);
    if (!ks) return;
    if ([self.owner handleAppKey:ks]) return;
    o2session_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = O2KeysymFromEvent(e);
    if (ks) o2session_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = O2KeysymFromFlagsChange(e, &down);
    if (ks) o2session_key(self.session, ks, down);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
        options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if ([urls count] == 0) return NO;
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in urls) [paths addObject:[u path]];
    /* after the drag session: what the drop does can be an alert */
    dispatch_async(dispatch_get_main_queue(), ^{ [self.owner loadMediaPaths:paths]; });
    return YES;
}
@end

@implementation O2AppDelegate {
    o2session *_session;
    const char *_mediaPath;
    NSString *_pendingMedia;     /* a file Finder handed over before launch finished */
    BOOL _launched;
    NSWindow *_window;
    O2DisplayView *_display;
    O2ContentView *_content;
    NSTimer *_statusTimer;
    NSTimer *_sysactTimer;
    BOOL _needsBiosShown;        /* the "needs the BIOS" alert, once per occurrence */
    NSString *_refusedCart;      /* a cartridge refused for want of a ROM: retried after an import */

    /* the transient message over the picture */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;
    /* the persistent "ROM missing" strip along the bottom */
    NSButton *_banner;
    unsigned _bannerMask;

    NSMenuItem *_aspectItem;
    NSMenuItem *_smoothItem;

    /* an open panel with an "All files" choice, while it runs */
    NSOpenPanel *_panel;
    NSArray<UTType *> *_panelTypes;

    O2SettingsWindow *_settings;
    O2ControllersWindow *_controllers;

    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
}

- (instancetype)initWithSession:(o2session *)session mediaPath:(const char *)mediaPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _mediaPath = mediaPath;
    return self;
}

- (NSString *)lastError
{
    const char *e = o2session_last_error(_session);
    return (e && e[0]) ? nsstr(e) : @"The machine is not running.";
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* The 360x243 picture at the TV's 4:3, three times its 243 lines high:
     * 972x729, a comfortable first window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 972, 729)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    /* NSWindow's default is to release itself on close, which ARC then does
     * a second time: the window is kept, and closing it quits (below) */
    [_window setReleasedWhenClosed:NO];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    O2SetHelpContext(_window, @"main");
    [_window center];

    _content = [[O2ContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    /* layer-backed, so the display's layer and the labels over it composite
     * in the order they were added */
    [_content setWantsLayer:YES];

    _display = [[O2DisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:o2session_get_int(_session, "aspect", 0) == 0];
    [_display setSmooth:o2session_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    /* The toast: a label over the top of the picture that fades. */
    _toast = [NSTextField labelWithString:@""];
    _toast.font = [NSFont boldSystemFontOfSize:13];
    _toast.textColor = NSColor.whiteColor;
    _toast.drawsBackground = YES;
    _toast.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.7];
    _toast.alignment = NSTextAlignmentCenter;
    _toast.alphaValue = 0.0;
    [_toast setFrame:NSMakeRect(0, [_content bounds].size.height - 34, [_content bounds].size.width, 26)];
    [_toast setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [_content addSubview:_toast];

    /* The banner: what is missing, along the bottom, until it is imported. A
     * click goes to Import BIOS. */
    _banner = [NSButton buttonWithTitle:@"" target:self action:@selector(importBios:)];
    [_banner setBordered:NO];
    [_banner setWantsLayer:YES];
    _banner.layer.backgroundColor = [O2AccentColor() CGColor];
    [_banner setFrame:NSMakeRect(0, 0, [_content bounds].size.width, BANNER_H)];
    [_banner setAutoresizingMask:NSViewWidthSizable];
    [_banner setHidden:YES];
    [_content addSubview:_banner];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];
    [_display updateVisibility];

    __weak O2AppDelegate *weakSelf = self;
    _controllers = [[O2ControllersWindow alloc] initWithSession:_session
                                                        appKeys:^BOOL(uint32_t ks) {
        return [weakSelf handleAppKey:ks];
    }];

    /* A file from the command line or from Finder is routed as a drop is: it
     * waits for the machine to be up, then a cartridge boots, a ROM is
     * imported, anything else goes to the SD folder. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;

    o2session_start_opts opts;
    o2session_default_opts(_session, &opts);
    const BOOL started = o2session_start(_session, &opts) == 0;
    _launched = YES;
    _toastGeneration = o2session_gamepad_generation(_session);
    if (!started) [self reportRefusal:[self lastError] title:@"Could not start"];
    if (media) [self loadMediaPaths:@[media]];

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("O2_OPEN_CONTROLLERS")) [_controllers toggle];
    if (getenv("O2_OPEN_DEBUGGER")) [O2DebuggerWindow showForSession:_session];
    if (getenv("O2_OPEN_SETTINGS")) [self showSettings:nil];

    /* The timers below run only while the machine's window can be seen: they
     * follow the same notices the display's frame clock does. */
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    for (NSString *name in @[NSWindowDidChangeOcclusionStateNotification, NSWindowDidMiniaturizeNotification,
                             NSWindowDidDeminiaturizeNotification, NSApplicationDidHideNotification,
                             NSApplicationDidUnhideNotification])
        [nc addObserver:self selector:@selector(visibilityChanged:) name:name object:nil];
    [self updateTitle];
    [self checkMissing];
    [self updateTimers];
}

/* What the 10 Hz timer does: system actions the gamepad thread resolved (it
 * cannot touch AppKit), gamepads coming and going, and what the running
 * machine is missing. */
- (void)sysactTick
{
    int sa;
    while (o2session_sysaction_take(_session, &sa)) [self runSysaction:sa];
    const unsigned gen = o2session_gamepad_generation(_session);
    if (gen != _toastGeneration) {
        _toastGeneration = gen;
        char msg[160];
        if (o2session_gamepad_last_event(_session, msg, sizeof msg) > 0)
            [self showToast:nsstr(msg)];
    }
    [self checkMissing];
}

- (BOOL)windowCanBeSeen:(NSWindow *)w
{
    return w && [w isVisible] && ![w isMiniaturized] &&
           ([w occlusionState] & NSWindowOcclusionStateVisible) && ![NSApp isHidden];
}

/* The status and system-action timers, and the console log's, run while their
 * windows can be seen; they catch up once when they start again. */
- (void)updateTimers
{
    const BOOL want = _launched && [self windowCanBeSeen:_window];
    if (want && !_sysactTimer) {
        __weak O2AppDelegate *weakSelf = self;
        _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf updateTitle]; }];
        _statusTimer.tolerance = 0.5;
        _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
            block:^(NSTimer *t) { (void)t; [weakSelf sysactTick]; }];
        _sysactTimer.tolerance = 0.05;
        [self updateTitle];
        [self sysactTick];
    } else if (!want && _sysactTimer) {
        [_statusTimer invalidate];
        [_sysactTimer invalidate];
        _statusTimer = nil;
        _sysactTimer = nil;
    }
    [self updateLogTimer];
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimers];
}

- (void)showToast:(NSString *)text
{
    if (!text) return;
    _toast.stringValue = text;
    _toast.alphaValue = 1.0;
    const NSUInteger serial = ++_toastSerial;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(TOAST_SECONDS * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        /* a newer message restarts the clock */
        if (serial != self->_toastSerial) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.5;
            self->_toast.animator.alphaValue = 0.0;
        } completionHandler:nil];
    });
}

/* ---- what the machine is missing ----------------------------------------------------- */

/* The strip along the bottom: the character set (text is blank without it)
 * and The Voice's ROMs. Only touched when the mask changes. */
- (void)updateBanner:(unsigned)missing
{
    const unsigned mask = missing & (O2_MISSING_FONT | O2_MISSING_VOICE);
    if (mask == _bannerMask) return;
    _bannerMask = mask;
    NSString *text = nil;
    if ((mask & O2_MISSING_FONT) && (mask & O2_MISSING_VOICE))
        text = @"Character set and The Voice's ROMs not imported -- text will be blank. Import BIOS…";
    else if (mask & O2_MISSING_FONT)
        text = @"Character set not imported -- text will be blank. Import BIOS…";
    else if (mask & O2_MISSING_VOICE)
        text = @"The Voice's speech ROMs are not imported -- it will be silent. Import BIOS…";
    if (!text) {
        [_banner setHidden:YES];
        return;
    }
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    [_banner setAttributedTitle:[[NSAttributedString alloc] initWithString:text attributes:@{
        NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
        NSForegroundColorAttributeName: NSColor.whiteColor,
        NSParagraphStyleAttributeName: para }]];
    [_banner setHidden:NO];
}

/* A game met the stand-in (CONFIG network-booted it with no BIOS imported):
 * the console is parked. Said once, as a sheet on the machine's window, until
 * the next power cycle clears it and a game trips it again. */
- (void)checkMissing
{
    const unsigned missing = o2session_missing(_session);
    [self updateBanner:missing];
    if (!(missing & O2_MISSING_BIOS)) {
        _needsBiosShown = NO;
        return;
    }
    if (_needsBiosShown) return;
    _needsBiosShown = YES;
    [self askForRoms:nil title:@"This cartridge needs the Odyssey 2 BIOS"];
}

/* The alert for a ROM the machine needs: Import BIOS… picks the files (and
 * retries the refused cartridge, or restarts into CONFIG), Not Now leaves it. */
- (void)askForRoms:(NSString *)detail title:(NSString *)title
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = detail.length ? detail
        : @"Machine ▸ Import BIOS… adds your own dump of it. Odyssey 2 games are written for the "
           "real BIOS, so only it runs them; CONFIG and the FujiNet apps run without it.";
    [a addButtonWithTitle:@"Import BIOS…"];
    [a addButtonWithTitle:@"Not Now"];
    a.showsHelp = YES;
    a.delegate = self;
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r != NSAlertFirstButtonReturn) return;
        dispatch_async(dispatch_get_main_queue(), ^{ [self importRomsFromPanel:NO]; });
    }];
}

/* The alert's Help button opens the nobios topic. */
- (BOOL)alertShowHelp:(NSAlert *)alert
{
    (void)alert;
    [[O2HelpWindow shared] showAt:@"nobios"];
    return YES;
}

/* The session refused to power up or to load something. When importing a ROM
 * would fix it (its message points at Import BIOS) the alert offers that,
 * and the cartridge is retried after; otherwise it is just said. */
- (void)reportRefusal:(NSString *)why title:(NSString *)plainTitle
{
    if (![why containsString:@"Import BIOS"]) {
        [self alert:plainTitle text:why];
        return;
    }
    NSString *title = [why containsString:@"Voice"] ? @"This cartridge needs The Voice's ROMs"
                    : [why containsString:@"G7400"] ? @"The Videopac+ G7400 needs its BIOS"
                    : @"This cartridge needs the Odyssey 2 BIOS";
    [self askForRoms:why title:title];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case O2_SYSACT_RESET_CONFIG:
        _refusedCart = nil;
        o2session_sysaction(_session, sa);
        break;
    case O2_SYSACT_DEBUG_STOP:
        /* the debugger window attaches, which stops the machine */
        [O2DebuggerWindow showForSession:_session];
        break;
    default: break;
    }
    [self updateTitle];
}

/* F9, F11 and F12 are the app's, not bindable; then a key bound to a system
 * action. YES when taken. */
- (BOOL)handleAppKey:(uint32_t)ks
{
    if (ks == O2_KEYSYM_F9) { [_controllers toggle]; return YES; }
    if (ks == O2_KEYSYM_F11) { [_window toggleFullScreen:nil]; return YES; }
    if (ks == O2_KEYSYM_F12) { [O2DebuggerWindow toggleForSession:_session]; return YES; }
    const int sa = o2session_key_sysaction(_session, ks);
    if (sa >= 0) { [self runSysaction:sa]; return YES; }
    return NO;
}

- (void)updateTitle
{
    NSString *state;
    char st[160];
    if (!o2session_is_running(_session)) {
        state = @"stopped";
    } else {
        o2session_cart_status(_session, st, sizeof st);
        /* the status dot: filled while the cartridge's link to FujiNet is up */
        const int linkUp = o2session_cart_link_up(_session);
        state = [NSString stringWithFormat:@"%@FujiNet: %s",
                 linkUp > 0 ? @"● " : (linkUp == 0 ? @"○ " : @""), st];
        const char *cart = o2session_cart_path(_session);
        if (cart && cart[0])
            state = [NSString stringWithFormat:@"%@ — %@", [nsstr(cart) lastPathComponent], state];
        if (o2session_is_paused(_session)) state = [state stringByAppendingString:@" — Paused"];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    NSString *title = [NSString stringWithFormat:@"%@ — %@", APP_TITLE, state];
    if (![[_window title] isEqualToString:title]) [_window setTitle:title];
}

/* Losing key status with keys held would leave the machine believing they
 * are still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    if (note.object == _window) o2session_release_all(_session);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_sysactTimer invalidate];
    [_logTimer invalidate];
    [_display stop];
    o2session_stop(_session);
    o2session_free(_session);
    _session = NULL;
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename
{
    (void)app;
    /* Opening a file from Finder launches the app and delivers the file
     * before applicationDidFinishLaunching:, when there is no machine yet
     * to load it into: hold it until there is. */
    if (!_launched) {
        _pendingMedia = filename;
        return YES;
    }
    [self loadMediaPaths:@[filename]];
    return YES;
}

/* ---- menu ------------------------------------------------------------------------- */

- (NSMenuItem *)item:(NSMenu *)menu title:(NSString *)title action:(SEL)sel key:(NSString *)key
{
    NSMenuItem *it = [menu addItemWithTitle:title action:sel keyEquivalent:key];
    [it setTarget:self];
    return it;
}

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [self item:appMenu title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:appMenu title:@"Settings…" action:@selector(showSettings:) key:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide " APP_TITLE action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *others = [appMenu addItemWithTitle:@"Hide Others"
                                            action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [others setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_TITLE action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [self item:machine title:@"Open Cartridge…" action:@selector(openCart:) key:@"o"];
    [self item:machine title:@"Eject Cartridge" action:@selector(ejectCart:) key:@""];
    [self item:machine title:@"Import Cartridge to SD…" action:@selector(importToSd:) key:@""];
    [self item:machine title:@"Import BIOS…" action:@selector(importBios:) key:@""];
    [self item:machine title:@"Import BIOS from a Folder…" action:@selector(importBiosFolder:) key:@""];
    [machine addItem:[NSMenuItem separatorItem]];
    [self item:machine title:@"Pause" action:@selector(togglePause:) key:@""];
    /* F1 and Escape are bindings (the console's RESET key, Reset to CONFIG),
     * not key equivalents: a menu equivalent would take the key before the
     * bindings table sees it and could not be remapped. The titles name
     * whatever key is bound now (see validateMenuItem:). */
    [self item:machine title:@"Reset" action:@selector(resetConsole:) key:@""];
    [self item:machine title:@"Reset to CONFIG" action:@selector(resetConfig:) key:@"r"];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Controllers (F9)" action:@selector(toggleControllers:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(o2session_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    _smoothItem = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [_smoothItem setState:(o2session_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
    /* F11 toggles it too, from the machine's window (see O2ContentView). */
    NSMenuItem *fs = [view addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [self item:fuji title:@"Web UI" action:@selector(openWebUI:) key:@""];
    [self item:fuji title:@"Console Log" action:@selector(showFujiNetLog:) key:@""];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    /* Window and Help: AppKit fills these in (the window list, the Help
     * search field) once they are registered as such. */
    NSMenuItem *windowItem = [[NSMenuItem alloc] init];
    NSMenu *windows = [[NSMenu alloc] initWithTitle:@"Window"];
    [windows addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windows addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windows addItem:[NSMenuItem separatorItem]];
    [windows addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
    [windowItem setSubmenu:windows];
    [bar addItem:windowItem];

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [self item:help title:APP_TITLE @" Help" action:@selector(openHelp:) key:@"?"];
    [help addItem:[NSMenuItem separatorItem]];
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[O2HelpWindow shared]];
}

/* ---- actions ------------------------------------------------------------------------ */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    if (text) [a setInformativeText:text];
    [a runModal];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSString *text = @"Magnavox Odyssey 2 and Philips Videopac with built-in FujiNet\n\n"
                      WEBSITE_URL "\n"
                      "Report issues: " ISSUES_URL "\n\n"
                      "Licensed under the GNU General Public License, version 3 or later "
                      "(GPL-3.0-or-later).\n\n"
                      "The emulator is MAME's Odyssey 2 / Videopac code -- the Intel 8048, the "
                      "8244/8245 video chip, the EF9340/41 and The Voice's SP0256 (BSD-3-Clause; "
                      "Peter Trauner, Wilbert Pol, hap, Couriersud, Joseph Zbiciak and others) -- "
                      "transposed to C. The FujiNet firmware and the cartridge: Thomas Cherryhomes "
                      "and the FujiNet contributors.";
    NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
        initWithString:text
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                          NSForegroundColorAttributeName: NSColor.labelColor,
                          NSParagraphStyleAttributeName: para }];
    /* the two addresses as links */
    for (NSString *url in @[@WEBSITE_URL, @ISSUES_URL]) {
        const NSRange r = [text rangeOfString:url];
        if (r.location != NSNotFound)
            [credits addAttribute:NSLinkAttributeName value:[NSURL URLWithString:url] range:r];
    }
    [NSApp orderFrontStandardAboutPanelWithOptions:@{
        NSAboutPanelOptionApplicationName: APP_TITLE,
        NSAboutPanelOptionApplicationVersion: nsstr(O2_VERSION_STRING),
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionCredits: credits,
    }];
}

/* A game while no BIOS is imported is refused with the reason: the session
 * says so; when importing the missing ROM would fix it (its message points at
 * Import BIOS), the alert offers that and the cartridge is retried after. */
- (void)openCartAtPath:(NSString *)path
{
    if (o2session_load_cart(_session, [path fileSystemRepresentation]) != 0) {
        NSString *why = [self lastError];
        if ([why containsString:@"Import BIOS"]) _refusedCart = path;
        [self reportRefusal:why title:@"Could not open"];
    } else {
        _refusedCart = nil;
    }
    [self updateTitle];
}

/* Open, a drop, Finder, the command line: ROM images and zips are imported
 * (together, one report), the first cartridge is opened, anything else goes
 * on FujiNet's SD card. */
- (void)loadMediaPaths:(NSArray<NSString *> *)paths
{
    NSMutableArray<NSString *> *roms = [NSMutableArray array];
    NSString *cart = nil;
    for (NSString *p in paths) {
        const char *fs = [p fileSystemRepresentation];
        if (o2session_media_is_rom(fs)) {
            [roms addObject:p];
        } else if (!cart && o2session_media_is_cartridge(fs)) {
            cart = p;
        } else {
            char dest[1024];
            if (o2session_import_media(_session, fs, dest, (int)sizeof dest) != 0) {
                [self alert:@"Import failed" text:[self lastError]];
                continue;
            }
            [self showToast:[NSString stringWithFormat:@"%@ copied to FujiNet's SD folder. Mount it from CONFIG.",
                             [nsstr(dest) lastPathComponent]]];
        }
    }
    if ([roms count]) [self importRomPaths:roms];
    if (cart) [self openCartAtPath:cart];
}

/* allName: offer an "All files" choice beside the filter, named so (an
 * accessory: AppKit's panels have no filter menu of their own); nil: the
 * filter alone. */
- (NSString *)pickFile:(NSString *)title types:(NSArray<NSString *> *)types allFilesBeside:(NSString *)allName
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setTitle:title];
    _panelTypes = typesForExtensions(types);
    p.allowedContentTypes = _panelTypes;
    if (allName) {
        NSPopUpButton *kind = [[NSPopUpButton alloc] init];
        [kind addItemsWithTitles:@[allName, @"All files"]];
        kind.target = self;
        kind.action = @selector(panelKindChanged:);
        NSStackView *acc = [NSStackView stackViewWithViews:@[[NSTextField labelWithString:@"Show"], kind]];
        acc.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        acc.edgeInsets = NSEdgeInsetsMake(8, 12, 8, 12);
        [acc setFrameSize:acc.fittingSize];
        p.accessoryView = acc;
        p.accessoryViewDisclosed = YES;
        _panel = p;
    }
    const NSModalResponse r = [p runModal];
    _panel = nil;
    if (r != NSModalResponseOK) return nil;
    return [[p URL] path];
}

- (void)panelKindChanged:(NSPopUpButton *)sender
{
    /* an empty list allows every file */
    _panel.allowedContentTypes = sender.indexOfSelectedItem == 0 ? _panelTypes : @[];
}

/* .bin, .rom or .o2: a raw image of the cartridge. */
- (NSString *)pickCartridge:(NSString *)title
{
    return [self pickFile:title types:@[@"bin", @"rom", @"o2"]
           allFilesBeside:@"Cartridge images (.bin .rom .o2)"];
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Open Cartridge"];
    if (path) [self openCartAtPath:path];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    _refusedCart = nil;
    if (o2session_eject(_session) != 0)
        [self alert:@"Could not eject" text:[self lastError]];
    [self updateTitle];
}

- (void)importToSd:(id)sender
{
    (void)sender;
    NSString *path = [self pickCartridge:@"Import Cartridge to SD"];
    if (!path) return;
    char dest[1024];
    if (o2session_import_cart_to_sd(_session, [path fileSystemRepresentation], dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ is on the SD host. Boot it from CONFIG.",
                     [nsstr(dest) lastPathComponent]]];
}

/* ---- the ROMs ---------------------------------------------------------------------------- */

/* Whatever was imported, the session's messages say what happened -- which ROM
 * it was, an unrecognised dump of the right size, the next power cycle. A
 * successful import takes effect at once when something was missing: the
 * cartridge that was refused is opened again, or the console power-cycles
 * (back onto the cartridge in the slot, or CONFIG). */
- (void)importRomPaths:(NSArray<NSString *> *)paths
{
    NSMutableArray<NSString *> *log = [NSMutableArray array];
    int imported = 0;
    for (NSString *p in paths) {
        char msg[2048];
        msg[0] = '\0';
        const int n = o2session_rom_import(_session, [p fileSystemRepresentation], msg, (int)sizeof msg);
        if (n > 0) imported += n;
        if (msg[0]) [log addObject:nsstr(msg)];
    }
    [_settings refresh];
    NSString *text = [log count] ? [log componentsJoinedByString:@"\n"]
                                 : (imported > 0 ? @"Imported." : @"None of these files is a ROM this app uses.");
    [self alert:(imported > 0 ? @"BIOS Imported" : @"Nothing Imported") text:text];
    if (imported <= 0) return;

    /* what is missing is a snapshot from the last power-up: a power cycle
     * with the new ROMs is what clears it (a machine that could not start has
     * none to clear) */
    if (_refusedCart) {
        NSString *cart = _refusedCart;
        _refusedCart = nil;
        [self openCartAtPath:cart];
    } else if (!o2session_is_running(_session) || o2session_missing(_session)) {
        if (o2session_restart(_session) != 0)
            [self reportRefusal:[self lastError] title:@"Could not start"];
        else
            [self showToast:@"The console was power-cycled with the new ROMs."];
    }
    [self updateTitle];
    [self checkMissing];
}

/* Machine > Import BIOS… (files and zips, any number) and Import BIOS from a
 * Folder…: the ROMs are recognised by checksum, so the files may be called
 * anything. */
- (void)importRomsFromPanel:(BOOL)folder
{
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setMessage:folder ? @"Import BIOS from a Folder (MAME's ROM folder works)"
                             : @"Import BIOS: ROM images or zips (MAME's videopac.zip and i8244.zip work)"];
    [panel setCanChooseDirectories:folder];
    [panel setCanChooseFiles:!folder];
    [panel setAllowsMultipleSelection:!folder];
    NSWindow *host = [NSApp keyWindow] ?: _window;
    [panel beginSheetModalForWindow:host completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK) return;
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSURL *u in [panel URLs]) [paths addObject:[u path]];
        /* after the sheet: what came in is an alert of its own */
        dispatch_async(dispatch_get_main_queue(), ^{ [self importRomPaths:paths]; });
    }];
}

- (void)importBios:(id)sender { (void)sender; [self importRomsFromPanel:NO]; }
- (void)importBiosFolder:(id)sender { (void)sender; [self importRomsFromPanel:YES]; }

/* ---- the console ---------------------------------------------------------------------------- */

- (void)togglePause:(id)sender
{
    (void)sender;
    o2session_set_paused(_session, !o2session_is_paused(_session));
    [self updateTitle];
}

/* The console's RESET key: pressed and released over a few frames by the
 * session, as the console samples it once a frame. */
- (void)resetConsole:(id)sender
{
    (void)sender;
    o2session_reset(_session);
}

- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:O2_SYSACT_RESET_CONFIG]; }

/* "Reset (F1)": the key bound to a target now, if any. */
- (NSString *)title:(NSString *)base forTarget:(int)target
{
    char key[32];
    const o2_binding b = o2session_binding_get(_session, target);
    if (!b.keysym || o2session_keysym_name(b.keysym, key, sizeof key) <= 0) return base;
    return [NSString stringWithFormat:@"%@ (%s)", base, key];
}

/* The console items' titles follow the bindings (the Controllers panel
 * remaps them). */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    if (a == @selector(resetConsole:)) {
        [item setTitle:[self title:@"Reset" forTarget:O2_TARGET_SWITCH(O2_SW_RESET)]];
        return o2session_is_running(_session);
    }
    if (a == @selector(resetConfig:)) {
        [item setTitle:[self title:@"Reset to CONFIG" forTarget:O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG)]];
        return o2session_is_running(_session);
    }
    if (a == @selector(togglePause:)) {
        [item setState:(o2session_is_paused(_session) ? NSControlStateValueOn : NSControlStateValueOff)];
        return o2session_is_running(_session);
    }
    if (a == @selector(ejectCart:))
        return o2session_is_running(_session);
    return YES;
}

- (void)toggleControllers:(id)sender { (void)sender; [_controllers toggle]; }
- (void)toggleDebugger:(id)sender { (void)sender; [O2DebuggerWindow toggleForSession:_session]; }

/* aspect 0: the TV's 4:3 (the default); 1: square pixels. */
- (void)setAspect:(int)aspect
{
    o2session_set_int(_session, "aspect", aspect);
    [_display setTvAspect:aspect == 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    [_settings syncDisplay];
}

- (void)setSmooth:(BOOL)on
{
    o2session_set_int(_session, "smooth", on ? 1 : 0);
    [_display setSmooth:on];
    [_smoothItem setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_settings syncDisplay];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:o2session_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    (void)sender;
    [self setSmooth:o2session_get_int(_session, "smooth", 0) == 0];
}

/* ---- settings ------------------------------------------------------------------------------- */

- (void)showSettings:(id)sender
{
    (void)sender;
    if (!_settings) _settings = [[O2SettingsWindow alloc] initWithSession:_session delegate:self];
    [_settings show];
}

- (void)settingsSetAspect:(int)aspect { [self setAspect:aspect]; }
- (void)settingsSetSmooth:(BOOL)smooth { [self setSmooth:smooth]; }
- (void)settingsImportRoms:(BOOL)folder { [self importRomsFromPanel:folder]; }
- (void)settingsOpenWebUI { [self openWebUI:nil]; }

/* A power cycle for the machine options, a full restart if a host option
 * changed: the session knows which. */
- (void)settingsClosed:(BOOL)needsRestart
{
    if (needsRestart) {
        if (o2session_restart(_session) != 0)
            [self reportRefusal:[self lastError] title:@"Could not start"];
        else
            [self showToast:@"Settings applied: the console was power-cycled."];
    }
    [_controllers refresh];
    [self updateTitle];
    [self checkMissing];
}

/* ---- FujiNet console log ----------------------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = o2session_fujinet_copy_log(_session, buf, sizeof buf);
    NSString *text = nil;
    if (n > 0) {
        /* UTF-8, or failing that Latin-1: one stray byte must not blank it */
        text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSUTF8StringEncoding];
        if (!text) text = [[NSString alloc] initWithBytes:buf length:(NSUInteger)n encoding:NSISOLatin1StringEncoding];
    }
    NSString *shown = text ?: @"(no FujiNet output yet)";
    /* nothing new: leave the text, and the reader's place in it, alone */
    if ([_logView.string isEqualToString:shown]) return;
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    [_logView setString:shown];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

/* Running while the log window can be seen; one refresh when it starts. */
- (void)updateLogTimer
{
    const BOOL want = [self windowCanBeSeen:_logWindow];
    if (want && !_logTimer) {
        _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                   selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        _logTimer.tolerance = 0.5;
        [self refreshLog:nil];
    } else if (!want && _logTimer) {
        [_logTimer invalidate];
        _logTimer = nil;
    }
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
    } else if (note.object == _window) {
        /* the machine's window is the app: with the debugger or Settings
         * still open, "the last window closed" would never come */
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (o2session_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Its console output is in the process that is running it."];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        [self refreshLog:nil];
        [self updateLogTimer];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 860, 564)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 860, 600)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
        NSButton *help = O2HelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    O2SetHelpContext(_logWindow, @"fujilog");
    [_logWindow center];

    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
    [self updateLogTimer];
}

/* For the window in front: Settings' page, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[O2HelpWindow shared] showFor:[NSApp keyWindow]];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (o2session_fujinet_external(_session)) {
        [self alert:@"FujiNet is external" text:@"Use the web UI of the FujiNet instance this app dials."];
        return;
    }
    if (!o2session_fujinet_running(_session)) {
        [self alert:@"FujiNet is not running" text:nil];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:nsstr(o2session_fujinet_webui_url(_session))]];
}
@end

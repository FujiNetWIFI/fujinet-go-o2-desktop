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

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DisplayView.h"
#import "KeyForward.h"
#import "keypad/KeypadWindow.h"
#import "debugger/DebuggerWindow.h"

#include <stdlib.h>
#include <string.h>

#ifndef O2_VERSION_STRING
#define O2_VERSION_STRING "unknown"
#endif

#define APP_TITLE @"FujiNet Go Odyssey 2"

#define WEBSITE_URL "https://fujinet.online/"
#define ISSUES_URL  "https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues"

/* How long a gamepad connect/disconnect message stays over the picture. */
#define TOAST_SECONDS 3.0

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

@interface O2AppDelegate ()
- (void)runSysaction:(int)sa;
- (void)loadMedia:(NSString *)path;
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
    if (ks == O2_KEYSYM_F9) { [O2KeypadWindow toggleWithSession:self.session]; return; }
    if (ks == O2_KEYSYM_F11) { [[self window] toggleFullScreen:nil]; return; }
    if (ks == O2_KEYSYM_F12) { [O2DebuggerWindow toggleForSession:self.session]; return; }

    const int sa = o2session_key_sysaction(self.session, ks);
    if (sa >= 0) { [self.owner runSysaction:sa]; return; }
    o2session_key(self.session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = O2KeysymFromEvent(e);
    if (ks) o2session_key(self.session, ks, 0);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; Left Alt is
 * keypad A's 0 by default, and a binding moved onto Shift or Control would
 * otherwise never press. */
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
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self.owner loadMedia:[(NSURL *)urls[0] path]];
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

    /* the transient gamepad hot-plug / import message */
    NSTextField *_toast;
    unsigned _toastGeneration;
    NSUInteger _toastSerial;

    NSMenuItem *_aspectItem;

    /* an open panel with an "All files" choice, while it runs */
    NSOpenPanel *_panel;
    NSArray<UTType *> *_panelTypes;

    NSWindow *_settingsWindow;
    BOOL _sessionDirty;
    NSPopUpButton *_padList, *_padPort, *_aspectPopup;
    NSTextField *_biosStatus;
    NSButton *_biosImport;
    NSTextField *_webUiLabel, *_externalField;
    NSTimer *_settingsTimer;
    unsigned _padGeneration;

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
    return (e && e[0]) ? [NSString stringWithUTF8String:e] : @"The machine is not running.";
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    /* The 100x246 picture at the TV's 4:3, three times its 246 lines high:
     * 984x738, a comfortable first window on any Mac. */
    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 984, 738)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:APP_TITLE];
    [_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [_window center];

    _content = [[O2ContentView alloc] initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    _content.owner = self;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

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

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:self];

    /* A file from the command line or from Finder is routed as a drop: a
     * cartridge boots straight away; a BIOS, or anything for the SD folder,
     * is handled once the machine is up. */
    NSString *media = _pendingMedia;
    if (!media && _mediaPath)
        media = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:_mediaPath
                                                                            length:strlen(_mediaPath)];
    _pendingMedia = nil;

    o2session_start_opts opts;
    o2session_default_opts(_session, &opts);
    if (media && !o2session_media_is_bios([media fileSystemRepresentation]) &&
        o2session_media_is_cartridge([media fileSystemRepresentation])) {
        opts.cart_path = [media fileSystemRepresentation];
        media = nil;
    }
    if (o2session_start(_session, &opts) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:[self lastError]];
        [a runModal];
    }
    _launched = YES;
    _toastGeneration = o2session_gamepad_generation(_session);
    if (media) [self loadMedia:media];

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("O2_OPEN_KEYPAD")) [O2KeypadWindow toggleWithSession:_session];
    if (getenv("O2_OPEN_DEBUGGER")) [O2DebuggerWindow showForSession:_session];
    if (getenv("O2_OPEN_SETTINGS")) [self showSettings:nil];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES
        block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    /* System actions the gamepad thread resolved (it cannot touch AppKit),
     * gamepads coming and going, and a game that needs the BIOS. */
    _sysactTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
        block:^(NSTimer *t) {
            (void)t;
            int sa;
            while (o2session_sysaction_take(self->_session, &sa)) [self runSysaction:sa];
            const unsigned gen = o2session_gamepad_generation(self->_session);
            if (gen != self->_toastGeneration) {
                self->_toastGeneration = gen;
                char msg[160];
                if (o2session_gamepad_last_event(self->_session, msg, sizeof msg) > 0)
                    [self showToast:[NSString stringWithUTF8String:msg]];
            }
            [self checkNeedsBios];
        }];
    [self updateTitle];
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

/* CONFIG network-booted a game while no BIOS is imported: the stand-in has
 * parked the console. Said once, as a sheet on the machine's window, until
 * the next power cycle clears it and a game trips it again. */
- (void)checkNeedsBios
{
    if (!o2session_needs_bios(_session)) {
        _needsBiosShown = NO;
        return;
    }
    if (_needsBiosShown) return;
    _needsBiosShown = YES;

    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"This cartridge needs the Magnavox Odyssey 2 BIOS";
    a.informativeText = @"Odyssey 2 games are written in the BIOS's own byte-code, so only the real BIOS "
                         "runs them; CONFIG and FujiNet apps run without it. Import your own dump of the "
                         "BIOS and the console restarts into CONFIG.";
    [a addButtonWithTitle:@"Import BIOS…"];
    [a addButtonWithTitle:@"Not Now"];
    [a beginSheetModalForWindow:_window completionHandler:^(NSModalResponse r) {
        if (r == NSAlertFirstButtonReturn) [self importBiosAndReset];
    }];
}

- (void)runSysaction:(int)sa
{
    switch (sa) {
    case O2_SYSACT_RESET_CONFIG:
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
            state = [NSString stringWithFormat:@"%@ — %@",
                     [[NSString stringWithUTF8String:cart] lastPathComponent], state];
    }
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"%@ — %@", APP_TITLE, state]];
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
    [_settingsTimer invalidate];
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
    [self loadMedia:filename];
    return YES;
}

/* ---- menu ------------------------------------------------------------------ */

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
    [machine addItem:[NSMenuItem separatorItem]];
    /* F3 and Escape are bindings (the console's CLEAR button, Reset to
     * CONFIG), not key equivalents: a menu equivalent would take the key
     * before the bindings table sees it and could not be remapped. The
     * titles name whatever key is bound now (see validateMenuItem:). */
    [self item:machine title:@"CLEAR" action:@selector(clearButton:) key:@""];
    [self item:machine title:@"Reset to CONFIG" action:@selector(resetConfig:) key:@"r"];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [self item:view title:@"Keypad (F9)" action:@selector(toggleKeypad:) key:@"j"];
    [self item:view title:@"Debugger (F12)" action:@selector(toggleDebugger:) key:@"d"];
    [view addItem:[NSMenuItem separatorItem]];
    _aspectItem = [self item:view title:@"TV Aspect" action:@selector(toggleAspect:) key:@""];
    [_aspectItem setState:(o2session_get_int(_session, "aspect", 0) == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [self item:view title:@"Smooth Scaling" action:@selector(toggleSmooth:) key:@""];
    [sm setState:(o2session_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff)];
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
    [self item:help title:@"About " APP_TITLE action:@selector(showAbout:) key:@""];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:windows];
    [NSApp setHelpMenu:help];
}

/* ---- actions ---------------------------------------------------------------- */

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
    NSString *text = @"Magnavox Odyssey 2 with built-in FujiNet\n\n"
                      WEBSITE_URL "\n"
                      "Report issues: " ISSUES_URL "\n\n"
                      "Licensed under the GNU General Public License, version 3 or later "
                      "(GPL-3.0-or-later).\n\n"
                      "The emulator is MAME's Magnavox Odyssey 2, CDP1802 and CDP1861 drivers "
                      "(Curt Coder, BSD-3-Clause), transposed to C. The FujiNet firmware and "
                      "the cartridge: Thomas Cherryhomes and the FujiNet contributors.";
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
        NSAboutPanelOptionApplicationVersion: [NSString stringWithUTF8String:O2_VERSION_STRING],
        NSAboutPanelOptionVersion: @"",
        NSAboutPanelOptionCredits: credits,
    }];
}

/* A game while no BIOS is imported is refused with the reason: the session
 * says so, and the alert passes it on. */
- (void)openCartAtPath:(NSString *)path
{
    if (o2session_load_cart(_session, [path fileSystemRepresentation]) != 0)
        [self alert:@"Could not open" text:[self lastError]];
    [self updateTitle];
}

/* Whatever was imported, the session's message says what happened -- which
 * ROM it was, the custom-CRC warning, the next power cycle. Returns the
 * image's index, or -1. */
- (int)importBiosAtPath:(NSString *)path
{
    char msg[512];
    const int idx = o2session_import_bios(_session, [path fileSystemRepresentation], msg, sizeof msg);
    [self alert:(idx >= 0 ? @"BIOS imported" : @"Import failed")
           text:(msg[0] ? [NSString stringWithUTF8String:msg] : [self lastError])];
    [self refreshMachineSettings];
    return idx;
}

- (void)loadMedia:(NSString *)path
{
    const char *fs = [path fileSystemRepresentation];
    if (o2session_media_is_bios(fs)) {
        [self importBiosAtPath:path];
        return;
    }
    if (o2session_media_is_cartridge(fs)) {
        [self openCartAtPath:path];
        return;
    }
    char dest[1024];
    if (o2session_import_media(_session, fs, dest, sizeof dest) != 0) {
        [self alert:@"Import failed" text:[self lastError]];
        return;
    }
    [self showToast:[NSString stringWithFormat:@"%@ copied to FujiNet's SD folder. Mount it from CONFIG.",
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
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

/* .st2, the Odyssey 2's own format; or a raw image of up to 1K at $0400. */
- (NSString *)pickCartridge:(NSString *)title
{
    return [self pickFile:title types:@[@"st2", @"bin", @"rom"] allFilesBeside:nil];
}

/* The 2K set, the 1K system ROM, or MAME's four 512-byte ROMs. */
- (NSString *)pickBios
{
    return [self pickFile:@"Import BIOS"
                    types:@[@"bin", @"rom", @"ic11", @"ic12", @"ic13", @"ic14"]
           allFilesBeside:@"BIOS images (.bin .rom .ic11-.ic14)"];
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
                     [[NSString stringWithUTF8String:dest] lastPathComponent]]];
}

/* The BIOS boots at the next power cycle. From Settings, that is the one the
 * window's closing does. */
- (void)importBios:(id)sender
{
    NSString *path = [self pickBios];
    if (!path) return;
    const int idx = [self importBiosAtPath:path];
    if (idx < 0 || sender != _biosImport) return;
    _sessionDirty = YES;
    [self refreshMachineSettings];
}

/* The "needs the BIOS" alert's action: import, then the power cycle that
 * boots it -- Reset to CONFIG, so the game can be chosen again. */
- (void)importBiosAndReset
{
    NSString *path = [self pickBios];
    if (!path || [self importBiosAtPath:path] < 0) return;
    if (o2session_reset_to_config(_session) != 0)
        [self alert:@"Could not start" text:[self lastError]];
    [self updateTitle];
}

/* Pressed and released over a few frames by the session: the console
 * samples CLEAR once a frame, so an instant's press would never be seen. */
- (void)clearButton:(id)sender
{
    (void)sender;
    o2session_clear(_session);
}

- (void)resetConfig:(id)sender { (void)sender; [self runSysaction:O2_SYSACT_RESET_CONFIG]; }

/* "CLEAR (F3)": the key bound to a target now, if any. */
- (NSString *)title:(NSString *)base forTarget:(int)target
{
    char key[32];
    const o2_binding b = o2session_binding_get(_session, target);
    if (!b.keysym || o2session_keysym_name(b.keysym, key, sizeof key) <= 0) return base;
    return [NSString stringWithFormat:@"%@ (%s)", base, key];
}

/* The console items' titles follow the bindings (the Keypad panel remaps
 * them). */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    const SEL a = [item action];
    if (a == @selector(clearButton:)) {
        [item setTitle:[self title:@"CLEAR" forTarget:O2_TARGET_SWITCH(O2_SW_CLEAR)]];
        return o2session_is_running(_session);
    }
    if (a == @selector(resetConfig:)) {
        [item setTitle:[self title:@"Reset to CONFIG" forTarget:O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG)]];
        return o2session_is_running(_session);
    }
    if (a == @selector(ejectCart:))
        return o2session_is_running(_session);
    return YES;
}

- (void)toggleKeypad:(id)sender { (void)sender; [O2KeypadWindow toggleWithSession:_session]; }
- (void)toggleDebugger:(id)sender { (void)sender; [O2DebuggerWindow toggleForSession:_session]; }

/* aspect 0: the TV's 4:3 (the default); 1: square pixels. */
- (void)setAspect:(int)aspect
{
    o2session_set_int(_session, "aspect", aspect);
    [_display setTvAspect:aspect == 0];
    [_aspectItem setState:(aspect == 0 ? NSControlStateValueOn : NSControlStateValueOff)];
    if (_aspectPopup) [_aspectPopup selectItemAtIndex:aspect];
}

- (void)toggleAspect:(id)sender
{
    (void)sender;
    [self setAspect:o2session_get_int(_session, "aspect", 0) == 0 ? 1 : 0];
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    o2session_set_int(_session, "smooth", on ? 1 : 0);
}

/* ---- settings ----------------------------------------------------------------
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The analog stick, the
 * picture and the volume apply live; the BIOS choice power-cycles the
 * machine, and the host options restart the session, when the window closes
 * (o2session_restart does whichever the changes need).
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField labelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    return n;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = o2session_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

/* What the BIOS row says: which images are imported, or that none is and
 * what that means. */
- (NSString *)biosStatusText
{
    if (!o2session_have_bios(_session))
        return @"Not imported — CONFIG and FujiNet apps run without it; games need it";
    BOOL set2k = NO, system1k = NO;
    for (int i = 0; i < o2session_bios_count(); i++) {
        const o2_bios_info *b = o2session_bios_info(i);
        if (!b || !o2session_bios_available(_session, i)) continue;
        if (!strcmp(b->name, "o2")) set2k = YES;
        else if (!strcmp(b->name, "system")) system1k = YES;
    }
    if (set2k) return @"Imported (2K set)";
    if (system1k) return @"Imported (1K system ROM)";
    return @"Imported (MAME's 512-byte ROMs)";
}

- (void)refreshMachineSettings
{
    if (!_settingsWindow) return;
    _biosStatus.stringValue = [self biosStatusText];
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = o2session_gamepad_count(_session);
    if (n == 0) [_padList addItemWithTitle:@"(no gamepads connected)"];
    for (int i = 0; i < n; i++) {
        char name[128];
        const int eff = o2session_gamepad_effective_port(_session, i);
        o2session_gamepad_name(_session, i, name, sizeof name);
        /* Menu items with equal titles collapse; the index keeps them apart. */
        [_padList addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [keypad %c]", i + 1, name, 'A' + eff]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])];
    }
    if (sel >= 0 && sel < n) [_padList selectItemAtIndex:sel];
    [self padSelected:nil];
}

- (void)padSelected:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < o2session_gamepad_count(_session))
        [_padPort selectItemAtIndex:o2session_gamepad_assignment(_session, (int)sel) + 1];
}

- (void)padPortChanged:(id)sender
{
    (void)sender;
    const NSInteger sel = _padList.indexOfSelectedItem;
    if (sel >= 0 && sel < o2session_gamepad_count(_session))
        o2session_gamepad_assign(_session, (int)sel, (int)_padPort.indexOfSelectedItem - 1);
    [self refreshPadList];
}

- (void)volumeChanged:(NSSlider *)sender
{
    o2session_set_volume(_session, (int)sender.integerValue);
}

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [self setAspect:(int)sender.indexOfSelectedItem];
}

- (void)refreshWebUi
{
    if (!_webUiLabel) return;
    const char *url = o2session_fujinet_webui_url(_session);
    _webUiLabel.stringValue = o2session_fujinet_external(_session)
        ? @"(FujiNet is external)"
        : (o2session_fujinet_running(_session) && url && url[0])
            ? [NSString stringWithUTF8String:url] : @"(FujiNet is not running)";
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:11520) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (o2session_parse_hostport(text.UTF8String, host, sizeof host, &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    o2session_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _sessionDirty = YES;
}

- (void)settingChanged:(id)sender
{
    NSButton *box = sender;
    const char *key = [box.identifier UTF8String];
    const int value = box.state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "analog_joystick")) {
        o2session_set_analog(_session, value);
        return;
    }
    o2session_set_int(_session, key, value);
    _sessionDirty = YES;
    if (!strcmp(key, "fujinet_external")) _externalField.enabled = value != 0;
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [self refreshMachineSettings];
        [self refreshWebUi];
        [self startSettingsTimer];
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _biosStatus = [NSTextField wrappingLabelWithString:@""];
    _biosStatus.preferredMaxLayoutWidth = 340;
    _biosImport = [NSButton buttonWithTitle:@"Import BIOS…" target:self action:@selector(importBios:)];
    NSStackView *biosRow = [NSStackView stackViewWithViews:@[_biosStatus, _biosImport]];
    biosRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    biosRow.alignment = NSLayoutAttributeCenterY;

    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Keypad A", @"Keypad B"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);

    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"TV (4:3)", @"Square pixels"]];
    [_aspectPopup selectItemAtIndex:(o2session_get_int(_session, "aspect", 0) ? 1 : 0)];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);

    NSSlider *volume = [NSSlider sliderWithValue:o2session_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;

    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    _externalField = [NSTextField textFieldWithString:
        @(o2session_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11520";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled = o2session_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    _webUiLabel = [NSTextField labelWithString:@""];
    _webUiLabel.selectable = YES;
    NSButton *webUiOpen = [NSButton buttonWithTitle:@"Open" target:self action:@selector(openWebUI:)];
    NSStackView *webUiRow = [NSStackView stackViewWithViews:@[_webUiLabel, webUiOpen]];
    webUiRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [self note:@"the BIOS applies with a power cycle when this window closes"] ],
        @[ [NSTextField labelWithString:@"BIOS"], biosRow ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"use_bios" title:@"Use the imported BIOS" fallback:1] ],
        @[ [NSTextField labelWithString:@""],
           [self note:@"off: the app's own stand-in always, which runs CONFIG and FujiNet apps but no games"] ],
        @[ [self sectionLabel:@"Display"], [self note:@"applied immediately"] ],
        @[ [NSTextField labelWithString:@"Picture"], _aspectPopup ],
        @[ [self sectionLabel:@"Audio"], [self note:@"the volume applies immediately; audio on/off restarts the session"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [NSTextField labelWithString:@"Volume"], volume ],
        @[ [self sectionLabel:@"Input"], [self note:@"applied immediately; gamepads on/off restarts the session"] ],
        @[ [NSTextField labelWithString:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks steer too" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
        @[ [NSTextField labelWithString:@"Gamepads"], padRow ],
        @[ [self sectionLabel:@"FujiNet"], [self note:@"applied by restarting the session when this window closes"] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [NSTextField labelWithString:@""], [self checkBoxForKey:"fujinet_external" title:@"Use external FujiNet" fallback:0] ],
        @[ [NSTextField labelWithString:@"Address:port"], _externalField ],
        @[ [NSTextField labelWithString:@"Web UI"], webUiRow ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSStackView *root = [NSStackView stackViewWithViews:@[grid]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 640, 580)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    _settingsWindow.contentView = root;
    [_settingsWindow center];

    [self refreshMachineSettings];
    [self refreshWebUi];
    [self startSettingsTimer];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

- (void)startSettingsTimer
{
    [_settingsTimer invalidate];
    _padGeneration = o2session_gamepad_generation(_session);
    [self refreshPadList];
    _settingsTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t;
        const unsigned gen = o2session_gamepad_generation(self->_session);
        if (gen != self->_padGeneration) { self->_padGeneration = gen; [self refreshPadList]; }
    }];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object != _settingsWindow) return;
    [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
    [_settingsTimer invalidate];
    _settingsTimer = nil;
    if (!_sessionDirty) return;
    _sessionDirty = NO;

    /* A power cycle for the machine options, a full restart if a host
     * option changed: the session knows which. */
    if (o2session_restart(_session) != 0)
        [self alert:@"Could not start" text:[self lastError]];
    else
        [self showToast:@"Settings applied: the console was power-cycled."];
    [self updateTitle];
}

/* ---- FujiNet console log ------------------------------------------------------- */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    static char buf[128 * 1024];
    const int n = o2session_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    const BOOL atEnd = !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                                   NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);
    NSString *text = n > 0 ? [NSString stringWithUTF8String:buf] : nil;
    [_logView setString:(text ?: @"(no FujiNet output yet)")];
    if (atEnd) [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
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
        if (!_logTimer)
            _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                                       selector:@selector(refreshLog:) userInfo:nil repeats:YES];
        return;
    }

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 860, 600)];
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

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
    _logWindow.contentView = scroll;
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self
                                               selector:@selector(refreshLog:) userInfo:nil repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
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
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:[NSString stringWithUTF8String:o2session_fujinet_webui_url(_session)]]];
}
@end

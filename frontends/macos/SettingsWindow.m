/*
 * O2SettingsWindow -- see the header.
 *
 * Same keys and defaults as the other frontends' Preferences, so a machine
 * configured in one comes up the same in another. The analog stick, the
 * picture and the volume apply live; the machine, the BIOS choices and the
 * host options power-cycle the console (o2session_restart) when the window
 * closes, which the app does when told the window closed with changes.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "SettingsWindow.h"
#import "help/HelpWindow.h"

#include <string.h>

#define WINDOW_W 640.0
#define WINDOW_H 540.0
#define NUM_PAGES 6

/* The Help topic of each page, in tab order (Display and Audio are one topic). */
static NSString *const kPageContexts[NUM_PAGES] = {
    @"prefs/Machine", @"prefs/ROMs", @"prefs/Display", @"prefs/Display", @"prefs/Input", @"prefs/FujiNet"
};

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

@implementation O2SettingsWindow {
    o2session *_session;
    __weak id<O2SettingsDelegate> _delegate;
    NSWindow *_window;
    BOOL _dirty;                 /* an option changed that a power cycle applies */

    NSPopUpButton *_machinePopup;
    NSTextField *_biosStatus, *_fontStatus, *_voiceStatus, *_machineWarning;
    NSMutableArray<NSTextField *> *_romStates;   /* by o2session_rom_item index */
    NSTextField *_romPath;
    NSPopUpButton *_aspectPopup;
    NSButton *_smoothBox;
    NSPopUpButton *_padList, *_padPort;
    NSTextField *_webUiLabel, *_externalField;
    NSTimer *_padTimer;
    unsigned _padGeneration;
}

- (instancetype)initWithSession:(o2session *)session delegate:(id<O2SettingsDelegate>)delegate
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _delegate = delegate;
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_padTimer invalidate];
}

/* ---- small builders ----------------------------------------------------------------- */

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)heading:(NSString *)text
{
    NSTextField *l = [NSTextField labelWithString:text];
    l.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return l;
}

- (NSTextField *)note:(NSString *)text
{
    NSTextField *n = [NSTextField wrappingLabelWithString:text];
    n.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    n.textColor = NSColor.secondaryLabelColor;
    n.preferredMaxLayoutWidth = 400;
    return n;
}

- (NSButton *)checkBoxForKey:(const char *)key title:(NSString *)title fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:@selector(settingChanged:)];
    box.state = o2session_get_int(_session, key, def) ? NSControlStateValueOn : NSControlStateValueOff;
    box.identifier = nsstr(key);
    return box;
}

/* A page: a two-column grid (labels trailing, controls leading) at the top
 * left of a plain view, followed by any extra views stacked under it, and the
 * page's Help button at the bottom left. */
- (NSView *)pageWithRows:(NSArray<NSArray<NSView *> *> *)rows extras:(NSArray<NSView *> *)extras
                    help:(NSString *)context
{
    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 10;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSMutableArray<NSView *> *views = [NSMutableArray array];
    for (NSView *v in extras) [views addObject:v];
    [views addObject:grid];
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *holder = [[NSView alloc] init];
    [holder addSubview:stack];
    [stack.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:20].active = YES;
    [stack.topAnchor constraintEqualToAnchor:holder.topAnchor constant:20].active = YES;

    NSButton *help = O2HelpButton(context);
    help.translatesAutoresizingMaskIntoConstraints = NO;
    [holder addSubview:help];
    [help.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:20].active = YES;
    [help.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-16].active = YES;
    return holder;
}

/* ---- the machine ---------------------------------------------------------------------- */

- (int)currentMachine
{
    const int m = o2session_get_int(_session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

- (void)machineChanged:(NSPopUpButton *)sender
{
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel < 0 || sel >= O2_MACHINE_COUNT) return;
    o2session_set_int(_session, "machine", (int)sel);
    _dirty = YES;
    [self refresh];
}

/* What each ROM row on the Machine page says for the machine chosen. */
- (void)refreshMachineStatus
{
    const int m = [self currentMachine];
    _biosStatus.stringValue = o2session_have_bios(_session, m)
        ? @"Imported"
        : @"Not imported: CONFIG and FujiNet apps run without it, games need it";
    _fontStatus.stringValue = o2session_have_font(_session, m)
        ? @"Imported"
        : @"Not imported: text is blank";
    _voiceStatus.stringValue = o2session_have_voice(_session)
        ? @"Imported" : @"Not imported";
    /* the G7400 will not start without its own BIOS: say so as it is picked */
    _machineWarning.stringValue = (m == O2_MACHINE_VIDEOPAC_PLUS && !o2session_have_bios(_session, m))
        ? @"The Videopac+ G7400 needs its BIOS: Machine ▸ Import BIOS…" : @"";
}

- (NSView *)machinePage
{
    _machinePopup = [[NSPopUpButton alloc] init];
    for (int i = 0; i < O2_MACHINE_COUNT; i++)
        [[_machinePopup menu] addItemWithTitle:nsstr(o2_machine_name(i)) action:nil keyEquivalent:@""];
    [_machinePopup selectItemAtIndex:[self currentMachine]];
    _machinePopup.target = self;
    _machinePopup.action = @selector(machineChanged:);

    _biosStatus = [self label:@""];
    _fontStatus = [self label:@""];
    _voiceStatus = [self label:@""];
    _machineWarning = [self label:@""];
    _machineWarning.textColor = NSColor.systemRedColor;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Machine"], [self note:@"applies with a power cycle when this window closes"] ],
        @[ [self label:@"Console"], _machinePopup ],
        @[ [self label:@""], _machineWarning ],
        @[ [self label:@""], [self checkBoxForKey:"use_bios" title:@"Use the imported BIOS" fallback:1] ],
        @[ [self label:@""],
           [self note:@"off: the app's own stand-in always, which runs CONFIG and FujiNet apps but no games"] ],
        @[ [self label:@""],
           [self checkBoxForKey:"voice" title:@"The Voice in front of the cartridge" fallback:0] ],
        @[ [self label:@""], [self note:@"needs The Voice's speech ROMs (the ROMs page)"] ],
        @[ [self heading:@"ROMs for this machine"], [self label:@""] ],
        @[ [self label:@"BIOS"], _biosStatus ],
        @[ [self label:@"Character set"], _fontStatus ],
        @[ [self label:@"The Voice"], _voiceStatus ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[0]];
}

/* ---- the ROMs ---------------------------------------------------------------------------- */

- (void)importClicked:(NSButton *)sender
{
    [_delegate settingsImportRoms:sender.tag != 0];
}

- (void)refreshRoms
{
    for (NSUInteger i = 0; i < [_romStates count]; i++)
        _romStates[i].stringValue = o2session_rom_item_available(_session, (int)i) ? @"Imported" : @"Not imported";
    _romPath.stringValue = [NSString stringWithFormat:@"Kept in %@", nsstr(o2session_roms_path(_session))];
}

- (NSView *)romsPage
{
    NSTextField *intro = [self note:@"The console's BIOS, the video chip's character set and The Voice's speech "
                                    @"ROMs are not part of the app. Import your own dumps from files, a zip or "
                                    @"a folder; they are recognised by their checksums, whatever they are called."];
    intro.preferredMaxLayoutWidth = 560;
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS…" target:self action:@selector(importClicked:)];
    files.tag = 0;
    NSButton *folder = [NSButton buttonWithTitle:@"Import from a Folder…" target:self action:@selector(importClicked:)];
    folder.tag = 1;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[files, folder]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 8;
    _romPath = [self label:@""];
    _romPath.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _romPath.textColor = NSColor.secondaryLabelColor;
    _romPath.selectable = YES;

    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    _romStates = [NSMutableArray array];
    NSString *group = nil;
    for (int i = 0; i < o2session_rom_item_count(); i++) {
        const o2_rom_item *item = o2session_rom_item(i);
        if (!item) {
            [_romStates addObject:[self label:@""]];
            continue;
        }
        if (!group || ![group isEqualToString:nsstr(item->group)]) {
            group = nsstr(item->group);
            [rows addObject:@[ [self heading:group], [self label:@""] ]];
        }
        NSTextField *state = [self label:@""];
        state.textColor = NSColor.secondaryLabelColor;
        [_romStates addObject:state];
        [rows addObject:@[ [self label:nsstr(item->name)], state ]];
    }
    return [self pageWithRows:rows extras:@[intro, buttons, _romPath] help:kPageContexts[1]];
}

/* ---- display, audio, input, FujiNet --------------------------------------------------------- */

- (void)aspectChanged:(NSPopUpButton *)sender
{
    [_delegate settingsSetAspect:(int)sender.indexOfSelectedItem];
}

- (NSView *)displayPage
{
    _aspectPopup = [[NSPopUpButton alloc] init];
    [_aspectPopup addItemsWithTitles:@[@"TV (4:3)", @"Square pixels"]];
    _aspectPopup.target = self;
    _aspectPopup.action = @selector(aspectChanged:);
    _smoothBox = [self checkBoxForKey:"smooth" title:@"Smooth scaling" fallback:0];
    [self syncDisplay];
    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Display"], [self note:@"applied immediately"] ],
        @[ [self label:@"Picture"], _aspectPopup ],
        @[ [self label:@""], _smoothBox ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[2]];
}

- (void)syncDisplay
{
    [_aspectPopup selectItemAtIndex:(o2session_get_int(_session, "aspect", 0) ? 1 : 0)];
    _smoothBox.state = o2session_get_int(_session, "smooth", 0) ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)volumeChanged:(NSSlider *)sender
{
    o2session_set_volume(_session, (int)sender.integerValue);
}

- (NSView *)audioPage
{
    NSSlider *volume = [NSSlider sliderWithValue:o2session_get_int(_session, "volume", 100)
                                        minValue:0 maxValue:100 target:self action:@selector(volumeChanged:)];
    [volume.widthAnchor constraintEqualToConstant:220].active = YES;
    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Audio"], [self note:@"the volume applies immediately; audio on/off power-cycles"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_audio" title:@"Audio" fallback:1] ],
        @[ [self label:@"Volume"], volume ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[3]];
}

- (void)refreshPadList
{
    const NSInteger sel = _padList.indexOfSelectedItem;
    [_padList removeAllItems];
    const int n = o2session_gamepad_count(_session);
    if (n == 0) [[_padList menu] addItemWithTitle:@"(no gamepads connected)" action:nil keyEquivalent:@""];
    for (int i = 0; i < n; i++) {
        char name[128];
        name[0] = '\0';
        const int eff = o2session_gamepad_effective_port(_session, i);
        o2session_gamepad_name(_session, i, name, (int)sizeof name);
        /* through the menu: a button's own addItemWithTitle: drops an earlier
         * item of the same title, and two pads can share a name */
        [[_padList menu] addItemWithTitle:(eff >= 0
            ? [NSString stringWithFormat:@"%d: %s  [stick %d]", i + 1, name, eff + 1]
            : [NSString stringWithFormat:@"%d: %s  [unused]", i + 1, name])
                                   action:nil keyEquivalent:@""];
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

- (NSView *)inputPage
{
    _padList = [[NSPopUpButton alloc] init];
    _padList.target = self;
    _padList.action = @selector(padSelected:);
    _padPort = [[NSPopUpButton alloc] init];
    [_padPort addItemsWithTitles:@[@"Automatic", @"Stick 1", @"Stick 2"]];
    _padPort.target = self;
    _padPort.action = @selector(padPortChanged:);
    NSStackView *padRow = [NSStackView stackViewWithViews:@[_padList, _padPort]];
    padRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"Input"], [self note:@"applied immediately; gamepads on/off power-cycles"] ],
        @[ [self label:@""],
           [self checkBoxForKey:"analog_joystick" title:@"Analog sticks steer too" fallback:1] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_gamepad" title:@"Gamepads" fallback:1] ],
        @[ [self label:@"Gamepads"], padRow ],
        @[ [self label:@""], [self note:@"Keys and gamepad buttons are re-mapped in the Controllers window (Map)."] ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[4]];
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
    if (o2session_parse_hostport(text.UTF8String, host, (int)sizeof host, &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    o2session_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _dirty = YES;
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    [_delegate settingsOpenWebUI];
}

- (void)refreshWebUi
{
    if (!_webUiLabel) return;
    const char *url = o2session_fujinet_webui_url(_session);
    _webUiLabel.stringValue = o2session_fujinet_external(_session)
        ? @"(FujiNet is external)"
        : (o2session_fujinet_running(_session) && url && url[0])
            ? nsstr(url) : @"(FujiNet is not running)";
}

- (NSView *)fujinetPage
{
    _externalField = [NSTextField textFieldWithString:nsstr(o2session_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11520";
    [_externalField.widthAnchor constraintEqualToConstant:240].active = YES;
    _externalField.enabled = o2session_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    _webUiLabel = [self label:@""];
    _webUiLabel.selectable = YES;
    NSButton *open = [NSButton buttonWithTitle:@"Open" target:self action:@selector(openWebUI:)];
    NSStackView *webUiRow = [NSStackView stackViewWithViews:@[_webUiLabel, open]];
    webUiRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self heading:@"FujiNet"], [self note:@"applied by power-cycling when this window closes"] ],
        @[ [self label:@""], [self checkBoxForKey:"enable_fujinet" title:@"Enable FujiNet" fallback:1] ],
        @[ [self label:@""], [self checkBoxForKey:"fujinet_external" title:@"Use external FujiNet" fallback:0] ],
        @[ [self label:@"Address:port"], _externalField ],
        @[ [self label:@"Web UI"], webUiRow ],
    ];
    return [self pageWithRows:rows extras:@[] help:kPageContexts[5]];
}

/* A checkbox. The picture and the analog stick apply at once; every other
 * option takes a power cycle, when the window closes. */
- (void)settingChanged:(id)sender
{
    NSButton *box = sender;
    const char *key = [box.identifier UTF8String];
    const int value = box.state == NSControlStateValueOn ? 1 : 0;

    if (!strcmp(key, "analog_joystick")) {
        o2session_set_analog(_session, value);
        o2session_set_int(_session, key, value);
        return;
    }
    if (!strcmp(key, "smooth")) {
        [_delegate settingsSetSmooth:value != 0];
        return;
    }
    o2session_set_int(_session, key, value);
    _dirty = YES;
    if (!strcmp(key, "fujinet_external")) _externalField.enabled = value != 0;
}

/* ---- the window --------------------------------------------------------------------------------- */

- (void)refresh
{
    if (!_window) return;
    [self refreshMachineStatus];
    [self refreshRoms];
    [self refreshWebUi];
}

/* Hot-plugged gamepads, twice a second, while the window can be seen. */
- (void)updatePadTimer
{
    const BOOL want = _window && [_window isVisible] && ![_window isMiniaturized] &&
                      ([_window occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_padTimer) {
        _padGeneration = o2session_gamepad_generation(_session);
        [self refreshPadList];
        __weak O2SettingsWindow *weakSelf = self;
        _padTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
            (void)t;
            O2SettingsWindow *s = weakSelf;
            if (!s) return;
            const unsigned gen = o2session_gamepad_generation(s->_session);
            if (gen != s->_padGeneration) {
                s->_padGeneration = gen;
                [s refreshPadList];
            }
        }];
        _padTimer.tolerance = 0.25;
    } else if (!want && _padTimer) {
        [_padTimer invalidate];
        _padTimer = nil;
    }
}

- (void)visibilityChanged:(NSNotification *)note
{
    (void)note;
    [self updatePadTimer];
}

/* The Help button and Command+? open the page being shown. */
- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    const NSInteger i = [tabView indexOfTabViewItem:item];
    if (i >= 0 && i < NUM_PAGES) O2SetHelpContext(_window, kPageContexts[i]);
}

- (void)show
{
    if (!_window) {
        NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)];
        NSArray *pages = @[ @[@"Machine", [self machinePage]], @[@"ROMs", [self romsPage]],
                            @[@"Display", [self displayPage]], @[@"Audio", [self audioPage]],
                            @[@"Input", [self inputPage]], @[@"FujiNet", [self fujinetPage]] ];
        for (NSArray *page in pages) {
            NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
            item.label = page[0];
            item.view = page[1];
            [tabs addTabViewItem:item];
        }
        tabs.delegate = self;
        _window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, WINDOW_W, WINDOW_H)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _window.title = @"Settings";
        _window.releasedWhenClosed = NO;
        _window.delegate = self;
        _window.contentView = tabs;
        O2SetHelpContext(_window, kPageContexts[0]);
        [_window center];
        for (NSString *name in @[NSWindowDidChangeOcclusionStateNotification,
                                 NSWindowDidMiniaturizeNotification, NSWindowDidDeminiaturizeNotification])
            [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(visibilityChanged:)
                                                         name:name object:_window];
    }
    [self refresh];
    [self refreshPadList];
    [_window makeKeyAndOrderFront:nil];
    [self updatePadTimer];
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    [_window makeFirstResponder:nil];   /* commit a field edit */
    [_padTimer invalidate];
    _padTimer = nil;
    const BOOL dirty = _dirty;
    _dirty = NO;
    [_delegate settingsClosed:dirty];
}
@end

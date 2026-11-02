/*
 * O2KeypadWindow -- the AppKit keypad panel: the Odyssey 2's two 10-key
 * keypads side by side, laid out as on the console (1 2 3 / 4 5 6 / 7 8 9,
 * 0 under the 8), the console's CLEAR button and the session's actions, the
 * gamepads that are connected and which keypad each drives, and the Map row.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp
 * rather than an action on click: a key is HELD, and the console scans its
 * keypads once a frame (OUT 2 latches a key, EF3/EF4 report it), so a value
 * present only for the instant of a click falls between frames. mouseUp
 * arrives even when the pointer has left the button (AppKit tracks the drag
 * for the view that got mouseDown), so dragging off cannot strand the
 * machine with a key held.
 *
 * Whatever holds a key -- the keyboard, a gamepad or a click here -- the
 * button lights up in the accent colour, from o2session_buttons_held and
 * o2session_switch_held.
 *
 * A utility panel of fixed size (no resize mask). Singleton, ordered out
 * rather than closed, so its position survives.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#import "../KeyForward.h"

#include <string.h>

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
 * pad button. */
static int g_mapState = -2;
static O2KeypadWindow *g_singleton;
static o2session *g_session;

#define MAX_PAD_ROWS 4

/* NOT named `target`: NSControl already has a `target` property. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@property (nonatomic, strong) NSFont *faceFont;
@property (nonatomic) BOOL down;
@property (nonatomic) BOOL lit;
- (void)setLitState:(BOOL)lit;
- (void)showText:(NSString *)text;
@end

@implementation PadButton
/* The face large, or a binding in Map mode small. The tan is pale, so a lit
 * button's text goes dark to stay legible on it. */
- (void)showText:(NSString *)text
{
    text = text ?: @"";
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    NSFont *font = ([text isEqualToString:self.face] && self.faceFont) ? self.faceFont
                                                                       : [NSFont systemFontOfSize:11];
    self.attributedTitle = [[NSAttributedString alloc] initWithString:text attributes:@{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: self.lit ? NSColor.blackColor : NSColor.controlTextColor,
        NSParagraphStyleAttributeName: para }];
}

- (void)setLitState:(BOOL)lit
{
    if (lit == self.lit) return;
    self.lit = lit;
    self.bezelColor = lit ? O2AccentColor() : nil;
    [self showText:self.title];
}

- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"O2PadMapTarget" object:nil];
        return;
    }
    if (g_mapState >= 0) return;
    self.down = YES;
    [self setLitState:YES];
    if (self.padTarget >= O2_TARGET_SYSACT(0)) return;   /* fires on release */
    /* the keys and CLEAR are held while the mouse is */
    o2session_press(g_session, self.padTarget, 1);
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!self.down) return;
    self.down = NO;
    [self setLitState:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= O2_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not power-cycle the machine. Posted, so the
         * application runs them exactly as it runs a gamepad's. */
        o2session_sysaction_post(g_session, self.padTarget - O2_TARGET_SYSACT(0));
        return;
    }
    o2session_press(g_session, self.padTarget, 0);
}
@end

/* The keypad's body: a dark rounded rectangle the keys sit on, as they sit
 * in the console's case. */
@interface PadBodyView : NSView
@end

@implementation PadBodyView
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect([self bounds], 1, 1)
                                                         xRadius:10 yRadius:10];
    [[NSColor colorWithWhite:0.16 alpha:1.0] setFill];
    [path fill];
    [[NSColor colorWithWhite:0.40 alpha:1.0] setStroke];
    [path stroke];
}
@end

/* The console row: the console's one button, then the session's actions. */
#define CONSOLE_BUTTONS 3

@implementation O2KeypadWindow {
    NSMutableArray<PadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    NSTextField *_noPads;
    unsigned _padGeneration;
    NSTimer *_captureTimer;
    NSTimer *_liveTimer;
}

#define KEY_W    72.0
#define KEY_H    34.0
#define GAP       6.0
#define BODY_PAD 14.0
#define PAD_GAP  24.0      /* between keypad A and keypad B */
#define GRID_W (3 * KEY_W + 2 * GAP)
#define GRID_H (4 * KEY_H + 3 * GAP)
#define PAD_W  (BODY_PAD + GRID_W + BODY_PAD)
#define PAD_H  (BODY_PAD + GRID_H + BODY_PAD)
#define CONTENT_W (2 * PAD_W + PAD_GAP)
#define MARGIN  12.0
#define ROW_H   26.0

- (PadButton *)buttonWithFace:(NSString *)face font:(NSFont *)font target:(int)target frame:(NSRect)frame
{
    PadButton *b = [[PadButton alloc] initWithFrame:frame];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    b.faceFont = font;
    [b showText:face];
    /* Not focusable: clicking a key must not steal the key window's first
     * responder. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

/* The key at (col, row) of a keypad whose grid's top-left is (x, top);
 * AppKit's y grows upwards, so rows are placed by their bottom edge. */
static NSRect cell(CGFloat x, CGFloat top, int col, int row)
{
    return NSMakeRect(x + col * (KEY_W + GAP), top - (row + 1) * KEY_H - row * GAP, KEY_W, KEY_H);
}

/* One keypad, laid out downwards from topY at x0. Returns the y below it. */
- (CGFloat)buildKeypad:(int)pad intoView:(NSView *)parent x:(CGFloat)x0 topY:(CGFloat)topY
{
    /* The console's layout: 1 2 3 / 4 5 6 / 7 8 9, and 0 alone under 8. */
    static const int layout[4][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { -1, 0, -1 } };
    CGFloat y = topY;

    NSTextField *title = [NSTextField labelWithString:(pad ? @"Keypad B" : @"Keypad A")];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 20, 120, 16)];
    [parent addSubview:title];
    y -= ROW_H + GAP;

    PadBodyView *body = [[PadBodyView alloc] initWithFrame:NSMakeRect(x0, y - PAD_H, PAD_W, PAD_H)];
    [parent addSubview:body];

    /* the keys, in the body's own coordinates */
    const CGFloat top = PAD_H - BODY_PAD;
    NSFont *digit = [NSFont boldSystemFontOfSize:15];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 3; col++) {
            const int key = layout[row][col];
            if (key < 0) continue;
            [body addSubview:[self buttonWithFace:[NSString stringWithFormat:@"%d", key] font:digit
                                           target:O2_TARGET_PORT(pad, O2_ACT_0 + key)
                                            frame:cell(BODY_PAD, top, col, row)]];
        }
    return y - PAD_H;
}

- (instancetype)init
{
    const CGFloat width = MARGIN + CONTENT_W + MARGIN;
    const CGFloat height = MARGIN
        + ROW_H + GAP + PAD_H + MARGIN            /* the two keypads, side by side */
        + 18 + KEY_H + MARGIN                     /* console row */
        + 18 + MAX_PAD_ROWS * ROW_H + MARGIN      /* gamepads */
        + 30 + MARGIN;                            /* map row */

    NSPanel *win = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Keypad"];
    /* Floats above the machine's window and stays out of the way. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];
    [win setBecomesKeyOnlyIfNeeded:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;
    [win setDelegate:self];

    _buttons = [NSMutableArray array];
    NSView *content = [win contentView];
    CGFloat y = height - MARGIN;
    [self buildKeypad:0 intoView:content x:MARGIN topY:y];
    y = [self buildKeypad:1 intoView:content x:MARGIN + PAD_W + PAD_GAP topY:y] - MARGIN;

    /* The console's CLEAR button and the session's actions, three across. */
    NSTextField *consoleLabel = [NSTextField labelWithString:@"Console"];
    [consoleLabel setFont:[NSFont boldSystemFontOfSize:12]];
    [consoleLabel setFrame:NSMakeRect(MARGIN, y - 16, 120, 16)];
    [content addSubview:consoleLabel];
    y -= 18;
    static const struct { NSString *__unsafe_unretained face; int target; } console[CONSOLE_BUTTONS] = {
        { @"CLEAR",           O2_TARGET_SWITCH(O2_SW_CLEAR) },
        { @"Reset to CONFIG", O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG) },
        { @"Debugger Stop",   O2_TARGET_SYSACT(O2_SYSACT_DEBUG_STOP) },
    };
    const CGFloat cw = (CONTENT_W - (CONSOLE_BUTTONS - 1) * GAP) / CONSOLE_BUTTONS;
    NSFont *wide = [NSFont systemFontOfSize:NSFont.systemFontSize];
    for (int i = 0; i < CONSOLE_BUTTONS; i++)
        [content addSubview:[self buttonWithFace:console[i].face font:wide target:console[i].target
                                           frame:NSMakeRect(MARGIN + i * (cw + GAP), y - KEY_H, cw, KEY_H)]];
    y -= KEY_H + MARGIN;

    /* The gamepads: which keypad each one drives. */
    NSTextField *pads = [NSTextField labelWithString:@"Gamepads"];
    [pads setFont:[NSFont boldSystemFontOfSize:12]];
    [pads setFrame:NSMakeRect(MARGIN, y - 16, 120, 16)];
    [content addSubview:pads];
    y -= 18;
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_noPads setFrame:NSMakeRect(MARGIN, y - 20, CONTENT_W, 18)];
    [content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat ry = y - (i + 1) * ROW_H;
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setFrame:NSMakeRect(MARGIN, ry + 4, CONTENT_W - 170, 18)];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [content addSubview:_padName[i]];
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(MARGIN + CONTENT_W - 160, ry, 160, 24)];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Keypad A", @"Keypad B"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [content addSubview:_padAssign[i]];
    }
    y -= MAX_PAD_ROWS * ROW_H + MARGIN;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(MARGIN, y - 30, 70, 30)];
    [_mapButton setRefusesFirstResponder:YES];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(MARGIN + 76, y - 30, 90, 30)];
    [defaults setRefusesFirstResponder:YES];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(MARGIN + 176, y - 24, width - MARGIN - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setLineBreakMode:NSLineBreakByTruncatingTail];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refresh)
                                                 name:@"O2PadMapTarget" object:nil];
    _padGeneration = (unsigned)-1;
    [self refresh];
    [self refreshPads];
    return self;
}

/* ---- the controls ------------------------------------------------------------ */

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    o2session_gamepad_assign(g_session, (int)sender.tag, (int)sender.indexOfSelectedItem - 1);
    [self refreshPads];
}

- (void)refreshPads
{
    _padGeneration = o2session_gamepad_generation(g_session);
    const int n = o2session_gamepad_count(g_session);
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < n;
        _padName[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        o2session_gamepad_name(g_session, i, name, sizeof name);
        const int eff = o2session_gamepad_effective_port(g_session, i);
        _padName[i].stringValue = eff >= 0
            ? [NSString stringWithFormat:@"%s — keypad %c", name, 'A' + eff]
            : [NSString stringWithFormat:@"%s — unused", name];
        [_padAssign[i] selectItemAtIndex:o2session_gamepad_assignment(g_session, i) + 1];
    }
}

/* The live part, twenty times a second while the panel is up: what is held,
 * and whether a gamepad came or went. */
- (void)tick
{
    if (o2session_gamepad_generation(g_session) != _padGeneration) [self refreshPads];
    if (g_mapState != -2) return;
    const unsigned held[2] = { o2session_buttons_held(g_session, 0), o2session_buttons_held(g_session, 1) };
    for (PadButton *b in _buttons) {
        if (b.down || b.padTarget >= O2_TARGET_SYSACT(0)) continue;
        if (b.padTarget >= O2_TARGET_SWITCH(0)) {
            [b setLitState:o2session_switch_held(g_session, b.padTarget - O2_TARGET_SWITCH(0)) != 0];
            continue;
        }
        const int pad = b.padTarget / O2_ACT_PER_PORT;
        const int key = b.padTarget % O2_ACT_PER_PORT;
        [b setLitState:(held[pad] & (1u << key)) != 0];
    }
}

/* ---- Map mode ---------------------------------------------------------------- */

- (void)toggleMap:(id)sender
{
    (void)sender;
    _hint.stringValue = @"";
    [self setMapState:(g_mapState == -2) ? -1 : -2];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    o2session_bindings_reset(g_session);
    _hint.stringValue = @"Default bindings restored";
    [self refresh];
}

- (void)setMapState:(int)state
{
    g_mapState = state;
    [_captureTimer invalidate];
    _captureTimer = nil;
    if (state >= 0) {
        o2session_gamepad_capture_begin(g_session);
        _captureTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [self pollCapture];
        }];
    } else {
        o2session_gamepad_capture_cancel(g_session);
        if (state == -2) _hint.stringValue = @"";
    }
    [self refresh];
}

- (void)pollCapture
{
    int button;
    if (g_mapState < 0) return;
    if (o2session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        const int target = g_mapState;
        o2session_binding_set_button(g_session, target, button, stolen, sizeof stolen);
        [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", o2_target_name(target),
                                         o2_pad_button_name(button), stolen]
            : [NSString stringWithFormat:@"%s: %s", o2_target_name(target), o2_pad_button_name(button)];
    }
}

/* In Map mode every button shows its binding; otherwise its face. */
- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = g_mapState == -2 ? nil : O2AccentColor();
    if (g_mapState == -1 && _hint.stringValue.length == 0)
        [_hint setStringValue:@"Click a control to remap"];
    else if (g_mapState >= 0)
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key or gamepad button for %s",
                               o2_target_name(g_mapState)]];

    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const o2_binding bind = o2session_binding_get(g_session, b.padTarget);
            char key[32];
            o2session_keysym_name(bind.keysym, key, sizeof key);
            NSString *text = key[0] ? [NSString stringWithUTF8String:key] : @"—";
            if (bind.button != O2_PAD_BTN_NONE)
                text = [text stringByAppendingFormat:@" / %s", o2_pad_button_name(bind.button)];
            /* Lit first: the text's colour follows it. A key's button is
             * narrow, so the tooltip carries the binding in full too. */
            [b setLitState:(b.padTarget == g_mapState)];
            [b showText:text];
            [b setToolTip:[NSString stringWithFormat:@"%s: %@", o2_target_name(b.padTarget), text]];
        } else {
            if (!b.down) [b setLitState:NO];
            [b showText:b.face];
            [b setToolTip:nil];
        }
    }
}

/* ---- the keyboard ------------------------------------------------------------- */

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key -- except in Map mode, where the next key
 * pressed is the binding. */
- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = O2KeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            char stolen[128], name[32];
            const int target = g_mapState;
            o2session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
            o2session_keysym_name(ks, name, sizeof name);
            [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
            _hint.stringValue = stolen[0]
                ? [NSString stringWithFormat:@"%s: %s (taken from %s)", o2_target_name(target), name, stolen]
                : [NSString stringWithFormat:@"%s: %s", o2_target_name(target), name];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;
    /* Cmd shortcuts are the menus' (and AppKit sends no keyUp under Cmd). */
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    if (ks == O2_KEYSYM_F9) { [O2KeypadWindow toggleWithSession:g_session]; return; }

    const int sa = o2session_key_sysaction(g_session, ks);
    if (sa >= 0) { o2session_sysaction_post(g_session, sa); return; }
    o2session_key(g_session, ks, 1);
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = O2KeysymFromEvent(e);
    if (ks) o2session_key(g_session, ks, 0);
}

/* Modifiers bind too (Shift, Control and Option are keys like any other --
 * Left Alt is keypad A's 0 by default). */
- (void)flagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = O2KeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (g_mapState >= 0) {
        if (!down) return;
        char stolen[128], name[32];
        const int target = g_mapState;
        o2session_binding_set_key(g_session, target, ks, stolen, sizeof stolen);
        o2session_keysym_name(ks, name, sizeof name);
        [self setMapState:-1];
        _hint.stringValue = stolen[0]
            ? [NSString stringWithFormat:@"%s: %s (taken from %s)", o2_target_name(target), name, stolen]
            : [NSString stringWithFormat:@"%s: %s", o2_target_name(target), name];
        return;
    }
    if (g_mapState == -2) o2session_key(g_session, ks, down);
}

/* ---- lifetime ------------------------------------------------------------------ */

- (void)hidePanel
{
    [self setMapState:-2];
    [_liveTimer invalidate];
    _liveTimer = nil;
    o2session_release_all(g_session);
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self hidePanel];
}

+ (void)toggleWithSession:(o2session *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[O2KeypadWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        [g_singleton hidePanel];
        [[g_singleton window] orderOut:nil];
    } else {
        [g_singleton refreshPads];
        [g_singleton refresh];
        g_singleton->_liveTimer = [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [g_singleton tick];
        }];
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end

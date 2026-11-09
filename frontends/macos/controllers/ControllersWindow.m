/*
 * O2ControllersWindow -- the AppKit Controllers panel: the Odyssey 2's
 * membrane keyboard (drawn from the session's layout table, so every frontend
 * shows the same caps for the machine), the console's red RESET key, the two
 * joysticks drawn like the console's (a black base, the red fire button in its
 * corner, a stick with four direction pads), the gamepads and the stick each
 * drives, and Map mode.
 *
 * One custom-drawn view (O2ConsoleView) paints the console and takes the
 * mouse. Every key cap and stick control is an O2Ctl with a rectangle and a
 * binding TARGET (o2session.h): pressing it is o2session_press(target), Map
 * rebinds that target. A control acts on mouseDown AND mouseUp, not on a
 * click: the console samples its keyboard and sticks once a frame, so a press
 * that lasted only the instant of a click would fall between frames. A cap is
 * HELD while the mouse is down on it, like the real key; dragging off lets it
 * go. Hiding, losing the key window and rebuilding let go of what the mouse
 * holds here (only that: the main window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this panel -- is
 * filled with the accent (white text), so the panel doubles as an input
 * tester for a new gamepad. Keys typed with the panel key go to the console as
 * in the main window; no view here takes the keyboard, so the panel itself is
 * the first responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents. Only
 * a machine change resizes it (the G7400's keyboard is wider); the top edge
 * stays put. Hidden at start, ordered out rather than closed, so its
 * position survives. The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a key cap or a stick control,
 * then press a key or a gamepad button; the next one is bound and whatever
 * held it is told so in the status line. Esc cancels the pick; typed keys are
 * not typed while Map is armed. Rebinding steals the key from its old target.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../KeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define KBD_UNIT_MAX 44.0   /* points per key unit */
#define KBD_WIDTH_MAX 740.0 /* the G7400's wider keyboard is squeezed to this */
#define PLATE_PAD    10.0   /* the black keyboard plate around the caps */
#define MARGIN       14.0
#define HEADER_H     30.0   /* the console's nameplate above the plate */
#define STICK_W     214.0
#define STICK_H     176.0
#define STICK_GAP    20.0
#define SECTION_GAP  14.0
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W       130.0
#define POPUP_W     130.0
#define BTN_H        28.0
#define MAX_PAD_ROWS  4
#define SLOW_TICKS    8     /* the 30 Hz tick's 250 ms part */

/* The kinds of control beyond the layout's o2_keycap_kind (0-4). */
#define O2CTL_DIR   10      /* a joystick direction pad */
#define O2CTL_FIRE  11      /* a joystick's red fire button */

static NSString *nsstr(const char *s)
{
    NSString *r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) o2session_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t v)
{
    return [NSColor colorWithSRGBRed:((v >> 16) & 0xff) / 255.0
                               green:((v >> 8) & 0xff) / 255.0
                                blue:(v & 0xff) / 255.0
                               alpha:1.0];
}

/* Lines of text centred in r, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect r, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat nlines = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineH = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat w = 0;
        lineH = 0;
        for (NSString *l in lines) {
            const NSSize s = [l sizeWithAttributes:attrs];
            if (s.width > w) w = s.width;
            if (s.height > lineH) lineH = s.height;
        }
        if ((w <= r.size.width && lineH * nlines <= r.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat y = r.origin.y + floor((r.size.height - lineH * nlines) / 2);
    for (NSString *l in lines) {
        [l drawInRect:NSMakeRect(r.origin.x, y, r.size.width, lineH) withAttributes:attrs];
        y += lineH;
    }
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' tables. */
@interface O2CtlFlippedView : NSView
@end

@implementation O2CtlFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* One key cap or stick control. */
@interface O2Ctl : NSObject
@property (nonatomic) NSRect rect;
@property (nonatomic, copy) NSString *face;
@property (nonatomic) int target;     /* the binding target: what a press applies and Map rebinds */
@property (nonatomic) int kind;       /* o2_keycap_kind, O2CTL_DIR or O2CTL_FIRE */
@property (nonatomic) BOOL lit;       /* held by any source */
@end

@implementation O2Ctl
@end

/* Keys typed with the panel key go to its owner. */
@class O2ControllersWindow;
@interface O2ControllersPanel : NSPanel
@property (nonatomic, weak) O2ControllersWindow *owner;
@end

@interface O2ControllersWindow () <NSWindowDelegate>
- (void)ctlPressed:(O2Ctl *)c down:(BOOL)down;
- (NSString *)legendForTarget:(int)target;
- (void)panelKeyDown:(NSEvent *)e;
- (void)panelKeyUp:(NSEvent *)e;
- (void)panelFlagsChanged:(NSEvent *)e;
@end

/* The console: the silver case, the black keyboard plate with its caps, and
 * the two joystick bases. Everything on it is drawn here and hit-tested here. */
@interface O2ConsoleView : O2CtlFlippedView
@property (nonatomic, weak) O2ControllersWindow *owner;
@property (nonatomic, copy) NSArray<O2Ctl *> *controls;
@property (nonatomic, copy) NSArray<NSValue *> *stickBoxes;   /* two rects */
@property (nonatomic) NSRect plate;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
- (void)forgetMouse;
@end

@implementation O2ConsoleView {
    O2Ctl *_down;                            /* the control the mouse holds */
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)drawControl:(O2Ctl *)c
{
    const NSRect r = NSInsetRect(c.rect, 0.5, 0.5);
    const BOOL sel = self.mapping && c.target == self.mapTarget;
    const BOOL on = c.lit || sel;
    NSColor *accent = rgb(O2SESSION_ACCENT_RGB);
    NSColor *fill, *ink, *edge;
    CGFloat size = 15;
    BOOL bold = YES;

    switch (c.kind) {
    case O2CTL_DIR:
        fill = on ? accent : rgb(0x2a2a2e);
        ink = on ? [NSColor whiteColor] : rgb(0xd0d0d4);
        edge = on ? [NSColor whiteColor] : rgb(0x56565c);
        size = 15;
        break;
    case O2CTL_FIRE:
        fill = on ? accent : rgb(0x6e1315);
        ink = on ? [NSColor whiteColor] : rgb(0xf0c8c8);
        edge = on ? [NSColor whiteColor] : rgb(0x3a0a0b);
        size = 12;
        break;
    case O2_KEYCAP_RESET:
        fill = on ? accent : rgb(O2SESSION_ACCENT_DARK_RGB);
        ink = [NSColor whiteColor];
        edge = on ? [NSColor whiteColor] : rgb(0x5a1516);
        size = 10;
        break;
    case O2_KEYCAP_FUNCTION:
        fill = on ? accent : rgb(0xF1EFE8);
        ink = on ? [NSColor whiteColor] : rgb(O2SESSION_ACCENT_RGB);
        edge = on ? [NSColor whiteColor] : rgb(0x7a7770);
        size = 18;
        break;
    case O2_KEYCAP_INPUT:
        fill = on ? accent : rgb(0xF1EFE8);
        ink = on ? [NSColor whiteColor] : rgb(0x1a1a1a);
        edge = on ? [NSColor whiteColor] : rgb(0x7a7770);
        size = 10;
        break;
    case O2_KEYCAP_SPACE:
        fill = on ? accent : rgb(0xF1EFE8);
        ink = on ? [NSColor whiteColor] : rgb(0x1a1a1a);
        edge = on ? [NSColor whiteColor] : rgb(0x7a7770);
        size = 11;
        break;
    default:
        fill = on ? accent : rgb(0xF1EFE8);
        ink = on ? [NSColor whiteColor] : rgb(0x1a1a1a);
        edge = on ? [NSColor whiteColor] : rgb(0x7a7770);
        size = 16;
        break;
    }

    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5];
    [fill setFill];
    [path fill];
    [edge setStroke];
    path.lineWidth = on ? 2 : 1;
    [path stroke];

    /* the bound host key as a small legend under the face, in Map mode (what
     * is bound to what) and when asked for */
    NSString *legend = nil;
    if (self.mapping || self.showKeys) legend = [self.owner legendForTarget:c.target];
    const NSRect inner = NSInsetRect(c.rect, 3, 2);
    if (legend.length) {
        const CGFloat top = floor(inner.size.height * 0.58);
        drawFitted(c.face, NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, top),
                   size, bold, ink);
        drawFitted(legend, NSMakeRect(inner.origin.x, inner.origin.y + top, inner.size.width,
                                      inner.size.height - top),
                   8, NO, on ? [NSColor whiteColor] : rgb(0x6a6a6a));
    } else {
        drawFitted(c.face, inner, size, bold, ink);
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];

    /* the silver case (O2SESSION's family colour for this console) */
    [rgb(0xC3C0BF) setFill];
    NSRectFill(b);
    [rgb(0x9d9a99) setFill];
    NSRectFill(NSMakeRect(0, b.size.height - 1, b.size.width, 1));

    NSDictionary *nameplate = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                                 NSForegroundColorAttributeName: rgb(0x2b2b2b) };
    [self.title drawAtPoint:NSMakePoint(MARGIN, 8) withAttributes:nameplate];

    /* the keyboard plate */
    NSBezierPath *plate = [NSBezierPath bezierPathWithRoundedRect:self.plate xRadius:8 yRadius:8];
    [rgb(0x17171a) setFill];
    [plate fill];

    /* the joysticks' bases, their sockets and their names */
    NSDictionary *name = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                            NSForegroundColorAttributeName: rgb(0xa8a8ad) };
    for (NSUInteger i = 0; i < [self.stickBoxes count]; i++) {
        const NSRect sb = [self.stickBoxes[i] rectValue];
        NSBezierPath *base = [NSBezierPath bezierPathWithRoundedRect:sb xRadius:10 yRadius:10];
        [rgb(0x1d1d20) setFill];
        [base fill];
        [rgb(0x34343a) setStroke];
        base.lineWidth = 1;
        [base stroke];
        [[NSString stringWithFormat:@"PLAYER %lu", (unsigned long)(i + 1)]
            drawAtPoint:NSMakePoint(sb.origin.x + 108, sb.origin.y + 16) withAttributes:name];
        const NSPoint c = NSMakePoint(sb.origin.x + 107, sb.origin.y + 100);
        NSBezierPath *socket = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 18, c.y - 18, 36, 36)];
        [rgb(0x3c3c40) setFill];
        [socket fill];
        NSBezierPath *knob = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - 8, c.y - 8, 16, 16)];
        [rgb(0x131316) setFill];
        [knob fill];
    }

    for (O2Ctl *c in self.controls) [self drawControl:c];
}

- (O2Ctl *)controlAt:(NSPoint)p
{
    for (O2Ctl *c in self.controls)
        if (NSPointInRect(p, c.rect)) return c;
    return nil;
}

- (void)mouseDown:(NSEvent *)e
{
    if (_down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    O2Ctl *c = [self controlAt:p];
    if (!c) return;
    _down = c;
    [self.owner ctlPressed:c down:YES];
}

/* Dragging off a control must let it go. */
- (void)mouseDragged:(NSEvent *)e
{
    if (!_down) return;
    const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    if (NSPointInRect(p, _down.rect)) return;
    O2Ctl *c = _down;
    _down = nil;
    [self.owner ctlPressed:c down:NO];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    if (!_down) return;
    O2Ctl *c = _down;
    _down = nil;
    [self.owner ctlPressed:c down:NO];
}

/* The owner let go (hide, rebuild): the mouse no longer holds anything. */
- (void)forgetMouse
{
    _down = nil;
}
@end

@implementation O2ControllersPanel
- (void)keyDown:(NSEvent *)e { [self.owner panelKeyDown:e]; }
- (void)keyUp:(NSEvent *)e { [self.owner panelKeyUp:e]; }
- (void)flagsChanged:(NSEvent *)e { [self.owner panelFlagsChanged:e]; }
/* Escape is the console's Reset to CONFIG (or cancels a Map pick), not "close
 * the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation O2ControllersWindow {
    o2session *_session;
    O2AppKeyHandler _appKeys;
    O2ControllersPanel *_panel;
    O2CtlFlippedView *_content;
    O2ConsoleView *_console;
    NSArray<O2Ctl *> *_ctls;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives stick N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    int _builtMachine;
    CGFloat _consoleW, _consoleH;
    int _heldTarget;                        /* what the mouse holds here, or -1 */
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(o2session *)session appKeys:(O2AppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _heldTarget = -1;
    _builtMachine = -1;
    _padGeneration = (unsigned)-1;

    _panel = [[O2ControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 560, 600)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    O2SetHelpContext(_panel, @"controllers");
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];

    _content = [[O2CtlFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 560, 600)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[O2ConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.mapTarget = -1;
    {
        /* "controllers_show_bindings"; falls back to the older key names. */
        static const char *const olds[] = { "controllers_key_names", "controllers_bindings", "show_bindings" };
        int val = o2session_get_int(_session, "controllers_show_bindings", -1);
        for (size_t idx = 0; val < 0 && idx < sizeof olds / sizeof olds[0]; idx++)
            val = o2session_get_int(_session, olds[idx], -1);
        _console.showKeys = val > 0;
    }
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the stick it drives and the one it
     * is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        _padName[i] = [NSTextField labelWithString:@""];
        [_padName[i] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[i]];
        _padEff[i] = [NSTextField labelWithString:@""];
        [_padEff[i] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[i] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[i]];
        /* -1 = automatic (connection order), 0 and 1 = that stick */
        _padAssign[i] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[i] addItemsWithTitles:@[@"Automatic", @"Stick 1", @"Stick 2"]];
        _padAssign[i].tag = i;
        _padAssign[i].target = self;
        _padAssign[i].action = @selector(padAssignChanged:);
        [_padAssign[i] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[i]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show keys" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _helpButton = O2HelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [self setMapState:-2];
    [self rebuild];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the keyboard and the sticks -------------------------------------------------- */

- (int)machineNow
{
    const int m = o2session_get_int(_session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

- (O2Ctl *)ctlWithFace:(NSString *)face rect:(NSRect)rect target:(int)target kind:(int)kind
{
    O2Ctl *c = [[O2Ctl alloc] init];
    c.face = face;
    c.rect = rect;
    c.target = target;
    c.kind = kind;
    return c;
}

/* Points from key units, rounded so neighbouring caps keep an even gap. */
static CGFloat snap(CGFloat v)
{
    return floor(v + 0.5);
}

/* One joystick's five controls in its base. */
- (void)addStick:(int)port box:(NSRect)sb to:(NSMutableArray<O2Ctl *> *)ctls
{
    const CGFloat cx = sb.origin.x + 107, cy = sb.origin.y + 100;
    [ctls addObject:[self ctlWithFace:@"FIRE"
                                 rect:NSMakeRect(sb.origin.x + 10, sb.origin.y + 10, 72, 34)
                               target:O2_TARGET_PORT(port, O2_ACT_FIRE) kind:O2CTL_FIRE]];
    [ctls addObject:[self ctlWithFace:@"▲" rect:NSMakeRect(cx - 22, cy - 62, 44, 36)
                               target:O2_TARGET_PORT(port, O2_ACT_UP) kind:O2CTL_DIR]];
    [ctls addObject:[self ctlWithFace:@"▶" rect:NSMakeRect(cx + 26, cy - 18, 44, 36)
                               target:O2_TARGET_PORT(port, O2_ACT_RIGHT) kind:O2CTL_DIR]];
    [ctls addObject:[self ctlWithFace:@"▼" rect:NSMakeRect(cx - 22, cy + 26, 44, 36)
                               target:O2_TARGET_PORT(port, O2_ACT_DOWN) kind:O2CTL_DIR]];
    [ctls addObject:[self ctlWithFace:@"◀" rect:NSMakeRect(cx - 70, cy - 18, 44, 36)
                               target:O2_TARGET_PORT(port, O2_ACT_LEFT) kind:O2CTL_DIR]];
}

/* The console's drawing area and everything on it, for the machine. */
- (void)buildConsole
{
    int n = 0;
    float kw = 0, kh = 0;
    const o2_keycap *caps = o2session_keyboard_layout(_builtMachine, &n, &kw, &kh);
    CGFloat unit = KBD_UNIT_MAX;
    if (kw > 0 && (CGFloat)kw * unit > KBD_WIDTH_MAX) unit = KBD_WIDTH_MAX / (CGFloat)kw;

    const CGFloat kbdW = snap((CGFloat)kw * unit), kbdH = snap((CGFloat)kh * unit);
    const CGFloat plateW = kbdW + 2 * PLATE_PAD, plateH = kbdH + 2 * PLATE_PAD;
    const CGFloat sticksW = 2 * STICK_W + STICK_GAP;
    const CGFloat innerW = MAX(plateW, sticksW);
    _consoleW = innerW + 2 * MARGIN;
    const NSRect plate = NSMakeRect(MARGIN + floor((innerW - plateW) / 2), HEADER_H, plateW, plateH);

    NSMutableArray<O2Ctl *> *ctls = [NSMutableArray array];
    for (int i = 0; caps && i < n; i++) {
        const o2_keycap *k = &caps[i];
        const CGFloat x0 = snap((CGFloat)k->x * unit), y0 = snap((CGFloat)k->y * unit);
        const CGFloat x1 = snap((CGFloat)(k->x + k->w) * unit), y1 = snap((CGFloat)(k->y + k->h) * unit);
        const int target = k->target >= 0 ? O2_TARGET_KEY(k->target) : O2_TARGET_SWITCH(O2_SW_RESET);
        [ctls addObject:[self ctlWithFace:nsstr(k->label)
                                     rect:NSMakeRect(plate.origin.x + PLATE_PAD + x0,
                                                     plate.origin.y + PLATE_PAD + y0, x1 - x0, y1 - y0)
                                   target:target kind:k->kind]];
    }

    const CGFloat sticksY = NSMaxY(plate) + SECTION_GAP;
    const CGFloat sticksX = MARGIN + floor((innerW - sticksW) / 2);
    NSMutableArray<NSValue *> *boxes = [NSMutableArray array];
    for (int port = 0; port < 2; port++) {
        const NSRect sb = NSMakeRect(sticksX + port * (STICK_W + STICK_GAP), sticksY, STICK_W, STICK_H);
        [boxes addObject:[NSValue valueWithRect:sb]];
        [self addStick:port box:sb to:ctls];
    }
    _consoleH = sticksY + STICK_H + MARGIN;

    _ctls = ctls;
    _console.controls = ctls;
    _console.stickBoxes = boxes;
    _console.plate = plate;
    _console.title = nsstr(o2_machine_name(_builtMachine));
}

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const CGFloat w = _consoleW;
    const CGFloat inner = w - 2 * MARGIN;
    const CGFloat padsY = _consoleH + MARGIN;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 8;
    const CGFloat hintY = mapY + BTN_H + 8;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(w, total)];
    [_console setFrame:NSMakeRect(0, 0, w, _consoleH)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, 200, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const CGFloat y = rowsY + i * ROW_H;
        [_padName[i] setFrame:NSMakeRect(MARGIN, y + 4, inner - POPUP_W - EFF_W - 16, 18)];
        [_padEff[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 8, y + 4, EFF_W, 18)];
        [_padAssign[i] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, y, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 76, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 82, mapY, 96, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 190, mapY + 3, 110, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(w - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_console setNeedsDisplay:YES];
}

- (void)rebuild
{
    [self letGo];
    _builtMachine = [self machineNow];
    [self buildConsole];
    [self relayout];
    [self refreshLabels];
}

/* ---- pressing --------------------------------------------------------------------- */

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console forgetMouse];
    if (_heldTarget >= 0) {
        o2session_press(_session, _heldTarget, 0);
        _heldTarget = -1;
    }
}

- (void)ctlPressed:(O2Ctl *)c down:(BOOL)down
{
    if (_mapState != -2) {
        /* armed: picks (or changes) the control to rebind */
        if (down) [self setMapState:c.target];
        return;
    }
    if (down) {
        [self letGo];
        _heldTarget = c.target;
        o2session_press(_session, c.target, 1);
    } else if (_heldTarget == c.target) {
        _heldTarget = -1;
        o2session_press(_session, c.target, 0);
    }
    [self pollHeld];
}

/* What the console sees held -- host keyboard, gamepads and this panel -- lit. */
- (void)pollHeld
{
    if (_mapState != -2) return;    /* Map shows what is bound, not what is held */
    const unsigned sticks[2] = { o2session_buttons_held(_session, 0), o2session_buttons_held(_session, 1) };
    for (O2Ctl *c in _ctls) {
        const int t = c.target;
        BOOL on;
        if (t < 2 * O2_ACT_PER_PORT)
            on = ((sticks[t / O2_ACT_PER_PORT] >> (t % O2_ACT_PER_PORT)) & 1u) != 0;
        else if (t < O2_TARGET_SWITCH(0))
            on = o2session_key_held(_session, t - O2_TARGET_KEY(0)) != 0;
        else
            on = o2session_switch_held(_session, t - O2_TARGET_SWITCH(0)) != 0;
        on = on || t == _heldTarget;
        if (c.lit != on) {
            c.lit = on;
            [_console setNeedsDisplayInRect:c.rect];
        }
    }
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(o2session_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const o2_binding b = o2session_binding_get(_session, target);
    NSString *key = keyName(b.keysym);
    NSString *text = key.length ? key : @"no key";
    if (b.button != O2_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@, pad %@", text, nsstr(o2_pad_button_name(b.button))];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    const BOOL mapping = _mapState != -2;
    _console.mapping = mapping;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    if (mapping)
        for (O2Ctl *c in _ctls) c.lit = NO;
    [_console setNeedsDisplay:YES];
    [self pollHeld];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        o2session_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(o2_target_name(state)), [self bindingText:state]];
    } else {
        o2session_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1 ? @"Click a key or a stick control to remap"
                                        : @"Click and hold to press a key or a stick control.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    _mapButton.bezelColor = state == -2 ? nil : rgb(O2SESSION_ACCENT_RGB);
    [self refreshLabels];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(o2_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"%@ is now %@ (it was %@)", what, name, nsstr(stolen)]
        : [NSString stringWithFormat:@"%@ is now %@", what, name];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[128];
    stolen[0] = '\0';
    const int target = _mapState;
    o2session_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = O2_PAD_BTN_NONE;
    char stolen[128];
    if (_mapState < 0 || !o2session_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    o2session_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(o2_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    o2session_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL on = [_showKeysBox state] == NSControlStateValueOn;
    o2session_set_int(_session, "controllers_show_bindings", on ? 1 : 0);
    _console.showKeys = on;
    [_console setNeedsDisplay:YES];
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int i = 0; i < _padRows; i++) {
        const int eff = o2session_gamepad_effective_port(_session, i);
        NSString *text = eff >= 0 ? [NSString stringWithFormat:@"drives stick %d", eff + 1] : @"drives nothing";
        if (![_padEff[i].stringValue isEqualToString:text]) _padEff[i].stringValue = text;
    }
}

- (void)refreshPads
{
    const int n = o2session_gamepad_count(_session);
    _padGeneration = o2session_gamepad_generation(_session);
    _padRows = n < MAX_PAD_ROWS ? (n > 0 ? n : 0) : MAX_PAD_ROWS;
    _noPads.hidden = n > 0;
    for (int i = 0; i < MAX_PAD_ROWS; i++) {
        const BOOL shown = i < _padRows;
        _padName[i].hidden = !shown;
        _padEff[i].hidden = !shown;
        _padAssign[i].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        o2session_gamepad_name(_session, i, name, (int)sizeof name);
        _padName[i].stringValue = nsstr(name);
        _padName[i].toolTip = nsstr(name);
        const int assign = o2session_gamepad_assignment(_session, i);
        [_padAssign[i] selectItemAtIndex:(assign < 0 || assign > 1 ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    o2session_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads, auto-assignment and a machine change in
 * Preferences. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (o2session_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
        if ([self machineNow] != _builtMachine) [self rebuild];
    }
    [self pollHeld];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak O2ControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)e
{
    if ([e modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t ks = O2KeysymFromEvent(e);
    if (!ks) return;
    if (_swallowKey && ks == _swallowKey) return;
    if ([e isARepeat]) return;
    if (ks == O2_KEYSYM_F9 || ks == O2_KEYSYM_F11 || ks == O2_KEYSYM_F12) {
        if (_appKeys) _appKeys(ks);
        return;
    }
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick */
        if (ks == O2_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:ks swallow:YES];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* a key bound to a system action goes to the app's queue, as a gamepad's does */
    const int sa = o2session_key_sysaction(_session, ks);
    if (sa >= 0) {
        o2session_sysaction_post(_session, sa);
        return;
    }
    o2session_key(_session, ks, 1);
}

- (void)panelKeyUp:(NSEvent *)e
{
    const uint32_t ks = O2KeysymFromEvent(e);
    if (!ks) return;
    if (_swallowKey && ks == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    o2session_key(_session, ks, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise
 * they are the console's. A release always goes through. */
- (void)panelFlagsChanged:(NSEvent *)e
{
    int down = 0;
    const uint32_t ks = O2KeysymFromFlagsChange(e, &down);
    if (!ks) return;
    if (_mapState >= 0 && down) {
        [self bindKey:ks swallow:NO];
        return;
    }
    if (_mapState != -2 && down) return;
    o2session_key(_session, ks, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    if ([self machineNow] != _builtMachine) [self rebuild];
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    if ([self isVisible]) {
        [self rebuild];
        [self refreshPads];
    } else {
        _builtMachine = -1;     /* rebuilt when next shown */
    }
}

/* Only what this panel's mouse holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end

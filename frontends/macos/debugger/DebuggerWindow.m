/*
 * Debugger window (AppKit) over this app's CDP1802 / CDP1861 debugger
 * engine, via core/include/o2debug.h. Mirrors the GTK, Qt and Win32
 * debuggers tab for tab: Prompt, CPU & RAM, Disassembly, Video, Sound & I/O,
 * Breakpoints, Cart, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "../KeyForward.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "o2debug.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_REGS 24          /* R0-RF, then D DF P X T B IE Q */
#define TAB_VIDEO 3          /* the tabs refreshed live while running and showing */
#define TAB_FUJINET 7
#define TRACE_MAX 512        /* FujiNet packets shown */

/* The console's 512 bytes of RAM, at $0800-$09FF (and mirrored above
 * wherever A9 is clear). The dump: a header line, then 16 bytes a line. */
#define RAM_BASE 0x0800
#define RAM_SIZE 512
#define RAM_HEADER @"       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n"
#define RAM_LINE_LEN 56      /* "$0800: " + 16 * "XX " + "\n" */

static O2DebuggerWindow *g_debugger;

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

/* F12 closes the debugger from inside it, as it opens it from the
 * machine's window. Taken as a key equivalent, so it works wherever the
 * focus is in the window. */
@interface DebuggerPanelWindow : NSWindow
@end

@implementation DebuggerPanelWindow
static BOOL isF12(NSEvent *event)
{
    NSString *chars = [event charactersIgnoringModifiers];
    return [event type] == NSEventTypeKeyDown && [chars length] == 1 &&
           [chars characterAtIndex:0] == NSF12FunctionKey;
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return YES;
    }
    return [super performKeyEquivalent:event];
}

/* and when nothing in the window took it as a key equivalent */
- (void)keyDown:(NSEvent *)event
{
    if (isF12(event)) {
        [self performClose:nil];
        return;
    }
    [super keyDown:event];
}
@end

/* Disassembly text view: a click toggles the breakpoint on the clicked
 * line; the wheel browses. */
@interface DasmTextView : NSTextView
@property (nonatomic, copy) void (^onToggleLine)(int line);
@property (nonatomic, copy) void (^onScroll)(int lines);
@end

@implementation DasmTextView
- (void)mouseDown:(NSEvent *)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSUInteger idx = [self characterIndexForInsertionAtPoint:p];
    NSString *text = self.string;
    if (idx > text.length) idx = text.length;
    int line = 0;
    for (NSUInteger i = 0; i < idx && i < text.length; i++)
        if ([text characterAtIndex:i] == '\n') line++;
    if (self.onToggleLine) self.onToggleLine(line);
}

- (void)scrollWheel:(NSEvent *)event
{
    if (self.onScroll) self.onScroll((int)(-event.scrollingDeltaY / 6.0));
}
@end

/* A video view: nearest-neighbour at the largest whole multiple that fits,
 * so every pixel of these small pictures is the same size; letterboxed. */
@interface PictureView : NSView
@property (nonatomic) CGImageRef image;
@end

@implementation PictureView
- (void)setImage:(CGImageRef)image
{
    if (_image) CGImageRelease(_image);
    _image = image;
    [self setNeedsDisplay:YES];
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];
    CGContextSetRGBFillColor(dc, 0.1, 0.1, 0.1, 1);
    CGContextFillRect(dc, b);
    if (!_image) return;
    const double iw = (double)CGImageGetWidth(_image), ih = (double)CGImageGetHeight(_image);
    double w = b.size.width, h = b.size.height, sw, sh;
    const double scale = floor(fmin(w / iw, h / ih));
    if (scale >= 1) { sw = iw * scale; sh = ih * scale; }
    else {
        const double want = iw / ih;
        if (w / h > want) { sh = h; sw = sh * want; } else { sw = w; sh = sw / want; }
    }
    CGContextSetInterpolationQuality(dc, kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake(floor((w - sw) / 2), floor((h - sh) / 2), sw, sh), _image);
}
@end

/* A legend's colour square. */
@interface Swatch : NSView
@property (nonatomic, strong) NSColor *color;
@end

@implementation Swatch
- (void)setColor:(NSColor *)color
{
    _color = color;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [(_color ?: NSColor.blackColor) setFill];
    NSRectFill([self bounds]);
    [[NSColor separatorColor] setStroke];
    [NSBezierPath strokeRect:NSInsetRect([self bounds], 0.5, 0.5)];
}
@end

/* What the cartridge serves at each page (o2debug_cart.page_type). */
static NSColor *pageColor(int type)
{
    switch (type) {
    case 1:  return NSColor.systemBlueColor;      /* the image's ROM */
    case 2:  return NSColor.systemOrangeColor;    /* a hotspot */
    case 3:  return NSColor.systemGreenColor;     /* the raster, read by DMA */
    default: return NSColor.quaternaryLabelColor; /* not the cartridge's */
    }
}

/* The 64K as 16 x 16 pages of 256 bytes: a row per 4K ($X000), a column per
 * page within it ($X?00), coloured by what the cartridge serves there. */
@interface PageMapView : NSView
- (void)setTypes:(const uint8_t *)types;
@end

#define PM_CELL 14.0
#define PM_LEFT 44.0          /* the row labels */
#define PM_TOP  16.0          /* the column labels */

@implementation PageMapView {
    uint8_t _types[256];
}
- (BOOL)isFlipped { return YES; }   /* rows downwards, as the addresses go */
- (NSSize)intrinsicContentSize { return NSMakeSize(PM_LEFT + 16 * PM_CELL + 1, PM_TOP + 16 * PM_CELL + 1); }
- (void)setTypes:(const uint8_t *)types
{
    if (!memcmp(_types, types, sizeof _types)) return;
    memcpy(_types, types, sizeof _types);
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
                             NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    for (int col = 0; col < 16; col++)
        [[NSString stringWithFormat:@"%X", col] drawAtPoint:NSMakePoint(PM_LEFT + col * PM_CELL + 3, 1)
                                             withAttributes:attrs];
    for (int row = 0; row < 16; row++) {
        [[NSString stringWithFormat:@"$%X000", row] drawAtPoint:NSMakePoint(2, PM_TOP + row * PM_CELL)
                                                 withAttributes:attrs];
        for (int col = 0; col < 16; col++) {
            [pageColor(_types[row * 16 + col]) setFill];
            NSRectFill(NSMakeRect(PM_LEFT + col * PM_CELL, PM_TOP + row * PM_CELL, PM_CELL - 1, PM_CELL - 1));
        }
    }
}
@end

/* The cartridge's text engine as it drew the screen: 10 rows of 16, the
 * inverse cells inverted, the cursor outlined in the accent. */
@interface TextGridView : NSView
- (void)setCart:(const o2debug_cart *)cart;
@end

#define TG_W 15.0
#define TG_H 19.0

@implementation TextGridView {
    uint8_t _text[10][16];
    uint8_t _inv[10][16];
    int _row, _col;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize { return NSMakeSize(16 * TG_W + 2, 10 * TG_H + 2); }
- (void)setCart:(const o2debug_cart *)cart
{
    memcpy(_text, cart->text, sizeof _text);
    memcpy(_inv, cart->text_inv, sizeof _inv);
    _row = cart->cursor_row;
    _col = cart->cursor_col;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect b = [self bounds];
    [NSColor.textBackgroundColor setFill];
    NSRectFill(b);
    [NSColor.separatorColor setStroke];
    [NSBezierPath strokeRect:NSInsetRect(b, 0.5, 0.5)];

    NSFont *font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < 16; c++) {
            const NSRect cellRect = NSMakeRect(1 + c * TG_W, 1 + r * TG_H, TG_W, TG_H);
            const uint8_t ch = _text[r][c];
            if (_inv[r][c]) {
                [NSColor.labelColor setFill];
                NSRectFill(cellRect);
            }
            /* printable ASCII as itself; anything else (a glyph the app
             * defined) as '#', as the prompt's "text" shows it */
            const unichar u = (ch >= 0x20 && ch < 0x7f) ? ch : (ch ? '#' : ' ');
            NSString *glyph = [NSString stringWithCharacters:&u length:1];
            NSDictionary *attrs = @{ NSFontAttributeName: font,
                                     NSForegroundColorAttributeName: _inv[r][c] ? NSColor.textBackgroundColor
                                                                               : NSColor.labelColor };
            const NSSize sz = [glyph sizeWithAttributes:attrs];
            [glyph drawAtPoint:NSMakePoint(NSMidX(cellRect) - sz.width / 2, NSMidY(cellRect) - sz.height / 2)
                withAttributes:attrs];
            if (r == _row && c == _col) {
                NSBezierPath *cursor = [NSBezierPath bezierPathWithRect:NSInsetRect(cellRect, 1, 1)];
                cursor.lineWidth = 2;
                [O2AccentTextColor() setStroke];
                [cursor stroke];
            }
        }
}
@end

/* One keypad as the console sees it: the keys held filled in the accent,
 * the key OUT 2 latched outlined. Laid out as on the console. */
@interface KeypadDiagramView : NSView
- (instancetype)initWithTitle:(NSString *)title;
- (void)setHeld:(unsigned)held latched:(int)key;
@end

#define KD_W   32.0
#define KD_H   22.0
#define KD_GAP  4.0
#define KD_TITLE 18.0

@implementation KeypadDiagramView {
    NSString *_title;
    unsigned _held;
    int _latched;
}
- (instancetype)initWithTitle:(NSString *)title
{
    self = [super initWithFrame:NSMakeRect(0, 0, 3 * KD_W + 2 * KD_GAP, KD_TITLE + 4 * KD_H + 3 * KD_GAP)];
    if (!self) return nil;
    _title = [title copy];
    _latched = -1;
    return self;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize { return NSMakeSize(3 * KD_W + 2 * KD_GAP, KD_TITLE + 4 * KD_H + 3 * KD_GAP); }
- (void)setHeld:(unsigned)held latched:(int)key
{
    if (held == _held && key == _latched) return;
    _held = held;
    _latched = key;
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    static const int layout[4][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { -1, 0, -1 } };
    [_title drawAtPoint:NSMakePoint(0, 0)
         withAttributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:11],
                           NSForegroundColorAttributeName: NSColor.labelColor }];
    NSFont *font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 3; col++) {
            const int key = layout[row][col];
            if (key < 0) continue;
            const NSRect r = NSMakeRect(col * (KD_W + KD_GAP), KD_TITLE + row * (KD_H + KD_GAP), KD_W, KD_H);
            const BOOL held = (_held >> key) & 1u;
            NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 1, 1) xRadius:4 yRadius:4];
            [(held ? O2AccentColor() : NSColor.controlBackgroundColor) setFill];
            [path fill];
            path.lineWidth = key == _latched ? 2 : 1;
            [(key == _latched ? O2AccentTextColor() : NSColor.separatorColor) setStroke];
            [path stroke];
            NSString *digit = [NSString stringWithFormat:@"%d", key];
            /* dark text on the pale tan */
            NSDictionary *attrs = @{ NSFontAttributeName: font,
                                     NSForegroundColorAttributeName: held ? NSColor.blackColor : NSColor.labelColor };
            const NSSize sz = [digit sizeWithAttributes:attrs];
            [digit drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2) withAttributes:attrs];
        }
}
@end

@interface O2DebuggerWindow () <NSTabViewDelegate>
- (instancetype)initWithSession:(o2session *)session;
- (void)refreshAll;
@end

@implementation O2DebuggerWindow {
    o2session *_session;
    o2debug *_dbg;
    NSWindow *_window;
    NSTimer *_tick;
    unsigned _seenGen;
    BOOL _wasStopped;
    int _runningTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_REGS];
    NSButton *_ef[4];
    NSTextField *_cpuState, *_beam;
    NSTextView *_ram;
    NSTextField *_ramGoto, *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    DasmTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    NSTextView *_videoText;
    NSPopUpButton *_videoViewSel;
    PictureView *_videoPic;
    uint32_t *_viewPx;

    NSTextView *_sound;
    KeypadDiagramView *_keypadDiag[2];

    NSPopUpButton *_bpType;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart;
    PageMapView *_pageMap;
    TextGridView *_textGrid;
    NSTextField *_textNote;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;
}

/* The register fields, in the order the CPU tab lays them out: R0-RF, then
 * the 1802's others, each with its o2debug_reg and the hex digits it
 * shows (0: a 0/1 flag). */
static const struct { NSString *__unsafe_unretained name; int reg; int digits; } kRegs[NUM_REGS] = {
    { @"R0", O2_REG_R0 + 0, 4 },  { @"R1", O2_REG_R0 + 1, 4 },
    { @"R2", O2_REG_R0 + 2, 4 },  { @"R3", O2_REG_R0 + 3, 4 },
    { @"R4", O2_REG_R0 + 4, 4 },  { @"R5", O2_REG_R0 + 5, 4 },
    { @"R6", O2_REG_R0 + 6, 4 },  { @"R7", O2_REG_R0 + 7, 4 },
    { @"R8", O2_REG_R0 + 8, 4 },  { @"R9", O2_REG_R0 + 9, 4 },
    { @"RA", O2_REG_R0 + 10, 4 }, { @"RB", O2_REG_R0 + 11, 4 },
    { @"RC", O2_REG_R0 + 12, 4 }, { @"RD", O2_REG_R0 + 13, 4 },
    { @"RE", O2_REG_R0 + 14, 4 }, { @"RF", O2_REG_R0 + 15, 4 },
    { @"D", O2_REG_D, 2 },   { @"DF", O2_REG_DF, 0 },
    { @"P", O2_REG_P, 1 },   { @"X", O2_REG_X, 1 },
    { @"T", O2_REG_T, 2 },   { @"B", O2_REG_B, 2 },
    { @"IE", O2_REG_IE, 0 }, { @"Q", O2_REG_Q, 0 },
};

#define NUM_FILE_KINDS 6
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "ram", "arena", "raster", "screen", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"Console RAM…", @"Mailbox Arena…", @"Cart Raster…", @"Screen (PPM)…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"ram.bin", @"arena.bin", @"raster.bin", @"screen.ppm", @"registers.txt"
};

/* O2_DEBUGGER_TAB takes an index or one of these. */
#define NUM_TABS 8
static const char *const kTabNames[NUM_TABS] = {
    "prompt", "cpu", "disassembly", "video", "io", "breakpoints", "cart", "fujinet"
};

/* ---- helpers --------------------------------------------------------------- */

static NSString *stripControl(const char *s)
{
    NSMutableString *out = [NSMutableString string];
    char buf[2] = { 0, 0 };
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') {
            buf[0] = *s;
            [out appendString:[NSString stringWithUTF8String:buf] ?: @""];
        }
    }
    return out;
}

/* $hex, 0xhex, hexh, #dec, or bare hex. */
static BOOL parseNum(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    int base = 16;
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    else if ([t hasPrefix:@"#"]) { t = [t substringFromIndex:1]; base = 10; }
    else if (t.length > 1 && [t.lowercaseString hasSuffix:@"h"]) t = [t substringToIndex:t.length - 1];
    if (t.length == 0) return NO;
    char *end;
    long v = strtol(t.UTF8String, &end, base);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* A label, or a number. -1 when neither. */
- (int)resolveAddr:(NSString *)text
{
    long v;
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (t.length == 0) return -1;
    int addr = o2debug_label_address(_dbg, t.UTF8String);
    if (addr < 0 && parseNum(t, &v) && v >= 0 && v <= 0xffff) addr = (int)v;
    return addr;
}

static NSTextView *monoView(NSScrollView **scrollOut, BOOL wrap)
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = !wrap;
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)];
    view.editable = NO;
    view.richText = NO;
    view.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    if (wrap) {
        view.autoresizingMask = NSViewWidthSizable;
    } else {
        view.horizontallyResizable = YES;
        view.textContainer.widthTracksTextView = NO;
        view.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    }
    scroll.documentView = view;
    *scrollOut = scroll;
    return view;
}

/* Replace a text view's text, keeping the reader's place. */
static void setTextKeepingPlace(NSTextView *view, NSString *text)
{
    NSScrollView *scroll = view.enclosingScrollView;
    const NSPoint at = scroll ? scroll.contentView.bounds.origin : NSZeroPoint;
    view.string = text;
    if (scroll) [scroll.contentView scrollToPoint:at];
}

/* Whether a view's reader is at its end (a log then follows new lines). */
static BOOL atEnd(NSTextView *view)
{
    NSScrollView *scroll = view.enclosingScrollView;
    if (!scroll) return YES;
    return NSMaxY(scroll.contentView.documentVisibleRect) >= NSHeight(view.frame) - 4;
}

/* A log's text where its reader left it, or, following, at its end. */
static void setLogText(NSTextView *view, NSString *text, BOOL follow)
{
    if (![view.string isEqualToString:text]) {
        if (follow) view.string = text;
        else setTextKeepingPlace(view, text);
    }
    if (follow) [view scrollRangeToVisible:NSMakeRange(view.string.length, 0)];
}

/* A button lit in the accent, its title dark on the pale tan; or plain. */
static void setButtonLit(NSButton *b, NSString *title, BOOL lit)
{
    b.bezelColor = lit ? O2AccentColor() : nil;
    b.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize],
        NSForegroundColorAttributeName: lit ? NSColor.blackColor : NSColor.controlTextColor }];
}

- (NSButton *)button:(NSString *)title action:(SEL)sel
{
    NSButton *b = [NSButton buttonWithTitle:title target:self action:sel];
    [b setRefusesFirstResponder:YES];
    return b;
}

- (NSTextField *)label:(NSString *)text
{
    return [NSTextField labelWithString:text];
}

- (NSTextField *)field:(NSString *)placeholder width:(CGFloat)width action:(SEL)sel
{
    NSTextField *f = [[NSTextField alloc] init];
    f.placeholderString = placeholder;
    f.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    f.target = self;
    f.action = sel;
    if (width > 0) [f.widthAnchor constraintEqualToConstant:width].active = YES;
    return f;
}

/* A pull-down button: `title` on its face, `items` underneath (each one's
 * tag is its index). */
- (NSPopUpButton *)pullDown:(NSString *)title items:(NSArray<NSString *> *)items action:(SEL)sel
{
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [p addItemWithTitle:title];
    p.lastItem.tag = -1;
    for (NSUInteger i = 0; i < items.count; i++) {
        [p addItemWithTitle:items[i]];
        p.lastItem.tag = (NSInteger)i;
    }
    p.target = self;
    p.action = sel;
    [p setRefusesFirstResponder:YES];
    return p;
}

static NSStackView *hstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 6;
    return s;
}

static NSStackView *vstack(NSArray<NSView *> *views)
{
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationVertical;
    s.alignment = NSLayoutAttributeLeading;
    s.spacing = 6;
    return s;
}

static void fill(NSStackView *stack, NSView *view)
{
    [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/* A drawn view at its own size: the stacks then neither squeeze nor
 * stretch it. */
static void fixSize(NSView *view)
{
    const NSSize sz = view.intrinsicContentSize;
    [view.widthAnchor constraintEqualToConstant:sz.width].active = YES;
    [view.heightAnchor constraintEqualToConstant:sz.height].active = YES;
}

/* ---- lifetime -------------------------------------------------------------- */

+ (void)showForSession:(o2session *)session
{
    if (!g_debugger) g_debugger = [[O2DebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    o2debug_attach(g_debugger->_dbg);
    [g_debugger refreshAll];
}

+ (void)toggleForSession:(o2session *)session
{
    if (g_debugger && [g_debugger->_window isVisible]) {
        /* windowWillClose: detaches */
        [g_debugger->_window close];
        return;
    }
    [self showForSession:session];
}

- (instancetype)initWithSession:(o2session *)session
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _dbg = o2session_debugger(session);
    _fujiCount = -1;
    _viewPx = calloc(O2DEBUG_VIEW_MAX_PIXELS, sizeof *_viewPx);
    _history = [NSMutableArray array];
    [self buildWindow];
    __weak O2DebuggerWindow *weakSelf = self;
    _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
        (void)t;
        [weakSelf onTick];
    }];
    return self;
}

- (void)dealloc
{
    [_tick invalidate];
    free(_viewPx);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    o2debug_detach(_dbg);
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const unsigned gen = o2debug_generation(_dbg);
    const BOOL stopped = o2debug_is_stopped(_dbg) != 0;
    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (!stopped && ++_runningTicks >= 5) {
        /* running: the live values twice a second */
        _runningTicks = 0;
        [self refreshStatus];
        [self refreshCpu];
        if ([_tabs indexOfTabViewItem:_tabs.selectedTabViewItem] == TAB_VIDEO) [self refreshVideo];
        [self refreshSound];
        [self refreshCart];
        if ([_tabs indexOfTabViewItem:_tabs.selectedTabViewItem] == TAB_FUJINET) [self refreshFujinet:NO];
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    if ([_tabs indexOfTabViewItem:item] == TAB_FUJINET) [self refreshFujinet:NO];
}

/* ---- construction ------------------------------------------------------------ */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = monoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = [self field:@"command (help, step, break $0400, bpw $0900, print r3, mem $0800 32, ...) — Tab completes, ↑↓ history"
                      width:0 action:@selector(runPrompt:)];
    _promptIn.delegate = self;

    NSStackView *v = vstack(@[outScroll, _promptIn]);
    fill(v, outScroll);
    fill(v, _promptIn);
    return v;
}

/* R0-R7 and R8-RF in two columns of eight, the 1802's other registers in a
 * third; EF1-EF4 as the CPU sees them (read-only); then the state, the beam
 * and the console's RAM. */
- (NSView *)buildCpu
{
    for (int i = 0; i < NUM_REGS; i++) {
        const int digits = kRegs[i].digits;
        _reg[i] = [self field:@"" width:(digits == 4 ? 52 : (digits == 2 ? 36 : 28)) action:@selector(applyRegister:)];
        _reg[i].tag = i;
    }
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int row = 0; row < 8; row++)
        [rows addObject:@[ [self label:kRegs[row].name], _reg[row],
                           [self label:kRegs[row + 8].name], _reg[row + 8],
                           [self label:kRegs[row + 16].name], _reg[row + 16] ]];
    NSGridView *regs = [NSGridView gridViewWithViews:rows];
    regs.rowSpacing = 4;
    regs.columnSpacing = 6;
    for (int col = 0; col < 6; col += 2) [regs columnAtIndex:col].xPlacement = NSGridCellPlacementTrailing;
    [regs columnAtIndex:2].leadingPadding = 12;
    [regs columnAtIndex:4].leadingPadding = 12;

    NSMutableArray *efs = [NSMutableArray arrayWithObject:[self label:@"External flags"]];
    for (int i = 0; i < 4; i++) {
        _ef[i] = [NSButton checkboxWithTitle:[NSString stringWithFormat:@"EF%d", i + 1] target:nil action:NULL];
        _ef[i].enabled = NO;   /* the console's inputs: shown, not set */
        [efs addObject:_ef[i]];
    }
    _cpuState = [self label:@""];
    _beam = [self label:@""];
    _beam.textColor = NSColor.secondaryLabelColor;
    NSTextField *regHint = [self label:@"Enter applies an edit while the machine is stopped; R(P) is the PC"];
    regHint.textColor = NSColor.secondaryLabelColor;

    NSScrollView *ramScroll;
    _ram = monoView(&ramScroll, NO);
    _ramGoto = [self field:@"$0800 or a label" width:140 action:@selector(gotoRam:)];
    NSStackView *ramHead = hstack(@[[self label:@"Console RAM ($0800-$09FF)"], [self label:@"Go to"], _ramGoto]);
    _ramAddr = [self field:@"$0800" width:110 action:@selector(writeRam:)];
    _ramVal = [self field:@"$00" width:60 action:@selector(writeRam:)];
    NSTextField *hint = [self label:@"RAM takes it as a write; a page the cartridge serves takes it in the cartridge's own buffer"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *edit = hstack(@[[self label:@"Write address"], _ramAddr, [self label:@"value"], _ramVal, hint]);

    NSStackView *v = vstack(@[regs, hstack(efs), regHint, _cpuState, _beam, ramHead, ramScroll, edit]);
    fill(v, ramScroll);
    return v;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = [self field:@"address or label" width:150 action:@selector(jumpTo:)];
    NSTextField *hint = [self label:@"Click a line to toggle its breakpoint (*); scroll to browse"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *row = hstack(@[_followPc, [self label:@"Jump to"], _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[DasmTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak O2DebuggerWindow *weakSelf = self;
    _disasm.onToggleLine = ^(int line) {
        O2DebuggerWindow *s = weakSelf;
        if (!s || line < 0 || line >= s->_lineCount) return;
        o2debug_breakpoint_toggle(s->_dbg, s->_lineAddr[line]);
        [s refreshDisasm];
        [s refreshBps];
    };
    _disasm.onScroll = ^(int lines) {
        O2DebuggerWindow *s = weakSelf;
        if (!s || lines == 0) return;
        s->_followPc.state = NSControlStateValueOff;
        s->_disasmAddr = (uint16_t)o2debug_row_address(s->_dbg, s->_disasmAddr, lines);
        [s refreshDisasm];
    };
    scroll.documentView = _disasm;

    NSStackView *v = vstack(@[row, scroll]);
    fill(v, scroll);
    return v;
}

/* The 1861's state left; one of its views right, in the order of
 * o2debug_view. */
- (NSView *)buildVideo
{
    NSScrollView *textScroll;
    _videoText = monoView(&textScroll, NO);
    NSStackView *left = vstack(@[textScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the text takes the column's height */
    fill(left, textScroll);

    _videoViewSel = [[NSPopUpButton alloc] init];
    [_videoViewSel addItemsWithTitles:@[@"Screen (112×262, the whole raster)",
                                        @"Cart raster (64×128)",
                                        @"BIOS display page (64×32, $0900)"]];
    _videoViewSel.target = self;
    _videoViewSel.action = @selector(refreshVideo);
    NSTextField *hint = [self label:@"Screen: the blanking dimmed, the visible area outlined, the beam crosshaired"];
    hint.textColor = NSColor.secondaryLabelColor;

    _videoPic = [[PictureView alloc] initWithFrame:NSMakeRect(0, 0, 512, 524)];
    [_videoPic.widthAnchor constraintGreaterThanOrEqualToConstant:448].active = YES;
    [_videoPic.heightAnchor constraintGreaterThanOrEqualToConstant:524].active = YES;

    NSStackView *right = vstack(@[_videoViewSel, _videoPic, hint]);
    fill(right, _videoPic);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The two keypads as the console sees them over the beeper's and the
 * ports' text. */
- (NSView *)buildSound
{
    NSScrollView *scroll;
    _sound = monoView(&scroll, YES);
    _keypadDiag[0] = [[KeypadDiagramView alloc] initWithTitle:@"Keypad A (EF3)"];
    _keypadDiag[1] = [[KeypadDiagramView alloc] initWithTitle:@"Keypad B (EF4)"];
    fixSize(_keypadDiag[0]);
    fixSize(_keypadDiag[1]);
    NSTextField *hint = [self label:@"Filled: held, as the console sees it. Outlined: the key OUT 2 latched."];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *pads = hstack(@[_keypadDiag[0], _keypadDiag[1], hint]);
    pads.spacing = 24;

    NSStackView *v = vstack(@[pads, scroll]);
    v.distribution = NSStackViewDistributionFill;   /* the text takes the rest of the height */
    fill(v, scroll);
    return v;
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write", @"Port In", @"Port Out", @"Port In/Out"]];
    _bpRange = [self field:@"$0400, $0800-$08FF, a label, or port 1-7" width:240 action:@selector(addBreakpoint:)];
    _bpCond = [self field:@"condition (optional): d == $10 && [$0900] > 2" width:0 action:@selector(addBreakpoint:)];
    NSButton *add = [self button:@"Add" action:@selector(addBreakpoint:)];
    NSButton *clear = [self button:@"Clear all" action:@selector(clearBreaks:)];
    NSStackView *row = hstack(@[_bpType, _bpRange, _bpCond, add, clear]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = vstack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    NSTextField *hint = [self label:@"Untick to disable; conditions are C expressions over registers, flags, [addr], value and addr"];
    hint.textColor = NSColor.secondaryLabelColor;
    NSStackView *v = vstack(@[row, hint, bpScroll]);
    fill(v, row);
    fill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* The cartridge's state as text left; its page map (with the legend) and
 * its text engine right. */
- (NSView *)buildCart
{
    NSScrollView *scroll;
    _cart = monoView(&scroll, NO);

    NSTextField *mapTitle = [self label:@"What the cartridge serves (a cell per 256-byte page)"];
    mapTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    _pageMap = [[PageMapView alloc] initWithFrame:NSZeroRect];
    fixSize(_pageMap);
    static NSString *const kinds[4] = { @"not the cartridge's", @"image ROM", @"hotspot", @"raster" };
    NSMutableArray *legend = [NSMutableArray array];
    for (int i = 0; i < 4; i++) {
        Swatch *sw = [[Swatch alloc] initWithFrame:NSMakeRect(0, 0, 12, 12)];
        sw.color = pageColor(i);
        [sw.widthAnchor constraintEqualToConstant:12].active = YES;
        [sw.heightAnchor constraintEqualToConstant:12].active = YES;
        [legend addObject:sw];
        [legend addObject:[self label:kinds[i]]];
    }

    NSTextField *textTitle = [self label:@"The text engine, as drawn"];
    textTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    _textGrid = [[TextGridView alloc] initWithFrame:NSZeroRect];
    fixSize(_textGrid);
    _textNote = [self label:@""];
    _textNote.textColor = NSColor.secondaryLabelColor;

    NSStackView *right = vstack(@[mapTitle, _pageMap, hstack(legend), textTitle, _textGrid, _textNote]);
    NSStackView *h = hstack(@[scroll, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [scroll.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [scroll.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
- (NSView *)buildFujinet
{
    _fujiNote = [self label:@""];
    _fujiNote.textColor = NSColor.secondaryLabelColor;
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = hstack(@[_fujiNote, [self button:@"Clear" action:@selector(fujiClear:)]]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = monoView(&fujiScroll, NO);
    _fujiState = monoView(&stateScroll, NO);
    NSTextField *title = [self label:@"FujiBus packets (SLIP-decoded)"];
    title.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *left = vstack(@[title, row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    fill(left, row);
    fill(left, fujiScroll);
    NSTextField *stateTitle = [self label:@"The cartridge's mailbox"];
    stateTitle.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    NSStackView *right = vstack(@[stateTitle, stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    fill(right, stateScroll);

    NSStackView *h = hstack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:520].active = YES;
    [right.widthAnchor constraintEqualToConstant:320].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

- (void)buildWindow
{
    _window = [[DebuggerPanelWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 1100, 780)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = [self button:@"Stop (F5)" action:@selector(toggleRun:)];
    NSButton *step = [self button:@"Step (F7)" action:@selector(step:)];
    NSButton *over = [self button:@"Step Over (F8)" action:@selector(stepOver:)];
    NSButton *outBtn = [self button:@"Step Out (⇧F8)" action:@selector(stepOut:)];
    NSButton *scan = [self button:@"Scanline+1" action:@selector(scanline:)];
    NSButton *frame = [self button:@"Frame+1" action:@selector(frame:)];
    _runBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
    _runBtn.keyEquivalentModifierMask = 0;
    step.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
    step.keyEquivalentModifierMask = 0;
    over.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    over.keyEquivalentModifierMask = 0;
    outBtn.keyEquivalent = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
    outBtn.keyEquivalentModifierMask = NSEventModifierFlagShift;
    NSPopUpButton *symbols = [self pullDown:@"Load Symbols…"
                                      items:@[@"From a File…", @"Beside the Cartridge"]
                                     action:@selector(loadSymbols:)];
    NSMutableArray *saveTitles = [NSMutableArray array];
    for (int i = 0; i < NUM_FILE_KINDS; i++) [saveTitles addObject:kFileTitles[i]];
    NSPopUpButton *save = [self pullDown:@"Save…" items:saveTitles action:@selector(saveFile:)];
    _status = [self label:@"Running"];
    _status.alignment = NSTextAlignmentRight;
    _status.textColor = NSColor.secondaryLabelColor;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = hstack(@[_runBtn, step, over, outBtn, scan, frame, symbols, save, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"Video", [self buildVideo]],
                        @[@"Sound & I/O", [self buildSound]], @[@"Breakpoints", [self buildBreaks]],
                        @[@"Cart", [self buildCart]], @[@"FujiNet", [self buildFujinet]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:6].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-6].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:6].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-6].active = YES;
        item.view = holder;
        [_tabs addTabViewItem:item];
    }
    const char *tab = getenv("O2_DEBUGGER_TAB");
    if (tab && *tab) {
        int t = -1;
        for (int i = 0; i < NUM_TABS; i++)
            if (!strcasecmp(tab, kTabNames[i])) t = i;
        if (t < 0 && tab[0] >= '0' && tab[0] <= '9') t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;

    NSStackView *root = vstack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions ------------------------------------------------------------------ */

- (void)fujiClear:(id)sender
{
    (void)sender;
    o2debug_fujinet_trace_clear(_dbg);
    [self refreshFujinet:YES];
}

- (void)toggleRun:(id)sender
{
    (void)sender;
    if (o2debug_is_stopped(_dbg)) o2debug_resume(_dbg); else o2debug_stop(_dbg);
    [self refreshAll];
}
- (void)step:(id)sender { (void)sender; o2debug_step(_dbg); [self refreshAll]; }
- (void)stepOver:(id)sender { (void)sender; o2debug_step_over(_dbg); [self refreshAll]; }
- (void)stepOut:(id)sender { (void)sender; o2debug_step_out(_dbg); [self refreshAll]; }
- (void)scanline:(id)sender { (void)sender; o2debug_scanline(_dbg, 1); [self refreshAll]; }
- (void)frame:(id)sender { (void)sender; o2debug_frame(_dbg, 1); [self refreshAll]; }

- (void)appendPrompt:(NSString *)text
{
    [_promptOut.textStorage appendAttributedString:
        [[NSAttributedString alloc] initWithString:text attributes:@{ NSFontAttributeName: _promptOut.font ?: [NSFont userFixedPitchFontOfSize:11] }]];
    [_promptOut scrollRangeToVisible:NSMakeRange(_promptOut.string.length, 0)];
}

- (void)runPrompt:(id)sender
{
    (void)sender;
    static char out[65536];
    NSString *cmd = [_promptIn.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (cmd.length == 0) return;
    if (![_history.lastObject isEqualToString:cmd]) [_history addObject:cmd];
    _historyPos = (NSInteger)_history.count;
    [self appendPrompt:[NSString stringWithFormat:@"> %@\n", cmd]];
    o2debug_command(_dbg, cmd.UTF8String, out, sizeof out);
    [self appendPrompt:[stripControl(out) stringByAppendingString:@"\n"]];
    _promptIn.stringValue = @"";
    [self refreshAll];
}

/* In the prompt, Tab completes instead of moving the focus, and Up/Down
 * walk the history. */
- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel
{
    (void)textView;
    if (control != _promptIn) return NO;
    if (sel == @selector(moveUp:) || sel == @selector(moveDown:)) {
        const NSInteger n = (NSInteger)_history.count;
        if (n == 0) return YES;
        _historyPos += sel == @selector(moveUp:) ? -1 : 1;
        if (_historyPos < 0) _historyPos = 0;
        if (_historyPos > n) _historyPos = n;
        _promptIn.stringValue = _historyPos < n ? _history[(NSUInteger)_historyPos] : @"";
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
        return YES;
    }
    if (sel != @selector(insertTab:)) return NO;
    NSString *text = _promptIn.stringValue;
    const NSRange sp = [text rangeOfString:@" " options:NSBackwardsSearch];
    NSString *word = sp.location == NSNotFound ? text : [text substringFromIndex:sp.location + 1];
    char comps[4096];
    const int n = o2debug_completions(_dbg, word.UTF8String, comps, sizeof comps);
    if (n == 1) {
        NSString *c = [[NSString stringWithUTF8String:comps] componentsSeparatedByString:@"\n"][0];
        NSString *head = sp.location == NSNotFound ? @"" : [text substringToIndex:sp.location + 1];
        _promptIn.stringValue = [NSString stringWithFormat:@"%@%@ ", head, c];
        [_promptIn.currentEditor setSelectedRange:NSMakeRange(_promptIn.stringValue.length, 0)];
    } else if (n > 1) {
        NSString *list = [NSString stringWithUTF8String:comps] ?: @"";
        if (![list hasSuffix:@"\n"]) list = [list stringByAppendingString:@"\n"];
        [self appendPrompt:list];
    }
    return YES;
}

/* "From a File…" asks; "Beside the Cartridge" takes <cart>.map/.lst/.sym
 * next to the opened cartridge. */
- (void)loadSymbols:(NSPopUpButton *)sender
{
    char msg[512];
    if (sender.selectedItem.tag == 1) {
        o2debug_load_symbols(_dbg, NULL, msg, sizeof msg);
    } else {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.title = @"Load Symbols (AS .map or .lst, WLA-DX .sym, or an address list)";
        p.allowedContentTypes = typesForExtensions(@[@"map", @"lst", @"sym", @"txt"]);
        const char *cart = o2session_cart_path(_session);
        if (cart && cart[0])
            [p setDirectoryURL:[NSURL fileURLWithPath:[[NSString stringWithUTF8String:cart]
                                                          stringByDeletingLastPathComponent] isDirectory:YES]];
        if ([p runModal] != NSModalResponseOK) return;
        o2debug_load_symbols(_dbg, [[p URL] fileSystemRepresentation], msg, sizeof msg);
    }
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = stripControl(msg);
    [self refreshAll];
}

- (void)saveFile:(NSPopUpButton *)sender
{
    const NSInteger kind = sender.selectedItem.tag;
    if (kind < 0 || kind >= NUM_FILE_KINDS) return;
    NSSavePanel *p = [NSSavePanel savePanel];
    p.title = [NSString stringWithFormat:@"Save %@", kFileTitles[kind]];
    [p setNameFieldStringValue:kFileNames[kind]];
    if ([p runModal] != NSModalResponseOK) return;
    char msg[512];
    o2debug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[stripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = stripControl(msg);
}

/* A register changes only while the machine is stopped: running, the next
 * instruction would overwrite it before anyone saw the edit land. */
- (void)applyRegister:(NSTextField *)sender
{
    long v;
    if (!o2debug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a register";
        return;
    }
    if (sender.tag >= 0 && sender.tag < NUM_REGS && parseNum(sender.stringValue, &v))
        o2debug_cpu_set(_dbg, kRegs[sender.tag].reg, (int)v);
    [self refreshAll];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long v;
    const int a = [self resolveAddr:_ramAddr.stringValue];
    if (a >= 0 && parseNum(_ramVal.stringValue, &v))
        o2debug_write(_dbg, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

/* Bring a RAM address's line to the top of the dump (the 512 bytes mirror
 * above $0800 wherever A9 is clear). */
- (void)gotoRam:(id)sender
{
    (void)sender;
    const int a = [self resolveAddr:_ramGoto.stringValue];
    if (a < RAM_BASE || (a & 0x0200)) {
        _status.stringValue = [NSString stringWithFormat:@"Not a console RAM address: %@", _ramGoto.stringValue];
        return;
    }
    const NSUInteger line = (NSUInteger)((a & (RAM_SIZE - 1)) / 16);
    const NSUInteger at = [RAM_HEADER length] + line * RAM_LINE_LEN;
    if (at >= _ram.string.length) return;
    /* to the end first, so scrolling back up leaves the line at the top */
    [_ram scrollRangeToVisible:NSMakeRange(_ram.string.length, 0)];
    [_ram scrollRangeToVisible:NSMakeRange(at, RAM_LINE_LEN - 1)];
    [_ram setSelectedRange:NSMakeRange(at, 6)];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)addr;
    [self refreshDisasm];
}

- (void)addBreakpoint:(id)sender
{
    (void)sender;
    static const int types[7] = {
        O2DEBUG_BP_EXEC, O2DEBUG_BP_READ, O2DEBUG_BP_WRITE,
        O2DEBUG_BP_READ | O2DEBUG_BP_WRITE,
        O2DEBUG_BP_IN, O2DEBUG_BP_OUT, O2DEBUG_BP_IN | O2DEBUG_BP_OUT
    };
    NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (range.length == 0) return;
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 7 ? sel : 0];
    const BOOL port = (type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
    NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
    const int start = [self resolveAddr:ends[0]];
    const int end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
    /* the 1802's I/O ports are 1-7 (N = 0 is no port at all) */
    if (start < 0 || end < 0 || end < start || (port && (start < 1 || end > 7))) {
        NSString *what = port ? @"port (1-7)" : @"address";
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad %@ %@\n", what, range]];
        _status.stringValue = [NSString stringWithFormat:@"Bad %@: %@", what, range];
        return;
    }
    const int bpId = o2debug_breakpoint_add(_dbg, type, (uint16_t)start, (uint16_t)end,
                                                 _bpCond.stringValue.UTF8String);
    if (bpId < 0) {
        _status.stringValue = @"The condition does not parse";
        [self appendPrompt:[NSString stringWithFormat:@"breakpoint: the condition does not parse: %@\n",
                            _bpCond.stringValue]];
        return;
    }
    _bpRange.stringValue = @"";
    _bpCond.stringValue = @"";
    [self refreshAll];
}

- (void)bpEnabled:(NSButton *)sender
{
    o2debug_breakpoint_enable(_dbg, (int)sender.tag, sender.state == NSControlStateValueOn);
    [self refreshAll];
}

- (void)bpRemove:(NSButton *)sender
{
    o2debug_breakpoint_remove(_dbg, (int)sender.tag);
    [self refreshAll];
}

- (void)clearBreaks:(id)sender
{
    (void)sender;
    o2debug_breakpoint_clear(_dbg);
    [self refreshAll];
}

/* ---- refreshers ------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = o2debug_is_stopped(_dbg) != 0;
    o2debug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    setButtonLit(_runBtn, stopped ? @"Run (F5)" : @"Stop (F5)", stopped);
}

- (void)refreshCpu
{
    o2debug_cpu c;
    o2debug_cpu_get(_dbg, &c);
    int vals[NUM_REGS];
    for (int i = 0; i < 16; i++) vals[i] = c.r[i];
    vals[16] = c.d;  vals[17] = c.df; vals[18] = c.p;  vals[19] = c.x;
    vals[20] = c.t;  vals[21] = c.b;  vals[22] = c.ie; vals[23] = c.q;
    for (int i = 0; i < NUM_REGS; i++) {
        if (_reg[i].currentEditor) continue;
        switch (kRegs[i].digits) {
        case 4:  _reg[i].stringValue = [NSString stringWithFormat:@"%04X", vals[i] & 0xffff]; break;
        case 2:  _reg[i].stringValue = [NSString stringWithFormat:@"%02X", vals[i] & 0xff]; break;
        case 1:  _reg[i].stringValue = [NSString stringWithFormat:@"%X", vals[i] & 0xf]; break;
        default: _reg[i].stringValue = [NSString stringWithFormat:@"%d", vals[i] ? 1 : 0]; break;
        }
    }
    for (int i = 0; i < 4; i++) _ef[i].state = c.ef[i] ? NSControlStateValueOn : NSControlStateValueOff;
    _cpuState.stringValue = [NSString stringWithFormat:
        @"PC $%04X (R%X)   state %s   mode %s   INT %d   DMA OUT %d",
        c.pc & 0xffff, c.p & 0xf, c.state_name ? c.state_name : "?", c.mode_name ? c.mode_name : "?",
        c.int_line, c.dma_line];
    _beam.stringValue = [NSString stringWithFormat:
        @"beam line %d   dot %d   frame %u   cycles %llu   instructions %llu",
        c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles, (unsigned long long)c.instructions];
}

- (void)refreshRam
{
    uint8_t ram[RAM_SIZE];
    o2debug_ram_get(_dbg, ram);
    NSMutableString *text = [NSMutableString stringWithString:RAM_HEADER];
    for (int row = 0; row < RAM_SIZE / 16; row++) {
        [text appendFormat:@"$%04X: ", RAM_BASE + row * 16];
        for (int col = 0; col < 16; col++) [text appendFormat:@"%02X ", ram[row * 16 + col]];
        [text appendString:@"\n"];
    }
    /* keep the reader's place */
    setTextKeepingPlace(_ram, text);
}

- (void)refreshDisasm
{
    static o2debug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        o2debug_cpu c;
        o2debug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)o2debug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    const int n = o2debug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0;
    BOOL havePc = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %04X  %-9s  %-16s %-24s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                          lines[i].comment[0] ? " ; " : "", lines[i].comment];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    if (havePc) {
        /* the current line in the accent, its text dark on the pale tan */
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: O2AccentColor(),
                                              NSForegroundColorAttributeName: NSColor.blackColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshVideo
{
    o2debug_video v;
    o2debug_video_get(_dbg, &v);
    NSMutableString *s = [NSMutableString string];
    [s appendString:@"CDP1861 (the Pixie)\n\n"];
    [s appendFormat:@"Display       %s   INP 1 (display on) %u times\n", v.display_on ? "on" : "off", v.inp1_count];
    [s appendFormat:@"Beam          line %d   dot %d   %s\n", v.vpos, v.hpos,
        v.in_display ? "in the picture (lines 80-207)" : "in the blanking"];
    [s appendFormat:@"Lines         INT %d   EFx %d   DMA OUT %d\n", v.int_line, v.efx_line, v.dma_line];
    [s appendFormat:@"Next INT      line %d   clock %llu\n", v.next_int_line, (unsigned long long)v.next_int_clock];
    [s appendFormat:@"Next EFx      line %d   clock %llu\n", v.next_efx_line, (unsigned long long)v.next_efx_clock];
    [s appendFormat:@"Next DMA      clock %llu\n", (unsigned long long)v.next_dma_clock];
    [s appendFormat:@"DMA pointer   R0 = $%04X   last fetch $%04X\n", v.dma_pointer, v.last_dma_addr];
    [s appendFormat:@"DMA bytes     %llu since power-on\n", (unsigned long long)v.dma_bytes];
    [s appendFormat:@"Short bursts  %u   (DMA bursts under 8 bytes: a $Cx long branch in flight)\n", v.short_lines];
    [s appendFormat:@"Frame         %u\n", v.frame];
    setTextKeepingPlace(_videoText, s);

    int w = 0, h = 0;
    if (o2debug_video_view(_dbg, (int)_videoViewSel.indexOfSelectedItem, _viewPx, &w, &h) && w > 0 && h > 0) {
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_viewPx, (CFIndex)w * h * 4);
        CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, provider,
                                       NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CFRelease(data);
        CGColorSpaceRelease(space);
        _videoPic.image = img;   /* the view owns it now */
    }
}

- (void)refreshSound
{
    o2debug_io io;
    o2debug_io_get(_dbg, &io);
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Beeper       Q %d   %s   %d Hz\n", io.q, io.q ? "sounding" : "silent", io.beep_hz];
    [s appendFormat:@"Key latch    %d   (OUT 2)   EF3 %d (keypad A)   EF4 %d (keypad B)\n",
        io.keylatch, io.ef3, io.ef4];
    for (int k = 0; k < 2; k++) {
        [s appendFormat:@"Keypad %c    ", 'A' + k];
        BOOL any = NO;
        for (int key = 0; key < O2_ACT_PER_PORT; key++)
            if (io.keys[k] & (1u << key)) {
                [s appendFormat:@" %d", key];
                any = YES;
            }
        [s appendString:any ? @" held\n" : @" (nothing held)\n"];
    }
    [s appendFormat:@"CLEAR        %s\n", io.clear_held ? "held" : "up"];
    [s appendString:@"Last OUT    "];
    for (int p = 1; p < 8; p++) [s appendFormat:@" %d=$%02X", p, io.last_out[p]];
    [s appendFormat:@"\nINP 1        %u times (display on)\n", io.inp1_count];
    [s appendFormat:@"BIOS         %s\n", io.have_bios ? "imported"
                    : (io.stub_parked ? "the stand-in, parked: a game needs the BIOS"
                                      : "the stand-in (runs CONFIG and FujiNet apps)")];
    setTextKeepingPlace(_sound, s);

    const int latched = io.keylatch < O2_ACT_PER_PORT ? io.keylatch : -1;
    for (int k = 0; k < 2; k++) [_keypadDiag[k] setHeld:io.keys[k] latched:latched];
}

- (void)refreshBps
{
    static o2debug_breakpoint bps[MAX_BPS];
    const int n = o2debug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:[self label:@"No breakpoints"]];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { O2DEBUG_BP_EXEC, "exec" }, { O2DEBUG_BP_READ, "read" }, { O2DEBUG_BP_WRITE, "write" },
            { O2DEBUG_BP_IN, "in" }, { O2DEBUG_BP_OUT, "out" },
        };
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%-3d ", bps[i].id];
        BOOL first = YES;
        for (int k = 0; k < 5; k++)
            if (bps[i].type & kinds[k].bit) {
                [what appendFormat:@"%s%s", first ? "" : "/", kinds[k].name];
                first = NO;
            }
        const BOOL port = (bps[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
        [what appendFormat:(port ? @"  port %d" : @"  $%04X"), bps[i].start];
        if (bps[i].end != bps[i].start) [what appendFormat:(port ? @"-%d" : @"-$%04X"), bps[i].end];
        char label[64];
        if (!port && o2debug_address_label(_dbg, bps[i].start, label, sizeof label) > 0)
            [what appendFormat:@"  %s", label];
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        [what appendFormat:@"   hits %u", bps[i].hits];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = [self button:@"Remove" action:@selector(bpRemove:)];
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:hstack(@[box, remove])];
    }
}

- (void)refreshCart
{
    o2debug_cart c;
    char info[256];
    o2debug_cart_get(_dbg, &c);
    o2debug_cart_info(_dbg, info, sizeof info);
    [_pageMap setTypes:c.page_type];
    [_textGrid setCart:&c];
    _textNote.stringValue = [NSString stringWithFormat:@"cursor row %d, column %d%s",
                             c.cursor_row, c.cursor_col, c.inverse ? "; printing inverse" : ""];

    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", stripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge running)\n"];
        setTextKeepingPlace(_cart, s);
        return;
    }
    [s appendFormat:@"Mode        %s (%d)   %s%s%s\n", c.mode_name ? c.mode_name : "?", c.mode,
        c.mailbox_mode ? "the mailbox is open" : "a game alone",
        c.direct ? "   opened from a file" : "", c.booted_game ? "   a game has booted" : ""];
    [s appendFormat:@"Link        %s%s\n", c.link_up ? "up" : "down", c.busy ? " (busy)" : ""];
    [s appendFormat:@"Live image  %s   %u bytes   %d blocks   CRC-32 %08X%s\n",
        c.live_st2 ? "ST2" : "raw", c.live_size, c.live_blocks, c.live_crc,
        c.live_mailbox ? "   claims the mailbox" : ""];
    if (c.staged)
        [s appendFormat:@"Staged      %s   CRC-32 %08X%s\n",
            c.staged_config ? "CONFIG" : (c.staged_claim ? "a FujiNet app" : "a game"),
            c.staged_crc, c.armed ? "   armed" : ""];
    else
        [s appendString:@"Staged      nothing\n"];
    [s appendFormat:@"Swaps       %d\n", c.swaps];
    [s appendFormat:@"Boot        state %d   %d%%   error %d   %u of %u bytes\n",
        c.boot_state, c.boot_pct, c.boot_err, c.boot_got, c.boot_total];
    [s appendFormat:@"Mailbox     ACKSEQ $%02X   STATUS $%02X   last error %d   reply cmd $%02X   RXLEN %d\n",
        c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen];
    [s appendFormat:@"Raster      %d frames served (mod 256)   %u short DMA bursts\n", c.frames, c.short_lines];
    [s appendFormat:@"Queue       %u hotspot events waiting\n", c.queue_depth];
    setTextKeepingPlace(_cart, s);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
- (void)refreshFujinet:(BOOL)force
{
    static o2debug_fuji_packet p[TRACE_MAX];
    static char state[2048];
    char status[128];
    int n = o2debug_fujinet_trace(_dbg, p, TRACE_MAX);
    if (n < 0) n = 0;

    if (o2session_fujinet_external(_session)) {
        o2session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s. Cartridge: %@.",
                                 o2session_boip_endpoint(_session), stripControl(status)];
    } else if (!o2session_fujinet_running(_session)) {
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    } else {
        o2session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on. Cartridge: %@.", stripControl(status)];
    }
    state[0] = '\0';
    o2debug_fujinet_state(_dbg, state, (int)sizeof state);
    setTextKeepingPlace(_fujiState, stripControl(state));

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || atEnd(_fujiText);
    NSMutableString *s = [NSMutableString string];
    for (int i = 0; i < n; i++) {
        if (i) [s appendString:@"\n"];
        [s appendString:[NSString stringWithUTF8String:p[i].text] ?: @""];
        if (p[i].repeat > 1) [s appendFormat:@" ×%u", p[i].repeat];
        [s appendString:@"\n     "];
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            [s appendFormat:@"%02X ", p[i].data[k]];
        if (p[i].nbytes > (int)sizeof p[i].data) [s appendString:@"…"];
    }
    if (n == 0) [s appendString:@"(no FujiBus packets since the trace was cleared)"];
    setLogText(_fujiText, s, follow);
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshCpu];
    [self refreshRam];
    [self refreshDisasm];
    [self refreshVideo];
    [self refreshSound];
    [self refreshBps];
    [self refreshCart];
    [self refreshFujinet:NO];
}
@end

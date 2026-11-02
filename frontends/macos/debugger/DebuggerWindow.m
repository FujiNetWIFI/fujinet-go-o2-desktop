/*
 * Debugger window (AppKit) over this app's Intel 8048 / 8244 debugger engine,
 * via core/include/o2debug.h. Mirrors the GTK, Qt and Win32 debuggers tab for
 * tab: Prompt, CPU & RAM, Disassembly, Video (the 8244: VideoTab.m), EF9340,
 * Sound & I/O, Breakpoints, Cart, FujiNet.
 *
 * Opening the window attaches the engine, which stops the machine; closing
 * it detaches, and the machine runs on. The window refreshes only while it
 * can be seen, and only the tab that shows: a stopped machine on a change, a
 * running one a couple of times a second (the Video tab ten).
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DebuggerWindow.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "DebugViews.h"
#import "VideoTab.h"
#import "../KeyForward.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "o2debug.h"
#include "carts.h"

#define DISASM_WINDOW 48
#define MAX_BPS 64
#define NUM_SCALARS 10       /* A PC PSW SP MB T CNT P1 P2 BUS */
#define TRACE_MAX 512        /* FujiNet packets shown */

/* The tabs, in order. */
#define TAB_PROMPT      0
#define TAB_CPU         1
#define TAB_DISASM      2
#define TAB_VIDEO       3
#define TAB_EF          4
#define TAB_IO          5
#define TAB_BREAKPOINTS 6
#define TAB_CART        7
#define TAB_FUJINET     8
#define NUM_TABS        9

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

/* ---- the keyboard matrix and the sticks, as the console sees them ---------------- */

#define IO_CELL_W 40.0
#define IO_CELL_H 22.0
#define IO_LEFT   22.0
#define IO_TOP    16.0
#define IO_STICK_W 30.0

/* The 8 x 8 keyboard matrix (a cell per key, filled while held), the two
 * sticks' switches and the RESET key. */
@interface O2IoDiagramView : NSView
- (void)setIo:(const o2debug_io *)io;
@end

@implementation O2IoDiagramView {
    uint64_t _keys;
    uint8_t _sticks[2];
    int _reset;
    int _machine;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize
{
    return NSMakeSize(IO_LEFT + 8 * IO_CELL_W + 24 + 2 * (3 * IO_STICK_W + 14), IO_TOP + 8 * IO_CELL_H + 2);
}

- (void)setIo:(const o2debug_io *)io
{
    if (io->keys == _keys && !memcmp(io->sticks, _sticks, sizeof _sticks) &&
        io->reset_held == _reset && io->machine == _machine)
        return;
    _keys = io->keys;
    memcpy(_sticks, io->sticks, sizeof _sticks);
    _reset = io->reset_held;
    _machine = io->machine;
    [self setNeedsDisplay:YES];
}

- (void)cell:(NSRect)r text:(NSString *)text lit:(BOOL)lit
{
    NSBezierPath *path = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 1, 1) xRadius:3 yRadius:3];
    [(lit ? O2AccentColor() : NSColor.controlBackgroundColor) setFill];
    [path fill];
    [NSColor.separatorColor setStroke];
    [path stroke];
    if (!text.length) return;
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary *a = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                         NSParagraphStyleAttributeName: para,
                         NSForegroundColorAttributeName: lit ? NSColor.whiteColor : NSColor.labelColor };
    const NSSize sz = [text sizeWithAttributes:a];
    [text drawInRect:NSMakeRect(r.origin.x + 2, NSMidY(r) - sz.height / 2, r.size.width - 4, sz.height)
      withAttributes:a];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont systemFontOfSize:9],
                            NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    for (int c = 0; c < 8; c++)
        [[NSString stringWithFormat:@"%d", c] drawAtPoint:NSMakePoint(IO_LEFT + c * IO_CELL_W + IO_CELL_W / 2 - 3, 2)
                                           withAttributes:head];
    for (int row = 0; row < 8; row++) {
        [[NSString stringWithFormat:@"%d", row] drawAtPoint:NSMakePoint(8, IO_TOP + row * IO_CELL_H + 5)
                                             withAttributes:head];
        for (int c = 0; c < 8; c++) {
            const int key = row * 8 + c;
            const char *label = o2session_key_label(_machine, key);
            [self cell:NSMakeRect(IO_LEFT + c * IO_CELL_W, IO_TOP + row * IO_CELL_H, IO_CELL_W, IO_CELL_H)
                  text:(label ? [NSString stringWithUTF8String:label] : @"")
                   lit:((_keys >> key) & 1u) != 0];
        }
    }

    /* the sticks: up, left, button, right, down as a cross */
    const CGFloat x0 = IO_LEFT + 8 * IO_CELL_W + 24;
    for (int p = 0; p < 2; p++) {
        const CGFloat x = x0 + p * (3 * IO_STICK_W + 14);
        const unsigned s = _sticks[p];
        [[NSString stringWithFormat:@"Stick %d", p + 1] drawAtPoint:NSMakePoint(x + IO_STICK_W, 2) withAttributes:head];
        [self cell:NSMakeRect(x + IO_STICK_W, IO_TOP, IO_STICK_W, IO_CELL_H) text:@"▲" lit:(s & 1u) != 0];
        [self cell:NSMakeRect(x, IO_TOP + IO_CELL_H, IO_STICK_W, IO_CELL_H) text:@"◀" lit:(s & 8u) != 0];
        [self cell:NSMakeRect(x + IO_STICK_W, IO_TOP + IO_CELL_H, IO_STICK_W, IO_CELL_H) text:@"FIRE"
               lit:(s & 16u) != 0];
        [self cell:NSMakeRect(x + 2 * IO_STICK_W, IO_TOP + IO_CELL_H, IO_STICK_W, IO_CELL_H) text:@"▶"
               lit:(s & 2u) != 0];
        [self cell:NSMakeRect(x + IO_STICK_W, IO_TOP + 2 * IO_CELL_H, IO_STICK_W, IO_CELL_H) text:@"▼"
               lit:(s & 4u) != 0];
    }
    [self cell:NSMakeRect(x0, IO_TOP + 4 * IO_CELL_H, 2 * IO_STICK_W, IO_CELL_H) text:@"RESET" lit:_reset != 0];
}
@end

/* ---- the window --------------------------------------------------------------------- */

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
    int _slowTicks;
    NSTabView *_tabs;

    NSButton *_runBtn;
    NSTextField *_status;

    NSTextView *_promptOut;
    NSTextField *_promptIn;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyPos;

    NSTextField *_reg[NUM_SCALARS];
    NSTextField *_bank0[8], *_bank1[8];
    NSButton *_flag[5];
    NSButton *_stat[7];
    NSTextField *_cpuState, *_beam;
    NSTextView *_iram, *_xram;
    NSPopUpButton *_ramSpace;
    NSTextField *_ramAddr, *_ramVal;

    NSButton *_followPc;
    NSTextField *_jump;
    O2LineTextView *_disasm;
    uint16_t _disasmAddr;
    uint16_t _lineAddr[DISASM_WINDOW];
    int _lineCount;

    O2VideoTab *_video;

    NSTextField *_efNote;
    NSTextView *_efRegs, *_efPage;
    O2PictureView *_efPic;
    uint32_t *_px;

    NSTextView *_sound;
    O2IoDiagramView *_ioDiag;

    NSPopUpButton *_bpType, *_bpSpace;
    NSTextField *_bpRange, *_bpCond;
    NSStackView *_bpList;

    NSTextView *_cart, *_mailbox;

    NSTextField *_fujiNote;
    NSTextView *_fujiText, *_fujiState;
    uint64_t _fujiLast;
    int _fujiCount;
}

/* The scalar registers, in the order the CPU tab lays them out: each with
 * its o2debug_reg and the hex digits it shows (1, 2 or 3). */
static const struct { NSString *__unsafe_unretained name; int reg; int digits; } kRegs[NUM_SCALARS] = {
    { @"A", O2_REG_A, 2 },     { @"PC", O2_REG_PC, 3 },   { @"PSW", O2_REG_PSW, 2 },
    { @"SP", O2_REG_SP, 1 },   { @"MB", O2_REG_MB, 1 },   { @"T", O2_REG_T, 2 },
    { @"CNT", O2_REG_CNT, 2 }, { @"P1", O2_REG_P1, 2 },   { @"P2", O2_REG_P2, 2 },
    { @"BUS", O2_REG_BUS, 2 },
};

/* The PSW's flags as checkboxes, and the register each sets. */
static const struct { NSString *__unsafe_unretained name; int reg; } kFlags[5] = {
    { @"C", O2_REG_C }, { @"AC", O2_REG_AC }, { @"F0", O2_REG_F0 }, { @"F1", O2_REG_F1 },
    { @"BS (bank 1)", O2_REG_BS },
};

#define NUM_FILE_KINDS 7
static const char *const kFileKinds[NUM_FILE_KINDS] = { "dis", "ram", "mailbox", "vdc", "font", "screen", "regs" };
static NSString *const kFileTitles[NUM_FILE_KINDS] = {
    @"Disassembly…", @"RAM (internal and external)…", @"Mailbox page…", @"8244 registers…",
    @"Character generator…", @"Screen (PPM)…", @"Registers…"
};
static NSString *const kFileNames[NUM_FILE_KINDS] = {
    @"disassembly.asm", @"ram.bin", @"mailbox.bin", @"vdc.bin", @"font.bin", @"screen.ppm", @"registers.txt"
};

/* O2_DEBUGGER_TAB takes an index or one of these. */
static const char *const kTabNames[NUM_TABS] = {
    "prompt", "cpu", "disassembly", "video", "ef9340", "io", "breakpoints", "cart", "fujinet"
};

/* ---- helpers ------------------------------------------------------------------------- */

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

/* A hex dump: a header of column numbers, then 16 bytes a line from `base`. */
static NSString *hexDump(const uint8_t *data, int n, int base, NSString *lineFormat)
{
    NSMutableString *s = [NSMutableString string];
    const NSUInteger pad = [[NSString stringWithFormat:lineFormat, base] length];
    [s appendString:[@"" stringByPaddingToLength:pad withString:@" " startingAtIndex:0]];
    for (int c = 0; c < 16; c++) [s appendFormat:@" %X ", c];
    [s appendString:@"\n"];
    for (int row = 0; row < n; row += 16) {
        [s appendFormat:lineFormat, base + row];
        for (int c = 0; c < 16 && row + c < n; c++) [s appendFormat:@"%02X ", data[row + c]];
        [s appendString:@"\n"];
    }
    return s;
}

/* A button lit in the accent, its title white on the red; or plain. */
static void setButtonLit(NSButton *b, NSString *title, BOOL lit)
{
    b.bezelColor = lit ? O2AccentColor() : nil;
    b.attributedTitle = [[NSAttributedString alloc] initWithString:title attributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize],
        NSForegroundColorAttributeName: lit ? NSColor.whiteColor : NSColor.controlTextColor }];
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

/* ---- lifetime ----------------------------------------------------------------------- */

+ (void)showForSession:(o2session *)session
{
    if (!g_debugger) g_debugger = [[O2DebuggerWindow alloc] initWithSession:session];
    [g_debugger->_window makeKeyAndOrderFront:nil];
    o2debug_attach(g_debugger->_dbg);
    [g_debugger updateTick];
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
    _px = calloc((size_t)O2DEBUG_VDC_VIEW_MAX_PIXELS * 4, sizeof *_px);
    _history = [NSMutableArray array];
    [self buildWindow];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_window];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_tick invalidate];
    free(_px);
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object != _window) return;
    o2debug_detach(_dbg);
    [_tick invalidate];
    _tick = nil;
}

/* The tick runs only while the window can be seen. */
- (void)updateTick
{
    const BOOL want = [_window isVisible] && ([_window occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_tick) {
        __weak O2DebuggerWindow *weakSelf = self;
        _tick = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
            (void)t;
            [weakSelf onTick];
        }];
        _tick.tolerance = 0.02;
    } else if (!want && _tick) {
        [_tick invalidate];
        _tick = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    const BOOL had = _tick != nil;
    [self updateTick];
    if (!had && _tick) [self refreshAll];     /* it was hidden: catch up */
}

- (NSInteger)selectedTab
{
    return [_tabs indexOfTabViewItem:_tabs.selectedTabViewItem];
}

- (void)onTick
{
    if (![_window isVisible]) return;
    const NSInteger tab = [self selectedTab];
    const unsigned gen = o2debug_generation(_dbg);
    const BOOL stopped = o2debug_is_stopped(_dbg) != 0;
    const BOOL slow = ++_slowTicks >= 5;     /* twice a second */
    if (slow) _slowTicks = 0;

    if (gen != _seenGen || stopped != _wasStopped) {
        _seenGen = gen;
        _wasStopped = stopped;
        [self refreshAll];
    } else if (tab == TAB_VIDEO && !stopped) {
        /* running: the 8244 live, ten times a second */
        [_video refresh];
    } else if (slow && (!stopped || tab == TAB_FUJINET)) {
        if (!stopped) [self refreshStatus];
        /* a running machine has no disassembly worth redrawing */
        if (!(tab == TAB_DISASM && !stopped)) [self refreshTab:tab];
    }
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    [self refreshTab:[_tabs indexOfTabViewItem:item]];
}

/* ---- construction -------------------------------------------------------------------- */

- (NSView *)buildPrompt
{
    NSScrollView *outScroll;
    _promptOut = O2MonoView(&outScroll, YES);
    _promptOut.string = @"Debugger prompt. Type 'help' for every command.\n";
    _promptIn = O2Field(@"command (help, step, break $0412, bpxw $E3, print a, mem prog $400 32, vdc, ...) — Tab completes, ↑↓ history",
                        0, self, @selector(runPrompt:));
    _promptIn.delegate = self;

    NSStackView *v = O2VStack(@[outScroll, _promptIn]);
    O2Fill(v, outScroll);
    O2Fill(v, _promptIn);
    return v;
}

/* A, PC, PSW and the rest as fields, R0-R7 of both register banks beside
 * them, the PSW's flags as boxes (editable while stopped) and the timer's and
 * lines' state (read-only); then the 8048's two RAMs. */
- (NSView *)buildCpu
{
    for (int i = 0; i < NUM_SCALARS; i++) {
        const int digits = kRegs[i].digits;
        _reg[i] = O2Field(@"", digits == 3 ? 44 : (digits == 2 ? 36 : 28), self, @selector(applyRegister:));
        _reg[i].tag = i;
    }
    for (int i = 0; i < 8; i++) {
        _bank0[i] = O2Field(@"", 36, self, @selector(applyRegister:));
        _bank0[i].tag = 100 + i;
        _bank1[i] = O2Field(@"", 36, self, @selector(applyRegister:));
        _bank1[i].tag = 200 + i;
    }
    NSMutableArray<NSArray<NSView *> *> *rows = [NSMutableArray array];
    for (int row = 0; row < NUM_SCALARS; row++) {
        NSView *l1 = O2Label(@""), *f1 = O2Label(@""), *l2 = O2Label(@""), *f2 = O2Label(@"");
        if (row < 8) {
            l1 = O2Label([NSString stringWithFormat:@"R%d", row]);
            f1 = _bank0[row];
            l2 = O2Label([NSString stringWithFormat:@"R%d'", row]);
            f2 = _bank1[row];
        }
        [rows addObject:@[ O2Label(kRegs[row].name), _reg[row], l1, f1, l2, f2 ]];
    }
    NSGridView *regs = [NSGridView gridViewWithViews:rows];
    regs.rowSpacing = 4;
    regs.columnSpacing = 6;
    for (int col = 0; col < 6; col += 2) [regs columnAtIndex:col].xPlacement = NSGridCellPlacementTrailing;
    [regs columnAtIndex:2].leadingPadding = 16;
    [regs columnAtIndex:4].leadingPadding = 16;

    NSMutableArray<NSView *> *flags = [NSMutableArray arrayWithObject:O2Label(@"PSW")];
    for (int i = 0; i < 5; i++) {
        _flag[i] = [NSButton checkboxWithTitle:kFlags[i].name target:self action:@selector(flagClicked:)];
        _flag[i].tag = i;
        [flags addObject:_flag[i]];
    }
    static NSString *const statTitles[7] = { @"TF", @"EN TCNTI", @"EN I", @"in interrupt", @"T0", @"T1", @"INT" };
    NSMutableArray<NSView *> *stats = [NSMutableArray arrayWithObject:O2Label(@"Timer and lines")];
    for (int i = 0; i < 7; i++) {
        _stat[i] = [NSButton checkboxWithTitle:statTitles[i] target:nil action:NULL];
        _stat[i].enabled = NO;   /* the machine's own state: shown, not set here */
        [stats addObject:_stat[i]];
    }
    _cpuState = O2Label(@"");
    _beam = O2Note(@"");
    NSTextField *hint = O2Note(@"Enter applies an edit while the machine is stopped. R0'-R7' are register bank 1; the PSW boxes set its bits");

    /* the two RAMs: 64 bytes inside, 128 outside ($00-$7F) */
    NSScrollView *iramScroll, *xramScroll;
    _iram = O2MonoView(&iramScroll, NO);
    _xram = O2MonoView(&xramScroll, NO);
    [iramScroll.heightAnchor constraintEqualToConstant:92].active = YES;
    [xramScroll.heightAnchor constraintEqualToConstant:156].active = YES;
    _ramSpace = [[NSPopUpButton alloc] init];
    [_ramSpace addItemsWithTitles:@[@"Internal RAM", @"External RAM"]];
    _ramAddr = O2Field(@"$20", 70, self, @selector(writeRam:));
    _ramVal = O2Field(@"$00", 60, self, @selector(writeRam:));
    NSStackView *edit = O2HStack(@[O2Label(@"Write"), _ramSpace, _ramAddr, O2Label(@"="), _ramVal,
                                   O2Button(@"Write", self, @selector(writeRam:))]);
    NSTextField *ramHint = O2Note(@"A page the cartridge serves takes an external write in the cartridge's own buffer");

    NSStackView *left = O2VStack(@[regs, O2HStack(flags), O2HStack(stats), hint, _cpuState, _beam]);
    NSStackView *right = O2VStack(@[O2Title(@"Internal RAM ($00-$3F; the register banks are $00-$07 and $18-$1F)"),
                                    iramScroll,
                                    O2Title(@"External RAM ($00-$7F)"), xramScroll, edit, ramHint]);
    O2Fill(right, iramScroll);
    O2Fill(right, xramScroll);
    NSStackView *h = O2HStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 16;
    [right.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    return h;
}

- (NSView *)buildDisasm
{
    _followPc = [NSButton checkboxWithTitle:@"Follow PC" target:self action:@selector(refreshDisasm)];
    _followPc.state = NSControlStateValueOn;
    _jump = O2Field(@"address or label", 150, self, @selector(jumpTo:));
    NSTextField *hint = O2Note(@"Click a line to toggle its breakpoint (*); scroll to browse");
    NSStackView *row = O2HStack(@[_followPc, O2Label(@"Jump to"), _jump, hint]);

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = NO;
    scroll.hasHorizontalScroller = YES;
    _disasm = [[O2LineTextView alloc] initWithFrame:NSMakeRect(0, 0, 600, 600)];
    _disasm.editable = NO;
    _disasm.richText = NO;
    _disasm.selectable = NO;
    _disasm.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _disasm.horizontallyResizable = YES;
    _disasm.textContainer.widthTracksTextView = NO;
    _disasm.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _disasm.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    __weak O2DebuggerWindow *weakSelf = self;
    _disasm.onClickLine = ^(int line) {
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

    NSStackView *v = O2VStack(@[row, scroll]);
    O2Fill(v, scroll);
    return v;
}

/* The G7400's EF9340/41 text chip: its registers and the 40 x 24 page as
 * text on the left, the text layer as it renders on the right. On the other
 * machines it says so. */
- (NSView *)buildEf
{
    _efNote = O2Note(@"");
    NSScrollView *regsScroll, *pageScroll;
    _efRegs = O2MonoView(&regsScroll, NO);
    _efPage = O2MonoView(&pageScroll, NO);
    [regsScroll.heightAnchor constraintEqualToConstant:190].active = YES;
    NSStackView *left = O2VStack(@[_efNote, O2Title(@"Registers"), regsScroll,
                                   O2Title(@"The text page (40 x 24)"), pageScroll]);
    left.distribution = NSStackViewDistributionFill;
    O2Fill(left, regsScroll);
    O2Fill(left, pageScroll);

    _efPic = [[O2PictureView alloc] initWithFrame:NSMakeRect(0, 0, 480, 360)];
    [_efPic.widthAnchor constraintGreaterThanOrEqualToConstant:320].active = YES;
    [_efPic.heightAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
    NSStackView *right = O2VStack(@[O2Title(@"The text layer, rendered"), _efPic]);
    O2Fill(right, _efPic);

    NSStackView *h = O2HStack(@[left, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:380].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The keyboard matrix and the sticks live over the ports' decoded text. */
- (NSView *)buildIo
{
    NSScrollView *scroll;
    _sound = O2MonoView(&scroll, NO);
    _ioDiag = [[O2IoDiagramView alloc] initWithFrame:NSZeroRect];
    O2FixSize(_ioDiag);
    NSTextField *hint = O2Note(@"Filled: held, as the console sees it (the matrix is read a row at a time by P20-P22).");
    NSStackView *v = O2VStack(@[O2Title(@"Keyboard matrix, sticks and RESET"), _ioDiag, hint, scroll]);
    v.distribution = NSStackViewDistributionFill;   /* the text takes the rest of the height */
    O2Fill(v, scroll);
    return v;
}

- (void)bpTypeChanged:(id)sender
{
    (void)sender;
    [self rebuildBpSpace];
}

/* The types: Execute, Read, Write, Read/Write, Port In, Port Out, Port In/Out. */
- (void)rebuildBpSpace
{
    const NSInteger type = _bpType.indexOfSelectedItem;
    const BOOL port = type >= 4;
    const BOOL exec = type == 0;
    [_bpSpace removeAllItems];
    [_bpSpace addItemsWithTitles:port ? @[@"BUS", @"P1", @"P2"]
                                      : @[@"Program", @"External data", @"Internal RAM"]];
    _bpSpace.enabled = !exec;
    _bpRange.enabled = !port;
    _bpRange.placeholderString = port ? @"(a port has no address)"
                                      : @"$0412, $0400-$04FF, or a label";
}

- (NSView *)buildBreaks
{
    _bpType = [[NSPopUpButton alloc] init];
    [_bpType addItemsWithTitles:@[@"Execute", @"Read", @"Write", @"Read/Write", @"Port In", @"Port Out", @"Port In/Out"]];
    _bpType.target = self;
    _bpType.action = @selector(bpTypeChanged:);
    _bpSpace = [[NSPopUpButton alloc] init];
    _bpRange = O2Field(@"", 230, self, @selector(addBreakpoint:));
    _bpCond = O2Field(@"condition (optional): a == $10 && vpos > 100", 0, self, @selector(addBreakpoint:));
    [self rebuildBpSpace];
    NSStackView *row = O2HStack(@[_bpType, _bpSpace, _bpRange, _bpCond, O2Button(@"Add", self, @selector(addBreakpoint:)),
                                  O2Button(@"Clear all", self, @selector(clearBreaks:))]);

    /* The rows scroll: a session can collect more breakpoints than the tab
     * is tall. The stack is the scroll view's document, pinned to the top
     * and to the clip view's width so rows keep the tab's width. */
    _bpList = O2VStack(@[]);
    _bpList.translatesAutoresizingMaskIntoConstraints = NO;
    NSScrollView *bpScroll = [[NSScrollView alloc] init];
    bpScroll.hasVerticalScroller = YES;
    bpScroll.drawsBackground = NO;
    bpScroll.documentView = _bpList;
    NSClipView *clip = bpScroll.contentView;
    [_bpList.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;
    [_bpList.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [_bpList.widthAnchor constraintEqualToAnchor:clip.widthAnchor].active = YES;
    NSTextField *hint = O2Note(@"Untick to disable; conditions are C expressions over registers, flags, [addr], value, addr, vpos, hpos");
    NSStackView *v = O2VStack(@[row, hint, bpScroll]);
    O2Fill(v, row);
    O2Fill(v, bpScroll);
    [bpScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
                         forOrientation:NSLayoutConstraintOrientationVertical];
    return v;
}

/* The cartridge's state as text left; the mailbox page ($F20-$FFF) as the
 * console reads it, right. */
- (NSView *)buildCart
{
    NSScrollView *scroll, *mailScroll;
    _cart = O2MonoView(&scroll, NO);
    _mailbox = O2MonoView(&mailScroll, NO);
    NSStackView *right = O2VStack(@[O2Title(@"The mailbox page ($F20-$FFF)"), mailScroll]);
    right.distribution = NSStackViewDistributionFill;
    O2Fill(right, mailScroll);
    NSStackView *h = O2HStack(@[scroll, right]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [scroll.widthAnchor constraintGreaterThanOrEqualToConstant:420].active = YES;
    [right.widthAnchor constraintEqualToConstant:420].active = YES;
    [scroll.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* The packets left (a note and Clear over them), the cartridge's mailbox
 * right. */
- (NSView *)buildFujinet
{
    _fujiNote = O2Note(@"");
    _fujiNote.lineBreakMode = NSLineBreakByTruncatingTail;
    [_fujiNote setContentHuggingPriority:NSLayoutPriorityDefaultLow
                          forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_fujiNote setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = O2HStack(@[_fujiNote, O2Button(@"Clear", self, @selector(fujiClear:))]);

    NSScrollView *fujiScroll, *stateScroll;
    _fujiText = O2MonoView(&fujiScroll, NO);
    _fujiState = O2MonoView(&stateScroll, NO);
    NSStackView *left = O2VStack(@[O2Title(@"FujiBus packets (SLIP-decoded)"), row, fujiScroll]);
    left.distribution = NSStackViewDistributionFill;   /* the log takes the column's height */
    O2Fill(left, row);
    O2Fill(left, fujiScroll);
    NSStackView *right = O2VStack(@[O2Title(@"The cartridge's mailbox"), stateScroll]);
    right.distribution = NSStackViewDistributionFill;
    O2Fill(right, stateScroll);

    NSStackView *h = O2HStack(@[left, right]);
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
        initWithContentRect:NSMakeRect(0, 0, 1180, 800)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Debugger";
    _window.releasedWhenClosed = NO;
    _window.delegate = self;

    /* Toolbar row. F5/F7/F8/Shift+F8 work as the buttons' key equivalents
     * wherever the focus is inside the window. */
    _runBtn = O2Button(@"Stop (F5)", self, @selector(toggleRun:));
    NSButton *step = O2Button(@"Step (F7)", self, @selector(step:));
    NSButton *over = O2Button(@"Step Over (F8)", self, @selector(stepOver:));
    NSButton *outBtn = O2Button(@"Step Out (⇧F8)", self, @selector(stepOut:));
    NSButton *scan = O2Button(@"Scanline+1", self, @selector(scanline:));
    NSButton *frame = O2Button(@"Frame+1", self, @selector(frame:));
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
    _status = O2Note(@"Running");
    _status.alignment = NSTextAlignmentRight;
    _status.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *toolbar = O2HStack(@[_runBtn, step, over, outBtn, scan, frame, symbols, save, _status]);
    [_status setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];

    _video = [[O2VideoTab alloc] initWithDebugger:_dbg];
    _tabs = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Prompt", [self buildPrompt]], @[@"CPU & RAM", [self buildCpu]],
                        @[@"Disassembly", [self buildDisasm]], @[@"Video", _video.view],
                        @[@"EF9340", [self buildEf]], @[@"Sound & I/O", [self buildIo]],
                        @[@"Breakpoints", [self buildBreaks]], @[@"Cart", [self buildCart]],
                        @[@"FujiNet", [self buildFujinet]] ];
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
        if (t < 0 && !strcasecmp(tab, "8244")) t = TAB_VIDEO;
        if (t < 0 && !strcasecmp(tab, "sound")) t = TAB_IO;
        if (t < 0 && !strcasecmp(tab, "ef")) t = TAB_EF;
        if (t < 0 && tab[0] >= '0' && tab[0] <= '9') t = atoi(tab);
        if (t >= 0 && t < (int)_tabs.numberOfTabViewItems) [_tabs selectTabViewItemAtIndex:t];
    }
    _tabs.delegate = self;

    NSStackView *root = O2VStack(@[toolbar, _tabs]);
    root.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    [toolbar.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    [_tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor constant:-16].active = YES;
    _window.contentView = root;
    [_window center];
}

/* ---- actions --------------------------------------------------------------------------- */

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
    [self appendPrompt:[O2StripControl(out) stringByAppendingString:@"\n"]];
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
    msg[0] = '\0';
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
    [self appendPrompt:[O2StripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = O2StripControl(msg);
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
    msg[0] = '\0';
    o2debug_save(_dbg, kFileKinds[kind], [[p URL] fileSystemRepresentation], msg, sizeof msg);
    [self appendPrompt:[O2StripControl(msg) stringByAppendingString:@"\n"]];
    _status.stringValue = O2StripControl(msg);
}

/* A register changes only while the machine is stopped: running, the next
 * instruction would overwrite it before anyone saw the edit land. The bank
 * registers are bytes of the internal RAM ($00-$07 and $18-$1F). */
- (void)applyRegister:(NSTextField *)sender
{
    long v;
    if (!o2debug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a register";
        return;
    }
    const NSInteger tag = sender.tag;
    if (parseNum(sender.stringValue, &v)) {
        if (tag >= 200)
            o2debug_write(_dbg, O2_SPACE_IRAM, (uint16_t)(24 + (tag - 200)), (uint8_t)v);
        else if (tag >= 100)
            o2debug_write(_dbg, O2_SPACE_IRAM, (uint16_t)(tag - 100), (uint8_t)v);
        else if (tag >= 0 && tag < NUM_SCALARS)
            o2debug_cpu_set(_dbg, kRegs[tag].reg, (int)v);
    }
    [self refreshAll];
}

- (void)flagClicked:(NSButton *)sender
{
    if (!o2debug_is_stopped(_dbg)) {
        [self refreshCpu];
        _status.stringValue = @"Stop the machine (F5) to change a flag";
        return;
    }
    if (sender.tag >= 0 && sender.tag < 5)
        o2debug_cpu_set(_dbg, kFlags[sender.tag].reg, sender.state == NSControlStateValueOn ? 1 : 0);
    [self refreshAll];
}

- (void)writeRam:(id)sender
{
    (void)sender;
    long a = 0, v = 0;
    const BOOL internal = _ramSpace.indexOfSelectedItem == 0;
    if (!parseNum(_ramAddr.stringValue, &a) || !parseNum(_ramVal.stringValue, &v) || a < 0 ||
        a > (internal ? 0x3f : 0x7f)) {
        _status.stringValue = internal ? @"Internal RAM: an address $00-$3F and a value"
                                       : @"External RAM: an address $00-$7F and a value";
        return;
    }
    o2debug_write(_dbg, internal ? O2_SPACE_IRAM : O2_SPACE_XDATA, (uint16_t)a, (uint8_t)v);
    [self refreshAll];
}

- (void)jumpTo:(id)sender
{
    (void)sender;
    const int addr = [self resolveAddr:_jump.stringValue];
    if (addr < 0) return;
    _followPc.state = NSControlStateValueOff;
    _disasmAddr = (uint16_t)(addr & 0xfff);
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
    const NSInteger sel = _bpType.indexOfSelectedItem;
    const int type = types[sel >= 0 && sel < 7 ? sel : 0];
    const BOOL port = (type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
    const BOOL exec = type == O2DEBUG_BP_EXEC;
    int space = (int)_bpSpace.indexOfSelectedItem;
    if (space < 0) space = 0;
    int start = 0, end = 0;
    if (!port) {
        NSString *range = [_bpRange.stringValue stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (range.length == 0) return;
        NSArray<NSString *> *ends = [range componentsSeparatedByString:@"-"];
        start = [self resolveAddr:ends[0]];
        end = ends.count > 1 ? [self resolveAddr:ends[1]] : start;
        if (exec) space = O2_SPACE_PROG;
        /* program space is 12 bits, external data 8, internal RAM 6 */
        const int limit = space == O2_SPACE_IRAM ? 0x3f : (space == O2_SPACE_XDATA ? 0xff : 0xfff);
        if (start < 0 || end < 0 || end < start || end > limit) {
            [self appendPrompt:[NSString stringWithFormat:@"breakpoint: bad address %@ (up to $%X here)\n", range, limit]];
            _status.stringValue = [NSString stringWithFormat:@"Bad address: %@", range];
            return;
        }
    }
    const int bpId = o2debug_breakpoint_add(_dbg, type, space, (uint16_t)start, (uint16_t)end,
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

/* ---- refreshers ------------------------------------------------------------------------ */

- (void)refreshStatus
{
    char reason[160];
    int addr;
    const BOOL stopped = o2debug_is_stopped(_dbg) != 0;
    reason[0] = '\0';
    o2debug_stop_reason(_dbg, reason, sizeof reason, &addr);
    _status.stringValue = stopped ? [NSString stringWithFormat:@"Stopped%s%s", reason[0] ? ": " : "", reason] : @"Running";
    setButtonLit(_runBtn, stopped ? @"Run (F5)" : @"Stop (F5)", stopped);
}

static void setField(NSTextField *f, NSString *text)
{
    if (f.currentEditor) return;       /* not while it is being typed into */
    if (![f.stringValue isEqualToString:text]) f.stringValue = text;
}

- (void)refreshCpu
{
    o2debug_cpu c;
    o2debug_cpu_get(_dbg, &c);
    const int vals[NUM_SCALARS] = { c.a, c.pc, c.psw, c.sp, c.mb ? 1 : 0, c.timer, c.prescaler, c.p1, c.p2, c.bus };
    for (int i = 0; i < NUM_SCALARS; i++) {
        switch (kRegs[i].digits) {
        case 3:  setField(_reg[i], [NSString stringWithFormat:@"%03X", vals[i] & 0xfff]); break;
        case 2:  setField(_reg[i], [NSString stringWithFormat:@"%02X", vals[i] & 0xff]); break;
        default: setField(_reg[i], [NSString stringWithFormat:@"%X", vals[i] & 0xf]); break;
        }
    }
    for (int i = 0; i < 8; i++) {
        setField(_bank0[i], [NSString stringWithFormat:@"%02X", c.r[0][i]]);
        setField(_bank1[i], [NSString stringWithFormat:@"%02X", c.r[1][i]]);
    }
    const int flags[5] = { c.cy, c.ac, c.f0, c.f1, c.bs };
    for (int i = 0; i < 5; i++) _flag[i].state = flags[i] ? NSControlStateValueOn : NSControlStateValueOff;
    const int stats[7] = { c.tf, c.tcnti_en, c.ext_int_en, c.in_interrupt, c.t0, c.t1, c.int_line };
    for (int i = 0; i < 7; i++) _stat[i].state = stats[i] ? NSControlStateValueOn : NSControlStateValueOff;

    static const char *const modes[3] = { "stopped", "timer", "event counter (T1)" };
    _cpuState.stringValue = [NSString stringWithFormat:
        @"PC $%03X (MB%d)   PSW $%02X: C %d AC %d F0 %d F1 %d bank %d, SP %d   timer %s   interrupts: %s%s",
        c.pc & 0xfff, c.mb ? 1 : 0, c.psw & 0xff, c.cy, c.ac, c.f0, c.f1, c.bs, c.sp,
        modes[c.timer_mode >= 0 && c.timer_mode < 3 ? c.timer_mode : 0],
        c.irq_pending ? "external pending " : "none pending", c.timer_irq_pending ? ", timer pending" : ""];
    _beam.stringValue = [NSString stringWithFormat:
        @"beam line %d   dot %d   frame %u   cycles %llu   instructions %llu",
        c.vpos, c.hpos, c.frame, (unsigned long long)c.cycles, (unsigned long long)c.instructions];

    uint8_t iram[64], xram[128];
    memset(iram, 0, sizeof iram);
    memset(xram, 0, sizeof xram);
    o2debug_read(_dbg, O2_SPACE_IRAM, 0, iram, (int)sizeof iram);
    o2debug_read(_dbg, O2_SPACE_XDATA, 0, xram, (int)sizeof xram);
    O2SetTextKeepingPlace(_iram, hexDump(iram, (int)sizeof iram, 0, @"$%02X: "));
    O2SetTextKeepingPlace(_xram, hexDump(xram, (int)sizeof xram, 0, @"$%02X: "));
}

- (void)refreshDisasm
{
    static o2debug_line lines[DISASM_WINDOW];
    int pcLine = -1;
    if (_followPc.state == NSControlStateValueOn) {
        o2debug_cpu c;
        o2debug_cpu_get(_dbg, &c);
        /* the PC a third of the way down */
        _disasmAddr = (uint16_t)(o2debug_row_address(_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3)) & 0xfff);
    }
    const int n = o2debug_disassemble(_dbg, _disasmAddr, lines, DISASM_WINDOW, &pcLine);
    _lineCount = n;
    NSMutableString *text = [NSMutableString string];
    NSUInteger pcStart = 0, pcLen = 0;
    BOOL havePc = NO;
    for (int i = 0; i < n; i++) {
        _lineAddr[i] = lines[i].address;
        NSString *line = [NSString stringWithFormat:@"%c%c %03X  %-8s  %-14s %-22s%s%s\n",
                          lines[i].has_breakpoint ? '*' : ' ', lines[i].is_pc ? '>' : ' ',
                          lines[i].address, lines[i].bytes, lines[i].label, lines[i].disasm,
                          lines[i].comment[0] ? " ; " : "", lines[i].comment];
        if (lines[i].is_pc) { havePc = YES; pcStart = text.length; pcLen = line.length - 1; }
        [text appendString:line];
    }
    if (n == 0) [text appendString:@"(no disassembly: the debugger is not attached)\n"];
    _disasm.string = text;
    if (havePc) {
        /* the current line in the accent, its text white on the red */
        [_disasm.textStorage addAttributes:@{ NSBackgroundColorAttributeName: O2AccentColor(),
                                              NSForegroundColorAttributeName: NSColor.whiteColor }
                                     range:NSMakeRange(pcStart, pcLen)];
    }
}

- (void)refreshEf
{
    if (!o2debug_ef_present(_dbg)) {
        _efNote.stringValue = @"Videopac+ G7400 only: the EF9340/41 text chip is not in this machine.";
        O2SetTextKeepingPlace(_efRegs, @"");
        O2SetTextKeepingPlace(_efPage, @"");
        [_efPic setImage:NULL];
        return;
    }
    static ef9340_snapshot_t e;
    o2debug_ef_get(_dbg, &e);
    _efNote.stringValue = [NSString stringWithFormat:@"EF9340/41 present. Font %s, %llu text lines drawn.",
                           e.font_present ? "imported" : "NOT imported (text is blank)",
                           (unsigned long long)e.lines_drawn];

    NSMutableString *r = [NSMutableString string];
    [r appendFormat:@"EF9341  TA $%02X  TB $%02X  %s\n", e.ta, e.tb, e.busy ? "busy" : "ready"];
    [r appendFormat:@"EF9340  X %d  Y %d  Y0 $%02X  R $%02X  M $%02X\n", e.x, e.y, e.y0, e.r, e.m];
    [r appendFormat:@"R   display %s   box mode %s   conceal %s   service row %s\n",
        e.display_on ? "on" : "off", e.box_mode ? "on" : "off", e.conceal_enabled ? "on" : "off",
        e.service_row ? "shown" : "hidden"];
    [r appendFormat:@"    cursor %s%s   %s   blink %s (prescaler %d, %s)\n",
        e.cursor_on ? "on" : "off", e.cursor_visible ? " (visible)" : "",
        e.lines625 ? "250 lines, 24-frame blink" : "210 lines, 32-frame blink",
        e.blink_enabled ? "enabled" : "off", e.blink_prescaler, e.blink ? "hidden now" : "shown now"];
    [r appendFormat:@"Y0  first row %d   zoom %s\n", e.first_row, e.zoom ? "on (double-height rows)" : "off"];
    [r appendFormat:@"M   mode $%02X %s   slice %d\n", e.mode, e.mode_name ? e.mode_name : "?", e.slice];
    O2SetTextKeepingPlace(_efRegs, r);

    NSMutableString *pg = [NSMutableString string];
    for (int row = 0; row < EF9340_ROWS; row++) {
        char line[EF9340_COLUMNS + 1];
        for (int col = 0; col < EF9340_COLUMNS; col++) {
            const ef9340_cell_t *cell = &e.page[row][col];
            char ch = ' ';
            switch (cell->kind) {
            case EF9340_CELL_ALNUM:
                ch = (cell->b & 0x7f) >= 0x20 && (cell->b & 0x7f) < 0x7f ? (char)(cell->b & 0x7f) : '.';
                break;
            case EF9340_CELL_GRAPHIC: ch = '#'; break;
            case EF9340_CELL_DELIMITER: ch = ' '; break;
            default: ch = '@'; break;
            }
            line[col] = ch;
        }
        line[EF9340_COLUMNS] = '\0';
        [pg appendFormat:@"%2d  %s\n", row, line];
    }
    O2SetTextKeepingPlace(_efPage, pg);

    int w = 0, h = 0;
    if (o2debug_ef_view(_dbg, _px, (size_t)O2DEBUG_VDC_VIEW_MAX_PIXELS * 4, &w, &h) && w > 0 && h > 0)
        [_efPic setImage:O2NewImage(_px, w, h)];
}

- (void)refreshIo
{
    o2debug_io io;
    o2debug_io_get(_dbg, &io);
    [_ioDiag setIo:&io];

    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"%s\n", o2_machine_name(io.machine)];
    [s appendFormat:@"BIOS        %s\n", io.stub_running
        ? (io.missing & O2_MISSING_BIOS ? "the stand-in, parked: a game needs the BIOS"
                                        : "the stand-in (runs CONFIG and FujiNet apps)")
        : "imported"];
    [s appendFormat:@"Characters  %s\n\n", io.have_font ? "imported" : "NOT imported: text is blank"];

    [s appendFormat:@"P1 = $%02X   bank P10/P11 = %d   keyboard %s (P12)   VDC %s (P13)   RAM/cart %s (P14)\n",
        io.p1, io.cart_bank, io.kbd_enable ? "on" : "off", io.vdc_select ? "selected" : "off",
        io.ram_select ? "selected" : "off"];
    [s appendFormat:@"             EF9341 %s (P15)   copy mode %s (P16)   LUM %s (P17)\n",
        io.ef_select ? "selected" : "off", io.copy_mode ? "on: external writes and VDC reads gated off" : "off",
        io.lum ? "on: every pixel brightened" : "off"];
    [s appendFormat:@"P2 = $%02X   keyboard row P20-P22 = %d   T0 = %d\n", io.p2, io.kbd_row, io.t0];
    [s appendFormat:@"Sticks      1: $%02X  2: $%02X (bit 0 up, 1 right, 2 down, 3 left, 4 button)   RESET %s\n\n",
        io.sticks[0], io.sticks[1], io.reset_held ? "held" : "up"];

    i8244_snapshot v;
    o2debug_vdc_get(_dbg, &v);
    [s appendFormat:@"8244 sound   %s   %s clock   noise %s   volume %d/15   %s\n",
        v.snd_enable ? "on" : "off", v.snd_fast ? "fast" : "slow", v.snd_noise ? "on" : "off", v.snd_volume,
        v.snd_noise ? "no pitch" : (v.freq_hz > 0 ? [[NSString stringWithFormat:@"%.1f Hz", v.freq_hz] UTF8String]
                                                  : "steady level")];
    [s appendFormat:@"Audio        %llu samples made\n\n", (unsigned long long)io.audio_samples];

    if (io.voice_present) {
        const sp0256_debug_t *p = &io.voice;
        [s appendString:@"The Voice (SP0256)\n"];
        [s appendFormat:@"  %s   LRQ %d   SBY %d   %s\n", p->halted ? "halted" : "running", p->lrq, p->sby,
            p->silent ? "silent" : (p->noise ? "noise source" : "voiced")];
        [s appendFormat:@"  allophone %d (pending %d, last %d)   FIFO %u decles%s\n", p->allophone, p->pending,
            p->last_allophone, p->fifo_count, p->fifo_sel ? "   fetching from the FIFO" : ""];
        [s appendFormat:@"  ROM $%05X bit %u   mode %u page $%05X stack $%05X\n", p->rom_addr, p->rom_bit,
            p->mode, p->page, p->stack];
        [s appendFormat:@"  repeat %d  count %d  period %u  amplitude %d  interpolation %d\n", p->rpt, p->cnt,
            p->period, p->amp, p->interp];
        [s appendFormat:@"  %llu samples at %u Hz (clock %u Hz), %llu allophones dropped, last sample %d\n",
            (unsigned long long)p->samples_made, p->rate_hz, p->clock_hz, (unsigned long long)p->alds_dropped,
            p->last_sample];
        [s appendString:@"  registers"];
        for (int i = 0; i < 16; i++) [s appendFormat:@" %02X", p->r[i]];
        [s appendString:@"\n"];
    } else {
        [s appendString:@"The Voice   not in front of the cartridge\n"];
    }
    O2SetTextKeepingPlace(_sound, s);
}

- (void)refreshBps
{
    static o2debug_breakpoint bps[MAX_BPS];
    const int n = o2debug_breakpoint_list(_dbg, bps, MAX_BPS);
    for (NSView *v in [_bpList.arrangedSubviews copy]) [_bpList removeView:v];
    if (n == 0) [_bpList addArrangedSubview:O2Label(@"No breakpoints")];
    for (int i = 0; i < n; i++) {
        static const struct { int bit; const char *name; } kinds[5] = {
            { O2DEBUG_BP_EXEC, "exec" }, { O2DEBUG_BP_READ, "read" }, { O2DEBUG_BP_WRITE, "write" },
            { O2DEBUG_BP_IN, "in" }, { O2DEBUG_BP_OUT, "out" },
        };
        static const char *const spaces[3] = { "program", "xdata", "iram" };
        static const char *const ports[3] = { "BUS", "P1", "P2" };
        NSMutableString *what = [NSMutableString stringWithFormat:@"#%-3d ", bps[i].id];
        BOOL first = YES;
        for (int k = 0; k < 5; k++)
            if (bps[i].type & kinds[k].bit) {
                [what appendFormat:@"%s%s", first ? "" : "/", kinds[k].name];
                first = NO;
            }
        const BOOL port = (bps[i].type & (O2DEBUG_BP_IN | O2DEBUG_BP_OUT)) != 0;
        const BOOL exec = (bps[i].type & O2DEBUG_BP_EXEC) != 0;
        const int sp = bps[i].space >= 0 && bps[i].space < 3 ? bps[i].space : 0;
        if (port) {
            [what appendFormat:@"  port %s", ports[sp]];
        } else {
            [what appendFormat:@"  %s $%03X", exec ? "program" : spaces[sp], bps[i].start];
            if (bps[i].end != bps[i].start) [what appendFormat:@"-$%03X", bps[i].end];
            char label[64];
            if ((exec || sp == O2_SPACE_PROG) &&
                o2debug_address_label(_dbg, bps[i].start, label, sizeof label) > 0)
                [what appendFormat:@"  %s", label];
        }
        if (bps[i].condition[0]) [what appendFormat:@"  if %s", bps[i].condition];
        [what appendFormat:@"   hits %u", bps[i].hits];
        NSButton *box = [NSButton checkboxWithTitle:what target:self action:@selector(bpEnabled:)];
        box.state = bps[i].enabled ? NSControlStateValueOn : NSControlStateValueOff;
        box.tag = bps[i].id;
        box.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        NSButton *remove = O2Button(@"Remove", self, @selector(bpRemove:));
        remove.tag = bps[i].id;
        [_bpList addArrangedSubview:O2HStack(@[box, remove])];
    }
}

- (void)refreshCart
{
    o2debug_cart c;
    char info[256];
    info[0] = '\0';
    o2debug_cart_get(_dbg, &c);
    o2debug_cart_info(_dbg, info, sizeof info);

    NSMutableString *s = [NSMutableString stringWithFormat:@"%@\n\n", O2StripControl(info)];
    if (!c.present) {
        [s appendString:@"(no cartridge in the slot)\n"];
        O2SetTextKeepingPlace(_cart, s);
        O2SetTextKeepingPlace(_mailbox, @"");
        return;
    }
    [s appendFormat:@"Type        %s%s\n", c.type_name ? c.type_name : "?",
        c.voice_in_front ? "   with The Voice in front" : ""];
    if (c.type == O2_CART_FUJINET) {
        [s appendFormat:@"Link        %s%s\n", c.link_up ? "up" : "down", c.link_off ? " (switched off)" : ""];
        [s appendFormat:@"Mailbox     %s   %s%s\n", c.mailbox ? "open (the app claims it)" : "closed",
            c.app_live ? "an app is live" : "CONFIG is live", c.booted_game ? "   a game has booted" : ""];
        [s appendFormat:@"Live image  %u bytes   CRC-32 %08X   %d bank%s, showing %d\n", c.live_size, c.live_crc,
            c.nbanks, c.nbanks == 1 ? "" : "s", c.fbank];
        if (c.staged_size)
            [s appendFormat:@"Staged      %s%s   %u bytes   CRC-32 %08X%s\n", c.staged_app ? "a FujiNet app" : "a game",
                c.staged_mailbox ? " (claims the mailbox)" : "", c.staged_size, c.staged_crc,
                c.armed ? "   armed" : ""];
        else
            [s appendString:@"Staged      nothing\n"];
        [s appendFormat:@"Swaps       %d\n", c.swaps];
        [s appendFormat:@"Boot        state %d   %d%%   error %d\n", c.boot_state, c.boot_pct, c.boot_err];
        [s appendFormat:@"Mailbox     ACKSEQ $%02X   STATUS $%02X   last error %d   reply cmd $%02X   RXLEN %d\n",
            c.ackseq, c.status, c.last_error, c.reply_cmd, c.rxlen];
        [s appendFormat:@"Queue       %u events waiting\n", c.queue_depth];
    } else {
        [s appendFormat:@"Image       %u bytes\n", c.rom_size];
        [s appendFormat:@"Bank        %d   control $%02X\n", c.bank, c.control];
    }
    O2SetTextKeepingPlace(_cart, s);
    O2SetTextKeepingPlace(_mailbox, hexDump(c.mailbox_page, (int)sizeof c.mailbox_page, 0xF20, @"$%03X: "));
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

    status[0] = '\0';
    if (o2session_fujinet_external(_session)) {
        o2session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"External FujiNet at %s. Cartridge: %@.",
                                 o2session_boip_endpoint(_session), O2StripControl(status)];
    } else if (!o2session_fujinet_running(_session)) {
        _fujiNote.stringValue = @"FujiNet is off (Settings ▸ FujiNet).";
    } else {
        o2session_cart_status(_session, status, (int)sizeof status);
        _fujiNote.stringValue = [NSString stringWithFormat:@"FujiNet is on. Cartridge: %@.", O2StripControl(status)];
    }
    state[0] = '\0';
    o2debug_fujinet_state(_dbg, state, (int)sizeof state);
    O2SetTextKeepingPlace(_fujiState, O2StripControl(state));

    if (!force && n == _fujiCount && (n == 0 || p[n - 1].serial == _fujiLast)) return;
    _fujiCount = n;
    _fujiLast = n > 0 ? p[n - 1].serial : 0;
    const BOOL follow = force || O2TextAtEnd(_fujiText);
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
    O2SetLogText(_fujiText, s, follow);
}

/* Only the tab that shows is brought up to date. */
- (void)refreshTab:(NSInteger)tab
{
    switch (tab) {
    case TAB_CPU:         [self refreshCpu]; break;
    case TAB_DISASM:      [self refreshDisasm]; break;
    case TAB_VIDEO:       [_video refresh]; break;
    case TAB_EF:          [self refreshEf]; break;
    case TAB_IO:          [self refreshIo]; break;
    case TAB_BREAKPOINTS: [self refreshBps]; break;
    case TAB_CART:        [self refreshCart]; break;
    case TAB_FUJINET:     [self refreshFujinet:NO]; break;
    default: break;
    }
}

- (void)refreshAll
{
    [self refreshStatus];
    [self refreshTab:[self selectedTab]];
}
@end

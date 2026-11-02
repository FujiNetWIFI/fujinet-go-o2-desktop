/*
 * O2VideoTab -- see the header.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "VideoTab.h"

#import "DebugViews.h"
#import "../KeyForward.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The views want at most one frame (360 x 243) at 1x, and the sheets up to 4x
 * of their own smaller size: room for four frames covers every one. */
#define PX_CAP ((size_t)O2DEBUG_VDC_VIEW_MAX_PIXELS * 4)
#define OBJ_CAP (128 * 128)

#define OBJ_ROW_H     34.0
#define OBJ_HEAD_H    24.0
#define OBJ_THUMB     28.0

#define SECTION_OBJECTS  0
#define SECTION_GRID     1
#define SECTION_REGS     2
#define SECTION_CONTROL  3
#define SECTION_SOUND    4
#define SECTION_SHEETS   5

/* The layer and overlay checkboxes, with the bits they set. */
static const struct { NSString *__unsafe_unretained title; unsigned bit; } kLayers[7] = {
    { @"Background", I8244_L_BG }, { @"Grid", I8244_L_GRID }, { @"Vertical grid", I8244_L_VGRID },
    { @"Dots", I8244_L_DOTS }, { @"Sprites", I8244_L_SPRITES }, { @"Singles", I8244_L_SINGLES },
    { @"Quads", I8244_L_QUADS },
};
static const struct { NSString *__unsafe_unretained title; unsigned bit; } kOverlays[6] = {
    { @"Bounding boxes + indices", I8244_F_BOXES }, { @"Collision highlight", I8244_F_COLLISION },
    { @"Overlap highlight", I8244_F_OVERLAP }, { @"Show hidden", I8244_F_SHOW_HIDDEN },
    { @"Ignore enables", I8244_F_IGNORE_ENABLE }, { @"Beam cross-hair", I8244_F_BEAM },
};

static NSColor *colorFromXrgb(uint32_t rgb)
{
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

/* $hex, 0xhex or bare hex. */
static BOOL parseHex(NSString *text, long *out)
{
    NSString *t = [text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if ([t hasPrefix:@"$"]) t = [t substringFromIndex:1];
    else if ([t.lowercaseString hasPrefix:@"0x"]) t = [t substringFromIndex:2];
    if (t.length == 0) return NO;
    char *end;
    const long v = strtol(t.UTF8String, &end, 16);
    if (*end) return NO;
    *out = v;
    return YES;
}

/* ---- the objects list ------------------------------------------------------------- */

/* One line of the objects list: a heading, or an object with its bitmap. */
@interface O2ObjectRow : NSObject
@property (nonatomic, copy) NSString *heading;     /* set: a section heading */
@property (nonatomic, copy) NSString *line1;
@property (nonatomic, copy) NSString *line2;
@property (nonatomic, strong) NSImage *thumb;
@end

@implementation O2ObjectRow
@end

/* The rows, drawn from the top; the view's height follows its rows, so the
 * scroll view around it scrolls them. */
@interface O2ObjectsView : NSView
@property (nonatomic, copy) NSArray<O2ObjectRow *> *rows;
@end

@implementation O2ObjectsView
- (BOOL)isFlipped { return YES; }

- (void)setRows:(NSArray<O2ObjectRow *> *)rows
{
    _rows = [rows copy];
    CGFloat h = 4;
    for (O2ObjectRow *r in _rows) h += r.heading ? OBJ_HEAD_H : OBJ_ROW_H;
    [self setFrameSize:NSMakeSize(self.frame.size.width, h)];
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSDictionary *head = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                            NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *one = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.labelColor };
    NSDictionary *two = @{ NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
    CGFloat y = 4;
    for (O2ObjectRow *r in _rows) {
        if (r.heading) {
            [r.heading drawAtPoint:NSMakePoint(6, y + 4) withAttributes:head];
            y += OBJ_HEAD_H;
            continue;
        }
        const NSRect box = NSMakeRect(6, y + 3, OBJ_THUMB, OBJ_THUMB);
        [colorFromXrgb(0x101010) setFill];
        NSRectFill(box);
        if (r.thumb) {
            [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
            const NSSize sz = r.thumb.size;
            const double f = fmin(1.0, fmin(OBJ_THUMB / sz.width, OBJ_THUMB / sz.height));
            const NSRect dst = NSMakeRect(box.origin.x + floor((OBJ_THUMB - sz.width * f) / 2),
                                          box.origin.y + floor((OBJ_THUMB - sz.height * f) / 2),
                                          sz.width * f, sz.height * f);
            [r.thumb drawInRect:dst fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                       fraction:1.0 respectFlipped:YES hints:nil];
        }
        [r.line1 drawAtPoint:NSMakePoint(42, y + 3) withAttributes:one];
        [r.line2 drawAtPoint:NSMakePoint(42, y + 18) withAttributes:two];
        y += OBJ_ROW_H;
    }
}
@end

/* ---- the grid diagram ------------------------------------------------------------- */

#define GRID_CW 36.0
#define GRID_CH 30.0
#define GRID_PAD 14.0

/* The grid as the chip holds it: 9 columns of 9 horizontal line segments and
 * 10 columns of 8 vertical ones, in the grid's colour. */
@interface O2GridView : NSView
- (void)setSnapshot:(const i8244_snapshot *)s colour:(uint32_t)rgb;
@end

@implementation O2GridView {
    uint16_t _h[9];
    uint8_t _v[10];
    int _gridOn, _dotsOn, _fill;
    uint32_t _rgb;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize { return NSMakeSize(9 * GRID_CW + 2 * GRID_PAD, 8 * GRID_CH + 2 * GRID_PAD); }

- (void)setSnapshot:(const i8244_snapshot *)s colour:(uint32_t)rgb
{
    if (!memcmp(_h, s->hgrid, sizeof _h) && !memcmp(_v, s->vgrid, sizeof _v) &&
        _gridOn == s->grid_en && _dotsOn == s->dots_en && _fill == s->fill && _rgb == rgb)
        return;
    memcpy(_h, s->hgrid, sizeof _h);
    memcpy(_v, s->vgrid, sizeof _v);
    _gridOn = s->grid_en;
    _dotsOn = s->dots_en;
    _fill = s->fill;
    _rgb = rgb;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    [colorFromXrgb(0x101010) setFill];
    NSRectFill([self bounds]);
    NSColor *ink = colorFromXrgb(_rgb);
    const CGFloat ox = GRID_PAD, oy = GRID_PAD;

    /* the line segments that are set */
    [ink setFill];
    for (int c = 0; c < 9; c++)
        for (int y = 0; y < 9; y++)
            if ((_h[c] >> y) & 1)
                NSRectFill(NSMakeRect(ox + c * GRID_CW, oy + y * GRID_CH - 1.5, GRID_CW, 3));
    const CGFloat vw = _fill ? 6 : 3;
    for (int i = 0; i < 10; i++)
        for (int y = 0; y < 8; y++)
            if ((_v[i] >> y) & 1)
                NSRectFill(NSMakeRect(ox + i * GRID_CW - vw / 2, oy + y * GRID_CH, vw, GRID_CH));

    /* the intersections: dots when the chip draws them, faint marks otherwise */
    [(_dotsOn ? ink : colorFromXrgb(0x505050)) setFill];
    const CGFloat d = _dotsOn ? 5 : 2;
    for (int i = 0; i < 10; i++)
        for (int y = 0; y < 9; y++)
            NSRectFill(NSMakeRect(ox + i * GRID_CW - d / 2, oy + y * GRID_CH - d / 2, d, d));

    if (!_gridOn) {
        NSDictionary *a = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:12],
                             NSForegroundColorAttributeName: colorFromXrgb(0xa0a0a0) };
        [@"grid off (control bit 3): nothing of this is drawn" drawAtPoint:NSMakePoint(ox + 4, oy + 4)
                                                             withAttributes:a];
    }
}
@end

/* ---- a register of bits -------------------------------------------------------------- */

#define BIT_W 24.0
#define BIT_H 28.0

/* The sound shift register: its bits, the most significant at the left. */
@interface O2BitsView : NSView
- (void)setValue:(uint32_t)value bits:(int)bits;
@end

@implementation O2BitsView {
    uint32_t _value;
    int _bits;
}
- (BOOL)isFlipped { return YES; }
- (NSSize)intrinsicContentSize { return NSMakeSize(24 * BIT_W + 2, BIT_H + 16); }

- (void)setValue:(uint32_t)value bits:(int)bits
{
    if (value == _value && bits == _bits) return;
    _value = value;
    _bits = bits;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    NSFont *font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    for (int i = 0; i < _bits; i++) {
        const int bit = _bits - 1 - i;
        const BOOL on = (_value >> bit) & 1u;
        const NSRect r = NSMakeRect(1 + i * BIT_W, 0, BIT_W - 2, BIT_H);
        [(on ? O2AccentColor() : NSColor.controlBackgroundColor) setFill];
        NSRectFill(r);
        [NSColor.separatorColor setStroke];
        [NSBezierPath strokeRect:NSInsetRect(r, 0.5, 0.5)];
        NSDictionary *a = @{ NSFontAttributeName: font,
                             NSForegroundColorAttributeName: on ? NSColor.whiteColor : NSColor.labelColor };
        NSString *digit = on ? @"1" : @"0";
        const NSSize sz = [digit sizeWithAttributes:a];
        [digit drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2) withAttributes:a];
        NSString *num = [NSString stringWithFormat:@"%d", bit];
        NSDictionary *n = @{ NSFontAttributeName: [NSFont systemFontOfSize:8],
                             NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
        const NSSize ns = [num sizeWithAttributes:n];
        [num drawAtPoint:NSMakePoint(NSMidX(r) - ns.width / 2, BIT_H + 2) withAttributes:n];
    }
}
@end

/* ---- the tab ----------------------------------------------------------------------------- */

@implementation O2VideoTab {
    o2debug *_dbg;
    i8244_snapshot _snap;
    uint32_t *_px;                      /* PX_CAP pixels: every view's scratch */
    uint32_t *_objPx;                   /* OBJ_CAP pixels: one object's bitmap */
    uint32_t _pal[16];                  /* 8 colours dim, then 8 bright */
    BOOL _palLoaded;
    CFAbsoluteTime _regsAt;

    NSPopUpButton *_viewSel;
    NSButton *_layer[7];
    NSButton *_overlay[6];
    O2PictureView *_pic;
    NSTextField *_hover, *_beam;
    NSTabView *_sections;

    O2ObjectsView *_objects;
    O2GridView *_grid;
    NSTextField *_gridNote;
    O2LineTextView *_regs;
    NSTextField *_regAddr, *_regVal, *_regNote;
    NSTextView *_control;
    O2BitsView *_bits;
    NSTextView *_sound;
    O2PictureView *_fontPic, *_shapePic, *_palPic;
}

- (instancetype)initWithDebugger:(o2debug *)dbg
{
    self = [super init];
    if (!self) return nil;
    _dbg = dbg;
    _px = calloc(PX_CAP, sizeof *_px);
    _objPx = calloc(OBJ_CAP, sizeof *_objPx);
    _view = [self buildView];
    return self;
}

- (void)dealloc
{
    free(_px);
    free(_objPx);
}

/* ---- construction ------------------------------------------------------------------------ */

- (NSView *)buildLeft
{
    _viewSel = [[NSPopUpButton alloc] init];
    [_viewSel addItemsWithTitles:@[@"Frame (what the display shows)",
                                   @"Rendered from the registers",
                                   @"Frame in progress, to the beam"]];
    _viewSel.target = self;
    _viewSel.action = @selector(viewChanged:);
    NSStackView *viewRow = O2HStack(@[O2Label(@"View"), _viewSel]);

    NSMutableArray<NSView *> *layerBoxes = [NSMutableArray array];
    for (int i = 0; i < 7; i++) {
        _layer[i] = [NSButton checkboxWithTitle:kLayers[i].title target:self action:@selector(optionChanged:)];
        _layer[i].state = NSControlStateValueOn;
        _layer[i].controlSize = NSControlSizeSmall;
        [layerBoxes addObject:_layer[i]];
    }
    NSMutableArray<NSView *> *overlayBoxes = [NSMutableArray array];
    for (int i = 0; i < 6; i++) {
        _overlay[i] = [NSButton checkboxWithTitle:kOverlays[i].title target:self action:@selector(optionChanged:)];
        _overlay[i].controlSize = NSControlSizeSmall;
        [overlayBoxes addObject:_overlay[i]];
    }
    NSStackView *layerRow1 = O2HStack([layerBoxes subarrayWithRange:NSMakeRange(0, 4)]);
    NSStackView *layerRow2 = O2HStack([layerBoxes subarrayWithRange:NSMakeRange(4, 3)]);
    NSStackView *ovRow1 = O2HStack([overlayBoxes subarrayWithRange:NSMakeRange(0, 3)]);
    NSStackView *ovRow2 = O2HStack([overlayBoxes subarrayWithRange:NSMakeRange(3, 3)]);
    layerRow1.spacing = 10;
    layerRow2.spacing = 10;
    ovRow1.spacing = 10;
    ovRow2.spacing = 10;
    NSTextField *layersTitle = O2Note(@"Layers (Rendered view)");
    NSTextField *overlaysTitle = O2Note(@"Overlays");

    _pic = [[O2PictureView alloc] initWithFrame:NSMakeRect(0, 0, 720, 486)];
    [_pic.widthAnchor constraintGreaterThanOrEqualToConstant:360].active = YES;
    [_pic.heightAnchor constraintGreaterThanOrEqualToConstant:243].active = YES;
    [_pic setContentHuggingPriority:(NSLayoutPriority)1 forOrientation:NSLayoutConstraintOrientationVertical];
    __weak O2VideoTab *weakSelf = self;
    _pic.onHover = ^(int x, int y) { [weakSelf hoverX:x y:y]; };

    _hover = O2Note(@"Point at the picture: the object under the pointer is named here and outlined");
    _hover.lineBreakMode = NSLineBreakByTruncatingTail;
    _beam = O2Note(@"");
    _beam.lineBreakMode = NSLineBreakByTruncatingTail;

    NSStackView *left = O2VStack(@[viewRow, layersTitle, layerRow1, layerRow2, overlaysTitle, ovRow1, ovRow2,
                                   _pic, _hover, _beam]);
    left.distribution = NSStackViewDistributionFill;
    O2Fill(left, _pic);
    O2Fill(left, _hover);
    O2Fill(left, _beam);
    return left;
}

- (NSView *)scrollFor:(NSView *)doc
{
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    scroll.documentView = doc;
    return scroll;
}

- (NSView *)buildObjects
{
    _objects = [[O2ObjectsView alloc] initWithFrame:NSMakeRect(0, 0, 480, 100)];
    _objects.autoresizingMask = NSViewWidthSizable;
    return [self scrollFor:_objects];
}

- (NSView *)buildGrid
{
    _grid = [[O2GridView alloc] initWithFrame:NSZeroRect];
    O2FixSize(_grid);
    _gridNote = O2Note(@"");
    _gridNote.maximumNumberOfLines = 4;
    NSStackView *v = O2VStack(@[O2Title(@"The grid"), _grid, _gridNote]);
    return v;
}

- (NSView *)buildRegisters
{
    _regs = [[O2LineTextView alloc] initWithFrame:NSMakeRect(0, 0, 480, 300)];
    _regs.editable = NO;
    _regs.richText = NO;
    _regs.selectable = NO;
    _regs.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _regs.horizontallyResizable = YES;
    _regs.textContainer.widthTracksTextView = NO;
    _regs.textContainer.containerSize = NSMakeSize(FLT_MAX, FLT_MAX);
    _regs.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;
    scroll.documentView = _regs;
    __weak O2VideoTab *weakSelf = self;
    _regs.onClickLine = ^(int line) { [weakSelf pickRegister:line]; };

    _regAddr = O2Field(@"$A0", 70, self, @selector(writeRegister:));
    _regVal = O2Field(@"$00", 60, self, @selector(writeRegister:));
    _regNote = O2Note(@"Click a line, edit the value, Return writes it (while stopped)");
    _regNote.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView *edit = O2HStack(@[O2Label(@"Register"), _regAddr, O2Label(@"value"), _regVal,
                                   O2Button(@"Write", self, @selector(writeRegister:))]);
    NSStackView *v = O2VStack(@[scroll, edit, _regNote]);
    v.distribution = NSStackViewDistributionFill;
    O2Fill(v, scroll);
    return v;
}

- (NSView *)buildControl
{
    NSScrollView *scroll;
    _control = O2MonoView(&scroll, NO);
    return scroll;
}

- (NSView *)buildSound
{
    NSScrollView *scroll;
    _sound = O2MonoView(&scroll, NO);
    _bits = [[O2BitsView alloc] initWithFrame:NSZeroRect];
    O2FixSize(_bits);
    NSStackView *v = O2VStack(@[O2Title(@"The 24-bit shift register ($A7-$A9)"), _bits, scroll]);
    v.distribution = NSStackViewDistributionFill;
    O2Fill(v, scroll);
    return v;
}

- (O2PictureView *)sheetPicture:(CGFloat)height
{
    O2PictureView *p = [[O2PictureView alloc] initWithFrame:NSMakeRect(0, 0, 480, height)];
    [p.heightAnchor constraintEqualToConstant:height].active = YES;
    return p;
}

- (NSView *)buildSheets
{
    _fontPic = [self sheetPicture:150];
    _shapePic = [self sheetPicture:90];
    _palPic = [self sheetPicture:80];
    NSStackView *v = O2VStack(@[O2Title(@"Font (64 glyphs: the lower 256 bytes, then the upper)"), _fontPic,
                                O2Title(@"Sprite shape tables"), _shapePic,
                                O2Title(@"Palette (8 colours dim above 8 bright)"), _palPic]);
    O2Fill(v, _fontPic);
    O2Fill(v, _shapePic);
    O2Fill(v, _palPic);
    return v;
}

- (NSView *)buildView
{
    NSView *left = [self buildLeft];

    _sections = [[NSTabView alloc] init];
    NSArray *pages = @[ @[@"Objects", [self buildObjects]], @[@"Grid", [self buildGrid]],
                        @[@"Registers", [self buildRegisters]], @[@"Control/Status", [self buildControl]],
                        @[@"Sound", [self buildSound]], @[@"Sheets", [self buildSheets]] ];
    for (NSArray *page in pages) {
        NSTabViewItem *item = [[NSTabViewItem alloc] initWithIdentifier:page[0]];
        item.label = page[0];
        NSView *content = page[1];
        NSView *holder = [[NSView alloc] init];
        [holder addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [content.leadingAnchor constraintEqualToAnchor:holder.leadingAnchor constant:4].active = YES;
        [content.trailingAnchor constraintEqualToAnchor:holder.trailingAnchor constant:-4].active = YES;
        [content.topAnchor constraintEqualToAnchor:holder.topAnchor constant:4].active = YES;
        [content.bottomAnchor constraintEqualToAnchor:holder.bottomAnchor constant:-4].active = YES;
        item.view = holder;
        [_sections addTabViewItem:item];
    }
    _sections.delegate = self;

    NSStackView *h = O2HStack(@[left, _sections]);
    h.alignment = NSLayoutAttributeTop;
    h.spacing = 12;
    [left.widthAnchor constraintGreaterThanOrEqualToConstant:380].active = YES;
    [_sections.widthAnchor constraintEqualToConstant:520].active = YES;
    [left.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    [_sections.heightAnchor constraintEqualToAnchor:h.heightAnchor].active = YES;
    return h;
}

/* ---- the controls --------------------------------------------------------------------------- */

- (unsigned)layers
{
    unsigned m = 0;
    for (int i = 0; i < 7; i++)
        if (_layer[i].state == NSControlStateValueOn) m |= kLayers[i].bit;
    return m;
}

- (unsigned)overlays
{
    unsigned m = 0;
    for (int i = 0; i < 6; i++)
        if (_overlay[i].state == NSControlStateValueOn) m |= kOverlays[i].bit;
    return m;
}

- (void)viewChanged:(id)sender
{
    (void)sender;
    [self refresh];
}

/* A layer or an overlay only means something in a re-render: the real frame
 * has neither, so touching one switches to it. */
- (void)optionChanged:(id)sender
{
    (void)sender;
    if (_viewSel.indexOfSelectedItem == O2DEBUG_VDC_FRAME) [_viewSel selectItemAtIndex:O2DEBUG_VDC_RENDER];
    [self refresh];
}

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)item
{
    (void)tabView;
    (void)item;
    _regsAt = 0;
    [self refresh];
}

/* ---- the pointer ------------------------------------------------------------------------------ */

/* What the chip draws for one hit, and where on the 360 x 243 picture. */
static NSString *describeHit(const i8244_snapshot *s, int kind, int index, NSRect *box)
{
    if (kind == 0 && index >= 0 && index < 4) {
        const i8244_sprite_info *p = &s->sprites[index];
        *box = NSMakeRect(p->x, p->y, p->width, p->height);
        return [NSString stringWithFormat:@"Sprite %d: x=%d y=%d colour=%d (%s)%s", index, p->x, p->y,
                p->color, i8244_colour_name(p->color), p->zoom ? " zoomed" : ""];
    }
    if (kind == 1 && index >= 0 && index < 12) {
        const i8244_char_info *p = &s->singles[index];
        *box = NSMakeRect(p->x, p->y, p->width, p->height);
        return [NSString stringWithFormat:@"Single %d: x=%d y=%d glyph=%d colour=%d (%s)", index, p->x, p->y,
                p->glyph, p->color, i8244_colour_name(p->color)];
    }
    if (kind == 2 && index >= 0 && index < 16) {
        const i8244_char_info *p = &s->quads[index / 4].chars[index % 4];
        *box = NSMakeRect(p->x, p->y, p->width, p->height);
        return [NSString stringWithFormat:@"Quad %d char %d: x=%d y=%d glyph=%d colour=%d (%s)", index / 4,
                index % 4, p->x, p->y, p->glyph, p->color, i8244_colour_name(p->color)];
    }
    return nil;
}

- (void)hoverX:(int)x y:(int)y
{
    if (x < 0 || y < 0) {
        _hover.stringValue = @"Point at the picture: the object under the pointer is named here and outlined";
        _pic.highlight = NSZeroRect;
        return;
    }
    i8244_snapshot snap;
    o2debug_hit hits[8];
    o2debug_vdc_get(_dbg, &snap);
    const int n = o2debug_vdc_hit_test(_dbg, x, y, hits, 8);
    NSRect box = NSZeroRect;
    NSString *what = n > 0 ? describeHit(&snap, hits[0].kind, hits[0].index, &box) : nil;
    if (!what) {
        _hover.stringValue = [NSString stringWithFormat:@"(%d, %d): nothing drawn here", x, y];
        _pic.highlight = NSZeroRect;
        return;
    }
    _hover.stringValue = [NSString stringWithFormat:@"(%d, %d)  %@%@", x, y, what,
                          n > 1 ? [NSString stringWithFormat:@"   (+%d more here)", n - 1] : @""];
    _pic.highlight = box;
}

/* ---- refreshing -------------------------------------------------------------------------------- */

- (NSInteger)section
{
    return [_sections indexOfTabViewItem:_sections.selectedTabViewItem];
}

- (void)refreshPicture
{
    int w = 0, h = 0;
    if (o2debug_vdc_view(_dbg, (int)_viewSel.indexOfSelectedItem, [self layers], [self overlays], 1,
                         _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_pic setImage:O2NewImage(_px, w, h)];
}

- (void)refreshBeam
{
    _beam.stringValue = [NSString stringWithFormat:@"%s   beam x %d, y %d (line %d, dot %d)   frame %llu",
                         _snap.is_pal ? "8245 PAL" : "8244 NTSC", _snap.x_beam, _snap.y_beam,
                         _snap.vpos, _snap.hpos, (unsigned long long)_snap.frames];
}

/* The 16 palette colours, read off the palette strip. */
- (void)loadPalette
{
    int w = 0, h = 0;
    if (!o2debug_vdc_view(_dbg, O2DEBUG_VDC_PALETTE, 0, 0, 1, _px, PX_CAP, &w, &h) || w < 8 || h < 2) return;
    const int cell = w / 8;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 8; c++)
            _pal[r * 8 + c] = _px[(size_t)(r * cell + cell / 2) * (size_t)w + (size_t)(c * cell + cell / 2)];
    _palLoaded = YES;
}

/* One object's bitmap as an image, or nil. */
- (NSImage *)thumbFor:(int)kind index:(int)index
{
    int w = 0, h = 0;
    if (!o2debug_vdc_object(_dbg, kind, index, 3, 0x00101010u, _objPx, OBJ_CAP, &w, &h) || w <= 0 || h <= 0)
        return nil;
    CGImageRef img = O2NewImage(_objPx, w, h);
    if (!img) return nil;
    NSImage *im = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(w, h)];
    CGImageRelease(img);
    return im;
}

static O2ObjectRow *headingRow(NSString *text)
{
    O2ObjectRow *r = [[O2ObjectRow alloc] init];
    r.heading = text;
    return r;
}

static const char *charState(const i8244_char_info *c)
{
    if (c->ntsc_hidden) return "hidden (NTSC, y < 14)";
    if (!c->visible) return "off screen";
    return c->clipped ? "clipped" : "visible";
}

- (void)refreshObjects
{
    NSMutableArray<O2ObjectRow *> *rows = [NSMutableArray array];

    [rows addObject:headingRow(@"Sprites ($00-$0F: Y, X, colour; shape $10-$1F)")];
    for (int i = 0; i < 4; i++) {
        const i8244_sprite_info *p = &_snap.sprites[i];
        O2ObjectRow *r = [[O2ObjectRow alloc] init];
        r.thumb = [self thumbFor:0 index:i];
        r.line1 = [NSString stringWithFormat:@"Sprite %d  x %3d  y %3d  colour %d (%s)  zoom %d  %s", i, p->x, p->y,
                   p->color, i8244_colour_name(p->color), p->zoom,
                   p->visible ? (p->clipped ? "clipped" : "visible") : "off screen"];
        r.line2 = [NSString stringWithFormat:@"Y $%02X X $%02X C $%02X  S %d X9 %d  shape %02X %02X %02X %02X %02X %02X %02X %02X",
                   p->reg_y, p->reg_x, p->reg_color, p->s, p->x9, p->shape[0], p->shape[1], p->shape[2],
                   p->shape[3], p->shape[4], p->shape[5], p->shape[6], p->shape[7]];
        [rows addObject:r];
    }

    [rows addObject:headingRow(@"Single characters (12)")];
    for (int i = 0; i < 12; i++) {
        const i8244_char_info *p = &_snap.singles[i];
        O2ObjectRow *r = [[O2ObjectRow alloc] init];
        r.thumb = [self thumbFor:1 index:i];
        r.line1 = [NSString stringWithFormat:@"Single %-2d  x %3d  y %3d  glyph %2d  colour %d (%s)  %s", i, p->x, p->y,
                   p->glyph, p->color, i8244_colour_name(p->color), charState(p)];
        r.line2 = [NSString stringWithFormat:@"Y $%02X X $%02X PTR $%02X C $%02X  bit8 %d  first row %d  rows %d",
                   p->reg_y, p->reg_x, p->reg_ptr, p->reg_color, p->bit8, p->first_row, p->rows];
        [rows addObject:r];
    }

    for (int q = 0; q < 4; q++) {
        const i8244_quad_info *quad = &_snap.quads[q];
        [rows addObject:headingRow([NSString stringWithFormat:@"Quad %d: x %d, y %d, %d rows (4 characters)",
                                    q, quad->x, quad->y, quad->rows])];
        for (int j = 0; j < 4; j++) {
            const i8244_char_info *p = &quad->chars[j];
            O2ObjectRow *r = [[O2ObjectRow alloc] init];
            r.thumb = [self thumbFor:2 index:4 * q + j];
            r.line1 = [NSString stringWithFormat:@"Quad %d.%d  x %3d  y %3d  glyph %2d  colour %d (%s)  %s", q, j,
                       p->x, p->y, p->glyph, p->color, i8244_colour_name(p->color), charState(p)];
            r.line2 = [NSString stringWithFormat:@"Y $%02X X $%02X PTR $%02X C $%02X  bit8 %d",
                       p->reg_y, p->reg_x, p->reg_ptr, p->reg_color, p->bit8];
            [rows addObject:r];
        }
    }
    _objects.rows = rows;
}

- (void)refreshGrid
{
    if (!_palLoaded) [self loadPalette];
    const int idx = (_snap.grid_color & 7) + (_snap.grid_lum ? 8 : 0);
    [_grid setSnapshot:&_snap colour:_pal[idx]];
    _gridNote.stringValue = [NSString stringWithFormat:
        @"Colour %d (%s)%s.  Grid %s, dots %s, vertical lines %s.\n"
        @"Origin x %d units (%d px), y %d; cells %d x %d px. Horizontal segments H0-H8 per column, vertical V0-V9 per row.",
        _snap.grid_color, i8244_colour_name(_snap.grid_color), _snap.grid_lum ? " bright" : "",
        _snap.grid_en ? "on" : "off", _snap.dots_en ? "on" : "off",
        _snap.fill ? "16 units wide (fill)" : "thin",
        _snap.grid_x0, _snap.grid_x0 * 2, _snap.grid_y0, _snap.cell_w, _snap.cell_h];
}

- (void)refreshRegisters
{
    /* 256 decoded lines is a lot of locking for a running machine: twice a second there */
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (!o2debug_is_stopped(_dbg) && now - _regsAt < 0.5) return;
    _regsAt = now;
    NSMutableString *s = [NSMutableString stringWithCapacity:256 * 64];
    for (unsigned off = 0; off < 0x100; off++) {
        char name[64], text[160];
        name[0] = text[0] = '\0';
        o2debug_vdc_describe(_dbg, off, name, sizeof name, text, sizeof text);
        [s appendFormat:@"$%02X  %02X  %-22s %s\n", off, _snap.regs[off], name, text];
    }
    O2SetTextKeepingPlace(_regs, s);
}

/* A click on a register line fills the edit fields from it. */
- (void)pickRegister:(int)line
{
    if (line < 0 || line > 0xff) return;
    _regAddr.stringValue = [NSString stringWithFormat:@"$%02X", line];
    _regVal.stringValue = [NSString stringWithFormat:@"$%02X", _snap.regs[line]];
}

/* A register changes only while the machine is stopped: running, the next
 * instruction would overwrite it before anyone saw the edit land. */
- (void)writeRegister:(id)sender
{
    (void)sender;
    long off, val;
    if (!o2debug_is_stopped(_dbg)) {
        _regNote.stringValue = @"Stop the machine (F5) to write a register";
        return;
    }
    if (!parseHex(_regAddr.stringValue, &off) || off < 0 || off > 0xff || !parseHex(_regVal.stringValue, &val)) {
        _regNote.stringValue = @"Needs a register $00-$FF and a value (hex)";
        return;
    }
    o2debug_vdc_write(_dbg, (unsigned)off, (uint8_t)val);
    _regNote.stringValue = [NSString stringWithFormat:@"$%02lX = $%02lX", off, val & 0xff];
    _regsAt = 0;
    [self refresh];
}

- (void)refreshControl
{
    char name[64], text[200];
    NSMutableString *s = [NSMutableString string];
    const i8244_snapshot *c = &_snap;
    [s appendFormat:@"CONTROL $A0 = $%02X\n", c->control];
    [s appendFormat:@"  bit 0  undocumented          %d\n", c->ctl_bit0];
    [s appendFormat:@"  bit 1  beam position strobe  %s\n", c->strobe ? "latched on the strobe" : "live"];
    [s appendFormat:@"  bit 2  sound interrupt       %s\n", c->sound_irq_en ? "enabled" : "off"];
    [s appendFormat:@"  bit 3  grid                  %s\n", c->grid_en ? "on" : "off"];
    [s appendFormat:@"  bit 4  external (CX) overlap %s\n", c->ext_irq_en ? "interrupt enabled" : "off"];
    [s appendFormat:@"  bit 5  foreground (objects)  %s\n", c->fg_en ? "on" : "off"];
    [s appendFormat:@"  bit 6  dots                  %s\n", c->dots_en ? "on" : "off"];
    [s appendFormat:@"  bit 7  vertical grid lines   %s\n", c->fill ? "16 units wide (fill)" : "thin"];
    [s appendFormat:@"  write lockouts: $00-$7F (objects) %s, $C0-$FF (grid) %s\n\n",
        c->lock_objects ? "ignored while the foreground is on" : "writable",
        c->lock_grid ? "ignored while the grid is on" : "writable"];

    [s appendFormat:@"STATUS $A1 = $%02X (as a read would give it now)\n", c->status];
    [s appendFormat:@"  horizontal status %d   strobe %d   sound %d\n", c->st_hstatus, c->st_strobe, c->st_sound];
    [s appendFormat:@"  vertical blank %d   external overlap %d   character overlap %d\n",
        c->st_vblank, c->st_ext_overlap, c->st_char_overlap];
    [s appendFormat:@"  _INTR line %s\n\n", c->irq ? "asserted" : "released"];

    [s appendFormat:@"COLLISION $A2: mask $%02X (written), result $%02X (latched)\n", c->col_mask, c->col_result];
    o2debug_vdc_describe(_dbg, 0xa2, name, sizeof name, text, sizeof text);
    [s appendFormat:@"  %s\n\n", text];

    [s appendFormat:@"COLOURS $A3 = $%02X\n", c->color_reg];
    [s appendFormat:@"  background %d (%s)   grid %d (%s)%s   P17 LUM %s\n\n",
        c->bg_color, i8244_colour_name(c->bg_color), c->grid_color, i8244_colour_name(c->grid_color),
        c->grid_lum ? " bright" : "", c->p17_lum ? "low: every pixel brightened" : "high"];

    [s appendFormat:@"BEAM  $A4 y = %d, $A5 x = %d   latched y %d, x %d\n", c->y_beam, c->x_beam,
        c->y_beam_latched, c->x_beam_latched];
    [s appendFormat:@"      at line %d, dot %d; the lazy renderer is at line %d, dot %d\n",
        c->vpos, c->hpos, c->last_scan, c->part_hpos];
    [s appendFormat:@"      %.2f Hz frames, %.1f Hz lines, pixel clock %.0f Hz\n",
        c->frame_hz, c->line_hz, c->pixel_hz];
    O2SetTextKeepingPlace(_control, s);
}

- (void)refreshSound
{
    const i8244_snapshot *c = &_snap;
    [_bits setValue:c->shift bits:24];
    NSMutableString *s = [NSMutableString string];
    [s appendFormat:@"Shift register  $%06X  (bits $A7 low, $A8, $A9 high)\n", c->shift & 0xffffff];
    [s appendFormat:@"Sound control   $AA = $%02X\n", c->snd_reg];
    [s appendFormat:@"  enable  %s\n", c->snd_enable ? "on" : "off"];
    [s appendFormat:@"  clock   %s (shift rate %.1f Hz)\n", c->snd_fast ? "fast" : "slow", c->shift_clock_hz];
    [s appendFormat:@"  noise   %s\n", c->snd_noise ? "on (the register is fed from a noise source)" : "off"];
    [s appendFormat:@"  volume  %d / 15\n", c->snd_volume];
    [s appendFormat:@"Output now  %d   shifter: count %d, prescaler %d%s\n", c->sh_output, c->sh_count,
        c->sh_prescaler, c->sh_pending ? ", a shift pending" : ""];
    if (c->snd_noise)
        [s appendString:@"Tone          noise: no pitch\n"];
    else if (c->freq_hz > 0)
        [s appendFormat:@"Tone          %.1f Hz (the pattern repeats every %d bit%s)\n", c->freq_hz,
            c->pattern_period, c->pattern_period == 1 ? "" : "s"];
    else
        [s appendString:@"Tone          none: a steady level\n"];
    [s appendFormat:@"Sound IRQ     %s\n", c->sound_irq_en ? "enabled" : "off"];
    O2SetTextKeepingPlace(_sound, s);
}

- (void)refreshSheets
{
    int w = 0, h = 0;
    if (o2debug_vdc_view(_dbg, O2DEBUG_VDC_FONT, 0, 0, 4, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_fontPic setImage:O2NewImage(_px, w, h)];
    if (o2debug_vdc_view(_dbg, O2DEBUG_VDC_SHAPES, 0, 0, 4, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_shapePic setImage:O2NewImage(_px, w, h)];
    if (o2debug_vdc_view(_dbg, O2DEBUG_VDC_PALETTE, 0, 0, 2, _px, PX_CAP, &w, &h) && w > 0 && h > 0)
        [_palPic setImage:O2NewImage(_px, w, h)];
    [self loadPalette];
}

- (void)refresh
{
    o2debug_vdc_get(_dbg, &_snap);
    [self refreshPicture];
    [self refreshBeam];
    switch ([self section]) {
    case SECTION_OBJECTS: [self refreshObjects]; break;
    case SECTION_GRID:    [self refreshGrid]; break;
    case SECTION_REGS:    [self refreshRegisters]; break;
    case SECTION_CONTROL: [self refreshControl]; break;
    case SECTION_SOUND:   [self refreshSound]; break;
    case SECTION_SHEETS:  [self refreshSheets]; break;
    default: break;
    }
}
@end

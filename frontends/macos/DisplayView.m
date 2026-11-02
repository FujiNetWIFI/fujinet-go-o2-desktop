/*
 * O2DisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* CoreVideo hands out host time in mach_absolute_time units, which are NOT
 * nanoseconds on every machine -- the timebase ratio converts them. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

/* The 1861's 64 x 128 picture sits in MAME's 100 x 246 visible area, and a
 * television shows that whole area at its 4:3 -- so a Odyssey 2 pixel is
 * about three times wider than tall. Square pixels show the 100 x 246 as it
 * is: a very tall image, which is correct. */
#define TV_ASPECT (4.0 / 3.0)

@implementation O2DisplayView {
    o2session *_session;
    CVDisplayLinkRef _link;
    uint32_t *_fb;
    BOOL _haveFrame;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
}

static CVReturn displayCallback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                const CVTimeStamp *out, CVOptionFlags flagsIn,
                                CVOptionFlags *flagsOut, void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    O2DisplayView *self = (__bridge O2DisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(o2session *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 984, 738)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = (size_t)O2SESSION_FB_WIDTH * O2SESSION_FB_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _cs = CGColorSpaceCreateDeviceRGB();
    /* The session's pixels are 0x00RRGGBB in host order: a little-endian
     * 32-bit context with the alpha byte skipped reads them as they are. */
    _ctx = CGBitmapContextCreate(_fb, O2SESSION_FB_WIDTH, O2SESSION_FB_HEIGHT, 8,
                                 O2SESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback, (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
}

- (void)tick:(uint64_t)hostTime
{
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    o2session_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    /* Every frame is the whole 100 x 246; nothing to measure. */
    if (!o2session_copy_frame(_session, _fb, &_serial)) return;
    _haveFrame = YES;
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{ [self setNeedsDisplay:YES]; });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);
    if (!_haveFrame) return;

    CGImageRef img = CGBitmapContextCreateImage(_ctx);
    if (!img) return;

    /* Letterboxed at the TV's 4:3, or at the picture's own 100:246. */
    const double want = _tv ? TV_ASPECT
                            : (double)O2SESSION_FB_WIDTH / (double)O2SESSION_FB_HEIGHT;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}
@end

/*
 * FujiNet Go Odyssey 2 -- the macOS (AppKit) frontend.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include "o2session.h"

int main(int argc, const char **argv)
{
    @autoreleasepool {
        /* The runtime is packed beside the executable (see CMakeLists.txt);
         * tell the session where, so a fresh install provisions its tree
         * from the bundle rather than hunting. */
        o2session_paths paths = { NULL, NULL, NULL, NULL };
        NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
        NSString *runtime = [exeDir stringByAppendingPathComponent:@"fujinet"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:runtime])
            paths.fujinet_runtime_src = [runtime fileSystemRepresentation];

        o2session *session = o2session_new(&paths);
        if (!session) {
            NSLog(@"Could not create the session (unusable config or data directories?)");
            return 1;
        }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* A path on the command line is routed as a dropped file would be:
         * a cartridge boots, a BIOS is imported, anything else goes to the
         * SD folder (see -[O2AppDelegate applicationDidFinishLaunching:]). */
        O2AppDelegate *delegate = [[O2AppDelegate alloc]
            initWithSession:session mediaPath:(argc > 1 && argv[1][0] != '-' ? argv[1] : NULL)];
        [app setDelegate:delegate];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}

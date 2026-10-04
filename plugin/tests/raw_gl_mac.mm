// Diagnostics for editor_cycle_test (macOS): one window with a plain NSOpenGLView (no JUCE) is shown, draws a few
// frames, and is closed, so the system's own per-context cost can be told apart from JUCE's and ours.
#import <AppKit/AppKit.h>
#import <OpenGL/gl.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void phyzoRawGLCycle() {
    @autoreleasepool {
        NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core, 0};
        NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 40, 1795, 848) styleMask:NSWindowStyleMaskBorderless
                                                    backing:NSBackingStoreBuffered defer:NO];
        [w setReleasedWhenClosed:NO];
        NSOpenGLView* v = [[NSOpenGLView alloc] initWithFrame:NSMakeRect(0, 0, 1795, 848) pixelFormat:pf];
        [v setWantsBestResolutionOpenGLSurface:YES];
        [w setContentView:v];
        [w orderFront:nil];
        for (int i = 0; i < 10; ++i) {
            [[v openGLContext] makeCurrentContext];
            glClearColor(0.5f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            [[v openGLContext] flushBuffer];
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        [NSOpenGLContext clearCurrentContext];
        [[v openGLContext] clearDrawable];
        [w orderOut:nil];
        [w setContentView:nil];
        [w close];
#if !__has_feature(objc_arc)
        [v release]; [w release]; [pf release];
#endif
    }
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
}

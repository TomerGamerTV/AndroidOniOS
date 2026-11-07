// onirun --window: a Mac window showing the guest's screen. The guest runs on a background thread;
// AppKit owns the main thread and redraws from the last frame the KMS device presented
// (virtgpu.c reads every committed framebuffer back into memory).
#import <AppKit/AppKit.h>
#include <pthread.h>
#include <signal.h>

#include "onx.h"
#include "sys.h"

static void (*guest_fn)(void *);
static void *guest_arg;

static void *guest_thread(void *a) { (void)a; guest_fn(guest_arg); return NULL; }

@interface OniView : NSView
@end
@implementation OniView {
    uint8_t *buf;
    size_t cap;
    uint64_t seq;
}
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
// Mouse = one finger; the view keeps the screen's aspect ratio, so it maps linearly to pixels.
- (void)touch:(NSEvent *)e phase:(int)phase {
    uint32_t w = 1080, h = 2400;
    gpu_display_size(&w, &h);
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    input_touch(phase, (int)(pt.x / b.size.width * w), (int)((1 - pt.y / b.size.height) * h));
}
- (void)mouseDown:(NSEvent *)e { [self touch:e phase:0]; }
- (void)mouseDragged:(NSEvent *)e { [self touch:e phase:1]; }
- (void)mouseUp:(NSEvent *)e { [self touch:e phase:2]; }
// Esc = Back, F1/Home = Home, F2 = Recents, F12 = Power.
- (int)keyFor:(NSEvent *)e {
    if (e.keyCode == 53) return 158;
    unichar ch = e.charactersIgnoringModifiers.length ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    if (ch == NSF1FunctionKey || ch == NSHomeFunctionKey) return 172;
    if (ch == NSF2FunctionKey) return 580;
    if (ch == NSF12FunctionKey) return 116;
    return 0;
}
- (void)keyDown:(NSEvent *)e { int k = [self keyFor:e]; if (k && !e.isARepeat) input_key(k, true); }
- (void)keyUp:(NSEvent *)e { int k = [self keyFor:e]; if (k) input_key(k, false); }
- (void)tick {
    uint32_t w = 0, h = 0;
    uint64_t s = seq;
    size_t need = 0;
    if (!gpu_frame_copy(NULL, 0, &w, &h, &s) || s == seq) return;   // size and sequence only
    need = (size_t)w * h * 4;
    if (need > cap) { free(buf); buf = malloc(need); cap = need; }
    if (!gpu_frame_copy(buf, cap, &w, &h, &seq)) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, (size_t)w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    if (img) { self.layer.contents = (__bridge id)img; CGImageRelease(img); }
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}
@end

void display_main(void (*fn)(void *), void *arg) {
    guest_fn = fn;
    guest_arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16 << 20);
    pthread_t t;
    pthread_create(&t, &at, guest_thread, NULL);
    // Signals are the guest side's business (sys_main sigwaits for them): keep AppKit's thread out.
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        uint32_t w = 1080, h = 2400;
        gpu_display_size(&w, &h);
        CGFloat scale = NSScreen.mainScreen.backingScaleFactor ?: 1;
        CGFloat maxh = NSScreen.mainScreen.visibleFrame.size.height - 40;
        CGFloat vw = w / scale, vh = h / scale;
        if (vh > maxh) { vw *= maxh / vh; vh = maxh; }
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, vw, vh)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"onirun  (Esc Back · F1 Home · F2 Recents · F12 Power)";
        win.contentAspectRatio = NSMakeSize(w, h);
        OniView *v = [[OniView alloc] initWithFrame:NSMakeRect(0, 0, vw, vh)];
        v.wantsLayer = YES;
        v.layer.backgroundColor = NSColor.blackColor.CGColor;
        v.layer.contentsGravity = kCAGravityResizeAspect;
        win.contentView = v;
        [win center];
        [win makeKeyAndOrderFront:nil];
        [win makeFirstResponder:v];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; _exit(0); }];
        [NSApp activateIgnoringOtherApps:YES];
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *tm) { (void)tm; [v tick]; }];
        [NSApp run];
    }
}

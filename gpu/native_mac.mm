#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "native.h"

// An NSView backed by a CAMetalLayer, added to the host's view. It forwards the
// mouse and runs a 60 Hz timer on the main run loop to draw frames.
@interface GpuView : NSView
@property(nonatomic) gpu::platform::Callbacks *callbacks;
@end

@implementation GpuView
- (BOOL)isFlipped { return YES; } // top-left origin, like the shader
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window ? self.window.backingScaleFactor : 1.0;
}

- (void)send:(gpu::View::Pointer)type event:(NSEvent *)event
{
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const CGFloat scale = self.layer.contentsScale;
    if (_callbacks) _callbacks->pointer(type, float(p.x * scale), float(p.y * scale));
}
- (void)mouseDown:(NSEvent *)event { [self send:gpu::View::Pointer::down event:event]; }
- (void)mouseDragged:(NSEvent *)event { [self send:gpu::View::Pointer::move event:event]; }
- (void)mouseUp:(NSEvent *)event { [self send:gpu::View::Pointer::up event:event]; }
@end

namespace gpu::platform {

const char *const windowApi = CLAP_WINDOW_API_COCOA;
const bool needsTimer = false;

struct Window
{
    GpuView *view;
    NSTimer *timer;
    Callbacks callbacks;
};

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    NSView *host = (__bridge NSView *)parent->cocoa;
    if (!host) return nullptr;
    auto *window = new Window{nil, nil, std::move(callbacks)};
    window->view = [[GpuView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    window->view.wantsLayer = YES;
    window->view.callbacks = &window->callbacks;
    window->view.layer.contentsScale = host.window ? host.window.backingScaleFactor : 1.0;
    [host addSubview:window->view];
    window->timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *) {
        window->callbacks.frame();
    }];
    // Common modes: keep drawing while the host is in a modal loop or a drag.
    [[NSRunLoop mainRunLoop] addTimer:window->timer forMode:NSRunLoopCommonModes];
    return window;
}

wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window)
{
    wgpu::SurfaceSourceMetalLayer source;
    source.layer = (__bridge void *)window->view.layer;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    return instance.CreateSurface(&descriptor);
}

void destroy(Window *window)
{
    [window->timer invalidate];
    window->view.callbacks = nullptr;
    [window->view removeFromSuperview];
    delete window;
}

void setSize(Window *window, uint32_t width, uint32_t height)
{
    [window->view setFrameSize:NSMakeSize(width, height)];
}

void pixelSize(Window *window, uint32_t &width, uint32_t &height)
{
    const CGFloat scale = window->view.layer.contentsScale;
    width = uint32_t(window->view.bounds.size.width * scale);
    height = uint32_t(window->view.bounds.size.height * scale);
}

float scale(Window *window) { return float(window->view.layer.contentsScale); }

void setVisible(Window *window, bool visible) { [window->view setHidden:!visible]; }

void pumpEvents(Window *) {}

}

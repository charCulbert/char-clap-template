#import <AppKit/AppKit.h>
#include "native.h"

namespace webview::platform {

const char *const windowApi = CLAP_WINDOW_API_COCOA;
const bool needsTimer = false;

void *attach(void *view, const clap_window_t *parent)
{
    NSView *child = (__bridge NSView *)view;
    NSView *host = (__bridge NSView *)parent->cocoa;
    if (!host) return nullptr;
    [child setFrame:[host bounds]];
    // clap.gui.set_size sizes the view; autoresizing would apply a host resize twice.
    [child setAutoresizingMask:NSViewNotSizable];
    [host addSubview:child];
    return (__bridge void *)host;
}

void detach(void *view, void *) { [(__bridge NSView *)view removeFromSuperview]; }

void setSize(void *view, uint32_t width, uint32_t height)
{
    [(__bridge NSView *)view setFrameSize:NSMakeSize(width, height)];
}

void setVisible(void *view, bool visible) { [(__bridge NSView *)view setHidden:!visible]; }

float scale(void *) { return 1.0f; } // clap.gui on macOS is in points already

void pumpEvents() {}

}

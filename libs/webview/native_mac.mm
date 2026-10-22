#import <AppKit/AppKit.h>
#include "native.h"

namespace webview::platform
{

const char* const windowApi = CLAP_WINDOW_API_COCOA;
const bool needsTimer = false;

void* attach (void* view, const clap_window_t* parent)
{
    NSView* child = (__bridge NSView*) view;
    NSView* host = (__bridge NSView*) parent->cocoa;
    if (!host)
        return nullptr;
    [child setFrame:[host bounds]];
    // clap.gui.set_size sizes the view; autoresizing would apply a host resize twice.
    [child setAutoresizingMask:NSViewNotSizable];
    [host addSubview:child];
    return (__bridge void*) host;
}

void detach (void* view, void*)
{
    [(__bridge NSView*) view removeFromSuperview];
}

void setSize (void* view, uint32_t width, uint32_t height)
{
    [(__bridge NSView*) view setFrameSize:NSMakeSize (width, height)];
}

void setVisible (void* view, bool visible)
{
    [(__bridge NSView*) view setHidden:!visible];
}

// Up the responder chain, as a chardraw view passes on a key it doesn't use.
void tabOut (void* view, bool back)
{
    NSView* page = (__bridge NSView*) view;
    NSString* tab = back ? @"\x19" : @"\t"; // Shift+Tab types a back tab
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                      location:NSZeroPoint
                                 modifierFlags:back ? NSEventModifierFlagShift : 0
                                     timestamp:NSProcessInfo.processInfo.systemUptime
                                  windowNumber:page.window.windowNumber
                                       context:nil
                                    characters:tab
                   charactersIgnoringModifiers:tab
                                     isARepeat:NO
                                       keyCode:48];
    [page.nextResponder keyDown:event];
}

// clap.gui on macOS is in points already.
float scale (void*)
{
    return 1.0f;
}
float defaultScale()
{
    return 1.0f;
}

} // namespace webview::platform

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include "native.h"
#include <optional>

// An NSView backed by a CAMetalLayer, added to the host's view. It forwards the
// mouse and keys and runs a 60 Hz timer on the main run loop to draw frames.
@interface GpuView : NSView <NSDraggingSource>
@property(nonatomic) gpu::platform::Callbacks *callbacks;
@property(nonatomic) BOOL locked;
- (BOOL)handleKey:(NSEvent *)event;
- (void)edit:(gpu::View::Key)key;
- (void)paste;
- (void)dragFiles:(const std::vector<std::string> &)paths;
@end

// Takes the keys while a text field is edited (see textInput) and gives them
// all to the GpuView, so nothing is typed into the text view itself.
@interface GpuKeys : NSTextView
@property(nonatomic, weak) GpuView *target;
@end

@implementation GpuKeys
- (void)keyDown:(NSEvent *)event { [self.target keyDown:event]; }
- (BOOL)performKeyEquivalent:(NSEvent *)event { return [self.target handleKey:event]; }
// The Edit menu's actions, which hosts send a text view for Cmd+C, X, V and A.
- (void)copy:(id)sender { [self.target edit:gpu::View::Key::copy]; }
- (void)cut:(id)sender { [self.target edit:gpu::View::Key::cut]; }
- (void)paste:(id)sender { [self.target paste]; }
- (void)selectAll:(id)sender { [self.target edit:gpu::View::Key::selectAll]; }
- (void)undo:(id)sender { [self.target edit:gpu::View::Key::undo]; }
- (void)redo:(id)sender { [self.target edit:gpu::View::Key::redo]; }
@end

@implementation GpuView
{
    NSPoint _position; // pixels; keeps moving while the pointer is locked
    NSEvent *_lastMouse; // the press or drag a drag of files out starts from
}
- (BOOL)isFlipped { return YES; } // top-left origin, like the shader
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window ? self.window.backingScaleFactor : 1.0;
}
// Moves without a button held (for hover), and leaving the view. Plugin
// windows are often not key, so track always.
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *area in self.trackingAreas) [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways
                                         | NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
}
- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow]; // hosts may add us to a window after creating us
    if (self.window) self.layer.contentsScale = self.window.backingScaleFactor;
}

static unsigned modifiers(NSEvent *event)
{
    const NSEventModifierFlags flags = event.modifierFlags;
    return (flags & NSEventModifierFlagShift ? gpu::View::shift : 0u) | (flags & NSEventModifierFlagControl ? gpu::View::control : 0u)
         | (flags & NSEventModifierFlagOption ? gpu::View::alt : 0u) | (flags & NSEventModifierFlagCommand ? gpu::View::command : 0u);
}

- (void)send:(gpu::View::Pointer)type event:(NSEvent *)event
{
    if (type == gpu::View::Pointer::down || type == gpu::View::Pointer::move) _lastMouse = event;
    const CGFloat scale = self.layer.contentsScale;
    if (_locked && type == gpu::View::Pointer::move)
    {
        // The cursor is frozen: move by the mouse's own deltas (points, y down).
        _position.x += event.deltaX * scale;
        _position.y += event.deltaY * scale;
    }
    else if (!_locked)
    {
        const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        _position = NSMakePoint(p.x * scale, p.y * scale);
    }
    if (_callbacks) _callbacks->pointer({type, float(_position.x), float(_position.y), modifiers(event)});
}
// Files dragged in: their paths, and where they are in pixels.
- (NSDragOperation)files:(gpu::View::FileDrag)type info:(id<NSDraggingInfo>)info
{
    if (!_callbacks) return NSDragOperationNone;
    std::vector<std::string> paths;
    for (NSURL *url in [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}])
        paths.push_back(url.fileSystemRepresentation);
    const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    const CGFloat scale = self.layer.contentsScale;
    return _callbacks->files(type, float(p.x * scale), float(p.y * scale), paths) ? NSDragOperationCopy : NSDragOperationNone;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { return [self files:gpu::View::FileDrag::over info:info]; }
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info { return [self files:gpu::View::FileDrag::over info:info]; }
- (void)draggingExited:(id<NSDraggingInfo>)info
{
    if (_callbacks) _callbacks->files(gpu::View::FileDrag::leave, 0, 0, {});
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    return [self files:gpu::View::FileDrag::drop info:info] != NSDragOperationNone;
}
// Files dragged out, each with its Finder icon under the pointer.
- (void)dragFiles:(const std::vector<std::string> &)paths
{
    if (!_lastMouse) return;
    const NSPoint at = [self convertPoint:_lastMouse.locationInWindow fromView:nil];
    NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
    for (const std::string &path : paths)
    {
        NSString *file = [NSString stringWithUTF8String:path.c_str()];
        if (!file) continue;
        NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:[NSURL fileURLWithPath:file]];
        [item setDraggingFrame:NSMakeRect(at.x - 16, at.y - 16, 32, 32) contents:[NSWorkspace.sharedWorkspace iconForFile:file]];
        [items addObject:item];
    }
    if (items.count) [self beginDraggingSessionWithItems:items event:_lastMouse source:self];
}
- (NSDragOperation)draggingSession:(NSDraggingSession *)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return NSDragOperationCopy;
}
// The drag had the mouse, so the view never saw it let go.
- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
    if (_callbacks) _callbacks->pointer({gpu::View::Pointer::up, float(_position.x), float(_position.y), 0});
}
- (void)mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self]; // for keys
    [self send:gpu::View::Pointer::down event:event];
}
- (void)scrollWheel:(NSEvent *)event
{
    if (!_callbacks) return;
    // Trackpads report points, wheels lines (notches). Follow the fingers,
    // not "natural scrolling": a swipe away from the user turns things up.
    double notches = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 10 : event.scrollingDeltaY;
    if (event.isDirectionInvertedFromDevice) notches = -notches;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const CGFloat scale = self.layer.contentsScale;
    _callbacks->pointer({gpu::View::Pointer::wheel, float(p.x * scale), float(p.y * scale), modifiers(event), float(notches)});
}
- (BOOL)pasted
{
    NSString *text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    return _callbacks && text && _callbacks->text(text.UTF8String);
}
- (void)paste { [self pasted]; }
- (void)edit:(gpu::View::Key)key
{
    if (_callbacks) _callbacks->key(key, gpu::View::command);
}
// Cmd+Z, Shift+Cmd+Z, A, X, C and V, for a text field being edited; false
// for the rest, which go on to the host (its own undo, for one).
- (BOOL)shortcut:(NSEvent *)event
{
    using Key = gpu::View::Key;
    const NSEventModifierFlags flags = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    const NSEventModifierFlags chords = NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption;
    if (!_callbacks || (flags & chords) != NSEventModifierFlagCommand) return NO;
    NSString *letter = event.charactersIgnoringModifiers.lowercaseString;
    if ([letter isEqualToString:@"v"]) return [self pasted];
    std::optional<Key> key;
    if ([letter isEqualToString:@"z"]) key = flags & NSEventModifierFlagShift ? Key::redo : Key::undo;
    else if ([letter isEqualToString:@"a"]) key = Key::selectAll;
    else if ([letter isEqualToString:@"x"]) key = Key::cut;
    else if ([letter isEqualToString:@"c"]) key = Key::copy;
    return key && _callbacks->key(*key, modifiers(event));
}
// Cmd shortcuts come here before the host's menus get them.
- (BOOL)performKeyEquivalent:(NSEvent *)event
{
    if (self.window.firstResponder == self && [self shortcut:event]) return YES;
    return [super performKeyEquivalent:event];
}
// Whether the view took the key: the shortcuts above, keys a focused control
// or text field uses, and typed text while a field is edited.
- (BOOL)handleKey:(NSEvent *)event
{
    if ([self shortcut:event]) return YES;
    using Key = gpu::View::Key;
    std::optional<Key> key;
    switch (event.keyCode) // kVK_* from Carbon's Events.h
    {
    case 123: key = Key::left; break;
    case 124: key = Key::right; break;
    case 125: key = Key::down; break;
    case 126: key = Key::up; break;
    case 116: key = Key::pageUp; break;
    case 121: key = Key::pageDown; break;
    case 115: key = Key::home; break;
    case 119: key = Key::end; break;
    case 48: key = Key::tab; break;
    case 36: case 76: key = Key::enter; break;
    case 53: key = Key::escape; break;
    case 51: key = Key::backspace; break;
    case 117: key = Key::deleteForward; break;
    }
    if (_callbacks && key && _callbacks->key(*key, modifiers(event))) return YES;
    // Typed characters, but not shortcuts or function keys (U+F700 and up).
    NSString *characters = event.characters;
    const bool typed = !key && characters.length > 0 && [characters characterAtIndex:0] >= 0x20
                    && [characters characterAtIndex:0] < 0xF700 && [characters characterAtIndex:0] != 0x7F
                    && !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl));
    return _callbacks && typed && _callbacks->text(characters.UTF8String);
}
- (void)keyDown:(NSEvent *)event
{
    if (![self handleKey:event]) [super keyDown:event]; // up the responder chain, to the host
}
- (void)mouseDragged:(NSEvent *)event { [self send:gpu::View::Pointer::move event:event]; }
- (void)mouseMoved:(NSEvent *)event { [self send:gpu::View::Pointer::move event:event]; }
- (void)mouseExited:(NSEvent *)event
{
    if (_callbacks && !_locked) _callbacks->pointer({gpu::View::Pointer::leave, float(_position.x), float(_position.y), modifiers(event)});
}
- (void)mouseUp:(NSEvent *)event { [self send:gpu::View::Pointer::up event:event]; }
@end

namespace gpu::platform {

const char *const windowApi = CLAP_WINDOW_API_COCOA;
const bool needsTimer = false;

struct Window
{
    GpuView *view;
    GpuKeys *keys;
    NSTimer *timer;
    Callbacks callbacks;
};

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    NSView *host = (__bridge NSView *)parent->cocoa;
    if (!host) return nullptr;
    auto *window = new Window{nil, nil, nil, std::move(callbacks)};
    window->view = [[GpuView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    window->view.wantsLayer = YES;
    window->view.callbacks = &window->callbacks;
    window->view.layer.contentsScale = host.window ? host.window.backingScaleFactor : 1.0;
    [window->view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [host addSubview:window->view];
    window->keys = [[GpuKeys alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    window->keys.target = window->view;
    window->keys.alphaValue = 0;
    [window->view addSubview:window->keys];
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

void lockPointer(Window *window, bool locked)
{
    if (window->view.locked == locked) return;
    window->view.locked = locked;
    // Detach the cursor from the mouse, so it stays where the drag began.
    CGAssociateMouseAndMouseCursorPosition(!locked);
    if (locked) [NSCursor hide];
    else [NSCursor unhide];
}

void destroy(Window *window)
{
    lockPointer(window, false);
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

float scale(Window *window) { return float(window ? window->view.layer.contentsScale : NSScreen.mainScreen.backingScaleFactor); }

void setVisible(Window *window, bool visible) { [window->view setHidden:!visible]; }

void pumpEvents(Window *) {}

void dragFiles(Window *window, const std::vector<std::string> &paths) { [window->view dragFiles:paths]; }

// While a field is edited, an invisible NSTextView holds the keyboard: hosts
// that act on keys before the focused view (REAPER plays on Space) leave a
// system text view alone, as they would VSTGUI's and iPlug2's native fields.
void textInput(Window *window, bool editing)
{
    NSWindow *host = window->view.window;
    if (!host) return;
    if (editing && host.firstResponder == window->view) [host makeFirstResponder:window->keys];
    else if (!editing && host.firstResponder == window->keys) [host makeFirstResponder:window->view];
}

void copyText(Window *, const std::string &utf8)
{
    NSString *text = [NSString stringWithUTF8String:utf8.c_str()];
    if (!text) return;
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:text forType:NSPasteboardTypeString];
}

// The view's own appearance: the system's, unless the host sets its window's.
bool darkMode(Window *window)
{
    NSAppearanceName name = [window->view.effectiveAppearance
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [name isEqualToString:NSAppearanceNameDarkAqua];
}

}

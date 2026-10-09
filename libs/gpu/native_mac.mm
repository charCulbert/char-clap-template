#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h> // CADisplayLink
#include "native.h"
#include "accesskit_tree.h"
#include <optional>

// The classes below are global to the host process: two plugins (or two
// versions of one) with a GpuView each would get one of them. GPU_OBJC_PREFIX
// (from gpu.cmake) names them after the plugin.
#ifndef GPU_OBJC_PREFIX
#define GPU_OBJC_PREFIX Gpu_
#endif
#define GPU_JOIN2(a, b) a##b
#define GPU_JOIN(a, b) GPU_JOIN2(a, b)
#define GpuView GPU_JOIN(GPU_OBJC_PREFIX, View)
#define GpuKeys GPU_JOIN(GPU_OBJC_PREFIX, Keys)

namespace gpu::platform { // the view's accessibility, below
NSArray *accessChildren(Window *window);
id accessFocus(Window *window);
id accessHitTest(Window *window, NSPoint point);
void accessFocused(Window *window, bool focused);
}

// Calls a block: the target a display link (or a timer) calls.
@interface GpuTick : NSObject
@property(nonatomic, copy) void (^tick)(void);
- (void)fire:(id)sender;
@end
@implementation GpuTick
- (void)fire:(id)sender { if (self.tick) self.tick(); }
@end

// An NSView backed by a CAMetalLayer, added to the host's view. It forwards the
// mouse and keys, and draws frames in step with its screen (a display link:
// 120 a second on a ProMotion display), or at 60 Hz before macOS 14.
@interface GpuView : NSView <NSDraggingSource>
@property(nonatomic) gpu::platform::Callbacks *callbacks;
@property(nonatomic) gpu::platform::Window *owner; // for screen readers (accessibility)
@property(nonatomic) BOOL locked;
@property(nonatomic, strong) NSCursor *cursor; // over the view (setCursor)
- (BOOL)handleKey:(NSEvent *)event;
- (BOOL)handleNamedKey:(NSEvent *)event;
- (BOOL)typeText:(NSString *)text;
- (void)edit:(gpu::View::Key)key;
- (void)paste;
- (void)dragFiles:(const std::vector<std::string> &)paths;
@end

// Takes the keys while a text field is edited (see textInput). The keys the
// view names (arrows, Enter, its shortcuts) go to it; the rest go through
// the system's text input, which composes what takes more than one key
// (dead keys, press-and-hold accents, input methods) and hands back text.
@interface GpuKeys : NSTextView
@property(nonatomic, weak) GpuView *target;
@end

@implementation GpuKeys
- (void)keyDown:(NSEvent *)event
{
    if (!self.hasMarkedText && [self.target handleNamedKey:event]) return;
    [self interpretKeyEvents:@[ event ]];
}
- (void)insertText:(id)string replacementRange:(NSRange)range
{
    [super insertText:string replacementRange:range]; // ends any composition
    [self.target typeText:[string isKindOfClass:NSAttributedString.class] ? [string string] : string];
    self.string = @""; // the view keeps the text, not us
}
- (void)doCommandBySelector:(SEL)selector {} // keys the view didn't take: nothing for a hidden field to do
// AppKit offers key equivalents to every view in the window, focused or not.
- (BOOL)performKeyEquivalent:(NSEvent *)event { return self.window.firstResponder == self && [self.target handleKey:event]; }
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
// Screen readers: the view is a container, and AccessKit answers for what's
// in it from the tree the view records.
- (BOOL)isAccessibilityElement { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:self.cursor ?: NSCursor.arrowCursor]; }
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
    // Trackpads report points, wheels lines (notches). Knobs follow the
    // fingers, not "natural scrolling": a swipe away from the user turns
    // things up. Lists scroll as the system does (inverted).
    double notches = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 10 : event.scrollingDeltaY;
    if (event.isDirectionInvertedFromDevice) notches = -notches;
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const CGFloat scale = self.layer.contentsScale;
    _callbacks->pointer({gpu::View::Pointer::wheel, float(p.x * scale), float(p.y * scale), modifiers(event), float(notches),
                         bool(event.isDirectionInvertedFromDevice)});
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
    if ([self handleNamedKey:event]) return YES;
    // Typed characters, but not shortcuts or function keys (U+F700 and up).
    const bool chord = event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl);
    return !chord && [self typeText:event.characters];
}
- (BOOL)typeText:(NSString *)text
{
    const bool typed = text.length > 0 && [text characterAtIndex:0] >= 0x20 && [text characterAtIndex:0] < 0xF700
                    && [text characterAtIndex:0] != 0x7F;
    return _callbacks && typed && _callbacks->text(text.UTF8String);
}
// Shortcuts and the keys the view names (arrows, Enter, Delete...).
- (BOOL)handleNamedKey:(NSEvent *)event
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
    return _callbacks && key && _callbacks->key(*key, modifiers(event));
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
- (NSArray *)accessibilityChildren { return self.owner ? gpu::platform::accessChildren(self.owner) : @[]; }
- (id)accessibilityFocusedUIElement { return self.owner ? gpu::platform::accessFocus(self.owner) : nil; }
- (id)accessibilityHitTest:(NSPoint)point { return self.owner ? gpu::platform::accessHitTest(self.owner, point) : nil; }
- (BOOL)becomeFirstResponder
{
    if (self.owner) gpu::platform::accessFocused(self.owner, true);
    return YES;
}
- (BOOL)resignFirstResponder
{
    if (self.owner) gpu::platform::accessFocused(self.owner, false);
    return YES;
}
@end

namespace gpu::platform {

const Api windowApi = Api::cocoa;
const bool needsTimer = false;

struct Window
{
    GpuView *view;
    GpuKeys *keys;
    GpuTick *tick;
    id link; // CADisplayLink, or before macOS 14 an NSTimer
    Callbacks callbacks;
    accesskit_macos_adapter *access = nullptr; // screen readers, through NSAccessibility
    Accessibility tree;                        // the view's, as screen readers last had it
};

static accesskit_tree_update *wholeTree(void *window) { return accesskit::update(static_cast<Window *>(window)->tree); }

static void accessAction(accesskit_action_request *request, void *window)
{
    uint64_t id;
    Accessible::Action kind;
    double value;
    if (!accesskit::action(request, id, kind, value)) return;
    dispatch_async(dispatch_get_main_queue(), ^{ // where views are used, should AppKit ever call from elsewhere
        if (auto *w = static_cast<Window *>(window); w->view.callbacks) w->callbacks.access(id, kind, value);
    });
}

static void raise(accesskit_macos_queued_events *events)
{
    if (events) accesskit_macos_queued_events_raise(events);
}

void accessibility(Window *window, const Accessibility &tree)
{
    window->tree = tree;
    raise(accesskit_macos_adapter_update_if_active(window->access, wholeTree, window));
}

NSArray *accessChildren(Window *window) { return (__bridge NSArray *)accesskit_macos_adapter_view_children(window->access, wholeTree, window); }
id accessFocus(Window *window) { return (__bridge id)accesskit_macos_adapter_focus(window->access, wholeTree, window); }
id accessHitTest(Window *window, NSPoint point) { return (__bridge id)accesskit_macos_adapter_hit_test(window->access, point.x, point.y, wholeTree, window); }
void accessFocused(Window *window, bool focused) { raise(accesskit_macos_adapter_update_view_focus_state(window->access, focused)); }

Window *create(const Parent &parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    NSView *host = (__bridge NSView *)parent.handle;
    if (!host) return nullptr;
    auto *window = new Window{nil, nil, nil, nil, std::move(callbacks)};
    window->view = [[GpuView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    window->view.wantsLayer = YES;
    window->view.callbacks = &window->callbacks;
    window->view.owner = window;
    window->access = accesskit_macos_adapter_new((__bridge void *)window->view, false, accessAction, window);
    window->view.layer.contentsScale = host.window ? host.window.backingScaleFactor : 1.0;
    [window->view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    [host addSubview:window->view];
    window->keys = [[GpuKeys alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    window->keys.target = window->view;
    window->keys.alphaValue = 0;
    [window->view addSubview:window->keys];
    window->tick = [GpuTick new];
    window->tick.tick = ^{ window->callbacks.frame(); };
    // In common modes: drawing goes on while the host is in a modal loop or a drag.
    if (@available(macOS 14.0, *))
    {
        // The view's own screen's refresh, following it to another screen.
        CADisplayLink *link = [window->view displayLinkWithTarget:window->tick selector:@selector(fire:)];
        link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 120);
        [link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        window->link = link;
    }
    else
    {
        NSTimer *timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 target:window->tick selector:@selector(fire:) userInfo:nil repeats:YES];
        [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
        window->link = timer;
    }
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

void setCursor(Window *window, View::Cursor cursor)
{
    NSCursor *shape = NSCursor.arrowCursor;
    switch (cursor)
    {
    case View::Cursor::arrow: break;
    case View::Cursor::text: shape = NSCursor.IBeamCursor; break;
    case View::Cursor::hand: shape = NSCursor.pointingHandCursor; break;
    case View::Cursor::move: shape = NSCursor.openHandCursor; break; // macOS has no four-way arrow
    case View::Cursor::resizeLeftRight: shape = NSCursor.resizeLeftRightCursor; break;
    case View::Cursor::resizeUpDown: shape = NSCursor.resizeUpDownCursor; break;
    case View::Cursor::resizeDiagonal:
        if (@available(macOS 15.0, *))
            shape = [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomRight
                                               inDirections:NSCursorFrameResizeDirectionsAll];
        else
            shape = NSCursor.crosshairCursor;
        break;
    }
    window->view.cursor = shape;
    [window->view.window invalidateCursorRectsForView:window->view];
    // Now, not at the next pointer move, when the pointer is over the view.
    const NSPoint at = [window->view convertPoint:window->view.window.mouseLocationOutsideOfEventStream fromView:nil];
    if (!window->view.locked && NSPointInRect(at, window->view.bounds)) [shape set];
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
    [window->link invalidate]; // CADisplayLink and NSTimer both
    window->view.callbacks = nullptr;
    window->view.owner = nullptr;
    [window->view removeFromSuperview];
    accesskit_macos_adapter_free(window->access);
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
float refreshRate(Window *) { return 0; } // a display link paces frames

void dragFiles(Window *window, const std::vector<std::string> &paths) { [window->view dragFiles:paths]; }

// Off screen, the layer has no drawables to give, and asking for one blocks
// the host's main thread for up to a second.
bool onScreen(Window *window)
{
    NSWindow *host = window->view.window;
    return host && !window->view.isHiddenOrHasHiddenAncestor && (host.occlusionState & NSWindowOcclusionStateVisible);
}

// While a field is edited, an invisible NSTextView holds the keyboard: hosts
// that act on keys before the focused view (REAPER plays on Space) leave a
// system text view alone, as they would VSTGUI's and iPlug2's native fields.
void textInput(Window *window, bool editing, View::Caret caret)
{
    NSWindow *host = window->view.window;
    if (!host) return;
    // At the caret, which is where input methods show what's being composed.
    const CGFloat scale = window->view.layer.contentsScale;
    if (editing && caret.height > 0)
        window->keys.frame = NSMakeRect(caret.x / scale, caret.y / scale, 1, caret.height / scale);
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

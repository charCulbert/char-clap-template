#include "native.h"
#include <X11/Xlib.h> // after webgpu_cpp.h: Xlib defines macros such as Status and None
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h> // XLookupString
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

// A child X11 window in the host's window. Dawn draws into it with Vulkan.
// Linux hosts have no shared event loop, so the host's timer (needsTimer)
// drives both our X events and frames.
namespace gpu::platform {

const char *const windowApi = CLAP_WINDOW_API_X11;
const bool needsTimer = true;

struct Window
{
    Display *display = nullptr; // our own connection, so we see only our events
    ::Window window = 0;
    ::Window parent = 0;          // the host's, which gets the keyboard back after a text edit
    bool focused = false;         // we took the keyboard for a text field
    Callbacks callbacks;
    bool locked = false;
    int anchorX = 0, anchorY = 0; // where the cursor is held while locked
    float x = 0, y = 0;           // pointer position reported to the view (pixels)
    Cursor blank = 0;             // an invisible cursor, made on first lock
    std::string clipboard;        // what we've copied, while we own CLIPBOARD
    Atom clipboardAtom = 0, utf8 = 0, targets = 0, pasted = 0;
    float scale = 1;              // see desktopScale
    // XDND, the X drag-and-drop protocol, both ways.
    Atom xdndAware = 0, xdndProxy = 0, xdndEnter = 0, xdndPosition = 0, xdndStatus = 0, xdndLeave = 0, xdndDrop = 0,
         xdndFinished = 0, xdndSelection = 0, xdndCopy = 0, uriList = 0;
    std::vector<std::string> dropping; // files dragged in, once their list has come
    ::Window dropSource = 0;           // the window dragging them in, and the one it addressed
    ::Window dropTarget = 0;
    int dropX = 0, dropY = 0;          // pixels in the view
    bool dropPending = false;          // XdndDrop came before the list
    std::vector<std::string> dragging; // files dragged out, offered as XdndSelection
    bool dragActive = false;           // the button is still down
    ::Window dragTo = 0;               // the XDND-aware window under the pointer
    bool dragTaken = false;            // which says it would take them
};

// The desktop's scale: Xft.dpi (GNOME and KDE set it from their scaling
// setting) over 96, times GDK_SCALE where set. X11 has no per-monitor scale
// to ask; a host that knows better says so with clap.gui.set_scale.
static float desktopScale(Display *display)
{
    float scale = 1;
    if (const char *resources = XResourceManagerString(display))
        if (const char *dpi = std::strstr(resources, "Xft.dpi:"))
            if (const float value = std::strtof(dpi + 8, nullptr); value >= 48 && value <= 480) scale = value / 96;
    if (const char *gdk = std::getenv("GDK_SCALE"))
        if (const int factor = std::atoi(gdk); factor >= 1 && factor <= 4) scale *= float(factor);
    return scale;
}

// X errors on our own connections (BadWindow once the host has destroyed our
// parent window, say) are ignored; Xlib's default handler would exit the
// host. Others go on to the handler that was there before.
static std::vector<Display *> ourDisplays;
static XErrorHandler hostHandler = nullptr;

static int ignoreOurErrors(Display *display, XErrorEvent *error)
{
    if (std::find(ourDisplays.begin(), ourDisplays.end(), display) != ourDisplays.end()) return 0;
    return hostHandler ? hostHandler(display, error) : 0;
}

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    Display *display = XOpenDisplay(nullptr);
    if (!display || !parent->x11)
    {
        if (display) XCloseDisplay(display);
        return nullptr;
    }
    if (ourDisplays.empty()) hostHandler = XSetErrorHandler(ignoreOurErrors);
    ourDisplays.push_back(display);
    auto *window = new Window;
    window->display = display;
    window->callbacks = std::move(callbacks);
    window->scale = desktopScale(display);
    window->parent = ::Window(parent->x11);
    window->window = XCreateSimpleWindow(display, window->parent, 0, 0, width, height, 0, 0, 0);
    window->clipboardAtom = XInternAtom(display, "CLIPBOARD", False);
    window->utf8 = XInternAtom(display, "UTF8_STRING", False);
    window->targets = XInternAtom(display, "TARGETS", False);
    window->pasted = XInternAtom(display, "GPU_PASTED", False); // where a paste is delivered
    for (auto [atom, name] : {std::pair{&window->xdndAware, "XdndAware"}, {&window->xdndProxy, "XdndProxy"},
                              {&window->xdndEnter, "XdndEnter"}, {&window->xdndPosition, "XdndPosition"},
                              {&window->xdndStatus, "XdndStatus"}, {&window->xdndLeave, "XdndLeave"},
                              {&window->xdndDrop, "XdndDrop"}, {&window->xdndFinished, "XdndFinished"},
                              {&window->xdndSelection, "XdndSelection"}, {&window->xdndCopy, "XdndActionCopy"},
                              {&window->uriList, "text/uri-list"}})
        *atom = XInternAtom(display, name, False);
    // Drag sources look for XdndAware on the window under the pointer that the
    // host made for us, not our own child of it (Visage does the same): mark
    // it, with XdndProxy sending its messages on to us.
    const long version = 5;
    for (::Window w : {window->window, window->parent})
        XChangeProperty(display, w, window->xdndAware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&version), 1);
    XChangeProperty(display, window->parent, window->xdndProxy, XA_WINDOW, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&window->window), 1);
    XSelectInput(display, window->window,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | KeyPressMask | ExposureMask);
    XMapWindow(display, window->window);
    XFlush(display);
    return window;
}

wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window)
{
    wgpu::SurfaceSourceXlibWindow source;
    source.display = window->display;
    source.window = window->window;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    return instance.CreateSurface(&descriptor);
}

void lockPointer(Window *window, bool locked)
{
    if (window->locked == locked) return;
    window->locked = locked;
    if (!locked)
    {
        XUngrabPointer(window->display, CurrentTime);
        XFlush(window->display);
        return;
    }
    if (!window->blank)
    {
        const char bits[1] = {0};
        const Pixmap empty = XCreateBitmapFromData(window->display, window->window, bits, 1, 1);
        XColor black = {};
        window->blank = XCreatePixmapCursor(window->display, empty, empty, &black, &black, 0, 0);
        XFreePixmap(window->display, empty);
    }
    window->anchorX = int(window->x), window->anchorY = int(window->y);
    XGrabPointer(window->display, window->window, False, ButtonReleaseMask | Button1MotionMask,
                 GrabModeAsync, GrabModeAsync, 0, window->blank, CurrentTime);
    XFlush(window->display);
}

void destroy(Window *window)
{
    lockPointer(window, false);
    if (window->blank) XFreeCursor(window->display, window->blank);
    XDeleteProperty(window->display, window->parent, window->xdndAware);
    XDeleteProperty(window->display, window->parent, window->xdndProxy);
    XDestroyWindow(window->display, window->window);
    XSync(window->display, False); // its errors, while they're still ours to ignore
    std::erase(ourDisplays, window->display);
    XCloseDisplay(window->display);
    if (ourDisplays.empty()) XSetErrorHandler(hostHandler);
    delete window;
}

void setSize(Window *window, uint32_t width, uint32_t height)
{
    XResizeWindow(window->display, window->window, width, height);
    XFlush(window->display);
}

void pixelSize(Window *window, uint32_t &width, uint32_t &height)
{
    XWindowAttributes attributes = {};
    XGetWindowAttributes(window->display, window->window, &attributes);
    width = uint32_t(attributes.width), height = uint32_t(attributes.height);
}

// Desktops have no common, cheap way to ask; GTK_THEME=Name:light or
// Name:dark is the one convention. Dark otherwise, as most audio software is.
bool darkMode(Window *)
{
    const char *theme = std::getenv("GTK_THEME");
    return !(theme && std::strstr(theme, ":light"));
}

float scale(Window *window)
{
    if (window) return window->scale;
    Display *display = XOpenDisplay(nullptr); // before the view has a window: the default screen's
    const float scale = display ? desktopScale(display) : 1.0f;
    if (display) XCloseDisplay(display);
    return scale;
}

void setVisible(Window *window, bool visible)
{
    if (visible) XMapWindow(window->display, window->window);
    else XUnmapWindow(window->display, window->window);
    XFlush(window->display);
}

static unsigned modifiers(unsigned state)
{
    return (state & ShiftMask ? View::shift : 0u) | (state & ControlMask ? View::control : 0u) | (state & Mod1Mask ? View::alt : 0u);
}

// X11's clipboard is a conversation: the owner answers each request for it.
// The keyboard is ours only while a text field is edited: a click alone
// doesn't take it, so the host's shortcuts (Space plays) keep working.
void textInput(Window *window, bool editing, View::Caret)
{
    if (editing == window->focused) return;
    window->focused = editing;
    XSetInputFocus(window->display, editing ? window->window : window->parent, RevertToParent, CurrentTime);
    XFlush(window->display);
}

bool onScreen(Window *window)
{
    XWindowAttributes attributes;
    return XGetWindowAttributes(window->display, window->window, &attributes) && attributes.map_state == IsViewable;
}

// Files dragged out: we own XdndSelection while the button is down, and
// pumpEvents tells the window under the pointer (see dragMotion).
void dragFiles(Window *window, const std::vector<std::string> &paths)
{
    window->dragging = paths;
    window->dragActive = true;
    window->dragTo = 0, window->dragTaken = false;
    XSetSelectionOwner(window->display, window->xdndSelection, window->window, CurrentTime);
    XFlush(window->display);
}

void copyText(Window *window, const std::string &utf8)
{
    window->clipboard = utf8;
    XSetSelectionOwner(window->display, window->clipboardAtom, window->window, CurrentTime);
    XFlush(window->display);
}

// text/uri-list, the form XDND passes files in: file:// URIs, %-escaped, one per line.
static std::string uriList(const std::vector<std::string> &paths)
{
    std::string list;
    for (const std::string &path : paths)
    {
        list += "file://";
        for (const unsigned char c : path)
        {
            if (std::isalnum(c) || std::strchr("/-._~", c)) list += char(c);
            else
            {
                char escaped[4];
                std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
                list += escaped;
            }
        }
        list += "\r\n";
    }
    return list;
}

static std::vector<std::string> pathsIn(const std::string &list)
{
    std::vector<std::string> paths;
    size_t at = 0;
    while (at < list.size())
    {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("file://", 0) != 0) continue; // comments, other schemes
        line.erase(0, line.find('/', 7)); // file://host/path: the host goes
        std::string path;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '%' && i + 2 < line.size() && std::isxdigit(static_cast<unsigned char>(line[i + 1]))
                && std::isxdigit(static_cast<unsigned char>(line[i + 2])))
                path += char(std::stoi(line.substr(i + 1, 2), nullptr, 16)), i += 2;
            else path += line[i];
        }
        paths.push_back(path);
    }
    return paths;
}

static void xdndMessage(Window *window, ::Window to, Atom type, long l1, long l2, long l3, long l4, ::Window from)
{
    XEvent message = {};
    message.xclient.type = ClientMessage;
    message.xclient.display = window->display;
    message.xclient.window = to;
    message.xclient.message_type = type;
    message.xclient.format = 32;
    message.xclient.data.l[0] = long(from);
    message.xclient.data.l[1] = l1, message.xclient.data.l[2] = l2, message.xclient.data.l[3] = l3, message.xclient.data.l[4] = l4;
    XSendEvent(window->display, to, False, NoEventMask, &message);
    XFlush(window->display);
}

// A window's XdndProxy, where its messages go, or the window itself.
static ::Window xdndReceiver(Window *window, ::Window target)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = nullptr;
    ::Window proxy = 0;
    if (XGetWindowProperty(window->display, target, window->xdndProxy, 0, 1, False, XA_WINDOW, &type, &format, &count, &after,
                           &data) == Success && data && count == 1)
        proxy = *reinterpret_cast<::Window *>(data);
    if (data) XFree(data);
    return proxy ? proxy : target;
}

static bool xdndAware(Window *window, ::Window w)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = nullptr;
    const bool aware = XGetWindowProperty(window->display, w, window->xdndAware, 0, 1, False, XA_ATOM, &type, &format, &count,
                                          &after, &data) == Success && data && count == 1;
    if (data) XFree(data);
    return aware;
}

// The outermost XDND-aware window under the pointer, as sources find it;
// none over our own (dropping on ourselves isn't a drag out).
static ::Window xdndTargetUnderPointer(Window *window)
{
    ::Window w = DefaultRootWindow(window->display), root, child;
    int rootX, rootY, x, y;
    unsigned int mask;
    while (XQueryPointer(window->display, w, &root, &child, &rootX, &rootY, &x, &y, &mask) && child)
    {
        w = child;
        if (xdndAware(window, w)) return w == window->parent || w == window->window ? 0 : w;
    }
    return 0;
}

// Files dragged in: what the view says to where they are now.
static void dropStatus(Window *window)
{
    const bool taken = !window->dropping.empty()
                    && window->callbacks.files(View::FileDrag::over, float(window->dropX), float(window->dropY), window->dropping);
    xdndMessage(window, window->dropSource, window->xdndStatus, taken ? 3 : 2, 0, 0, taken ? long(window->xdndCopy) : None,
                window->dropTarget); // 2: send every position, as the view's targets are small
}

static void dropNow(Window *window)
{
    const bool used = !window->dropping.empty()
                   && window->callbacks.files(View::FileDrag::drop, float(window->dropX), float(window->dropY), window->dropping);
    xdndMessage(window, window->dropSource, window->xdndFinished, used ? 1 : 0, used ? long(window->xdndCopy) : None, 0, 0,
                window->dropTarget);
    window->dropping.clear(), window->dropPending = false;
}

// The XDND messages: a drag coming in (enter, position, leave, drop) and
// replies to ours going out (status, finished).
static void xdndEvent(Window *window, const XClientMessageEvent &message)
{
    const long *l = message.data.l;
    if (message.message_type == window->xdndEnter)
    {
        // The list of files comes as XdndSelection, asked for now.
        window->dropSource = ::Window(l[0]), window->dropTarget = message.window;
        window->dropping.clear(), window->dropPending = false;
        XConvertSelection(window->display, window->xdndSelection, window->uriList, window->xdndSelection, window->window, CurrentTime);
        XFlush(window->display);
    }
    else if (message.message_type == window->xdndPosition && ::Window(l[0]) == window->dropSource)
    {
        ::Window child;
        XTranslateCoordinates(window->display, DefaultRootWindow(window->display), window->window, int(l[2] >> 16),
                              int(l[2] & 0xFFFF), &window->dropX, &window->dropY, &child);
        dropStatus(window);
    }
    else if (message.message_type == window->xdndLeave && ::Window(l[0]) == window->dropSource)
    {
        window->callbacks.files(View::FileDrag::leave, 0, 0, {});
        window->dropping.clear(), window->dropSource = 0;
    }
    else if (message.message_type == window->xdndDrop && ::Window(l[0]) == window->dropSource)
    {
        if (window->dropping.empty()) window->dropPending = true; // the list is still on its way
        else dropNow(window);
    }
    else if (message.message_type == window->xdndStatus && ::Window(l[0]) == window->dragTo)
        window->dragTaken = l[1] & 1;
}

// Dragging files out, the pointer moved: tell the XDND-aware window under it
// where (entering and leaving as it changes).
static void dragMotion(Window *window, const XMotionEvent &motion)
{
    const ::Window under = xdndTargetUnderPointer(window);
    if (under != window->dragTo)
    {
        if (window->dragTo) xdndMessage(window, xdndReceiver(window, window->dragTo), window->xdndLeave, 0, 0, 0, 0, window->window);
        window->dragTo = under, window->dragTaken = false;
        if (under) xdndMessage(window, xdndReceiver(window, under), window->xdndEnter, 5L << 24, long(window->uriList), None, None, window->window);
    }
    if (window->dragTo)
        xdndMessage(window, xdndReceiver(window, window->dragTo), window->xdndPosition, 0, long(motion.x_root) << 16 | (motion.y_root & 0xFFFF),
                    long(motion.time), long(window->xdndCopy), window->window);
}

static void answerSelection(Window *window, const XSelectionRequestEvent &request)
{
    XSelectionEvent reply = {};
    reply.type = SelectionNotify;
    reply.requestor = request.requestor, reply.selection = request.selection;
    reply.target = request.target, reply.time = request.time;
    const Atom property = request.property ? request.property : request.target; // pre-ICCCM clients
    if (request.selection == window->xdndSelection) // files we're dragging out
    {
        if (request.target == window->targets)
        {
            XChangeProperty(window->display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(&window->uriList), 1);
            reply.property = property;
        }
        else if (request.target == window->uriList)
        {
            const std::string list = uriList(window->dragging);
            XChangeProperty(window->display, request.requestor, property, window->uriList, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(list.data()), int(list.size()));
            reply.property = property;
        }
    }
    else if (request.target == window->targets)
    {
        const Atom offered[] = {window->targets, window->utf8, XA_STRING};
        XChangeProperty(window->display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(offered), 3);
        reply.property = property;
    }
    else if (request.target == window->utf8 || request.target == XA_STRING)
    {
        XChangeProperty(window->display, request.requestor, property, request.target, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(window->clipboard.data()), int(window->clipboard.size()));
        reply.property = property;
    }
    XSendEvent(window->display, request.requestor, False, 0, reinterpret_cast<XEvent *>(&reply));
    XFlush(window->display);
}

// Asks the owner for CLIPBOARD as UTF-8 and waits up to 100 ms for it.
static std::string clipboardText(Window *window)
{
    if (XGetSelectionOwner(window->display, window->clipboardAtom) == window->window) return window->clipboard;
    XConvertSelection(window->display, window->clipboardAtom, window->utf8, window->pasted, window->window, CurrentTime);
    XFlush(window->display);
    std::vector<XEvent> others; // a drop's file list, say: back in the queue afterwards
    struct PutBack
    {
        Window *window;
        std::vector<XEvent> &events;
        ~PutBack() { for (XEvent &e : events) XPutBackEvent(window->display, &e); }
    } putBack{window, others};
    for (int tries = 0; tries < 50; ++tries, usleep(2000))
    {
        XEvent event;
        if (!XCheckTypedWindowEvent(window->display, window->window, SelectionNotify, &event)) continue;
        if (event.xselection.selection != window->clipboardAtom)
        {
            others.push_back(event);
            continue;
        }
        if (!event.xselection.property) return {};
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char *data = nullptr;
        XGetWindowProperty(window->display, window->window, window->pasted, 0, 1 << 20, True, AnyPropertyType, &type,
                           &format, &count, &after, &data);
        std::string text = data && format == 8 ? std::string(reinterpret_cast<char *>(data), count) : std::string();
        if (data) XFree(data);
        return text;
    }
    return {};
}

// Ctrl+Z, Ctrl+Shift+Z or Y, A, X, C and V, for a text field being edited.
static bool shortcut(Window *window, const XKeyEvent &event)
{
    if (!(event.state & ControlMask) || (event.state & Mod1Mask)) return false;
    View::Key key;
    switch (XLookupKeysym(const_cast<XKeyEvent *>(&event), 0))
    {
    case XK_v:
    {
        const std::string text = clipboardText(window);
        return !text.empty() && window->callbacks.text(text.c_str());
    }
    case XK_z: key = event.state & ShiftMask ? View::Key::redo : View::Key::undo; break;
    case XK_y: key = View::Key::redo; break;
    case XK_a: key = View::Key::selectAll; break;
    case XK_x: key = View::Key::cut; break;
    case XK_c: key = View::Key::copy; break;
    default: return false;
    }
    return window->callbacks.key(key, modifiers(event.state));
}

void pumpEvents(Window *window)
{
    while (XPending(window->display))
    {
        XEvent event;
        XNextEvent(window->display, &event);
        if (event.type == SelectionRequest) answerSelection(window, event.xselectionrequest);
        if (event.type == ClientMessage) xdndEvent(window, event.xclient);
        if (event.type == Expose) window->callbacks.damaged(); // uncovered, without a compositor to keep it
        if (event.type == SelectionNotify && event.xselection.selection == window->xdndSelection) // a drop's file list
        {
            window->dropping.clear();
            if (event.xselection.property)
            {
                Atom type;
                int format;
                unsigned long count, after;
                unsigned char *data = nullptr;
                XGetWindowProperty(window->display, window->window, event.xselection.property, 0, 1 << 20, True, AnyPropertyType,
                                   &type, &format, &count, &after, &data);
                if (data && format == 8) window->dropping = pathsIn(std::string(reinterpret_cast<char *>(data), count));
                if (data) XFree(data);
            }
            if (window->dropPending) dropNow(window);
            else if (window->dropSource) dropStatus(window);
        }
        if (event.type == SelectionClear) window->clipboard.clear();
        if (event.type == LeaveNotify && !window->locked)
            window->callbacks.pointer({View::Pointer::leave, window->x, window->y, modifiers(event.xcrossing.state)});
        if (event.type == MotionNotify)
        {
            const int x = event.xmotion.x, y = event.xmotion.y;
            if (!window->locked)
                window->x = float(x), window->y = float(y);
            else if (x == window->anchorX && y == window->anchorY)
                continue; // our own warp back to the anchor
            else
            {
                // Move by how far the cursor got from the anchor, then put it back.
                window->x += float(x - window->anchorX), window->y += float(y - window->anchorY);
                XWarpPointer(window->display, 0, window->window, 0, 0, 0, 0, window->anchorX, window->anchorY);
            }
            window->callbacks.pointer({View::Pointer::move, window->x, window->y, modifiers(event.xmotion.state)});
            if (window->dragActive) dragMotion(window, event.xmotion);
        }
        else if (event.type == ButtonPress && (event.xbutton.button == Button4 || event.xbutton.button == Button5))
            window->callbacks.pointer({View::Pointer::wheel, float(event.xbutton.x), float(event.xbutton.y),
                                       modifiers(event.xbutton.state), event.xbutton.button == Button4 ? 1.0f : -1.0f});
        else if ((event.type == ButtonPress || event.type == ButtonRelease) && event.xbutton.button == Button1)
        {
            if (!window->locked) window->x = float(event.xbutton.x), window->y = float(event.xbutton.y);
            if (event.type == ButtonRelease && window->dragActive) // let go: drop on the window under it, or don't
            {
                if (window->dragTo)
                    xdndMessage(window, xdndReceiver(window, window->dragTo), window->dragTaken ? window->xdndDrop : window->xdndLeave,
                                0, long(event.xbutton.time), 0, 0, window->window);
                window->dragActive = false, window->dragTo = 0;
            }
            window->callbacks.pointer({event.type == ButtonPress ? View::Pointer::down : View::Pointer::up,
                                       window->x, window->y, modifiers(event.xbutton.state)});
        }
        else if (event.type == KeyPress)
        {
            if (shortcut(window, event.xkey)) continue;
            View::Key key;
            switch (XLookupKeysym(&event.xkey, 0))
            {
            case XK_Left: key = View::Key::left; break;
            case XK_Right: key = View::Key::right; break;
            case XK_Up: key = View::Key::up; break;
            case XK_Down: key = View::Key::down; break;
            case XK_Page_Up: key = View::Key::pageUp; break;
            case XK_Page_Down: key = View::Key::pageDown; break;
            case XK_Home: key = View::Key::home; break;
            case XK_End: key = View::Key::end; break;
            case XK_Tab: case XK_ISO_Left_Tab: key = View::Key::tab; break;
            case XK_Return: case XK_KP_Enter: key = View::Key::enter; break;
            case XK_Escape: key = View::Key::escape; break;
            case XK_BackSpace: key = View::Key::backspace; break;
            case XK_Delete: key = View::Key::deleteForward; break;
            default:
            {
                // Typed characters (Latin-1 without an input method, which is
                // enough for numbers). X11 has no simple way to hand unused
                // keys back to the host.
                char typed[8] = {};
                if (XLookupString(&event.xkey, typed, sizeof(typed) - 1, nullptr, nullptr) == 1
                    && static_cast<unsigned char>(typed[0]) >= 0x20 && typed[0] != 0x7F && !(event.xkey.state & ControlMask))
                {
                    const auto c = static_cast<unsigned char>(typed[0]);
                    char utf8[3] = {char(c)};
                    if (c >= 0x80) utf8[0] = char(0xC0 | c >> 6), utf8[1] = char(0x80 | (c & 0x3F));
                    window->callbacks.text(utf8);
                }
                continue;
            }
            }
            window->callbacks.key(key, modifiers(event.xkey.state));
        }
    }
}

}

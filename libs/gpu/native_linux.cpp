#include "native.h"
#include "accesskit_tree.h" // before Xlib's macros
#include <X11/extensions/Xrandr.h>
#include <X11/Xlib.h> // after webgpu_cpp.h: Xlib defines macros such as Status and None
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xutil.h> // XLookupString
#include <poll.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// A child X11 window in the host's window. Dawn draws into it with Vulkan.
// Linux hosts have no shared event loop, so the host's timer (needsTimer)
// drives both our X events and frames.
namespace gpu::platform
{

const Api windowApi = Api::x11;
const bool needsTimer = true;

struct Window
{
    Display* display = nullptr; // our own connection, so we see only our events
    ::Window window = 0;
    ::Window parent = 0;  // the host's, which gets the keyboard back after a text edit
    bool focused = false; // we took the keyboard for a text field
    Callbacks callbacks;
    bool locked = false;
    int anchorX = 0, anchorY = 0; // where the cursor is held while locked
    float x = 0, y = 0;           // pointer position reported to the view (pixels)
    Cursor blank = 0;             // an invisible cursor, made on first lock
    Cursor shapes[7] = {};        // setCursor's, each made on first use
    std::string clipboard;        // what we've copied, while we own CLIPBOARD
    Atom clipboardAtom = 0, utf8 = 0, targets = 0, pasted = 0;
    float scale = 1; // see desktopScale
    // XDND, the X drag-and-drop protocol, both ways.
    Atom xdndAware = 0, xdndProxy = 0, xdndEnter = 0, xdndPosition = 0, xdndStatus = 0, xdndLeave = 0, xdndDrop = 0,
         xdndFinished = 0, xdndSelection = 0, xdndCopy = 0, uriList = 0;
    std::vector<std::string> dropping; // files dragged in, once their list has come
    ::Window dropSource = 0;           // the window dragging them in, and the one it addressed
    ::Window dropTarget = 0;
    int dropX = 0, dropY = 0;                 // pixels in the view
    bool dropPending = false;                 // XdndDrop came before the list
    bool filesAsked = false;                  // the list has been asked for
    std::vector<std::string> dragging;        // files dragged out, offered as XdndSelection
    bool dragActive = false;                  // the button is still down
    ::Window dragTo = 0;                      // the XDND-aware window under the pointer
    bool dragTaken = false;                   // which says it would take them
    std::vector<::Window> proxies;            // windows we marked XdndAware, sending their drops to us
    accesskit_unix_adapter* access = nullptr; // screen readers, through AT-SPI
    uintptr_t accessId = 0;                   // its inbox, below
    ::Window outer = 0;                       // our outermost ancestor below the root: the host's frame
    // The input method, where there is one: keys as UTF-8 text, composed
    // (dead keys, Compose, IBus). Without, keys give Latin-1 (XLookupString).
    XIM im = nullptr;
    XIC ic = nullptr;
};

// Screen readers: AccessKit calls from a thread of its own, and may still
// for a moment after a window is gone. So its callbacks find the window's
// inbox by number, under a lock: the tree to start a screen reader on, and
// its requests, which pumpEvents hands the view on the main thread.
struct AccessRequest
{
    uint64_t id;
    Accessible::Action kind;
    double value;
};
struct AccessInbox
{
    Accessibility tree; // the view's, as screen readers last had it
    std::vector<AccessRequest> requests;
};
static std::mutex accessLock;
static std::map<uintptr_t, AccessInbox> accessInboxes;
static uintptr_t accessIds = 0;

static accesskit_tree_update* accessStart (void* id)
{
    std::lock_guard lock (accessLock);
    const auto inbox = accessInboxes.find (uintptr_t (id));
    return inbox != accessInboxes.end() ? accesskit::update (inbox->second.tree) : nullptr;
}

static void accessAction (accesskit_action_request* request, void* id)
{
    uint64_t node;
    Accessible::Action kind;
    double value;
    if (!accesskit::action (request, node, kind, value))
        return;
    std::lock_guard lock (accessLock);
    if (const auto inbox = accessInboxes.find (uintptr_t (id)); inbox != accessInboxes.end())
        inbox->second.requests.push_back ({ node, kind, value });
}

static void accessStop (void*) {}

static accesskit_tree_update* wholeTree (void* tree)
{
    return accesskit::update (*static_cast<const Accessibility*> (tree));
}

void accessibility (Window* window, const Accessibility& tree)
{
    {
        std::lock_guard lock (accessLock); // not held into AccessKit, which holds its own while it calls accessStart
        accessInboxes[window->accessId].tree = tree;
    }
    accesskit_unix_adapter_update_if_active (window->access, wholeTree, const_cast<Accessibility*> (&tree));
}

// Where our window is on the screen, for the bounds screen readers show.
static void accessBounds (Window* window)
{
    XWindowAttributes attributes;
    int x, y;
    ::Window child;
    if (!XGetWindowAttributes (window->display, window->window, &attributes)
        || !XTranslateCoordinates (
            window->display, window->window, DefaultRootWindow (window->display), 0, 0, &x, &y, &child))
        return;
    const accesskit_rect bounds = { double (x), double (y), double (x + attributes.width),
        double (y + attributes.height) };
    accesskit_unix_adapter_set_root_window_bounds (window->access, bounds, bounds);
}

// Whether the host's window is the active one: the keyboard focus is in it.
static void accessFocus (Window* window)
{
    ::Window focus, root, up, *children;
    int revert;
    unsigned int count;
    XGetInputFocus (window->display, &focus, &revert);
    while (focus > PointerRoot && focus != window->outer && focus != DefaultRootWindow (window->display)
           && XQueryTree (window->display, focus, &root, &up, &children, &count))
    {
        if (children)
            XFree (children);
        focus = up;
    }
    accesskit_unix_adapter_update_window_focus_state (window->access, focus == window->outer);
}

// Our window moves with the host's, and the host's inside the window
// manager's frame: we watch each window above ours for moves
// (ConfigureNotify), new parents (ReparentNotify: watched again) and the
// keyboard focus.
static void watchAncestors (Window* window)
{
    const ::Window top = DefaultRootWindow (window->display);
    for (::Window w = window->parent, root, up, *children; w && w != top; w = up)
    {
        XSelectInput (window->display, w, StructureNotifyMask | FocusChangeMask);
        window->outer = w;
        unsigned int count = 0;
        if (!XQueryTree (window->display, w, &root, &up, &children, &count))
            break;
        if (children)
            XFree (children);
    }
}

static ::Window xdndReceiver (Window* window, ::Window target);

// The desktop's scale: Xft.dpi (GNOME and KDE set it from their scaling
// setting) over 96, times GDK_SCALE where set. X11 has no per-monitor scale
// to ask; a host that knows better says so.
static float desktopScale (Display* display)
{
    float scale = 1;
    if (const char* resources = XResourceManagerString (display))
        if (const char* dpi = std::strstr (resources, "Xft.dpi:"))
            if (const float value = std::strtof (dpi + 8, nullptr); value >= 48 && value <= 480)
                scale = value / 96;
    if (const char* gdk = std::getenv ("GDK_SCALE"))
        if (const int factor = std::atoi (gdk); factor >= 1 && factor <= 4)
            scale *= float (factor);
    return scale;
}

// X errors on our own connections (BadWindow once the host has destroyed our
// parent window, say) are ignored; Xlib's default handler would exit the
// host. Others go on to the handler that was there before.
static std::vector<Display*> ourDisplays;
static XErrorHandler hostHandler = nullptr;

static int ignoreOurErrors (Display* display, XErrorEvent* error)
{
    if (std::find (ourDisplays.begin(), ourDisplays.end(), display) != ourDisplays.end())
        return 0;
    return hostHandler ? hostHandler (display, error) : 0;
}

Window* create (const Parent& parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    Display* display = XOpenDisplay (nullptr);
    if (!display || !parent.handle)
    {
        if (display)
            XCloseDisplay (display);
        return nullptr;
    }
    if (ourDisplays.empty())
        hostHandler = XSetErrorHandler (ignoreOurErrors);
    ourDisplays.push_back (display);
    auto* window = new Window;
    window->display = display;
    window->callbacks = std::move (callbacks);
    window->scale = desktopScale (display);
    window->parent = ::Window (uintptr_t (parent.handle));
    window->window = XCreateSimpleWindow (display, window->parent, 0, 0, width, height, 0, 0, 0);
    window->clipboardAtom = XInternAtom (display, "CLIPBOARD", False);
    window->utf8 = XInternAtom (display, "UTF8_STRING", False);
    window->targets = XInternAtom (display, "TARGETS", False);
    window->pasted = XInternAtom (display, "GPU_PASTED", False); // where a paste is delivered
    for (auto [atom, name] : { std::pair { &window->xdndAware, "XdndAware" }, { &window->xdndProxy, "XdndProxy" },
             { &window->xdndEnter, "XdndEnter" }, { &window->xdndPosition, "XdndPosition" },
             { &window->xdndStatus, "XdndStatus" }, { &window->xdndLeave, "XdndLeave" },
             { &window->xdndDrop, "XdndDrop" }, { &window->xdndFinished, "XdndFinished" },
             { &window->xdndSelection, "XdndSelection" }, { &window->xdndCopy, "XdndActionCopy" },
             { &window->uriList, "text/uri-list" } })
        *atom = XInternAtom (display, name, False);
    // Drag sources look for XdndAware on a window above ours: the one the host
    // made for us (Visage marks that), or, as the XDND spec has it, the
    // top-level window (GTK 4 looks only there). We mark both with XdndProxy
    // sending their messages on to us, the top level only when the host
    // doesn't take drops there itself.
    const long version = 5;
    auto hasProperty = [&] (::Window w, Atom name) {
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char* data = nullptr;
        const bool has =
            XGetWindowProperty (display, w, name, 0, 1, False, AnyPropertyType, &type, &format, &count, &after, &data)
                == Success
            && data && count > 0;
        if (data)
            XFree (data);
        return has;
    };
    auto proxy = [&] (::Window w) {
        XChangeProperty (display, w, window->xdndAware, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<const unsigned char*> (&version), 1);
        XChangeProperty (display, w, window->xdndProxy, XA_WINDOW, 32, PropModeReplace,
            reinterpret_cast<const unsigned char*> (&window->window), 1);
        window->proxies.push_back (w);
    };
    // The proxy, ours, says XdndProxy is itself: sources (GTK 4) check that to
    // tell a live proxy from a stale one.
    XChangeProperty (display, window->window, window->xdndAware, XA_ATOM, 32, PropModeReplace,
        reinterpret_cast<const unsigned char*> (&version), 1);
    XChangeProperty (display, window->window, window->xdndProxy, XA_WINDOW, 32, PropModeReplace,
        reinterpret_cast<const unsigned char*> (&window->window), 1);
    proxy (window->parent);
    const Atom wmState = XInternAtom (display, "WM_STATE", False);
    for (::Window w = window->parent, root, up, *children; w && w != DefaultRootWindow (display); w = up)
    {
        if (hasProperty (w, wmState)) // the top level: the host's client window
        {
            if (w != window->parent && !hasProperty (w, window->xdndAware))
                proxy (w);
            break;
        }
        unsigned int count = 0;
        if (!XQueryTree (display, w, &root, &up, &children, &count))
            break;
        if (children)
            XFree (children);
    }
    // The one XMODIFIERS names (@im=ibus), else Xlib's own. The modifiers
    // are Xlib's, for the whole process: the host's put back afterwards.
    const char* current = XSetLocaleModifiers (nullptr);
    const std::string hostModifiers = current ? current : "";
    if (XSetLocaleModifiers (""))
        window->im = XOpenIM (display, nullptr, nullptr, nullptr);
    if (!window->im && XSetLocaleModifiers ("@im=none"))
        window->im = XOpenIM (display, nullptr, nullptr, nullptr);
    XSetLocaleModifiers (hostModifiers.c_str());
    if (window->im)
        window->ic = XCreateIC (window->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
            window->window, XNFocusWindow, window->window, nullptr);
    long imEvents = 0; // what the input method needs to see besides
    if (window->ic)
        XGetICValues (window->ic, XNFilterEvents, &imEvents, nullptr);
    XSelectInput (display, window->window,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask | KeyPressMask | ExposureMask
            | StructureNotifyMask | imEvents);
    XMapWindow (display, window->window);
    {
        std::lock_guard lock (accessLock);
        window->accessId = ++accessIds;
        accessInboxes[window->accessId];
    }
    void* id = reinterpret_cast<void*> (window->accessId);
    window->access = accesskit_unix_adapter_new (accessStart, id, accessAction, id, accessStop, id);
    watchAncestors (window);
    accessBounds (window);
    accessFocus (window);
    XFlush (display);
    return window;
}

wgpu::Surface createSurface (const wgpu::Instance& instance, Window* window)
{
    wgpu::SurfaceSourceXlibWindow source;
    source.display = window->display;
    source.window = window->window;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    return instance.CreateSurface (&descriptor);
}

void setCursor (Window* window, View::Cursor cursor)
{
    if (cursor == View::Cursor::arrow) // the host window's
    {
        XUndefineCursor (window->display, window->window);
        XFlush (window->display);
        return;
    }
    static const unsigned glyph[7] = { XC_left_ptr, XC_xterm, XC_hand2, XC_fleur, XC_sb_h_double_arrow,
        XC_sb_v_double_arrow, XC_bottom_right_corner };
    ::Cursor& shape = window->shapes[int (cursor)];
    if (!shape)
        shape = XCreateFontCursor (window->display, glyph[int (cursor)]);
    XDefineCursor (window->display, window->window, shape);
    XFlush (window->display);
}

void lockPointer (Window* window, bool locked)
{
    if (window->locked == locked)
        return;
    window->locked = locked;
    if (!locked)
    {
        XUngrabPointer (window->display, CurrentTime);
        XFlush (window->display);
        return;
    }
    if (!window->blank)
    {
        const char bits[1] = { 0 };
        const Pixmap empty = XCreateBitmapFromData (window->display, window->window, bits, 1, 1);
        XColor black = {};
        window->blank = XCreatePixmapCursor (window->display, empty, empty, &black, &black, 0, 0);
        XFreePixmap (window->display, empty);
    }
    window->anchorX = int (window->x), window->anchorY = int (window->y);
    XGrabPointer (window->display, window->window, False, ButtonReleaseMask | Button1MotionMask, GrabModeAsync,
        GrabModeAsync, 0, window->blank, CurrentTime);
    XFlush (window->display);
}

void destroy (Window* window)
{
    accesskit_unix_adapter_free (window->access);
    {
        std::lock_guard lock (accessLock);
        accessInboxes.erase (window->accessId);
    }
    lockPointer (window, false);
    if (window->blank)
        XFreeCursor (window->display, window->blank);
    for (::Cursor shape : window->shapes)
        if (shape)
            XFreeCursor (window->display, shape);
    if (window->ic)
        XDestroyIC (window->ic);
    if (window->im)
        XCloseIM (window->im);
    for (::Window w : window->proxies) // ours to take back, unless something else took them over
        if (xdndReceiver (window, w) == window->window)
        {
            XDeleteProperty (window->display, w, window->xdndAware);
            XDeleteProperty (window->display, w, window->xdndProxy);
        }
    XDestroyWindow (window->display, window->window);
    XSync (window->display, False); // its errors, while they're still ours to ignore
    std::erase (ourDisplays, window->display);
    XCloseDisplay (window->display);
    if (ourDisplays.empty())
    {
        // Put back the handler we replaced, unless the host has since set its own.
        if (XErrorHandler current = XSetErrorHandler (hostHandler); current != ignoreOurErrors)
            XSetErrorHandler (current);
    }
    delete window;
}

void setSize (Window* window, uint32_t width, uint32_t height)
{
    XResizeWindow (window->display, window->window, width, height);
    XFlush (window->display);
}

void pixelSize (Window* window, uint32_t& width, uint32_t& height)
{
    XWindowAttributes attributes = {};
    XGetWindowAttributes (window->display, window->window, &attributes);
    width = uint32_t (attributes.width), height = uint32_t (attributes.height);
}

// Desktops have no common, cheap way to ask; GTK_THEME=Name:light or
// Name:dark is the one convention. Dark otherwise, as most audio software is.
bool isDarkMode (Window*)
{
    const char* theme = std::getenv ("GTK_THEME");
    return !(theme && std::strstr (theme, ":light"));
}

// The refresh rate of the monitor under the window's middle, from XRandR's
// current mode (as JUCE paces its Linux frames); 0 if it can't tell.
float refreshRate (Window* window)
{
    Display* display = window->display;
    const ::Window root = DefaultRootWindow (display);
    XWindowAttributes attributes;
    int x = 0, y = 0;
    ::Window child;
    if (!XGetWindowAttributes (display, window->window, &attributes)
        || !XTranslateCoordinates (
            display, window->window, root, attributes.width / 2, attributes.height / 2, &x, &y, &child))
        return 0;
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent (display, root);
    if (!resources)
        return 0;
    float rate = 0;
    for (int i = 0; i < resources->ncrtc && rate == 0; ++i)
    {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo (display, resources, resources->crtcs[i]);
        if (!crtc)
            continue;
        if (crtc->mode && x >= crtc->x && y >= crtc->y && x < crtc->x + int (crtc->width)
            && y < crtc->y + int (crtc->height))
            for (int m = 0; m < resources->nmode; ++m)
            {
                const XRRModeInfo& mode = resources->modes[m];
                if (mode.id == crtc->mode && mode.hTotal && mode.vTotal)
                    rate = float (double (mode.dotClock) / (double (mode.hTotal) * mode.vTotal));
            }
        XRRFreeCrtcInfo (crtc);
    }
    XRRFreeScreenResources (resources);
    return rate;
}

float scale (Window* window)
{
    if (window)
        return window->scale;
    Display* display = XOpenDisplay (nullptr); // before the view has a window: the default screen's
    const float scale = display ? desktopScale (display) : 1.0f;
    if (display)
        XCloseDisplay (display);
    return scale;
}

void setVisible (Window* window, bool visible)
{
    if (visible)
        XMapWindow (window->display, window->window);
    else
        XUnmapWindow (window->display, window->window);
    XFlush (window->display);
}

static unsigned modifiers (unsigned state)
{
    return (state & ShiftMask ? View::shift : 0u) | (state & ControlMask ? View::control : 0u)
           | (state & Mod1Mask ? View::alt : 0u);
}

// X11's clipboard is a conversation: the owner answers each request for it.
// The keyboard is ours only while a text field is edited: a click alone
// doesn't take it, so the host's shortcuts (Space plays) keep working.
void textInput (Window* window, bool editing, View::Caret)
{
    if (editing == window->focused)
        return;
    window->focused = editing;
    if (window->ic)
        editing ? XSetICFocus (window->ic) : XUnsetICFocus (window->ic);
    XSetInputFocus (window->display, editing ? window->window : window->parent, RevertToParent, CurrentTime);
    XFlush (window->display);
}

bool onScreen (Window* window)
{
    XWindowAttributes attributes;
    return XGetWindowAttributes (window->display, window->window, &attributes) && attributes.map_state == IsViewable;
}

// Files dragged out: we own XdndSelection while the button is down, and
// pumpEvents tells the window under the pointer (see dragMotion).
void dragFiles (Window* window, const std::vector<std::string>& paths)
{
    window->dragging = paths;
    window->dragActive = true;
    window->dragTo = 0, window->dragTaken = false;
    XSetSelectionOwner (window->display, window->xdndSelection, window->window, CurrentTime);
    XFlush (window->display);
}

void copyText (Window* window, const std::string& utf8)
{
    window->clipboard = utf8;
    XSetSelectionOwner (window->display, window->clipboardAtom, window->window, CurrentTime);
    XFlush (window->display);
}

// text/uri-list, the form XDND passes files in: file:// URIs, %-escaped, one per line.
static std::string uriList (const std::vector<std::string>& paths)
{
    std::string list;
    for (const std::string& path : paths)
    {
        list += "file://";
        for (const unsigned char c : path)
        {
            if (std::isalnum (c) || std::strchr ("/-._~", c))
                list += char (c);
            else
            {
                char escaped[4];
                std::snprintf (escaped, sizeof (escaped), "%%%02X", c);
                list += escaped;
            }
        }
        list += "\r\n";
    }
    return list;
}

static std::vector<std::string> pathsIn (const std::string& list)
{
    std::vector<std::string> paths;
    size_t at = 0;
    while (at < list.size())
    {
        size_t end = list.find ('\n', at);
        if (end == std::string::npos)
            end = list.size();
        std::string line = list.substr (at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind ("file://", 0) != 0)
            continue;                       // comments, other schemes
        line.erase (0, line.find ('/', 7)); // file://host/path: the host goes
        std::string path;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '%' && i + 2 < line.size() && std::isxdigit (static_cast<unsigned char> (line[i + 1]))
                && std::isxdigit (static_cast<unsigned char> (line[i + 2])))
                path += char (std::stoi (line.substr (i + 1, 2), nullptr, 16)), i += 2;
            else
                path += line[i];
        }
        paths.push_back (path);
    }
    return paths;
}

static void xdndMessage (Window* window, ::Window to, Atom type, long l1, long l2, long l3, long l4, ::Window from)
{
    XEvent message = {};
    message.xclient.type = ClientMessage;
    message.xclient.display = window->display;
    message.xclient.window = to;
    message.xclient.message_type = type;
    message.xclient.format = 32;
    message.xclient.data.l[0] = long (from);
    message.xclient.data.l[1] = l1, message.xclient.data.l[2] = l2, message.xclient.data.l[3] = l3,
    message.xclient.data.l[4] = l4;
    XSendEvent (window->display, to, False, NoEventMask, &message);
    XFlush (window->display);
}

// A window's XdndProxy, where its messages go, or the window itself.
static ::Window xdndReceiver (Window* window, ::Window target)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char* data = nullptr;
    ::Window proxy = 0;
    if (XGetWindowProperty (
            window->display, target, window->xdndProxy, 0, 1, False, XA_WINDOW, &type, &format, &count, &after, &data)
            == Success
        && data && count == 1)
        proxy = *reinterpret_cast<::Window*> (data);
    if (data)
        XFree (data);
    return proxy ? proxy : target;
}

static bool xdndAware (Window* window, ::Window w)
{
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char* data = nullptr;
    const bool aware = XGetWindowProperty (window->display, w, window->xdndAware, 0, 1, False, XA_ATOM, &type, &format,
                           &count, &after, &data)
                           == Success
                       && data && count == 1;
    if (data)
        XFree (data);
    return aware;
}

// The outermost XDND-aware window under the pointer, as sources find it;
// none over our own (dropping on ourselves isn't a drag out).
static ::Window xdndTargetUnderPointer (Window* window)
{
    ::Window w = DefaultRootWindow (window->display), root, child;
    int rootX, rootY, x, y;
    unsigned int mask;
    while (XQueryPointer (window->display, w, &root, &child, &rootX, &rootY, &x, &y, &mask) && child)
    {
        w = child;
        if (xdndAware (window, w))
            return w == window->parent || w == window->window ? 0 : w;
    }
    return 0;
}

// Files dragged in: what the view says to where they are now.
static void dropStatus (Window* window)
{
    const bool taken = !window->dropping.empty()
                       && window->callbacks.files (
                           View::FileDrag::over, float (window->dropX), float (window->dropY), window->dropping);
    xdndMessage (window, window->dropSource, window->xdndStatus, taken ? 3 : 2, 0, 0,
        taken ? long (window->xdndCopy) : None,
        window->dropTarget); // 2: send every position, as the view's targets are small
}

static void dropNow (Window* window)
{
    const bool used = !window->dropping.empty()
                      && window->callbacks.files (
                          View::FileDrag::drop, float (window->dropX), float (window->dropY), window->dropping);
    xdndMessage (window, window->dropSource, window->xdndFinished, used ? 1 : 0, used ? long (window->xdndCopy) : None,
        0, 0, window->dropTarget);
    window->dropping.clear(), window->dropPending = false;
}

// The XDND messages: a drag coming in (enter, position, leave, drop) and
// replies to ours going out (status, finished).
static void xdndEvent (Window* window, const XClientMessageEvent& message)
{
    const long* l = message.data.l;
    // The list of files comes as XdndSelection, asked for with the time of a
    // position or the drop (as the spec has it; GTK 4 refuses CurrentTime).
    auto askForFiles = [&] (Time time) {
        window->filesAsked = true;
        XConvertSelection (
            window->display, window->xdndSelection, window->uriList, window->xdndSelection, window->window, time);
        XFlush (window->display);
    };
    if (message.message_type == window->xdndEnter)
    {
        window->dropSource = ::Window (l[0]), window->dropTarget = message.window;
        window->dropping.clear(), window->dropPending = false, window->filesAsked = false;
    }
    else if (message.message_type == window->xdndPosition && ::Window (l[0]) == window->dropSource)
    {
        if (!window->filesAsked)
            askForFiles (Time (l[3]));
        // Where the pointer is over us, from the server: some sources (GTK 4
        // under Mutter) report a position off by their window's frame.
        ::Window root, child;
        int rootX, rootY;
        unsigned int mask;
        XQueryPointer (
            window->display, window->window, &root, &child, &rootX, &rootY, &window->dropX, &window->dropY, &mask);
        dropStatus (window);
    }
    else if (message.message_type == window->xdndLeave && ::Window (l[0]) == window->dropSource)
    {
        window->callbacks.files (View::FileDrag::leave, 0, 0, {});
        window->dropping.clear(), window->dropSource = 0;
    }
    else if (message.message_type == window->xdndDrop && ::Window (l[0]) == window->dropSource)
    {
        if (window->dropping.empty()) // the list is still on its way
        {
            window->dropPending = true;
            askForFiles (Time (l[2]));
        }
        else
            dropNow (window);
    }
    else if (message.message_type == window->xdndStatus && ::Window (l[0]) == window->dragTo)
        window->dragTaken = l[1] & 1;
}

// Dragging files out, the pointer moved: tell the XDND-aware window under it
// where (entering and leaving as it changes).
static void dragMotion (Window* window, const XMotionEvent& motion)
{
    const ::Window under = xdndTargetUnderPointer (window);
    if (under != window->dragTo)
    {
        if (window->dragTo)
            xdndMessage (window, xdndReceiver (window, window->dragTo), window->xdndLeave, 0, 0, 0, 0, window->window);
        window->dragTo = under, window->dragTaken = false;
        if (under)
            xdndMessage (window, xdndReceiver (window, under), window->xdndEnter, 5L << 24, long (window->uriList),
                None, None, window->window);
    }
    if (window->dragTo)
        xdndMessage (window, xdndReceiver (window, window->dragTo), window->xdndPosition, 0,
            long (motion.x_root) << 16 | (motion.y_root & 0xFFFF), long (motion.time), long (window->xdndCopy),
            window->window);
}

// Latin-1, X11's STRING, from UTF-8 and back; false for text beyond it.
static bool toLatin1 (const std::string& utf8, std::string& latin1)
{
    for (size_t i = 0; i < utf8.size(); ++i)
    {
        const auto c = static_cast<unsigned char> (utf8[i]);
        if (c < 0x80)
            latin1 += char (c);
        else if ((c == 0xC2 || c == 0xC3) && i + 1 < utf8.size()
                 && (static_cast<unsigned char> (utf8[i + 1]) & 0xC0) == 0x80)
            latin1 += char ((c & 0x03) << 6 | (utf8[++i] & 0x3F));
        else
            return false;
    }
    return true;
}

static std::string fromLatin1 (const char* latin1, size_t length)
{
    std::string utf8;
    for (size_t i = 0; i < length; ++i)
    {
        const auto c = static_cast<unsigned char> (latin1[i]);
        if (c < 0x80)
            utf8 += char (c);
        else
            utf8 += char (0xC0 | c >> 6), utf8 += char (0x80 | (c & 0x3F));
    }
    return utf8;
}

static void answerSelection (Window* window, const XSelectionRequestEvent& request)
{
    XSelectionEvent reply = {};
    reply.type = SelectionNotify;
    reply.requestor = request.requestor, reply.selection = request.selection;
    reply.target = request.target, reply.time = request.time;
    const Atom property = request.property ? request.property : request.target; // pre-ICCCM clients
    if (request.selection == window->xdndSelection)                             // files we're dragging out
    {
        if (request.target == window->targets)
        {
            XChangeProperty (window->display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                reinterpret_cast<const unsigned char*> (&window->uriList), 1);
            reply.property = property;
        }
        else if (request.target == window->uriList)
        {
            const std::string list = uriList (window->dragging);
            XChangeProperty (window->display, request.requestor, property, window->uriList, 8, PropModeReplace,
                reinterpret_cast<const unsigned char*> (list.data()), int (list.size()));
            reply.property = property;
        }
    }
    else if (request.target == window->targets)
    {
        const Atom offered[] = { window->targets, window->utf8, XA_STRING };
        XChangeProperty (window->display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<const unsigned char*> (offered), 3);
        reply.property = property;
    }
    else if (request.target == window->utf8 || request.target == XA_STRING)
    {
        // STRING is Latin-1: declined (no property) for text beyond it.
        std::string latin1;
        const std::string* text = &window->clipboard;
        if (request.target == XA_STRING)
            text = toLatin1 (window->clipboard, latin1) ? &latin1 : nullptr;
        if (text)
        {
            XChangeProperty (window->display, request.requestor, property, request.target, 8, PropModeReplace,
                reinterpret_cast<const unsigned char*> (text->data()), int (text->size()));
            reply.property = property;
        }
    }
    XSendEvent (window->display, request.requestor, False, 0, reinterpret_cast<XEvent*> (&reply));
    XFlush (window->display);
}

// Asks the owner for CLIPBOARD as UTF-8 and waits up to 100 ms for it.
static std::string clipboardText (Window* window)
{
    if (XGetSelectionOwner (window->display, window->clipboardAtom) == window->window)
        return window->clipboard;
    XConvertSelection (
        window->display, window->clipboardAtom, window->utf8, window->pasted, window->window, CurrentTime);
    XFlush (window->display);
    std::vector<XEvent> others; // a drop's file list, say: back in the queue afterwards
    struct PutBack
    {
        Window* window;
        std::vector<XEvent>& events;
        ~PutBack()
        {
            for (XEvent& e : events)
                XPutBackEvent (window->display, &e);
        }
    } putBack { window, others };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (100);
    for (;;)
    {
        XEvent event;
        if (!XCheckTypedWindowEvent (window->display, window->window, SelectionNotify, &event))
        {
            // Nothing yet: sleep until the X connection has something to read.
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                return {};
            pollfd connection = { ConnectionNumber (window->display), POLLIN, 0 };
            poll (&connection, 1, int (left.count()));
            continue;
        }
        if (event.xselection.selection != window->clipboardAtom)
        {
            others.push_back (event);
            continue;
        }
        if (!event.xselection.property)
            return {};
        Atom type;
        int format;
        unsigned long count, after;
        unsigned char* data = nullptr;
        XGetWindowProperty (window->display, window->window, window->pasted, 0, 1 << 20, True, AnyPropertyType, &type,
            &format, &count, &after, &data);
        std::string text = data && format == 8 ? std::string (reinterpret_cast<char*> (data), count) : std::string();
        if (data)
            XFree (data);
        return text;
    }
}

// Ctrl+Z, Ctrl+Shift+Z or Y, A, X, C and V, for a text field being edited.
static bool shortcut (Window* window, const XKeyEvent& event)
{
    if (!(event.state & ControlMask) || (event.state & Mod1Mask))
        return false;
    View::Key key;
    switch (XLookupKeysym (const_cast<XKeyEvent*> (&event), 0))
    {
        case XK_v:
        {
            const std::string text = clipboardText (window);
            return !text.empty() && window->callbacks.text (text.c_str());
        }
        case XK_z:
            key = event.state & ShiftMask ? View::Key::redo : View::Key::undo;
            break;
        case XK_y:
            key = View::Key::redo;
            break;
        case XK_a:
            key = View::Key::selectAll;
            break;
        case XK_x:
            key = View::Key::cut;
            break;
        case XK_c:
            key = View::Key::copy;
            break;
        default:
            return false;
    }
    return window->callbacks.key (key, modifiers (event.state));
}

void pumpEvents (Window* window)
{
    std::vector<AccessRequest> requests;
    {
        std::lock_guard lock (accessLock);
        requests.swap (accessInboxes[window->accessId].requests);
    }
    for (const AccessRequest& r : requests)
        window->callbacks.access (r.id, r.kind, r.value);
    while (XPending (window->display))
    {
        XEvent event;
        XNextEvent (window->display, &event);
        if (XFilterEvent (&event, None))
            continue; // the input method's: a key it composes with
        if (event.type == ReparentNotify && event.xreparent.window != window->window)
            watchAncestors (window);
        if (event.type == ConfigureNotify || event.type == ReparentNotify)
            accessBounds (window);
        if (event.type == FocusIn || event.type == FocusOut)
            accessFocus (window);
        if (event.type == SelectionRequest)
            answerSelection (window, event.xselectionrequest);
        if (event.type == ClientMessage)
            xdndEvent (window, event.xclient);
        if (event.type == Expose)
            window->callbacks.damaged(); // uncovered, without a compositor to keep it
        if (event.type == SelectionNotify && event.xselection.selection == window->xdndSelection) // a drop's file list
        {
            window->dropping.clear();
            if (event.xselection.property)
            {
                Atom type;
                int format;
                unsigned long count, after;
                unsigned char* data = nullptr;
                XGetWindowProperty (window->display, window->window, event.xselection.property, 0, 1 << 20, True,
                    AnyPropertyType, &type, &format, &count, &after, &data);
                if (data && format == 8)
                    window->dropping = pathsIn (std::string (reinterpret_cast<char*> (data), count));
                if (data)
                    XFree (data);
            }
            if (window->dropPending)
                dropNow (window);
            else if (window->dropSource)
                dropStatus (window);
        }
        if (event.type == SelectionClear && event.xselectionclear.selection == window->clipboardAtom)
            window->clipboard.clear();
        if (event.type == LeaveNotify && !window->locked)
            window->callbacks.pointer (
                { View::Pointer::leave, window->x, window->y, modifiers (event.xcrossing.state) });
        if (event.type == MotionNotify)
        {
            const int x = event.xmotion.x, y = event.xmotion.y;
            if (!window->locked)
                window->x = float (x), window->y = float (y);
            else if (x == window->anchorX && y == window->anchorY)
                continue; // our own warp back to the anchor
            else
            {
                // Move by how far the cursor got from the anchor, then put it back.
                window->x += float (x - window->anchorX), window->y += float (y - window->anchorY);
                XWarpPointer (window->display, 0, window->window, 0, 0, 0, 0, window->anchorX, window->anchorY);
            }
            window->callbacks.pointer ({ View::Pointer::move, window->x, window->y, modifiers (event.xmotion.state) });
            if (window->dragActive)
                dragMotion (window, event.xmotion);
        }
        else if (event.type == ButtonPress && (event.xbutton.button == Button4 || event.xbutton.button == Button5))
            window->callbacks.pointer ({ View::Pointer::wheel, float (event.xbutton.x), float (event.xbutton.y),
                modifiers (event.xbutton.state), event.xbutton.button == Button4 ? 1.0f : -1.0f });
        else if ((event.type == ButtonPress || event.type == ButtonRelease) && event.xbutton.button == Button1)
        {
            if (!window->locked)
                window->x = float (event.xbutton.x), window->y = float (event.xbutton.y);
            if (event.type == ButtonRelease && window->dragActive) // let go: drop on the window under it, or don't
            {
                if (window->dragTo)
                    xdndMessage (window, xdndReceiver (window, window->dragTo),
                        window->dragTaken ? window->xdndDrop : window->xdndLeave, 0, long (event.xbutton.time), 0, 0,
                        window->window);
                window->dragActive = false, window->dragTo = 0;
            }
            window->callbacks.pointer ({ event.type == ButtonPress ? View::Pointer::down : View::Pointer::up, window->x,
                window->y, modifiers (event.xbutton.state) });
        }
        else if ((event.type == ButtonPress || event.type == ButtonRelease)
                 && (event.xbutton.button == Button3
                     || event.xbutton.button == Button2)) // the secondary button: a context menu
        {
            if (!window->locked)
                window->x = float (event.xbutton.x), window->y = float (event.xbutton.y);
            window->callbacks.pointer ({ event.type == ButtonPress ? View::Pointer::down : View::Pointer::up, window->x,
                window->y, modifiers (event.xbutton.state), 0, false, event.xbutton.button == Button3 ? 1 : 2 });
        }
        else if (event.type == KeyPress)
        {
            if (shortcut (window, event.xkey))
                continue;
            View::Key key;
            switch (XLookupKeysym (&event.xkey, 0))
            {
                case XK_Left:
                    key = View::Key::left;
                    break;
                case XK_Right:
                    key = View::Key::right;
                    break;
                case XK_Up:
                    key = View::Key::up;
                    break;
                case XK_Down:
                    key = View::Key::down;
                    break;
                case XK_Page_Up:
                    key = View::Key::pageUp;
                    break;
                case XK_Page_Down:
                    key = View::Key::pageDown;
                    break;
                case XK_Home:
                    key = View::Key::home;
                    break;
                case XK_End:
                    key = View::Key::end;
                    break;
                case XK_Tab:
                case XK_ISO_Left_Tab:
                    key = View::Key::tab;
                    break;
                case XK_Return:
                case XK_KP_Enter:
                    key = View::Key::enter;
                    break;
                case XK_Escape:
                    key = View::Key::escape;
                    break;
                case XK_BackSpace:
                    key = View::Key::backspace;
                    break;
                case XK_Delete:
                    key = View::Key::deleteForward;
                    break;
                case XK_Menu:
                    key = View::Key::contextMenu;
                    break;
                case XK_F10:
                    if (!(event.xkey.state & ShiftMask))
                        continue;
                    key = View::Key::contextMenu; // Shift+F10
                    break;
                default:
                {
                    // Typed characters, through the input method where there is
                    // one. X11 has no simple way to hand unused keys back to the host.
                    if (event.xkey.state & ControlMask)
                        continue;
                    char typed[64];
                    std::string text;
                    if (window->ic)
                    {
                        Status status;
                        const int length =
                            Xutf8LookupString (window->ic, &event.xkey, typed, sizeof (typed), nullptr, &status);
                        if (status == XLookupChars || status == XLookupBoth)
                            text.assign (typed, size_t (length));
                    }
                    else
                        text = fromLatin1 (
                            typed, size_t (XLookupString (&event.xkey, typed, sizeof (typed), nullptr, nullptr)));
                    if (!text.empty() && static_cast<unsigned char> (text[0]) >= 0x20 && text[0] != 0x7F)
                        window->callbacks.text (text.c_str());
                    continue;
                }
            }
            window->callbacks.key (key, modifiers (event.xkey.state));
        }
    }
}

} // namespace gpu::platform

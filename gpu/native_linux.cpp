#include "native.h"
#include <X11/Xlib.h> // after webgpu_cpp.h: Xlib defines macros such as Status and None

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
    Callbacks callbacks;
    bool locked = false;
    int anchorX = 0, anchorY = 0; // where the cursor is held while locked
    float x = 0, y = 0;           // pointer position reported to the view (pixels)
    Cursor blank = 0;             // an invisible cursor, made on first lock
};

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks)
{
    Display *display = XOpenDisplay(nullptr);
    if (!display || !parent->x11)
    {
        if (display) XCloseDisplay(display);
        return nullptr;
    }
    auto *window = new Window{display, 0, std::move(callbacks)};
    window->window = XCreateSimpleWindow(display, ::Window(parent->x11), 0, 0, width, height, 0, 0, 0);
    XSelectInput(display, window->window, ButtonPressMask | ButtonReleaseMask | Button1MotionMask);
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
    XDestroyWindow(window->display, window->window);
    XCloseDisplay(window->display);
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

float scale(Window *) { return 1.0f; }

void setVisible(Window *window, bool visible)
{
    if (visible) XMapWindow(window->display, window->window);
    else XUnmapWindow(window->display, window->window);
    XFlush(window->display);
}

void pumpEvents(Window *window)
{
    while (XPending(window->display))
    {
        XEvent event;
        XNextEvent(window->display, &event);
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
            window->callbacks.pointer(View::Pointer::move, window->x, window->y);
        }
        else if ((event.type == ButtonPress || event.type == ButtonRelease) && event.xbutton.button == Button1)
        {
            if (!window->locked) window->x = float(event.xbutton.x), window->y = float(event.xbutton.y);
            window->callbacks.pointer(event.type == ButtonPress ? View::Pointer::down : View::Pointer::up,
                                      window->x, window->y);
        }
    }
}

}

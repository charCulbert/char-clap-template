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

void destroy(Window *window)
{
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
        if (event.type == ButtonPress && event.xbutton.button == Button1)
            window->callbacks.pointer(View::Pointer::down, float(event.xbutton.x), float(event.xbutton.y));
        else if (event.type == MotionNotify)
            window->callbacks.pointer(View::Pointer::move, float(event.xmotion.x), float(event.xmotion.y));
        else if (event.type == ButtonRelease && event.xbutton.button == Button1)
            window->callbacks.pointer(View::Pointer::up, float(event.xbutton.x), float(event.xbutton.y));
    }
}

}

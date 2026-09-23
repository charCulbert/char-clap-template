#pragma once
#include "clap/ext/gui.h"
#include <webgpu/webgpu_cpp.h>
#include <functional>
#include "view.h"

// A child window inside the host's window that WebGPU can draw into. One small
// file per desktop platform: native_mac.mm, native_win.cpp, native_linux.cpp.
namespace gpu::platform {

extern const char *const windowApi; // CLAP_WINDOW_API_COCOA, _WIN32 or _X11
extern const bool needsTimer;       // true where the host's timer drives frames and events

struct Window;
struct Callbacks
{
    std::function<void()> frame;                       // draw one frame
    std::function<void(View::Pointer, float, float)> pointer; // pixels
};

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks);
wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window);
void destroy(Window *window);
void setSize(Window *window, uint32_t width, uint32_t height); // clap.gui units
void pixelSize(Window *window, uint32_t &width, uint32_t &height);
float scale(Window *window); // pixels per logical point
void setVisible(Window *window, bool visible);
void pumpEvents(Window *window); // only where needsTimer

}

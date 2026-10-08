#pragma once
#include "clap/ext/gui.h"
#include <webgpu/webgpu_cpp.h>
#include <functional>
#include <string>
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
    std::function<void(const View::PointerEvent &)> pointer;
    std::function<bool(View::Key, unsigned modifiers)> key; // true if used
    std::function<bool(const char *utf8)> text;             // true if used
    std::function<bool(View::FileDrag, float x, float y, const std::vector<std::string> &paths)> files; // View::files
};

Window *create(const clap_window_t *parent, uint32_t width, uint32_t height, Callbacks callbacks);
wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window);
void destroy(Window *window);
void setSize(Window *window, uint32_t width, uint32_t height); // clap.gui units
void pixelSize(Window *window, uint32_t &width, uint32_t &height);
float scale(Window *window); // pixels per logical point
void setVisible(Window *window, bool visible);
void pumpEvents(Window *window); // only where needsTimer
void lockPointer(Window *window, bool locked); // see View::lockPointer
bool darkMode(Window *window); // the system's (or host window's) appearance; polled about once a second
void copyText(Window *window, const std::string &utf8); // to the clipboard: View::copyText
void textInput(Window *window, bool editing); // a text field has the keys: see View::editingText
void dragFiles(Window *window, const std::vector<std::string> &paths); // View::dragFiles, during a press

}

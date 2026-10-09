#pragma once
#include <webgpu/webgpu_cpp.h>
#include <functional>
#include <string>
#include "view.h"

// A child window inside the host's window that WebGPU can draw into. One small
// file per desktop platform: native_mac.mm, native_win.cpp, native_linux.cpp.
namespace gpu::platform {

enum class Api { cocoa, win32, x11 };
extern const Api windowApi;   // the window system this platform draws into
extern const bool needsTimer;       // true where the host's timer drives frames and events

// The host's view: an NSView* (cocoa), an HWND (win32) or an X11 window id (x11).
struct Parent { void *handle; };

struct Window;
struct Callbacks
{
    std::function<void()> frame;                       // draw one frame
    std::function<void()> damaged;                     // the system lost what's on screen: draw it again
    std::function<void(const View::PointerEvent &)> pointer;
    std::function<bool(View::Key, unsigned modifiers)> key; // true if used
    std::function<bool(const char *utf8)> text;             // true if used
    std::function<bool(View::FileDrag, float x, float y, const std::vector<std::string> &paths)> files; // View::files
    std::function<void(uint64_t id, Accessible::Action, double value)> access; // View::accessAction, main thread
};

Window *create(const Parent &parent, uint32_t width, uint32_t height, Callbacks callbacks);
wgpu::Surface createSurface(const wgpu::Instance &instance, Window *window);
void destroy(Window *window);
void setSize(Window *window, uint32_t width, uint32_t height); // the parent window's units
void pixelSize(Window *window, uint32_t &width, uint32_t &height);
float scale(Window *window); // pixels per logical point
void setVisible(Window *window, bool visible);
void pumpEvents(Window *window); // only where needsTimer
float refreshRate(Window *window); // its monitor's, in Hz (0 if unknown): where needsTimer, the timer's pace
void lockPointer(Window *window, bool locked); // see View::lockPointer
void setCursor(Window *window, View::Cursor cursor); // see View::setCursor
bool darkMode(Window *window); // the system's (or host window's) appearance; polled about once a second
void copyText(Window *window, const std::string &utf8); // to the clipboard: View::copyText
bool onScreen(Window *window); // false while hidden, minimised or covered: frames wait
void textInput(Window *window, bool editing, View::Caret caret); // a text field has the keys: see View::editingText
void dragFiles(Window *window, const std::vector<std::string> &paths); // View::dragFiles, during a press
void accessibility(Window *window, const Accessibility &tree); // the view's tree changed: tell screen readers

}

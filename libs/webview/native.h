#pragma once
#include "clap/ext/gui.h"

// Puts a CHOC WebView's native view inside the host's window. One small file
// per desktop platform implements this: native_mac.mm, native_win.cpp,
// native_linux.cpp.
namespace webview::platform {

extern const char *const windowApi; // CLAP_WINDOW_API_COCOA, _WIN32 or _X11
extern const bool needsTimer;       // true where the plugin must pump UI events

// Returns platform data that detach() needs, or nullptr on failure.
void *attach(void *view, const clap_window_t *parent);
void detach(void *view, void *attachment);
void setSize(void *view, uint32_t width, uint32_t height);
void setVisible(void *view, bool visible);
void pumpEvents();

}

#pragma once
#include "clap/ext/gui.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The page inside the host's window. On macOS and Windows it is CHOC's
// WebView in the plugin's own process (gui.cpp), placed by native_mac.mm and
// native_win.cpp. On Linux it is a WebKitGTK view in a helper process
// (native_linux.cpp): GTK 3 can't share a process with a GTK 2 host such as
// Ardour, and the plugin needn't link WebKit at all.
namespace webview::platform
{

extern const char* const windowApi; // CLAP_WINDOW_API_COCOA, _WIN32 or _X11
extern const bool needsTimer;       // true where the plugin must pump UI events

struct Resource
{
    std::string bytes, mime;
};

// What the page asks of the plugin.
struct PageClient
{
    std::string uri;                  // the page to open
    std::vector<std::string> scripts; // run before each document's own scripts
    std::function<std::optional<Resource> (const std::string& path)> fetch;
    std::function<bool (const std::string& base64)> receive; // from window.clapWebviewReceive
};

class Page
{
public:
    virtual ~Page() = default;
    virtual bool attach (const clap_window_t* parent) = 0;
    virtual void setSize (uint32_t width, uint32_t height) = 0; // pixels
    virtual void setVisible (bool visible) = 0;
    virtual float scale() const = 0; // pixels per page point (CSS pixel)
    virtual bool evaluate (const std::string& script) = 0;
    virtual void pumpEvents() {}
    virtual bool lost() const
    {
        return false;
    } // true once the helper has gone
};

// Pixels per page point before there is a page.
float defaultScale();

#if defined(__linux__)
std::unique_ptr<Page> openPage (PageClient client); // nullptr if it can't start
#else
// For gui.cpp's page: puts a CHOC WebView's native view in the host's window.
// attach returns platform data that detach needs, or nullptr on failure.
void* attach (void* view, const clap_window_t* parent);
void detach (void* view, void* attachment);
void setSize (void* view, uint32_t width, uint32_t height);
void setVisible (void* view, bool visible);
float scale (void* view);
void tabOut (void* view, bool back); // Tab (Shift+Tab if back) to the host, as if the page hadn't taken it
#endif

} // namespace webview::platform

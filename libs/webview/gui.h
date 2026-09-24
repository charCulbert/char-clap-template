#pragma once
#include "clap/clap.h"
#include "clap/ext/draft/webview.h"
#include "clap/ext/timer-support.h"
#include "../core/messages.h"
#include <memory>

// clap.gui for a plugin whose UI is a page served by its own clap.webview
// extension. Hosts with clap.webview show that page themselves. Other hosts get
// a CHOC WebView embedded in their native window, which loads the same page
// through the same extension. Sizing policy stays with the plugin.
namespace webview {

struct NativeView;

class Gui
{
public:
    Gui();
    ~Gui();

    // Call from clap_plugin.init: looks up the host's and the plugin's webview extensions.
    void init(const clap_plugin_t *plugin, const clap_host_t *host);

    // clap.gui
    bool isApiSupported(const char *api, bool floating) const;
    bool getPreferredApi(const char **api, bool *floating) const;
    bool create(const char *api, bool floating);
    void destroy();
    bool isCreated() const { return created; }
    bool setParent(const clap_window_t *window);
    void setSize(uint32_t width, uint32_t height);
    bool show();
    bool hide();

    // Linux GTK needs servicing from the host's main thread timer.
    void onTimer(clap_id timer);

    // Sends to whichever WebView shows the page. Hosts that open the page
    // without clap.gui.create still receive messages. Fails if none is open.
    bool send(const core::Value &message);

private:
    const clap_plugin_t *plugin = nullptr;
    const clap_host_t *host = nullptr;
    const clap_plugin_webview_t *pluginWebview = nullptr;
    const clap_host_webview_t *hostWebview = nullptr;
    const clap_host_timer_support_t *hostTimers = nullptr;
    clap_id timer = CLAP_INVALID_ID;
    bool created = false;
    std::unique_ptr<NativeView> native;
};

}

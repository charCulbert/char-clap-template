#include "gui.h"
#include <cstring>
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// A native WebView (CHOC) on desktop. WCLAP hosts show the page themselves,
// and iOS has no native view here yet.
#if !defined(__wasm__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define WEBVIEW_NATIVE 1
#endif

#if WEBVIEW_NATIVE
#include "native.h"
#include <choc/gui/choc_WebView.h>
#include <choc/memory/choc_Base64.h>
#endif

namespace webview {

#if WEBVIEW_NATIVE
// Makes the CHOC page behave as clap.webview/3 describes: the page posts
// ArrayBuffers to window.parent, which here is the page itself, so catch those
// and pass them to C++ as base64. Messages from C++ arrive as 'message' events.
static constexpr const char *bridgeScript = R"JS(
window.addEventListener('message', event => {
  if (event.source !== window) return;
  event.stopImmediatePropagation();
  const data = event.data;
  const bytes = data instanceof ArrayBuffer ? new Uint8Array(data)
      : ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : null;
  if (!bytes) return;
  let binary = '';
  for (let i = 0; i < bytes.length; i += 8192)
    binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
  window.clapWebviewReceive(btoa(binary));
}, {capture: true});
)JS";

struct NativeView
{
    std::unique_ptr<choc::ui::WebView> view;
    void *attachment = nullptr;
    // CHOC starts loading before it lets us install the bridge (and WebView2
    // installs scripts asynchronously), so serve a blank page until then.
    bool bridgeInstalled = false;

    ~NativeView()
    {
        if (attachment) platform::detach(view->getViewHandle(), attachment);
    }
};

// Reads one resource through the plugin's own clap.webview extension.
static std::optional<choc::ui::WebView::Options::Resource>
fetchFromPlugin(const clap_plugin_t *plugin, const clap_plugin_webview_t *webview, const std::string &path)
{
    struct Collector
    {
        clap_ostream_t stream{this, write};
        std::string bytes;
        static int64_t CLAP_ABI write(const clap_ostream_t *stream, const void *data, uint64_t size)
        {
            static_cast<Collector *>(stream->ctx)->bytes.append(static_cast<const char *>(data), size);
            return int64_t(size);
        }
    } collector;
    char mime[128] = {};
    if (!webview->get_resource(plugin, path.c_str(), mime, sizeof(mime), &collector.stream))
        return std::nullopt;
    return choc::ui::WebView::Options::Resource(collector.bytes, mime);
}
#else
struct NativeView {};
#endif

Gui::Gui() = default;
Gui::~Gui() = default;

void Gui::init(const clap_plugin_t *plugin_, const clap_host_t *host_)
{
    plugin = plugin_;
    host = host_;
    pluginWebview = static_cast<const clap_plugin_webview_t *>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
    hostWebview = static_cast<const clap_host_webview_t *>(host->get_extension(host, CLAP_EXT_WEBVIEW));
    hostTimers = static_cast<const clap_host_timer_support_t *>(host->get_extension(host, CLAP_EXT_TIMER_SUPPORT));
}

bool Gui::isApiSupported(const char *api, bool floating) const
{
    if (!api || floating || !pluginWebview) return false;
    if (std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0) return hostWebview != nullptr;
#if WEBVIEW_NATIVE
    return std::strcmp(api, platform::windowApi) == 0 && (!platform::needsTimer || hostTimers);
#else
    return false;
#endif
}

bool Gui::getPreferredApi(const char **api, bool *floating) const
{
    *floating = false;
    *api = CLAP_WINDOW_API_WEBVIEW;
    if (isApiSupported(*api, false)) return true;
#if WEBVIEW_NATIVE
    *api = platform::windowApi;
    return isApiSupported(*api, false);
#else
    return false;
#endif
}

bool Gui::create(const char *api, bool floating)
{
    if (created || !isApiSupported(api, floating)) return false;
    if (std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0)
    {
        created = true; // the host owns and shows the WebView
        return true;
    }
#if WEBVIEW_NATIVE
    auto nativeView = std::make_unique<NativeView>();
    auto *state = nativeView.get();
    choc::ui::WebView::Options options;
#if defined(_WIN32)
    options.customSchemeURI = "https://choc.localhost/"; // WebView2 serves custom pages over https
#else
    options.customSchemeURI = "choc://choc.choc/";
#endif
    options.acceptsFirstMouseClick = true;
#ifndef NDEBUG
    options.enableDebugMode = true; // right-click > Inspect
#endif
    options.fetchResource = [this, state](const std::string &path)
        -> std::optional<choc::ui::WebView::Options::Resource> {
        if (!state->bridgeInstalled) return choc::ui::WebView::Options::Resource("", "text/html");
        return fetchFromPlugin(plugin, pluginWebview, path);
    };
    options.webviewIsReady = [this, state, home = options.customSchemeURI](choc::ui::WebView &view) {
        view.bind("clapWebviewReceive", [this](const choc::value::ValueView &args) {
            std::string bytes;
            if (!args.isArray() || args.size() != 1 || !args[0].isString() ||
                args[0].getString().size() > (core::maxMessageBytes / 3 + 1) * 4 ||
                !choc::base64::decodeToContainer(bytes, args[0].getString()))
                return choc::value::Value(false);
            return choc::value::Value(pluginWebview->receive(plugin, bytes.data(), uint32_t(bytes.size())));
        });
        view.addInitScript(bridgeScript);
        state->bridgeInstalled = true;
        char uri[2048] = {};
        const int32_t length = pluginWebview->get_uri(plugin, uri, sizeof(uri));
        if (length <= 0 || length > int32_t(sizeof(uri))) return;
        view.navigate(uri[0] == '/' ? home + (uri + 1) : std::string(uri));
    };
    nativeView->view = std::make_unique<choc::ui::WebView>(options);
    if (!nativeView->view->loadedOK() || !nativeView->view->getViewHandle()) return false;
    if (platform::needsTimer && !hostTimers->register_timer(host, 16, &timer))
    {
        timer = CLAP_INVALID_ID;
        return false;
    }
    native = std::move(nativeView);
    created = true;
    return true;
#else
    return false;
#endif
}

void Gui::destroy()
{
    if (timer != CLAP_INVALID_ID) hostTimers->unregister_timer(host, timer);
    timer = CLAP_INVALID_ID;
    native.reset();
    created = false;
}

bool Gui::setParent(const clap_window_t *window)
{
    if (!created || !window || !window->api) return false;
    if (!native) return std::strcmp(window->api, CLAP_WINDOW_API_WEBVIEW) == 0 && !window->ptr;
#if WEBVIEW_NATIVE
    if (native->attachment || std::strcmp(window->api, platform::windowApi) != 0) return false;
    native->attachment = platform::attach(native->view->getViewHandle(), window);
    return native->attachment != nullptr;
#else
    return false;
#endif
}

float Gui::pixelsPerPoint() const
{
#if WEBVIEW_NATIVE
    return platform::scale(native ? native->view->getViewHandle() : nullptr);
#else
    return 1.0f;
#endif
}

void Gui::setSize(uint32_t width, uint32_t height)
{
#if WEBVIEW_NATIVE
    if (native) platform::setSize(native->view->getViewHandle(), width, height);
#endif
}

bool Gui::show()
{
#if WEBVIEW_NATIVE
    if (native) platform::setVisible(native->view->getViewHandle(), true);
#endif
    return created;
}

bool Gui::hide()
{
#if WEBVIEW_NATIVE
    if (native) platform::setVisible(native->view->getViewHandle(), false);
#endif
    return created;
}

void Gui::onTimer(clap_id id)
{
#if WEBVIEW_NATIVE
    if (id == timer && native) platform::pumpEvents();
#endif
}

bool Gui::send(const core::Value &message)
{
    const auto bytes = core::encode(message);
#if WEBVIEW_NATIVE
    if (native)
    {
        const auto base64 = choc::base64::encodeToString(bytes.data(), bytes.size());
        return native->view->evaluateJavascript(
            "window.dispatchEvent(new MessageEvent('message', {data: Uint8Array.from(atob('" + base64 +
            "'), c => c.charCodeAt(0)).buffer}));");
    }
#endif
    return hostWebview && hostWebview->send(host, bytes.data(), uint32_t(bytes.size()));
}

}

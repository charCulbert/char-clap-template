#include "gui.h"
#include "native.h"
#include <cstring>
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// A native WebView on desktop. WCLAP hosts show the page themselves,
// and iOS has no native view here yet.
#if !defined(__wasm__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define WEBVIEW_NATIVE 1
#endif

#if WEBVIEW_NATIVE
#include <choc/memory/choc_Base64.h>
#if !defined(__linux__)
#include <choc/gui/choc_WebView.h>
#endif
#endif

namespace webview {

#if WEBVIEW_NATIVE
// Makes the native page behave as clap.webview/3 describes: the page posts
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

// Where the page's files are served: WebView2 serves custom pages over https.
#if defined(_WIN32)
static constexpr const char *home = "https://choc.localhost/";
#else
static constexpr const char *home = "choc://choc.choc/";
#endif

#if !defined(__linux__)
// CHOC's WebView, in the plugin's process, inside the host's window.
class ChocPage final : public platform::Page
{
public:
    explicit ChocPage(platform::PageClient client_) : client(std::move(client_))
    {
        choc::ui::WebView::Options options;
        options.customSchemeURI = home;
        options.acceptsFirstMouseClick = true;
#ifndef NDEBUG
        options.enableDebugMode = true; // right-click > Inspect
#endif
        // CHOC starts loading before it lets us install the scripts (and
        // WebView2 installs them asynchronously), so serve a blank page until then.
        options.fetchResource = [this](const std::string &path) -> std::optional<choc::ui::WebView::Options::Resource> {
            if (!scriptsInstalled) return choc::ui::WebView::Options::Resource("", "text/html");
            auto resource = client.fetch(path);
            if (!resource) return std::nullopt;
            return choc::ui::WebView::Options::Resource(resource->bytes, resource->mime);
        };
        options.webviewIsReady = [this](choc::ui::WebView &webView) {
            webView.bind("clapWebviewReceive", [this](const choc::value::ValueView &args) {
                if (!args.isArray() || args.size() != 1 || !args[0].isString()) return choc::value::Value(false);
                return choc::value::Value(client.receive(std::string(args[0].getString())));
            });
            for (const auto &script : client.scripts)
                webView.addInitScript(script);
            scriptsInstalled = true;
            // WebView2 adds init scripts (ours and bind's) asynchronously, and a
            // page loaded before they're in runs without them. CHOC doesn't pass
            // on their completion, so the page waits for a script run after them
            // to complete: WebView2 handles a view's calls in order. Elsewhere
            // they're in already, and this costs a round trip.
            webView.evaluateJavascript("0", [self = std::weak_ptr(self), uri = client.uri](const std::string &,
                                                                                         const choc::value::ValueView &) {
                if (const auto alive = self.lock()) (*alive)->view->navigate(uri);
            });
        };
        view = std::make_unique<choc::ui::WebView>(options);
    }

    ~ChocPage() override
    {
        if (attachment) platform::detach(view->getViewHandle(), attachment);
    }

    bool loaded() const { return view->loadedOK() && view->getViewHandle(); }

    bool attach(const clap_window_t *parent) override
    {
        if (attachment) return false;
        attachment = platform::attach(view->getViewHandle(), parent);
        return attachment != nullptr;
    }

    void setSize(uint32_t width, uint32_t height) override { platform::setSize(view->getViewHandle(), width, height); }
    void setVisible(bool visible) override { platform::setVisible(view->getViewHandle(), visible); }
    float scale() const override { return platform::scale(view->getViewHandle()); }
    bool evaluate(const std::string &script) override { return view->evaluateJavascript(script); }

private:
    platform::PageClient client;
    // Lets a callback the WebView makes later (a script's completion) find
    // out whether we're still here.
    std::shared_ptr<ChocPage *> self = std::make_shared<ChocPage *>(this);
    std::unique_ptr<choc::ui::WebView> view;
    void *attachment = nullptr;
    bool scriptsInstalled = false;
};
#endif

// Reads one resource through the plugin's own clap.webview extension.
static std::optional<platform::Resource>
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
    return platform::Resource{std::move(collector.bytes), mime};
}
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
    hostGui = static_cast<const clap_host_gui_t *>(host->get_extension(host, CLAP_EXT_GUI));
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
    auto newPage = openPage();
    if (!newPage) return false;
    if (platform::needsTimer && !hostTimers->register_timer(host, 16, &timer))
    {
        timer = CLAP_INVALID_ID;
        return false;
    }
    page = std::move(newPage);
    parent = {};
    visible = false;
    restarts = 0;
    created = true;
    return true;
#else
    return false;
#endif
}

#if WEBVIEW_NATIVE
std::unique_ptr<platform::Page> Gui::openPage()
{
    char uri[2048] = {};
    const int32_t length = pluginWebview->get_uri(plugin, uri, sizeof(uri));
    if (length <= 0 || length > int32_t(sizeof(uri))) return nullptr;
    platform::PageClient client;
    client.uri = uri[0] == '/' ? home + std::string(uri + 1) : std::string(uri);
    client.scripts = {bridgeScript};
    client.fetch = [this](const std::string &path) { return fetchFromPlugin(plugin, pluginWebview, path); };
    client.receive = [this](const std::string &base64) {
        std::string bytes;
        if (base64.size() > (core::maxMessageBytes / 3 + 1) * 4 || !choc::base64::decodeToContainer(bytes, base64))
            return false;
        return pluginWebview->receive(plugin, bytes.data(), uint32_t(bytes.size()));
    };
#if defined(__linux__)
    return platform::openPage(std::move(client));
#else
    auto chocPage = std::make_unique<ChocPage>(std::move(client));
    if (!chocPage->loaded()) return nullptr;
    return chocPage;
#endif
}
#endif

void Gui::destroy()
{
    if (timer != CLAP_INVALID_ID) hostTimers->unregister_timer(host, timer);
    timer = CLAP_INVALID_ID;
    page.reset();
    created = false;
}

bool Gui::setParent(const clap_window_t *window)
{
    if (!created || !window || !window->api) return false;
    if (!page) return std::strcmp(window->api, CLAP_WINDOW_API_WEBVIEW) == 0 && !window->ptr;
#if WEBVIEW_NATIVE
    if (std::strcmp(window->api, platform::windowApi) != 0 || !page->attach(window)) return false;
    parent = *window;
    parent.api = platform::windowApi;
    return true;
#else
    return false;
#endif
}

float Gui::pixelsPerPoint() const
{
#if WEBVIEW_NATIVE
    return page ? page->scale() : platform::defaultScale();
#else
    return 1.0f;
#endif
}

void Gui::setSize(uint32_t width_, uint32_t height_)
{
    width = width_;
    height = height_;
#if WEBVIEW_NATIVE
    if (page) page->setSize(width, height);
#endif
}

bool Gui::show()
{
    visible = true;
#if WEBVIEW_NATIVE
    if (page) page->setVisible(true);
#endif
    return created;
}

bool Gui::hide()
{
    visible = false;
#if WEBVIEW_NATIVE
    if (page) page->setVisible(false);
#endif
    return created;
}

void Gui::onTimer(clap_id id)
{
#if WEBVIEW_NATIVE
    if (id != timer || !page) return;
    const auto now = std::chrono::steady_clock::now();
    if (!page->lost())
    {
        page->pumpEvents();
        if (!page->lost()) return;
        // The page's process has gone, and its window with it: the host
        // should close ours and call destroy.
        lostAt = now;
        if (hostGui) hostGui->closed(host, true);
        return;
    }
    // A host that keeps the window (a VST3 host through clap-wrapper, which
    // can't pass on closed) gets the page started again in it, a few times.
    if (now - lostAt < std::chrono::seconds(1) || restarts >= 3 || !parent.api) return;
    lostAt = now;
    ++restarts;
    auto newPage = openPage();
    if (!newPage) return;
    newPage->setSize(width, height);
    newPage->attach(&parent);
    newPage->setVisible(visible);
    page = std::move(newPage);
#endif
}

bool Gui::send(const core::Value &message)
{
    const auto bytes = core::encode(message);
#if WEBVIEW_NATIVE
    if (page)
    {
        const auto base64 = choc::base64::encodeToString(bytes.data(), bytes.size());
        return page->evaluate(
            "window.dispatchEvent(new MessageEvent('message', {data: Uint8Array.from(atob('" + base64 +
            "'), c => c.charCodeAt(0)).buffer}));");
    }
#endif
    return hostWebview && hostWebview->send(host, bytes.data(), uint32_t(bytes.size()));
}

}

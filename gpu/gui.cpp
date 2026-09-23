#include "gui.h"
#include "context.h"
#include "native.h"
#include <cstring>

namespace gpu {

Gui::Gui() = default;
Gui::~Gui() { destroy(); }

void Gui::init(const clap_plugin_t *plugin_, const clap_host_t *host_)
{
    plugin = plugin_;
    host = host_;
    pluginWebview = static_cast<const clap_plugin_webview_t *>(plugin->get_extension(plugin, CLAP_EXT_WEBVIEW));
    hostTimers = static_cast<const clap_host_timer_support_t *>(host->get_extension(host, CLAP_EXT_TIMER_SUPPORT));
}

bool Gui::isApiSupported(const char *api, bool floating) const
{
    return api && !floating && pluginWebview && std::strcmp(api, platform::windowApi) == 0 &&
           (!platform::needsTimer || hostTimers);
}

bool Gui::getPreferredApi(const char **api, bool *floating) const
{
    *api = platform::windowApi;
    *floating = false;
    return isApiSupported(*api, false);
}

bool Gui::create(const char *api, bool floating)
{
    if (context || !isApiSupported(api, floating)) return false;
    auto newContext = std::make_unique<Context>();
    if (!newContext->createDevice()) return false;
    if (platform::needsTimer && !hostTimers->register_timer(host, 16, &timer))
    {
        timer = CLAP_INVALID_ID;
        return false;
    }
    context = std::move(newContext);
    view = createView();
    // The View talks to the plugin exactly as the page does: through clap.webview.
    view->send = [this](const webview::Value &message) {
        const auto bytes = webview::encode(message);
        pluginWebview->receive(plugin, bytes.data(), uint32_t(bytes.size()));
    };
    return true;
}

void Gui::destroy()
{
    if (timer != CLAP_INVALID_ID) hostTimers->unregister_timer(host, timer);
    timer = CLAP_INVALID_ID;
    if (window) platform::destroy(window);
    window = nullptr;
    view.reset();
    context.reset();
}

bool Gui::setParent(const clap_window_t *parent)
{
    if (!context || window || !parent || !parent->api || std::strcmp(parent->api, platform::windowApi) != 0)
        return false;
    window = platform::create(parent, width, height, {
        [this] { frame(); },
        [this](View::Pointer type, float x, float y) { if (view) view->pointer(type, x, y); },
    });
    if (!window) return false;
    context->surface = platform::createSurface(context->instance, window);
    uint32_t w, h;
    platform::pixelSize(window, w, h);
    context->configure(w, h);
    view->prepare(context->device, context->format); // sends "ready"
    return true;
}

void Gui::setSize(uint32_t newWidth, uint32_t newHeight)
{
    width = newWidth;
    height = newHeight;
    if (!window) return;
    platform::setSize(window, width, height);
    uint32_t w, h;
    platform::pixelSize(window, w, h);
    context->configure(w, h);
}

bool Gui::show()
{
    if (window) platform::setVisible(window, true);
    return context != nullptr;
}

bool Gui::hide()
{
    if (window) platform::setVisible(window, false);
    return context != nullptr;
}

void Gui::onTimer(clap_id id)
{
    if (id != timer || !window) return;
    platform::pumpEvents(window);
    frame();
}

void Gui::frame()
{
    if (window && view) context->frame(*view, platform::scale(window));
}

bool Gui::send(const webview::Value &message)
{
    if (!view) return false;
    view->receive(message);
    return true;
}

}

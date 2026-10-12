#include "gui.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__wasm__)

// WCLAP: the host shows the page (gpu_add_page) and sends its messages to
// clap.webview; this only accepts the host's "webview" window and sends ours.
namespace gpu
{

Gui::Gui() = default;
Gui::~Gui() = default;

void Gui::init (const clap_plugin_t* plugin_, const clap_host_t* host_)
{
    plugin = plugin_;
    host = host_;
    pluginWebview = static_cast<const clap_plugin_webview_t*> (plugin->get_extension (plugin, CLAP_EXT_WEBVIEW));
    hostWebview = static_cast<const clap_host_webview_t*> (host->get_extension (host, CLAP_EXT_WEBVIEW));
}

bool Gui::isApiSupported (const char* api, bool floating) const
{
    return api && !floating && pluginWebview && hostWebview && std::strcmp (api, CLAP_WINDOW_API_WEBVIEW) == 0;
}

float Gui::pixelsPerPoint() const
{
    return 1.0f;
} // clap.webview sizes are logical

bool Gui::getPreferredApi (const char** api, bool* floating) const
{
    *api = CLAP_WINDOW_API_WEBVIEW;
    *floating = false;
    return isApiSupported (*api, false);
}

bool Gui::create (const char* api, bool floating)
{
    if (created || !isApiSupported (api, floating))
        return false;
    return created = true;
}

void Gui::destroy()
{
    created = false;
}
bool Gui::setParent (const clap_window_t* window)
{
    return created && window && window->api && std::strcmp (window->api, CLAP_WINDOW_API_WEBVIEW) == 0;
}
void Gui::setSize (uint32_t, uint32_t) {}
bool Gui::setScale (double)
{
    return false;
} // the browser scales the page

bool Gui::show()
{
    return created;
}
bool Gui::hide()
{
    return created;
}
void Gui::onTimer (clap_id) {}

bool Gui::send (const core::Value& message)
{
    const auto bytes = core::encode (message);
    return hostWebview && hostWebview->send (host, bytes.data(), uint32_t (bytes.size()));
}

} // namespace gpu

#else

#include "context.h"
#include "native.h"
#include "../core/resources.h"

namespace gpu
{

std::string loadAsset (const char* path)
{
    auto resource = core::readResource (path);
    return resource ? std::move (resource->bytes) : std::string();
}

// The presenters know nothing of CLAP: the window API and parent are translated here.
static const char* apiName (platform::Api api)
{
    switch (api)
    {
        case platform::Api::cocoa:
            return CLAP_WINDOW_API_COCOA;
        case platform::Api::win32:
            return CLAP_WINDOW_API_WIN32;
        case platform::Api::x11:
            return CLAP_WINDOW_API_X11;
    }
    return nullptr;
}

static platform::Parent parentHandle (const clap_window_t* parent)
{
    switch (platform::windowApi)
    {
        case platform::Api::cocoa:
            return { parent->cocoa };
        case platform::Api::win32:
            return { parent->win32 };
        case platform::Api::x11:
            return { reinterpret_cast<void*> (uintptr_t (parent->x11)) };
    }
    return { nullptr };
}

Gui::Gui() = default;
Gui::~Gui()
{
    destroy();
}

void Gui::init (const clap_plugin_t* plugin_, const clap_host_t* host_)
{
    plugin = plugin_;
    host = host_;
    pluginWebview = static_cast<const clap_plugin_webview_t*> (plugin->get_extension (plugin, CLAP_EXT_WEBVIEW));
    hostTimers = static_cast<const clap_host_timer_support_t*> (host->get_extension (host, CLAP_EXT_TIMER_SUPPORT));
    hostGui = static_cast<const clap_host_gui_t*> (host->get_extension (host, CLAP_EXT_GUI));
    pluginGui = static_cast<const clap_plugin_gui_t*> (plugin->get_extension (plugin, CLAP_EXT_GUI));
}

bool Gui::isApiSupported (const char* api, bool floating) const
{
    return api && !floating && pluginWebview && std::strcmp (api, apiName (platform::windowApi)) == 0
           && (!platform::needsTimer || hostTimers);
}

float Gui::pixelsPerPoint() const
{
    return platform::windowApi == platform::Api::cocoa ? 1.0f : scale();
}

bool Gui::getPreferredApi (const char** api, bool* floating) const
{
    *api = apiName (platform::windowApi);
    *floating = false;
    return isApiSupported (*api, false);
}

bool Gui::create (const char* api, bool floating)
{
    if (context || !isApiSupported (api, floating))
        return false;
    auto newContext = std::make_unique<Context>();
    if (!newContext->createDevice())
        return false;
    period = 16;
    if (platform::needsTimer && !hostTimers->register_timer (host, period, &timer))
    {
        timer = CLAP_INVALID_ID;
        return false;
    }
    context = std::move (newContext);
    view = createView();
    // The View talks to the plugin exactly as the page does: through clap.webview.
    view->send = [this] (const core::Value& message) {
        const auto bytes = core::encode (message);
        pluginWebview->receive (plugin, bytes.data(), uint32_t (bytes.size()));
    };
    view->lockPointer = [this] (bool locked) {
        if (window)
            platform::lockPointer (window, locked);
    };
    view->setCursor = [this] (View::Cursor cursor) {
        if (window)
            platform::setCursor (window, cursor);
    };
    view->copyText = [this] (const std::string& utf8) {
        if (window)
            platform::copyText (window, utf8);
    };
    view->dragFiles = [this] (const std::vector<std::string>& paths) {
        if (window)
            platform::dragFiles (window, paths);
    };
    if (hostGui && pluginGui)
        view->resizeTo = [this] (float w, float h) {
            // The view measures in pixels; clap.gui in points on macOS.
            const float perUnit = scale() / pixelsPerPoint();
            uint32_t newWidth = uint32_t (std::max (1.0f, w / perUnit)),
                     newHeight = uint32_t (std::max (1.0f, h / perUnit));
            pluginGui->adjust_size (plugin, &newWidth, &newHeight);
            // A host that agrees needn't call set_size afterwards: the plugin's own set_size takes it.
            if ((newWidth != width || newHeight != height) && hostGui->request_resize (host, newWidth, newHeight))
                pluginGui->set_size (plugin, newWidth, newHeight);
        };
    return true;
}

void Gui::destroy()
{
    if (timer != CLAP_INVALID_ID)
        hostTimers->unregister_timer (host, timer);
    timer = CLAP_INVALID_ID;
    // GPU objects first: the surface refers to our window (and on Linux to its
    // X display connection), which must outlive it.
    view.reset();
    context.reset();
    if (window)
        platform::destroy (window);
    window = nullptr;
    // What the last window had: a new one starts without it. width and
    // height stay, for the next one to open at the size this had.
    dirty = true;
    damaged = false;
    shownScale = 0;
    accessVersion = 0;
    appearanceChecked = recoveryTried = paced = {};
}

bool Gui::setParent (const clap_window_t* parent)
{
    if (!context || window || !parent || !parent->api || std::strcmp (parent->api, apiName (platform::windowApi)) != 0)
        return false;
    window = platform::create (parentHandle (parent), width, height,
        {
            [this] { frame(); },
            [this] { damaged = true; },
            [this] (const View::PointerEvent& event) {
                if (view)
                    view->pointer (event), dirty = true;
            },
            [this] (View::Key key, unsigned modifiers) {
                dirty = true;
                return view && view->key (key, modifiers);
            },
            [this] (const char* utf8) {
                dirty = true;
                return view && view->text (utf8);
            },
            [this] (View::FileDrag type, float x, float y, const std::vector<std::string>& paths) {
                dirty = true;
                return view && view->files (type, x, y, paths);
            },
            [this] (uint64_t id, Accessible::Action action, double value) {
                if (view)
                    view->accessAction (id, action, value), dirty = true;
            },
        });
    if (!window)
        return false;
    context->surface = platform::createSurface (context->instance, window);
    uint32_t w, h;
    platform::pixelSize (window, w, h);
    context->configure (w, h);
    dirty = true;
    view->prepare (context->device, context->format); // sends "ready"
    paceTimer();
    return true;
}

// Where the host's timer draws frames, as often as the monitor under the
// window refreshes (8 ms at 120 Hz), not just every 16 ms. Asked again every
// couple of seconds (onTimer): the window may have moved to another monitor.
void Gui::paceTimer()
{
    paced = std::chrono::steady_clock::now();
    if (!platform::needsTimer || timer == CLAP_INVALID_ID)
        return;
    const float rate = platform::refreshRate (window);
    uint32_t wanted = uint32_t (std::clamp (1000.0f / rate, 4.0f, 16.0f));
    if (rate <= 0 || wanted == period)
        return;
    hostTimers->unregister_timer (host, timer);
    if (!hostTimers->register_timer (host, wanted, &timer) && !hostTimers->register_timer (host, wanted = 16, &timer))
        timer = CLAP_INVALID_ID;
    period = wanted;
}

void Gui::setSize (uint32_t newWidth, uint32_t newHeight)
{
    width = newWidth;
    height = newHeight;
    if (!window)
        return;
    platform::setSize (window, width, height);
    uint32_t w, h;
    platform::pixelSize (window, w, h);
    context->configure (w, h);
    dirty = true;
}

// The GPU was reset or removed: a new device and surface, and the view builds
// its resources again on them. Tried at most once a second until one comes.
void Gui::recover()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - recoveryTried < std::chrono::seconds (1))
        return;
    recoveryTried = now;
    auto fresh = std::make_unique<Context>();
    if (!fresh->createDevice())
        return;
    fresh->surface = platform::createSurface (fresh->instance, window);
    uint32_t w, h;
    platform::pixelSize (window, w, h);
    fresh->configure (w, h);
    context = std::move (fresh);
    view->prepare (context->device, context->format);
    damaged = true;
}

float Gui::scale() const
{
    return hostScale > 0 ? hostScale : platform::scale (window); // a guess before there's a window
}

bool Gui::setScale (double newScale)
{
    // Only X11 has no way for us to know: elsewhere the window's own scale wins.
    if (platform::windowApi != platform::Api::x11 || newScale <= 0)
        return false;
    hostScale = float (newScale);
    dirty = true;
    return true;
}

bool Gui::show()
{
    if (window)
        platform::setVisible (window, true);
    return context != nullptr;
}

bool Gui::hide()
{
    if (window)
        platform::setVisible (window, false);
    return context != nullptr;
}

void Gui::onTimer (clap_id id)
{
    if (id != timer || !window)
        return;
    platform::pumpEvents (window);
    frame();
    if (std::chrono::steady_clock::now() - paced > std::chrono::seconds (2))
        paceTimer();
}

void Gui::frame()
{
    if (!window || !view)
        return;
    if (context->lost->load())
        recover();
    if (context->lost->load())
        return; // no GPU to draw with yet
    if (!platform::onScreen (window))
    {
        damaged = true; // draw when it's back: what was there may be gone
        return;
    }
    // The view's size in pixels changes without clap.gui.set_size too: a host
    // may create it before it is in a window (AU), or move it to a screen of
    // another scale. Keep the surface matching it.
    uint32_t w, h;
    platform::pixelSize (window, w, h);
    if (w && h && (w != context->width || h != context->height))
    {
        context->configure (w, h);
        dirty = true;
    }
    // Moved to a screen of another scale: ask for the size that keeps the
    // view the same in points (clap.gui counts pixels outside macOS).
    const float newScale = scale();
    if (shownScale > 0 && newScale != shownScale && platform::windowApi != platform::Api::cocoa && hostGui && pluginGui)
    {
        uint32_t wide = uint32_t (std::lround (width * newScale / shownScale)),
                 tall = uint32_t (std::lround (height * newScale / shownScale));
        pluginGui->adjust_size (plugin, &wide, &tall);
        if (hostGui->request_resize (host, wide, tall))
            pluginGui->set_size (plugin, wide, tall);
    }
    shownScale = newScale;
    const auto now = std::chrono::steady_clock::now();
    if (now - appearanceChecked > std::chrono::seconds (1))
    {
        appearanceChecked = now;
        const bool dark = platform::darkMode (window);
        if (dark != view->darkMode)
            view->darkMode = dark, dirty = true;
    }
    platform::textInput (window, view->editingText(), view->textCaret());
    if (!(dirty || damaged || view->animating()))
        return;
    // Cleared first: what the view does while it records (a resize, a
    // message to the plugin) can ask for the next frame.
    const bool redraw = damaged;
    dirty = damaged = false;
    // Not shown (no surface texture this time): the view has recorded it
    // already, so draw it next time even if nothing changes in between.
    if (!context->frame (*view, scale(), redraw))
        damaged = true;
    if (const Accessibility* tree = view->accessibility(); tree && tree->version != accessVersion)
        platform::accessibility (window, *tree), accessVersion = tree->version;
}

bool Gui::send (const core::Value& message)
{
    if (!view)
        return false;
    view->receive (message);
    dirty = true;
    return true;
}

} // namespace gpu

#endif

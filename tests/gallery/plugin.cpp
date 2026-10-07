// GUI Gallery: a page of every feature of the GPU interface (libs/gpu,
// libs/draw2d), for checking how it looks and performs in real hosts. Audio
// passes through; its four settings exist only for the controls to edit.
// A test bench, not an example: tools/new-plugin.py doesn't offer it.
#include "clap/clap.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/ext/draft/webview.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string.h>
#include <string>
#include <vector>
#include "core/messages.h"
#include "core/resources.h"
#include "gpu/gui.h"

// Settings: {name, min, max, default}. Their index is their id in messages.
struct Setting
{
    const char *name;
    double min, max, defaultValue;
};
static const Setting settings[] = {
    {"Gain", -24, 24, 0},
    {"Mix", 0, 100, 100},
    {"Cutoff", 20, 20000, 1000},
    {"Drive", 0, 10, 2},
};
#define S_COUNT (4)

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    double settingValues[S_COUNT]; // main thread only

    gpu::Gui gui;
    uint32_t guiWidth = 720, guiHeight = 480;
    bool uiReady = false; // the interface has said "ready" and not been closed since
};

static bool PluginReceiveMessage(MyPlugin *plugin, const core::Value &message);
static void PluginSendValues(MyPlugin *plugin);

static const char *const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.guigallery",
    .name = "GUI Gallery",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "1.0.0",
    .description = "Every feature of the GPU interface, on one page each.",
    .features = pluginFeatures,
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t { return 1; },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_audio_port_info_t *info) -> bool
    {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = 0; // output may share the input's buffer
        snprintf(info->name, sizeof(info->name), "%s", isInput ? "Audio Input" : "Audio Output");
        return true;
    },
};

// The interface talks to the plugin through clap.webview: natively gpu::Gui
// passes its messages here, and in WCLAP the host shows the interface as a web
// page (the same view compiled to wasm), which it loads through get_uri and
// get_resource.
static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [](const clap_plugin_t *_plugin, char *uri, uint32_t capacity) -> int32_t
    {
        static const char start[] = "/page/index.html"; // in resources/
        if (capacity)
            snprintf(uri, capacity, "%s", start);
        return sizeof(start); // including the terminating zero
    },

    .get_resource = [](const clap_plugin_t *_plugin, const char *path, char *mime, uint32_t mimeCapacity,
                       const clap_ostream_t *stream) -> bool
    {
        auto resource = core::readResource(path);
        if (!resource || resource->mime.size() >= mimeCapacity)
            return false;
        strcpy(mime, resource->mime.c_str());
        const char *bytes = resource->bytes.data();
        uint64_t remaining = resource->bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write(stream, bytes, remaining);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (uint64_t)written;
        }
        return true;
    },

    .receive = [](const clap_plugin_t *_plugin, const void *buffer, uint32_t size) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        auto message = core::decode(buffer, size);
        return message && PluginReceiveMessage(plugin, *message);
    },
};

// clap.gui: size policy is ours; creating and embedding the view is gpu::Gui's.
// The limits are in points.
#define GUI_MIN_WIDTH (480)
#define GUI_MIN_HEIGHT (320)
#define GUI_MAX_WIDTH (2000)
#define GUI_MAX_HEIGHT (1200)

// Clamps a clap.gui size, which is in pixels outside macOS, to the limits.
static void clampGuiSize(const MyPlugin *plugin, uint32_t &width, uint32_t &height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    const auto clamp = [scale](uint32_t value, float low, float high) {
        return std::clamp(value, uint32_t(std::ceil(low * scale)), uint32_t(high * scale));
    };
    width = clamp(width, GUI_MIN_WIDTH, GUI_MAX_WIDTH);
    height = clamp(height, GUI_MIN_HEIGHT, GUI_MAX_HEIGHT);
}

static const clap_plugin_gui_t extensionGui = {
    .is_api_supported = [](const clap_plugin_t *_plugin, const char *api, bool isFloating) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.isApiSupported(api, isFloating); },

    .get_preferred_api = [](const clap_plugin_t *_plugin, const char **api, bool *isFloating) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.getPreferredApi(api, isFloating); },

    .create = [](const clap_plugin_t *_plugin, const char *api, bool isFloating) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (!plugin->gui.create(api, isFloating))
            return false;
        clampGuiSize(plugin, plugin->guiWidth, plugin->guiHeight); // so get_size never reports a size set_size refuses
        plugin->gui.setSize(plugin->guiWidth, plugin->guiHeight);
        return true;
    },

    .destroy = [](const clap_plugin_t *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->gui.destroy();
        plugin->uiReady = false;
    },

    .set_scale = [](const clap_plugin_t *_plugin, double scale) -> bool
    { return false; }, // the view reads the window's scale itself

    .get_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        *width = plugin->guiWidth;
        *height = plugin->guiHeight;
        return true;
    },

    .can_resize = [](const clap_plugin_t *_plugin) -> bool { return true; },

    .get_resize_hints = [](const clap_plugin_t *_plugin, clap_gui_resize_hints_t *hints) -> bool
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },

    .adjust_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        clampGuiSize((MyPlugin *)_plugin->plugin_data, *width, *height);
        return true;
    },

    .set_size = [](const clap_plugin_t *_plugin, uint32_t width, uint32_t height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        uint32_t allowedWidth = width, allowedHeight = height;
        clampGuiSize(plugin, allowedWidth, allowedHeight);
        if (allowedWidth != width || allowedHeight != height)
            return false;
        plugin->guiWidth = width;
        plugin->guiHeight = height;
        plugin->gui.setSize(width, height);
        return true;
    },

    .set_parent = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.setParent(window); },

    .set_transient = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool { return false; },

    .suggest_title = [](const clap_plugin_t *_plugin, const char *title) {},

    .show = [](const clap_plugin_t *_plugin) -> bool { return ((MyPlugin *)_plugin->plugin_data)->gui.show(); },

    .hide = [](const clap_plugin_t *_plugin) -> bool { return ((MyPlugin *)_plugin->plugin_data)->gui.hide(); },
};

// On Linux the host's timer drives the view's events and frames; see gpu::Gui::onTimer.
static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [](const clap_plugin_t *_plugin, clap_id timerId)
    { ((MyPlugin *)_plugin->plugin_data)->gui.onTimer(timerId); },
};

// Messages between the interface and the plugin (main thread only):
//   UI -> plugin  {type: "ready"}
//                 {type: "change", id, value}     a setting changed
//                 {type: "begin" | "end", id}     a drag starts or ends (unused here)
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default}]}  the settings
//                 {type: "values", values: [value per setting id]}
static bool PluginReceiveMessage(MyPlugin *plugin, const core::Value &message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        core::Value::Array list;
        for (uint32_t i = 0; i < S_COUNT; i++)
            list.push_back(core::Value::Map{{"id", (double)i}, {"name", settings[i].name}, {"min", settings[i].min},
                                            {"max", settings[i].max}, {"default", settings[i].defaultValue}});
        plugin->gui.send(core::Value::Map{{"type", "params"}, {"params", list}});
        PluginSendValues(plugin);
        plugin->uiReady = true;
        return true;
    }

    const double id = message["id"].number(-1);
    if (!(id >= 0 && id < S_COUNT))
        return false;
    if (type == "change")
    {
        const Setting &setting = settings[(int)id];
        const double value = message["value"].number(setting.defaultValue);
        plugin->settingValues[(int)id] = value < setting.min ? setting.min : value > setting.max ? setting.max : value;
    }
    return type == "change" || type == "begin" || type == "end";
}

static void PluginSendValues(MyPlugin *plugin)
{
    core::Value::Array values;
    for (uint32_t i = 0; i < S_COUNT; i++)
        values.push_back(plugin->settingValues[i]);
    plugin->gui.send(core::Value::Map{{"type", "values"}, {"values", values}});
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [](const clap_plugin *_plugin) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->gui.init(_plugin, plugin->host);
        for (uint32_t i = 0; i < S_COUNT; i++)
            plugin->settingValues[i] = settings[i].defaultValue;
        return true;
    },

    .destroy = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->gui.destroy();
        delete plugin;
    },

    .activate = [](const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount, uint32_t maximumFramesCount) -> bool
    { return true; },

    .deactivate = [](const clap_plugin *_plugin) {},
    .start_processing = [](const clap_plugin *_plugin) -> bool { return true; },
    .stop_processing = [](const clap_plugin *_plugin) {},
    .reset = [](const clap_plugin *_plugin) {},

    .process = [](const clap_plugin *_plugin, const clap_process_t *process) -> clap_process_status
    {
        const uint32_t frameCount = process->frames_count;
        float *const *input = process->audio_inputs[0].data32;
        float *const *output = process->audio_outputs[0].data32;

        // Pass the audio through (the host may give us one buffer for both).
        for (uint32_t channel = 0; channel < 2; channel++)
            if (output[channel] != input[channel])
                memcpy(output[channel], input[channel], sizeof(float) * frameCount);

        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [](const clap_plugin *plugin, const char *id) -> const void *
    {
        if (0 == strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp(id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp(id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp(id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin) {},
};

// What entry.cpp needs.
const clap_plugin_descriptor_t *getPluginDescriptor()
{
    return &pluginDescriptor;
}

const clap_plugin_t *createPlugin(const clap_host_t *host)
{
    MyPlugin *plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
}

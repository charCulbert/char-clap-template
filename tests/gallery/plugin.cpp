// GUI Gallery: a page of every feature of the GPU interface (libs/gpu,
// libs/draw2d), for checking how it looks and performs in real hosts. Audio
// passes through; its four parameters exist only for the controls to edit.
// A test bench, not an example: tools/new-plugin.py doesn't offer it.
//
// Undo goes through the host, so one history covers the project. Parameter
// drags are gestures, which the host records itself. The interface's other
// edits (envelope, steps, name, switches) come here as one "data" value, which
// is part of the state: each finished edit is a clap.undo change, or marks the
// project dirty where the host lacks clap.undo, and the host's undo loads the
// state back.
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/ext/draft/undo.h"
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

static_assert(std::atomic<float>::is_always_lock_free);

// Parameters: {name, min, max, default, format}. Their index is their id.
struct Setting
{
    const char *name;
    double min, max, defaultValue;
    const char *format;
};
static const Setting settings[] = {
    {"Gain", -24, 24, 0, "%.1f dB"},
    {"Mix", 0, 100, 100, "%.0f %%"},
    {"Cutoff", 20, 20000, 1000, "%.0f Hz"},
    {"Drive", 0, 10, 2, "%.2f"},
};
#define P_COUNT (4)

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *hostParams;
    const clap_host_state_t *hostState;
    const clap_host_undo_t *hostUndo;

    // Each parameter as the audio thread has it, and as the main thread does;
    // the changed flags say which side has news for the other.
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    std::atomic<bool> gestureBegin[P_COUNT], gestureEnd[P_COUNT];
    std::atomic<bool> uiDirty; // the host changed a parameter

    core::Value data; // the interface's other state (main thread)

    gpu::Gui gui;
    float guiWidth = 720, guiHeight = 480; // points, so the window keeps its size across scales
};

static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event);
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out);
static void PluginSyncAudioToMain(MyPlugin *plugin);
static double PluginGetValue(MyPlugin *plugin, uint32_t i);
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

static const clap_plugin_params_t extensionParams = {
    .count = [](const clap_plugin_t *plugin) -> uint32_t { return P_COUNT; },

    .get_info = [](const clap_plugin_t *_plugin, uint32_t index, clap_param_info_t *information) -> bool
    {
        if (index >= P_COUNT)
            return false;
        memset(information, 0, sizeof(clap_param_info_t));
        information->id = index;
        information->flags = CLAP_PARAM_IS_AUTOMATABLE;
        information->min_value = settings[index].min;
        information->max_value = settings[index].max;
        information->default_value = settings[index].defaultValue;
        snprintf(information->name, sizeof(information->name), "%s", settings[index].name);
        return true;
    },

    .get_value = [](const clap_plugin_t *_plugin, clap_id id, double *value) -> bool
    {
        if (id >= P_COUNT)
            return false;
        *value = PluginGetValue((MyPlugin *)_plugin->plugin_data, id);
        return true;
    },

    .value_to_text = [](const clap_plugin_t *_plugin, clap_id id, double value, char *display, uint32_t size) -> bool
    {
        if (id >= P_COUNT)
            return false;
        snprintf(display, size, settings[id].format, value);
        return true;
    },

    .text_to_value = [](const clap_plugin_t *_plugin, clap_id id, const char *display, double *value) -> bool
    {
        if (id >= P_COUNT)
            return false;
        char *end = nullptr;
        *value = strtod(display, &end); // "1000 Hz" reads as 1000
        return end != display;
    },

    .flush = [](const clap_plugin_t *_plugin, const clap_input_events_t *in, const clap_output_events_t *out)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        PluginSyncMainToAudio(plugin, out);
        for (uint32_t i = 0, n = in->size(in); i < n; i++)
            PluginProcessEvent(plugin, in->get(in, i));
    },
};

// The state is a CBOR map (core/messages.h): a version, each parameter by
// name, and the interface's data.
static const clap_plugin_state_t extensionState = {
    .save = [](const clap_plugin_t *_plugin, const clap_ostream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        PluginSyncAudioToMain(plugin);
        core::Value::Map state{{"version", 1}, {"data", plugin->data}};
        for (uint32_t i = 0; i < P_COUNT; i++)
            state[settings[i].name] = PluginGetValue(plugin, i);
        const auto bytes = core::encode(state);
        const unsigned char *data = bytes.data();
        uint64_t remaining = bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write(stream, data, remaining);
            if (written <= 0)
                return false;
            data += written;
            remaining -= (uint64_t)written;
        }
        return true;
    },

    .load = [](const clap_plugin_t *_plugin, const clap_istream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        std::vector<unsigned char> bytes;
        unsigned char buffer[256];
        for (int64_t read; (read = stream->read(stream, buffer, sizeof(buffer))) > 0;)
        {
            bytes.insert(bytes.end(), buffer, buffer + read);
            if (bytes.size() > core::maxMessageBytes)
                return false;
        }
        auto state = core::decode(bytes.data(), bytes.size());
        if (!state || (*state)["version"].number() < 1)
            return false;
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Setting &setting = settings[i];
            const double value = (*state)[setting.name].number(setting.defaultValue);
            plugin->mainParameters[i].store((float)std::clamp(value, setting.min, setting.max), std::memory_order_relaxed);
            plugin->mainChanged[i].store(true, std::memory_order_release);
        }
        plugin->data = (*state)["data"];
        if (plugin->hostParams)
        {
            plugin->hostParams->rescan(plugin->host, CLAP_PARAM_RESCAN_VALUES);
            plugin->hostParams->request_flush(plugin->host);
        }
        PluginSendValues(plugin);
        plugin->gui.send(core::Value::Map{{"type", "data"}, {"data", plugin->data}});
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

// The size to report to clap.gui, clamped so get_size never reports a size set_size refuses.
static void guiSize(const MyPlugin *plugin, uint32_t &width, uint32_t &height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    width = uint32_t(std::lround(plugin->guiWidth * scale));
    height = uint32_t(std::lround(plugin->guiHeight * scale));
    clampGuiSize(plugin, width, height);
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
        uint32_t width, height;
        guiSize(plugin, width, height);
        plugin->gui.setSize(width, height);
        return true;
    },

    .destroy = [](const clap_plugin_t *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->gui.destroy();
    },

    .set_scale = [](const clap_plugin_t *_plugin, double scale) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.setScale(scale); },

    .get_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        guiSize(plugin, *width, *height);
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
        plugin->guiWidth = width / plugin->gui.pixelsPerPoint();
        plugin->guiHeight = height / plugin->gui.pixelsPerPoint();
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
//                 {type: "begin" | "end", id}     a drag starts or ends
//                 {type: "change", id, value}
//                 {type: "edit", name, data}      a finished edit of the other state
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default}]}
//                 {type: "values", values: [value per parameter id]}
//                 {type: "data", data}            the other state, after a load or undo
static bool PluginReceiveMessage(MyPlugin *plugin, const core::Value &message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        core::Value::Array list;
        for (uint32_t i = 0; i < P_COUNT; i++)
            list.push_back(core::Value::Map{{"id", (double)i}, {"name", settings[i].name}, {"min", settings[i].min},
                                            {"max", settings[i].max}, {"default", settings[i].defaultValue}});
        plugin->gui.send(core::Value::Map{{"type", "params"}, {"params", list}});
        PluginSendValues(plugin);
        if (!std::holds_alternative<std::monostate>(plugin->data.data))
            plugin->gui.send(core::Value::Map{{"type", "data"}, {"data", plugin->data}});
        return true;
    }
    if (type == "edit")
    {
        plugin->data = message["data"];
        const std::string name(message["name"].text());
        if (plugin->hostUndo)
            plugin->hostUndo->change_made(plugin->host, name.empty() ? "Edit" : name.c_str(), nullptr, 0, false);
        else if (plugin->hostState)
            plugin->hostState->mark_dirty(plugin->host);
        return true;
    }

    const double id = message["id"].number(-1);
    if (!(id >= 0 && id < P_COUNT))
        return false;
    const uint32_t i = (uint32_t)id;
    if (type == "change")
    {
        const Setting &setting = settings[i];
        const double value = message["value"].number(setting.defaultValue);
        plugin->mainParameters[i].store((float)std::clamp(value, setting.min, setting.max), std::memory_order_relaxed);
        plugin->mainChanged[i].store(true, std::memory_order_release);
    }
    else if (type == "begin")
        plugin->gestureBegin[i].store(true, std::memory_order_release);
    else if (type == "end")
        plugin->gestureEnd[i].store(true, std::memory_order_release);
    else
        return false;

    // The host then calls process() or flush(), which send these on to it.
    if (plugin->hostParams)
    {
        plugin->hostParams->request_flush(plugin->host);
        if (type == "change")
            plugin->hostParams->rescan(plugin->host, CLAP_PARAM_RESCAN_VALUES);
    }
    return true;
}

static void PluginSendValues(MyPlugin *plugin)
{
    core::Value::Array values;
    for (uint32_t i = 0; i < P_COUNT; i++)
        values.push_back(PluginGetValue(plugin, i));
    plugin->gui.send(core::Value::Map{{"type", "values"}, {"values", values}});
}

// The main thread's view of a value: its own edit if it has an unsent one,
// else the audio thread's.
static double PluginGetValue(MyPlugin *plugin, uint32_t i)
{
    return plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
}

static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event)
{
    if (event->space_id != CLAP_CORE_EVENT_SPACE_ID || event->type != CLAP_EVENT_PARAM_VALUE)
        return;
    const auto *valueEvent = (const clap_event_param_value_t *)event;
    if (valueEvent->param_id >= P_COUNT)
        return;
    const uint32_t i = valueEvent->param_id;
    plugin->parameters[i].store((float)valueEvent->value, std::memory_order_relaxed);
    plugin->changed[i].store(true, std::memory_order_release);
    if (!plugin->uiDirty.exchange(true, std::memory_order_acq_rel))
        plugin->host->request_callback(plugin->host); // on_main_thread tells the interface
}

static void PluginSendGesture(const clap_output_events_t *out, uint32_t i, uint16_t type)
{
    clap_event_param_gesture_t event = {};
    event.header.size = sizeof(event);
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    event.param_id = i;
    out->try_push(out, &event.header);
}

// The interface's edits to the host, in order: begin, values, end.
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        if (plugin->gestureBegin[i].exchange(false, std::memory_order_acquire))
            PluginSendGesture(out, i, CLAP_EVENT_PARAM_GESTURE_BEGIN);
        if (plugin->mainChanged[i].exchange(false, std::memory_order_acquire))
        {
            const float value = plugin->mainParameters[i].load(std::memory_order_relaxed);
            plugin->parameters[i].store(value, std::memory_order_relaxed);
            clap_event_param_value_t event = {};
            event.header.size = sizeof(event);
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.param_id = i;
            event.note_id = -1;
            event.port_index = -1;
            event.channel = -1;
            event.key = -1;
            event.value = value;
            out->try_push(out, &event.header);
        }
        if (plugin->gestureEnd[i].exchange(false, std::memory_order_acquire))
            PluginSendGesture(out, i, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

static void PluginSyncAudioToMain(MyPlugin *plugin)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
        if (plugin->changed[i].exchange(false, std::memory_order_acquire))
            plugin->mainParameters[i].store(plugin->parameters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [](const clap_plugin *_plugin) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->hostParams = (const clap_host_params_t *)plugin->host->get_extension(plugin->host, CLAP_EXT_PARAMS);
        plugin->hostState = (const clap_host_state_t *)plugin->host->get_extension(plugin->host, CLAP_EXT_STATE);
        plugin->hostUndo = (const clap_host_undo_t *)plugin->host->get_extension(plugin->host, CLAP_EXT_UNDO);
        plugin->gui.init(_plugin, plugin->host);
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            plugin->parameters[i].store((float)settings[i].defaultValue, std::memory_order_relaxed);
            plugin->mainParameters[i].store((float)settings[i].defaultValue, std::memory_order_relaxed);
        }
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
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        PluginSyncMainToAudio(plugin, process->out_events);
        for (uint32_t i = 0, n = process->in_events->size(process->in_events); i < n; i++)
            PluginProcessEvent(plugin, process->in_events->get(process->in_events, i));

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
        if (0 == strcmp(id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp(id, CLAP_EXT_STATE))
            return &extensionState;
        if (0 == strcmp(id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp(id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp(id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (plugin->uiDirty.exchange(false, std::memory_order_acq_rel))
            PluginSendValues(plugin);
    },
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

#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/ext/draft/webview.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <atomic>
#include "chardsp_StateVariableFilter.h"
#include "webview/gui.h"
#include "webview/resources.h"

static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// Parameters.
#define P_CUTOFF (0)
#define P_Q (1)
#define P_COUNT (2)

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    float sampleRate;
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    const clap_host_params_t *hostParams;
    chardsp::StateVariableFilter<float> filterL, filterR;

    // Web UI. The UI edits parameters from the main thread through
    // mainParameters/mainChanged, plus these gesture flags.
    webview::Gui gui;
    uint32_t guiWidth = 480, guiHeight = 300;
    std::atomic<bool> gestureBegin[P_COUNT], gestureEnd[P_COUNT];
    std::atomic<bool> uiDirty; // set when the host changes a parameter
};

// The params/state extension tables below refer to these helpers before
// their full definitions later in this file, so declare them here first.
static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event);
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out);
static bool PluginSyncAudioToMain(MyPlugin *plugin);
static double PluginGetValue(MyPlugin *plugin, uint32_t i);
static bool PluginReceiveMessage(MyPlugin *plugin, const webview::Value &message);
static void PluginSendValues(MyPlugin *plugin);

static const char *const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "charlieculbert.simpleCharClap",
    .name = "simpleCharClap",
    .vendor = "charlieculbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "1.0.0",
    .description = "A clap plug demoing a simple SVF.",

    .features = pluginFeatures,
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [](const clap_plugin_t *plugin, bool isInput) -> uint32_t
    {
        return isInput ? 1 : 1;
    },

    .get = [](const clap_plugin_t *plugin, uint32_t index, bool isInput, clap_audio_port_info_t *info) -> bool
    {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf(info->name, sizeof(info->name), "%s", (isInput ? "Audio Input" : "Audio Output"));
        return true;
    },
};

static const clap_plugin_params_t extensionParams = {
    .count = [](const clap_plugin_t *plugin) -> uint32_t
    {
        return P_COUNT;
    },

    .get_info = [](const clap_plugin_t *_plugin, uint32_t index, clap_param_info_t *information) -> bool
    {
        if (index == P_CUTOFF)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE;
            information->min_value = 20.0f;
            information->max_value = 15000.0f;
            information->default_value = 1000.0f;
            strcpy(information->name, "Cutoff");
            return true;
        }
        else if (index == P_Q)
        {
            memset(information, 0, sizeof(clap_param_info_t));
            information->id = index;
            information->flags = CLAP_PARAM_IS_AUTOMATABLE;
            information->min_value = 0.0f;
            information->max_value = 10.0f;
            information->default_value = 0.5f;
            strcpy(information->name, "Q");
            return true;
        }
        else
        {
            return false;
        }
    },

    .get_value = [](const clap_plugin_t *_plugin, clap_id id, double *value) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        uint32_t i = (uint32_t)id;
        if (i >= P_COUNT)
            return false;

        *value = PluginGetValue(plugin, i);
        return true;
    },

    .value_to_text = [](const clap_plugin_t *_plugin, clap_id id, double value, char *display, uint32_t size)
    {
        uint32_t i = (uint32_t) id;
        if (i >= P_COUNT) return false;
        if (i == P_CUTOFF)
        {
            snprintf(display, size, "%f Hz", value);
            return true;
        }
        if (i == P_Q)
        {
            snprintf(display, size, "%f", value);
            return true;
        }
        return false; },

    .text_to_value = [](const clap_plugin_t *_plugin, clap_id param_id, const char *display, double *value)
    {
        // TODO Implement this.
        return false; },

    .flush = [](const clap_plugin_t *_plugin, const clap_input_events_t *in, const clap_output_events_t *out)
    { MyPlugin *plugin = (MyPlugin *) _plugin->plugin_data; const uint32_t eventCount = in->size(in);

        // For parameters that have been modified by the main thread, send CLAP_EVENT_PARAM_VALUE events to the host.
        PluginSyncMainToAudio(plugin, out);

        // Process events sent to our plugin from the host.
        for (uint32_t eventIndex = 0; eventIndex < eventCount; eventIndex++) {
            PluginProcessEvent(plugin, in->get(in, eventIndex));
        } },
};

static const clap_plugin_state_t extensionState = {
    .save = [](const clap_plugin_t *_plugin, const clap_ostream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        // Synchronize any changes from the audio thread (that is, parameter values sent to us by the host)
        // before we save the state of the plugin.
        PluginSyncAudioToMain(plugin);

        float snapshot[P_COUNT];
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            snapshot[i] = plugin->mainParameters[i].load(std::memory_order_relaxed);
        }

        return sizeof(float) * P_COUNT == stream->write(stream, snapshot, sizeof(float) * P_COUNT);
    },

    .load = [](const clap_plugin_t *_plugin, const clap_istream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        float loaded[P_COUNT];
        bool success = sizeof(float) * P_COUNT == stream->read(stream, loaded, sizeof(float) * P_COUNT);

        if (success)
        {
            for (uint32_t i = 0; i < P_COUNT; i++)
            {
                plugin->mainParameters[i].store(loaded[i], std::memory_order_relaxed);
                plugin->mainChanged[i].store(true, std::memory_order_release);
            }
            if (plugin->hostParams)
                plugin->hostParams->rescan(plugin->host, CLAP_PARAM_RESCAN_VALUES);
            PluginSendValues(plugin);
        }

        return success;
    },
};

// The UI is one web page, in the webUI folder. clap.webview gives it to the
// host: its start URI, its files, and messages from it. Hosts that show pages
// themselves use this directly; for the others, webview::Gui (behind clap.gui
// below) shows the same page through this same table.
static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [](const clap_plugin_t *_plugin, char *uri, uint32_t capacity) -> int32_t
    {
        static const char start[] = "/index.html";
        if (capacity)
            snprintf(uri, capacity, "%s", start);
        return sizeof(start); // including the terminating zero
    },

    .get_resource = [](const clap_plugin_t *_plugin, const char *path, char *mime, uint32_t mimeCapacity,
                       const clap_ostream_t *stream) -> bool
    {
        auto resource = webview::readResource(path);
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
        auto message = webview::decode(buffer, size);
        return message && PluginReceiveMessage(plugin, *message);
    },
};

// clap.gui: size policy is ours; creating and embedding the view is webview::Gui's.
#define GUI_MIN_WIDTH (320)
#define GUI_MIN_HEIGHT (200)
#define GUI_MAX_WIDTH (1600)
#define GUI_MAX_HEIGHT (1200)

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
        plugin->gui.setSize(plugin->guiWidth, plugin->guiHeight);
        return true;
    },

    .destroy = [](const clap_plugin_t *_plugin)
    { ((MyPlugin *)_plugin->plugin_data)->gui.destroy(); },

    .set_scale = [](const clap_plugin_t *_plugin, double scale) -> bool
    { return false; }, // WebViews scale their content themselves

    .get_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        *width = plugin->guiWidth;
        *height = plugin->guiHeight;
        return true;
    },

    .can_resize = [](const clap_plugin_t *_plugin) -> bool
    { return true; },

    .get_resize_hints = [](const clap_plugin_t *_plugin, clap_gui_resize_hints_t *hints) -> bool
    {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },

    .adjust_size = [](const clap_plugin_t *_plugin, uint32_t *width, uint32_t *height) -> bool
    {
        *width = *width < GUI_MIN_WIDTH ? GUI_MIN_WIDTH : *width > GUI_MAX_WIDTH ? GUI_MAX_WIDTH : *width;
        *height = *height < GUI_MIN_HEIGHT ? GUI_MIN_HEIGHT : *height > GUI_MAX_HEIGHT ? GUI_MAX_HEIGHT : *height;
        return true;
    },

    .set_size = [](const clap_plugin_t *_plugin, uint32_t width, uint32_t height) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        if (width < GUI_MIN_WIDTH || width > GUI_MAX_WIDTH || height < GUI_MIN_HEIGHT || height > GUI_MAX_HEIGHT)
            return false;
        plugin->guiWidth = width;
        plugin->guiHeight = height;
        plugin->gui.setSize(width, height);
        return true;
    },

    .set_parent = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.setParent(window); },

    .set_transient = [](const clap_plugin_t *_plugin, const clap_window_t *window) -> bool
    { return false; },

    .suggest_title = [](const clap_plugin_t *_plugin, const char *title) {},

    .show = [](const clap_plugin_t *_plugin) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.show(); },

    .hide = [](const clap_plugin_t *_plugin) -> bool
    { return ((MyPlugin *)_plugin->plugin_data)->gui.hide(); },
};

// Linux's native WebView needs a main-thread timer; see webview::Gui::onTimer.
static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [](const clap_plugin_t *_plugin, clap_id timerId)
    { ((MyPlugin *)_plugin->plugin_data)->gui.onTimer(timerId); },
};

// Messages between the UI and the plugin (main thread only):
//   UI -> plugin  {type: "ready"}
//                 {type: "begin" | "end", id}     a drag starts or ends
//                 {type: "change", id, value}
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default}]}
//                 {type: "values", values: [value per parameter id]}
static bool PluginReceiveMessage(MyPlugin *plugin, const webview::Value &message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        webview::Value::Array params;
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info(&plugin->plugin, i, &information);
            params.push_back(webview::Value::Map{
                {"id", (double)information.id}, {"name", information.name},
                {"min", information.min_value}, {"max", information.max_value},
                {"default", information.default_value}});
        }
        plugin->gui.send(webview::Value::Map{{"type", "params"}, {"params", params}});
        PluginSendValues(plugin);
        return true;
    }

    const double id = message["id"].number(-1);
    if (!(id >= 0 && id < P_COUNT))
        return false;
    const uint32_t i = (uint32_t)id;

    if (type == "change")
    {
        clap_param_info_t information = {};
        extensionParams.get_info(&plugin->plugin, i, &information);
        const double value = message["value"].number(information.default_value);
        plugin->mainParameters[i].store((float)fmin(fmax(value, information.min_value), information.max_value),
                                        std::memory_order_relaxed);
        plugin->mainChanged[i].store(true, std::memory_order_release);
    }
    else if (type == "begin")
        plugin->gestureBegin[i].store(true, std::memory_order_release);
    else if (type == "end")
        plugin->gestureEnd[i].store(true, std::memory_order_release);
    else
        return false;

    // The host then calls process() or flush(), which send these to the host.
    if (plugin->hostParams)
        plugin->hostParams->request_flush(plugin->host);
    return true;
}

static void PluginSendValues(MyPlugin *plugin)
{
    webview::Value::Array values;
    for (uint32_t i = 0; i < P_COUNT; i++)
        values.push_back(PluginGetValue(plugin, i));
    plugin->gui.send(webview::Value::Map{{"type", "values"}, {"values", values}});
}

// The current value on the main thread: the audio thread's value, unless the
// main thread has changed it since. get_value and the UI both use this.
static double PluginGetValue(MyPlugin *plugin, uint32_t i)
{
    return plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
}

static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event)
{
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
    {
        if (event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t *valueEvent = (const clap_event_param_value_t *)event;
            if (valueEvent->param_id >= P_COUNT)
                return;
            uint32_t i = (uint32_t)valueEvent->param_id;
            plugin->parameters[i].store(valueEvent->value, std::memory_order_relaxed);
            plugin->changed[i].store(true, std::memory_order_release);

            // Tell the UI on the main thread (see on_main_thread).
            if (!plugin->uiDirty.exchange(true, std::memory_order_acq_rel))
                plugin->host->request_callback(plugin->host);
        }
    }
}

static void PluginSendGesture(const clap_output_events_t *out, uint32_t i, uint16_t type)
{
    clap_event_param_gesture_t event = {};
    event.header.size = sizeof(event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    event.header.flags = 0;
    event.param_id = i;
    out->try_push(out, &event.header);
}

static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        // A UI drag: begin, then values, then end, in that order.
        if (plugin->gestureBegin[i].exchange(false, std::memory_order_acquire))
            PluginSendGesture(out, i, CLAP_EVENT_PARAM_GESTURE_BEGIN);

        if (plugin->mainChanged[i].exchange(false, std::memory_order_acquire))
        {
            const float value = (plugin->mainParameters[i].load(std::memory_order_relaxed));
            plugin->parameters[i].store(value, std::memory_order_relaxed);
            clap_event_param_value_t event = {};
            event.header.size = sizeof(event);
            event.header.time = 0;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_PARAM_VALUE;
            event.header.flags = 0;
            event.param_id = i;
            event.cookie = NULL;
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

static bool PluginSyncAudioToMain(MyPlugin *plugin)
{
    bool anyChanged = false;

    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        if (plugin->changed[i].exchange(false, std::memory_order_acquire))
        {
            plugin->mainParameters[i].store(plugin->parameters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            anyChanged = true;
        }
    }

    return anyChanged;
}

static void PluginRenderAudio(MyPlugin *plugin, uint32_t start, uint32_t end,
                              float *const *input, float *const *output)
{
    float cutoff = plugin->parameters[P_CUTOFF].load(std::memory_order_relaxed);
    float q = plugin->parameters[P_Q].load(std::memory_order_relaxed);
    plugin->filterL.setCutoffAndQ(cutoff, q);
    plugin->filterR.setCutoffAndQ(cutoff, q);

    for (uint32_t index = start; index < end; index++)
    {
            output[0][index] = plugin->filterL.process(input[0][index]);
            output[1][index] = plugin->filterR.process(input[1][index]);
    }
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,
    .init = [](const clap_plugin *_plugin) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        plugin->hostParams = (const clap_host_params_t *)plugin->host->get_extension(plugin->host, CLAP_EXT_PARAMS);
        plugin->gui.init(_plugin, plugin->host);

        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info(_plugin, i, &information);
            float defaultValue = information.default_value;
            plugin->mainParameters[i].store(defaultValue, std::memory_order_relaxed);
            plugin->parameters[i].store(defaultValue, std::memory_order_relaxed);
        }

        return true;
    },
    .destroy = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *) _plugin->plugin_data;
        plugin->gui.destroy();
        delete plugin; },
    .activate = [](const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount, uint32_t maximumFramesCount) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        plugin->sampleRate = sampleRate;
        plugin->filterL.prepare((float)sampleRate);
        plugin->filterR.prepare((float)sampleRate);
        return true;
    },
    .deactivate = [](const clap_plugin *_plugin) {},

    .start_processing = [](const clap_plugin *_plugin) -> bool
    {
        return true;
    },

    .stop_processing = [](const clap_plugin *_plugin) {},

    .reset = [](const clap_plugin *_plugin)
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;
        // reset stuff here
        plugin->filterL.reset();
        plugin->filterR.reset();
    },

    .process = [](const clap_plugin *_plugin, const clap_process_t *process) -> clap_process_status
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        PluginSyncMainToAudio(plugin, process->out_events);

        assert(process->audio_outputs_count == 1);
        assert(process->audio_inputs_count == 1);

        const uint32_t frameCount = process->frames_count;
        const uint32_t inputEventCount = process->in_events->size(process->in_events);
        uint32_t eventIndex = 0;
        uint32_t nextEventFrame = inputEventCount ? 0 : frameCount;

        for (uint32_t i = 0; i < frameCount;)
        {
            while (eventIndex < inputEventCount && nextEventFrame == i)
            {
                const clap_event_header_t *event = process->in_events->get(process->in_events, eventIndex);

                if (event->time != i)
                {
                    nextEventFrame = event->time;
                    break;
                }

                PluginProcessEvent(plugin, event);
                eventIndex++;

                if (eventIndex == inputEventCount)
                {
                    nextEventFrame = frameCount;
                    break;
                }
            }

            PluginRenderAudio(
                plugin, i, nextEventFrame,
                process->audio_inputs[0].data32,
                process->audio_outputs[0].data32);
            i = nextEventFrame;
        }

        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [](const clap_plugin *plugin, const char *id) -> const void *
    {
        if (0 == strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp(id, CLAP_EXT_PARAMS)) return &extensionParams; // New!
        if (0 == strcmp(id, CLAP_EXT_STATE))
            return &extensionState; // New!
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

// Functions available to the entry via plugin.h
const clap_plugin_descriptor_t *getPluginDescriptor()
{
    return &pluginDescriptor;
};

const clap_plugin_t *createPlugin(const clap_host_t *host)
{
    MyPlugin *plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
};

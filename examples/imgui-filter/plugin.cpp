// Laid out as nakst's CLAP tutorial: https://nakst.gitlab.io/tutorial/clap-part-1.html
//
// ImGui Filter: a stereo state-variable filter (lowpass, bandpass or highpass)
// whose interface is drawn with Dear ImGui (ui/FilterView.cpp) on WebGPU.
//
// This file is the whole CLAP plugin: its descriptor, audio ports, parameters,
// state, the clap.webview and clap.gui extensions its interface uses, and the
// audio processing; entry.cpp is the entry point that makes it. libs/ holds the
// reusable parts: the message codec and file loader (core/), and the presenter
// that shows a gpu::View in the host's window (gpu/).
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
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
#include <algorithm>
#include <cmath>
#include <atomic>
#include <vector>
#include "chardsp_StateVariableFilter.h"
#include "core/messages.h"
#include "core/resources.h"
#include "gpu/gui.h"

static_assert (std::atomic<float>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);

// Parameters.
#define P_TYPE (0) // 0 lowpass, 1 bandpass, 2 highpass
#define P_CUTOFF (1)
#define P_RESONANCE (2)
#define P_COUNT (3)

static const char* const filterTypes[] = { "Lowpass", "Bandpass", "Highpass" };

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    float sampleRate;
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    const clap_host_params_t* hostParams;
    chardsp::StateVariableFilter<float> filterL, filterR;

    // The interface. It edits parameters from the main thread through
    // mainParameters/mainChanged, plus these gesture flags.
    gpu::Gui gui;
    float guiWidth = 420, guiHeight = 160; // points, so the window keeps its size across scales
    std::atomic<bool> gestureBegin[P_COUNT], gestureEnd[P_COUNT];
    std::atomic<bool> uiDirty; // set when the host changes a parameter
};

// The params/state extension tables below refer to these helpers before
// their full definitions later in this file, so declare them here first.
static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event);
static void PluginSyncMainToAudio (MyPlugin* plugin, const clap_output_events_t* out);
static bool PluginSyncAudioToMain (MyPlugin* plugin);
static double PluginGetValue (MyPlugin* plugin, uint32_t i);
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message);
static void PluginSendValues (MyPlugin* plugin);

static const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_FILTER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.imguifilter",
    .name = "ImGui Filter",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "1.0.0",
    .description = "A state-variable filter with a Dear ImGui interface.",

    .features = pluginFeatures,
};

static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput ? 1 : 1; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_audio_port_info_t* info) -> bool {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf (info->name, sizeof (info->name), "%s", (isInput ? "Audio Input" : "Audio Output"));
        return true;
    },
};

static const clap_plugin_params_t extensionParams = {
    .count = [] (const clap_plugin_t* plugin) -> uint32_t { return P_COUNT; },

    .get_info = [] (const clap_plugin_t* _plugin, uint32_t index, clap_param_info_t* information) -> bool {
        if (index >= P_COUNT)
            return false;
        memset (information, 0, sizeof (clap_param_info_t));
        information->id = index;
        information->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (index == P_TYPE)
        {
            information->flags |= CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
            information->min_value = 0;
            information->max_value = 2;
            information->default_value = 0;
            strcpy (information->name, "Type");
        }
        else if (index == P_CUTOFF)
        {
            information->min_value = 20.0;
            information->max_value = 20000.0;
            information->default_value = 1000.0;
            strcpy (information->name, "Cutoff");
        }
        else
        {
            information->min_value = 0.1;
            information->max_value = 10.0;
            information->default_value = 0.707;
            strcpy (information->name, "Resonance");
        }
        return true;
    },

    .get_value = [] (const clap_plugin_t* _plugin, clap_id id, double* value) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t i = (uint32_t) id;
        if (i >= P_COUNT)
            return false;

        *value = PluginGetValue (plugin, i);
        return true;
    },

    .value_to_text = [] (const clap_plugin_t* _plugin, clap_id id, double value, char* display, uint32_t size) -> bool {
        if (id == P_TYPE)
            snprintf (display, size, "%s", filterTypes[(int) fmin (fmax (value, 0), 2)]);
        else if (id == P_CUTOFF)
            snprintf (display, size, "%.0f Hz", value);
        else if (id == P_RESONANCE)
            snprintf (display, size, "%.2f", value);
        else
            return false;
        return true;
    },

    .text_to_value = [] (const clap_plugin_t* _plugin, clap_id id, const char* display, double* value) -> bool {
        if (id == P_TYPE)
        {
            for (int i = 0; i < 3; i++)
                if (strcmp (display, filterTypes[i]) == 0)
                    return *value = i, true;
            return false;
        }
        if (id >= P_COUNT)
            return false;
        char* end = nullptr;
        *value = strtod (display, &end); // "1000 Hz" reads as 1000
        return end != display;
    },

    .flush =
        [] (const clap_plugin_t* _plugin, const clap_input_events_t* in, const clap_output_events_t* out) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            const uint32_t eventCount = in->size (in);

            // For parameters that have been modified by the main thread, send CLAP_EVENT_PARAM_VALUE events to the host.
            PluginSyncMainToAudio (plugin, out);

            // Process events sent to our plugin from the host.
            for (uint32_t eventIndex = 0; eventIndex < eventCount; eventIndex++)
            {
                PluginProcessEvent (plugin, in->get (in, eventIndex));
            }
        },
};

// The state is the parameters as a CBOR map (core/messages.h) from name to
// value, with a version, so later versions can add parameters and still read
// this one.
static const clap_plugin_state_t extensionState = {
    .save = [] (const clap_plugin_t* _plugin, const clap_ostream_t* stream) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        // Synchronize any changes from the audio thread (that is, parameter values sent to us by the host)
        // before we save the state of the plugin.
        PluginSyncAudioToMain (plugin);

        core::Value::Map state { { "version", 1 } };
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info (_plugin, i, &information);
            state[information.name] = (double) plugin->mainParameters[i].load (std::memory_order_relaxed);
        }
        const auto bytes = core::encode (state);
        const unsigned char* data = bytes.data();
        uint64_t remaining = bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, data, remaining);
            if (written <= 0)
                return false;
            data += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .load = [] (const clap_plugin_t* _plugin, const clap_istream_t* stream) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        std::vector<unsigned char> bytes;
        unsigned char buffer[256];
        for (int64_t read; (read = stream->read (stream, buffer, sizeof (buffer))) > 0;)
        {
            bytes.insert (bytes.end(), buffer, buffer + read);
            if (bytes.size() > core::maxMessageBytes)
                return false;
        }
        auto state = core::decode (bytes.data(), bytes.size());
        if (!state || (*state)["version"].number() < 1)
            return false;

        // A parameter missing from the state keeps its default.
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info (_plugin, i, &information);
            const double value = (*state)[information.name].number (information.default_value);
            plugin->mainParameters[i].store (
                (float) fmin (fmax (value, information.min_value), information.max_value), std::memory_order_relaxed);
            plugin->mainChanged[i].store (true, std::memory_order_release);
        }
        if (plugin->hostParams)
            plugin->hostParams->rescan (plugin->host, CLAP_PARAM_RESCAN_VALUES);
        PluginSendValues (plugin);
        return true;
    },
};

// The interface talks to the plugin through clap.webview: natively gpu::Gui
// passes its messages here, and in WCLAP the host shows the interface as a web
// page (the same view compiled to wasm), which it loads through get_uri and
// get_resource.
static const clap_plugin_webview_t extensionWebview = {
    .get_uri = [] (const clap_plugin_t* _plugin, char* uri, uint32_t capacity) -> int32_t {
        static const char start[] = "/page/index.html"; // in resources/
        if (capacity)
            snprintf (uri, capacity, "%s", start);
        return sizeof (start); // including the terminating zero
    },

    .get_resource = [] (const clap_plugin_t* _plugin, const char* path, char* mime, uint32_t mimeCapacity,
                        const clap_ostream_t* stream) -> bool {
        auto resource = core::readResource (path);
        if (!resource || resource->mime.size() >= mimeCapacity)
            return false;
        strcpy (mime, resource->mime.c_str());
        const char* bytes = resource->bytes.data();
        uint64_t remaining = resource->bytes.size();
        while (remaining)
        {
            const int64_t written = stream->write (stream, bytes, remaining);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (uint64_t) written;
        }
        return true;
    },

    .receive = [] (const clap_plugin_t* _plugin, const void* buffer, uint32_t size) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        auto message = core::decode (buffer, size);
        return message && PluginReceiveMessage (plugin, *message);
    },
};

// clap.gui: size policy is ours; creating and embedding the view is gpu::Gui's.
// The limits are in points.
#define GUI_MIN_WIDTH (320)
#define GUI_MIN_HEIGHT (140)
#define GUI_MAX_WIDTH (1600)
#define GUI_MAX_HEIGHT (1200)

// Clamps a clap.gui size, which is in pixels outside macOS, to the limits.
static void clampGuiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    const auto clamp = [scale] (uint32_t value, float low, float high) {
        return std::clamp (value, uint32_t (std::ceil (low * scale)), uint32_t (high * scale));
    };
    width = clamp (width, GUI_MIN_WIDTH, GUI_MAX_WIDTH);
    height = clamp (height, GUI_MIN_HEIGHT, GUI_MAX_HEIGHT);
}

// The size to report to clap.gui, clamped so get_size never reports a size set_size refuses.
static void guiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    width = uint32_t (std::lround (plugin->guiWidth * scale));
    height = uint32_t (std::lround (plugin->guiHeight * scale));
    clampGuiSize (plugin, width, height);
}

static const clap_plugin_gui_t extensionGui = {
    .is_api_supported = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.isApiSupported (api, isFloating);
    },

    .get_preferred_api = [] (const clap_plugin_t* _plugin, const char** api, bool* isFloating) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.getPreferredApi (api, isFloating);
    },

    .create = [] (const clap_plugin_t* _plugin, const char* api, bool isFloating) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        if (!plugin->gui.create (api, isFloating))
            return false;
        uint32_t width, height;
        guiSize (plugin, width, height);
        plugin->gui.setSize (width, height);
        return true;
    },

    .destroy = [] (const clap_plugin_t* _plugin) { ((MyPlugin*) _plugin->plugin_data)->gui.destroy(); },

    .set_scale = [] (const clap_plugin_t* _plugin, double scale) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.setScale (scale);
    },

    .get_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        guiSize (plugin, *width, *height);
        return true;
    },

    .can_resize = [] (const clap_plugin_t* _plugin) -> bool { return true; },

    .get_resize_hints = [] (const clap_plugin_t* _plugin, clap_gui_resize_hints_t* hints) -> bool {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        return true;
    },

    .adjust_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool {
        clampGuiSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
        return true;
    },

    .set_size = [] (const clap_plugin_t* _plugin, uint32_t width, uint32_t height) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t allowedWidth = width, allowedHeight = height;
        clampGuiSize (plugin, allowedWidth, allowedHeight);
        if (allowedWidth != width || allowedHeight != height)
            return false;
        plugin->guiWidth = width / plugin->gui.pixelsPerPoint();
        plugin->guiHeight = height / plugin->gui.pixelsPerPoint();
        plugin->gui.setSize (width, height);
        return true;
    },

    .set_parent = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.setParent (window);
    },

    .set_transient = [] (const clap_plugin_t* _plugin, const clap_window_t* window) -> bool { return false; },

    .suggest_title = [] (const clap_plugin_t* _plugin, const char* title) {},

    .show = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.show(); },

    .hide = [] (const clap_plugin_t* _plugin) -> bool { return ((MyPlugin*) _plugin->plugin_data)->gui.hide(); },
};

// On Linux the host's timer drives the view's events and frames; see gpu::Gui::onTimer.
static const clap_plugin_timer_support_t extensionTimerSupport = {
    .on_timer = [] (const clap_plugin_t* _plugin,
                    clap_id timerId) { ((MyPlugin*) _plugin->plugin_data)->gui.onTimer (timerId); },
};

// Messages between the UI and the plugin (main thread only):
//   UI -> plugin  {type: "ready"}
//                 {type: "begin" | "end", id}     a drag starts or ends
//                 {type: "change", id, value}
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default}]}
//                 {type: "values", values: [value per parameter id]}
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        core::Value::Array params;
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info (&plugin->plugin, i, &information);
            params.push_back (core::Value::Map { { "id", (double) information.id }, { "name", information.name },
                { "min", information.min_value }, { "max", information.max_value },
                { "default", information.default_value } });
        }
        plugin->gui.send (core::Value::Map { { "type", "params" }, { "params", params } });
        PluginSendValues (plugin);
        return true;
    }

    const double id = message["id"].number (-1);
    if (!(id >= 0 && id < P_COUNT))
        return false;
    const uint32_t i = (uint32_t) id;

    if (type == "change")
    {
        clap_param_info_t information = {};
        extensionParams.get_info (&plugin->plugin, i, &information);
        const double value = message["value"].number (information.default_value);
        plugin->mainParameters[i].store (
            (float) fmin (fmax (value, information.min_value), information.max_value), std::memory_order_relaxed);
        plugin->mainChanged[i].store (true, std::memory_order_release);
    }
    else if (type == "begin")
        plugin->gestureBegin[i].store (true, std::memory_order_release);
    else if (type == "end")
        plugin->gestureEnd[i].store (true, std::memory_order_release);
    else
        return false;

    // The host then calls process() or flush(), which send these to the host
    // as gesture and value events. (Not a values rescan: that's for a preset
    // load, and as a VST3 or AU it makes the host re-read every parameter.)
    if (plugin->hostParams)
        plugin->hostParams->request_flush (plugin->host);
    return true;
}

static void PluginSendValues (MyPlugin* plugin)
{
    core::Value::Array values;
    for (uint32_t i = 0; i < P_COUNT; i++)
        values.push_back (PluginGetValue (plugin, i));
    plugin->gui.send (core::Value::Map { { "type", "values" }, { "values", values } });
}

// The current value on the main thread: the audio thread's value, unless the
// main thread has changed it since. get_value and the UI both use this.
static double PluginGetValue (MyPlugin* plugin, uint32_t i)
{
    return plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
}

static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event)
{
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
    {
        if (event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* valueEvent = (const clap_event_param_value_t*) event;
            if (valueEvent->param_id >= P_COUNT)
                return;
            uint32_t i = (uint32_t) valueEvent->param_id;
            // Kept in range, whatever the host sends (get_info only reads a table).
            clap_param_info_t information = {};
            extensionParams.get_info (&plugin->plugin, i, &information);
            const double value = valueEvent->value == valueEvent->value ? valueEvent->value
                                                                        : information.default_value; // NaN: the default
            plugin->parameters[i].store (
                (float) fmin (fmax (value, information.min_value), information.max_value), std::memory_order_relaxed);
            plugin->changed[i].store (true, std::memory_order_release);

            // Tell the UI on the main thread (see on_main_thread).
            if (!plugin->uiDirty.exchange (true, std::memory_order_acq_rel))
                plugin->host->request_callback (plugin->host);
        }
    }
}

static void PluginSendGesture (const clap_output_events_t* out, uint32_t i, uint16_t type)
{
    clap_event_param_gesture_t event = {};
    event.header.size = sizeof (event);
    event.header.time = 0;
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = type;
    event.header.flags = 0;
    event.param_id = i;
    out->try_push (out, &event.header);
}

static void PluginSyncMainToAudio (MyPlugin* plugin, const clap_output_events_t* out)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        // A UI drag: begin, then values, then end, in that order.
        if (plugin->gestureBegin[i].exchange (false, std::memory_order_acquire))
            PluginSendGesture (out, i, CLAP_EVENT_PARAM_GESTURE_BEGIN);

        if (plugin->mainChanged[i].exchange (false, std::memory_order_acquire))
        {
            const float value = (plugin->mainParameters[i].load (std::memory_order_relaxed));
            plugin->parameters[i].store (value, std::memory_order_relaxed);
            clap_event_param_value_t event = {};
            event.header.size = sizeof (event);
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
            out->try_push (out, &event.header);
        }

        if (plugin->gestureEnd[i].exchange (false, std::memory_order_acquire))
            PluginSendGesture (out, i, CLAP_EVENT_PARAM_GESTURE_END);
    }
}

static bool PluginSyncAudioToMain (MyPlugin* plugin)
{
    bool anyChanged = false;

    for (uint32_t i = 0; i < P_COUNT; i++)
    {
        // An edit from the main thread not yet sent to the audio thread is
        // newer than the host's value: keep it.
        if (plugin->changed[i].exchange (false, std::memory_order_acquire)
            && !plugin->mainChanged[i].load (std::memory_order_acquire))
        {
            plugin->mainParameters[i].store (
                plugin->parameters[i].load (std::memory_order_relaxed), std::memory_order_relaxed);
            anyChanged = true;
        }
    }

    return anyChanged;
}

static void PluginRenderAudio (
    MyPlugin* plugin, uint32_t start, uint32_t end, float* const* input, float* const* output)
{
    const auto type = (chardsp::FilterMode) (int) plugin->parameters[P_TYPE].load (std::memory_order_relaxed);
    const float cutoff = plugin->parameters[P_CUTOFF].load (std::memory_order_relaxed);
    const float resonance = plugin->parameters[P_RESONANCE].load (std::memory_order_relaxed);
    for (auto* filter : { &plugin->filterL, &plugin->filterR })
    {
        filter->setMode (type);
        filter->setCutoffAndQ (cutoff, resonance);
    }

    for (uint32_t index = start; index < end; index++)
    {
        output[0][index] = plugin->filterL.process (input[0][index]);
        output[1][index] = plugin->filterR.process (input[1][index]);
    }
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,
    .init = [] (const clap_plugin* _plugin) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        plugin->hostParams = (const clap_host_params_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_PARAMS);
        plugin->gui.init (_plugin, plugin->host);

        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info (_plugin, i, &information);
            float defaultValue = (float) information.default_value;
            plugin->mainParameters[i].store (defaultValue, std::memory_order_relaxed);
            plugin->parameters[i].store (defaultValue, std::memory_order_relaxed);
        }

        return true;
    },
    .destroy =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            plugin->gui.destroy();
            delete plugin;
        },
    .activate = [] (const clap_plugin* _plugin, double sampleRate, uint32_t minimumFramesCount,
                    uint32_t maximumFramesCount) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        plugin->sampleRate = (float) sampleRate;
        plugin->filterL.prepare ((float) sampleRate);
        plugin->filterR.prepare ((float) sampleRate);
        return true;
    },
    .deactivate = [] (const clap_plugin* _plugin) {},

    .start_processing = [] (const clap_plugin* _plugin) -> bool { return true; },

    .stop_processing = [] (const clap_plugin* _plugin) {},

    .reset =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            // reset stuff here
            plugin->filterL.reset();
            plugin->filterR.reset();
        },

    .process = [] (const clap_plugin* _plugin, const clap_process_t* process) -> clap_process_status {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        PluginSyncMainToAudio (plugin, process->out_events);

        assert (process->audio_outputs_count == 1);
        assert (process->audio_inputs_count == 1);

        const uint32_t frameCount = process->frames_count;
        const uint32_t inputEventCount = process->in_events->size (process->in_events);
        uint32_t eventIndex = 0;
        uint32_t nextEventFrame = inputEventCount ? 0 : frameCount;

        for (uint32_t i = 0; i < frameCount;)
        {
            while (eventIndex < inputEventCount && nextEventFrame == i)
            {
                const clap_event_header_t* event = process->in_events->get (process->in_events, eventIndex);

                if (event->time != i)
                {
                    nextEventFrame = event->time;
                    break;
                }

                PluginProcessEvent (plugin, event);
                eventIndex++;

                if (eventIndex == inputEventCount)
                {
                    nextEventFrame = frameCount;
                    break;
                }
            }

            PluginRenderAudio (
                plugin, i, nextEventFrame, process->audio_inputs[0].data32, process->audio_outputs[0].data32);
            i = nextEventFrame;
        }

        return CLAP_PROCESS_CONTINUE;
    },

    .get_extension = [] (const clap_plugin* plugin, const char* id) -> const void* {
        if (0 == strcmp (id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp (id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp (id, CLAP_EXT_STATE))
            return &extensionState;
        if (0 == strcmp (id, CLAP_EXT_WEBVIEW))
            return &extensionWebview;
        if (0 == strcmp (id, CLAP_EXT_GUI))
            return &extensionGui;
        if (0 == strcmp (id, CLAP_EXT_TIMER_SUPPORT))
            return &extensionTimerSupport;
        return nullptr;
    },

    .on_main_thread =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            if (plugin->uiDirty.exchange (false, std::memory_order_acq_rel))
                PluginSendValues (plugin);
        },
};

// What entry.cpp needs.
const clap_plugin_descriptor_t* getPluginDescriptor()
{
    return &pluginDescriptor;
};

const clap_plugin_t* createPlugin (const clap_host_t* host)
{
    MyPlugin* plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
};

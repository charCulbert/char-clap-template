// Headless Gain: stereo gain and pan with no interface of its own. Hosts show
// their own controls for its parameters.
//
// This one file is the whole CLAP plugin: its descriptor, audio ports,
// parameters, state and the audio processing. libs/core holds the entry point
// (entry.cpp) and the CBOR codec its state uses (messages.h).
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <atomic>
#include <vector>
#include "core/messages.h"

static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

// Parameters.
#define P_GAIN (0) // dB
#define P_PAN (1)  // -1 left .. 1 right
#define P_COUNT (2)

// Parameter values live twice: `parameters` on the audio thread, and
// `mainParameters` on the main thread (for get_value and the state), each
// with a flag saying the other thread has news for it.
struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t *host;
    float sampleRate;
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    const clap_host_params_t *hostParams;
};

// The params/state extension tables below refer to these helpers before
// their full definitions later in this file, so declare them here first.
static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event);
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out);
static bool PluginSyncAudioToMain(MyPlugin *plugin);
static double PluginGetValue(MyPlugin *plugin, uint32_t i);

static const char *const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.headlessgain",
    .name = "Headless Gain",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "1.0.0",
    .description = "Stereo gain and pan, with no interface of its own.",

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
        if (index >= P_COUNT)
            return false;
        memset(information, 0, sizeof(clap_param_info_t));
        information->id = index;
        information->flags = CLAP_PARAM_IS_AUTOMATABLE;
        if (index == P_GAIN)
        {
            information->min_value = -60.0;
            information->max_value = 12.0;
            information->default_value = 0.0;
            strcpy(information->name, "Gain");
        }
        else
        {
            information->min_value = -1.0;
            information->max_value = 1.0;
            information->default_value = 0.0;
            strcpy(information->name, "Pan");
        }
        return true;
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

    .value_to_text = [](const clap_plugin_t *_plugin, clap_id id, double value, char *display, uint32_t size) -> bool
    {
        if (id == P_GAIN)
            snprintf(display, size, "%.1f dB", value);
        else if (id == P_PAN)
            snprintf(display, size, value == 0 ? "C" : value < 0 ? "%.0f L" : "%.0f R", fabs(value) * 100);
        else
            return false;
        return true;
    },

    .text_to_value = [](const clap_plugin_t *_plugin, clap_id id, const char *display, double *value) -> bool
    {
        if (id >= P_COUNT)
            return false;
        char *end = nullptr;
        const double number = strtod(display, &end); // "-6 dB" reads as -6
        if (id == P_PAN)
        {
            // "C", "50 L", "50 R"
            if (end == display)
                return display[0] == 'C' ? (*value = 0, true) : false;
            *value = fmin(number, 100) / 100 * (strchr(end, 'L') ? -1 : 1);
            return true;
        }
        *value = number;
        return end != display;
    },

    .flush = [](const clap_plugin_t *_plugin, const clap_input_events_t *in, const clap_output_events_t *out)
    { MyPlugin *plugin = (MyPlugin *) _plugin->plugin_data; const uint32_t eventCount = in->size(in);

        // For parameters that have been modified by the main thread, send CLAP_EVENT_PARAM_VALUE events to the host.
        PluginSyncMainToAudio(plugin, out);

        // Process events sent to our plugin from the host.
        for (uint32_t eventIndex = 0; eventIndex < eventCount; eventIndex++) {
            PluginProcessEvent(plugin, in->get(in, eventIndex));
        } },
};

// The state is the parameters as a CBOR map (core/messages.h) from name to
// value, with a version, so later versions can add parameters and still read
// this one.
static const clap_plugin_state_t extensionState = {
    .save = [](const clap_plugin_t *_plugin, const clap_ostream_t *stream) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        // Synchronize any changes from the audio thread (that is, parameter values sent to us by the host)
        // before we save the state of the plugin.
        PluginSyncAudioToMain(plugin);

        core::Value::Map state{{"version", 1}};
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info(_plugin, i, &information);
            state[information.name] = (double)plugin->mainParameters[i].load(std::memory_order_relaxed);
        }
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

        // A parameter missing from the state keeps its default.
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            clap_param_info_t information = {};
            extensionParams.get_info(_plugin, i, &information);
            const double value = (*state)[information.name].number(information.default_value);
            plugin->mainParameters[i].store((float)fmin(fmax(value, information.min_value), information.max_value),
                                            std::memory_order_relaxed);
            plugin->mainChanged[i].store(true, std::memory_order_release);
        }
        if (plugin->hostParams)
            plugin->hostParams->rescan(plugin->host, CLAP_PARAM_RESCAN_VALUES);
        return true;
    },
};

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
        }
    }
}

static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out)
{
    for (uint32_t i = 0; i < P_COUNT; i++)
    {
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
    const float gain = powf(10.0f, plugin->parameters[P_GAIN].load(std::memory_order_relaxed) / 20.0f);
    const float pan = plugin->parameters[P_PAN].load(std::memory_order_relaxed);
    // A balance control: turning towards one side lowers the other.
    const float left = gain * fminf(1.0f, 1.0f - pan), right = gain * fminf(1.0f, 1.0f + pan);

    for (uint32_t index = start; index < end; index++)
    {
        output[0][index] = input[0][index] * left;
        output[1][index] = input[1][index] * right;
    }
}

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,
    .init = [](const clap_plugin *_plugin) -> bool
    {
        MyPlugin *plugin = (MyPlugin *)_plugin->plugin_data;

        plugin->hostParams = (const clap_host_params_t *)plugin->host->get_extension(plugin->host, CLAP_EXT_PARAMS);

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
        delete (MyPlugin *)_plugin->plugin_data; },
    .activate = [](const clap_plugin *_plugin, double sampleRate, uint32_t minimumFramesCount, uint32_t maximumFramesCount) -> bool
    {
        ((MyPlugin *)_plugin->plugin_data)->sampleRate = (float)sampleRate;
        return true;
    },
    .deactivate = [](const clap_plugin *_plugin) {},

    .start_processing = [](const clap_plugin *_plugin) -> bool
    {
        return true;
    },

    .stop_processing = [](const clap_plugin *_plugin) {},

    .reset = [](const clap_plugin *_plugin) {}, // nothing to clear: gain has no memory

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
        if (0 == strcmp(id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp(id, CLAP_EXT_STATE))
            return &extensionState;
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin) {},
};

// What libs/core/entry.cpp needs (core/plugin.h).
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

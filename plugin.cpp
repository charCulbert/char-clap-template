#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <atomic>
#include "chardsp_StateVariableFilter.h"

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
};

// The params/state extension tables below refer to these helpers before
// their full definitions later in this file, so declare them here first.
static void PluginProcessEvent(MyPlugin *plugin, const clap_event_header_t *event);
static void PluginSyncMainToAudio(MyPlugin *plugin, const clap_output_events_t *out);
static bool PluginSyncAudioToMain(MyPlugin *plugin);

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

        // get_value is called on the main thread, but should return the value of the parameter according to the audio thread,
        // since the value on the audio thread is the one that host communicates with us via CLAP_EVENT_PARAM_VALUE events.
        // And although we need to check the mainChanged array, we mustn't actually modify the parameters array,
        // since that can only be done on the audio thread. Don't worry -- it'll pick up the changes eventually.
        *value = plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
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
        }

        return success;
    },
};

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
        return nullptr;
    },

    .on_main_thread = [](const clap_plugin *_plugin) {},
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

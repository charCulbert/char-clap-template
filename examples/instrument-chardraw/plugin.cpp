// Laid out as nakst's CLAP tutorial: https://nakst.gitlab.io/tutorial/clap-part-1.html
//
// Instrument chardraw: a polyphonic saw synth through a lowpass filter, with
// a peak meter. Its interface is drawn with chardraw (ui/PluginView.cpp) on
// WebGPU.
//
// This file is the whole CLAP plugin, in the same sections, in the same order,
// as every example's plugin.cpp, so comparing two shows only what differs:
//   Descriptor and features
//   Audio ports
//   Note ports
//   Parameters (the CLAP params extension)
//   Recalled settings (saved with the plugin, but not CLAP parameters)
//   Latency and tail
//   Process: events, then DSP
//   State: parameters and settings
//   UI connection (the only section the webview and chardraw examples differ in)
//   The plugin: CLAP's callbacks, and what entry.cpp needs
// Every example has every extension, so a plugin becomes another kind by
// filling in bodies; an unused one returns 0 or false and says what goes there.
//
// params.h numbers the parameters, for this file and the interface. entry.cpp
// is the entry point that makes the plugin. libs/ holds the reusable parts:
// the message codec and file loader (core/), and the presenter that shows a
// gpu::View in the host's window (gpu/).
#include "clap/clap.h"
#include "clap/events.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/latency.h"
#include "clap/ext/tail.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/ext/draft/webview.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include "core/messages.h"
#include "core/resources.h"
#include "gpu/gui.h"
#include "params.h" // the parameters' ids

static_assert (std::atomic<float>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);

// One playing note: a saw with a short attack and a release. Removing one
// copies the last over it (PluginEndFinishedVoices), which is fine for a few
// numbers; a voice with a buffer (a delay line, a sample) keeps a pointer to
// one allocated in init, so removing it moves the pointer, not the buffer.
struct Voice
{
    int32_t noteID;
    int16_t channel, key;
    float phase, increment; // cycles, and cycles per sample
    float envelope;         // 0..1
    bool released;
};
// The most notes at once: a note past them is ignored, so the audio thread
// never allocates.
#define MAX_VOICES (32)

struct MyPlugin
{
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_params_t* hostParams;
    const clap_host_state_t* hostState; // mark_dirty: a recalled setting changed
    float sampleRate;

    // Parameter values live twice: `parameters` on the audio thread, and
    // `mainParameters` on the main thread (for get_value, the state and the
    // UI), each with a flag saying the other thread has news for it. The
    // gesture flags carry the UI's drags to the host.
    std::atomic<float> parameters[P_COUNT], mainParameters[P_COUNT];
    std::atomic<bool> changed[P_COUNT], mainChanged[P_COUNT];
    std::atomic<bool> gestureBegin[P_COUNT], gestureEnd[P_COUNT];

    // Recalled settings (main thread; see Recalled settings below): the
    // window's size, in points.
    double guiWidth = 300, guiHeight = 150;

    // The DSP's state (audio thread): the voices playing, and the filter's
    // last output.
    Voice voices[MAX_VOICES];
    uint32_t voiceCount = 0;
    float filtered = 0;

    // The peak meter: the highest level per output channel since the main
    // thread last took it, and the audio counted since it was last asked to.
    std::atomic<float> peaks[2];
    uint32_t framesSinceMeter = 0; // audio thread
    std::atomic<bool> meterPending;

    // The UI connection.
    gpu::Gui gui;
    const clap_host_gui_t* hostGui;             // request_resize, when the content outgrows the window
    double contentWidth = 0, contentHeight = 0; // the view's "minimum size", in points
    std::atomic<bool> uiDirty;                  // the host changed a parameter: send the UI the values
};

// The extension tables refer to these before their definitions further down.
static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event);
static void PluginSyncMainToAudio (MyPlugin* plugin, const clap_output_events_t* out);
static bool PluginSyncAudioToMain (MyPlugin* plugin);
static double PluginGetValue (MyPlugin* plugin, uint32_t i);
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message);
static void PluginSendValues (MyPlugin* plugin);

// ---- Descriptor and features ----------------------------------------------

static const char* const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

static const clap_plugin_descriptor_t pluginDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.charlieculbert.instrumentchardraw",
    .name = "Instrument chardraw",
    .vendor = "Charlie Culbert",
    .url = "https://github.com/charCulbert",
    .manual_url = "https://github.com/charCulbert",
    .support_url = "https://github.com/charCulbert",
    .version = "1.0.0",
    .description = "A polyphonic saw synth with a lowpass filter.",
    .features = pluginFeatures,
};

// ---- Audio ports ----------------------------------------------------------

// No input: an instrument. One stereo output. (An effect returns 1 for both,
// for its stereo input.)
static const clap_plugin_audio_ports_t extensionAudioPorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput ? 0 : 1; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_audio_port_info_t* info) -> bool {
        if (index)
            return false;
        info->id = 0;
        info->channel_count = 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        snprintf (info->name, sizeof (info->name), "%s", isInput ? "Audio Input" : "Audio Output");
        return true;
    },
};

// ---- Note ports -----------------------------------------------------------

// One note input, taking CLAP note events or MIDI. (An effect returns 0.)
static const clap_plugin_note_ports_t extensionNotePorts = {
    .count = [] (const clap_plugin_t* plugin, bool isInput) -> uint32_t { return isInput ? 1 : 0; },

    .get = [] (const clap_plugin_t* plugin, uint32_t index, bool isInput, clap_note_port_info_t* info) -> bool {
        if (!isInput || index)
            return false;
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        snprintf (info->name, sizeof (info->name), "%s", "Note Input");
        return true;
    },
};

// ---- Parameters -----------------------------------------------------------

// How a parameter's value reads as text: value_to_text and text_to_value
// have a case for each.
enum ParamUnit
{
    UNIT_HERTZ,   // "440 Hz", "1.20 kHz"
    UNIT_SECONDS, // "300 ms", "1.20 s"
};

// Each parameter, in the order of its id (params.h): what get_info tells the
// host, and what the rest of this file reads. The key is what the value is
// saved under in the state: never change one once a version is released, or
// the values people saved are lost. The name is only what the host shows, and
// can change freely.
struct Param
{
    clap_id id;
    const char* key;
    const char* name;
    double min, max, defaultValue;
    clap_param_info_flags flags;
    ParamUnit unit;
};
static constexpr Param paramTable[] = {
    { P_CUTOFF, "cutoff", "Filter cutoff", 20.0, 20000.0, 2000.0, CLAP_PARAM_IS_AUTOMATABLE, UNIT_HERTZ },
    { P_RELEASE, "release", "Release", 0.01, 2.0, 0.3, CLAP_PARAM_IS_AUTOMATABLE, UNIT_SECONDS },
};
static_assert (std::size (paramTable) == P_COUNT, "a row for each id in params.h");
static_assert (
    [] {
        for (uint32_t i = 0; i < P_COUNT; i++)
            if (paramTable[i].id != i)
                return false;
        return true;
    }(),
    "the rows in the order of their ids");

static const clap_plugin_params_t extensionParams = {
    .count = [] (const clap_plugin_t* plugin) -> uint32_t { return P_COUNT; },

    .get_info = [] (const clap_plugin_t* _plugin, uint32_t index, clap_param_info_t* information) -> bool {
        if (index >= P_COUNT)
            return false;
        const Param& param = paramTable[index];
        memset (information, 0, sizeof (clap_param_info_t));
        information->id = param.id;
        information->flags = param.flags;
        information->min_value = param.min;
        information->max_value = param.max;
        information->default_value = param.defaultValue;
        snprintf (information->name, sizeof (information->name), "%s", param.name);
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
        if (id >= P_COUNT)
            return false;
        switch (paramTable[id].unit)
        {
            case UNIT_HERTZ:
                snprintf (display, size, value < 1000 ? "%.0f Hz" : "%.2f kHz", value < 1000 ? value : value / 1000);
                return true;
            case UNIT_SECONDS:
                snprintf (display, size, value < 1 ? "%.0f ms" : "%.2f s", value < 1 ? value * 1000 : value);
                return true;
        }
        return false;
    },

    .text_to_value = [] (const clap_plugin_t* _plugin, clap_id id, const char* display, double* value) -> bool {
        if (id >= P_COUNT)
            return false;
        char* end = nullptr;
        const double number = strtod (display, &end); // "440 Hz" reads as 440
        const bool read = end != display;
        while (*end == ' ')
            end++;
        switch (paramTable[id].unit)
        {
            case UNIT_HERTZ: // "1.2 kHz"
                *value = *end == 'k' || *end == 'K' ? number * 1000 : number;
                return read;
            case UNIT_SECONDS: // "300 ms", "1.2 s"
                *value = end[0] == 'm' && end[1] == 's' ? number / 1000 : number;
                return read;
        }
        return false;
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

// The current value on the main thread: the audio thread's value, unless the
// main thread has changed it since. get_value and the UI both use this.
static double PluginGetValue (MyPlugin* plugin, uint32_t i)
{
    return plugin->mainChanged[i] ? plugin->mainParameters[i].load() : plugin->parameters[i].load();
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

// The main thread's edits (the UI's) to the audio thread, and to the host as
// events: in process, or flush when the host isn't processing.
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

// The host's changes (on the audio thread) to the main thread.
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

// ---- Recalled settings ----------------------------------------------------

// Settings saved with the plugin's state, but not CLAP parameters, so hosts
// don't list or automate them. Here, the window's size (guiWidth and
// guiHeight, in points), so the window opens as it was left. A new setting
// takes the same four steps as the window's size; for a number, `tuning`:
//   1. MyPlugin        double tuning = 440;
//   2. State save      state["tuning"] = plugin->tuning;
//   3. State load      plugin->tuning = (*state)["tuning"].number (plugin->tuning);
//   4. Where it changes, on the main thread (a UI message, see
//      PluginReceiveMessage):
//                      plugin->tuning = value;
//                      PluginSettingChanged (plugin);

// Tells the host a setting changed, so it saves the project with it. (A
// parameter's change needs no call: the host sees its event.)
static void PluginSettingChanged (MyPlugin* plugin)
{
    if (plugin->hostState)
        plugin->hostState->mark_dirty (plugin->host);
}

// The window's limits, in points. The view's content can raise the minimum: see
// PluginClampSize.
#define GUI_MIN_WIDTH (240)
#define GUI_MIN_HEIGHT (130)
#define GUI_MAX_WIDTH (1600)
#define GUI_MAX_HEIGHT (1200)

// ---- Latency and tail -----------------------------------------------------

// 0: the output isn't delayed. (A lookahead returns its delay in samples.)
static const clap_plugin_latency_t extensionLatency = {
    .get = [] (const clap_plugin_t* plugin) -> uint32_t { return 0; },
};

// 0: silence in, silence out at once. (A reverb or delay returns how many
// samples it rings on for.)
static const clap_plugin_tail_t extensionTail = {
    .get = [] (const clap_plugin_t* plugin) -> uint32_t { return 0; },
};

// ---- Process: events, then DSP --------------------------------------------

static void PluginNoteOn (MyPlugin* plugin, int32_t noteID, int16_t channel, int16_t key)
{
    if (plugin->voiceCount == MAX_VOICES)
        return;
    plugin->voices[plugin->voiceCount++] = { noteID, channel, key, 0.0f,
        440.0f * powf (2.0f, (key - 69) / 12.0f) / plugin->sampleRate, 0.0f, false };
}

// Releases (or, for a choke, silences at once) the voices a note event names:
// -1 in a field matches any.
static void PluginNoteOff (MyPlugin* plugin, int32_t noteID, int16_t channel, int16_t key, bool choke)
{
    for (uint32_t i = 0; i < plugin->voiceCount; i++)
    {
        Voice& voice = plugin->voices[i];
        if ((key == -1 || voice.key == key) && (noteID == -1 || voice.noteID == noteID)
            && (channel == -1 || voice.channel == channel))
        {
            voice.released = true;
            if (choke)
                voice.envelope = 0;
        }
    }
}

static void PluginProcessEvent (MyPlugin* plugin, const clap_event_header_t* event)
{
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
    {
        if (event->type == CLAP_EVENT_NOTE_ON || event->type == CLAP_EVENT_NOTE_OFF
            || event->type == CLAP_EVENT_NOTE_CHOKE)
        {
            const clap_event_note_t* note = (const clap_event_note_t*) event;
            if (event->type == CLAP_EVENT_NOTE_ON)
                PluginNoteOn (plugin, note->note_id, note->channel, note->key);
            else
                PluginNoteOff (plugin, note->note_id, note->channel, note->key, event->type == CLAP_EVENT_NOTE_CHOKE);
        }
        else if (event->type == CLAP_EVENT_MIDI)
        {
            const uint8_t* data = ((const clap_event_midi_t*) event)->data;
            const uint8_t status = data[0] & 0xF0, channel = data[0] & 0x0F;
            if (status == 0x90 && data[2] > 0)
                PluginNoteOn (plugin, -1, channel, data[1]);
            else if (status == 0x80 || status == 0x90)
                PluginNoteOff (plugin, -1, channel, data[1], false);
        }
        else if (event->type == CLAP_EVENT_PARAM_VALUE)
        {
            const clap_event_param_value_t* valueEvent = (const clap_event_param_value_t*) event;
            if (valueEvent->param_id >= P_COUNT)
                return;
            uint32_t i = (uint32_t) valueEvent->param_id;
            // Kept in range, whatever the host sends.
            const Param& param = paramTable[i];
            const double value =
                valueEvent->value == valueEvent->value ? valueEvent->value : param.defaultValue; // NaN: the default
            plugin->parameters[i].store ((float) fmin (fmax (value, param.min), param.max), std::memory_order_relaxed);
            plugin->changed[i].store (true, std::memory_order_release);

            // Tell the UI on the main thread (see on_main_thread).
            if (!plugin->uiDirty.exchange (true, std::memory_order_acq_rel))
                plugin->host->request_callback (plugin->host);
        }
    }
}

// The DSP, from frame start up to end. input is nullptr in an instrument.
static void PluginRenderAudio (
    MyPlugin* plugin, uint32_t start, uint32_t end, float* const* input, float* const* output)
{
    // The simplest lowpass: one pole, each output moving a fraction of the
    // way to the input, more of the way the higher the cutoff.
    const float cutoff = plugin->parameters[P_CUTOFF].load (std::memory_order_relaxed);
    const float fraction = 1.0f - expf (-2.0f * 3.14159265f * cutoff / plugin->sampleRate);
    const float attackStep = 1.0f / (0.005f * plugin->sampleRate); // 5 ms
    const float releaseStep =
        1.0f / (plugin->parameters[P_RELEASE].load (std::memory_order_relaxed) * plugin->sampleRate);

    for (uint32_t index = start; index < end; index++)
    {
        float sum = 0.0f;
        for (uint32_t i = 0; i < plugin->voiceCount; i++)
        {
            Voice& voice = plugin->voices[i];
            voice.envelope =
                voice.released ? fmaxf (voice.envelope - releaseStep, 0.0f) : fminf (voice.envelope + attackStep, 1.0f);
            sum += (2.0f * voice.phase - 1.0f) * voice.envelope; // a naive saw: it aliases on high notes
            voice.phase += voice.increment;
            voice.phase -= floorf (voice.phase);
        }
        plugin->filtered += (sum * 0.2f - plugin->filtered) * fraction;
        // As it decays to silence it would pass through subnormal numbers,
        // which are slow on most CPUs: stop at zero first.
        if (fabsf (plugin->filtered) < 1e-20f)
            plugin->filtered = 0;
        output[0][index] = output[1][index] = plugin->filtered;
    }
}

// Tells the host each voice that has finished, so it can free the note, and
// removes it.
static void PluginEndFinishedVoices (MyPlugin* plugin, const clap_output_events_t* out, uint32_t time)
{
    for (uint32_t i = 0; i < plugin->voiceCount;)
    {
        const Voice& voice = plugin->voices[i];
        if (!voice.released || voice.envelope > 0)
        {
            i++;
            continue;
        }
        clap_event_note_t event = {};
        event.header.size = sizeof (event);
        event.header.time = time;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_NOTE_END;
        event.header.flags = 0;
        event.key = voice.key;
        event.note_id = voice.noteID;
        event.channel = voice.channel;
        event.port_index = 0;
        out->try_push (out, &event.header);
        plugin->voices[i] = plugin->voices[--plugin->voiceCount];
    }
}

// The block's peak per channel, for the meter; and about 30 times a second
// of audio, a request for the main thread to send it to the UI.
static void PluginMeasurePeaks (MyPlugin* plugin, uint32_t frameCount, float* const* output)
{
    for (uint32_t channel = 0; channel < 2; channel++)
    {
        float peak = 0;
        for (uint32_t index = 0; index < frameCount; index++)
            peak = fmaxf (peak, fabsf (output[channel][index]));
        // Only this thread raises it, and the main thread only resets it to 0,
        // so no compare-and-swap is needed: at worst a reset is overwritten
        // with this block's peak, which is newer anyway.
        if (peak > plugin->peaks[channel].load (std::memory_order_relaxed))
            plugin->peaks[channel].store (peak, std::memory_order_relaxed);
    }
    plugin->framesSinceMeter += frameCount;
    if (plugin->framesSinceMeter >= plugin->sampleRate / 30)
    {
        plugin->framesSinceMeter = 0;
        if (!plugin->meterPending.exchange (true, std::memory_order_acq_rel))
            plugin->host->request_callback (plugin->host);
    }
}

static clap_process_status PluginProcess (MyPlugin* plugin, const clap_process_t* process)
{
    PluginSyncMainToAudio (plugin, process->out_events);

    assert (process->audio_outputs_count == 1);
    float* const* input = process->audio_inputs_count ? process->audio_inputs[0].data32 : nullptr;
    float* const* output = process->audio_outputs[0].data32;

    // Events at their time in the block, the audio between them.
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

        PluginRenderAudio (plugin, i, nextEventFrame, input, output);
        i = nextEventFrame;
    }
    PluginEndFinishedVoices (plugin, process->out_events, frameCount ? frameCount - 1 : 0);
    PluginMeasurePeaks (plugin, frameCount, output);

    return CLAP_PROCESS_CONTINUE;
}

// ---- State: parameters and settings ---------------------------------------

// A CBOR map (core/messages.h) of each parameter's value by key, the
// settings, and a version, so later versions can add to it and still read
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
            const Param& param = paramTable[i];
            state[param.key] = (double) plugin->mainParameters[i].load (std::memory_order_relaxed);
        }
        state["window width"] = plugin->guiWidth;
        state["window height"] = plugin->guiHeight;

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

        // Anything missing from the state keeps its default.
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramTable[i];
            const double value = (*state)[param.key].number (param.defaultValue);
            plugin->mainParameters[i].store (
                (float) fmin (fmax (value, param.min), param.max), std::memory_order_relaxed);
            plugin->mainChanged[i].store (true, std::memory_order_release);
        }
        // The window takes its size when it next opens.
        plugin->guiWidth =
            fmin (fmax ((*state)["window width"].number (plugin->guiWidth), GUI_MIN_WIDTH), GUI_MAX_WIDTH);
        plugin->guiHeight =
            fmin (fmax ((*state)["window height"].number (plugin->guiHeight), GUI_MIN_HEIGHT), GUI_MAX_HEIGHT);

        if (plugin->hostParams)
            plugin->hostParams->rescan (plugin->host, CLAP_PARAM_RESCAN_VALUES);
        PluginSendValues (plugin);
        return true;
    },
};

// ---- UI connection --------------------------------------------------------

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

// Clamps a clap.gui size, which is in pixels outside macOS, to the limits:
// GUI_MIN_* and GUI_MAX_*, and the view's content.
static void PluginClampSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    const auto clamp = [scale] (uint32_t value, double low, double high) {
        return std::clamp (value, uint32_t (std::ceil (low * scale)), uint32_t (high * scale));
    };
    // The least is GUI_MIN_*, or the view's content ("minimum size") if more.
    const double leastWidth = std::min<double> (std::max<double> (GUI_MIN_WIDTH, plugin->contentWidth), GUI_MAX_WIDTH);
    const double leastHeight =
        std::min<double> (std::max<double> (GUI_MIN_HEIGHT, plugin->contentHeight), GUI_MAX_HEIGHT);
    width = clamp (width, leastWidth, GUI_MAX_WIDTH);
    height = clamp (height, leastHeight, GUI_MAX_HEIGHT);
}

// The size to report to clap.gui, clamped so get_size never reports a size set_size refuses.
static void PluginGuiSize (const MyPlugin* plugin, uint32_t& width, uint32_t& height)
{
    const float scale = plugin->gui.pixelsPerPoint();
    width = uint32_t (std::lround (plugin->guiWidth * scale));
    height = uint32_t (std::lround (plugin->guiHeight * scale));
    PluginClampSize (plugin, width, height);
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
        PluginGuiSize (plugin, width, height);
        plugin->gui.setSize (width, height);
        return true;
    },

    .destroy = [] (const clap_plugin_t* _plugin) { ((MyPlugin*) _plugin->plugin_data)->gui.destroy(); },

    .set_scale = [] (const clap_plugin_t* _plugin, double scale) -> bool {
        return ((MyPlugin*) _plugin->plugin_data)->gui.setScale (scale);
    },

    .get_size = [] (const clap_plugin_t* _plugin, uint32_t* width, uint32_t* height) -> bool {
        PluginGuiSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
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
        PluginClampSize ((MyPlugin*) _plugin->plugin_data, *width, *height);
        return true;
    },

    .set_size = [] (const clap_plugin_t* _plugin, uint32_t width, uint32_t height) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
        uint32_t allowedWidth = width, allowedHeight = height;
        PluginClampSize (plugin, allowedWidth, allowedHeight);
        if (allowedWidth != width || allowedHeight != height)
            return false;
        const double newWidth = width / plugin->gui.pixelsPerPoint(), newHeight = height / plugin->gui.pixelsPerPoint();
        if (newWidth != plugin->guiWidth || newHeight != plugin->guiHeight)
        {
            plugin->guiWidth = newWidth;
            plugin->guiHeight = newHeight;
            PluginSettingChanged (plugin);
        }
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
//                 {type: "minimum size", width, height}  what the content fits, in points
//   plugin -> UI  {type: "params", params: [{id, name, min, max, default, stepped}]}
//                 {type: "values", values: [value per parameter id]}
//                 {type: "meter", peaks: [left, right]}  the highest level since the last, 1 = 0 dBFS
static bool PluginReceiveMessage (MyPlugin* plugin, const core::Value& message)
{
    const auto type = message["type"].text();
    if (type == "ready")
    {
        core::Value::Array params;
        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramTable[i];
            params.push_back (core::Value::Map { { "id", (double) param.id }, { "name", param.name },
                { "min", param.min }, { "max", param.max }, { "default", param.defaultValue },
                { "stepped", (param.flags & CLAP_PARAM_IS_STEPPED) != 0 } });
        }
        plugin->gui.send (core::Value::Map { { "type", "params" }, { "params", params } });
        PluginSendValues (plugin);
        return true;
    }

    if (type == "minimum size")
    {
        // The window's minimum rises to the view's content; and if the window
        // is smaller now, it asks the host for the size that fits.
        plugin->contentWidth = message["width"].number (0);
        plugin->contentHeight = message["height"].number (0);
        const float scale = plugin->gui.pixelsPerPoint();
        const uint32_t oldWidth = uint32_t (std::lround (plugin->guiWidth * scale)),
                       oldHeight = uint32_t (std::lround (plugin->guiHeight * scale));
        uint32_t width = oldWidth, height = oldHeight;
        PluginClampSize (plugin, width, height);
        // A host that agrees needn't call set_size afterwards: take the size then.
        if (plugin->hostGui && (width != oldWidth || height != oldHeight)
            && plugin->hostGui->request_resize (plugin->host, width, height))
            extensionGui.set_size (&plugin->plugin, width, height);
        return true;
    }

    const double id = message["id"].number (-1);
    if (!(id >= 0 && id < double (P_COUNT)))
        return false;
    const uint32_t i = (uint32_t) id;

    if (type == "change")
    {
        const Param& param = paramTable[i];
        const double value = message["value"].number (param.defaultValue);
        plugin->mainParameters[i].store ((float) fmin (fmax (value, param.min), param.max), std::memory_order_relaxed);
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

// The peaks since the last call, and starts the next measurement.
static void PluginSendMeter (MyPlugin* plugin)
{
    const double left = plugin->peaks[0].exchange (0, std::memory_order_relaxed);
    const double right = plugin->peaks[1].exchange (0, std::memory_order_relaxed);
    plugin->gui.send (core::Value::Map { { "type", "meter" }, { "peaks", core::Value::Array { left, right } } });
}

// ---- The plugin -----------------------------------------------------------

static const clap_plugin_t pluginClass = {
    .desc = &pluginDescriptor,
    .plugin_data = nullptr,

    .init = [] (const clap_plugin* _plugin) -> bool {
        MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;

        plugin->hostParams = (const clap_host_params_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_PARAMS);
        plugin->hostState = (const clap_host_state_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_STATE);
        plugin->hostGui = (const clap_host_gui_t*) plugin->host->get_extension (plugin->host, CLAP_EXT_GUI);
        plugin->gui.init (_plugin, plugin->host);

        for (uint32_t i = 0; i < P_COUNT; i++)
        {
            const Param& param = paramTable[i];
            float defaultValue = (float) param.defaultValue;
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
        plugin->voiceCount = 0;
        plugin->filtered = 0;
        return true;
    },

    .deactivate = [] (const clap_plugin* _plugin) {},

    .start_processing = [] (const clap_plugin* _plugin) -> bool { return true; },

    .stop_processing = [] (const clap_plugin* _plugin) {},

    .reset =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            plugin->voiceCount = 0;
            plugin->filtered = 0;
        },

    .process = [] (const clap_plugin* _plugin, const clap_process_t* process) -> clap_process_status {
        return PluginProcess ((MyPlugin*) _plugin->plugin_data, process);
    },

    .get_extension = [] (const clap_plugin* plugin, const char* id) -> const void* {
        if (0 == strcmp (id, CLAP_EXT_AUDIO_PORTS))
            return &extensionAudioPorts;
        if (0 == strcmp (id, CLAP_EXT_NOTE_PORTS))
            return &extensionNotePorts;
        if (0 == strcmp (id, CLAP_EXT_PARAMS))
            return &extensionParams;
        if (0 == strcmp (id, CLAP_EXT_LATENCY))
            return &extensionLatency;
        if (0 == strcmp (id, CLAP_EXT_TAIL))
            return &extensionTail;
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

    // Asked for with request_callback: the host changed parameters, or it's
    // time for the meter.
    .on_main_thread =
        [] (const clap_plugin* _plugin) {
            MyPlugin* plugin = (MyPlugin*) _plugin->plugin_data;
            if (plugin->uiDirty.exchange (false, std::memory_order_acq_rel))
                PluginSendValues (plugin);
            if (plugin->meterPending.exchange (false, std::memory_order_acq_rel))
                PluginSendMeter (plugin);
        },
};

// What entry.cpp needs.
const clap_plugin_descriptor_t* getPluginDescriptor()
{
    return &pluginDescriptor;
}

const clap_plugin_t* createPlugin (const clap_host_t* host)
{
    MyPlugin* plugin = new MyPlugin();
    plugin->host = host;
    plugin->plugin = pluginClass;
    plugin->plugin.plugin_data = plugin;
    return &plugin->plugin;
}

// Instrument chardraw's interface: knobs for Filter cutoff and Release, and
// a peak meter per channel, laid out with Clay in chardraw's palette (dark or light,
// as the system is). Parameters arrive and go back as the messages plugin.cpp
// lists; the meter's levels arrive as "meter" messages.
#include "chardraw/ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{

constexpr uint32_t cutoffId = 0, releaseId = 1; // as P_CUTOFF and P_RELEASE in plugin.cpp

std::string hertz (double value) // "440 Hz", "1.20 kHz"
{
    char text[16];
    std::snprintf (text, sizeof (text), value < 1000 ? "%.0f Hz" : "%.2f kHz", value < 1000 ? value : value / 1000);
    return text;
}

std::string seconds (double value) // "300 ms", "1.20 s"
{
    char text[16];
    std::snprintf (text, sizeof (text), value < 1 ? "%.0f ms" : "%.2f s", value < 1 ? value * 1000 : value);
    return text;
}

class PluginView : public gpu::View
{
public:
    void prepare (const wgpu::Device& device, wgpu::TextureFormat format) override
    {
        ui.draw.prepare (device, format, gpu::loadAsset ("/fonts/Inter.ttf"));
        params.send = send;
        ui.lockPointer = lockPointer;
        ui.setCursor = setCursor;
        ui.copyText = copyText;
        ui.resizeTo = resizeTo;
        send (core::Value::Map { { "type", "ready" } });
    }

    void receive (const core::Value& message) override
    {
        if (params.receive (message) || message["type"].text() != "meter")
            return;
        // 0 to 1 on the meter is -60 to 0 dBFS. The bar falls back over about
        // half a second, and the peak line over two.
        const auto* peaks = std::get_if<core::Value::Array> (&message["peaks"].data);
        for (size_t c = 0; peaks && c < 2 && c < peaks->size(); ++c)
        {
            const float peak = float ((*peaks)[c].number (0));
            const float level = std::clamp ((20 * std::log10 (peak + 1e-9f) + 60) / 60, 0.0f, 1.0f);
            shown[c] = std::max (level, shown[c] - 0.06f);
            held[c] = std::max (level, held[c] - 0.015f);
        }
    }

    void pointer (const PointerEvent& event) override
    {
        ui.pointer (event);
    }
    bool key (Key key, unsigned modifiers) override
    {
        return ui.key (key, modifiers);
    }
    bool text (const char* utf8) override
    {
        return ui.text (utf8);
    }
    bool editingText() const override
    {
        return ui.editingText();
    }
    Caret textCaret() const override
    {
        return ui.textCaret();
    }
    const gpu::Accessibility* accessibility() const override
    {
        return ui.accessibility();
    }
    void accessAction (uint64_t id, gpu::Accessible::Action action, double value) override
    {
        ui.accessAction (id, action, value);
    }

    // The frame is built in record, so the presenter can skip one that looks
    // like the last; draw only flushes it.
    void draw (const wgpu::RenderPassEncoder& pass, float, float, float) override
    {
        ui.flush (pass);
    }
    bool record (float width, float height, float scale) override
    {
        gpu::Param* cutoff = params.find (cutoffId);
        gpu::Param* release = params.find (releaseId);
        ui.theme = ui.theme.withColors (darkMode);
        ui.begin (width, height, scale);
        CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0), CLAY_SIZING_GROW (0) },
                    .padding = CLAY_PADDING_ALL (20),
                    .childGap = 24,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
            .backgroundColor = gpu::clayColor (ui.theme.background) })
        {
            if (cutoff && release) // once the plugin has sent its parameters
            {
                ui.knob (params, cutoff, { hertz, "Cutoff" }, true, 52); // log: even steps per octave
                ui.knob (params, release, seconds, true, 52);
            }
            CLAY ({ .layout = { .sizing = { CLAY_SIZING_GROW (0) } } }) {} // the meter to the right
            CLAY ({ .layout = { .sizing = { CLAY_SIZING_FIT (0), CLAY_SIZING_GROW (0) }, .childGap = 3 } })
            {
                ui.meter (shown[0], held[0] > 0 ? held[0] : -1, 8);
                ui.meter (shown[1], held[1] > 0 ? held[1] : -1, 8);
            }
        }
        ui.resizeGrip (width, height);
        return ui.record();
    }

private:
    float shown[2] = {}, held[2] = {}; // the meter, 0..1
    gpu::Params params;
    gpu::Ui ui;
};

} // namespace

std::unique_ptr<gpu::View> gpu::createView()
{
    return std::make_unique<PluginView>();
}

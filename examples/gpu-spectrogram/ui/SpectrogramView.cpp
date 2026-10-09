// GPU Spectrogram's interface, made only of libs/draw2d's parts: a gpu::Spectrogram
// (draw2d/views.h) filling the window, and Ui controls for the plugin's three
// settings (Palette, Scale, Floor) above it, laid out with Clay. The columns
// arrive from the plugin as "column" messages (see plugin.cpp).
#include "draw2d/ui.h"
#include "draw2d/views.h"
#include <vector>

namespace {

class SpectrogramView : public gpu::View
{
public:
    void prepare(const wgpu::Device &device, wgpu::TextureFormat format) override
    {
        ui.draw.prepare(device, format, gpu::loadAsset("/fonts/Inter.ttf"));
        params.send = send;
        ui.lockPointer = lockPointer;
        ui.copyText = copyText;
        ui.resizeTo = resizeTo;
        ui.setCursor = setCursor;
        send(core::Value::Map{{"type", "ready"}});
    }

    void receive(const core::Value &message) override
    {
        if (params.receive(message) || message["type"].text() != "column") return;
        auto *db = std::get_if<core::Value::Array>(&message["db"].data);
        const float rate = float(message["rate"].number(48000));
        if (!db || db->empty()) return;
        if (hz.size() != db->size() || rate != binRate) // bin k is at k * rate / FFT size; the FFT size is twice the bins
        {
            hz.resize(db->size());
            for (size_t k = 0; k < hz.size(); ++k) hz[k] = float(k) * rate / float(2 * hz.size());
            binRate = rate;
        }
        column.resize(db->size());
        for (size_t k = 0; k < column.size(); ++k) column[k] = float((*db)[k].number(-200));
        spectrogram.add(column.data(), hz.data(), column.size());
    }

    void pointer(const PointerEvent &event) override { ui.pointer(event); }
    bool key(Key key, unsigned modifiers) override { return ui.key(key, modifiers); }
    bool text(const char *utf8) override { return ui.text(utf8); }
    bool animating() const override { return ui.animating(); }
    const gpu::Accessibility *accessibility() const override { return ui.accessibility(); }
    void accessAction(uint64_t id, gpu::Accessible::Action action, double value) override { ui.accessAction(id, action, value); }

    // The frame is built in record, so the presenter can skip one that looks
    // like the last; draw only flushes it.
    void draw(const wgpu::RenderPassEncoder &pass, float, float, float) override { ui.flush(pass); }
    bool record(float width, float height, float scale) override
    {
        gpu::Param *palette = params.find("Palette"), *scaleSetting = params.find("Scale"), *floor = params.find("Floor");
        if (!palette || !scaleSetting || !floor) return false; // until the plugin has sent its settings
        gpu::Spectrogram::Options o = spectrogram.settings();
        o.palette = gpu::Spectrogram::Palette(int(palette->value));
        o.scale = gpu::Spectrogram::Scale(int(scaleSetting->value));
        o.minDecibels = float(floor->value);
        o.maxFrequency = binRate / 2;
        spectrogram.configure(o); // cheap when nothing changed: it only redraws when its picture would

        ui.begin(width, height, scale);
        // The spectrogram fills the window, under a slim bar of controls.
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = gpu::clayColor(ui.theme.background)})
        {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {10, 10, 6, 6}, .childGap = 12,
                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = gpu::clayColor(ui.theme.panel)})
            {
                static const char *const palettes[] = {"System", "Bone", "Inferno", "Viridis", "Magma"};
                static const char *const scales[] = {"Linear", "Log", "Mel"};
                ui.segmented(params, palette, palettes, 5);
                ui.segmented(params, scaleSetting, scales, 3);
                ui.numberBox(params, floor, "%.0f dB");
            }
            // A box that draws itself (Ui::paint), edge to edge.
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                  .custom = {.customData = ui.paint([this](gpu::Rect r) { spectrogram.draw(ui, "Spectrogram", r); })}}) {}
        }
        ui.resizeGrip(width, height);
        return ui.record();
    }

private:
    gpu::Spectrogram spectrogram{{.columns = 600}}; // about 6 s at 48 kHz: one column per 512 samples
    std::vector<float> hz, column; // the bins' frequencies, and the column being added
    float binRate = 48000;
    gpu::Params params; // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Ui ui;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<SpectrogramView>(); }

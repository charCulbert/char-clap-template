// GPU Spectrogram's interface, made only of libs/draw2d's parts, laid out as
// char-spectrum's: a gpu::Spectrogram (draw2d/views.h) fills the window, its
// scales drawn over its edges, and the plugin's settings sit in the top right
// corner as readouts (Ui::readout) to drag, scroll or click. Hovering shows
// the frequency, nearest pitch and level under the pointer. The columns
// arrive from the plugin as "column" messages (see plugin.cpp).
#include "draw2d/ui.h"
#include "draw2d/views.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

// The plugin's settings, in its order (plugin.cpp's SettingId).
enum Setting { fftSize, hopSize, window, channel, smoothing, low, high, floorLevel, ceiling, frequencyScale, palette, scrollRate, piano,
               peakSnap, cursor, settingCount };
const char *const settingNames[settingCount] = {"FFT size", "Hop size", "Window", "Channel", "Smoothing", "Low frequency",
                                                "High frequency", "Floor", "Ceiling", "Scale", "Palette", "Scroll rate",
                                                "Piano", "Peak snap", "Cursor"};

// A note's name and octave, and how far off it the frequency is: "A4 +0¢".
void pitchName(float hz, char *out, size_t size)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const double pitch = 69 + 12 * std::log2(hz / 440.0);
    const int note = int(std::lround(pitch)), cents = int(std::lround((pitch - note) * 100));
    std::snprintf(out, size, "%s%d %s%d\xc2\xa2", names[(note % 12 + 12) % 12], int(std::floor(note / 12.0)) - 1,
                  cents < 0 ? "\xe2\x88\x92" : "+", std::abs(cents)); // U+2212 minus, U+00A2 cent sign
}

class SpectrogramView : public gpu::View
{
public:
    void prepare(const wgpu::Device &device, wgpu::TextureFormat format) override
    {
        ui.draw.prepare(device, format, gpu::loadAsset("/fonts/IBMPlexMono-Medium.ttf"));
        ui.theme.textSize = 12;
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
        for (size_t k = 0; k < column.size(); ++k) column[k] = float((*db)[k].number(-160));
        spectrogram.add(column.data(), hz.data(), column.size(), float(message["seconds"].number(0)));
    }

    void pointer(const PointerEvent &event) override
    {
        if (event.type == Pointer::leave) hovering = false;
        else if (event.type != Pointer::wheel) hovering = true, pointerX = event.x, pointerY = event.y;
        modifiers = event.modifiers;
        ui.pointer(event);
    }
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
        for (int i = 0; i < settingCount; ++i)
            if (!(p[i] = params.find(settingNames[i]))) return false; // until the plugin has sent its settings
        // The plugin keeps what was chosen; shown here, the hop stops at the
        // FFT size and the range at Nyquist, as the analysis has them.
        p[hopSize]->max = double(1 << int(std::lround(p[fftSize]->value)));
        p[high]->max = std::max(p[high]->min, double(binRate) / 2);
        for (Setting s : {hopSize, high}) p[s]->value = std::min(p[s]->value, p[s]->max);

        gpu::Spectrogram::Options o = spectrogram.settings();
        o.minFrequency = float(p[low]->value);
        o.maxFrequency = float(p[high]->value);
        o.minDecibels = float(p[floorLevel]->value);
        o.maxDecibels = float(p[ceiling]->value);
        o.scale = gpu::Spectrogram::Scale(std::lround(p[frequencyScale]->value));
        o.palette = gpu::Spectrogram::Palette(std::lround(p[palette]->value));
        o.pixelsPerSecond = float(p[scrollRate]->value);
        // A column a point of time: as many as the window is wide, and some.
        o.columns = int(std::ceil((width / scale + 64) / 128) * 128);
        o.rows = std::clamp(int(std::lround(height / 64)) * 64, 128, 1024);
        spectrogram.configure(o); // cheap when nothing changed: it only redraws when its picture would

        ui.begin(width, height, scale);
        const Clay_ElementId plot = ui.id("Plot"), panel = ui.id("Settings");
        // The spectrogram fills the window, edge to edge, the settings over its top right corner.
        CLAY({.id = plot, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .padding = {8, 8, 8, 8},
                                     .childAlignment = {CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_TOP}},
              .custom = {.customData = ui.paint([this, plot, panel](gpu::Rect r) {
                  spectrogram.draw(ui, "Spectrogram", r);
                  if (p[piano]->value >= 0.5f) drawPiano(r);
                  drawScales(r);
                  if (hovering && p[cursor]->value >= 0.5f && ui.pointerOver(plot) && !ui.pointerOver(panel)) drawCursor(r);
              })}})
        {
            readouts(panel, width / scale);
        }
        ui.resizeGrip(width, height);
        return ui.record();
    }

private:
    // The settings in a block, three to a row (two in a narrow window), on a
    // backing that reads over any palette. Each has a context menu to reset it.
    void readouts(Clay_ElementId panel, float widthPoints)
    {
        static const char *const fftSizes[] = {"256", "512", "1024", "2048", "4096", "8192", "16384"};
        static const char *const windows[] = {"Rect", "Hann", "Hamming", "Blackman"};
        static const char *const channels[] = {"Mix", "Left", "Right"};
        static const char *const scales[] = {"Linear", "Log", "Mel"};
        static const char *const palettes[] = {"System", "Bone", "Inferno", "Viridis", "Magma"};
        static const char *const onOff[] = {"Off", "On"};
        static const char *const menu[] = {"Reset to Default"};
        // Each: its setting, label, and a format (with log dragging) or items.
        struct Readout { Setting setting; const char *label, *format; bool log; const char *const *items; int count; };
        static const Readout all[] = {
            {fftSize, "FFT", nullptr, false, fftSizes, 7}, {hopSize, "Hop", "%.0f", true}, {window, "Window", nullptr, false, windows, 4},
            {channel, "Channel", nullptr, false, channels, 3}, {smoothing, "Smooth", "%.2f"}, {scrollRate, "Scroll", "%.0f px/s"},
            {low, "Low", "%.0f Hz"}, {high, "High", "%.0f Hz", true}, {frequencyScale, "Scale", nullptr, false, scales, 3},
            {floorLevel, "Floor", "%.0f dB"}, {ceiling, "Ceiling", "%.0f dB"}, {palette, "Palette", nullptr, false, palettes, 5},
            {piano, "Piano", nullptr, false, onOff, 2}, {peakSnap, "Snap", nullptr, false, onOff, 2}, {cursor, "Cursor", nullptr, false, onOff, 2},
        };
        const float cell = 126; // points: "Window Blackman" and its padding
        const int perRow = widthPoints >= 3 * cell + 120 ? 3 : 2; // clear of the frequencies on the left
        const gpu::Color &b = ui.theme.background;
        CLAY({.id = panel, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {4, 4, 4, 4},
                                      .layoutDirection = CLAY_TOP_TO_BOTTOM},
              .backgroundColor = gpu::clayColor({b.r, b.g, b.b, 0.78f}), .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            for (int first = 0; first < settingCount; first += perRow)
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}})
                {
                    for (int i = first; i < std::min(first + perRow, int(settingCount)); ++i)
                    {
                        const Readout &r = all[i];
                        gpu::Param *q = p[r.setting];
                        if (r.items) ui.readout(params, q, r.label, r.items, r.count, cell);
                        else ui.readout(params, q, r.label, r.format, r.log, cell);
                        if (ui.contextMenu(menu, 1) == 0) params.begin(*q), params.change(*q, q->defaultValue), params.end(*q);
                    }
                }
        }
    }

    // Text on whole pixels, which keeps it sharp; y is the baseline, size in points.
    void label(float x, float y, std::string_view text, gpu::Color color, gpu::Draw2D::Align align = gpu::Draw2D::Align::left)
    {
        ui.draw.text(std::round(x), std::round(y), 11 * ui.s, text, color, align);
    }
    // A label on a backing, so it reads over any palette.
    void tag(float x, float y, std::string_view text, gpu::Draw2D::Align align)
    {
        const float s = ui.s, w = ui.draw.measure(11 * s, text);
        const float left = align == gpu::Draw2D::Align::left ? x : align == gpu::Draw2D::Align::right ? x - w : x - w / 2;
        const gpu::Color &b = ui.theme.background;
        ui.draw.rect(left - 3 * s, y - 10 * s, w + 6 * s, 14 * s, 2 * s, {b.r, b.g, b.b, 0.7f});
        label(x, y, text, ui.theme.text, align);
    }
    float stripWidth() const { return p[piano]->value >= 0.5f ? 12 * ui.s : 0; }

    // A piano's keys up the left edge, each at its pitch on the frequency
    // scale (as Listener's piano strip). To screen readers a picture.
    void drawPiano(gpu::Rect r)
    {
        const float s = ui.s, w = stripWidth();
        const float top = spectrogram.settings().maxFrequency, bottom = std::max(spectrogram.settings().minFrequency, 8.0f);
        auto black = [](int note) { const int n = (note % 12 + 12) % 12; return n == 1 || n == 3 || n == 6 || n == 8 || n == 10; };
        auto y = [&](double note) { return spectrogram.yAt(r, float(440 * std::pow(2.0, (note - 69) / 12))); };
        const int first = int(std::floor(69 + 12 * std::log2(bottom / 440))) - 1, last = int(std::ceil(69 + 12 * std::log2(top / 440))) + 1;
        ui.draw.pushClip(r.x, r.y, w, r.h);
        ui.draw.rect(r.x, r.y, w, r.h, 0, ui.theme.whiteKey);
        const gpu::Color line{ui.theme.blackKey.r, ui.theme.blackKey.g, ui.theme.blackKey.b, 0.35f};
        for (int note = first; note <= last; ++note)
        {
            if (black(note))
            {
                const float y0 = y(note + 0.5), y1 = y(note - 0.5);
                ui.draw.rect(r.x, y0, w * 0.6f, std::max(1.0f, y1 - y0), 0, ui.theme.blackKey);
            }
            else if (!black(note + 1) && y(note) - y(note + 1) > 3 * s) // where two white keys meet (E to F, B to C)
                ui.draw.line(r.x, y(note + 0.5), r.x + w, y(note + 0.5), 1 * s, line);
        }
        ui.draw.popClip();
        ui.draw.line(r.x + w, r.y, r.x + w, r.y + r.h, 1 * s, line);
        ui.accessible(gpu::Accessible::Role::image, "Piano strip", nullptr, {r.x, r.y, w, r.h},
                      "Piano keys up the side, each at its pitch on the frequency scale");
    }

    // Frequencies up the left edge and seconds along the bottom, over the plot.
    void drawScales(gpu::Rect r)
    {
        const float s = ui.s, x0 = r.x + stripWidth();
        const gpu::Color tick{ui.theme.text.r, ui.theme.text.g, ui.theme.text.b, 0.7f};
        const float lowest = spectrogram.settings().minFrequency, highest = spectrogram.settings().maxFrequency;
        std::vector<float> marks;
        if (spectrogram.settings().scale == gpu::Spectrogram::Scale::linear)
        {
            // A round step giving eight or so.
            const float rough = (highest - lowest) / 8, power = std::pow(10.0f, std::floor(std::log10(rough)));
            const float step = power * (rough / power < 2 ? 2 : rough / power < 5 ? 5 : 10);
            for (float f = std::ceil(lowest / step) * step; f <= highest; f += step) marks.push_back(f);
        }
        else
            for (float decade = 10; decade <= highest; decade *= 10)
                for (float k : {1.0f, 2.0f, 5.0f})
                    if (decade * k >= lowest && decade * k <= highest) marks.push_back(decade * k);
        float lastY = 1e9f;
        for (float f : marks)
        {
            const float y = spectrogram.yAt(r, f);
            if (y > r.y + r.h - 22 * s || y < r.y + 8 * s || lastY - y < 18 * s) continue; // clear of the edges and each other
            char text[16];
            if (f >= 1000) std::snprintf(text, sizeof text, "%gk", f / 1000);
            else std::snprintf(text, sizeof text, "%g", f);
            ui.draw.line(x0, y, x0 + 5 * s, y, 1 * s, tick);
            tag(x0 + 9 * s, y + 4 * s, text, gpu::Draw2D::Align::left);
            lastY = y;
        }
        // Seconds back from the newest, at a round step at least 60 points apart.
        const float rate = spectrogram.settings().pixelsPerSecond;
        if (rate <= 0) return;
        float step = 0.25f;
        for (float candidate : {0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f})
            if ((step = candidate) * rate >= 60) break;
        const float right = r.x + r.w, bottom = r.y + r.h;
        for (int i = 1;; ++i)
        {
            const float x = right - i * step * rate * s;
            if (x < x0 + 48 * s) break;
            char text[16];
            std::snprintf(text, sizeof text, "\xe2\x88\x92%g s", i * step);
            ui.draw.line(x, bottom, x, bottom - 5 * s, 1 * s, tick);
            tag(x, bottom - 9 * s, text, gpu::Draw2D::Align::center);
        }
    }

    // What's under the pointer: a line at its frequency, a dot on a peak it
    // snapped to, and a label: "440.0 Hz  A4 +0¢  -12.3 dB".
    void drawCursor(gpu::Rect r)
    {
        gpu::Spectrogram::Probe at;
        const bool snap = p[peakSnap]->value >= 0.5f && !(modifiers & gpu::View::command); // Command bypasses it, as Listener's
        if (pointerX < r.x + stripWidth() || !spectrogram.probe(pointerX, pointerY, snap, at)) return;
        const float s = ui.s;
        const gpu::Color guide{ui.theme.text.r, ui.theme.text.g, ui.theme.text.b, 0.45f};
        ui.draw.line(r.x + stripWidth(), at.y, r.x + r.w, at.y, 1 * s, guide);
        if (at.peak) ui.draw.rect(pointerX - 3 * s, at.y - 3 * s, 6 * s, 6 * s, 3 * s, ui.theme.accent);
        char pitch[24], text[80];
        pitchName(at.frequency, pitch, sizeof pitch);
        std::snprintf(text, sizeof text, at.frequency < 10000 ? "%.1f Hz  %s  %.1f dB" : "%.0f Hz  %s  %.1f dB", at.frequency, pitch,
                      at.decibels);
        std::string shown = text; // a minus sign, not a hyphen, before the level
        if (const size_t minus = shown.rfind("  -"); minus != std::string::npos) shown.replace(minus + 2, 1, "\xe2\x88\x92");
        const float w = ui.draw.measure(11 * s, shown);
        float x = pointerX + 14 * s, y = at.y - 8 * s;
        if (x + w + 4 * s > r.x + r.w) x = pointerX - 14 * s - w; // flipped to stay inside
        if (y - 12 * s < r.y) y = at.y + 18 * s;
        tag(x, y, shown, gpu::Draw2D::Align::left);
    }

    gpu::Spectrogram spectrogram;
    std::vector<float> hz, column; // the bins' frequencies, and the column being added
    float binRate = 48000;
    gpu::Params params; // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Param *p[settingCount] = {};
    gpu::Ui ui;
    bool hovering = false;
    float pointerX = 0, pointerY = 0; // pixels
    unsigned modifiers = 0;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<SpectrogramView>(); }

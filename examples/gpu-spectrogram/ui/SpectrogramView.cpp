// GPU Spectrogram's interface, made only of libs/draw2d's parts: a
// gpu::Spectrogram (draw2d/views.h) fills the window, its scales drawn over
// its edges, with a Pause toggle in its top right corner and the settings
// drawer's tab (Ui::drawer, an overlay) on its right edge. Open, the drawer's
// panel covers the plot's right side, which stays as it is, with the
// plugin's settings as readouts (Ui::readout) to drag, scroll or click; its
// edge resizes it. Hovering shows the frequency, nearest
// pitch and level under the pointer; holding a key of the piano strip plays
// its pitch (through the plugin) and marks it across the plot. Space pauses
// the display. The columns arrive from the plugin as "column" messages (see
// plugin.cpp).
#include "draw2d/ui.h"
#include "draw2d/views.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>

namespace {

// The plugin's settings, in its order (plugin.cpp's SettingId).
enum Setting { fftSize, hopSize, window, channel, smoothing, low, high, floorLevel, ceiling, frequencyScale, palette, scrollRate, piano,
               peakSnap, cursor, transform, cqtBands, tuning, cqtLow, cqtHigh, cqtRate, keyTone, drawer, drawerWidth, settingCount };
const char *const settingNames[settingCount] = {"FFT size", "Hop size", "Window", "Channel", "Smoothing", "Low frequency",
                                                "High frequency", "Floor", "Ceiling", "Scale", "Palette", "Scroll rate",
                                                "Piano", "Peak snap", "Cursor", "Transform", "CQT bands per octave", "A4 tuning",
                                                "CQT low frequency", "CQT high frequency", "CQT update rate", "Key tone",
                                                "Settings drawer", "Settings drawer width"};

bool blackKey(int note)
{
    const int n = (note % 12 + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// A note's name and octave, and how far off it the frequency is, with A4 at
// a4 Hz: "A4 +0¢".
void pitchName(float hz, double a4, char *out, size_t size)
{
    static const char *const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const double pitch = 69 + 12 * std::log2(hz / a4);
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
        binRate = rate;
        if (paused) return; // the analysis runs on; the picture holds still
        if (hz.size() != db->size() || rate != hzRate) // bin k is at k * rate / FFT size; the FFT size is twice the bins
        {
            hz.resize(db->size());
            for (size_t k = 0; k < hz.size(); ++k) hz[k] = float(k) * rate / float(2 * hz.size());
            hzRate = rate;
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
    // Space pauses and resumes the display, as Listener's, unless the focused
    // control takes it (a button, toggle, drawer header or list presses or
    // opens with it): Ui::text has it first. It arrives as typed text.
    bool text(const char *utf8) override
    {
        if (ui.text(utf8)) return true;
        if (std::strcmp(utf8, " ") != 0 || ui.editingText()) return false;
        setPaused(!paused);
        return true;
    }
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
        // FFT size and the ranges at Nyquist (the CQT's at 0.4 x the sample
        // rate, as Listener's), as the analysis has them.
        p[hopSize]->max = double(1 << int(std::lround(p[fftSize]->value)));
        p[high]->max = std::max(p[high]->min, double(binRate) / 2);
        p[cqtHigh]->max = std::max(p[cqtHigh]->min, double(binRate) * 0.4);
        for (Setting s : {hopSize, high, cqtHigh}) p[s]->value = std::min(p[s]->value, p[s]->max);

        gpu::Spectrogram::Options o = spectrogram.settings();
        o.minFrequency = float(p[low]->value);
        o.maxFrequency = float(p[high]->value);
        o.minDecibels = float(p[floorLevel]->value);
        o.maxDecibels = float(p[ceiling]->value);
        o.scale = gpu::Spectrogram::Scale(std::lround(p[frequencyScale]->value));
        o.palette = gpu::Spectrogram::Palette(std::lround(p[palette]->value));
        // A new scroll rate starts the history again, so paused it waits for
        // the resume, which starts it again anyway.
        if (!paused) o.pixelsPerSecond = float(p[scrollRate]->value);
        // A column a point of time: as many as the window is wide, and some.
        o.columns = int(std::ceil((width / scale + 64) / 128) * 128);
        o.rows = std::clamp(int(std::lround(height / 64)) * 64, 128, 1024);
        spectrogram.configure(o); // cheap when nothing changed: it only redraws when its picture would

        ui.begin(width, height, scale);
        const Clay_ElementId plot = ui.id("Plot"), corner = ui.id("Corner"), panel = ui.id("Settings");
        const Clay_ElementId tab = ui.id("DrawerHeader", panel.id); // the drawer's tab, as Ui names it
        // The spectrogram fills the window, edge to edge, and stays so: the
        // settings drawer opens over it from the right edge. Pause sits in
        // its top right corner, and left of the drawer's panel while it's open.
        const float panelWidth = p[drawer]->value >= 0.5 ? ui.bounds(panel).w : 0;
        CLAY({.id = plot, .layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
              .custom = {.customData = ui.paint([this, plot, corner, panel, tab](gpu::Rect r) {
                  spectrogram.draw(ui, "Spectrogram", r);
                  if (p[piano]->value >= 0.5f) drawPiano(r);
                  else if (heldNote >= 0) playNote(-1); // the strip went away with a key held
                  drawScales(r);
                  if (heldNote >= 0) drawGuide(r);
                  if (hovering && heldNote < 0 && p[cursor]->value >= 0.5f && ui.pointerOver(plot) && !ui.pointerOver(corner)
                      && !ui.pointerOver(panel) && !ui.pointerOver(tab))
                      drawCursor(r);
              })}})
        {
            const gpu::Color &b = ui.theme.background;
            CLAY({.id = corner, .layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {4, 4, 4, 4}},
                  .backgroundColor = gpu::clayColor({b.r, b.g, b.b, 0.78f}), .cornerRadius = CLAY_CORNER_RADIUS(4),
                  .floating = {.offset = {-8 - panelWidth, 8}, .attachPoints = {CLAY_ATTACH_POINT_RIGHT_TOP, CLAY_ATTACH_POINT_RIGHT_TOP},
                               .attachTo = CLAY_ATTACH_TO_PARENT}})
            {
                bool pause = paused;
                if (ui.toggle("Pause", pause)) setPaused(pause);
                ui.tooltip("Freeze the display (Space); the analysis keeps running");
            }
            settingsDrawer(width / scale);
        }
        ui.resizeGrip(width, height);
        return ui.record();
    }

private:
    // Paused, the picture holds still while the analysis runs on (columns
    // that arrive are dropped), and the settings that only change how it
    // looks still apply to it; resuming starts a new history, as Listener's.
    void setPaused(bool on)
    {
        if (on == paused) return;
        paused = on;
        if (!paused) spectrogram.clear();
        ui.announce(paused ? "Display paused" : "Display resumed");
    }

    // The settings drawer, over the plot's right edge (see record), which keeps
    // whether it's open and its width in the plugin's state. In it the
    // settings as readouts, as many to a row as its width has room for; the
    // CQT's only while it's the transform. Each has a context menu to reset it.
    void settingsDrawer(float widthPoints)
    {
        bool open = p[drawer]->value >= 0.5;
        float size = float(p[drawerWidth]->value);
        const float least = float(p[drawerWidth]->min), most = std::max(least, std::min(float(p[drawerWidth]->max), widthPoints - 200));
        ui.drawer("Settings", open, size, [&] { readouts(size); },
                  {.edge = gpu::DrawerOptions::Edge::right, .min = least, .max = most, .overlay = true});
        auto keep = [this](gpu::Param &q, double value) {
            if (value == q.value) return;
            params.begin(q), params.change(q, value), params.end(q);
        };
        keep(*p[drawer], open ? 1 : 0);
        keep(*p[drawerWidth], std::round(size));
    }

    void readouts(float widthPoints)
    {
        static const char *const transforms[] = {"FFT", "CQT"};
        static const char *const fftSizes[] = {"256", "512", "1024", "2048", "4096", "8192", "16384"};
        static const char *const windows[] = {"Rect", "Hann", "Hamming", "Blackman"};
        static const char *const channels[] = {"Mix", "Left", "Right"};
        static const char *const bands[] = {"12", "24", "36"};
        static const char *const rates[] = {"15/s", "30/s", "60/s"};
        static const char *const scales[] = {"Linear", "Log", "Mel"};
        static const char *const palettes[] = {"System", "Bone", "Inferno", "Viridis", "Magma"};
        static const char *const onOff[] = {"Off", "On"};
        static const char *const menu[] = {"Reset to Default"};
        // Each: its setting, label, and a format (with log dragging) or items.
        struct Readout { Setting setting; const char *label, *format; bool log; const char *const *items; int count; };
        static const Readout analysis[] = {
            {transform, "Transform", nullptr, false, transforms, 2}, {fftSize, "FFT", nullptr, false, fftSizes, 7},
            {hopSize, "Hop", "%.0f", true}, {window, "Window", nullptr, false, windows, 4},
            {channel, "Channel", nullptr, false, channels, 3}, {smoothing, "Smooth", "%.2f"},
        };
        static const Readout cqt[] = {
            {cqtBands, "Bands/oct", nullptr, false, bands, 3}, {tuning, "A4", "%.1f Hz"}, {cqtLow, "CQT low", "%.0f Hz", true},
            {cqtHigh, "CQT high", "%.0f Hz", true}, {cqtRate, "Updates", nullptr, false, rates, 3},
        };
        static const Readout display[] = {
            {frequencyScale, "Scale", nullptr, false, scales, 3}, {low, "Low", "%.0f Hz"}, {high, "High", "%.0f Hz", true},
            {floorLevel, "Floor", "%.0f dB"}, {ceiling, "Ceiling", "%.0f dB"}, {palette, "Palette", nullptr, false, palettes, 5},
            {scrollRate, "Scroll", "%.0f px/s"}, {piano, "Piano", nullptr, false, onOff, 2}, {keyTone, "Key tone", nullptr, false, onOff, 2},
            {peakSnap, "Snap", nullptr, false, onOff, 2}, {cursor, "Cursor", nullptr, false, onOff, 2},
        };
        const float room = widthPoints - 12; // inside the drawer's padding
        const int perRow = std::max(1, int(room / 126)); // 126 points: "Window Blackman" and its padding
        const float cell = std::floor(room / float(perRow));
        auto group = [&](const char *title, const Readout *all, int count) {
            CLAY_TEXT(ui.layout.text(title), CLAY_TEXT_CONFIG({.textColor = gpu::clayColor(ui.theme.dim), .fontSize = 11}));
            for (int first = 0; first < count; first += perRow)
                CLAY({.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}})
                {
                    for (int i = first; i < std::min(first + perRow, count); ++i)
                    {
                        const Readout &r = all[i];
                        gpu::Param *q = p[r.setting];
                        if (r.items) ui.readout(params, q, r.label, r.items, r.count, cell);
                        else ui.readout(params, q, r.label, r.format, r.log, cell);
                        if (ui.contextMenu(menu, 1) == 0) params.begin(*q), params.change(*q, q->defaultValue), params.end(*q);
                    }
                }
        };
        group("Analysis", analysis, int(std::size(analysis)));
        if (p[transform]->value >= 0.5) group("CQT", cqt, int(std::size(cqt)));
        group("Display", display, int(std::size(display)));
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
    // scale with A4 at the tuning set (as Listener's piano strip). Hold a key
    // to hear it, drag to another to change note, let go to stop: the held
    // key is lit and drawGuide marks its pitch. To screen readers a picture
    // (its keys are for the pointer only).
    void drawPiano(gpu::Rect r)
    {
        const float s = ui.s, w = stripWidth();
        const double a4 = p[tuning]->value;
        const float top = spectrogram.settings().maxFrequency, bottom = std::max(spectrogram.settings().minFrequency, 8.0f);
        auto y = [&](double note) { return spectrogram.yAt(r, float(a4 * std::pow(2.0, (note - 69) / 12))); };
        const int first = int(std::floor(69 + 12 * std::log2(bottom / a4))) - 1, last = int(std::ceil(69 + 12 * std::log2(top / a4))) + 1;

        // The key under the pointer: the nearest note, but beside a black
        // key (past its 0.6 of the width) the white key on that side of its middle.
        const gpu::Ui::Area keys = ui.area(&heldNote, r.x, r.y, w, r.h, gpu::Ui::eachPress);
        if (keys.held && !keys.released)
        {
            const float at = std::clamp(keys.y, r.y, r.y + r.h - 1);
            const double pitch = 69 + 12 * std::log2(spectrogram.frequencyAt(r, at) / a4);
            int note = int(std::lround(pitch));
            if (blackKey(note) && keys.x > r.x + w * 0.6f) note += pitch < note ? -1 : 1;
            playNote(note);
        }
        else if (heldNote >= 0) playNote(-1);

        ui.draw.pushClip(r.x, r.y, w, r.h);
        ui.draw.rect(r.x, r.y, w, r.h, 0, ui.theme.whiteKey);
        const gpu::Color line{ui.theme.blackKey.r, ui.theme.blackKey.g, ui.theme.blackKey.b, 0.35f};
        for (int note = first; note <= last; ++note)
        {
            if (blackKey(note))
            {
                const float y0 = y(note + 0.5), y1 = y(note - 0.5);
                ui.draw.rect(r.x, y0, w * 0.6f, std::max(1.0f, y1 - y0), 0, ui.theme.blackKey);
            }
            else if (!blackKey(note + 1) && y(note) - y(note + 1) > 3 * s) // where two white keys meet (E to F, B to C)
                ui.draw.line(r.x, y(note + 0.5), r.x + w, y(note + 0.5), 1 * s, line);
        }
        if (heldNote >= 0)
        {
            const float y0 = y(heldNote + 0.5), y1 = y(heldNote - 0.5);
            ui.draw.rect(r.x, y0, blackKey(heldNote) ? w * 0.6f : w, std::max(2 * s, y1 - y0), 0, ui.theme.accent);
        }
        ui.draw.popClip();
        ui.draw.line(r.x + w, r.y, r.x + w, r.y + r.h, 1 * s, line);
        ui.accessible(gpu::Accessible::Role::image, "Piano strip", nullptr, {r.x, r.y, w, r.h},
                      "Piano keys up the side, each at its pitch on the frequency scale");
    }

    // Holds a note (-1: none), and tells the plugin the pitch to play, 0 for
    // none; the plugin plays it only while Key tone is on.
    void playNote(int note)
    {
        if (note == heldNote) return;
        heldNote = note;
        const double hz = note < 0 ? 0 : p[tuning]->value * std::pow(2.0, (note - 69) / 12.0);
        send(core::Value::Map{{"type", "tone"}, {"hz", hz}});
    }

    // A line across the plot at the held key's exact pitch, and its name.
    void drawGuide(gpu::Rect r)
    {
        const double hz = p[tuning]->value * std::pow(2.0, (heldNote - 69) / 12.0);
        const float y = spectrogram.yAt(r, float(hz)), s = ui.s;
        if (y < r.y || y > r.y + r.h) return;
        ui.draw.line(r.x + stripWidth(), y, r.x + r.w, y, 1.5f * s, ui.theme.accent);
        char pitch[24], text[48];
        pitchName(float(hz), p[tuning]->value, pitch, sizeof pitch);
        std::snprintf(text, sizeof text, "%s  %.1f Hz", pitch, hz);
        tag(r.x + r.w - 8 * s, y > r.y + r.h - 40 * s ? y - 6 * s : y + 16 * s, text, gpu::Draw2D::Align::right); // below it, clear of the corner's controls
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
        pitchName(at.frequency, p[tuning]->value, pitch, sizeof pitch);
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
    float binRate = 48000, hzRate = 0; // the sample rate, and the one hz was made for
    bool paused = false;
    int heldNote = -1; // the piano key held, or -1
    gpu::Params params; // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Param *p[settingCount] = {};
    gpu::Ui ui;
    bool hovering = false;
    float pointerX = 0, pointerY = 0; // pixels
    unsigned modifiers = 0;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<SpectrogramView>(); }

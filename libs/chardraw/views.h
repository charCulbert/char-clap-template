#pragma once
#include "ui.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// Modelled on, where named below:
// compost https://github.com/charCulbert/compost

// Views of a signal, as compost's compost-waveform, compost-scope and
// compost-spectrogram: they draw what they're given, and the analysis
// (decoding, triggering, FFTs) stays with the caller. Each comes laid out
// (it grows to fill its box) or at a rectangle, as Ui's controls do, and
// tells screen readers what it shows. Built on Ui's public surface only
// (draw, theme, paint, accessible): a view of your own is made the same way.
// Laid out, a view draws when the frame ends: what it's given (samples,
// peaks, the name) must last until Ui::end, as the box it paints does.
namespace gpu {

// A waveform overview: one envelope per channel (left above right), each a
// run of buckets' lowest and highest samples in -1..1, as compost's peaks.
// start and end are the fraction of it shown.
struct Peak { float min, max; };
struct WaveformView { float start = 0, end = 1; };

inline void waveform(Ui &ui, const char *name, const std::vector<std::vector<Peak>> &channels, Rect r, WaveformView view = {})
{
    const float s = ui.s;
    const Theme &t = ui.theme;
    ui.draw.rect(r.x, r.y, r.w, r.h, 0, t.background, 1 * s, Color{t.text.r, t.text.g, t.text.b, 0.3f});
    const float lane = r.h / float(std::max<size_t>(1, channels.size()));
    const float scaleY = std::max(0.0f, lane / 2 - 3 * s);
    const int columns = std::max(1, int(r.w));
    std::vector<float> outline; // across the top, then back along the bottom
    for (size_t c = 0; c < channels.size(); ++c)
    {
        const std::vector<Peak> &peaks = channels[c];
        if (peaks.empty()) continue;
        const float middle = r.y + (float(c) + 0.5f) * lane;
        const double start = std::clamp(double(view.start), 0.0, 1.0), end = std::clamp(double(view.end), start, 1.0);
        const double first = start * peaks.size(), length = (end - start) * peaks.size();
        std::vector<float> top(static_cast<size_t>(columns)), bottom(static_cast<size_t>(columns));
        for (int x = 0; x < columns; ++x) // each pixel column: the extremes of the buckets under it
        {
            const size_t from = std::min(peaks.size() - 1, size_t(first + x * length / columns));
            const size_t to = std::max(from + 1, size_t(std::ceil(first + (x + 1) * length / columns)));
            float lowest = 1, highest = -1;
            for (size_t i = from; i < to && i < peaks.size(); ++i)
                lowest = std::min(lowest, peaks[i].min), highest = std::max(highest, peaks[i].max);
            top[size_t(x)] = middle - std::clamp(highest, -1.0f, 1.0f) * scaleY;
            bottom[size_t(x)] = std::max(top[size_t(x)] + 1 * s, middle - std::clamp(lowest, -1.0f, 1.0f) * scaleY);
        }
        outline.clear();
        for (int x = 0; x < columns; ++x) outline.insert(outline.end(), {r.x + float(x), top[size_t(x)]});
        for (int x = columns - 1; x >= 0; --x) outline.insert(outline.end(), {r.x + float(x) + 1, bottom[size_t(x)]});
        ui.draw.fill(outline.data(), outline.size() / 2, t.accent);
    }
    const char *layout = channels.size() == 1 ? "Mono waveform" : channels.size() == 2 ? "Stereo waveform, left above right"
                                                                                      : "Waveform, channels stacked in order";
    ui.accessible(Accessible::Role::image, name, nullptr, r, channels.empty() ? "Empty waveform" : layout);
}

inline void waveform(Ui &ui, const char *name, const std::vector<std::vector<Peak>> &channels, WaveformView view = {},
                     float heightPoints = 120)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
          .custom = {.customData = ui.paint([&ui, name, &channels, view](Rect r) { waveform(ui, name, channels, r, view); })}}) {}
}

// An oscilloscope: one channel's samples across its width, over a grid of
// 8 by 4 with the zero line; range is the value at the top (and minus it at
// the bottom), around offset.
struct ScopeView { float range = 1, offset = 0; };

inline void scope(Ui &ui, const char *name, const float *samples, size_t count, Rect r, ScopeView view = {})
{
    const float s = ui.s;
    const Theme &t = ui.theme;
    auto ink = [&](float alpha) { return Color{t.text.r, t.text.g, t.text.b, alpha}; };
    ui.draw.pushClip(r.x, r.y, r.w, r.h);
    for (int i = 1; i < 8; ++i) ui.draw.line(r.x + r.w * i / 8, r.y, r.x + r.w * i / 8, r.y + r.h, 1 * s, ink(0.24f));
    for (int i = 1; i < 4; ++i) ui.draw.line(r.x, r.y + r.h * i / 4, r.x + r.w, r.y + r.h * i / 4, 1 * s, ink(0.24f));
    const float middle = r.y + r.h / 2;
    auto y = [&](float v) { return middle - (v - view.offset) / std::max(view.range, 1e-6f) * r.h * 0.46f; };
    ui.draw.line(r.x, y(0), r.x + r.w, y(0), 1 * s, ink(0.3f));
    if (samples && count >= 2)
    {
        std::vector<float> xy(count * 2);
        for (size_t i = 0; i < count; ++i) xy[i * 2] = r.x + r.w * float(i) / float(count - 1), xy[i * 2 + 1] = y(samples[i]);
        ui.draw.polyline(xy.data(), count, 2 * s, t.text);
    }
    ui.draw.popClip();
    ui.draw.rect(r.x, r.y, r.w, r.h, 0, {0, 0, 0, 0}, 1 * s, ink(0.3f));
    char about[96];
    std::snprintf(about, sizeof about, "One-channel waveform, from %.4g to %.4g", view.offset - view.range, view.offset + view.range);
    ui.accessible(Accessible::Role::image, name, nullptr, r, about);
}

inline void scope(Ui &ui, const char *name, const float *samples, size_t count, ScopeView view = {}, float heightPoints = 160)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
          .custom = {.customData = ui.paint([&ui, name, samples, count, view](Rect r) { scope(ui, name, samples, count, r, view); })}}) {}
}

// A colour scheme: colours from quiet to loud, 2 to 9 of them, which a view
// blends evenly into nine (ramp). Take a ready-made one (preset, named) or
// list your own, as {{theme.background, theme.accent}}.
struct ColourScheme
{
    std::vector<Color> colours;

    // The ready-made ones, nine even steps of each. Magma is the default; a
    // new one is a line here.
    struct Preset { const char *name; uint32_t colours[9]; };
    static constexpr Preset presets[] = {
        // Magma, Viridis, Inferno and Plasma: Nathaniel Smith and Stéfan van der Walt's, from matplotlib (CC0)
        {"Magma", {0x000004, 0x1d1147, 0x51127c, 0x832681, 0xb73779, 0xe75263, 0xfc8961, 0xfec488, 0xfcfdbf}},
        {"Viridis", {0x440154, 0x472d7b, 0x3b528b, 0x2c728e, 0x21918c, 0x28ae80, 0x5ec962, 0xaddc30, 0xfde725}},
        {"Inferno", {0x000004, 0x210c4a, 0x57106e, 0x8a226a, 0xbc3754, 0xe45a31, 0xf98e09, 0xf9cb35, 0xfcffa4}},
        {"Plasma", {0x0d0887, 0x4c02a1, 0x7e03a8, 0xaa2395, 0xcc4778, 0xe66c5c, 0xf89540, 0xfdc527, 0xf0f921}},
        // Cividis: Nuñez, Anderton and Renslow's, from matplotlib (Battelle Memorial Institute, BSD-style)
        {"Cividis", {0x00224e, 0x1a386f, 0x434e6c, 0x61656f, 0x7d7c78, 0x9b9476, 0xbcae6c, 0xdec958, 0xfee838}},
        // Turbo: Anton Mikhailov's, Google (Apache-2.0)
        {"Turbo", {0x30123b, 0x466be3, 0x28bceb, 0x32f298, 0xa4fc3c, 0xeecf3a, 0xfb7e21, 0xd02f05, 0x7a0403}},
        {"Grayscale", {0x000000, 0x202020, 0x404040, 0x606060, 0x808080, 0x9f9f9f, 0xbfbfbf, 0xdfdfdf, 0xffffff}},
    };
    static constexpr int presetCount = int(sizeof presets / sizeof presets[0]);

    // A ready-made one by its place in presets, or its name; Magma if there's none.
    static ColourScheme preset(int index)
    {
        const Preset &p = presets[index >= 0 && index < presetCount ? index : 0];
        ColourScheme scheme;
        for (uint32_t c : p.colours) scheme.colours.push_back({float(c >> 16 & 255) / 255, float(c >> 8 & 255) / 255, float(c & 255) / 255});
        return scheme;
    }
    static ColourScheme named(std::string_view name)
    {
        for (int i = 0; i < presetCount; ++i)
            if (name == presets[i].name) return preset(i);
        return preset(0);
    }

    // Nine colours from quiet to loud, blended evenly from these; Magma's
    // with fewer than two.
    static constexpr int rampColours = 9;
    void ramp(Color out[rampColours]) const
    {
        const int n = int(colours.size());
        if (n < 2)
        {
            preset(0).ramp(out);
            return;
        }
        for (int i = 0; i < rampColours; ++i)
        {
            const float position = float(i) * float(n - 1) / (rampColours - 1);
            const int lower = std::min(n - 2, int(position));
            const float f = position - float(lower);
            const Color &a = colours[size_t(lower)], &b = colours[size_t(lower) + 1];
            out[i] = {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
        }
    }
};

// A live magnitude spectrum, as an analyzer shows one: levels in decibels at
// frequencies (an FFT's bins, or an analyzer's bands), drawn as a curve
// across a log frequency axis over a grid, with a fill under it, brighter
// higher up. The curve dims as it nears the floor and falls through it, out
// of sight, as Pro-Q's does. tilt raises the highs and lowers the lows by so
// many dB per octave around pivot: music falls off about 4.5 dB an octave
// (Pro-Q's default), so with that it looks roughly level. With scales,
// frequencies are marked along the bottom and levels up the left, on the
// plot, and the floor sits above the frequencies' labels. Smoothing over
// time is the caller's, with the analysis.
struct SpectrumView
{
    float minFrequency = 20, maxFrequency = 20000;
    float minDecibels = -100, maxDecibels = 0;
    float tilt = 0, pivot = 1000; // dB per octave, around pivot (Hz)
    bool scales = true;
    // Colours, the theme's where unset: the curve 30 dB up from the floor
    // (accent), the fill at its top, fading to the background below (a tint
    // of the accent; keep it opaque), the grid (the text, faint), and the
    // scales' labels and long ticks (dim).
    std::optional<Color> curveColour, fillColour, gridColour, labelColour;

    // Where the levels' range falls in r (pixels; s is pixels per point): all
    // of it, or with scales, less room for the top label and the frequencies.
    Rect plot(Rect r, float s) const { return scales ? Rect{r.x, r.y + 12 * s, r.w, std::max(1.0f, r.h - 36 * s)} : r; }
    // A frequency's x and a level's y in a plot (pixels).
    float x(Rect plot, float hz) const
    {
        const float low = std::max(minFrequency, 1e-3f), high = std::max(maxFrequency, low * 1.001f);
        return plot.x + plot.w * std::log(std::max(hz, 1e-3f) / low) / std::log(high / low);
    }
    float y(Rect plot, float decibels) const
    {
        return plot.y + plot.h * (maxDecibels - decibels) / std::max(maxDecibels - minDecibels, 1e-6f);
    }

    // The curve through decibels[i] at frequencies[i] (Hz, ascending), tilted,
    // as points x, y, ... a pixel apart across the plot. Each point is the
    // loudest value within half a pixel of it, or where there's none (bins
    // further apart than pixels, in the lows), on a smooth curve between the
    // two either side that never overshoots them (Fritsch-Carlson). A level
    // past the floor goes no lower than a whole range below it.
    void curve(const float *decibels, const float *frequencies, size_t count, Rect plot, std::vector<float> &xy) const
    {
        xy.clear();
        const int n = std::max(1, int(std::ceil(plot.w)));
        const float lowest = minDecibels - (maxDecibels - minDecibels);
        std::vector<float> at(count), level(count), slope(count, 0.0f); // across the plot, 0..1; the level; its slope there
        for (size_t i = 0; i < count; ++i)
        {
            at[i] = (x(plot, frequencies[i]) - plot.x) / std::max(plot.w, 1e-6f);
            level[i] = std::isfinite(decibels[i]) ? std::max(decibels[i], lowest) : lowest;
        }
        for (size_t i = 1; i + 1 < count; ++i)
        {
            const float before = (level[i] - level[i - 1]) / std::max(at[i] - at[i - 1], 1e-9f);
            const float after = (level[i + 1] - level[i]) / std::max(at[i + 1] - at[i], 1e-9f);
            slope[i] = before * after > 0 ? 2 / (1 / before + 1 / after) : 0; // flat at peaks and dips
        }
        const float low = std::max(minFrequency, 1e-3f), high = std::max(maxFrequency, low * 1.001f);
        size_t next = 0; // the first bin not left of this point's half pixel
        for (int k = 0; k <= n; ++k)
        {
            const float u = float(k) / float(n), half = 0.5f / float(n);
            while (next < count && at[next] < u - half) ++next;
            float v = -INFINITY;
            for (size_t i = next; i < count && at[i] < u + half; ++i) v = std::max(v, level[i]);
            if (v == -INFINITY)
            {
                if (count == 0) v = lowest;
                else if (next == 0 || next >= count) v = level[next == 0 ? 0 : count - 1]; // past the ends: the end's level
                else
                {
                    const size_t a = next - 1, b = next;
                    const float h = at[b] - at[a], t = (u - at[a]) / std::max(h, 1e-9f), t2 = t * t, t3 = t2 * t;
                    v = (2 * t3 - 3 * t2 + 1) * level[a] + (t3 - 2 * t2 + t) * h * slope[a] + (-2 * t3 + 3 * t2) * level[b]
                      + (t3 - t2) * h * slope[b];
                }
            }
            v += tilt * std::log2(low * std::pow(high / low, u) / std::max(pivot, 1e-3f));
            xy.insert(xy.end(), {plot.x + plot.w * u, std::min(y(plot, v), y(plot, lowest))});
        }
    }
};

inline void spectrum(Ui &ui, const char *name, const float *decibels, const float *frequencies, size_t count, Rect r,
                     SpectrumView view = {})
{
    const float s = ui.s;
    const Theme &t = ui.theme;
    auto mix = [](Color a, Color b, float f) { return Color{a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f}; };
    auto ink = [&](float alpha) { return Color{t.text.r, t.text.g, t.text.b, alpha}; };
    const Rect p = view.plot(r, s);
    const float bottom = r.y + r.h;
    std::vector<float> xy;
    if (decibels && frequencies) view.curve(decibels, frequencies, count, p, xy);
    ui.draw.pushClip(r.x, r.y, r.w, r.h);
    ui.draw.rect(r.x, r.y, r.w, r.h, 0, t.background);

    // The fill: a strip a pixel wide under each step of the curve, opaque and
    // a little wider than the step, so strips overlap without seams.
    const Color fill = view.fillColour.value_or(mix(t.background, t.accent, 0.16f));
    const Draw2D::Gradient shade{fill, mix(t.background, fill, 0.1875f), 0, p.y, 0, p.y + p.h}; // the theme's: 16% to 3% accent
    for (size_t i = 0; i + 3 < xy.size(); i += 2)
    {
        const float top = (xy[i + 1] + xy[i + 3]) / 2;
        if (top < bottom) ui.draw.rect(xy[i], top, xy[i + 2] - xy[i] + 1, bottom - top, {}, shade);
    }

    // Round frequencies (1, 2 and 5 times a power of ten) and levels (a step
    // of 1, 2 or 5 times one, 16 points apart or more).
    std::vector<float> hz, db;
    for (double decade = std::pow(10.0, std::floor(std::log10(std::max(view.minFrequency, 1.0f)))); decade <= view.maxFrequency; decade *= 10)
        for (double k : {1.0, 2.0, 5.0})
            if (decade * k >= view.minFrequency && decade * k <= view.maxFrequency) hz.push_back(float(decade * k));
    const float range = std::max(view.maxDecibels - view.minDecibels, 1e-3f);
    float step = 1;
    for (int i = 0; step * p.h / range < 16 * s && i < 12; ++i) step *= i % 3 == 1 ? 2.5f : 2; // 1, 2, 5, 10, 20, 50...
    for (float d = std::floor(view.maxDecibels / step) * step; d >= view.minDecibels - 1e-3f; d -= step) db.push_back(d);
    const Color grid = view.gridColour.value_or(ink(0.07f));
    for (float f : hz) ui.draw.line(view.x(p, f), r.y, view.x(p, f), bottom, 1 * s, grid);
    for (float d : db) ui.draw.line(r.x, view.y(p, d), r.x + r.w, view.y(p, d), 1 * s, grid);

    // The curve, from the theme's dim colour at the floor to its accent 30 dB
    // up: a polyline per run of points of one shade, of eight.
    auto shadeOf = [&](size_t i) {
        const float level = view.maxDecibels - (xy[i + 1] - p.y) / std::max(p.h, 1e-6f) * range;
        return int(std::lround(std::clamp((level - view.minDecibels) / 30, 0.0f, 1.0f) * 7));
    };
    for (size_t from = 0, i = 2; i < xy.size(); i += 2)
        if (i + 2 >= xy.size() || shadeOf(i) != shadeOf(from))
        {
            const float f = float(shadeOf(from)) / 7;
            ui.draw.polyline(&xy[from], (i - from) / 2 + 1, 1.5f * s, mix(t.dim, view.curveColour.value_or(t.accent), f * f * (3 - 2 * f)));
            from = i;
        }

    if (view.scales)
    {
        const Color labels = view.labelColour.value_or(t.dim);
        auto label = [&](float x, float y, const char *text, Draw2D::Align align) {
            return ui.draw.text(std::round(x), std::round(y), 11 * s, text, labels, align, t.valueFont);
        };
        // Frequencies along the bottom: a short tick at each of 1 to 9 times a
        // power of ten, a longer one and a label at the round ones, where it fits.
        for (double decade = std::pow(10.0, std::floor(std::log10(std::max(view.minFrequency, 1.0f)))); decade <= view.maxFrequency; decade *= 10)
            for (int k = 1; k < 10; ++k)
                if (decade * k >= view.minFrequency && decade * k <= view.maxFrequency)
                    ui.draw.line(view.x(p, float(decade * k)), bottom, view.x(p, float(decade * k)), bottom - 3 * s, 1 * s, t.edge);
        float right = r.x + 34 * s; // clear of the levels' labels
        for (float f : hz)
        {
            char text[16];
            std::snprintf(text, sizeof text, f >= 1000 ? "%gk" : "%g", f >= 1000 ? f / 1000 : f);
            const float x = view.x(p, f), half = ui.draw.measure(11 * s, text, t.valueFont) / 2;
            ui.draw.line(x, bottom, x, bottom - 6 * s, 1 * s, labels);
            if (x - half < right + 8 * s || x + half > r.x + r.w - 2 * s) continue;
            label(x, bottom - 10 * s, text, Draw2D::Align::center);
            right = x + half;
        }
        // Levels up the left: a tick at each step and half step, labelled at each
        // step 6 points clear of its tick, so a tick never reads as a minus sign.
        for (float d = db.empty() ? 0 : db.front(); d >= view.minDecibels - 1e-3f; d -= step / 2)
        {
            const bool major = std::abs(std::remainder(d, step)) < 1e-3f;
            ui.draw.line(r.x, view.y(p, d), r.x + (major ? 4 : 2) * s, view.y(p, d), 1 * s, major ? labels : t.edge);
            char text[16];
            std::snprintf(text, sizeof text, "%g", d == 0 ? 0.0f : d);
            if (major) label(r.x + 10 * s, view.y(p, d) + 4 * s, text, Draw2D::Align::left);
        }
    }
    ui.draw.popClip();

    char about[200];
    int length = std::snprintf(about, sizeof about, "Spectrum: %g Hz to %g Hz on a log scale, %g to %g dB", view.minFrequency,
                               view.maxFrequency, view.minDecibels, view.maxDecibels);
    if (view.tilt != 0 && length > 0 && length < int(sizeof about))
        std::snprintf(about + length, sizeof about - size_t(length), ", tilted %g dB per octave around %g Hz", view.tilt, view.pivot);
    ui.accessible(Accessible::Role::image, name, nullptr, r, about);
}

inline void spectrum(Ui &ui, const char *name, const float *decibels, const float *frequencies, size_t count,
                     SpectrumView view = {}, float heightPoints = 200)
{
    CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
          .custom = {.customData = ui.paint([&ui, name, decibels, frequencies, count, view](Rect r) {
              spectrum(ui, name, decibels, frequencies, count, r, view);
          })}}) {}
}

// A scrolling spectrogram: the caller adds one column at a time (a value in
// decibels per frequency, as an FFT gives), newest on the right, and it keeps
// the last columns that fit. Frequencies go up the side on a linear, log or
// mel scale. It keeps the values it was given, so a new scale, range or
// colour scheme redraws the whole history, and it sends the GPU only new columns.
// Time goes across one column per add(), or (pixelsPerSecond) at a steady
// rate whatever the columns' rate. probe() says what's under the pointer.
// Keep one per view (it's the history); add from the UI thread.
class Spectrogram
{
public:
    enum class Scale { linear, log, mel };
    struct Options
    {
        float minFrequency = 20, maxFrequency = 24000;
        Scale scale = Scale::log;
        float minDecibels = -100, maxDecibels = 0;
        ColourScheme colours = ColourScheme::preset(0);
        int columns = 512, rows = 256; // the history kept, and its resolution up the side
        // 0: each add() is a column, the history stretched across the width.
        // Else time goes across at this many points a second: each add()
        // covers the seconds it's given, and each column is a point's worth
        // of time, the loudest of what fell in it.
        // Then `columns` is the most kept: make it the widest it's drawn, in
        // points, and a few more.
        float pixelsPerSecond = 0;
        // Scrolls smoothly at the rate columns arrive, a little behind the
        // newest, asking for a frame at each of the display's (Ui::animate);
        // else it moves a whole column at a time, as they're added.
        bool smooth = true;
    };

    Spectrogram() { configure(Options{}); }
    explicit Spectrogram(const Options &o) { configure(o); }

    // New options. A different number of columns keeps the newest that fit;
    // a different pixelsPerSecond starts the history again. The same options
    // again change nothing, so it can be called every frame.
    void configure(Options o)
    {
        o.columns = std::max(1, o.columns), o.rows = std::max(1, o.rows);
        const int before = options.columns;
        const bool resized = o.columns != options.columns, restart = o.pixelsPerSecond != options.pixelsPerSecond;
        const bool remap = resized || o.rows != options.rows || o.scale != options.scale || o.minFrequency != options.minFrequency
                        || o.maxFrequency != options.maxFrequency;
        const bool recolour = o.minDecibels != options.minDecibels || o.maxDecibels != options.maxDecibels;
        options = o;
        if (restart || before <= 0) clear();
        else if (resized) keep(before);
        if (remap) mapped = false;
        if (recolour) redraw = true;
    }

    // One column: values[i] decibels at frequencies[i] (Hz, ascending), the
    // last `seconds` of the signal (needed with pixelsPerSecond). Other
    // frequencies than the last column's (a new FFT size) start the history again.
    void add(const float *values, const float *frequencies, size_t count, float seconds = 0)
    {
        if (!values || !frequencies || !count) return;
        if (count != bins.size() || !std::equal(bins.begin(), bins.end(), frequencies))
        {
            bins.assign(frequencies, frequencies + count);
            clear();
        }
        if (options.pixelsPerSecond <= 0)
        {
            push(values);
            return;
        }
        // The column is part of the one being filled, and may fill it and more.
        if (pending.empty()) pending.assign(values, values + count);
        else for (size_t i = 0; i < count; ++i) pending[i] = std::max(pending[i], values[i]);
        filled = std::min(filled + std::max(0.0, double(seconds) * options.pixelsPerSecond), double(options.columns)); // no more than it keeps
        while (filled >= 1)
        {
            push(pending.data());
            filled -= 1;
            pending.assign(values, values + count);
        }
        if (filled <= 0) pending.clear();
    }

    void clear()
    {
        history.assign(size_t(options.columns) * bins.size(), -INFINITY);
        head = 0, mapped = false, changed.clear();
        added = 0, edge = 0, arrivals.clear();
        pending.clear(), filled = 0;
    }

    const Options &settings() const { return options; }

    // The colours' nine (ColourScheme::ramp); levels between the floor and
    // the ceiling are spread evenly across them (a level bar draws the same).
    static constexpr int rampColours = ColourScheme::rampColours;
    void ramp(Color out[rampColours]) const { options.colours.ramp(out); }

    // Draws it at r (pixels).
    void draw(Ui &ui, const char *name, Rect r)
    {
        upload(ui);
        const double n = options.columns;
        const int lag = maxLag();
        // The history's right edge, in columns added, and how many show: all
        // of them, or (scrolling smoothly) all but the ones that may already
        // be overwritten: the edge lags up to `lag` columns behind the
        // newest, and the viewport's left end lags with it, so lag + 1 of
        // the oldest slots (those the newest columns went into) stay out.
        // Across time, a column a point, as many as fit.
        double end = double(added), count = n;
        if (options.smooth && options.columns > lag + 1) end = scroll(ui, lag), count = n - (lag + 1);
        // Across time, a column a point, right-aligned: as many as fit, and
        // the left part empty while fewer are kept.
        Rect at = r;
        if (options.pixelsPerSecond > 0)
        {
            count = std::max(1.0, std::min(count, double(r.w / ui.s)));
            at.w = std::min(r.w, float(count) * ui.s), at.x = r.x + r.w - at.w;
        }
        const double start = std::fmod(std::fmod(end - count, n) + n, n); // where the left edge is, in the ring
        const double first = std::min(count, n - start);                   // up to the ring's end, then from its start
        if (image >= 0)
        {
            const float w1 = float(at.w * first / count);
            ui.draw.image(image, at.x, at.y, w1, at.h, {}, {1, 1, 1, 1}, {float(start / n), 0, float((start + first) / n), 1});
            if (count - first > 1e-6)
                ui.draw.image(image, at.x + w1, at.y, at.w - w1, at.h, {}, {1, 1, 1, 1}, {0, 0, float((count - first) / n), 1});
        }
        drawn = {r, at.x, at.w, end - count, count, ui.s};
        static const char *const scales[] = {"linear", "log", "mel"};
        char about[200];
        int length = std::snprintf(about, sizeof about, "Spectrogram, newest on the right: %g Hz to %g Hz on a %s scale, %g to %g dB",
                                   options.minFrequency, options.maxFrequency, scales[int(options.scale)], options.minDecibels,
                                   options.maxDecibels);
        if (options.pixelsPerSecond > 0 && length > 0 && length < int(sizeof about))
            std::snprintf(about + length, sizeof about - size_t(length), ", the last %.1f s", r.w / ui.s / options.pixelsPerSecond);
        ui.accessible(Accessible::Role::image, name, nullptr, r, about);
    }

    // Laid out: it grows to fill the width, heightPoints tall.
    void draw(Ui &ui, const char *name, float heightPoints = 200)
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
              .custom = {.customData = ui.paint([this, &ui, name](Rect r) { draw(ui, name, r); })}}) {}
    }

    // The frequency at a height in r, and the height of a frequency (pixels).
    float frequencyAt(Rect r, float y) const { return frequencyOf(1 - (y - r.y) / std::max(r.h, 1e-6f)); }
    float yAt(Rect r, float hz) const { return r.y + (1 - fractionOf(hz)) * r.h; }

    // Frequencies to mark up the side of r (pixels), lowest first: round
    // numbers, as many as fit `gap` pixels apart. Linear: a step of 1, 2 or 5
    // times a power of ten. Log and mel, a decade at a time (they crowd
    // differently in each): its start alone, or 1, 2 and 5 times it, or
    // steps of 1, 0.5, 0.2, 0.1... times it (30, 40, 50 in a narrow range;
    // 41, 42, 43 in a narrower one), the finest that fits.
    std::vector<float> frequencyMarks(Rect r, float gap) const
    {
        const double low = options.minFrequency, high = std::max(options.maxFrequency, options.minFrequency + 1);
        auto fits = [&](const std::vector<float> &marks) {
            for (size_t i = 1; i < marks.size(); ++i)
                if (std::abs(yAt(r, marks[i - 1]) - yAt(r, marks[i])) < gap) return false;
            return true;
        };
        // Multiples of step from `from` to `to`, within the range.
        auto multiples = [&](double step, double from, double to) {
            std::vector<float> marks;
            for (double k = std::ceil(std::max(from, low) / step - 1e-9); k * step <= std::min(to, high) * (1 + 1e-9); ++k)
                marks.push_back(float(k * step));
            return marks;
        };
        std::vector<float> marks;
        if (options.scale == Scale::linear)
        {
            // From one step across the range, finer while they fit.
            for (double step = std::pow(10.0, std::ceil(std::log10(high - low))), next; step > (high - low) * 1e-4; step = next)
            {
                std::vector<float> finer = multiples(step, low, high);
                if (!fits(finer)) break;
                marks = std::move(finer);
                const double digit = step / std::pow(10.0, std::floor(std::log10(step) + 1e-9));
                next = step * (digit > 4.5 ? 0.4 : 0.5); // 10, 5, 2, 1, 0.5, ...
            }
            return marks;
        }
        for (double decade = std::pow(10.0, std::floor(std::log10(std::max(low, 1.0)))); decade <= high; decade *= 10)
        {
            std::vector<float> best;
            for (int level = 0;; ++level)
            {
                std::vector<float> here;
                if (level == 0) { if (decade >= low * (1 - 1e-9)) here.push_back(float(decade)); } // the decade's start
                else if (level == 1)
                    for (double k : {1.0, 2.0, 5.0})
                    {
                        if (decade * k >= low * (1 - 1e-9) && decade * k <= high * (1 + 1e-9)) here.push_back(float(decade * k));
                    }
                else
                {
                    // Steps of 1, 0.5, 0.2, 0.1... times the decade.
                    const int finer = level - 2;
                    const double step = decade * std::pow(10.0, -(finer / 3)) * (finer % 3 == 0 ? 1 : finer % 3 == 1 ? 0.5 : 0.2);
                    if (step < decade * 1e-3) break;
                    here = multiples(step, decade, decade * 10 * (1 - 1e-9));
                }
                if (!fits(here)) break;
                if (here.size() >= best.size()) best = std::move(here); // (1, 2 and 5 may show fewer than its start alone)
            }
            for (float f : best) // across decades: none closer than gap to the last kept
                if (marks.empty() || std::abs(yAt(r, marks.back()) - yAt(r, f)) >= gap) marks.push_back(f);
        }
        return marks;
    }

    // What's under a point of the last drawing (pixels): the frequency at
    // that height and the level there in the column under it. Snapping:
    // the nearest peak in that column within 6 points
    // above or below (one already followed counts 2 points nearer, so it
    // doesn't flicker between two, yet one between others can be reached),
    // its frequency and level found between the bins around it; y is then
    // its height.
    // False off the picture, or over columns not yet added.
    struct Probe
    {
        float frequency = 0, decibels = -INFINITY, y = 0;
        bool peak = false; // snapped to one
    };
    bool probe(float x, float y, bool snap, Probe &out)
    {
        const Rect &r = drawn.r;
        if (bins.size() < 3 || x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return false;
        if (x < drawn.left) return false;
        const double column = std::floor(drawn.start + (x - drawn.left) / drawn.width * drawn.count);
        if (column < 0 || column >= double(added) || column < double(added) - options.columns) return false;
        const float *values = &history[size_t(uint64_t(column) % uint64_t(options.columns)) * bins.size()];
        out = {frequencyAt(r, y), -INFINITY, y, false};
        // The level between the two bins either side of the frequency.
        const size_t above = std::min(bins.size() - 1, size_t(std::max<ptrdiff_t>(1, std::lower_bound(bins.begin(), bins.end(), out.frequency) - bins.begin())));
        const float f0 = bins[above - 1], f1 = bins[above], along = std::clamp((out.frequency - f0) / std::max(f1 - f0, 1e-6f), 0.0f, 1.0f);
        out.decibels = values[above - 1] + (values[above] - values[above - 1]) * along;
        if (!std::isfinite(out.decibels)) return false;
        if (!snap) return true;
        size_t best = 0;
        float closest = 6 * drawn.scale;
        for (size_t k = 1; k + 1 < bins.size(); ++k)
        {
            const float v = values[k];
            if (bins[k] < options.minFrequency || bins[k] > options.maxFrequency || !std::isfinite(v) || v < options.minDecibels + 6
                || v < values[k - 1] || v < values[k + 1] || (v == values[k - 1] && v == values[k + 1]))
                continue;
            float distance = std::abs(yAt(r, bins[k]) - y);
            if (bins[k] == followed) distance = std::max(0.0f, distance - 2 * drawn.scale);
            if (distance <= closest) best = k, closest = distance;
        }
        followed = best ? bins[best] : 0;
        if (!best) return true;
        // A parabola through the peak's bin and its neighbours (in dB).
        const float a = values[best - 1], b = values[best], c = values[best + 1], curve = a - 2 * b + c;
        const float p = curve < 0 ? std::clamp(0.5f * (a - c) / curve, -0.5f, 0.5f) : 0;
        out.frequency = bins[best] + p * (bins[best + 1] - bins[best - 1]) / 2;
        out.decibels = b - 0.25f * (a - c) * p;
        out.y = yAt(r, out.frequency);
        out.peak = true;
        return true;
    }

private:
    // Adds a column of bins.size() values on the right.
    void push(const float *values)
    {
        std::copy_n(values, bins.size(), &history[size_t(head) * bins.size()]);
        // Not drawn for a while (hidden): past a full turn, upload all of it
        // once rather than keep a list that grows without end.
        if (int(changed.size()) < options.columns) changed.push_back(head);
        else redraw = true, changed.clear();
        head = (head + 1) % options.columns;
        ++added;
        arrivals.push_back(std::chrono::steady_clock::now());
        if (arrivals.size() > 32) arrivals.pop_front();
    }

    // A new number of columns: the newest that fit stay, each where the
    // column count puts it (column a at a % columns, as push writes them).
    void keep(int before)
    {
        const size_t n = bins.size();
        std::vector<float> kept(size_t(options.columns) * n, -INFINITY);
        const uint64_t count = std::min<uint64_t>(added, uint64_t(std::min(before, options.columns)));
        for (uint64_t a = added - count; a < added; ++a)
            std::copy_n(&history[size_t(a % uint64_t(before)) * n], n, &kept[size_t(a % uint64_t(options.columns)) * n]);
        history.swap(kept);
        head = int(added % uint64_t(options.columns));
        changed.clear();
    }

    // How far the smooth edge may lag the newest column: three columns, or
    // across time three of the bursts columns tend to come in (an interface
    // gets them about 60 times a second) and a little more.
    int maxLag() const { return options.pixelsPerSecond > 0 ? std::max(3, int(std::ceil(options.pixelsPerSecond / 20)) + 2) : 3; }

    // Where the right edge is now (in columns added): moving at the rate they
    // arrive, so a pair arriving together doesn't jump, a column behind the
    // newest; caught up if it falls `lag` behind; still when they stop.
    double scroll(Ui &ui, int lag)
    {
        using namespace std::chrono;
        const steady_clock::time_point now = steady_clock::now();
        const double dt = std::min(0.1, duration<double>(now - lastDraw).count());
        lastDraw = now;
        const bool flowing = arrivals.size() >= 2 && duration<double>(now - arrivals.back()).count() < 0.25;
        if (!flowing)
        {
            edge = double(added);
            return edge;
        }
        const double rate = double(arrivals.size() - 1) / std::max(1e-3, duration<double>(arrivals.back() - arrivals.front()).count());
        edge = std::clamp(edge + dt * rate, double(added) - lag, double(added));
        if (edge <= double(added) - lag) edge = double(added) - std::max(1.5, lag / 2.0); // fell behind: back to the middle
        ui.animate();
        return edge;
    }

    // The frequency at a fraction of the height, 0 at the bottom, and back.
    float frequencyOf(float fraction) const
    {
        const float low = options.minFrequency, high = std::max(options.maxFrequency, low + 1);
        if (options.scale == Scale::log)
        {
            const float floor = std::max(low, 1.0f);
            return floor * std::pow(high / floor, fraction);
        }
        if (options.scale == Scale::mel)
            return 700 * (std::pow(10.0f, (mel(low) + (mel(high) - mel(low)) * fraction) / 2595) - 1);
        return low + (high - low) * fraction;
    }
    float fractionOf(float hz) const
    {
        const float low = options.minFrequency, high = std::max(options.maxFrequency, low + 1);
        if (options.scale == Scale::log)
        {
            const float floor = std::max(low, 1.0f);
            return std::log(std::max(hz, 1e-3f) / floor) / std::log(high / floor);
        }
        if (options.scale == Scale::mel) return (mel(hz) - mel(low)) / (mel(high) - mel(low));
        return (hz - low) / (high - low);
    }
    static float mel(float hz) { return 2595 * std::log10(1 + hz / 700); }

    // Which bins each row shows, top row first: the loudest of those in its
    // band, or between the two either side of a band with none in it.
    struct Band { size_t from, to; float between; };
    void map()
    {
        bands.resize(size_t(options.rows));
        const size_t count = bins.size();
        for (int row = 0; row < options.rows; ++row)
        {
            const float high = frequencyOf(1 - float(row) / float(options.rows)), low = frequencyOf(1 - float(row + 1) / float(options.rows));
            Band &b = bands[size_t(row)];
            b.from = size_t(std::lower_bound(bins.begin(), bins.end(), low) - bins.begin());
            b.to = size_t(std::upper_bound(bins.begin(), bins.end(), high) - bins.begin());
            b.between = -1;
            if (b.from >= b.to && b.from > 0 && b.from < count)
            {
                const float centre = (low + high) / 2, f0 = bins[b.from - 1], f1 = bins[b.from];
                b.between = std::clamp((centre - f0) / (f1 - f0), 0.0f, 1.0f);
            }
        }
        mapped = true;
    }

    // Colours one column of pixels from its values.
    void colour(int column, const Color ramp[rampColours])
    {
        const float span = std::max(options.maxDecibels - options.minDecibels, 1e-6f);
        const float *values = bins.empty() ? nullptr : &history[size_t(column) * bins.size()];
        for (int row = 0; row < options.rows; ++row)
        {
            float value = -INFINITY;
            if (values)
            {
                const Band &b = bands[size_t(row)];
                for (size_t i = b.from; i < b.to; ++i) value = std::max(value, values[i]);
                if (b.between >= 0) value = values[b.from - 1] + (values[b.from] - values[b.from - 1]) * b.between;
            }
            const float level = std::isfinite(value) ? std::clamp((value - options.minDecibels) / span, 0.0f, 1.0f) : 0;
            const float position = level * (rampColours - 1);
            const int lower = std::min(rampColours - 2, int(position));
            const float f = position - float(lower);
            const Color &a = ramp[lower], &b = ramp[lower + 1];
            uint8_t *p = &pixels[(size_t(row) * size_t(options.columns) + size_t(column)) * 4];
            p[0] = uint8_t(std::lround((a.r + (b.r - a.r) * f) * 255));
            p[1] = uint8_t(std::lround((a.g + (b.g - a.g) * f) * 255));
            p[2] = uint8_t(std::lround((a.b + (b.b - a.b) * f) * 255));
            p[3] = 255;
        }
    }

    // Brings the image up to date: all of it when it's new (or the device was
    // lost, or the size, mapping or colours changed), else the columns added since.
    void upload(Ui &ui)
    {
        Color now[rampColours];
        ramp(now);
        bool all = !mapped || redraw || image < 0 || prepared != ui.draw.prepared() || imageColumns != options.columns || imageRows != options.rows;
        for (int i = 0; i < rampColours; ++i) all |= now[i].r != shown[i].r || now[i].g != shown[i].g || now[i].b != shown[i].b;
        if (all)
        {
            if (!mapped) map();
            pixels.assign(size_t(options.columns) * size_t(options.rows) * 4, 0);
            for (int c = 0; c < options.columns; ++c) colour(c, now);
            std::copy(now, now + rampColours, shown);
            redraw = false;
            if (prepared != ui.draw.prepared()) images.clear(), image = -1, prepared = ui.draw.prepared();
            if (image < 0 || imageColumns != options.columns || imageRows != options.rows)
            {
                // Draw2D can't drop an image, so one made for a size is kept and
                // reused when that size comes back: toggling between sizes costs
                // one texture per distinct size used, not one per toggle.
                image = -1;
                for (const Sized &s : images)
                    if (s.columns == options.columns && s.rows == options.rows) image = s.image;
                if (image < 0)
                {
                    image = ui.draw.addImage(pixels.data(), options.columns, options.rows);
                    if (image >= 0) images.push_back({options.columns, options.rows, image});
                }
                else ui.draw.updateImage(image, pixels.data(), 0, 0, options.columns, options.rows);
                imageColumns = options.columns, imageRows = options.rows;
            }
            else ui.draw.updateImage(image, pixels.data(), 0, 0, options.columns, options.rows);
            changed.clear();
            return;
        }
        std::vector<uint8_t> column(size_t(options.rows) * 4);
        for (int c : changed)
        {
            colour(c, now);
            for (int row = 0; row < options.rows; ++row)
                std::copy_n(&pixels[(size_t(row) * size_t(options.columns) + size_t(c)) * 4], 4, &column[size_t(row) * 4]);
            ui.draw.updateImage(image, column.data(), c, 0, 1, options.rows);
        }
        changed.clear();
    }

    Options options{0, 0, Scale::log, 0, 0, {}, 0, 0};
    std::vector<float> bins;    // the frequencies of the values added
    std::vector<float> pending; // across time: the column being filled, the loudest of what fell in it
    double filled = 0;          // and how much of it is (0..1)
    // The last drawing: its box, the part the columns took (pixels), from which column, how many, and the scale.
    struct { Rect r; float left, width; double start, count; float scale; } drawn{};
    float followed = 0;         // the peak probe() snapped to last, by its bin's frequency
    std::vector<float> history; // columns x bins: a ring of the values added
    std::vector<Band> bands;    // each row's bins, for the scale and range
    std::vector<uint8_t> pixels; // rows x columns, coloured
    std::vector<int> changed;   // columns added since the last upload
    int head = 0;               // the next column to write: the oldest
    uint64_t added = 0;         // columns added since the history began
    double edge = 0;            // smooth: the right edge's place, in columns added
    std::deque<std::chrono::steady_clock::time_point> arrivals; // the last columns' times
    std::chrono::steady_clock::time_point lastDraw;
    int image = -1, imageColumns = 0, imageRows = 0; // the texture in use, and its size
    struct Sized { int columns, rows, image; };
    std::vector<Sized> images; // every texture made, by size, for reuse
    unsigned prepared = 0; // Draw2D::prepared() when the image was added
    bool mapped = false, redraw = true; // the rows' bins are known; the whole picture is to be coloured again
    Color shown[rampColours] = {};
};

}

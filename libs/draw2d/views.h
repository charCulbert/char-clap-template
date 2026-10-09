#pragma once
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Views of a signal, as compost's compost-waveform, compost-scope and
// compost-spectrogram: they draw what they're given, and the analysis
// (decoding, triggering, FFTs) stays with the caller. Each comes laid out
// (it grows to fill its box) or at a rectangle, as Ui's controls do, and
// tells screen readers what it shows. Built on Ui's public surface only
// (draw, theme, paint, accessible): a view of your own is made the same way.
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
        const double first = view.start * peaks.size(), length = (view.end - view.start) * peaks.size();
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

// A scrolling spectrogram: the caller adds one column at a time (a value in
// decibels per frequency, as an FFT gives), newest on the right, and it keeps
// the last columns that fit. Frequencies go up the side on a linear, log or
// mel scale. It keeps the values it was given, so a new scale, range or
// palette redraws the whole history, and it sends the GPU only new columns.
// Keep one per view (it's the history); add from the UI thread.
class Spectrogram
{
public:
    enum class Scale { linear, log, mel };
    enum class Palette { system, bone, inferno, viridis, magma }; // system: the theme's background to its text
    struct Options
    {
        float minFrequency = 20, maxFrequency = 24000;
        Scale scale = Scale::log;
        float minDecibels = -100, maxDecibels = 0;
        Palette palette = Palette::system;
        int columns = 512, rows = 256; // the history kept, and its resolution up the side
    };

    Spectrogram() { configure(Options{}); }
    explicit Spectrogram(const Options &o) { configure(o); }

    // New options; a different number of columns starts the history again.
    // The same options again change nothing, so it can be called every frame.
    void configure(Options o)
    {
        o.columns = std::max(1, o.columns), o.rows = std::max(1, o.rows);
        const bool resized = o.columns != options.columns;
        const bool remap = resized || o.rows != options.rows || o.scale != options.scale || o.minFrequency != options.minFrequency
                        || o.maxFrequency != options.maxFrequency;
        const bool recolour = o.palette != options.palette || o.minDecibels != options.minDecibels || o.maxDecibels != options.maxDecibels;
        options = o;
        if (resized) clear();
        if (remap) mapped = false;
        if (recolour) redraw = true;
    }

    // One column: values[i] decibels at frequencies[i] (Hz, ascending). Other
    // frequencies than the last column's (a new FFT size) start the history again.
    void add(const float *values, const float *frequencies, size_t count)
    {
        if (!values || !frequencies || !count) return;
        if (count != bins.size() || !std::equal(bins.begin(), bins.end(), frequencies))
        {
            bins.assign(frequencies, frequencies + count);
            clear();
        }
        std::copy_n(values, count, &history[size_t(head) * count]);
        changed.push_back(head);
        head = (head + 1) % options.columns;
    }

    void clear()
    {
        history.assign(size_t(options.columns) * bins.size(), -INFINITY);
        head = 0, mapped = false, changed.clear();
    }

    const Options &settings() const { return options; }

    // Draws it at r (pixels).
    void draw(Ui &ui, const char *name, Rect r)
    {
        upload(ui);
        const float split = float(head) / float(options.columns); // the oldest column is head's
        const float older = r.w * (1 - split);
        if (image >= 0)
        {
            ui.draw.image(image, r.x, r.y, older, r.h, {}, {1, 1, 1, 1}, {split, 0, 1, 1});
            if (head > 0) ui.draw.image(image, r.x + older, r.y, r.w - older, r.h, {}, {1, 1, 1, 1}, {0, 0, split, 1});
        }
        const Color &t = ui.theme.text;
        ui.draw.rect(r.x, r.y, r.w, r.h, 0, {0, 0, 0, 0}, 1 * ui.s, Color{t.r, t.g, t.b, 0.3f});
        static const char *const scales[] = {"linear", "log", "mel"};
        char about[160];
        std::snprintf(about, sizeof about, "Spectrogram, newest on the right: %g Hz to %g Hz on a %s scale, %g to %g dB",
                      options.minFrequency, options.maxFrequency, scales[int(options.scale)], options.minDecibels, options.maxDecibels);
        ui.accessible(Accessible::Role::image, name, nullptr, r, about);
    }

    // Laid out: it grows to fill the width, heightPoints tall.
    void draw(Ui &ui, const char *name, float heightPoints = 200)
    {
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(heightPoints)}},
              .custom = {.customData = ui.paint([this, &ui, name](Rect r) { draw(ui, name, r); })}}) {}
    }

private:
    // The frequency at a fraction of the height, 0 at the bottom.
    float frequencyAt(float fraction) const
    {
        const float low = options.minFrequency, high = std::max(options.maxFrequency, low + 1);
        if (options.scale == Scale::log)
        {
            const float floor = std::max(low, 1.0f);
            return floor * std::pow(high / floor, fraction);
        }
        if (options.scale == Scale::mel)
        {
            auto mel = [](float f) { return 2595 * std::log10(1 + f / 700); };
            return 700 * (std::pow(10.0f, (mel(low) + (mel(high) - mel(low)) * fraction) / 2595) - 1);
        }
        return low + (high - low) * fraction;
    }

    // Which bins each row shows, top row first: the loudest of those in its
    // band, or between the two either side of a band with none in it.
    struct Band { size_t from, to; float between; };
    void map()
    {
        bands.resize(size_t(options.rows));
        const size_t count = bins.size();
        for (int row = 0; row < options.rows; ++row)
        {
            const float high = frequencyAt(1 - float(row) / float(options.rows)), low = frequencyAt(1 - float(row + 1) / float(options.rows));
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

    // Five colours from quiet to loud, as compost's ramps.
    void ramp(const Theme &theme, Color out[5]) const
    {
        auto hex = [](uint32_t c) { return Color{float(c >> 16 & 255) / 255, float(c >> 8 & 255) / 255, float(c & 255) / 255}; };
        static const uint32_t palettes[][5] = {
            {0x0a0b0c, 0x191918, 0x45433e, 0x8e897d, 0xf5ebd4}, // bone (Listener)
            {0x000000, 0x1d0000, 0x730000, 0xff6100, 0xfff843}, // inferno (Listener)
            {0x2d14d7, 0x2e33cf, 0x3a8ac7, 0x6ed3a8, 0xfaf255}, // viridis (Listener)
            {0x000004, 0x51127c, 0xb73779, 0xfc8961, 0xfcfdbf}, // magma (compost)
        };
        if (options.palette == Palette::system) // the text mixed into the background: 0, 14, 34, 66, 100%
        {
            const float mix[] = {0, 0.14f, 0.34f, 0.66f, 1};
            for (int i = 0; i < 5; ++i)
                out[i] = {theme.background.r + (theme.text.r - theme.background.r) * mix[i],
                          theme.background.g + (theme.text.g - theme.background.g) * mix[i],
                          theme.background.b + (theme.text.b - theme.background.b) * mix[i]};
            return;
        }
        for (int i = 0; i < 5; ++i) out[i] = hex(palettes[int(options.palette) - 1][i]);
    }

    // Colours one column of pixels from its values.
    void colour(int column, const Color ramp[5])
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
            const float position = level * 4;
            const int lower = std::min(3, int(position));
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
    // lost, or the mapping or colours changed), else the columns added since.
    void upload(Ui &ui)
    {
        Color now[5];
        ramp(ui.theme, now);
        bool all = !mapped || redraw || image < 0 || prepared != ui.draw.prepared() || imageRows != options.rows;
        for (int i = 0; i < 5; ++i) all |= now[i].r != shown[i].r || now[i].g != shown[i].g || now[i].b != shown[i].b;
        if (all)
        {
            if (!mapped) map();
            pixels.assign(size_t(options.columns) * size_t(options.rows) * 4, 0);
            for (int c = 0; c < options.columns; ++c) colour(c, now);
            std::copy(now, now + 5, shown);
            redraw = false;
            if (image < 0 || prepared != ui.draw.prepared() || imageRows != options.rows)
                image = ui.draw.addImage(pixels.data(), options.columns, options.rows), prepared = ui.draw.prepared(), imageRows = options.rows;
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

    Options options{0, 0, Scale::log, 0, 0, Palette::system, 0, 0};
    std::vector<float> bins;    // the frequencies of the values added
    std::vector<float> history; // columns x bins: a ring of the values added
    std::vector<Band> bands;    // each row's bins, for the scale and range
    std::vector<uint8_t> pixels; // rows x columns, coloured
    std::vector<int> changed;   // columns added since the last upload
    int head = 0;               // the next column to write: the oldest
    int image = -1, imageRows = 0;
    unsigned prepared = 0; // Draw2D::prepared() when the image was added
    bool mapped = false, redraw = true; // the rows' bins are known; the whole picture is to be coloured again
    Color shown[5] = {};
};

}

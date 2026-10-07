// GPU Analyzer's interface: the live spectrum, drawn by one fragment shader,
// with labels and three knobs (Smoothing, Colour, Tilt) from draw2d's gpu::Ui on top,
// laid out with Clay. The spectrum arrives from the plugin as "spectrum"
// messages; the knobs edit the plugin's settings (see plugin.cpp).
#include "draw2d/ui.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>

namespace {

// One full-screen triangle; everything is computed per pixel.
constexpr const char *shader = R"(
struct Uniforms {
    size: vec4f,    // width, height (pixels), scale, unused
    color: vec4f,   // the spectrum's colour
    ranges: vec4f,  // lowest Hz, highest Hz, unused, unused
    plot: vec4f,    // plot area: left, top, right, bottom (pixels)
};
@group(0) @binding(0) var<uniform> u: Uniforms;
// The live spectrum from the plugin: bands in dB, log-spaced over the plot.
@group(0) @binding(1) var<storage, read> spectrum: array<f32>;
fn bandAt(x: f32) -> f32 { // fractional band index at a pixel x; band d covers [d, d+1)/n
    let n = f32(arrayLength(&spectrum));
    return clamp(clamp((x - u.plot.x) / (u.plot.z - u.plot.x), 0.0, 1.0) * n - 0.5, 0.0, n - 1.0);
}
fn band(i: i32) -> f32 {
    return spectrum[u32(clamp(i, 0, i32(arrayLength(&spectrum)) - 1))];
}
// The tangent at a band for a monotone curve: flat at peaks and dips, so
// the curve never overshoots the bands (Fritsch-Carlson).
fn tangent(before: f32, after: f32) -> f32 {
    if (before * after <= 0.0) { return 0.0; }
    return 2.0 / (1.0 / before + 1.0 / after);
}
fn spectrumDb(x: f32) -> f32 {
    // A smooth curve through the bands around this pixel.
    let t = bandAt(x);
    let i = i32(floor(t));
    let f = t - floor(t);
    let p0 = band(i - 1);
    let p1 = band(i);
    let p2 = band(i + 1);
    let p3 = band(i + 2);
    let m1 = tangent(p1 - p0, p2 - p1);
    let m2 = tangent(p2 - p1, p3 - p2);
    let f2 = f * f;
    let f3 = f2 * f;
    return (2.0 * f3 - 3.0 * f2 + 1.0) * p1 + (f3 - 2.0 * f2 + f) * m1 + (-2.0 * f3 + 3.0 * f2) * p2 + (f3 - f2) * m2;
}
// Distance from a point to the segment a-b.
fn segmentDistance(p: vec2f, a: vec2f, b: vec2f) -> f32 {
    let ab = b - a;
    let t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    return length(p - (a + t * ab));
}

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

const pi = 3.14159265;

// Plot coordinates, inside the plot area the C++ side also uses for its labels.
// x is Signalsmith's frequency scale, hz / (1500 + hz): nearly linear in the
// lows and nearly logarithmic above, like hearing.
fn plotRect() -> vec4f { return u.plot; }
fn warp(hz: f32) -> f32 { return log(hz); } // logarithmic: every octave the same width
fn xOf(f: f32) -> f32 {
    let r = plotRect();
    return r.x + (r.z - r.x) * (warp(f) - warp(u.ranges.x)) / (warp(u.ranges.y) - warp(u.ranges.x));
}
// The level scale: linear in dB, 0 dBFS at the top, the floor at the bottom
// edge; anything quieter rests on it.
const floorDb = -100.0;
fn levelY(dbfs: f32) -> f32 {
    let r = plotRect();
    return r.y + (r.w - r.y) * clamp(dbfs / floorDb, 0.0, 1.0);
}
@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let px = pos.xy;
    let s = u.size.z;
    let r = plotRect();
    let inside = step(r.x, px.x) * step(px.x, r.z) * step(r.y, px.y) * step(px.y, r.w);

    // Background: dark vertical gradient.
    var color = mix(vec3f(0.075, 0.085, 0.10), vec3f(0.035, 0.04, 0.05), px.y / u.size.y);

    // Grid: the labelled frequencies and every 12 dB.
    var grid = 0.0;
    let gridHz = array<f32, 10>(20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0);
    for (var i = 0; i < 10; i++) {
        grid = max(grid, 1.0 - smoothstep(0.0, s, abs(px.x - xOf(gridHz[i]))));
    }
    // Horizontal lines on the spectrum's level scale (dBFS, labelled on the left).
    let gridDb = array<f32, 11>(0.0, -10.0, -20.0, -30.0, -40.0, -50.0, -60.0, -70.0, -80.0, -90.0, -100.0);
    for (var i = 0; i < 11; i++) {
        grid = max(grid, 1.0 - smoothstep(0.0, s, abs(px.y - levelY(gridDb[i]))));
    }
    color += vec3f(0.06, 0.07, 0.08) * grid * inside;

    // The spectrum: a fill below it, brighter higher up, and the line
    // itself, which rests on the bottom edge in silence. The line may spill a
    // little past the plot, so it isn't cut in half there.
    let db = spectrumDb(px.x);
    let ys = levelY(db);
    let nearPlot = step(r.x, px.x) * step(px.x, r.z) * step(r.y - 2.0 * s, px.y) * step(px.y, r.w + 2.0 * s);
    // The line: the true distance to the curve, drawn as segments one pixel
    // wide, looking a few pixels each side, so steep sides stay solid.
    var distance = 1e9;
    var previous = vec2f(px.x - 4.0, levelY(spectrumDb(px.x - 4.0)));
    for (var k = -3.0; k <= 4.0; k += 1.0) {
        let next = vec2f(px.x + k, levelY(spectrumDb(px.x + k)));
        distance = min(distance, segmentDistance(px, previous, next));
        previous = next;
    }
    let below = smoothstep(ys - 0.5, ys + 0.5, px.y) * inside;
    // The fill's brightness follows height on the plot, not distance from the
    // line, so it is the same in every column and doesn't streak.
    let height = 1.0 - clamp((px.y - r.y) / (r.w - r.y), 0.0, 1.0);
    color += u.color.rgb * below * (0.05 + 0.25 * height * height);
    color += u.color.rgb * (1.0 - smoothstep(0.6 * s, 1.6 * s, distance)) * nearPlot;

    return vec4f(color, 1.0);
}
)";

struct Uniforms
{
    float size[4];
    float color[4];
    float ranges[4];
    float plot[4];
};

constexpr float minHz = 10, maxHz = 30000; // past the audible range each side, as Pro-Q

// The plot's frequency scale (the shader's warp/xOf): logarithmic, as a 0..1
// position across the plot.
float hzToUnit(float hz) { return std::log(hz / minHz) / std::log(maxHz / minHz); }

// A bright colour of the given hue (degrees): HSV with value 0.9, saturation 0.6.
gpu::Color hueColor(float hue)
{
    auto channel = [&](float n) {
        const float k = std::fmod(n + hue / 60.0f, 6.0f);
        return 0.9f - 0.9f * 0.6f * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    };
    return {channel(5), channel(3), channel(1)};
}

class AnalyzerView : public gpu::View
{
public:
    void prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format) override
    {
        device = newDevice;
        createPipeline(format);
        ui.draw.prepare(device, format, gpu::loadAsset("/fonts/Inter.ttf"));
        params.send = send;
        ui.lockPointer = lockPointer;
        ui.resizeTo = resizeTo;
        send(core::Value::Map{{"type", "ready"}});
    }

    void receive(const core::Value &message) override
    {
        if (params.receive(message) || message["type"].text() != "spectrum") return;
        auto *db = std::get_if<core::Value::Array>(&message["db"].data);
        const gpu::Param *smoothing = params.find("Smoothing"), *tilt = params.find("Tilt");
        if (!db || !smoothing || !tilt) return;
        // Levels fall back with a time constant Smoothing sets, from 10 ms
        // to 5 s, and rise eight times faster than they fall
        // (messages arrive about 60 times a second).
        const float fallSeconds = 0.01f * std::pow(500.0f, float(smoothing->value) / 100);
        const float fall = 1 - std::exp(-1 / (60 * fallSeconds));
        const float rise = 1 - std::exp(-8 / (60 * fallSeconds));
        for (size_t i = 0; i < spectrum.size() && i < db->size(); ++i)
        {
            const float level = float((*db)[i].number(-120));
            spectrum[i] += (level - spectrum[i]) * (level > spectrum[i] ? rise : fall);
        }
        // What's drawn: each band averaged a little with its neighbours
        // (in power), which calms the noise without blunting peaks much.
        auto power = [](float db) { return std::pow(10.0f, db / 10); };
        for (size_t i = 0; i < spectrum.size(); ++i)
        {
            const float left = spectrum[i > 0 ? i - 1 : i], right = spectrum[std::min(i + 1, spectrum.size() - 1)];
            shown[i] = 10 * std::log10(0.25f * power(left) + 0.5f * power(spectrum[i]) + 0.25f * power(right) + 1e-14f);
            // Tilt, pivoting at 1 kHz: music tends to fall off about this fast,
            // so it then looks roughly level across the plot.
            const float hz = minHz * std::pow(maxHz / minHz, (i + 0.5f) / shown.size());
            shown[i] += float(tilt->value) * std::log2(hz / 1000);
        }
    }
    void pointer(const PointerEvent &event) override { ui.pointer(event); }
    bool key(Key key, unsigned modifiers) override { return ui.key(key, modifiers); }

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float scale) override
    {
        auto *smoothing = params.find("Smoothing");
        auto *colour = params.find("Colour");
        auto *tilt = params.find("Tilt");
        if (!smoothing || !colour || !tilt) return; // until the plugin has sent its settings
        ui.theme.accent = hueColor(float(colour->value)); // the knobs take the spectrum's colour too
        ui.begin(width, height, scale);

        // The layout, in points: the plot fills the height on the left; a side
        // bar on the right holds the knobs.
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
        {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                  .custom = {.customData = ui.layout.custom(plotBox)}}) {}
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(112), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(12),
                             .childGap = 10, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .backgroundColor = gpu::clayColor(ui.theme.panel)})
            {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 12,
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    // 74 points keeps each dial 40 across above its two lines of text.
                    for (gpu::Param *p : {smoothing, colour, tilt})
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(80), CLAY_SIZING_GROW(74, 108)}},
                              .custom = {.customData = ui.layout.custom(knobBox, p)}}) {}
                }
            }
        }

        // Draw what the layout placed: knobs, and the plot.
        ui.resizeGrip(width, height);
        ui.end(pass, [&](int kind, void *data, gpu::Rect r) {
            if (kind == knobBox)
            {
                auto *p = static_cast<gpu::Param *>(data);
                ui.knob(params, p, r, p == tilt ? "%.1f dB/oct" : "%.0f");
            }
            if (kind == plotBox) drawPlot(width, height, r, ui.theme.accent);
        });
    }

private:
    enum { plotBox, knobBox };

    // The plot: the spectrum inside, frequencies below, levels on the left.
    void drawPlot(float width, float height, gpu::Rect box, gpu::Color color)
    {
        const float s = ui.s;
        const float plot[4] = {box.x + 34 * s, box.y + 10 * s, box.x + box.w - 10 * s, box.y + box.h - 22 * s};
        drawSpectrum(width, height, color, plot);
        auto xOf = [&](float hz) { return plot[0] + (plot[2] - plot[0]) * hzToUnit(hz); };
        // Frequency labels, skipping any that would overlap the previous one.
        float lastRight = -1e9f;
        for (auto [hz, label] : {std::pair{20.0f, "20"}, {50.0f, "50"}, {100.0f, "100"}, {200.0f, "200"}, {500.0f, "500"},
                                 {1000.0f, "1k"}, {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"}, {20000.0f, "20k"}})
        {
            const float half = ui.draw.measure(11 * s, label) / 2;
            const float x = std::clamp(xOf(hz), plot[0] + half, plot[2] - half);
            if (x - half < lastRight + 6 * s) continue;
            ui.draw.text(x, box.y + box.h - 7 * s, 11 * s, label, ui.theme.dim, gpu::Draw2D::Align::center);
            lastRight = x + half;
        }
        // Left: the spectrum's level in dBFS, linear, 0 at the top and -100 at
        // the bottom (the shader's levelY). Labels too close together are skipped.
        auto levelY = [&](float dbfs) { return plot[1] + (plot[3] - plot[1]) * std::clamp(dbfs / -100.0f, 0.0f, 1.0f); };
        float lastY = -1e9f;
        for (float dbfs = 0; dbfs >= -100; dbfs -= 10)
        {
            const float y = std::clamp(levelY(dbfs), plot[1] + 9 * s, plot[3] - 2 * s);
            if (y - lastY < 14 * s) continue;
            char label[8];
            std::snprintf(label, sizeof(label), "%.0f", dbfs);
            ui.draw.text(plot[0] - 6 * s, y + 4 * s, 11 * s, label, ui.theme.dim, gpu::Draw2D::Align::right);
            lastY = y;
        }
    }

    // The spectrum: one full-screen triangle through the shader above.
    void createPipeline(wgpu::TextureFormat format)
    {
        wgpu::ShaderSourceWGSL source;
        source.code = shader;
        wgpu::ShaderModuleDescriptor moduleDescriptor;
        moduleDescriptor.nextInChain = &source;
        wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);
        wgpu::ColorTargetState target;
        target.format = format;
        wgpu::FragmentState fragment;
        fragment.module = module;
        fragment.targetCount = 1;
        fragment.targets = &target;
        wgpu::RenderPipelineDescriptor pipelineDescriptor;
        pipelineDescriptor.vertex.module = module;
        pipelineDescriptor.fragment = &fragment;
        pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

        wgpu::BufferDescriptor bufferDescriptor;
        bufferDescriptor.size = sizeof(Uniforms);
        bufferDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniforms = device.CreateBuffer(&bufferDescriptor);
        bufferDescriptor.size = sizeof(float) * spectrum.size();
        bufferDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        spectrumBuffer = device.CreateBuffer(&bufferDescriptor);

        wgpu::BindGroupEntry entries[2];
        entries[0].binding = 0;
        entries[0].buffer = uniforms;
        entries[1].binding = 1;
        entries[1].buffer = spectrumBuffer;
        wgpu::BindGroupDescriptor groupDescriptor;
        groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
        groupDescriptor.entryCount = 2;
        groupDescriptor.entries = entries;
        bindGroup = device.CreateBindGroup(&groupDescriptor);
    }

    // In order with the labels and knobs: Draw2D runs it during its flush.
    void drawSpectrum(float width, float height, gpu::Color color, const float plot[4])
    {
        const Uniforms data{{width, height, ui.s, 0},
                            {color.r, color.g, color.b, 1},
                            {minHz, maxHz, 0, 0},
                            {plot[0], plot[1], plot[2], plot[3]}};
        device.GetQueue().WriteBuffer(uniforms, 0, &data, sizeof(data));
        device.GetQueue().WriteBuffer(spectrumBuffer, 0, shown.data(), sizeof(float) * shown.size());
        ui.draw.custom([this](const wgpu::RenderPassEncoder &pass) {
            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, bindGroup);
            pass.Draw(3);
        });
    }

    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms;
    wgpu::BindGroup bindGroup;
    wgpu::Buffer spectrumBuffer;
    std::vector<float> spectrum = std::vector<float>(512, -120.0f); // dB per band, smoothed over time
    std::vector<float> shown = std::vector<float>(512, -120.0f);    // and across neighbours: what's drawn
    gpu::Params params; // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Ui ui;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<AnalyzerView>(); }

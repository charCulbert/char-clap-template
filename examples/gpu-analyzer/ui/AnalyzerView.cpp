// GPU Analyzer's interface: the live spectrum, drawn by one fragment shader,
// with labels and two knobs (Smoothing, Colour) from draw2d's gpu::Ui on top,
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
fn spectrumDb(x: f32) -> f32 {
    // Interpolated between bands, but never below the loudest band this pixel
    // column covers, so narrow peaks survive when bands outnumber pixels.
    let n = arrayLength(&spectrum);
    let t = bandAt(x);
    let i = u32(t);
    var db = mix(spectrum[i], spectrum[min(i + 1u, n - 1u)], fract(t));
    for (var k = u32(ceil(bandAt(x - 0.5))); f32(k) <= bandAt(x + 0.5) && k < n; k++) {
        db = max(db, spectrum[k]);
    }
    return db;
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
fn warp(hz: f32) -> f32 { return hz / (1500.0 + hz); }
fn xOf(f: f32) -> f32 {
    let r = plotRect();
    return r.x + (r.z - r.x) * (warp(f) - warp(u.ranges.x)) / (warp(u.ranges.y) - warp(u.ranges.x));
}
// The spectrum's own vertical scale, Signalsmith's: 0 dBFS at the top, quiet
// levels compressed towards the bottom (energy^0.12, then a soft curve).
fn levelY(dbfs: f32) -> f32 {
    let r = plotRect();
    let v = pow(10.0, 0.012 * dbfs); // (10^(dB/10))^0.12
    return r.w - (r.w - r.y) * 0.6 * v / (0.4 + 0.2 * v);
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
    let gridHz = array<f32, 8>(50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0);
    for (var i = 0; i < 8; i++) {
        grid = max(grid, 1.0 - smoothstep(0.0, s, abs(px.x - xOf(gridHz[i]))));
    }
    // Horizontal lines on the spectrum's level scale (dBFS, labelled on the left).
    let gridDb = array<f32, 7>(0.0, -12.0, -24.0, -36.0, -48.0, -60.0, -80.0);
    for (var i = 0; i < 7; i++) {
        grid = max(grid, 1.0 - smoothstep(0.0, s, abs(px.y - levelY(gridDb[i]))));
    }
    color += vec3f(0.06, 0.07, 0.08) * grid * inside;

    // The spectrum: a fill below it, brighter near the line, and the line itself.
    let ys = levelY(spectrumDb(px.x));
    let slope = (levelY(spectrumDb(px.x + 1.0)) - levelY(spectrumDb(px.x - 1.0))) * 0.5;
    let distance = abs(px.y - ys) / sqrt(1.0 + slope * slope);
    let below = smoothstep(ys - 0.5, ys + 0.5, px.y) * inside;
    color += u.color.rgb * below * (0.08 + 0.22 * exp(-(px.y - ys) / (40.0 * s)));
    color += u.color.rgb * (1.0 - smoothstep(0.5 * s, 1.5 * s, distance)) * inside;

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

constexpr float minHz = 20, maxHz = 20000;

// The plot's frequency scale (the shader's warp/xOf): Signalsmith's
// hz / (1500 + hz), as a 0..1 position across the plot.
float hzToUnit(float hz)
{
    auto warp = [](float f) { return f / (1500 + f); };
    return (warp(hz) - warp(minHz)) / (warp(maxHz) - warp(minHz));
}

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
        send(core::Value::Map{{"type", "ready"}});
    }

    void receive(const core::Value &message) override
    {
        if (params.receive(message) || message["type"].text() != "spectrum") return;
        auto *db = std::get_if<core::Value::Array>(&message["db"].data);
        const gpu::Param *smoothing = params.find("Smoothing");
        if (!db || !smoothing) return;
        // Levels jump up at once and fall back at a rate Smoothing sets
        // (messages arrive about 60 times a second).
        const float fall = 1 - 0.97f * float(smoothing->value) / 100;
        for (size_t i = 0; i < spectrum.size() && i < db->size(); ++i)
        {
            const float level = float((*db)[i].number(-120));
            spectrum[i] = level > spectrum[i] ? level : spectrum[i] + (level - spectrum[i]) * fall;
        }
    }
    void pointer(const PointerEvent &event) override { ui.pointer(event); }
    bool key(Key key, unsigned modifiers) override { return ui.key(key, modifiers); }

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float scale) override
    {
        auto *smoothing = params.find("Smoothing");
        auto *colour = params.find("Colour");
        if (!smoothing || !colour) return; // until the plugin has sent its settings
        ui.theme.accent = hueColor(float(colour->value)); // the knobs take the spectrum's colour too
        ui.begin(width, height, scale);

        // The layout, in points: the plot fills the height on the left; a side
        // bar on the right holds the knobs.
        layout.begin(ui.draw, width, height, scale);
        CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}}})
        {
            CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                  .custom = {.customData = layout.custom(plotBox)}}) {}
            CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(112), CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(12),
                             .childGap = 10, .childAlignment = {.x = CLAY_ALIGN_X_CENTER}, .layoutDirection = CLAY_TOP_TO_BOTTOM},
                  .backgroundColor = gpu::clayColor(ui.theme.panel)})
            {
                CLAY({.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}, .childGap = 12,
                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    for (gpu::Param *p : {smoothing, colour})
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(80), CLAY_SIZING_GROW(0, 108)}},
                              .custom = {.customData = layout.custom(knobBox, p)}}) {}
                }
            }
        }

        // Draw what the layout placed: knobs, and the plot.
        gpu::Rect plot{};
        layout.end([&](int kind, void *data, gpu::Rect r) {
            if (kind == knobBox) ui.knob(params, static_cast<gpu::Param *>(data), r, "%.0f");
            if (kind == plotBox) plot = r;
        });
        drawPlot(pass, width, height, plot, ui.theme.accent);
        ui.end(pass);
    }

private:
    enum { plotBox, knobBox };

    // The plot: the spectrum inside, frequencies below, levels on the left.
    void drawPlot(const wgpu::RenderPassEncoder &pass, float width, float height, gpu::Rect box, gpu::Color color)
    {
        const float s = ui.s;
        const float plot[4] = {box.x + 34 * s, box.y + 10 * s, box.x + box.w - 10 * s, box.y + box.h - 22 * s};
        drawSpectrum(pass, width, height, color, plot);
        auto xOf = [&](float hz) { return plot[0] + (plot[2] - plot[0]) * hzToUnit(hz); };
        // Frequency labels, skipping any that would overlap the previous one.
        float lastRight = -1e9f;
        for (auto [hz, label] : {std::pair{50.0f, "50"}, {100.0f, "100"}, {200.0f, "200"}, {500.0f, "500"},
                                 {1000.0f, "1k"}, {2000.0f, "2k"}, {5000.0f, "5k"}, {10000.0f, "10k"}})
        {
            const float half = ui.draw.measure(11 * s, label) / 2;
            if (xOf(hz) - half < lastRight + 6 * s) continue;
            ui.draw.text(xOf(hz), box.y + box.h - 7 * s, 11 * s, label, ui.theme.dim, gpu::Draw2D::Align::center);
            lastRight = xOf(hz) + half;
        }
        // Left: the spectrum's level in dBFS, on Signalsmith's scale (the
        // shader's levelY). Labels that would crowd the one above are skipped.
        auto levelY = [&](float dbfs) {
            const float v = std::pow(10.0f, 0.012f * dbfs);
            return plot[3] - (plot[3] - plot[1]) * 0.6f * v / (0.4f + 0.2f * v);
        };
        float lastY = -1e9f;
        for (float dbfs : {0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -80.0f})
        {
            const float y = std::max(levelY(dbfs), plot[1] + 9 * s);
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

    void drawSpectrum(const wgpu::RenderPassEncoder &pass, float width, float height, gpu::Color color, const float plot[4])
    {
        const Uniforms data{{width, height, ui.s, 0},
                            {color.r, color.g, color.b, 1},
                            {minHz, maxHz, 0, 0},
                            {plot[0], plot[1], plot[2], plot[3]}};
        device.GetQueue().WriteBuffer(uniforms, 0, &data, sizeof(data));
        device.GetQueue().WriteBuffer(spectrumBuffer, 0, spectrum.data(), sizeof(float) * spectrum.size());
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
    }

    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms;
    wgpu::BindGroup bindGroup;
    wgpu::Buffer spectrumBuffer;
    std::vector<float> spectrum = std::vector<float>(256, -120.0f); // dB per band, as shown
    gpu::Params params; // the plugin's settings (not CLAP parameters; see plugin.cpp)
    gpu::Ui ui;
    gpu::Layout layout;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<AnalyzerView>(); }

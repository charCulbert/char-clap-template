// simpleCharClap's GPU interface: the filter's frequency response, drawn by one
// fragment shader, with labels and knobs from gpu::Ui on top. Drag the curve
// (left/right: Cutoff, up/down: the resonant peak, i.e. Q) or the knobs.
#include "../gpu/ui.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>

namespace {

// One full-screen triangle; everything is computed per pixel.
constexpr const char *shader = R"(
struct Uniforms {
    size: vec4f,    // width, height (pixels), scale, time (seconds)
    svf: vec4f,     // cutoff Hz, Q, dragging (0/1), sample rate
    ranges: vec4f,  // lowest Hz, highest Hz, lowest dB, highest dB
    plot: vec4f,    // plot area: left, top, right, bottom (pixels)
};
@group(0) @binding(0) var<uniform> u: Uniforms;
// The response in dB at each pixel column, computed on the CPU once per frame.
@group(0) @binding(1) var<storage, read> curve: array<f32>;
fn curveDb(x: f32) -> f32 {
    let i = clamp(i32(x), 0, i32(arrayLength(&curve)) - 1);
    return curve[i];
}
// The live spectrum from the plugin: bands in dB, log-spaced over the plot.
@group(0) @binding(2) var<storage, read> spectrum: array<f32>;
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

// Plot coordinates, inside the plot area the C++ side also uses for text and
// the pointer. y is dB. x is Signalsmith's frequency scale, hz / (1500 + hz):
// nearly linear in the lows and nearly logarithmic above, like hearing.
fn plotRect() -> vec4f { return u.plot; }
fn warp(hz: f32) -> f32 { return hz / (1500.0 + hz); }
fn freqAt(x: f32) -> f32 {
    let r = plotRect();
    let t = clamp((x - r.x) / (r.z - r.x), 0.0, 1.0);
    let w = mix(warp(u.ranges.x), warp(u.ranges.y), t);
    return 1500.0 * w / (1.0 - w);
}
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
fn yOf(db: f32) -> f32 {
    let r = plotRect();
    let t = clamp((db - u.ranges.z) / (u.ranges.w - u.ranges.z), -0.2, 1.2);
    return r.w - (r.w - r.y) * t;
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

    // The spectrum, behind the curve: a soft fill below it and a faint edge.
    let ys = levelY(spectrumDb(px.x));
    let spectrumSlope = (levelY(spectrumDb(px.x + 1.0)) - levelY(spectrumDb(px.x - 1.0))) * 0.5;
    let spectrumDistance = abs(px.y - ys) / sqrt(1.0 + spectrumSlope * spectrumSlope);
    color += vec3f(0.30, 0.42, 0.52) * 0.30 * smoothstep(ys - 0.5, ys + 0.5, px.y) * inside;
    color += vec3f(0.45, 0.60, 0.72) * 0.5 * (1.0 - smoothstep(0.25 * s, 1.25 * s, spectrumDistance)) * inside;

    // The curve: distance to it from this pixel, using the local slope.
    let y0 = yOf(curveDb(px.x));
    let slope = (yOf(curveDb(px.x + 1.0)) - yOf(curveDb(px.x - 1.0))) * 0.5;
    let dist = abs(px.y - y0) / sqrt(1.0 + slope * slope);
    let accent = vec3f(0.37, 0.71, 0.64);

    // Filled area under the curve, fading downwards.
    let below = step(y0, px.y) * inside;
    color += accent * 0.16 * below * (1.0 - (px.y - y0) / (r.w - r.y));

    // Line: sharp core plus a soft glow that breathes a little while dragging.
    let dragging = u.svf.z;
    let glowWidth = (6.0 + 3.0 * dragging * (0.5 + 0.5 * sin(u.size.w * 6.0))) * s;
    color += accent * (1.0 - smoothstep(0.75 * s, 1.75 * s, dist)) * inside;
    color += accent * 0.35 * exp(-dist * dist / (glowWidth * glowWidth)) * inside;

    // Handle on the curve at the cutoff; its height is the resonant peak.
    let handle = vec2f(xOf(u.svf.x), yOf(20.0 * log(max(u.svf.y, 1e-3)) / log(10.0)));
    let hd = length(px - handle);
    let radius = (7.0 + 2.0 * dragging) * s;
    color = mix(color, vec3f(0.95), 1.0 - smoothstep(radius - s, radius, hd));
    color = mix(color, accent, 1.0 - smoothstep(radius - 3.0 * s, radius - 2.0 * s, hd));
    color += accent * 0.4 * exp(-hd * hd / (radius * radius * 6.0));

    return vec4f(color, 1.0);
}
)";

struct Uniforms
{
    float size[4];
    float filter[4];
    float ranges[4];
    float plot[4];
};

constexpr float minHz = 20, maxHz = 20000, minDb = -48, maxDb = 24;

// The plot's frequency scale (the shader's warp/xOf/freqAt): Signalsmith's
// hz / (1500 + hz), as a 0..1 position across the plot and back.
float hzToUnit(float hz)
{
    auto warp = [](float f) { return f / (1500 + f); };
    return (warp(hz) - warp(minHz)) / (warp(maxHz) - warp(minHz));
}
float unitToHz(float t)
{
    const float lo = minHz / (1500 + minHz), hi = maxHz / (1500 + maxHz);
    const float w = lo + t * (hi - lo);
    return 1500 * w / (1 - w);
}

// The plugin's TPT state-variable lowpass is a bilinear-transformed analog
// filter, so this is its exact digital magnitude response in dB.
float responseDb(float hz, float cutoff, float q, float sampleRate)
{
    const float pi = 3.14159265f;
    const float fc = std::clamp(cutoff, 1.0f, sampleRate * 0.48f);
    const float w = std::tan(pi * std::min(hz, sampleRate * 0.499f) / sampleRate) / std::tan(pi * fc / sampleRate);
    const float k = 1 / std::max(q, 1e-3f);
    return -10 * std::log10((1 - w * w) * (1 - w * w) + (w * k) * (w * k));
}

class FilterView : public gpu::View
{
public:
    void prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format) override
    {
        device = newDevice;
        createCurvePipeline(format);
        ui.draw.prepare(device, format, gpu::loadAsset("/fonts/Inter.ttf"));
        params.send = send;
        ui.lockPointer = lockPointer;
        send(webview::Value::Map{{"type", "ready"}});
    }

    void receive(const webview::Value &message) override
    {
        if (params.receive(message) || message["type"].text() != "spectrum") return;
        if (auto db = std::get_if<webview::Value::Array>(&message["db"].data))
            for (size_t i = 0; i < spectrum.size() && i < db->size(); ++i)
                spectrum[i] = float((*db)[i].number(-120));
    }
    void pointer(const PointerEvent &event) override { ui.pointer(event); }
    bool key(Key key, unsigned modifiers) override { return ui.key(key, modifiers); }
    bool animating() const override { return dragging; } // the glow breathes while dragging

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float scale) override
    {
        auto *cutoff = params.find("Cutoff");
        auto *q = params.find("Q");
        if (!cutoff || !q) return; // until the plugin has sent its parameters
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
                    for (gpu::Param *p : {cutoff, q})
                        CLAY({.layout = {.sizing = {CLAY_SIZING_FIXED(80), CLAY_SIZING_GROW(0, 108)}},
                              .custom = {.customData = layout.custom(knobBox, p)}}) {}
                }
            }
        }

        // Draw what the layout placed: knobs, and the plot (curve, axes, drag).
        gpu::Rect plot{};
        layout.end([&](int kind, void *data, gpu::Rect r) {
            if (kind == knobBox)
            {
                auto *p = static_cast<gpu::Param *>(data);
                ui.knob(params, p, r, p == cutoff ? "%.0f Hz" : "%.2f", p == cutoff);
            }
            if (kind == plotBox) plot = r;
        });
        drawPlot(pass, width, height, plot, *cutoff, *q);
        ui.end(pass);
    }

private:
    enum { plotBox, knobBox };

    // The plot: axis labels on its left and bottom edges, the curve inside.
    // Dragging on it moves the peak (x: Cutoff, y: dB = 20 log10 Q), the
    // wheel changes Q, and a double-click resets both.
    void drawPlot(const wgpu::RenderPassEncoder &pass, float width, float height, gpu::Rect box, gpu::Param &cutoff, gpu::Param &q)
    {
        const float s = ui.s;
        const float plot[4] = {box.x + 34 * s, box.y + 10 * s, box.x + box.w - 10 * s, box.y + box.h - 22 * s};
        auto area = ui.area(this, plot[0], plot[1], plot[2] - plot[0], plot[3] - plot[1]);
        if (area.pressed) params.begin(cutoff), params.begin(q);
        if (area.held)
        {
            const float tx = std::clamp((area.x - plot[0]) / (plot[2] - plot[0]), 0.0f, 1.0f);
            const float ty = std::clamp((plot[3] - area.y) / (plot[3] - plot[1]), 0.0f, 1.0f);
            params.change(cutoff, unitToHz(tx));
            params.change(q, std::pow(10.0f, (minDb + ty * (maxDb - minDb)) / 20.0f));
        }
        if (area.released) params.end(cutoff), params.end(q);
        if (area.doubleClicked)
            for (gpu::Param *p : {&cutoff, &q})
                params.begin(*p), params.change(*p, p->defaultValue), params.end(*p);
        if (area.wheel != 0)
        {
            const float db = 20 * std::log10(float(q.value)) + area.wheel * (area.modifiers & shift ? 0.1f : 1.0f);
            params.begin(q), params.change(q, std::pow(10.0f, db / 20)), params.end(q);
        }
        dragging = area.held;

        drawCurve(pass, width, height, float(cutoff.value), float(q.value), plot);
        auto xOf = [&](float hz) { return plot[0] + (plot[2] - plot[0]) * hzToUnit(hz); };
        auto yOf = [&](float db) { return plot[3] - (plot[3] - plot[1]) * (db - minDb) / (maxDb - minDb); };
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
        // Right edge: the filter curve's gain, small ticks for +12, 0 and -12 dB.
        for (float gain : {12.0f, 0.0f, -12.0f})
        {
            char label[12];
            std::snprintf(label, sizeof(label), gain > 0 ? "+%.0f dB" : "%.0f dB", gain);
            ui.draw.text(plot[2] - 4 * s, yOf(gain) - 4 * s, 10 * s, label, ui.theme.accent, gpu::Draw2D::Align::right);
        }
    }

    // The curve: one full-screen triangle through the shader above.
    void createCurvePipeline(wgpu::TextureFormat format)
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
    }

    // One float per pixel column; the bind group follows the buffer's size.
    void resizeCurve(size_t columns)
    {
        if (curveBuffer && curveColumns == columns) return;
        curveColumns = columns;
        wgpu::BufferDescriptor bufferDescriptor;
        bufferDescriptor.size = columns * sizeof(float);
        bufferDescriptor.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
        curveBuffer = device.CreateBuffer(&bufferDescriptor);
        wgpu::BindGroupEntry entries[3];
        entries[0].binding = 0;
        entries[0].buffer = uniforms;
        entries[1].binding = 1;
        entries[1].buffer = curveBuffer;
        entries[2].binding = 2;
        entries[2].buffer = spectrumBuffer;
        wgpu::BindGroupDescriptor groupDescriptor;
        groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
        groupDescriptor.entryCount = 3;
        groupDescriptor.entries = entries;
        bindGroup = device.CreateBindGroup(&groupDescriptor);
    }

    void drawCurve(const wgpu::RenderPassEncoder &pass, float width, float height, float cutoff, float q, const float plot[4])
    {
        frames += 1;
        const Uniforms data{{width, height, ui.s, float(frames) / 60.0f},
                            {cutoff, q, dragging ? 1.0f : 0.0f, 48000.0f}, // the UI assumes 48 kHz for drawing
                            {minHz, maxHz, minDb, maxDb},
                            {plot[0], plot[1], plot[2], plot[3]}};
        device.GetQueue().WriteBuffer(uniforms, 0, &data, sizeof(data));
        resizeCurve(size_t(width) + 1);
        columns.resize(curveColumns);
        for (size_t x = 0; x < columns.size(); ++x)
        {
            const float t = std::clamp((float(x) - plot[0]) / (plot[2] - plot[0]), 0.0f, 1.0f);
            columns[x] = responseDb(unitToHz(t), cutoff, q, 48000.0f);
        }
        device.GetQueue().WriteBuffer(curveBuffer, 0, columns.data(), columns.size() * sizeof(float));
        device.GetQueue().WriteBuffer(spectrumBuffer, 0, spectrum.data(), sizeof(float) * spectrum.size());
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
    }

    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms;
    wgpu::BindGroup bindGroup;
    wgpu::Buffer curveBuffer, spectrumBuffer;
    std::vector<float> spectrum = std::vector<float>(256, -120.0f); // dB per band, from the plugin
    size_t curveColumns = 0;
    std::vector<float> columns;
    gpu::Params params;
    gpu::Ui ui;
    gpu::Layout layout;
    unsigned frames = 0;
    bool dragging = false;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<FilterView>(); }

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

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

const pi = 3.14159265;

// Plot coordinates: x is log frequency, y is dB, inside the plot area the C++
// side also uses for text and the pointer.
fn plotRect() -> vec4f { return u.plot; }
fn freqAt(x: f32) -> f32 {
    let r = plotRect();
    let t = clamp((x - r.x) / (r.z - r.x), 0.0, 1.0);
    return u.ranges.x * pow(u.ranges.y / u.ranges.x, t);
}
fn xOf(f: f32) -> f32 {
    let r = plotRect();
    return r.x + (r.z - r.x) * log(f / u.ranges.x) / log(u.ranges.y / u.ranges.x);
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

    // Grid: decades (100 Hz, 1 kHz, 10 kHz) and every 12 dB.
    var grid = 0.0;
    for (var d = 100.0; d < u.ranges.y; d = d * 10.0) {
        grid = max(grid, 1.0 - smoothstep(0.0, s, abs(px.x - xOf(d))));
    }
    for (var db = -48.0; db <= 24.0; db = db + 12.0) {
        let line = 1.0 - smoothstep(0.0, s, abs(px.y - yOf(db)));
        grid = max(grid, line * select(0.6, 1.0, db == 0.0));
    }
    color += vec3f(0.06, 0.07, 0.08) * grid * inside;

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
        send(webview::Value::Map{{"type", "ready"}});
    }

    void receive(const webview::Value &message) override { params.receive(message); }
    void pointer(Pointer type, float x, float y) override { ui.pointer(type, x, y); }
    bool animating() const override { return dragging; } // the glow breathes while dragging

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float scale) override
    {
        auto *cutoff = params.find("Cutoff");
        auto *q = params.find("Q");
        if (!cutoff || !q) return; // until the plugin has sent its parameters
        ui.begin(width, height, scale);
        const float s = scale;

        // Layout, in pixels: header, curve on the left, knobs on the right.
        const float plot[4] = {40 * s, 48 * s, width - 112 * s, height - 26 * s};

        // Dragging on the curve moves the peak: x is Cutoff, y the peak's height (dB = 20 log10 Q).
        auto area = ui.area(this, plot[0], plot[1], plot[2] - plot[0], plot[3] - plot[1]);
        if (area.pressed) params.begin(*cutoff), params.begin(*q);
        if (area.held)
        {
            const float tx = std::clamp((area.x - plot[0]) / (plot[2] - plot[0]), 0.0f, 1.0f);
            const float ty = std::clamp((plot[3] - area.y) / (plot[3] - plot[1]), 0.0f, 1.0f);
            params.change(*cutoff, minHz * std::pow(maxHz / minHz, tx));
            params.change(*q, std::pow(10.0f, (minDb + ty * (maxDb - minDb)) / 20.0f));
        }
        if (area.released) params.end(*cutoff), params.end(*q);
        dragging = area.held;

        drawCurve(pass, width, height, float(cutoff->value), float(q->value), plot);

        // Header, axis labels, and the two knobs.
        ui.draw.circle(20 * s, 24 * s, 5 * s, ui.theme.accent);
        const float title = ui.draw.text(32 * s, 29 * s, 15 * s, "simpleCharClap", ui.theme.text);
        ui.draw.text(32 * s + title + 10 * s, 29 * s, 11 * s, "SVF LOWPASS", ui.theme.dim);
        auto xOf = [&](float hz) { return plot[0] + (plot[2] - plot[0]) * std::log(hz / minHz) / std::log(maxHz / minHz); };
        auto yOf = [&](float db) { return plot[3] - (plot[3] - plot[1]) * (db - minDb) / (maxDb - minDb); };
        for (auto [hz, text] : {std::pair{100.0f, "100"}, {1000.0f, "1k"}, {10000.0f, "10k"}})
            ui.draw.text(xOf(hz), height - 9 * s, 11 * s, text, ui.theme.dim, gpu::Draw2D::Align::center);
        for (auto [db, text] : {std::pair{24.0f, "+24"}, {12.0f, "+12"}, {0.0f, "0"}, {-12.0f, "-12"}, {-24.0f, "-24"}, {-36.0f, "-36"}})
            ui.draw.text(plot[0] - 8 * s, yOf(db) + 4 * s, 11 * s, text, ui.theme.dim, gpu::Draw2D::Align::right);
        const float column = width / s - 96;
        ui.knob(params, cutoff, column + 10, 52, 64, "%.0f Hz", true);
        ui.knob(params, q, column + 10, 162, 64, "%.2f");
        ui.end(pass);
    }

private:
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
        wgpu::BindGroupEntry entries[2];
        entries[0].binding = 0;
        entries[0].buffer = uniforms;
        entries[1].binding = 1;
        entries[1].buffer = curveBuffer;
        wgpu::BindGroupDescriptor groupDescriptor;
        groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
        groupDescriptor.entryCount = 2;
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
            columns[x] = responseDb(minHz * std::pow(maxHz / minHz, t), cutoff, q, 48000.0f);
        }
        device.GetQueue().WriteBuffer(curveBuffer, 0, columns.data(), columns.size() * sizeof(float));
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
    }

    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms;
    wgpu::BindGroup bindGroup;
    wgpu::Buffer curveBuffer;
    size_t curveColumns = 0;
    std::vector<float> columns;
    gpu::Params params;
    gpu::Ui ui;
    unsigned frames = 0;
    bool dragging = false;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<FilterView>(); }

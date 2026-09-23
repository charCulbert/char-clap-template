// simpleCharClap's GPU interface: the filter's frequency response, drawn by one
// fragment shader, with labels and readouts from gpu::Draw2D on top. Drag
// anywhere: left/right sets Cutoff, up/down sets the resonant peak (Q). It
// speaks the same messages as webUI/app.js.
#include "../gpu/draw2d.h"
#include "../gpu/view.h"
#include <algorithm>
#include <cmath>
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

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

const pi = 3.14159265;

// The plugin's TPT state-variable lowpass is a bilinear-transformed analog
// filter, so this is its exact digital magnitude response in dB.
fn responseDb(f: f32) -> f32 {
    let fs = u.svf.w;
    let fc = clamp(u.svf.x, 1.0, fs * 0.48);
    let w = tan(pi * min(f, fs * 0.499) / fs) / tan(pi * fc / fs);
    let q = max(u.svf.y, 1e-3);
    let m = (1.0 - w * w) * (1.0 - w * w) + (w / q) * (w / q);
    return -10.0 * log(m) / log(10.0);
}

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
    let y0 = yOf(responseDb(freqAt(px.x)));
    let slope = (yOf(responseDb(freqAt(px.x + 1.0))) - yOf(responseDb(freqAt(px.x - 1.0)))) * 0.5;
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

// Formats a frequency the way the axis and readouts show it: 950 Hz, 1.20 kHz.
std::string formatHz(float hz)
{
    char text[32];
    if (hz >= 1000) std::snprintf(text, sizeof(text), "%.2f kHz", hz / 1000);
    else std::snprintf(text, sizeof(text), "%.0f Hz", hz);
    return text;
}

class FilterView : public gpu::View
{
public:
    void prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format) override
    {
        device = newDevice;
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

        wgpu::BindGroupEntry entry;
        entry.binding = 0;
        entry.buffer = uniforms;
        wgpu::BindGroupDescriptor groupDescriptor;
        groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
        groupDescriptor.entryCount = 1;
        groupDescriptor.entries = &entry;
        bindGroup = device.CreateBindGroup(&groupDescriptor);

        hasText = draw2d.prepare(device, format, gpu::loadAsset("/fonts/Inter.ttf"));
        send(webview::Value::Map{{"type", "ready"}});
    }

    void draw(const wgpu::RenderPassEncoder &pass, float width, float height, float newScale) override
    {
        size[0] = width, size[1] = height, scale = newScale;
        frames += 1;
        const float s = scale;
        // Header above the plot, axis labels below and to the left of it.
        plot[0] = 44 * s, plot[1] = 52 * s, plot[2] = width - 16 * s, plot[3] = height - 30 * s;
        const Uniforms data{{width, height, scale, float(frames) / 60.0f},
                            {cutoff, q, dragging ? 1.0f : 0.0f, sampleRate},
                            {minHz, maxHz, minDb, maxDb},
                            {plot[0], plot[1], plot[2], plot[3]}};
        device.GetQueue().WriteBuffer(uniforms, 0, &data, sizeof(data));
        pass.SetPipeline(pipeline);
        pass.SetBindGroup(0, bindGroup);
        pass.Draw(3);
        if (hasText) drawText(pass, width, height);
    }

    // Everything with words in it, drawn over the curve.
    void drawText(const wgpu::RenderPassEncoder &pass, float width, float height)
    {
        const float s = scale;
        const gpu::Color text{0.90f, 0.93f, 0.95f}, dim{0.55f, 0.61f, 0.66f}, accent{0.37f, 0.71f, 0.64f};
        const gpu::Color panel{0.11f, 0.13f, 0.15f, 0.92f}, edge{0.25f, 0.29f, 0.32f};
        draw2d.begin(width, height);

        // Header: name, then the two values as chips on the right.
        draw2d.circle(24 * s, 26 * s, 6 * s, accent);
        draw2d.text(38 * s, 31 * s, 15 * s, "simpleCharClap", text);
        draw2d.text(40 * s + draw2d.measure(15 * s, "simpleCharClap"), 31 * s, 11 * s, "  SVF LOWPASS", dim);
        char qText[32];
        std::snprintf(qText, sizeof(qText), "%.2f", q);
        float right = width - 16 * s;
        for (const auto &[label, value] : {std::pair<const char *, std::string>{"Q", qText}, {"CUTOFF", formatHz(cutoff)}})
        {
            const float w = draw2d.measure(10 * s, label) + draw2d.measure(13 * s, value) + 26 * s;
            draw2d.rect(right - w, 12 * s, w, 26 * s, 13 * s, panel, 1 * s, dragging ? accent : edge);
            draw2d.text(right - w + 10 * s, 29 * s, 10 * s, label, dim);
            draw2d.text(right - 10 * s, 30 * s, 13 * s, value, text, gpu::Draw2D::Align::right);
            right -= w + 8 * s;
        }

        // Axis labels: decades under the plot, every 12 dB to its left.
        auto xOf = [&](float hz) { return plot[0] + (plot[2] - plot[0]) * std::log(hz / minHz) / std::log(maxHz / minHz); };
        auto yOf = [&](float db) { return plot[3] - (plot[3] - plot[1]) * (db - minDb) / (maxDb - minDb); };
        for (float hz : {100.0f, 1000.0f, 10000.0f})
            draw2d.text(xOf(hz), height - 12 * s, 10 * s, hz >= 1000 ? (hz >= 10000 ? "10k" : "1k") : "100", dim,
                        gpu::Draw2D::Align::center);
        for (float db = -36; db <= 24; db += 12)
        {
            char label[8];
            std::snprintf(label, sizeof(label), "%+.0f", db);
            draw2d.text(plot[0] - 8 * s, yOf(db) + 4 * s, 10 * s, db == 0 ? "0" : label, dim, gpu::Draw2D::Align::right);
        }

        // While dragging: a tooltip by the handle with both values.
        if (dragging)
        {
            char tip[48];
            std::snprintf(tip, sizeof(tip), "%s  Q %.2f", formatHz(cutoff).c_str(), q);
            const float w = draw2d.measure(12 * s, tip) + 20 * s;
            const float hx = xOf(cutoff), hy = yOf(20 * std::log10(std::max(q, 1e-3f)));
            const float x = std::clamp(hx - w / 2, plot[0], plot[2] - w);
            const float y = hy - 44 * s < plot[1] ? hy + 18 * s : hy - 44 * s;
            draw2d.rect(x, y, w, 26 * s, 6 * s, panel, 1 * s, accent);
            draw2d.text(x + w / 2, y + 17.5f * s, 12 * s, tip, text, gpu::Draw2D::Align::center);
        }
        draw2d.flush(pass);
    }

    void pointer(Pointer type, float x, float y) override
    {
        if (type == Pointer::down)
        {
            dragging = true;
            send(webview::Value::Map{{"type", "begin"}, {"id", cutoffId}});
            send(webview::Value::Map{{"type", "begin"}, {"id", qId}});
        }
        if (!dragging) return;
        if (type != Pointer::up)
        {
            // The inverse of the shader's xOf/yOf, clamped to the parameters' ranges.
            const float tx = std::clamp((x - plot[0]) / (plot[2] - plot[0]), 0.0f, 1.0f);
            const float ty = std::clamp((plot[3] - y) / (plot[3] - plot[1]), 0.0f, 1.0f);
            cutoff = std::clamp(minHz * std::pow(maxHz / minHz, tx), cutoffMin, cutoffMax);
            q = std::clamp(std::pow(10.0f, (minDb + ty * (maxDb - minDb)) / 20.0f), qMin, qMax);
            send(webview::Value::Map{{"type", "change"}, {"id", cutoffId}, {"value", double(cutoff)}});
            send(webview::Value::Map{{"type", "change"}, {"id", qId}, {"value", double(q)}});
        }
        if (type == Pointer::up)
        {
            dragging = false;
            send(webview::Value::Map{{"type", "end"}, {"id", cutoffId}});
            send(webview::Value::Map{{"type", "end"}, {"id", qId}});
        }
    }

    bool animating() const override { return dragging; } // the glow breathes while dragging

    void receive(const webview::Value &message) override
    {
        const auto type = message["type"].text();
        if (type == "params")
        {
            if (auto params = std::get_if<webview::Value::Array>(&message["params"].data))
                for (const auto &p : *params)
                {
                    if (p["name"].text() == "Cutoff")
                        cutoffId = int(p["id"].number()), cutoffMin = float(p["min"].number()), cutoffMax = float(p["max"].number());
                    if (p["name"].text() == "Q")
                        qId = int(p["id"].number()), qMin = float(p["min"].number()), qMax = float(p["max"].number());
                }
        }
        else if (type == "values" && !dragging)
        {
            if (auto values = std::get_if<webview::Value::Array>(&message["values"].data))
            {
                if (size_t(cutoffId) < values->size()) cutoff = float((*values)[cutoffId].number(cutoff));
                if (size_t(qId) < values->size()) q = float((*values)[qId].number(q));
            }
        }
    }

private:
    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms;
    wgpu::BindGroup bindGroup;

    gpu::Draw2D draw2d;
    bool hasText = false;
    float size[2] = {1, 1}, scale = 1;
    float plot[4] = {0, 0, 1, 1}; // left, top, right, bottom (pixels)
    unsigned frames = 0;
    bool dragging = false;

    int cutoffId = 0, qId = 1;
    float cutoff = 1000, q = 0.5f;
    float cutoffMin = 20, cutoffMax = 15000, qMin = 0, qMax = 10;
    // Drawing only; the plugin's sample rate is not sent to the UI.
    static constexpr float sampleRate = 48000;
    static constexpr float minHz = 20, maxHz = 20000, minDb = -48, maxDb = 24;
};

}

std::unique_ptr<gpu::View> gpu::createView() { return std::make_unique<FilterView>(); }

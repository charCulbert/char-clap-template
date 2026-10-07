// Draw2D's pixels: each case draws into a 64 x 64 offscreen target, reads it
// back and probes a few pixels, as Visage's canvas tests do: inside a shape,
// outside it, in a rounded corner, across a clip edge. Probes stay clear of
// antialiased edges, so they hold on any GPU, the software ones included.
// Skips (exit code 77) where there's no WebGPU adapter at all.
// Usage: draw2d_test <font.ttf>
#include "../libs/draw2d/draw2d.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

using gpu::Color;
using gpu::Draw2D;

static int failures = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), ++failures; \
    } while (0)

constexpr uint32_t size = 64; // 64 x 4 bytes: rows already 256-byte aligned for the copy
constexpr wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;
constexpr float pi = 3.14159265f;
const Color red{1, 0, 0}, green{0, 1, 0}, blue{0, 0, 1}, white{1, 1, 1};

struct Gpu
{
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Texture target;
    wgpu::Buffer readback;

    bool create()
    {
        static const wgpu::InstanceFeatureName features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
        wgpu::InstanceDescriptor instanceDescriptor;
        instanceDescriptor.requiredFeatureCount = 1;
        instanceDescriptor.requiredFeatures = features;
        instance = wgpu::CreateInstance(&instanceDescriptor);
        for (bool fallback : {false, true})
        {
            wgpu::RequestAdapterOptions options;
            options.forceFallbackAdapter = fallback;
            instance.WaitAny(instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                [](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView, Gpu *self) {
                    if (status == wgpu::RequestAdapterStatus::Success) self->adapter = std::move(result);
                }, this), UINT64_MAX);
            if (adapter) break;
        }
        if (!adapter) return false;
        wgpu::DeviceDescriptor deviceDescriptor;
        deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device &, wgpu::ErrorType, wgpu::StringView message) {
            std::fprintf(stderr, "WebGPU error: %.*s\n", int(message.length), message.data);
            ++failures;
        });
        instance.WaitAny(adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
            [](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView, Gpu *self) {
                if (status == wgpu::RequestDeviceStatus::Success) self->device = std::move(result);
            }, this), UINT64_MAX);
        if (!device) return false;
        wgpu::TextureDescriptor textureDescriptor;
        textureDescriptor.size = {size, size, 1};
        textureDescriptor.format = format;
        textureDescriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
        target = device.CreateTexture(&textureDescriptor);
        wgpu::BufferDescriptor bufferDescriptor;
        bufferDescriptor.size = size * size * 4;
        bufferDescriptor.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
        readback = device.CreateBuffer(&bufferDescriptor);
        return true;
    }

    // Clears to black, draws, and returns the pixels (RGBA, rows top down).
    template <typename F> std::vector<uint8_t> render(Draw2D &draw, F &&drawing)
    {
        draw.begin(size, size);
        drawing();
        wgpu::RenderPassColorAttachment attachment;
        attachment.view = target.CreateView();
        attachment.loadOp = wgpu::LoadOp::Clear;
        attachment.storeOp = wgpu::StoreOp::Store;
        attachment.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor passDescriptor;
        passDescriptor.colorAttachmentCount = 1;
        passDescriptor.colorAttachments = &attachment;
        const wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        const wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);
        draw.flush(pass);
        pass.End();
        wgpu::TexelCopyTextureInfo source;
        source.texture = target;
        wgpu::TexelCopyBufferInfo destination;
        destination.buffer = readback;
        destination.layout.bytesPerRow = size * 4;
        destination.layout.rowsPerImage = size;
        const wgpu::Extent3D extent = {size, size, 1};
        encoder.CopyTextureToBuffer(&source, &destination, &extent);
        const wgpu::CommandBuffer commands = encoder.Finish();
        device.GetQueue().Submit(1, &commands);
        instance.WaitAny(readback.MapAsync(wgpu::MapMode::Read, 0, size * size * 4, wgpu::CallbackMode::WaitAnyOnly,
                                           [](wgpu::MapAsyncStatus, wgpu::StringView) {}), UINT64_MAX);
        const auto *bytes = static_cast<const uint8_t *>(readback.GetConstMappedRange());
        std::vector<uint8_t> pixels(bytes, bytes + size * size * 4);
        readback.Unmap();
        return pixels;
    }
};

// The pixel whose top-left corner is at x, y is within a few levels of c.
static bool is(const std::vector<uint8_t> &pixels, int x, int y, Color c)
{
    const uint8_t *p = &pixels[(size_t(y) * size + size_t(x)) * 4];
    auto near = [](uint8_t value, float expected) { return std::abs(value - expected * 255) <= 6; };
    const bool match = near(p[0], c.r) && near(p[1], c.g) && near(p[2], c.b);
    if (!match) std::fprintf(stderr, "  pixel %d,%d is %d %d %d\n", x, y, p[0], p[1], p[2]);
    return match;
}
const Color black{0, 0, 0};

int main(int argc, char **argv)
{
    Gpu gpu;
    if (!gpu.create())
    {
        std::fprintf(stderr, "no WebGPU adapter: skipped\n");
        return 77;
    }
    std::string font;
    if (argc > 1)
        if (std::ifstream in{argv[1], std::ios::binary}) font.assign(std::istreambuf_iterator<char>(in), {});
    Draw2D draw;
    CHECK(draw.prepare(gpu.device, format, font));

    // A filled rectangle: inside is its colour, outside the background.
    auto px = gpu.render(draw, [&] { draw.rect(8, 8, 16, 16, 0, red); });
    CHECK(is(px, 16, 16, red) && is(px, 9, 9, red) && is(px, 4, 4, black) && is(px, 26, 16, black));

    // A rounded corner leaves its corner empty; a square one doesn't.
    px = gpu.render(draw, [&] { draw.rect(0, 0, 32, 32, Draw2D::Corners{12, 0, 0, 0}, white); });
    CHECK(is(px, 1, 1, black) && is(px, 30, 1, white) && is(px, 16, 16, white));

    // Each side its own border width: bottom 6, top none.
    px = gpu.render(draw, [&] { draw.rect(0, 0, 32, 32, Draw2D::Corners{}, blue, Draw2D::Sides{0, 0, 0, 6}, red); });
    CHECK(is(px, 16, 29, red) && is(px, 16, 1, blue) && is(px, 16, 16, blue));

    // Draw order holds across kinds: a box after a line covers it, a line after a box crosses it.
    px = gpu.render(draw, [&] { draw.line(0, 32, 64, 32, 8, red); draw.rect(16, 16, 32, 32, 0, green); });
    CHECK(is(px, 32, 31, green) && is(px, 4, 31, red));
    px = gpu.render(draw, [&] { draw.rect(16, 16, 32, 32, 0, green); draw.line(0, 32, 64, 32, 8, red); });
    CHECK(is(px, 32, 31, red));

    // A clip cuts what's inside it, and nests.
    px = gpu.render(draw, [&] {
        draw.pushClip(0, 0, 32, 64);
        draw.pushClip(16, 0, 48, 64); // inside the first: 16..32
        draw.rect(0, 0, 64, 64, 0, green);
        draw.popClip();
        draw.popClip();
        draw.rect(40, 40, 8, 8, 0, red); // after the clips: unclipped
    });
    CHECK(is(px, 24, 10, green) && is(px, 8, 10, black) && is(px, 40 + 2, 10, black) && is(px, 44, 44, red));

    // An arc covers its angle range only. Centred on a pixel row's centre,
    // where atan2 once flipped quadrant at -0.
    px = gpu.render(draw, [&] { draw.arc(32, 32.5f, 20, 6, 0, pi, red); });
    CHECK(is(px, 52, 32, red)); // 90 degrees: in range
    CHECK(is(px, 11, 32, black)); // 270 degrees: not

    // A custom draw runs in order, under what follows it.
    int calls = 0;
    px = gpu.render(draw, [&] {
        draw.custom([&](const wgpu::RenderPassEncoder &) { ++calls; });
        draw.rect(0, 0, 8, 8, 0, red);
    });
    CHECK(calls == 1 && is(px, 4, 4, red));

    // Text: ink where the letters are; widths that follow UTF-8 characters,
    // not bytes; a visible box for a character the font lacks.
    if (!font.empty())
    {
        px = gpu.render(draw, [&] { draw.text(4, 40, 32, "H", white); });
        int ink = 0;
        for (size_t i = 0; i < px.size(); i += 4) ink += px[i] > 128;
        CHECK(ink > 40);
        CHECK(draw.measure(20, "\xC3\xA9") > 0 && draw.measure(20, "\xC3\xA9") < draw.measure(20, "ee")); // é is one character, not two
        CHECK(draw.measure(20, "\xE2\x86\x92") > 0); // → (Barlow lacks it): its missing glyph still takes space
    }

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}

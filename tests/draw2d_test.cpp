// Draw2D's pixels: each case draws into a 64 x 64 offscreen target, reads it
// back and probes a few pixels, as Visage's canvas tests do: inside a shape,
// outside it, in a rounded corner, across a clip edge. Probes stay clear of
// antialiased edges, so they hold on any GPU, the software ones included.
// Skips (exit code 77) where there's no WebGPU adapter at all.
// Usage: draw2d_test <font.ttf>
#include "../libs/chardraw/draw2d.h"
#include "../libs/chardraw/views.h"
#include "../libs/gpu/context.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

using gpu::Color;
using gpu::ColourScheme;
using gpu::Draw2D;
using gpu::Spectrogram;
using gpu::Ui;

static int failures = 0;
#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition), ++failures; \
    } while (0)

constexpr uint32_t size = 64; // 64 x 4 bytes: rows already 256-byte aligned for the copy
constexpr wgpu::TextureFormat format = wgpu::TextureFormat::RGBA8Unorm;
constexpr float pi = 3.14159265f;
const Color red{1, 0, 0}, green{0, 1, 0}, blue{0, 0, 1}, white{1, 1, 1};

// The device as the plugins make it (gpu::Context), drawing into a texture
// the test reads back.
struct Gpu
{
    gpu::Context context;
    wgpu::Device device;
    wgpu::Instance instance;
    wgpu::Texture target;
    wgpu::Buffer readback;

    bool create()
    {
        if (!context.createDevice()) return false;
        device = context.device, instance = context.instance;
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

    // Without its font (a plugin's resources not found) it still draws, just
    // no text: images and shapes work, and nothing crashes.
    {
        Draw2D bare;
        CHECK(!bare.prepare(gpu.device, format, ""));
        const uint8_t pixel[] = {255, 255, 255, 255};
        CHECK(bare.addImage(pixel, 1, 1) == 0);
        const auto px = gpu.render(bare, [&] {
            bare.rect(8, 8, 16, 16, 0, red);
            bare.text(4, 40, 12, "Hi", white);
            bare.image(0, 40, 40, 4, 4);
        });
        CHECK(is(px, 16, 16, red) && is(px, 41, 41, white));
    }

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
    // Its ends are antialiased: an end through a pixel's centre covers half of it.
    px = gpu.render(draw, [&] { draw.arc(32, 32.5f, 20, 6, std::asin(1.5f / 20), pi, red); });
    {
        const int value = px[(size_t(12) * size + 33) * 4];
        if (value < 70 || value > 190) std::fprintf(stderr, "  arc end pixel is %d\n", value);
        CHECK(value >= 70 && value <= 190);
    }

    // An image stretches over its rectangle, clamped at its edges, and its
    // corners round like a box's.
    {
        const uint8_t pixels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        const int picture = draw.addImage(pixels, 2, 2);
        CHECK(picture == 0);
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32); });
        CHECK(is(px, 2, 2, red) && is(px, 29, 2, green) && is(px, 2, 29, blue) && is(px, 29, 29, white) && is(px, 40, 40, black));
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32, {16, 0, 0, 0}); });
        CHECK(is(px, 1, 1, black) && is(px, 29, 2, green));
        // A source rectangle draws part of it: the top-left quarter is all red.
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32, {}, {1, 1, 1, 1}, {0, 0, 0.25f, 0.25f}); });
        CHECK(is(px, 2, 2, red) && is(px, 29, 29, red));
        // A tint multiplies each pixel: white becomes the tint, red under a green tint goes black.
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32, {}, {0, 1, 0, 1}); });
        CHECK(is(px, 29, 29, green) && is(px, 2, 2, black) && is(px, 29, 2, green));
        int w = 0, h = 0;
        draw.imageSize(picture, w, h);
        CHECK(w == 2 && h == 2);
        // An update replaces part of it, and the same drawing counts as changed.
        const uint8_t whitePixel[] = {255, 255, 255, 255};
        draw.begin(size, size);
        draw.image(picture, 0, 0, 32, 32);
        draw.changed();
        draw.updateImage(picture, whitePixel, 0, 0, 1, 1);
        draw.begin(size, size);
        draw.image(picture, 0, 0, 32, 32);
        CHECK(draw.changed());
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32); });
        CHECK(is(px, 2, 2, white) && is(px, 29, 2, green));
        draw.updateImage(picture, whitePixel, 2, 0, 1, 1); // outside it: nothing
        px = gpu.render(draw, [&] { draw.image(picture, 0, 0, 32, 32); });
        CHECK(is(px, 29, 2, green));
        CHECK(draw.addImage("not an image") == -1);
        // SVG: rasterized to fit, keeping its proportions.
        const int svg = draw.addSvg("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'>"
                                    "<rect width='10' height='10' fill='#ff0000'/><rect x='10' width='10' height='10' fill='#00ff00'/></svg>", 64, 64);
        draw.imageSize(svg, w, h);
        CHECK(svg >= 0 && w == 64 && h == 32);
        px = gpu.render(draw, [&] { draw.image(svg, 0, 0, 64, 32); });
        CHECK(is(px, 8, 16, red) && is(px, 56, 16, green) && is(px, 32, 48, black));
        CHECK(draw.addSvg("not svg", 32, 32) == -1);
    }

    // A custom draw runs in order, under what follows it.
    int calls = 0;
    px = gpu.render(draw, [&] {
        draw.custom([&](const wgpu::RenderPassEncoder &) { ++calls; });
        draw.rect(0, 0, 8, 8, 0, red);
    });
    CHECK(calls == 1 && is(px, 4, 4, red));

    // A linear gradient runs between its ends; a shadow is solid inside its
    // box, half at the edge and gone three sigmas out.
    px = gpu.render(draw, [&] { draw.rect(0, 0, 64, 64, Draw2D::Corners{}, Draw2D::Gradient{red, blue, 0, 0, 64, 0}); });
    CHECK(is(px, 1, 32, red) && is(px, 62, 32, blue) && is(px, 32, 10, {0.5f, 0, 0.5f}));
    px = gpu.render(draw, [&] { draw.shadow(16, 16, 32, 32, 0, 8, white); });
    {
        const int edge = px[(size_t(32) * size + 16) * 4];
        if (edge < 90 || edge > 165) std::fprintf(stderr, "  shadow edge is %d\n", edge);
        CHECK(is(px, 32, 32, white) && is(px, 1, 32, black) && edge >= 90 && edge <= 165);
    }

    // A concave fill: inside both arms of an L, not in its notch.
    px = gpu.render(draw, [&] {
        // (with a repeated corner and a point in line, which mustn't stop it)
        const float l[] = {8, 8, 32, 8, 56, 8, 56, 24, 24, 24, 24, 56, 8, 56, 8, 56};
        draw.fill(l, 8, red);
    });
    CHECK(is(px, 40, 16, red) && is(px, 16, 40, red) && is(px, 40, 40, black) && is(px, 2, 2, black));
    // A fill keeps to the clip it was drawn in, after a shape outside it.
    px = gpu.render(draw, [&] {
        draw.rect(0, 0, 4, 4, 0, white);
        draw.pushClip(0, 0, 32, 64);
        const float square[] = {8, 8, 56, 8, 56, 56, 8, 56};
        draw.fill(square, 4, red);
        draw.popClip();
    });
    CHECK(is(px, 16, 16, red) && is(px, 40, 16, black));

    // changed(): the same drawing again is unchanged, a different one isn't,
    // and a custom draw is judged by its version.
    {
        auto scene = [&](float x, uint64_t version) {
            draw.begin(size, size);
            draw.rect(x, 8, 16, 16, 4, red);
            draw.custom([](const wgpu::RenderPassEncoder &) {}, version);
        };
        gpu.render(draw, [&] { scene(8, 1); });
        scene(8, 1);
        CHECK(!draw.changed());
        scene(9, 1);
        CHECK(draw.changed());
        scene(8, 2);
        CHECK(draw.changed());
        scene(8, 0);
        CHECK(draw.changed()); // version 0: always
    }

    // A spectrum in colours of its own: a flat curve at -50 dB, halfway up,
    // in the curve's, the fill under it in the fill's, no grid; unset, the
    // curve is the theme's accent.
    {
        Ui ui;
        CHECK(ui.draw.prepare(gpu.device, format, font));
        const float frequencies[] = {20, 20000}, decibels[] = {-50, -50};
        gpu::SpectrumView view{.scales = false, .curveColour = Color{1, 0, 0}, .fillColour = Color{0, 1, 0}, .gridColour = Color{0, 0, 0, 0}};
        auto shot = [&] { return gpu.render(ui.draw, [&] { gpu::spectrum(ui, "Spectrum", decibels, frequencies, 2, {0.5f, 0.5f, float(size), float(size)}, view); }); };
        px = shot();
        const int x = size / 2;
        CHECK(is(px, x, size / 2, {1, 0, 0}) && is(px, x, size / 4, ui.theme.background));
        const uint8_t *under = &px[(size_t(size / 2 + 3) * size + size_t(x)) * 4];
        CHECK(under[1] > 120 && under[0] < 60 && under[2] < 60);
        view = {.scales = false};
        px = shot();
        CHECK(is(px, x, size / 2, ui.theme.accent));
    }

    // A spectrogram: the newest column on the right, low frequencies at the
    // bottom, coloured from its scheme (Magma); a later column goes up on its own.
    {
        Ui ui;
        CHECK(ui.draw.prepare(gpu.device, format, font));
        Spectrogram spectrogram({.minFrequency = 0, .maxFrequency = 1000, .scale = Spectrogram::Scale::linear,
                                 .columns = 4, .rows = 2, .smooth = false}); // a column at a time: pixels to check
        const Color quiet{0, 0, 0.016f}, loud{0.988f, 0.992f, 0.749f}; // magma's ends, #000004 and #fcfdbf
        const float frequencies[] = {250, 750};
        const float lowLoud[] = {0, -100}, highLoud[] = {-100, 0};
        spectrogram.add(lowLoud, frequencies, 2);
        // Half a pixel in: pixel centres fall on its columns' centres, unblended.
        auto shot = [&] { return gpu.render(ui.draw, [&] { spectrogram.draw(ui, "Spectrum", {0.5f, 0.5f, float(size), float(size)}); }); };
        px = shot();
        const int newest = size * 7 / 8, before = size * 5 / 8, top = size / 4, bottom = size * 3 / 4;
        CHECK(is(px, newest, bottom, loud) && is(px, newest, top, quiet) && is(px, size / 8, bottom, quiet));
        spectrogram.add(highLoud, frequencies, 2);
        px = shot();
        CHECK(is(px, newest, top, loud) && is(px, newest, bottom, quiet) && is(px, before, bottom, loud));
        // Another scheme recolours what's kept.
        auto settings = spectrogram.settings();
        settings.colours = ColourScheme::named("Viridis");
        spectrogram.configure(settings);
        px = shot();
        const Color quieter{0.267f, 0.004f, 0.329f}, louder{0.992f, 0.906f, 0.145f}; // viridis's ends, #440154 and #fde725
        CHECK(is(px, newest, top, louder));
        // Another range redraws what's kept: up to 500 Hz, the 250 Hz bin
        // fills the top row, loud in the column before (it was 0 dB there).
        settings.maxFrequency = 500;
        spectrogram.configure(settings);
        px = shot();
        CHECK(is(px, before, top, louder) && is(px, newest, top, quieter));
        // A scheme of its own is blended into the nine, and a change redraws.
        const Color red{1, 0, 0}, blue{0, 0, 1};
        settings.colours = {{red, blue}};
        spectrogram.configure(settings);
        Color ramp[Spectrogram::rampColours];
        spectrogram.ramp(ramp);
        CHECK(ramp[0].r == 1 && ramp[4].r == 0.5f && ramp[4].b == 0.5f && ramp[8].b == 1);
        ColourScheme{{red, blue, red}}.ramp(ramp); // three: the middle one halfway
        CHECK(ramp[4].b == 1 && ramp[2].r == 0.5f && ramp[8].r == 1);
        px = shot();
        CHECK(is(px, before, top, blue) && is(px, newest, top, red));
        settings.colours = {{blue, red}};
        spectrogram.configure(settings);
        px = shot();
        CHECK(is(px, before, top, red) && is(px, newest, top, blue));
    }

    // A spectrogram resized: more columns than it began with take columns
    // written past the old width; toggling sizes reuses a size's texture, so
    // the images in Draw2D stop growing (a probe image's number shows it).
    {
        Ui ui;
        CHECK(ui.draw.prepare(gpu.device, format, font));
        Spectrogram spectrogram({.minFrequency = 0, .maxFrequency = 1000, .scale = Spectrogram::Scale::linear,
                                 .columns = 4, .rows = 2, .smooth = false});
        const Color quiet{0, 0, 0.016f}, loud{0.988f, 0.992f, 0.749f};
        const float frequencies[] = {250, 750};
        const float lowLoud[] = {0, -100}, highLoud[] = {-100, 0};
        auto shot = [&] { return gpu.render(ui.draw, [&] { spectrogram.draw(ui, "Spectrum", {0.5f, 0.5f, float(size), float(size)}); }); };
        spectrogram.add(lowLoud, frequencies, 2);
        px = shot();
        auto settings = spectrogram.settings();
        settings.columns = 8;
        spectrogram.configure(settings);
        for (int i = 0; i < 6; ++i) spectrogram.add(i == 5 ? highLoud : lowLoud, frequencies, 2); // slots 4 and 5 are past the old width
        px = shot();
        const int top = size / 4, bottom = size * 3 / 4;
        CHECK(is(px, size * 15 / 16, top, loud) && is(px, size * 15 / 16, bottom, quiet)); // the newest, slot 5
        CHECK(is(px, size * 13 / 16, bottom, loud) && is(px, size * 13 / 16, top, quiet)); // slot 4
        // Fewer columns again keep the newest, in order: low, low, high and
        // the one added now, low.
        settings.columns = 4;
        spectrogram.configure(settings);
        spectrogram.add(lowLoud, frequencies, 2);
        px = shot();
        CHECK(is(px, size * 7 / 8, bottom, loud) && is(px, size * 5 / 8, top, loud) && is(px, size * 5 / 8, bottom, quiet));
        CHECK(is(px, size / 8, bottom, loud) && is(px, size / 8, top, quiet));
        // Toggling the rows makes one texture per size, not one per toggle.
        const uint8_t one[4] = {};
        const int before = ui.draw.addImage(one, 1, 1);
        for (int i = 0; i < 6; ++i)
        {
            settings.rows = i % 2 ? 2 : 4;
            spectrogram.configure(settings);
            px = shot();
        }
        CHECK(is(px, size * 7 / 8, bottom, loud) && is(px, size * 5 / 8, top, loud)); // still drawing right
        CHECK(ui.draw.addImage(one, 1, 1) == before + 2); // the rows-4 texture was new; the rest were reused
    }

    // Text: ink where the letters are; widths that follow UTF-8 characters,
    // not bytes; a visible box for a character the font lacks.
    if (!font.empty())
    {
        px = gpu.render(draw, [&] { draw.text(4, 40, 32, "H", white); });
        int ink = 0;
        for (size_t i = 0; i < px.size(); i += 4) ink += px[i] > 128;
        CHECK(ink > 40);
        // Small text is rasterized at its size: still ink, and the same width as the field's.
        px = gpu.render(draw, [&] { draw.text(4, 40, 12, "Hello", white); });
        ink = 0;
        for (size_t i = 0; i < px.size(); i += 4) ink += px[i] > 128;
        CHECK(ink > 20);
        CHECK(draw.measure(12, "Hello") * 2 == draw.measure(24, "Hello"));
        CHECK(draw.measure(20, "\xC3\xA9") > 0 && draw.measure(20, "\xC3\xA9") < draw.measure(20, "ee")); // é is one character, not two
        CHECK(draw.measure(20, "\xE2\x86\x92") > 0); // → (Barlow lacks it): its missing glyph still takes space
        // Thousands of different glyphs overflow the atlas; the next frame
        // starts it again and draws its text.
        std::string many;
        for (uint32_t c = 0x4E00; c < 0x4E00 + 8000; ++c)
            many += {char(0xE0 | c >> 12), char(0x80 | (c >> 6 & 0x3F)), char(0x80 | (c & 0x3F))};
        gpu.render(draw, [&] { draw.text(4, 40, 16, many, white); });
        CHECK(draw.atlasRefilling());
        px = gpu.render(draw, [&] { draw.text(4, 40, 13, "Hello", white); }); // a size not baked yet
        ink = 0;
        for (size_t i = 0; i < px.size(); i += 4) ink += px[i] > 128;
        CHECK(ink > 20 && !draw.atlasRefilling());
    }

    // A meter: filled from the bottom to its level, its track above, a line at the peak.
    {
        Ui ui;
        CHECK(ui.draw.prepare(gpu.device, format, font));
        ui.theme.track = blue, ui.theme.accent = green, ui.theme.text = white;
        px = gpu.render(ui.draw, [&] { ui.meter(0.5f, 1 - 16.5f / 64, {0, 0, 64, 64}); });
        CHECK(is(px, 32, 48, green) && is(px, 32, 24, blue) && is(px, 32, 16, white) && is(px, 32, 8, blue));
    }

    // A lost device: prepared again on a new one, shapes, lines and fills
    // all draw there, with nothing left over from the old device.
    {
        Draw2D moved;
        CHECK(moved.prepare(gpu.device, format, font));
        const float square[] = {40, 40, 56, 40, 56, 56, 40, 56};
        auto scene = [&] {
            moved.rect(8, 8, 16, 16, 0, green);
            moved.line(0, 32, 64, 32, 4, red);
            moved.fill(square, 4, blue);
        };
        gpu.render(moved, scene);
        Gpu next;
        CHECK(next.create());
        CHECK(moved.prepare(next.device, format, font));
        px = next.render(moved, scene);
        CHECK(is(px, 16, 16, green) && is(px, 4, 32, red) && is(px, 48, 48, blue));
        CHECK(*next.context.errors == 0);
    }

    CHECK(*gpu.context.errors == 0); // no WebGPU validation errors along the way
    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}

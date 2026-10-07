#pragma once
#include <webgpu/webgpu_cpp.h>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stb_truetype.h>

// Small 2D drawing on WebGPU: rounded rectangles, circles, arcs, lines and
// text. Every shape is one instanced quad; a signed-distance shader gives
// antialiased edges at any scale. Text uses a distance-field atlas from a
// TrueType font (stb_truetype): ASCII is baked up front, any other character
// the first time it is drawn. Everything draws in call order.
namespace gpu {

struct Color { float r, g, b, a = 1; };

class Draw2D
{
public:
    enum class Align { left, center, right };
    struct Corners { float topLeft, topRight, bottomRight, bottomLeft; };
    struct Sides { float left, right, top, bottom; };

    // fontData: the bytes of a .ttf file, which becomes font 0. Returns false
    // if the font is unusable.
    bool prepare(const wgpu::Device &device, wgpu::TextureFormat format, std::string fontData);
    // Adds another font after prepare; returns its number, or -1 if unusable.
    int addFont(std::string fontData);

    // Coordinates are pixels from the top left, as in View::draw.
    void begin(float width, float height);
    void rect(float x, float y, float w, float h, float radius, Color fill,
              float border = 0, Color borderColor = {0, 0, 0, 0});
    // Each corner its own radius, each side its own border width.
    void rect(float x, float y, float w, float h, Corners radius, Color fill,
              Sides border = {}, Color borderColor = {0, 0, 0, 0});
    void circle(float cx, float cy, float radius, Color fill, float border = 0, Color borderColor = {0, 0, 0, 0});
    // Strokes with round ends and joins.
    void line(float x0, float y0, float x1, float y1, float width, Color color);
    void polyline(const float *xy, size_t points, float width, Color color);
    // A ring sector: angles in radians, 0 pointing up, increasing clockwise.
    void arc(float cx, float cy, float radius, float width, float from, float to, Color color);
    // y is the text's baseline; text is UTF-8; returns the text's width.
    // Distance-field text without hinting: keep it at 11 px or more on 1x screens.
    float text(float x, float y, float size, std::string_view text, Color color, Align align = Align::left, int font = 0);
    float measure(float size, std::string_view text, int font = 0);

    // Clips what follows to a rectangle, inside any clip already set, until
    // the matching popClip.
    void pushClip(float x, float y, float w, float h);
    void popClip();
    // Records a draw of your own (another pipeline, e.g. a plot shader) in
    // order with the rest; it runs during flush, clipped like the shapes.
    void custom(std::function<void(const wgpu::RenderPassEncoder &)> draw);

    void flush(const wgpu::RenderPassEncoder &pass);

private:
    struct Instance
    {
        float rect[4];   // x, y, width, height (pixels)
        float uv[4];     // glyphs: atlas rectangle; boxes: corner radii; arcs: angles
        float fill[4];
        float stroke[4]; // border color
        float shape[4];  // kind (0 box, 1 glyph, 3 arc), then per kind: see draw2d.cpp
        float border[4]; // boxes: left, right, top, bottom border widths
    };
    struct Glyph
    {
        float u0, v0, u1, v1; // atlas rectangle, 0..1
        float x, y, w, h;     // quad relative to the pen, at bakeSize
        float advance;        // at bakeSize
    };
    static constexpr float bakeSize = 24; // pixels the atlas is rendered at; distance fields scale up well
    static constexpr int atlasSize = 1024;

    struct LineStyle
    {
        float color[4];
        float halfWidth, pad[3];
    };
    struct LinePoint
    {
        float x, y, style, start; // start = 1 on each polyline's first point
    };
    // A stretch of draws of one kind under one clip, drawn in order by flush.
    struct Run
    {
        enum Kind { shapes, lines, custom } kind;
        uint32_t first, count; // instances or points
        uint32_t clip[4];      // scissor rectangle, pixels
        size_t callback;       // for custom: index into callbacks
    };
    Run &run(Run::Kind kind);
    const Glyph &glyph(int font, uint32_t codepoint);

    wgpu::Device device;
    wgpu::RenderPipeline pipeline, linePipeline;
    wgpu::Buffer uniforms, instances, lineStyles, linePoints;
    wgpu::BindGroup bindGroup, lineBindGroup;
    wgpu::Texture atlasTexture;
    size_t capacity = 0, styleCapacity = 0, pointCapacity = 0;
    std::vector<LineStyle> styles;
    std::vector<LinePoint> points;
    std::vector<Instance> batch;
    std::vector<Run> runs;
    std::vector<std::function<void(const wgpu::RenderPassEncoder &)>> callbacks;
    std::vector<std::array<uint32_t, 4>> clips;
    float size[2] = {1, 1};

    struct Font
    {
        std::string data; // stb_truetype reads it while baking
        stbtt_fontinfo info{};
        float scale = 1;
    };
    std::deque<Font> fonts; // a deque, so each info's pointer into data stays put
    std::unordered_map<uint64_t, Glyph> glyphs; // font << 32 | codepoint
    int penX = 4, penY = 4, rowHeight = 0; // where the next glyph goes in the atlas
};

}

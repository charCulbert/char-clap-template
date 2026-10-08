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
// antialiased edges at any scale. Text comes from a TrueType font
// (stb_truetype) through one atlas: as distance fields that scale, and below
// smallText pixels as glyphs rasterized at their exact size (at four
// quarter-pixel offsets, for even spacing), which stay sharp where a scaled
// field loses strokes. ASCII is baked
// up front, anything else the first time it is drawn. Everything draws in
// call order.
namespace gpu {

struct Color { float r, g, b, a = 1; };

class Draw2D
{
public:
    enum class Align { left, center, right };
    struct Corners { float topLeft, topRight, bottomRight, bottomLeft; };
    struct Sides { float left, right, top, bottom; };
    // From one colour to another along a line (pixels, like the rest), or
    // outwards from x0, y0 to the distance of x1, y1 if radial.
    struct Gradient { Color from, to; float x0, y0, x1, y1; bool radial = false; };

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
    // Filled with a gradient.
    void rect(float x, float y, float w, float h, Corners radius, const Gradient &fill);
    // A soft shadow of a rounded box; blur is about its width in pixels.
    // Draw it before the box it's under.
    void shadow(float x, float y, float w, float h, float radius, float blur, Color color);
    void circle(float cx, float cy, float radius, Color fill, float border = 0, Color borderColor = {0, 0, 0, 0});
    // Strokes with round ends and joins.
    void line(float x0, float y0, float x1, float y1, float width, Color color);
    void polyline(const float *xy, size_t points, float width, Color color);
    // A filled polygon (points x, y, ...; simple: edges may not cross),
    // antialiased by a hairline along its outline. For curves' fills, plots,
    // envelopes. Concave is fine.
    void fill(const float *xy, size_t points, Color color);
    // A ring sector: angles in radians, 0 pointing up, increasing clockwise.
    void arc(float cx, float cy, float radius, float width, float from, float to, Color color);
    // y is the text's baseline; text is UTF-8; returns the text's width.
    float text(float x, float y, float size, std::string_view text, Color color, Align align = Align::left, int font = 0);
    float measure(float size, std::string_view text, int font = 0);

    // Images: RGBA pixels (straight alpha, rows top down), or the bytes of a
    // PNG or JPEG file. Returns the image's number, or -1 if unusable.
    int addImage(const uint8_t *rgba, int width, int height);
    int addImage(std::string_view fileBytes);
    // An SVG document rasterized (with nanosvg) to fit width x height pixels,
    // keeping its proportions, as an image. Rasterize at the size it's drawn,
    // scale included, for sharp edges.
    int addSvg(std::string_view svg, int width, int height);
    // Draws an image (or the part of it in source, 0..1 across and down)
    // stretched over the rectangle, its corners rounded, each pixel multiplied
    // by tint: white draws it as it is, a lower alpha fades it, and a colour
    // tints a white icon.
    struct Source { float u0, v0, u1, v1; };
    void image(int image, float x, float y, float w, float h, Corners radius = {}, Color tint = {1, 1, 1, 1},
               Source source = {0, 0, 1, 1});
    // An image's size in pixels, or 0 x 0 for no such image.
    void imageSize(int image, int &width, int &height) const;

    // Clips what follows to a rectangle, inside any clip already set, until
    // the matching popClip.
    void pushClip(float x, float y, float w, float h);
    void popClip();
    // Records a draw of your own (another pipeline, e.g. a plot shader) in
    // order with the rest; it runs during flush, clipped like the shapes.
    // version: change it whenever what it draws changes, so changed() can
    // tell; 0 means it may change every frame.
    void custom(std::function<void(const wgpu::RenderPassEncoder &)> draw, uint64_t version = 0);

    void flush(const wgpu::RenderPassEncoder &pass);
    // True if what's recorded since begin differs from what the last flush
    // drew, custom draws' versions included (a version 0 always differs).
    bool changed() const;

private:
    struct Instance
    {
        float rect[4];   // x, y, width, height (pixels)
        float uv[4];     // glyphs: atlas rectangle; boxes: corner radii; arcs: angles
        float fill[4];
        float stroke[4]; // border color
        float shape[4];  // kind (0 box, 1 glyph, 2 image, 3 arc, 4 small glyph, 5 gradient, 6 shadow): see draw2d.cpp
        float border[4]; // boxes: left, right, top, bottom border widths; images: corner radii
    };
    struct Glyph
    {
        float u0, v0, u1, v1; // atlas rectangle, 0..1
        float x, y, w, h;     // quad relative to the pen, at bakeSize
        float advance;        // at bakeSize
    };
    static constexpr float bakeSize = 24; // pixels distance fields are rendered at; they scale up well
    static constexpr float smallText = 18; // pixels: below this, glyphs are rasterized at their size
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
        enum Kind { shapes, lines, custom, fills } kind;
        uint32_t first, count; // instances, points or (fills) vertices
        uint32_t clip[4];      // scissor rectangle, pixels
        size_t callback;       // for custom: index into callbacks
        int image;             // for shapes: the image bound, or -1 for the glyph atlas
        uint64_t version;      // for custom: see custom()
    };
    Run &run(Run::Kind kind, int image = -1);
    uint64_t signature() const; // a hash of everything recorded
    uint64_t drawn = 0;         // the signature the last flush drew
    mutable uint64_t recorded = 0; // this frame's, once changed() has worked it out
    float uniformSize[2] = {-1, -1}; // the canvas size the uniform buffer holds
    // pixelSize 0: a distance field; else rasterized at that size, shifted
    // right by phase quarters of a pixel.
    const Glyph &glyph(int font, uint32_t codepoint, int pixelSize = 0, int phase = 0);

    wgpu::Device device;
    wgpu::RenderPipeline pipeline, linePipeline, fillPipeline;
    wgpu::Buffer uniforms, instances, lineStyles, linePoints, fillBuffer;
    wgpu::BindGroup bindGroup, lineBindGroup, fillBindGroup;
    struct FillVertex { float x, y, color[4]; };
    std::vector<FillVertex> fillVertices;
    size_t fillCapacity = 0;
    wgpu::Texture atlasTexture;
    wgpu::Sampler sampler;
    std::vector<wgpu::BindGroup> images; // each image's textures, bound in place of the atlas
    std::vector<std::pair<int, int>> imageSizes;
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
    std::unordered_map<uint64_t, Glyph> glyphs; // pixel size << 48 | phase << 44 | font << 32 | codepoint
    int penX = 4, penY = 4, rowHeight = 0; // where the next glyph goes in the atlas
};

}

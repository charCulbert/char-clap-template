#pragma once
#include <webgpu/webgpu_cpp.h>
#include <string>
#include <string_view>
#include <vector>

// Small 2D drawing on WebGPU: rounded rectangles, circles and text. Every shape
// is one instanced quad; a signed-distance shader gives antialiased edges at
// any scale. Text uses a distance-field atlas built once from a TrueType font
// with stb_truetype (printable ASCII only).
namespace gpu {

struct Color { float r, g, b, a = 1; };

class Draw2D
{
public:
    enum class Align { left, center, right };

    // fontData: the bytes of a .ttf file. Returns false if the font is unusable.
    bool prepare(const wgpu::Device &device, wgpu::TextureFormat format, const std::string &fontData);

    // Coordinates are pixels from the top left, as in View::draw.
    void begin(float width, float height);
    void rect(float x, float y, float w, float h, float radius, Color fill,
              float border = 0, Color borderColor = {0, 0, 0, 0});
    void circle(float cx, float cy, float radius, Color fill, float border = 0, Color borderColor = {0, 0, 0, 0});
    // y is the text's baseline; returns the text's width.
    float text(float x, float y, float size, std::string_view text, Color color, Align align = Align::left);
    float measure(float size, std::string_view text) const;
    void flush(const wgpu::RenderPassEncoder &pass);

private:
    struct Instance
    {
        float rect[4];   // x, y, width, height (pixels)
        float uv[4];     // atlas rectangle, for glyphs
        float fill[4];
        float stroke[4]; // border color
        float shape[4];  // kind (0 shape, 1 glyph), corner radius, border width, glyph distance range
    };
    struct Glyph
    {
        float u0, v0, u1, v1; // atlas rectangle, 0..1
        float x, y, w, h;     // quad relative to the pen, at bakeSize
        float advance;        // at bakeSize
    };
    static constexpr float bakeSize = 48; // pixels the atlas is rendered at
    static constexpr int atlasSize = 1024;

    wgpu::Device device;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniforms, instances;
    wgpu::BindGroup bindGroup;
    size_t capacity = 0;
    float size[2] = {1, 1};
    Glyph glyphs[96] = {}; // ' ' .. '~'
    std::vector<Instance> batch;
};

}

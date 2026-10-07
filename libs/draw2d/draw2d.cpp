#define STB_TRUETYPE_IMPLEMENTATION
#include "draw2d.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <algorithm>
#include <cmath>

namespace gpu {

namespace {

// Lines: instance i is the segment from point i to point i + 1 (skipped where
// point i + 1 starts a new polyline). Only the points live on the GPU.
constexpr const char *lineShader = R"(
struct Uniforms { size: vec4f };
struct Style { color: vec4f, halfWidth: vec4f };
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read> styles: array<Style>;
@group(0) @binding(2) var<storage, read> points: array<vec4f>; // x, y, style, start
struct Varyings {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f,
    @location(1) @interpolate(flat) params: vec4f, // half length, half width
    @location(2) @interpolate(flat) color: vec4f,
};
@vertex fn vs(@builtin(vertex_index) v: u32, @builtin(instance_index) i: u32) -> Varyings {
    let p0 = points[i];
    let p1 = points[i + 1u];
    let style = styles[u32(p0.z)];
    var out: Varyings;
    if (p1.w > 0.5) { // next point starts another polyline
        out.position = vec4f(2.0, 2.0, 2.0, 1.0);
        return out;
    }
    let corner = vec2f(f32(v & 1u), f32(v >> 1u)) - 0.5;
    let len = max(length(p1.xy - p0.xy), 1e-3);
    let along = (p1.xy - p0.xy) / len;
    let across = vec2f(-along.y, along.x);
    let r = style.halfWidth.x + 1.0; // one pixel for antialiasing
    out.local = corner * 2.0 * vec2f(len * 0.5 + r, r);
    let pixel = (p0.xy + p1.xy) * 0.5 + along * out.local.x + across * out.local.y;
    out.position = vec4f(pixel / u.size.xy * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.params = vec4f(len * 0.5, style.halfWidth.x, 0.0, 0.0);
    out.color = style.color;
    return out;
}
@fragment fn fs(in: Varyings) -> @location(0) vec4f {
    let q = abs(in.local) - vec2f(in.params.x, 0.0);
    let d = length(max(q, vec2f(0.0))) - in.params.y; // pixels
    return vec4f(in.color.rgb, in.color.a * clamp(0.5 - d, 0.0, 1.0));
}
)";

constexpr const char *shader = R"(
struct Uniforms { size: vec4f };
struct Instance {
    @location(0) rect: vec4f,
    @location(1) uv: vec4f,
    @location(2) fill: vec4f,
    @location(3) stroke: vec4f,
    @location(4) shape: vec4f,
    @location(5) border: vec4f,
};
struct Varyings {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f,   // pixels from the quad's centre
    @location(1) uv: vec2f,
    @location(2) fill: vec4f,
    @location(3) stroke: vec4f,
    @location(4) @interpolate(flat) shape: vec4f,
    @location(5) @interpolate(flat) half: vec2f,
    @location(6) @interpolate(flat) radii: vec4f,
    @location(7) @interpolate(flat) border: vec4f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var atlasSampler: sampler;

// Kinds: 0 box   rect = x, y, w, h        uv = corner radii (TL, TR, BR, BL), border = L, R, T, B widths
//        1 glyph rect = x, y, w, h        uv = atlas rectangle
//        2 image rect = x, y, w, h        uv = 0, 0, 1, 1, border = corner radii; the image is bound as atlas
//        3 arc   rect = x, y, w, h        uv = from, to (radians), shape.y = half width

// Distance to a box of half size b, each corner its own radius r (TL, TR,
// BR, BL; y grows downwards). After Inigo Quilez's sdRoundedBox
// (iquilezles.org/articles/distfunctions2d).
fn roundedBox(p: vec2f, b: vec2f, r4: vec4f) -> f32 {
    let side = select(vec2f(r4.x, r4.w), vec2f(r4.y, r4.z), p.x > 0.0); // left or right corners
    let r = min(select(side.x, side.y, p.y > 0.0), min(b.x, b.y));
    let q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2f(0.0))) - r;
}
@vertex fn vs(@builtin(vertex_index) v: u32, i: Instance) -> Varyings {
    let corner = vec2f(f32(v & 1u), f32(v >> 1u));  // triangle strip: 0,0 1,0 0,1 1,1
    let pixel = i.rect.xy + corner * i.rect.zw;
    let half = i.rect.zw * 0.5;
    let local = (corner - 0.5) * i.rect.zw;
    var out: Varyings;
    out.position = vec4f(pixel / u.size.xy * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.half = half;
    out.local = local;
    out.uv = select(mix(i.uv.xy, i.uv.zw, corner), i.uv.xy, i.shape.x == 3.0); // arcs: constant angles
    out.radii = i.uv;
    out.border = i.border;
    out.fill = i.fill;
    out.stroke = i.stroke;
    out.shape = i.shape;
    return out;
}

@fragment fn fs(in: Varyings) -> @location(0) vec4f {
    // Both paths run for every pixel: WGSL allows derivatives and texture
    // sampling only where all neighbouring pixels take the same branch.

    // Glyph: the atlas holds distance to the outline, 0.5 on the edge.
    let texel = textureSample(atlas, atlasSampler, in.uv);
    let g = texel.r;
    let gw = fwidth(g) * 0.75;
    let glyph = vec4f(in.fill.rgb, in.fill.a * smoothstep(0.5 - gw, 0.5 + gw, g));

    // Rounded rectangle (a circle is one with radius = half its size). The
    // border is what lies outside the box inset by each side's width, whose
    // corners shrink by the border, as CSS does.
    // Distances are in pixels, so a pixel's coverage is 0.5 - d: exact
    // antialiasing without derivatives (which double along corner diagonals).
    let d = roundedBox(in.local, in.half, in.radii);
    let inside = clamp(0.5 - d, 0.0, 1.0);
    let b = in.border; // left, right, top, bottom
    let innerHalf = max(in.half - 0.5 * vec2f(b.x + b.y, b.z + b.w), vec2f(0.0));
    let innerCentre = 0.5 * vec2f(b.x - b.y, b.z - b.w);
    let innerRadii = max(in.radii - vec4f(max(b.x, b.z), max(b.y, b.z), max(b.y, b.w), max(b.x, b.w)), vec4f(0.0));
    let di = roundedBox(in.local - innerCentre, innerHalf, innerRadii);
    let inner = select(clamp(0.5 - di, 0.0, 1.0), 1.0, max(max(b.x, b.y), max(b.z, b.w)) <= 0.0);
    let color = mix(in.stroke, in.fill, inner);
    let shape = vec4f(color.rgb, color.a * inside);

    // Arc: the ring, cut square at both ends. The angle is measured from
    // the arc's middle, so it's continuous across the arc whatever its range
    // (one may pass 2π, as a knob's track from 7:30 through 12 to 4:30 does);
    // past an end, the distance is to the line the cut lies on.
    let mid = 0.5 * (in.uv.x + in.uv.y);
    let span = 0.5 * (in.uv.y - in.uv.x);
    let r = length(in.local);
    let delta = atan2(dot(in.local, vec2f(cos(mid), sin(mid))), dot(in.local, vec2f(sin(mid), -cos(mid))));
    let beyond = abs(delta) - span; // radians past the nearer end
    let ends = select(r * sin(clamp(beyond, -1.5707963, 1.5707963)), -1e3, span >= 3.1415926);
    let ring = abs(r - (in.half.x - in.shape.y - 1.0)) - in.shape.y;
    let da = max(ring, ends);
    let arc = vec4f(in.fill.rgb, in.fill.a * clamp(0.5 - da, 0.0, 1.0));

    // Image: its texel, inside the rounded box.
    let image = vec4f(texel.rgb, texel.a * in.fill.a * clamp(0.5 - roundedBox(in.local, in.half, in.border), 0.0, 1.0));

    let kind = in.shape.x;
    return select(select(select(shape, glyph, kind == 1.0), image, kind == 2.0), arc, kind == 3.0);
}
)";

}

bool Draw2D::prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format, std::string fontData)
{
    device = newDevice;
    fonts.clear();
    glyphs.clear();
    penX = penY = 4, rowHeight = 0;

    wgpu::TextureDescriptor textureDescriptor;
    textureDescriptor.size = {atlasSize, atlasSize, 1};
    textureDescriptor.format = wgpu::TextureFormat::R8Unorm;
    textureDescriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    atlasTexture = device.CreateTexture(&textureDescriptor);
    if (addFont(std::move(fontData)) != 0) return false;

    wgpu::SamplerDescriptor samplerDescriptor;
    samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
    sampler = device.CreateSampler(&samplerDescriptor); // clamps to the edge: images don't wrap
    images.clear();

    wgpu::ShaderSourceWGSL source;
    source.code = shader;
    wgpu::ShaderModuleDescriptor moduleDescriptor;
    moduleDescriptor.nextInChain = &source;
    wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

    constexpr uint32_t attributeCount = sizeof(Instance) / 16;
    wgpu::VertexAttribute attributes[attributeCount];
    for (uint32_t i = 0; i < attributeCount; ++i)
        attributes[i] = {nullptr, wgpu::VertexFormat::Float32x4, i * 16ull, i};
    wgpu::VertexBufferLayout instanceLayout;
    instanceLayout.stepMode = wgpu::VertexStepMode::Instance;
    instanceLayout.arrayStride = sizeof(Instance);
    instanceLayout.attributeCount = attributeCount;
    instanceLayout.attributes = attributes;

    wgpu::BlendState blend;
    blend.color = {wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha};
    blend.alpha = {wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha};
    wgpu::ColorTargetState target;
    target.format = format;
    target.blend = &blend;
    wgpu::FragmentState fragment;
    fragment.module = module;
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor pipelineDescriptor;
    pipelineDescriptor.vertex.module = module;
    pipelineDescriptor.vertex.bufferCount = 1;
    pipelineDescriptor.vertex.buffers = &instanceLayout;
    pipelineDescriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    pipelineDescriptor.fragment = &fragment;
    pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

    wgpu::BufferDescriptor uniformDescriptor;
    uniformDescriptor.size = 16;
    uniformDescriptor.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    uniforms = device.CreateBuffer(&uniformDescriptor);

    // The line pipeline: same blending, points and styles from storage buffers.
    wgpu::ShaderSourceWGSL lineSource;
    lineSource.code = lineShader;
    wgpu::ShaderModuleDescriptor lineModuleDescriptor;
    lineModuleDescriptor.nextInChain = &lineSource;
    wgpu::ShaderModule lineModule = device.CreateShaderModule(&lineModuleDescriptor);
    fragment.module = lineModule;
    pipelineDescriptor.vertex.module = lineModule;
    pipelineDescriptor.vertex.bufferCount = 0;
    linePipeline = device.CreateRenderPipeline(&pipelineDescriptor);

    wgpu::BindGroupEntry entries[3];
    entries[0].binding = 0;
    entries[0].buffer = uniforms;
    entries[1].binding = 1;
    entries[1].textureView = atlasTexture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = sampler;
    wgpu::BindGroupDescriptor groupDescriptor;
    groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
    groupDescriptor.entryCount = 3;
    groupDescriptor.entries = entries;
    bindGroup = device.CreateBindGroup(&groupDescriptor);
    return true;
}

int Draw2D::addFont(std::string fontData)
{
    Font &font = fonts.emplace_back();
    font.data = std::move(fontData);
    const auto *bytes = reinterpret_cast<const unsigned char *>(font.data.data());
    if (font.data.empty() || !stbtt_InitFont(&font.info, bytes, stbtt_GetFontOffsetForIndex(bytes, 0)))
    {
        fonts.pop_back();
        return -1;
    }
    font.scale = stbtt_ScaleForPixelHeight(&font.info, bakeSize);
    const int number = int(fonts.size() - 1);
    for (uint32_t c = ' '; c <= '~'; ++c) glyph(number, c); // the common ones up front
    return number;
}

// The glyph for a character, baked into the atlas the first time it's asked
// for. Characters the font lacks get its "missing" glyph, so they show.
const Draw2D::Glyph &Draw2D::glyph(int fontNumber, uint32_t c)
{
    const uint64_t key = uint64_t(fontNumber) << 32 | c;
    if (auto found = glyphs.find(key); found != glyphs.end()) return found->second;
    Glyph &g = glyphs[key];
    if (fontNumber < 0 || fontNumber >= int(fonts.size())) return g;
    Font &font = fonts[size_t(fontNumber)];
    const int index = stbtt_FindGlyphIndex(&font.info, int(c));
    int advance, bearing;
    stbtt_GetGlyphHMetrics(&font.info, index, &advance, &bearing);
    g.advance = advance * font.scale;
    constexpr int padding = 4;
    constexpr unsigned char onEdge = 128;
    constexpr float distanceScale = onEdge / float(padding);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *sdf = stbtt_GetGlyphSDF(&font.info, font.scale, index, padding, onEdge, distanceScale, &w, &h, &xoff, &yoff);
    if (!sdf) return g; // nothing to draw, e.g. a space
    // A few texels between glyphs, so small text sampled from the big atlas
    // never picks up a neighbour. A full atlas draws nothing for new glyphs.
    constexpr int gap = 4;
    if (penX + w + gap > atlasSize) penX = gap, penY += rowHeight + gap, rowHeight = 0;
    if (penY + h + gap > atlasSize) { stbtt_FreeSDF(sdf, nullptr); return g; }
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = atlasTexture;
    destination.origin = {uint32_t(penX), uint32_t(penY), 0};
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = uint32_t(w);
    layout.rowsPerImage = uint32_t(h);
    const wgpu::Extent3D extent = {uint32_t(w), uint32_t(h), 1};
    device.GetQueue().WriteTexture(&destination, sdf, size_t(w) * h, &layout, &extent);
    stbtt_FreeSDF(sdf, nullptr);
    g = {float(penX) / atlasSize, float(penY) / atlasSize, float(penX + w) / atlasSize, float(penY + h) / atlasSize,
         float(xoff), float(yoff), float(w), float(h), g.advance};
    penX += w + gap;
    rowHeight = std::max(rowHeight, h);
    return g;
}

void Draw2D::begin(float width, float height)
{
    size[0] = width, size[1] = height;
    batch.clear();
    styles.clear();
    points.clear();
    runs.clear();
    callbacks.clear();
    clips.assign(1, {0, 0, uint32_t(std::max(width, 0.0f)), uint32_t(std::max(height, 0.0f))});
}

// The run the next draw of this kind joins: the last one, unless the kind or
// the clip changed since.
Draw2D::Run &Draw2D::run(Run::Kind kind, int image)
{
    const auto &clip = clips.back();
    if (runs.empty() || runs.back().kind != kind || kind == Run::custom || runs.back().image != image
        || !std::equal(clip.begin(), clip.end(), runs.back().clip))
    {
        const uint32_t first = uint32_t(kind == Run::lines ? points.size() : batch.size());
        runs.push_back({kind, first, 0, {clip[0], clip[1], clip[2], clip[3]}, 0, image});
    }
    return runs.back();
}

void Draw2D::rect(float x, float y, float w, float h, float radius, Color fill, float border, Color borderColor)
{
    rect(x, y, w, h, Corners{radius, radius, radius, radius}, fill, Sides{border, border, border, border}, borderColor);
}

void Draw2D::rect(float x, float y, float w, float h, Corners r, Color fill, Sides b, Color borderColor)
{
    run(Run::shapes).count++;
    batch.push_back({{x, y, w, h}, {r.topLeft, r.topRight, r.bottomRight, r.bottomLeft}, {fill.r, fill.g, fill.b, fill.a},
                     {borderColor.r, borderColor.g, borderColor.b, borderColor.a}, {0, 0, 0, 0},
                     {b.left, b.right, b.top, b.bottom}});
}

void Draw2D::circle(float cx, float cy, float radius, Color fill, float border, Color borderColor)
{
    rect(cx - radius, cy - radius, 2 * radius, 2 * radius, radius, fill, border, borderColor);
}

void Draw2D::line(float x0, float y0, float x1, float y1, float width, Color color)
{
    const float xy[] = {x0, y0, x1, y1};
    polyline(xy, 2, width, color);
}

void Draw2D::polyline(const float *xy, size_t count, float width, Color color)
{
    if (count < 2) return;
    run(Run::lines).count += uint32_t(count);
    const float style = float(styles.size());
    styles.push_back({{color.r, color.g, color.b, color.a}, width / 2, {}});
    for (size_t i = 0; i < count; ++i)
        points.push_back({xy[2 * i], xy[2 * i + 1], style, i == 0 ? 1.0f : 0.0f});
}

void Draw2D::arc(float cx, float cy, float radius, float width, float from, float to, Color color)
{
    const float r = radius + width / 2 + 1;
    run(Run::shapes).count++;
    batch.push_back({{cx - r, cy - r, 2 * r, 2 * r}, {from, to, 0, 0}, {color.r, color.g, color.b, color.a}, {},
                     {3, width / 2, 0, 0}, {}});
}

// Reads one UTF-8 character from text at i, moving i past it. Malformed
// bytes read as U+FFFD.
static uint32_t nextCodepoint(std::string_view text, size_t &i)
{
    const unsigned char c = text[i++];
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0) return 0xFFFD;
    uint32_t codepoint = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k)
    {
        if (i >= text.size() || (text[i] & 0xC0) != 0x80) return 0xFFFD;
        codepoint = codepoint << 6 | (text[i++] & 0x3F);
    }
    return codepoint;
}

float Draw2D::measure(float textSize, std::string_view text, int font)
{
    float width = 0;
    for (size_t i = 0; i < text.size();)
        if (const uint32_t c = nextCodepoint(text, i); c >= ' ') width += glyph(font, c).advance;
    return width * textSize / bakeSize;
}

float Draw2D::text(float x, float y, float textSize, std::string_view text, Color color, Align align, int font)
{
    const float width = measure(textSize, text, font);
    if (align == Align::center) x -= width / 2;
    if (align == Align::right) x -= width;
    const float k = textSize / bakeSize;
    for (size_t i = 0; i < text.size();)
    {
        const uint32_t c = nextCodepoint(text, i);
        if (c < ' ') continue;
        const Glyph &g = glyph(font, c);
        if (g.w > 0)
        {
            run(Run::shapes).count++;
            batch.push_back({{x + g.x * k, y + g.y * k, g.w * k, g.h * k}, {g.u0, g.v0, g.u1, g.v1},
                             {color.r, color.g, color.b, color.a}, {}, {1, 0, 0, 0}, {}});
        }
        x += g.advance * k;
    }
    return width;
}

int Draw2D::addImage(const uint8_t *rgba, int width, int height)
{
    if (!device || !rgba || width <= 0 || height <= 0) return -1;
    wgpu::TextureDescriptor textureDescriptor;
    textureDescriptor.size = {uint32_t(width), uint32_t(height), 1};
    textureDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    textureDescriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    const wgpu::Texture texture = device.CreateTexture(&textureDescriptor);
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = uint32_t(width) * 4;
    layout.rowsPerImage = uint32_t(height);
    device.GetQueue().WriteTexture(&destination, rgba, size_t(width) * height * 4, &layout, &textureDescriptor.size);
    wgpu::BindGroupEntry entries[3];
    entries[0].binding = 0;
    entries[0].buffer = uniforms;
    entries[1].binding = 1;
    entries[1].textureView = texture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = sampler;
    wgpu::BindGroupDescriptor descriptor;
    descriptor.layout = pipeline.GetBindGroupLayout(0);
    descriptor.entryCount = 3;
    descriptor.entries = entries;
    images.push_back(device.CreateBindGroup(&descriptor));
    return int(images.size() - 1);
}

int Draw2D::addImage(std::string_view fileBytes)
{
    int width = 0, height = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(fileBytes.data()),
                                                int(fileBytes.size()), &width, &height, &channels, 4);
    if (!rgba) return -1;
    const int number = addImage(rgba, width, height);
    stbi_image_free(rgba);
    return number;
}

void Draw2D::image(int number, float x, float y, float w, float h, Corners r, float alpha)
{
    if (number < 0 || number >= int(images.size())) return;
    run(Run::shapes, number).count++;
    batch.push_back({{x, y, w, h}, {0, 0, 1, 1}, {1, 1, 1, alpha}, {}, {2, 0, 0, 0},
                     {r.topLeft, r.topRight, r.bottomRight, r.bottomLeft}});
}

void Draw2D::pushClip(float x, float y, float w, float h)
{
    const auto &outer = clips.back();
    const float x0 = std::max(std::floor(x), float(outer[0])), y0 = std::max(std::floor(y), float(outer[1]));
    const float x1 = std::min(std::ceil(x + w), float(outer[0] + outer[2]));
    const float y1 = std::min(std::ceil(y + h), float(outer[1] + outer[3]));
    clips.push_back({uint32_t(x0), uint32_t(y0), uint32_t(std::max(x1 - x0, 0.0f)), uint32_t(std::max(y1 - y0, 0.0f))});
}

void Draw2D::popClip()
{
    if (clips.size() > 1) clips.pop_back();
}

void Draw2D::custom(std::function<void(const wgpu::RenderPassEncoder &)> draw)
{
    run(Run::custom).callback = callbacks.size();
    callbacks.push_back(std::move(draw));
}

// Grows a buffer to hold at least `bytes`, doubling to avoid churn.
static void reserve(const wgpu::Device &device, wgpu::Buffer &buffer, size_t &capacity, size_t bytes, wgpu::BufferUsage usage)
{
    if (bytes <= capacity) return;
    capacity = std::max(bytes, 2 * capacity);
    wgpu::BufferDescriptor descriptor;
    descriptor.size = (capacity + 15) & ~size_t(15);
    descriptor.usage = usage | wgpu::BufferUsage::CopyDst;
    buffer = device.CreateBuffer(&descriptor);
}

void Draw2D::flush(const wgpu::RenderPassEncoder &pass)
{
    if (!pipeline) return;
    const float data[4] = {size[0], size[1], 0, 0};
    device.GetQueue().WriteBuffer(uniforms, 0, data, sizeof(data));

    // Everything goes up in one write per buffer; the runs then draw slices of it.
    if (!batch.empty())
    {
        const uint64_t bytes = batch.size() * sizeof(Instance);
        reserve(device, instances, capacity, bytes, wgpu::BufferUsage::Vertex);
        device.GetQueue().WriteBuffer(instances, 0, batch.data(), bytes);
    }
    if (points.size() >= 2)
    {
        const size_t styleBytes = styles.size() * sizeof(LineStyle), pointBytes = points.size() * sizeof(LinePoint);
        const size_t oldStyles = styleCapacity, oldPoints = pointCapacity;
        reserve(device, lineStyles, styleCapacity, styleBytes, wgpu::BufferUsage::Storage);
        reserve(device, linePoints, pointCapacity, pointBytes, wgpu::BufferUsage::Storage);
        if (!lineBindGroup || oldStyles != styleCapacity || oldPoints != pointCapacity)
        {
            wgpu::BindGroupEntry entries[3];
            entries[0].binding = 0;
            entries[0].buffer = uniforms;
            entries[1].binding = 1;
            entries[1].buffer = lineStyles;
            entries[2].binding = 2;
            entries[2].buffer = linePoints;
            wgpu::BindGroupDescriptor descriptor;
            descriptor.layout = linePipeline.GetBindGroupLayout(0);
            descriptor.entryCount = 3;
            descriptor.entries = entries;
            lineBindGroup = device.CreateBindGroup(&descriptor);
        }
        device.GetQueue().WriteBuffer(lineStyles, 0, styles.data(), styleBytes);
        device.GetQueue().WriteBuffer(linePoints, 0, points.data(), pointBytes);
    }

    for (const Run &r : runs)
    {
        if (r.clip[2] == 0 || r.clip[3] == 0) continue; // clipped away entirely
        pass.SetScissorRect(r.clip[0], r.clip[1], r.clip[2], r.clip[3]);
        if (r.kind == Run::shapes && r.count)
        {
            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, r.image >= 0 ? images[size_t(r.image)] : bindGroup);
            pass.SetVertexBuffer(0, instances);
            pass.Draw(4, r.count, 0, r.first);
        }
        // Instance i is the segment from point i to i + 1, so n points draw n - 1.
        if (r.kind == Run::lines && r.count >= 2)
        {
            pass.SetPipeline(linePipeline);
            pass.SetBindGroup(0, lineBindGroup);
            pass.Draw(4, r.count - 1, 0, r.first);
        }
        if (r.kind == Run::custom) callbacks[r.callback](pass);
    }
    pass.SetScissorRect(0, 0, uint32_t(size[0]), uint32_t(size[1]));
}

}

#define STB_TRUETYPE_IMPLEMENTATION
#include "draw2d.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>

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
//        1 glyph rect = x, y, w, h        uv = atlas rectangle (a distance field)
//        4 glyph rect = x, y, w, h        uv = atlas rectangle (coverage, drawn 1:1)
//        2 image rect = x, y, w, h        uv = 0, 0, 1, 1, border = corner radii; the image is bound as atlas
//        3 arc   rect = x, y, w, h        uv = from, to (radians), shape.y = half width
//        5 gradient (a box)               uv = corner radii, fill to stroke along border = x0, y0, x1, y1
//                                         (from the centre), shape.y = 1 for radial
//        6 shadow rect = the blur's extent  border = box half width, half height, corner radius, blur sigma

// Error function, approximated (Abramowitz and Stegun 7.1.27).
fn erf2(x: vec2f) -> vec2f {
    let s = sign(x);
    let a = abs(x);
    var r = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    r = r * r;
    return s - s / (r * r);
}
// A rounded box blurred by a Gaussian, in closed form along x and summed
// over a few samples along y: Evan Wallace's "Fast rounded rectangle
// shadows" (madebyevan.com/shaders/fast-rounded-rectangle-shadows).
fn shadowX(x: f32, y: f32, sigma: f32, corner: f32, half: vec2f) -> f32 {
    let delta = min(half.y - corner - abs(y), 0.0);
    let curved = half.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    let integral = 0.5 + 0.5 * erf2((x + vec2f(-curved, curved)) * (0.70710678 / sigma));
    return integral.y - integral.x;
}
fn boxShadow(p: vec2f, half: vec2f, corner: f32, sigma: f32) -> f32 {
    let low = p.y - half.y;
    let high = p.y + half.y;
    let start = clamp(-3.0 * sigma, low, high);
    let end = clamp(3.0 * sigma, low, high);
    let step = (end - start) / 4.0;
    var y = start + step * 0.5;
    var value = 0.0;
    for (var i = 0; i < 4; i++) {
        value += shadowX(p.x, p.y - y, sigma, corner, half) * exp(-(y * y) / (2.0 * sigma * sigma)) / (2.5066283 * sigma) * step;
        y += step;
    }
    return value;
}

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

    // Small glyph: the atlas holds coverage, at this very size.
    let small = vec4f(in.fill.rgb, in.fill.a * g);

    // Image: its texel, inside the rounded box.
    let image = vec4f(texel.rgb, texel.a * in.fill.a * clamp(0.5 - roundedBox(in.local, in.half, in.border), 0.0, 1.0));

    // Gradient: from fill to stroke along the line border.xy to border.zw,
    // or outwards from border.xy to the distance of border.zw.
    let along = in.border.zw - in.border.xy;
    let linear = dot(in.local - in.border.xy, along) / max(dot(along, along), 1e-6);
    let radial = length(in.local - in.border.xy) / max(length(along), 1e-6);
    let tone = mix(in.fill, in.stroke, clamp(select(linear, radial, in.shape.y > 0.5), 0.0, 1.0));
    let gradient = vec4f(tone.rgb, tone.a * inside);

    // Shadow: the blurred box, its sigma at least a tenth of a pixel.
    let shade = boxShadow(in.local, in.border.xy, in.border.z, max(in.border.w, 0.1));
    let shadow = vec4f(in.fill.rgb, in.fill.a * shade);

    let kind = in.shape.x;
    let basic = select(select(select(select(shape, glyph, kind == 1.0), image, kind == 2.0), arc, kind == 3.0), small, kind == 4.0);
    return select(select(basic, gradient, kind == 5.0), shadow, kind == 6.0);
}
)";

}

bool Draw2D::prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format, std::string fontData)
{
    device = newDevice;
    drawn = 0;
    uniformSize[0] = uniformSize[1] = -1;
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
const Draw2D::Glyph &Draw2D::glyph(int fontNumber, uint32_t c, int pixelSize, int phase)
{
    const uint64_t key = uint64_t(pixelSize) << 48 | uint64_t(phase) << 44 | uint64_t(fontNumber) << 32 | c;
    if (auto found = glyphs.find(key); found != glyphs.end()) return found->second;
    Glyph &g = glyphs[key];
    if (fontNumber < 0 || fontNumber >= int(fonts.size())) return g;
    Font &font = fonts[size_t(fontNumber)];
    const int index = stbtt_FindGlyphIndex(&font.info, int(c));
    int advance, bearing;
    stbtt_GetGlyphHMetrics(&font.info, index, &advance, &bearing);
    g.advance = advance * font.scale; // at bakeSize for both kinds, so they measure alike
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *sdf = nullptr;
    std::vector<unsigned char> coverage;
    if (pixelSize > 0)
    {
        const float scale = stbtt_ScaleForPixelHeight(&font.info, float(pixelSize));
        const float shift = phase / 4.0f;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&font.info, index, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
        w = x1 - x0, h = y1 - y0, xoff = x0, yoff = y0;
        if (w <= 0 || h <= 0) return g;
        coverage.resize(size_t(w) * h);
        stbtt_MakeGlyphBitmapSubpixel(&font.info, coverage.data(), w, h, w, scale, scale, shift, 0, index);
    }
    else
    {
        constexpr int padding = 4;
        constexpr unsigned char onEdge = 128;
        constexpr float distanceScale = onEdge / float(padding);
        sdf = stbtt_GetGlyphSDF(&font.info, font.scale, index, padding, onEdge, distanceScale, &w, &h, &xoff, &yoff);
        if (!sdf) return g; // nothing to draw, e.g. a space
    }
    const unsigned char *pixels = sdf ? sdf : coverage.data();
    // A few texels between glyphs, so small text sampled from the big atlas
    // never picks up a neighbour. A full atlas draws nothing for new glyphs.
    constexpr int gap = 4;
    if (penX + w + gap > atlasSize) penX = gap, penY += rowHeight + gap, rowHeight = 0;
    if (penY + h + gap > atlasSize) { if (sdf) stbtt_FreeSDF(sdf, nullptr); return g; }
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = atlasTexture;
    destination.origin = {uint32_t(penX), uint32_t(penY), 0};
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = uint32_t(w);
    layout.rowsPerImage = uint32_t(h);
    const wgpu::Extent3D extent = {uint32_t(w), uint32_t(h), 1};
    device.GetQueue().WriteTexture(&destination, pixels, size_t(w) * h, &layout, &extent);
    if (sdf) stbtt_FreeSDF(sdf, nullptr);
    g = {float(penX) / atlasSize, float(penY) / atlasSize, float(penX + w) / atlasSize, float(penY + h) / atlasSize,
         float(xoff), float(yoff), float(w), float(h), g.advance};
    penX += w + gap;
    rowHeight = std::max(rowHeight, h);
    return g;
}

void Draw2D::begin(float width, float height)
{
    recorded = 0;
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
        runs.push_back({kind, first, 0, {clip[0], clip[1], clip[2], clip[3]}, 0, image, 0});
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

void Draw2D::rect(float x, float y, float w, float h, Corners r, const Gradient &g)
{
    run(Run::shapes).count++;
    const float cx = x + w / 2, cy = y + h / 2;
    batch.push_back({{x, y, w, h}, {r.topLeft, r.topRight, r.bottomRight, r.bottomLeft},
                     {g.from.r, g.from.g, g.from.b, g.from.a}, {g.to.r, g.to.g, g.to.b, g.to.a}, {5, g.radial ? 1.0f : 0.0f, 0, 0},
                     {g.x0 - cx, g.y0 - cy, g.x1 - cx, g.y1 - cy}});
}

void Draw2D::shadow(float x, float y, float w, float h, float radius, float blur, Color color)
{
    // The quad reaches three sigmas past the box, where the blur fades out.
    const float sigma = std::max(blur, 0.1f) / 2, reach = 3 * sigma;
    run(Run::shapes).count++;
    batch.push_back({{x - reach, y - reach, w + 2 * reach, h + 2 * reach}, {}, {color.r, color.g, color.b, color.a}, {},
                     {6, 0, 0, 0}, {w / 2, h / 2, std::min(radius, std::min(w, h) / 2), sigma}});
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
    // Small text: glyphs made at this size, each drawn on a whole pixel from
    // the bitmap shifted nearest the pen's fraction of one; the pen still
    // advances by the exact widths, so layout doesn't change.
    const int pixelSize = textSize < smallText ? int(std::lround(textSize)) : 0;
    const float baseline = pixelSize ? std::round(y) : y;
    for (size_t i = 0; i < text.size();)
    {
        const uint32_t c = nextCodepoint(text, i);
        if (c < ' ') continue;
        const float advance = glyph(font, c).advance; // a copy: the next lookup may rehash the map
        float whole = std::floor(x);
        int phase = int(std::lround((x - whole) * 4));
        if (phase == 4) whole += 1, phase = 0;
        const Glyph &g = glyph(font, c, pixelSize, pixelSize ? phase : 0);
        if (g.w > 0)
        {
            run(Run::shapes).count++;
            if (pixelSize)
                batch.push_back({{whole + g.x, baseline + g.y, g.w, g.h}, {g.u0, g.v0, g.u1, g.v1},
                                 {color.r, color.g, color.b, color.a}, {}, {4, 0, 0, 0}, {}});
            else
                batch.push_back({{x + g.x * k, y + g.y * k, g.w * k, g.h * k}, {g.u0, g.v0, g.u1, g.v1},
                                 {color.r, color.g, color.b, color.a}, {}, {1, 0, 0, 0}, {}});
        }
        x += advance * k;
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

void Draw2D::custom(std::function<void(const wgpu::RenderPassEncoder &)> draw, uint64_t version)
{
    Run &r = run(Run::custom);
    r.callback = callbacks.size();
    r.version = version;
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

// A hash of the recorded draws, runs and canvas size: FNV-1a's mixing, a
// word at a time (draws are floats, so their sizes are multiples of 4).
uint64_t Draw2D::signature() const
{
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](const void *data, size_t bytes) {
        const auto *p = static_cast<const uint8_t *>(data);
        for (; bytes >= 8; p += 8, bytes -= 8)
        {
            uint64_t word;
            std::memcpy(&word, p, 8);
            hash = (hash ^ word) * 1099511628211ull;
        }
        for (; bytes; ++p, --bytes) hash = (hash ^ *p) * 1099511628211ull;
    };
    add(size, sizeof(size));
    add(batch.data(), batch.size() * sizeof(Instance));
    add(styles.data(), styles.size() * sizeof(LineStyle));
    add(points.data(), points.size() * sizeof(LinePoint));
    for (const Run &r : runs)
        add(&r.kind, sizeof(r.kind)), add(&r.first, 2 * sizeof(uint32_t)), add(r.clip, sizeof(r.clip)), add(&r.image, sizeof(r.image)),
            add(&r.version, sizeof(r.version));
    return hash;
}

bool Draw2D::changed() const
{
    recorded = signature(); // flush reuses it
    for (const Run &r : runs)
        if (r.kind == Run::custom && r.version == 0) return true;
    return recorded != drawn;
}

void Draw2D::flush(const wgpu::RenderPassEncoder &pass)
{
    if (!pipeline) return;
    drawn = recorded ? recorded : signature();
    recorded = 0;
    if (size[0] != uniformSize[0] || size[1] != uniformSize[1])
    {
        const float data[4] = {size[0], size[1], 0, 0};
        device.GetQueue().WriteBuffer(uniforms, 0, data, sizeof(data));
        uniformSize[0] = size[0], uniformSize[1] = size[1];
    }

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

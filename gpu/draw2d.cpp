#include "draw2d.h"
#include <algorithm>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace gpu {

namespace {

constexpr const char *shader = R"(
struct Uniforms { size: vec4f };
struct Instance {
    @location(0) rect: vec4f,
    @location(1) uv: vec4f,
    @location(2) fill: vec4f,
    @location(3) stroke: vec4f,
    @location(4) shape: vec4f,
};
struct Varyings {
    @builtin(position) position: vec4f,
    @location(0) local: vec2f,   // pixels from the quad's centre
    @location(1) uv: vec2f,
    @location(2) fill: vec4f,
    @location(3) stroke: vec4f,
    @location(4) @interpolate(flat) shape: vec4f,
    @location(5) @interpolate(flat) half: vec2f,
};
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var atlasSampler: sampler;

@vertex fn vs(@builtin(vertex_index) v: u32, i: Instance) -> Varyings {
    let corner = vec2f(f32(v & 1u), f32(v >> 1u));  // triangle strip: 0,0 1,0 0,1 1,1
    let pixel = i.rect.xy + corner * i.rect.zw;
    var out: Varyings;
    out.position = vec4f(pixel / u.size.xy * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    out.half = i.rect.zw * 0.5;
    out.local = (corner - 0.5) * i.rect.zw;
    out.uv = mix(i.uv.xy, i.uv.zw, corner);
    out.fill = i.fill;
    out.stroke = i.stroke;
    out.shape = i.shape;
    return out;
}

@fragment fn fs(in: Varyings) -> @location(0) vec4f {
    // Both paths run for every pixel: WGSL allows derivatives and texture
    // sampling only where all neighbouring pixels take the same branch.

    // Glyph: the atlas holds distance to the outline, 0.5 on the edge.
    let g = textureSample(atlas, atlasSampler, in.uv).r;
    let gw = fwidth(g) * 0.75;
    let glyph = vec4f(in.fill.rgb, in.fill.a * smoothstep(0.5 - gw, 0.5 + gw, g));

    // Rounded rectangle (a circle is one with radius = half its size).
    let radius = min(in.shape.y, min(in.half.x, in.half.y));
    let q = abs(in.local) - in.half + radius;
    let d = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - radius;
    let aa = fwidth(d);
    let inside = 1.0 - smoothstep(-aa, aa, d);
    let inner = 1.0 - smoothstep(-aa, aa, d + in.shape.z);
    let color = mix(in.stroke, in.fill, select(1.0, inner, in.shape.z > 0.0));
    let shape = vec4f(color.rgb, color.a * inside);

    return select(shape, glyph, in.shape.x > 0.5);
}
)";

}

bool Draw2D::prepare(const wgpu::Device &newDevice, wgpu::TextureFormat format, const std::string &fontData)
{
    device = newDevice;

    // Bake the atlas: one distance field per printable ASCII glyph, packed in rows.
    stbtt_fontinfo font;
    const auto *bytes = reinterpret_cast<const unsigned char *>(fontData.data());
    if (fontData.empty() || !stbtt_InitFont(&font, bytes, stbtt_GetFontOffsetForIndex(bytes, 0))) return false;
    const float scale = stbtt_ScaleForPixelHeight(&font, bakeSize);
    constexpr int padding = 6;
    constexpr unsigned char onEdge = 128;
    constexpr float distanceScale = onEdge / float(padding);
    std::vector<unsigned char> atlas(size_t(atlasSize) * atlasSize, 0);
    int penX = 1, penY = 1, rowHeight = 0;
    for (int c = ' '; c <= '~'; ++c)
    {
        Glyph &g = glyphs[c - ' '];
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
        g.advance = advance * scale;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char *sdf = stbtt_GetCodepointSDF(&font, scale, c, padding, onEdge, distanceScale, &w, &h, &xoff, &yoff);
        if (!sdf) continue; // space
        if (penX + w + 1 > atlasSize) penX = 1, penY += rowHeight + 1, rowHeight = 0;
        if (penY + h + 1 > atlasSize) { stbtt_FreeSDF(sdf, nullptr); return false; }
        for (int row = 0; row < h; ++row)
            std::copy_n(sdf + row * w, w, &atlas[size_t(penY + row) * atlasSize + penX]);
        stbtt_FreeSDF(sdf, nullptr);
        g = {float(penX) / atlasSize, float(penY) / atlasSize, float(penX + w) / atlasSize,
             float(penY + h) / atlasSize, float(xoff), float(yoff), float(w), float(h), g.advance};
        penX += w + 1;
        rowHeight = std::max(rowHeight, h);
    }

    wgpu::TextureDescriptor textureDescriptor;
    textureDescriptor.size = {atlasSize, atlasSize, 1};
    textureDescriptor.format = wgpu::TextureFormat::R8Unorm;
    textureDescriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    wgpu::Texture texture = device.CreateTexture(&textureDescriptor);
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = atlasSize;
    layout.rowsPerImage = atlasSize;
    device.GetQueue().WriteTexture(&destination, atlas.data(), atlas.size(), &layout, &textureDescriptor.size);

    wgpu::SamplerDescriptor samplerDescriptor;
    samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
    wgpu::Sampler sampler = device.CreateSampler(&samplerDescriptor);

    wgpu::ShaderSourceWGSL source;
    source.code = shader;
    wgpu::ShaderModuleDescriptor moduleDescriptor;
    moduleDescriptor.nextInChain = &source;
    wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

    wgpu::VertexAttribute attributes[5];
    for (uint32_t i = 0; i < 5; ++i)
        attributes[i] = {nullptr, wgpu::VertexFormat::Float32x4, i * 16ull, i};
    wgpu::VertexBufferLayout instanceLayout;
    instanceLayout.stepMode = wgpu::VertexStepMode::Instance;
    instanceLayout.arrayStride = sizeof(Instance);
    instanceLayout.attributeCount = 5;
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

    wgpu::BindGroupEntry entries[3];
    entries[0].binding = 0;
    entries[0].buffer = uniforms;
    entries[1].binding = 1;
    entries[1].textureView = texture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = sampler;
    wgpu::BindGroupDescriptor groupDescriptor;
    groupDescriptor.layout = pipeline.GetBindGroupLayout(0);
    groupDescriptor.entryCount = 3;
    groupDescriptor.entries = entries;
    bindGroup = device.CreateBindGroup(&groupDescriptor);
    return true;
}

void Draw2D::begin(float width, float height)
{
    size[0] = width, size[1] = height;
    batch.clear();
}

void Draw2D::rect(float x, float y, float w, float h, float radius, Color fill, float border, Color borderColor)
{
    batch.push_back({{x, y, w, h}, {}, {fill.r, fill.g, fill.b, fill.a},
                     {borderColor.r, borderColor.g, borderColor.b, borderColor.a}, {0, radius, border, 0}});
}

void Draw2D::circle(float cx, float cy, float radius, Color fill, float border, Color borderColor)
{
    rect(cx - radius, cy - radius, 2 * radius, 2 * radius, radius, fill, border, borderColor);
}

float Draw2D::measure(float textSize, std::string_view text) const
{
    float width = 0;
    for (char c : text)
        if (c >= ' ' && c <= '~') width += glyphs[c - ' '].advance;
    return width * textSize / bakeSize;
}

float Draw2D::text(float x, float y, float textSize, std::string_view text, Color color, Align align)
{
    const float width = measure(textSize, text);
    if (align == Align::center) x -= width / 2;
    if (align == Align::right) x -= width;
    const float k = textSize / bakeSize;
    for (char c : text)
    {
        if (c < ' ' || c > '~') continue;
        const Glyph &g = glyphs[c - ' '];
        if (g.w > 0)
            batch.push_back({{x + g.x * k, y + g.y * k, g.w * k, g.h * k}, {g.u0, g.v0, g.u1, g.v1},
                             {color.r, color.g, color.b, color.a}, {}, {1, 0, 0, 0}});
        x += g.advance * k;
    }
    return width;
}

void Draw2D::flush(const wgpu::RenderPassEncoder &pass)
{
    if (batch.empty() || !pipeline) return;
    const uint64_t bytes = batch.size() * sizeof(Instance);
    if (batch.size() > capacity)
    {
        capacity = std::max<size_t>(batch.size(), 2 * capacity);
        wgpu::BufferDescriptor descriptor;
        descriptor.size = capacity * sizeof(Instance);
        descriptor.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        instances = device.CreateBuffer(&descriptor);
    }
    const float data[4] = {size[0], size[1], 0, 0};
    device.GetQueue().WriteBuffer(uniforms, 0, data, sizeof(data));
    device.GetQueue().WriteBuffer(instances, 0, batch.data(), bytes);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.SetVertexBuffer(0, instances, 0, bytes);
    pass.Draw(4, uint32_t(batch.size()));
}

}

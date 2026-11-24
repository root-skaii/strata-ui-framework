// Apple Metal renderer backend (Objective-C++): ports strata/shaders/ui.hlsl to MSL.

#include "strata/backend/metal.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace strata {

namespace {

constexpr const char* kMslSource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct Transform {
    float4 scale_translate;
    float4 output_params;
    float4 view_origin;
};

struct Shape {
    float2 center;
    float2 half_size;
    float4 radius;
    uint   fill_top;
    uint   fill_bottom;
    uint   border_color;
    uint   shadow_color;
    float  border_width;
    float  shadow_blur;
    float2 shadow_offset;
    float2 gradient_dir;
    float  gradient_kind;
    float  extra;
};

struct VsIn {
    float2 pos [[attribute(0)]];
    ushort2 uv [[attribute(1)]];
    float4 col [[attribute(2)]];
};

struct VsOut {
    float4 pos [[position]];
    float2 uv;
    float4 col;
    uint   shape_idx [[flat]];
};

constant uint NO_SHAPE = 0xffffffffu;

vertex VsOut vs_main(VsIn i [[stage_in]], constant Transform& u [[buffer(1)]]) {
    VsOut o;
    o.pos = float4(i.pos * u.scale_translate.xy + u.scale_translate.zw, 0.0f, 1.0f);
    o.col = i.col;
    if ((i.uv.y & 0x8000u) != 0u) {
        o.uv        = float2(0.0f);
        o.shape_idx = uint(i.uv.x) | ((uint(i.uv.y) & 0x7fffu) << 16u);
    } else {
        o.uv        = float2(i.uv) * (1.0f / 32767.0f);
        o.shape_idx = NO_SHAPE;
    }
    return o;
}

static inline float4 unpack_color(uint c) {
    return float4(float(c & 255u), float((c >> 8u) & 255u), float((c >> 16u) & 255u), float(c >> 24u)) * (1.0f / 255.0f);
}

static inline float3 srgb_to_linear(float3 c) {
    return select(pow((c + 0.055f) / 1.055f, 2.4f), c / 12.92f, c <= 0.04045f);
}

static inline float4 encode_output(float4 premul, float4 out_p) {
    if (out_p.x < 0.5f || premul.a <= 0.0f) { return premul; }
    float3 l = srgb_to_linear(saturate(premul.rgb / premul.a));
    if (out_p.x >= 1.5f) { l *= out_p.y; }
    return float4(l * premul.a, premul.a);
}

static inline float sd_round_box(float2 p, float2 half_size, float4 r) {
    float rr = p.x > 0.0f ? (p.y > 0.0f ? r.z : r.y) : (p.y > 0.0f ? r.w : r.x);
    float2 q = abs(p) - half_size + rr;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - rr;
}

static inline float4 shade_shape(Shape s, float2 pixel) {
    float2 p = pixel - s.center;
    float  d = sd_round_box(p, s.half_size, s.radius);
    float outer = saturate(0.5f - d);
    float inner = s.border_width > 0.0f ? saturate(0.5f - (d + s.border_width)) : outer;

    float4 fill = unpack_color(s.fill_top);
    if (s.fill_top != s.fill_bottom) {
        float extent = max(abs(s.gradient_dir.x) * 2.0f * s.half_size.x + abs(s.gradient_dir.y) * 2.0f * s.half_size.y, 1.0e-3f);
        float t = s.gradient_kind > 0.5f ? saturate(length(p / max(s.half_size, 1.0e-3f)))
                                         : saturate(dot(p, s.gradient_dir) / extent + 0.5f);
        fill = mix(fill, unpack_color(s.fill_bottom), t);
    }
    float4 res = float4(fill.rgb * fill.a, fill.a) * inner;
    if (s.border_width > 0.0f) {
        float4 edge = unpack_color(s.border_color);
        res += float4(edge.rgb * edge.a, edge.a) * max(outer - inner, 0.0f);
    }
    float4 sh = unpack_color(s.shadow_color);
    if (sh.a > 0.0f && s.shadow_blur > 0.0f) {
        float ds = sd_round_box(p - s.shadow_offset, s.half_size, s.radius);
        float k  = 1.0f - smoothstep(-s.shadow_blur, s.shadow_blur, ds);
        float sa = sh.a * k * (1.0f - outer);
        res += float4(sh.rgb * sa, sa) * (1.0f - res.a);
    }
    return res;
}

fragment float4 ps_main(VsOut i [[stage_in]],
                        constant Transform& u [[buffer(1)]],
                        const device Shape* shapes [[buffer(2)]],
                        texture2d<float> atlas [[texture(0)]],
                        sampler samp [[sampler(0)]]) {
    if (i.shape_idx != NO_SHAPE) {
        return encode_output(shade_shape(shapes[i.shape_idx], i.pos.xy + u.view_origin.xy) * i.col.a, u.output_params);
    }
    float cov = atlas.sample(samp, i.uv).r;
    float tc  = u.output_params.z;
    if (tc < 0.0f) {
        float w = max(fwidth(cov), 0.02f);
        cov = smoothstep(0.5f - w, 0.5f + w, cov);
    } else if (tc > 0.0f && cov > 0.0f && cov < 1.0f) {
        float luma = 0.2126f * i.col.r + 0.7152f * i.col.g + 0.0722f * i.col.b;
        cov = pow(cov, mix(1.0f + tc, 1.0f / (1.0f + tc), luma));
    }
    float a = i.col.a * cov;
    return encode_output(float4(i.col.rgb * a, a), u.output_params);
}

fragment float4 ps_image(VsOut i [[stage_in]],
                         constant Transform& u [[buffer(1)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
    float4 t = tex.sample(samp, i.uv) * i.col;
    return encode_output(float4(t.rgb * t.a, t.a), u.output_params);
}
)msl";

struct alignas(16) mtl_xform {
    f32 scale[2]{};
    f32 translate[2]{};
    f32 output[4]{};
    f32 view[4]{};
};

[[nodiscard]] MTLPixelFormat to_mtl_format(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return MTLPixelFormatBGRA8Unorm;
    case texture_layout::rgba16f: return MTLPixelFormatRGBA16Float;
    default:                      return MTLPixelFormatRGBA8Unorm;
    }
}

} // namespace

struct metal_renderer::impl {
    id<MTLDevice>              device = nil;
    id<MTLRenderPipelineState> pipe_main = nil;
    id<MTLRenderPipelineState> pipe_image = nil;
    id<MTLSamplerState>        sampler = nil;
    id<MTLTexture>             atlas_tex = nil;
    u32                        atlas_w{};
    u32                        atlas_h{};
    output_desc                output{};

    struct gpu_tex {
        id<MTLTexture> tex = nil;
        texture_image  cpu;
        bool           owned{};
        bool           updatable{};
        bool           in_use{};
    };
    std::vector<gpu_tex> textures;

    bool upload_atlas(const font_atlas& atlas)
    {
        const u32 w = atlas.width(), h = atlas.height();
        if (w == 0 || h == 0 || atlas.pixels().empty()) { return false; }
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        atlas_tex = [device newTextureWithDescriptor:td];
        if (atlas_tex == nil) { return false; }
        [atlas_tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                     mipmapLevel:0
                       withBytes:atlas.pixels().data()
                     bytesPerRow:w];
        atlas_w = w;
        atlas_h = h;
        return true;
    }
};

metal_renderer::metal_renderer() noexcept = default;
metal_renderer::~metal_renderer() { destroy(); }
metal_renderer::metal_renderer(metal_renderer&&) noexcept = default;
metal_renderer& metal_renderer::operator=(metal_renderer&&) noexcept = default;

bool metal_renderer::create(const metal_init_info& info, const font_atlas& atlas)
{
    destroy();
    if (info.device == nullptr) { return false; }
    auto p = std::make_unique<impl>();
    p->device = (__bridge id<MTLDevice>)info.device;

    NSError* err = nil;
    NSString* src = [NSString stringWithUTF8String:kMslSource];
    id<MTLLibrary> lib = [p->device newLibraryWithSource:src options:nil error:&err];
    if (lib == nil) { return false; }

    id<MTLFunction> vs     = [lib newFunctionWithName:@"vs_main"];
    id<MTLFunction> fs     = [lib newFunctionWithName:@"ps_main"];
    id<MTLFunction> fs_img = [lib newFunctionWithName:@"ps_image"];

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format      = MTLVertexFormatFloat2;
    vd.attributes[0].offset      = offsetof(vertex, pos);
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format      = MTLVertexFormatUShort2;
    vd.attributes[1].offset      = offsetof(vertex, u);
    vd.attributes[1].bufferIndex = 0;
    vd.attributes[2].format      = MTLVertexFormatUChar4Normalized;
    vd.attributes[2].offset      = offsetof(vertex, col);
    vd.attributes[2].bufferIndex = 0;
    vd.layouts[0].stride         = sizeof(vertex);

    MTLRenderPipelineDescriptor* rpd = [[MTLRenderPipelineDescriptor alloc] init];
    rpd.vertexFunction   = vs;
    rpd.fragmentFunction = fs;
    rpd.vertexDescriptor = vd;
    rpd.sampleCount      = std::max(info.sample_count, 1u);
    rpd.colorAttachments[0].pixelFormat                 = static_cast<MTLPixelFormat>(info.pixel_format);
    rpd.colorAttachments[0].blendingEnabled             = YES;
    rpd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
    rpd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
    rpd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
    rpd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
    rpd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
    rpd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    p->pipe_main = [p->device newRenderPipelineStateWithDescriptor:rpd error:&err];
    rpd.fragmentFunction = fs_img;
    p->pipe_image = [p->device newRenderPipelineStateWithDescriptor:rpd error:&err];

    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter     = MTLSamplerMinMagFilterLinear;
    sd.magFilter     = MTLSamplerMinMagFilterLinear;
    sd.mipFilter     = MTLSamplerMipFilterLinear;
    sd.sAddressMode  = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode  = MTLSamplerAddressModeClampToEdge;
    p->sampler = [p->device newSamplerStateWithDescriptor:sd];

    if (p->pipe_main == nil || p->pipe_image == nil || p->sampler == nil || !p->upload_atlas(atlas)) {
        return false;
    }
    impl_ = std::move(p);
    return true;
}

void metal_renderer::destroy() noexcept { impl_.reset(); }

void metal_renderer::render(const draw_data& data, void* render_encoder)
{
    if (!impl_ || render_encoder == nullptr || data.vertices.empty() || data.indices.empty() ||
        data.display_size.x <= 0.0f || data.display_size.y <= 0.0f) {
        return;
    }
    impl& im = *impl_;
    id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)render_encoder;

    id<MTLBuffer> vbuf = [im.device newBufferWithBytes:data.vertices.data()
                                                length:data.vertices.size() * sizeof(vertex)
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> ibuf = [im.device newBufferWithBytes:data.indices.data()
                                                length:data.indices.size() * sizeof(u16)
                                               options:MTLResourceStorageModeShared];
    const shape_record dummy_shape{};
    id<MTLBuffer> sbuf = [im.device newBufferWithBytes:data.shapes.empty() ? &dummy_shape : data.shapes.data()
                                                length:std::max<std::size_t>(data.shapes.size(), 1u) * sizeof(shape_record)
                                               options:MTLResourceStorageModeShared];

    mtl_xform u{};
    u.scale[0]     = 2.0f / data.display_size.x;
    u.scale[1]     = -2.0f / data.display_size.y;
    u.translate[0] = -1.0f - data.origin.x * u.scale[0];
    u.translate[1] = 1.0f - data.origin.y * u.scale[1];
    u.output[0]    = static_cast<f32>(im.output.space);
    u.output[1]    = im.output.paper_white_nits / 80.0f;
    u.output[2]    = data.text_contrast;
    u.view[0]      = data.origin.x;
    u.view[1]      = data.origin.y;

    const u32 dw = static_cast<u32>(std::max(data.display_size.x, 1.0f));
    const u32 dh = static_cast<u32>(std::max(data.display_size.y, 1.0f));
    [enc setViewport:MTLViewport{0.0, 0.0, static_cast<double>(dw), static_cast<double>(dh), 0.0, 1.0}];
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBuffer:sbuf offset:0 atIndex:2];
    [enc setFragmentSamplerState:im.sampler atIndex:0];

    bool using_image = false;
    bool pipe_bound  = false;
    for (const draw_cmd& cmd : data.commands) {
        if (cmd.idx_count == 0) { continue; }
        const i32 x0 = std::clamp(static_cast<i32>(std::floor(cmd.clip.min.x)), 0, static_cast<i32>(dw));
        const i32 y0 = std::clamp(static_cast<i32>(std::floor(cmd.clip.min.y)), 0, static_cast<i32>(dh));
        const i32 x1 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.max.x)), 0, static_cast<i32>(dw));
        const i32 y1 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.max.y)), 0, static_cast<i32>(dh));
        if (x1 <= x0 || y1 <= y0) { continue; }
        [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
                                           static_cast<NSUInteger>(x1 - x0), static_cast<NSUInteger>(y1 - y0)}];

        const bool want_image = cmd.texture != 0 && cmd.texture <= im.textures.size() && im.textures[cmd.texture - 1].in_use;
        if (!pipe_bound || want_image != using_image) {
            [enc setRenderPipelineState:want_image ? im.pipe_image : im.pipe_main];
            if (!want_image) { [enc setFragmentTexture:im.atlas_tex atIndex:0]; }
            using_image = want_image;
            pipe_bound  = true;
        }
        if (want_image) {
            [enc setFragmentTexture:im.textures[cmd.texture - 1].tex atIndex:0];
        }
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:cmd.idx_count
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:ibuf
                 indexBufferOffset:cmd.idx_offset * sizeof(u16)
                     instanceCount:1
                        baseVertex:static_cast<NSInteger>(cmd.vtx_offset)
                      baseInstance:0];
    }
}

bool metal_renderer::update_atlas(const font_atlas& atlas) { return impl_ && impl_->upload_atlas(atlas); }

bool metal_renderer::update_atlas_region(const font_atlas& atlas, atlas_region region)
{
    if (!impl_ || region.empty() || impl_->atlas_tex == nil) { return false; }
    if (atlas.width() != impl_->atlas_w || atlas.height() != impl_->atlas_h) { return update_atlas(atlas); }
    const u8* src = atlas.pixels().data() + static_cast<std::size_t>(region.y) * atlas.width() + region.x;
    [impl_->atlas_tex replaceRegion:MTLRegionMake2D(region.x, region.y, region.w, region.h)
                        mipmapLevel:0
                          withBytes:src
                        bytesPerRow:atlas.width()];
    return true;
}

void        metal_renderer::set_output(const output_desc& output) noexcept { if (impl_) { impl_->output = output; } }
output_desc metal_renderer::output() const noexcept { return impl_ ? impl_->output : output_desc{}; }

texture_id metal_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id metal_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (!impl_) { return 0; }
    impl::gpu_tex gt;
    if (!gt.cpu.create(desc, pixels)) { return 0; }
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:to_mtl_format(gt.cpu.layout())
                                                                                  width:gt.cpu.width(0)
                                                                                 height:gt.cpu.height(0)
                                                                              mipmapped:(gt.cpu.mip_count() > 1)];
    td.mipmapLevelCount = gt.cpu.mip_count();
    td.usage            = MTLTextureUsageShaderRead;
    gt.tex = [impl_->device newTextureWithDescriptor:td];
    if (gt.tex == nil) { return 0; }
    gt.owned     = true;
    gt.updatable = desc.updatable;
    gt.in_use    = true;
    const u32 bpp = texture_layout_bytes(gt.cpu.layout());
    for (u32 m = 0; m < gt.cpu.mip_count(); ++m) {
        [gt.tex replaceRegion:MTLRegionMake2D(0, 0, gt.cpu.width(m), gt.cpu.height(m))
                  mipmapLevel:m
                    withBytes:gt.cpu.pixels(m).data()
                  bytesPerRow:gt.cpu.width(m) * bpp];
    }
    for (std::size_t i = 0; i < impl_->textures.size(); ++i) {
        if (!impl_->textures[i].in_use) { impl_->textures[i] = std::move(gt); return static_cast<texture_id>(i + 1); }
    }
    impl_->textures.push_back(std::move(gt));
    return static_cast<texture_id>(impl_->textures.size());
}

bool metal_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return false; }
    impl::gpu_tex& gt = impl_->textures[id - 1];
    if (!gt.in_use || !gt.updatable || !gt.owned) { return false; }
    std::vector<texture_image::region> dirty;
    if (!gt.cpu.update(x, y, width, height, pixels, dirty)) { return false; }
    const u32 bpp = texture_layout_bytes(gt.cpu.layout());
    for (const auto& r : dirty) {
        const u8* src = gt.cpu.pixels(r.mip).data() + (static_cast<std::size_t>(r.y) * gt.cpu.width(r.mip) + r.x) * bpp;
        [gt.tex replaceRegion:MTLRegionMake2D(r.x, r.y, r.w, r.h)
                  mipmapLevel:r.mip
                    withBytes:src
                  bytesPerRow:gt.cpu.width(r.mip) * bpp];
    }
    return true;
}

texture_id metal_renderer::register_texture(void* mtl_texture)
{
    if (!impl_ || mtl_texture == nullptr) { return 0; }
    impl::gpu_tex gt;
    gt.tex    = (__bridge id<MTLTexture>)mtl_texture;
    gt.owned  = false;
    gt.in_use = true;
    for (std::size_t i = 0; i < impl_->textures.size(); ++i) {
        if (!impl_->textures[i].in_use) { impl_->textures[i] = std::move(gt); return static_cast<texture_id>(i + 1); }
    }
    impl_->textures.push_back(std::move(gt));
    return static_cast<texture_id>(impl_->textures.size());
}

void metal_renderer::destroy_texture(texture_id id) noexcept
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return; }
    impl_->textures[id - 1] = {};
}

} // namespace strata

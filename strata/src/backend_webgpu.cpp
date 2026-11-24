// WebGPU renderer backend (Dawn, wgpu-native, Emscripten emdawnwebgpu): ports strata/shaders/ui.hlsl to WGSL.

#include "strata/backend/webgpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace strata {

namespace {

constexpr const char* kWgslSource = R"wgsl(
struct Transform {
    scale_translate : vec4<f32>, // xy = scale, zw = translate
    output_params   : vec4<f32>, // x = space, y = white, z = text_contrast, w = unused
    view_origin     : vec4<f32>, // xy = target origin in ui px
};

struct Shape {
    center        : vec2<f32>,
    half_size     : vec2<f32>,
    radius        : vec4<f32>,
    fill_top      : u32,
    fill_bottom   : u32,
    border_color  : u32,
    shadow_color  : u32,
    border_width  : f32,
    shadow_blur   : f32,
    shadow_offset : vec2<f32>,
    gradient_dir  : vec2<f32>,
    gradient_kind : f32,
    extra         : f32,
};

@group(0) @binding(0) var<uniform> u_xform : Transform;
@group(0) @binding(1) var<storage, read> u_shapes : array<Shape>;
@group(0) @binding(2) var u_atlas : texture_2d<f32>;
@group(0) @binding(3) var u_sampler : sampler;
@group(1) @binding(0) var u_tex : texture_2d<f32>;

struct VsIn {
    @location(0) pos : vec2<f32>,
    @location(1) uv  : u32,       // low 16 = u, high 16 = v
    @location(2) col : vec4<f32>, // unorm8x4
};

struct VsOut {
    @builtin(position) pos : vec4<f32>,
    @location(0) uv        : vec2<f32>,
    @location(1) col       : vec4<f32>,
    @location(2) @interpolate(flat) shape_idx : u32,
};

const NO_SHAPE : u32 = 0xffffffffu;

@vertex
fn vs_main(i : VsIn) -> VsOut {
    var o : VsOut;
    o.pos = vec4<f32>(i.pos * u_xform.scale_translate.xy + u_xform.scale_translate.zw, 0.0, 1.0);
    o.col = i.col;
    let ux = i.uv & 0xffffu;
    let uy = (i.uv >> 16u) & 0xffffu;
    if ((uy & 0x8000u) != 0u) {
        o.uv = vec2<f32>(0.0, 0.0);
        o.shape_idx = ux | ((uy & 0x7fffu) << 16u);
    } else {
        o.uv = vec2<f32>(f32(ux), f32(uy)) * (1.0 / 32767.0);
        o.shape_idx = NO_SHAPE;
    }
    return o;
}

fn unpack_color(c : u32) -> vec4<f32> {
    return vec4<f32>(
        f32(c & 255u),
        f32((c >> 8u) & 255u),
        f32((c >> 16u) & 255u),
        f32((c >> 24u) & 255u)
    ) * (1.0 / 255.0);
}

fn srgb_to_linear(c : vec3<f32>) -> vec3<f32> {
    let lo = c / 12.92;
    let hi = pow((c + vec3<f32>(0.055)) / 1.055, vec3<f32>(2.4));
    return select(hi, lo, c <= vec3<f32>(0.04045));
}

fn pq_encode(l : vec3<f32>) -> vec3<f32> {
    let m1 = 0.1593017578125;
    let m2 = 78.84375;
    let c1 = 0.8359375;
    let c2 = 18.8515625;
    let c3 = 18.6875;
    let p = pow(clamp(l, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(m1));
    return pow((vec3<f32>(c1) + c2 * p) / (vec3<f32>(1.0) + c3 * p), vec3<f32>(m2));
}

fn encode_rgb(c : vec3<f32>) -> vec3<f32> {
    let sp = u_xform.output_params.x;
    if (sp < 0.5) { return c; }
    let l = srgb_to_linear(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)));
    if (sp < 1.5) { return l; }
    if (sp < 2.5) { return l * u_xform.output_params.y; }
    let r = 0.6274040 * l.r + 0.3292820 * l.g + 0.0433136 * l.b;
    let g = 0.0690970 * l.r + 0.9195400 * l.g + 0.0113612 * l.b;
    let b = 0.0163916 * l.r + 0.0880132 * l.g + 0.8955950 * l.b;
    return pq_encode(vec3<f32>(r, g, b) * u_xform.output_params.y);
}

fn encode_output(premul : vec4<f32>) -> vec4<f32> {
    if (u_xform.output_params.x < 0.5 || premul.a <= 0.0) { return premul; }
    return vec4<f32>(encode_rgb(premul.rgb / premul.a) * premul.a, premul.a);
}

fn sd_round_box(p : vec2<f32>, half_size : vec2<f32>, r : vec4<f32>) -> f32 {
    var rr : f32;
    if (p.x > 0.0) {
        rr = select(r.y, r.z, p.y > 0.0);
    } else {
        rr = select(r.x, r.w, p.y > 0.0);
    }
    let q = abs(p) - half_size + vec2<f32>(rr);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0))) - rr;
}

fn shade_shape(s : Shape, pixel : vec2<f32>) -> vec4<f32> {
    let p = pixel - s.center;
    let d = sd_round_box(p, s.half_size, s.radius);
    let outer = clamp(0.5 - d, 0.0, 1.0);
    let inner = select(outer, clamp(0.5 - (d + s.border_width), 0.0, 1.0), s.border_width > 0.0);

    var fill = unpack_color(s.fill_top);
    if (s.fill_top != s.fill_bottom) {
        let extent = max(abs(s.gradient_dir.x) * 2.0 * s.half_size.x + abs(s.gradient_dir.y) * 2.0 * s.half_size.y, 1.0e-3);
        let t = select(
            clamp(dot(p, s.gradient_dir) / extent + 0.5, 0.0, 1.0),
            clamp(length(p / max(s.half_size, vec2<f32>(1.0e-3))), 0.0, 1.0),
            s.gradient_kind > 0.5
        );
        fill = mix(fill, unpack_color(s.fill_bottom), t);
    }

    var res = vec4<f32>(fill.rgb * fill.a, fill.a) * inner;
    if (s.border_width > 0.0) {
        let edge = unpack_color(s.border_color);
        res = res + vec4<f32>(edge.rgb * edge.a, edge.a) * max(outer - inner, 0.0);
    }
    let sh = unpack_color(s.shadow_color);
    if (sh.a > 0.0 && s.shadow_blur > 0.0) {
        let ds = sd_round_box(p - s.shadow_offset, s.half_size, s.radius);
        let k  = 1.0 - smoothstep(-s.shadow_blur, s.shadow_blur, ds);
        let sa = sh.a * k * (1.0 - outer);
        res = res + vec4<f32>(sh.rgb * sa, sa) * (1.0 - res.a);
    }
    return res;
}

@fragment
fn ps_main(i : VsOut) -> @location(0) vec4<f32> {
    var cov = textureSampleLevel(u_atlas, u_sampler, i.uv, 0.0).r;
    let fw = fwidth(cov);
    if (i.shape_idx != NO_SHAPE) {
        let s = u_shapes[i.shape_idx];
        return encode_output(shade_shape(s, i.pos.xy + u_xform.view_origin.xy));
    }
    let tc = u_xform.output_params.z;
    if (tc < 0.0) {
        let w = max(fw, 0.02);
        cov = smoothstep(0.5 - w, 0.5 + w, cov);
    } else if (tc > 0.0) {
        let k = tc * dot(i.col.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
        cov = cov * (1.0 + k) / (1.0 + k * cov);
    }
    let a = i.col.a * cov;
    return encode_output(vec4<f32>(i.col.rgb * a, a));
}

@fragment
fn ps_image(i : VsOut) -> @location(0) vec4<f32> {
    let s = u_shapes[i.shape_idx];
    let p = i.pos.xy + u_xform.view_origin.xy - s.center;
    let cov = clamp(0.5 - sd_round_box(p, s.half_size, s.radius), 0.0, 1.0);
    let t = clamp(p / (2.0 * s.half_size) + vec2<f32>(0.5), vec2<f32>(0.0), vec2<f32>(1.0));
    let uv0 = s.shadow_offset;
    let uv1 = vec2<f32>(s.border_width, s.shadow_blur);
    let uv = mix(uv0, uv1, t);
    let dim = vec2<f32>(textureDimensions(u_tex));
    let tpp = abs(uv1 - uv0) * dim / max(2.0 * s.half_size, vec2<f32>(1.0));
    let lod = max(log2(max(max(tpp.x, tpp.y), 1.0e-4)), 0.0);
    let c = textureSampleLevel(u_tex, u_sampler, uv, lod) * i.col;
    let a = c.a * cov;
    return encode_output(vec4<f32>(c.rgb * a, a));
}
)wgsl";

struct alignas(16) xform_ubo {
    f32 scale[2]{};
    f32 translate[2]{};
    f32 output[4]{};
    f32 view[4]{};
};

[[nodiscard]] WGPUStringView wgpu_sv(const char* s) noexcept
{
    return WGPUStringView{s, WGPU_STRLEN};
}

[[nodiscard]] WGPUTextureFormat to_wgpu_format(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return WGPUTextureFormat_BGRA8Unorm;
    case texture_layout::rgba16f: return WGPUTextureFormat_RGBA16Float;
    default:                      return WGPUTextureFormat_RGBA8Unorm;
    }
}

[[nodiscard]] WGPUShaderModule create_shader(WGPUDevice dev, const char* wgsl) noexcept
{
    WGPUShaderSourceWGSL wgsl_src{};
    wgsl_src.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_src.code        = wgpu_sv(wgsl);
    WGPUShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl_src.chain;
    return wgpuDeviceCreateShaderModule(dev, &desc);
}

} // namespace

struct wgpu_renderer::impl {
    WGPUDevice        device{};
    WGPUQueue         queue{};
    WGPUTextureFormat color_format{WGPUTextureFormat_BGRA8Unorm};
    u32               sample_count{1};
    output_desc       output{};
    bool              lost{};

    WGPUShaderModule     shader{};
    WGPUSampler          sampler{};
    WGPUBindGroupLayout  bgl_frame{};
    WGPUBindGroupLayout  bgl_tex{};
    WGPUPipelineLayout   pl_main{};
    WGPUPipelineLayout   pl_image{};
    WGPURenderPipeline   pipe_main{};
    WGPURenderPipeline   pipe_image{};

    // ring of per-pass buffers so multiple render() calls before one submit do not overwrite each other
    static constexpr u32 kPassRing = 8;
    struct pass_slot {
        WGPUBuffer    ubo{};
        WGPUBuffer    vbuf{};
        std::size_t   vcap{};
        WGPUBuffer    ibuf{};
        std::size_t   icap{};
        WGPUBuffer    sbuf{};
        std::size_t   scap{};
        WGPUBindGroup bg_frame{};
        bool          bg_dirty{true};
    };
    std::array<pass_slot, kPassRing> passes{};
    u32                              pass_cursor{};
    std::vector<WGPUBuffer>          retired_buffers;

    WGPUTexture          atlas_tex{};
    WGPUTextureView      atlas_view{};
    u32                  atlas_w{};
    u32                  atlas_h{};

    struct gpu_tex {
        WGPUTexture     tex{};
        WGPUTextureView view{};
        WGPUBindGroup   bg{};
        texture_image   cpu;
        bool            owned{};
        bool            updatable{};
        bool            in_use{};
    };
    std::vector<gpu_tex> textures;

    void invalidate_all_bgs() noexcept
    {
        for (auto& ps : passes) {
            if (ps.bg_frame != nullptr) { wgpuBindGroupRelease(ps.bg_frame); ps.bg_frame = nullptr; }
            ps.bg_dirty = true;
        }
    }

    void flush_retired() noexcept
    {
        for (WGPUBuffer b : retired_buffers) {
            wgpuBufferDestroy(b);
            wgpuBufferRelease(b);
        }
        retired_buffers.clear();
    }

    void release_atlas() noexcept
    {
        invalidate_all_bgs();
        if (atlas_view != nullptr) { wgpuTextureViewRelease(atlas_view); atlas_view = nullptr; }
        if (atlas_tex != nullptr)  { wgpuTextureDestroy(atlas_tex); wgpuTextureRelease(atlas_tex); atlas_tex = nullptr; }
        atlas_w = atlas_h = 0;
    }

    void ensure_buffer(pass_slot& ps, WGPUBuffer& buf, std::size_t& cap, std::size_t need, WGPUBufferUsage usage)
    {
        need = std::max<std::size_t>(need, 64);
        if (buf != nullptr && cap >= need) { return; }
        if (buf != nullptr) {
            // defer destruction until a later pass cycle so earlier commands in this encoder remain valid
            retired_buffers.push_back(buf);
            buf = nullptr;
        }
        cap = std::max(need, cap * 2);
        cap = (cap + 15u) & ~std::size_t{15u};
        WGPUBufferDescriptor bd{};
        bd.size  = cap;
        bd.usage = usage | WGPUBufferUsage_CopyDst;
        buf = wgpuDeviceCreateBuffer(device, &bd);
        if (&buf == &ps.sbuf) {
            if (ps.bg_frame != nullptr) { wgpuBindGroupRelease(ps.bg_frame); ps.bg_frame = nullptr; }
            ps.bg_dirty = true;
        }
    }

    bool upload_atlas(const font_atlas& atlas)
    {
        const u32 w = atlas.width(), h = atlas.height();
        if (w == 0 || h == 0 || atlas.pixels().empty()) { return false; }
        release_atlas();

        WGPUTextureDescriptor td{};
        td.dimension     = WGPUTextureDimension_2D;
        td.size          = {w, h, 1};
        td.mipLevelCount = 1;
        td.sampleCount   = 1;
        td.format        = WGPUTextureFormat_R8Unorm;
        td.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        atlas_tex = wgpuDeviceCreateTexture(device, &td);
        if (atlas_tex == nullptr) { return false; }
        atlas_view = wgpuTextureCreateView(atlas_tex, nullptr);
        atlas_w = w;
        atlas_h = h;

        WGPUTexelCopyTextureInfo dst{};
        dst.texture = atlas_tex;
        dst.aspect  = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout lay{};
        lay.bytesPerRow  = w;
        lay.rowsPerImage = h;
        WGPUExtent3D ext{w, h, 1};
        wgpuQueueWriteTexture(queue, &dst, atlas.pixels().data(), atlas.pixels().size(), &lay, &ext);
        invalidate_all_bgs();
        return true;
    }

    void rebuild_frame_bg(pass_slot& ps)
    {
        if (!ps.bg_dirty && ps.bg_frame != nullptr) { return; }
        if (ps.bg_frame != nullptr) { wgpuBindGroupRelease(ps.bg_frame); ps.bg_frame = nullptr; }
        if (ps.ubo == nullptr || ps.sbuf == nullptr || atlas_view == nullptr || sampler == nullptr) { return; }
        std::array<WGPUBindGroupEntry, 4> e{};
        e[0].binding = 0; e[0].buffer = ps.ubo;  e[0].size = sizeof(xform_ubo);
        e[1].binding = 1; e[1].buffer = ps.sbuf; e[1].size = ps.scap;
        e[2].binding = 2; e[2].textureView = atlas_view;
        e[3].binding = 3; e[3].sampler = sampler;
        WGPUBindGroupDescriptor bgd{};
        bgd.layout     = bgl_frame;
        bgd.entryCount = static_cast<u32>(e.size());
        bgd.entries    = e.data();
        ps.bg_frame = wgpuDeviceCreateBindGroup(device, &bgd);
        ps.bg_dirty = false;
    }

    void destroy_all() noexcept
    {
        for (auto& t : textures) {
            if (!t.in_use) { continue; }
            if (t.bg != nullptr) { wgpuBindGroupRelease(t.bg); }
            if (t.owned && t.view != nullptr) { wgpuTextureViewRelease(t.view); }
            if (t.owned && t.tex != nullptr)  { wgpuTextureDestroy(t.tex); wgpuTextureRelease(t.tex); }
        }
        textures.clear();
        release_atlas();
        flush_retired();
        for (auto& ps : passes) {
            if (ps.bg_frame != nullptr) { wgpuBindGroupRelease(ps.bg_frame); ps.bg_frame = nullptr; }
            if (ps.vbuf != nullptr)     { wgpuBufferDestroy(ps.vbuf); wgpuBufferRelease(ps.vbuf); ps.vbuf = nullptr; }
            if (ps.ibuf != nullptr)     { wgpuBufferDestroy(ps.ibuf); wgpuBufferRelease(ps.ibuf); ps.ibuf = nullptr; }
            if (ps.sbuf != nullptr)     { wgpuBufferDestroy(ps.sbuf); wgpuBufferRelease(ps.sbuf); ps.sbuf = nullptr; }
            if (ps.ubo != nullptr)      { wgpuBufferDestroy(ps.ubo);  wgpuBufferRelease(ps.ubo);  ps.ubo = nullptr; }
        }
        if (pipe_main != nullptr)  { wgpuRenderPipelineRelease(pipe_main);  pipe_main = nullptr; }
        if (pipe_image != nullptr) { wgpuRenderPipelineRelease(pipe_image); pipe_image = nullptr; }
        if (pl_main != nullptr)    { wgpuPipelineLayoutRelease(pl_main);    pl_main = nullptr; }
        if (pl_image != nullptr)   { wgpuPipelineLayoutRelease(pl_image);   pl_image = nullptr; }
        if (bgl_frame != nullptr)  { wgpuBindGroupLayoutRelease(bgl_frame); bgl_frame = nullptr; }
        if (bgl_tex != nullptr)    { wgpuBindGroupLayoutRelease(bgl_tex);   bgl_tex = nullptr; }
        if (sampler != nullptr)    { wgpuSamplerRelease(sampler);           sampler = nullptr; }
        if (shader != nullptr)     { wgpuShaderModuleRelease(shader);       shader = nullptr; }
    }
};

wgpu_renderer::wgpu_renderer() noexcept = default;
wgpu_renderer::~wgpu_renderer() { destroy(); }
wgpu_renderer::wgpu_renderer(wgpu_renderer&&) noexcept = default;
wgpu_renderer& wgpu_renderer::operator=(wgpu_renderer&&) noexcept = default;

bool wgpu_renderer::create(const wgpu_init_info& info, const font_atlas& atlas)
{
    destroy();
    if (info.device == nullptr || info.queue == nullptr) { return false; }

    auto p = std::make_unique<impl>();
    p->device       = info.device;
    p->queue        = info.queue;
    p->color_format = info.color_format;
    p->sample_count = std::max(info.sample_count, 1u);

    p->shader = create_shader(p->device, kWgslSource);
    if (p->shader == nullptr) { return false; }

    WGPUSamplerDescriptor sd{};
    sd.addressModeU  = WGPUAddressMode_ClampToEdge;
    sd.addressModeV  = WGPUAddressMode_ClampToEdge;
    sd.addressModeW  = WGPUAddressMode_ClampToEdge;
    sd.magFilter     = WGPUFilterMode_Linear;
    sd.minFilter     = WGPUFilterMode_Linear;
    sd.mipmapFilter  = WGPUMipmapFilterMode_Linear;
    sd.lodMinClamp   = 0.0f;
    sd.lodMaxClamp   = 32.0f;
    sd.maxAnisotropy = 1;
    p->sampler = wgpuDeviceCreateSampler(p->device, &sd);

    std::array<WGPUBindGroupLayoutEntry, 4> fe{};
    fe[0].binding = 0; fe[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment; fe[0].buffer.type = WGPUBufferBindingType_Uniform;
    fe[1].binding = 1; fe[1].visibility = WGPUShaderStage_Fragment; fe[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    fe[2].binding = 2; fe[2].visibility = WGPUShaderStage_Fragment; fe[2].texture.sampleType = WGPUTextureSampleType_Float; fe[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    fe[3].binding = 3; fe[3].visibility = WGPUShaderStage_Fragment; fe[3].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor bgld{};
    bgld.entryCount = static_cast<u32>(fe.size());
    bgld.entries    = fe.data();
    p->bgl_frame = wgpuDeviceCreateBindGroupLayout(p->device, &bgld);

    WGPUBindGroupLayoutEntry te{};
    te.binding = 0; te.visibility = WGPUShaderStage_Fragment; te.texture.sampleType = WGPUTextureSampleType_Float; te.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor tbgld{};
    tbgld.entryCount = 1;
    tbgld.entries    = &te;
    p->bgl_tex = wgpuDeviceCreateBindGroupLayout(p->device, &tbgld);

    WGPUPipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts     = &p->bgl_frame;
    p->pl_main = wgpuDeviceCreatePipelineLayout(p->device, &pld);

    std::array<WGPUBindGroupLayout, 2> both_bgl{p->bgl_frame, p->bgl_tex};
    pld.bindGroupLayoutCount = 2;
    pld.bindGroupLayouts     = both_bgl.data();
    p->pl_image = wgpuDeviceCreatePipelineLayout(p->device, &pld);

    std::array<WGPUVertexAttribute, 3> attrs{};
    attrs[0].format = WGPUVertexFormat_Float32x2; attrs[0].offset = offsetof(vertex, pos); attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Uint32;    attrs[1].offset = offsetof(vertex, u);   attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Unorm8x4;  attrs[2].offset = offsetof(vertex, col); attrs[2].shaderLocation = 2;

    WGPUVertexBufferLayout vbl{};
    vbl.arrayStride    = sizeof(vertex);
    vbl.stepMode       = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = static_cast<u32>(attrs.size());
    vbl.attributes     = attrs.data();

    WGPUBlendState blend{};
    blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};

    WGPUColorTargetState target{};
    target.format    = p->color_format;
    target.blend     = &blend;
    target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState frag{};
    frag.module      = p->shader;
    frag.entryPoint  = wgpu_sv("ps_main");
    frag.targetCount = 1;
    frag.targets     = &target;

    WGPURenderPipelineDescriptor rpd{};
    rpd.layout             = p->pl_main;
    rpd.vertex.module      = p->shader;
    rpd.vertex.entryPoint  = wgpu_sv("vs_main");
    rpd.vertex.bufferCount = 1;
    rpd.vertex.buffers     = &vbl;
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.cullMode = WGPUCullMode_None;
    rpd.multisample.count  = p->sample_count;
    rpd.multisample.mask   = 0xffffffffu;
    rpd.fragment           = &frag;
    p->pipe_main = wgpuDeviceCreateRenderPipeline(p->device, &rpd);

    rpd.layout      = p->pl_image;
    frag.entryPoint = wgpu_sv("ps_image");
    p->pipe_image = wgpuDeviceCreateRenderPipeline(p->device, &rpd);

    WGPUBufferDescriptor ubd{};
    ubd.size  = sizeof(xform_ubo);
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    for (auto& ps : p->passes) {
        ps.ubo = wgpuDeviceCreateBuffer(p->device, &ubd);
        p->ensure_buffer(ps, ps.sbuf, ps.scap, sizeof(shape_record) * 16, WGPUBufferUsage_Storage);
        if (!ps.ubo || !ps.sbuf) {
            p->destroy_all();
            return false;
        }
    }

    if (!p->pipe_main || !p->pipe_image || !p->upload_atlas(atlas)) {
        p->destroy_all();
        return false;
    }
    p->rebuild_frame_bg(p->passes[0]);
    impl_ = std::move(p);
    return true;
}

void wgpu_renderer::destroy() noexcept
{
    if (impl_) {
        impl_->destroy_all();
        impl_.reset();
    }
}

void wgpu_renderer::render(const draw_data& data, WGPURenderPassEncoder pass)
{
    if (!impl_ || pass == nullptr || data.vertices.empty() || data.indices.empty() ||
        data.display_size.x <= 0.0f || data.display_size.y <= 0.0f) {
        return;
    }
    impl& im = *impl_;
    if (im.pass_cursor == 0 && !im.retired_buffers.empty()) {
        im.flush_retired();
    }
    impl::pass_slot& ps = im.passes[im.pass_cursor];
    im.pass_cursor      = (im.pass_cursor + 1u) % impl::kPassRing;

    const std::size_t vbytes = data.vertices.size() * sizeof(vertex);
    const std::size_t ibytes = (data.indices.size() * sizeof(u16) + 3u) & ~std::size_t{3u};
    const std::size_t sbytes = std::max<std::size_t>(data.shapes.size(), 1u) * sizeof(shape_record);
    im.ensure_buffer(ps, ps.vbuf, ps.vcap, vbytes, WGPUBufferUsage_Vertex);
    im.ensure_buffer(ps, ps.ibuf, ps.icap, ibytes, WGPUBufferUsage_Index);
    im.ensure_buffer(ps, ps.sbuf, ps.scap, sbytes, WGPUBufferUsage_Storage);
    im.rebuild_frame_bg(ps);

    wgpuQueueWriteBuffer(im.queue, ps.vbuf, 0, data.vertices.data(), vbytes);
    const std::size_t raw_ibytes = data.indices.size() * sizeof(u16);
    if ((raw_ibytes & 3u) == 0u) {
        wgpuQueueWriteBuffer(im.queue, ps.ibuf, 0, data.indices.data(), raw_ibytes);
    } else {
        const std::size_t aligned = raw_ibytes & ~std::size_t{3u};
        if (aligned > 0) { wgpuQueueWriteBuffer(im.queue, ps.ibuf, 0, data.indices.data(), aligned); }
        const u16 tail[2] = {data.indices.back(), 0};
        wgpuQueueWriteBuffer(im.queue, ps.ibuf, aligned, tail, sizeof(tail));
    }
    if (!data.shapes.empty()) {
        wgpuQueueWriteBuffer(im.queue, ps.sbuf, 0, data.shapes.data(), data.shapes.size() * sizeof(shape_record));
    }

    xform_ubo u{};
    u.scale[0]     = 2.0f / data.display_size.x;
    u.scale[1]     = -2.0f / data.display_size.y;
    u.translate[0] = -1.0f - data.origin.x * u.scale[0];
    u.translate[1] = 1.0f - data.origin.y * u.scale[1];
    u.output[0]    = static_cast<f32>(im.output.space);
    u.output[1]    = im.output.space == output_space::hdr10 ? im.output.paper_white_nits / 10000.0f
                                                            : im.output.paper_white_nits / 80.0f;
    u.output[2]    = data.text_contrast;
    u.view[0]      = data.origin.x;
    u.view[1]      = data.origin.y;
    wgpuQueueWriteBuffer(im.queue, ps.ubo, 0, &u, sizeof u);

    const u32 dw = static_cast<u32>(std::max(data.display_size.x, 1.0f));
    const u32 dh = static_cast<u32>(std::max(data.display_size.y, 1.0f));
    wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, static_cast<f32>(dw), static_cast<f32>(dh), 0.0f, 1.0f);
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, ps.vbuf, 0, vbytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, ps.ibuf, WGPUIndexFormat_Uint16, 0, ibytes);

    WGPURenderPipeline cur_pipe = nullptr;
    for (const draw_cmd& cmd : data.commands) {
        if (cmd.idx_count == 0) { continue; }
        const i32 x0 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.min.x - data.origin.x - 0.5f)), 0, static_cast<i32>(dw));
        const i32 y0 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.min.y - data.origin.y - 0.5f)), 0, static_cast<i32>(dh));
        const i32 x1 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.max.x - data.origin.x - 0.5f)), 0, static_cast<i32>(dw));
        const i32 y1 = std::clamp(static_cast<i32>(std::ceil(cmd.clip.max.y - data.origin.y - 0.5f)), 0, static_cast<i32>(dh));
        if (x1 <= x0 || y1 <= y0) { continue; }

        wgpuRenderPassEncoderSetScissorRect(pass, static_cast<u32>(x0), static_cast<u32>(y0),
                                            static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0));
        if (cmd.texture != 0 && cmd.texture <= im.textures.size() && im.textures[cmd.texture - 1].in_use) {
            if (cur_pipe != im.pipe_image) {
                wgpuRenderPassEncoderSetPipeline(pass, im.pipe_image);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, ps.bg_frame, 0, nullptr);
                cur_pipe = im.pipe_image;
            }
            wgpuRenderPassEncoderSetBindGroup(pass, 1, im.textures[cmd.texture - 1].bg, 0, nullptr);
        } else {
            if (cur_pipe != im.pipe_main) {
                wgpuRenderPassEncoderSetPipeline(pass, im.pipe_main);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, ps.bg_frame, 0, nullptr);
                cur_pipe = im.pipe_main;
            }
        }
        wgpuRenderPassEncoderDrawIndexed(pass, cmd.idx_count, 1, cmd.idx_offset, static_cast<i32>(cmd.vtx_offset), 0);
    }
}

void wgpu_renderer::render(const draw_data& data, WGPUCommandEncoder encoder, WGPUTextureView target_view,
                           WGPUTexture, bool clear, color clear_col)
{
    if (!impl_ || encoder == nullptr || target_view == nullptr) { return; }
    WGPURenderPassColorAttachment ca{};
    ca.view       = target_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp     = clear ? WGPULoadOp_Clear : WGPULoadOp_Load;
    ca.storeOp    = WGPUStoreOp_Store;
    ca.clearValue = {clear_col.r / 255.0, clear_col.g / 255.0, clear_col.b / 255.0, clear_col.a / 255.0};
    WGPURenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments     = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    render(data, pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

bool wgpu_renderer::update_atlas(const font_atlas& atlas)
{
    return impl_ && impl_->upload_atlas(atlas);
}

bool wgpu_renderer::update_atlas_region(const font_atlas& atlas, atlas_region region)
{
    if (!impl_ || region.empty() || impl_->atlas_tex == nullptr) { return false; }
    if (atlas.width() != impl_->atlas_w || atlas.height() != impl_->atlas_h) { return update_atlas(atlas); }
    WGPUTexelCopyTextureInfo dst{};
    dst.texture = impl_->atlas_tex;
    dst.origin  = {region.x, region.y, 0};
    dst.aspect  = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout lay{};
    lay.offset       = static_cast<u64>(region.y) * atlas.width() + region.x;
    lay.bytesPerRow  = atlas.width();
    lay.rowsPerImage = atlas.height();
    WGPUExtent3D ext{region.w, region.h, 1};
    wgpuQueueWriteTexture(impl_->queue, &dst, atlas.pixels().data(), atlas.pixels().size(), &lay, &ext);
    return true;
}

void        wgpu_renderer::set_output(const output_desc& output) noexcept { if (impl_) { impl_->output = output; } }
output_desc wgpu_renderer::output() const noexcept { return impl_ ? impl_->output : output_desc{}; }
bool        wgpu_renderer::device_lost() const noexcept { return impl_ && impl_->lost; }

texture_id wgpu_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id wgpu_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (!impl_) { return 0; }
    impl::gpu_tex gt;
    if (!gt.cpu.create(desc, pixels)) { return 0; }

    WGPUTextureDescriptor td{};
    td.dimension     = WGPUTextureDimension_2D;
    td.size          = {gt.cpu.width(0), gt.cpu.height(0), 1};
    td.mipLevelCount = gt.cpu.level_count();
    td.sampleCount   = 1;
    td.format        = to_wgpu_format(gt.cpu.layout());
    td.usage         = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    gt.tex = wgpuDeviceCreateTexture(impl_->device, &td);
    if (gt.tex == nullptr) { return 0; }
    gt.view      = wgpuTextureCreateView(gt.tex, nullptr);
    gt.owned     = true;
    gt.updatable = desc.updatable;
    gt.in_use    = true;

    const u32 bpp = texture_layout_bytes(gt.cpu.layout());
    for (u32 m = 0; m < gt.cpu.level_count(); ++m) {
        const auto mpx = gt.cpu.pixels(m);
        WGPUTexelCopyTextureInfo dst{};
        dst.texture  = gt.tex;
        dst.mipLevel = m;
        dst.aspect   = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout lay{};
        lay.bytesPerRow  = gt.cpu.width(m) * bpp;
        lay.rowsPerImage = gt.cpu.height(m);
        WGPUExtent3D ext{gt.cpu.width(m), gt.cpu.height(m), 1};
        wgpuQueueWriteTexture(impl_->queue, &dst, mpx.data(), mpx.size(), &lay, &ext);
    }

    WGPUBindGroupEntry e{};
    e.binding     = 0;
    e.textureView = gt.view;
    WGPUBindGroupDescriptor bgd{};
    bgd.layout     = impl_->bgl_tex;
    bgd.entryCount = 1;
    bgd.entries    = &e;
    gt.bg = wgpuDeviceCreateBindGroup(impl_->device, &bgd);

    for (std::size_t i = 0; i < impl_->textures.size(); ++i) {
        if (!impl_->textures[i].in_use) {
            impl_->textures[i] = std::move(gt);
            return static_cast<texture_id>(i + 1);
        }
    }
    impl_->textures.push_back(std::move(gt));
    return static_cast<texture_id>(impl_->textures.size());
}

bool wgpu_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return false; }
    impl::gpu_tex& gt = impl_->textures[id - 1];
    if (!gt.in_use || !gt.updatable || !gt.owned) { return false; }
    std::vector<texture_image::region> dirty;
    if (!gt.cpu.update(x, y, width, height, pixels, dirty)) { return false; }
    const u32 bpp = texture_layout_bytes(gt.cpu.layout());
    for (const auto& r : dirty) {
        const auto mpx = gt.cpu.pixels(r.level);
        WGPUTexelCopyTextureInfo dst{};
        dst.texture  = gt.tex;
        dst.mipLevel = r.level;
        dst.origin   = {r.x, r.y, 0};
        dst.aspect   = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout lay{};
        lay.offset       = (static_cast<u64>(r.y) * gt.cpu.width(r.level) + r.x) * bpp;
        lay.bytesPerRow  = gt.cpu.width(r.level) * bpp;
        lay.rowsPerImage = gt.cpu.height(r.level);
        WGPUExtent3D ext{r.w, r.h, 1};
        wgpuQueueWriteTexture(impl_->queue, &dst, mpx.data(), mpx.size(), &lay, &ext);
    }
    return true;
}

texture_id wgpu_renderer::register_texture(WGPUTextureView view)
{
    if (!impl_ || view == nullptr) { return 0; }
    impl::gpu_tex gt;
    gt.view   = view;
    gt.owned  = false;
    gt.in_use = true;
    WGPUBindGroupEntry e{};
    e.binding     = 0;
    e.textureView = view;
    WGPUBindGroupDescriptor bgd{};
    bgd.layout     = impl_->bgl_tex;
    bgd.entryCount = 1;
    bgd.entries    = &e;
    gt.bg = wgpuDeviceCreateBindGroup(impl_->device, &bgd);
    for (std::size_t i = 0; i < impl_->textures.size(); ++i) {
        if (!impl_->textures[i].in_use) {
            impl_->textures[i] = std::move(gt);
            return static_cast<texture_id>(i + 1);
        }
    }
    impl_->textures.push_back(std::move(gt));
    return static_cast<texture_id>(impl_->textures.size());
}

void wgpu_renderer::destroy_texture(texture_id id) noexcept
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return; }
    impl::gpu_tex& gt = impl_->textures[id - 1];
    if (!gt.in_use) { return; }
    if (gt.bg != nullptr) { wgpuBindGroupRelease(gt.bg); }
    if (gt.owned && gt.view != nullptr) { wgpuTextureViewRelease(gt.view); }
    if (gt.owned && gt.tex != nullptr)  { wgpuTextureDestroy(gt.tex); wgpuTextureRelease(gt.tex); }
    gt = {};
}

} // namespace strata

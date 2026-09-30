#include "strata/backend/d3d11.hpp"
#include "strata/backend/backend.hpp"

#include "blur_plan.hpp"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include "g_strata_ui_vs.h"
#include "g_strata_ui_ps.h"
#include "g_strata_ui_ps_image.h"
#include "g_strata_ui_ps_backdrop.h"
#include "g_strata_ui_vs_fullscreen.h"
#include "g_strata_ui_ps_blur_down.h"
#include "g_strata_ui_ps_blur_gauss.h"

namespace strata {

static_assert(renderer_backend<d3d11_renderer>);

using Microsoft::WRL::ComPtr;

namespace {

// snapshot of every stage render() overwrites; restored on scope exit
class state_guard {
public:
    explicit state_guard(ID3D11DeviceContext* ctx) noexcept : ctx_{ctx}
    {
        viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx_->RSGetViewports(&viewport_count_, viewports_);
        scissor_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx_->RSGetScissorRects(&scissor_count_, scissors_);
        ctx_->RSGetState(&raster_);
        ctx_->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx_->OMGetDepthStencilState(&depth_, &stencil_ref_);
        ctx_->VSGetShader(&vs_, nullptr, nullptr);
        ctx_->PSGetShader(&ps_, nullptr, nullptr);
        ctx_->GSGetShader(&gs_, nullptr, nullptr);
        ctx_->HSGetShader(&hs_, nullptr, nullptr);
        ctx_->DSGetShader(&ds_, nullptr, nullptr);
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
        ctx_->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
        for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT; ++i) { so_[i].Attach(so[i]); }
        ctx_->GetPredication(&predicate_, &predicate_value_);
        ctx_->VSGetConstantBuffers(0, 1, &vs_cb_);
        ID3D11ShaderResourceView* srv[4]{};
        ctx_->PSGetShaderResources(0, 4, srv);
        for (int i = 0; i < 4; ++i) { ps_srv_[i].Attach(srv[i]); }
        ctx_->PSGetConstantBuffers(0, 1, &ps_cb0_);
        ctx_->PSGetConstantBuffers(1, 1, &ps_cb_);
        ctx_->PSGetSamplers(0, 1, &ps_sampler_);
        ctx_->IAGetInputLayout(&layout_);
        ctx_->IAGetPrimitiveTopology(&topology_);
        ctx_->IAGetVertexBuffers(0, 1, &vb_, &vb_stride_, &vb_offset_);
        ctx_->IAGetIndexBuffer(&ib_, &ib_format_, &ib_offset_);
    }

    ~state_guard()
    {
        ctx_->RSSetViewports(viewport_count_, viewports_);
        ctx_->RSSetScissorRects(scissor_count_, scissors_);
        ctx_->RSSetState(raster_.Get());
        ctx_->OMSetBlendState(blend_.Get(), blend_factor_, sample_mask_);
        ctx_->OMSetDepthStencilState(depth_.Get(), stencil_ref_);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->GSSetShader(gs_.Get(), nullptr, 0);
        ctx_->HSSetShader(hs_.Get(), nullptr, 0);
        ctx_->DSSetShader(ds_.Get(), nullptr, 0);
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT]{};
        UINT so_offsets[D3D11_SO_BUFFER_SLOT_COUNT]{};
        for (UINT i = 0; i < D3D11_SO_BUFFER_SLOT_COUNT; ++i) { so[i] = so_[i].Get(); so_offsets[i] = static_cast<UINT>(-1); } // -1: append
        ctx_->SOSetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so, so_offsets);
        ctx_->SetPredication(predicate_.Get(), predicate_value_);
        ctx_->VSSetConstantBuffers(0, 1, vs_cb_.GetAddressOf());
        ID3D11ShaderResourceView* srv[4] = {ps_srv_[0].Get(), ps_srv_[1].Get(), ps_srv_[2].Get(), ps_srv_[3].Get()};
        ctx_->PSSetShaderResources(0, 4, srv);
        ctx_->PSSetConstantBuffers(0, 1, ps_cb0_.GetAddressOf());
        ctx_->PSSetConstantBuffers(1, 1, ps_cb_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, ps_sampler_.GetAddressOf());
        ctx_->IASetInputLayout(layout_.Get());
        ctx_->IASetPrimitiveTopology(topology_);
        ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &vb_stride_, &vb_offset_);
        ctx_->IASetIndexBuffer(ib_.Get(), ib_format_, ib_offset_);
    }

    state_guard(const state_guard&)            = delete;
    state_guard& operator=(const state_guard&) = delete;

private:
    ID3D11DeviceContext* ctx_;
    UINT                 viewport_count_{};
    UINT                 scissor_count_{};
    D3D11_VIEWPORT       viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT           scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ComPtr<ID3D11RasterizerState>   raster_;
    ComPtr<ID3D11BlendState>        blend_;
    FLOAT                           blend_factor_[4]{};
    UINT                            sample_mask_{};
    ComPtr<ID3D11DepthStencilState> depth_;
    UINT                            stencil_ref_{};
    ComPtr<ID3D11VertexShader>      vs_;
    ComPtr<ID3D11PixelShader>       ps_;
    ComPtr<ID3D11GeometryShader>    gs_;
    ComPtr<ID3D11HullShader>        hs_;
    ComPtr<ID3D11DomainShader>      ds_;
    ComPtr<ID3D11Buffer>            so_[D3D11_SO_BUFFER_SLOT_COUNT];
    ComPtr<ID3D11Predicate>         predicate_;
    BOOL                            predicate_value_{};
    ComPtr<ID3D11Buffer>            vs_cb_;
    ComPtr<ID3D11ShaderResourceView> ps_srv_[4];
    ComPtr<ID3D11Buffer>            ps_cb0_;
    ComPtr<ID3D11Buffer>            ps_cb_;
    ComPtr<ID3D11SamplerState>      ps_sampler_;
    ComPtr<ID3D11InputLayout>       layout_;
    D3D11_PRIMITIVE_TOPOLOGY        topology_{};
    ComPtr<ID3D11Buffer>            vb_;
    UINT                            vb_stride_{};
    UINT                            vb_offset_{};
    ComPtr<ID3D11Buffer>            ib_;
    DXGI_FORMAT                     ib_format_{};
    UINT                            ib_offset_{};
};

} // namespace

struct d3d11_renderer::impl {
    ComPtr<ID3D11Device>             device;
    ComPtr<ID3D11DeviceContext>      context;
    ComPtr<ID3D11VertexShader>       vs;
    ComPtr<ID3D11PixelShader>        ps;
    ComPtr<ID3D11PixelShader>        ps_image;
    ComPtr<ID3D11InputLayout>        layout;
    ComPtr<ID3D11Buffer>             vb;
    ComPtr<ID3D11Buffer>             ib;
    ComPtr<ID3D11Buffer>             cb;
    ComPtr<ID3D11BlendState>         blend;
    ComPtr<ID3D11RasterizerState>    raster;
    ComPtr<ID3D11DepthStencilState>  depth;
    ComPtr<ID3D11SamplerState>       sampler;
    ComPtr<ID3D11ShaderResourceView> atlas_srv;
    ComPtr<ID3D11Buffer>             shape_buf;
    ComPtr<ID3D11ShaderResourceView> shape_srv;
    struct texture_slot {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11ShaderResourceView> srv; // null = free slot
        texture_image                    image; // kept (for update_texture) only when the texture is updatable
        bool                             updatable{};
    };
    std::vector<texture_slot> textures; // texture_id - 1

    // backdrop blur: frame copied to snap_tex, boxed down into blur_tex[0], blurred back and forth
    ComPtr<ID3D11VertexShader>       vs_fullscreen;
    ComPtr<ID3D11PixelShader>        ps_backdrop;
    ComPtr<ID3D11PixelShader>        ps_blur_down;
    ComPtr<ID3D11PixelShader>        ps_blur_gauss;
    ComPtr<ID3D11Buffer>             cb_blur;
    ComPtr<ID3D11BlendState>         blend_off;
    ComPtr<ID3D11Texture2D>          snap_tex;
    ComPtr<ID3D11ShaderResourceView> snap_srv;
    UINT                             snap_w{}, snap_h{};
    DXGI_FORMAT                      snap_format{};
    ComPtr<ID3D11Texture2D>          blur_tex[2];
    ComPtr<ID3D11RenderTargetView>   blur_rtv[2];
    ComPtr<ID3D11ShaderResourceView> blur_srv[2];
    UINT                             blur_w{}, blur_h{};
    DXGI_FORMAT                      blur_format{};
    bool                             blur_supported{};
    output_desc                      output{};
    internal::ui_constants           cb_written{}; // what cb holds, so it is only mapped when something changed
    bool                             cb_valid{};

    struct blur_constants {
        float bp0[4]{};
        float bp1[4]{};
        float bp2[4]{};
        float bp3[4]{};
        float bp4[4]{};
    };

    void set_blur_constants(const blur_constants& c) noexcept
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(cb_blur.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            context->Unmap(cb_blur.Get(), 0);
        }
    }

    // (re)creates the snapshot and both low-res blur targets for this target size / format
    [[nodiscard]] bool ensure_blur_targets(ID3D11Texture2D* target, DXGI_FORMAT view_format) noexcept
    {
        D3D11_TEXTURE2D_DESC rd{};
        target->GetDesc(&rd);
        if (rd.SampleDesc.Count != 1 || rd.ArraySize != 1) {
            return false;
        }
        if (snap_tex == nullptr || snap_w != rd.Width || snap_h != rd.Height || snap_format != rd.Format) {
            D3D11_TEXTURE2D_DESC d = rd;
            d.MipLevels      = 1;
            d.Usage          = D3D11_USAGE_DEFAULT;
            d.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = 0;
            d.MiscFlags      = 0;
            ComPtr<ID3D11Texture2D> tex;
            ComPtr<ID3D11ShaderResourceView> srv;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format              = view_format;
            sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &tex)) || FAILED(device->CreateShaderResourceView(tex.Get(), &sv, &srv))) {
                return false;
            }
            snap_tex = std::move(tex);
            snap_srv = std::move(srv);
            snap_w = rd.Width; snap_h = rd.Height; snap_format = rd.Format;
        }
        const UINT lw = (rd.Width + 1) / 2;
        const UINT lh = (rd.Height + 1) / 2;
        const DXGI_FORMAT bf = internal::wide_format(rd.Format) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (blur_tex[0] == nullptr || blur_w != lw || blur_h != lh || blur_format != bf) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width            = lw;
            d.Height           = lh;
            d.MipLevels        = 1;
            d.ArraySize        = 1;
            d.Format           = bf;
            d.SampleDesc.Count = 1;
            d.Usage            = D3D11_USAGE_DEFAULT;
            d.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            for (int k = 0; k < 2; ++k) {
                if (FAILED(device->CreateTexture2D(&d, nullptr, &blur_tex[k])) ||
                    FAILED(device->CreateRenderTargetView(blur_tex[k].Get(), nullptr, &blur_rtv[k])) ||
                    FAILED(device->CreateShaderResourceView(blur_tex[k].Get(), nullptr, &blur_srv[k]))) {
                    blur_tex[0].Reset();
                    return false;
                }
            }
            blur_w      = lw;
            blur_h      = lh;
            blur_format = bf;
        }
        return true;
    }

    // one full-screen triangle into `dst`, scissored to the plan's low-res region
    void blur_pass(ID3D11RenderTargetView* dst, UINT vw, UINT vh, ID3D11ShaderResourceView* src, const blur_constants& c,
                   ID3D11PixelShader* shader, const internal::blur_plan& plan) noexcept
    {
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(3, 1, &none); // the source may have been the previous target
        context->OMSetRenderTargets(1, &dst, nullptr);
        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
        context->RSSetViewports(1, &vp);
        const D3D11_RECT sc{static_cast<LONG>(plan.lx0), static_cast<LONG>(plan.ly0), static_cast<LONG>(plan.lx1), static_cast<LONG>(plan.ly1)};
        context->RSSetScissorRects(1, &sc);
        set_blur_constants(c);
        context->PSSetConstantBuffers(1, 1, cb_blur.GetAddressOf());
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetShaderResources(3, 1, &src);
        context->Draw(3, 0);
    }

    // blurs the current target around `panel` (physical); on success blur_srv[0] holds the result in its top-left
    // (vw, vh), exact inside `out_exact`
    [[nodiscard]] bool run_blur(ID3D11RenderTargetView* host_rtv, ID3D11DepthStencilView* host_dsv, float radius_px, const rect& panel,
                                const D3D11_VIEWPORT& main_vp, UINT& out_vw, UINT& out_vh, rect& out_exact) noexcept
    {
        ComPtr<ID3D11Resource> res;
        host_rtv->GetResource(&res);
        ComPtr<ID3D11Texture2D> target;
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        host_rtv->GetDesc(&rvd);
        if (res == nullptr || FAILED(res.As(&target)) || rvd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
            !ensure_blur_targets(target.Get(), rvd.Format)) {
            return false;
        }

        const internal::blur_plan plan = internal::plan_blur(radius_px, panel, snap_w, snap_h);
        if (plan.x1 <= plan.x0 || plan.y1 <= plan.y0 || plan.lx1 <= plan.lx0 || plan.ly1 <= plan.ly0) {
            return false;
        }
        const D3D11_BOX box{plan.x0, plan.y0, 0, plan.x1, plan.y1, 1};
        context->CopySubresourceRegion(snap_tex.Get(), 0, plan.x0, plan.y0, 0, target.Get(), 0, &box);

        const UINT ds = plan.ds;
        const UINT vw = std::max(1u, (snap_w + ds - 1) / ds);
        const UINT vh = std::max(1u, (snap_h + ds - 1) / ds);
        const float tw = static_cast<float>(blur_w);
        const float th = static_cast<float>(blur_h);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs_fullscreen.Get(), nullptr, 0);
        context->RSSetState(raster.Get()); // (scissored: the passes only write the plan's region)
        const float blend_factor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(blend_off.Get(), blend_factor, 0xffffffffu);

        // 1. box filter the frame down into blur_tex[0]
        blur_constants c;
        c.bp0[0] = static_cast<float>(vw); c.bp0[1] = static_cast<float>(vh); c.bp0[2] = 1.0f; c.bp0[3] = 1.0f;
        c.bp1[0] = 1.0f / static_cast<float>(snap_w); c.bp1[1] = 1.0f / static_cast<float>(snap_h);
        c.bp1[2] = 1.0f - 0.5f * c.bp1[0]; c.bp1[3] = 1.0f - 0.5f * c.bp1[1];
        c.bp2[0] = static_cast<float>(ds) * 0.25f;
        blur_pass(blur_rtv[0].Get(), vw, vh, snap_srv.Get(), c, ps_blur_down.Get(), plan);

        // 2. gaussian, horizontal then vertical, once or several times for the big radii
        c.bp0[0] = static_cast<float>(vw); c.bp0[1] = static_cast<float>(vh);
        c.bp0[2] = static_cast<float>(vw) / tw; c.bp0[3] = static_cast<float>(vh) / th;
        c.bp1[2] = (static_cast<float>(vw) - 0.5f) / tw; c.bp1[3] = (static_cast<float>(vh) - 0.5f) / th;
        c.bp2[2] = plan.w0;
        c.bp3[0] = plan.w[0]; c.bp3[1] = plan.o[0]; c.bp3[2] = plan.w[1]; c.bp3[3] = plan.o[1];
        c.bp4[0] = plan.w[2]; c.bp4[1] = plan.o[2];
        for (int it = 0; it < plan.iterations; ++it) {
            c.bp1[0] = 1.0f / tw; c.bp1[1] = 0.0f;
            blur_pass(blur_rtv[1].Get(), vw, vh, blur_srv[0].Get(), c, ps_blur_gauss.Get(), plan);
            c.bp1[0] = 0.0f; c.bp1[1] = 1.0f / th;
            blur_pass(blur_rtv[0].Get(), vw, vh, blur_srv[1].Get(), c, ps_blur_gauss.Get(), plan);
        }

        // back to the host's target and the ui's own pipeline state
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(3, 1, &none);
        ID3D11RenderTargetView* rtv = host_rtv;
        context->OMSetRenderTargets(1, &rtv, host_dsv);
        context->RSSetViewports(1, &main_vp);
        context->RSSetState(raster.Get());
        context->OMSetBlendState(blend.Get(), blend_factor, 0xffffffffu);
        context->IASetInputLayout(layout.Get());
        context->VSSetShader(vs.Get(), nullptr, 0);
        out_vw    = vw;
        out_vh    = vh;
        out_exact = plan.exact;
        return true;
    }
    UINT                             vb_capacity{};
    UINT                             ib_capacity{};
    UINT                             shape_capacity{};
    // content hash of the dynamic buffers, to skip identical uploads. 0 = unknown / reset by reallocation.
    u64                              uploaded_hash{};
    bool                             restore_state{true};

    // best effort: overwrite dynamic buffers before release (the driver may still hold renamed copies)
    void wipe() noexcept
    {
        for (texture_slot& t : textures) { t.image.wipe(); } // (the copies kept for update_texture)
        const auto zero = [&](ID3D11Buffer* b) {
            if (b == nullptr) { return; }
            D3D11_BUFFER_DESC d{};
            b->GetDesc(&d);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(context->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                std::memset(m.pData, 0, d.ByteWidth);
                context->Unmap(b, 0);
            }
        };
        zero(vb.Get());
        zero(ib.Get());
        zero(shape_buf.Get());
    }

    [[nodiscard]] bool ensure_buffer(ComPtr<ID3D11Buffer>& buf, UINT& capacity, UINT needed, UINT stride, UINT bind) noexcept
    {
        if (buf != nullptr && capacity >= needed) {
            return true;
        }
        const UINT new_capacity = needed + needed / 2 + 1024; // headroom so steady state never reallocates
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth      = new_capacity * stride;
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.BindFlags      = bind;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> fresh;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &fresh))) {
            return false;
        }
        buf           = std::move(fresh);
        capacity      = new_capacity;
        uploaded_hash = 0; // a fresh buffer holds nothing
        return true;
    }

    // structured buffer of shape_record, bound at t1
    [[nodiscard]] bool ensure_shapes(UINT needed) noexcept
    {
        if (shape_buf != nullptr && shape_capacity >= needed) {
            return true;
        }
        const UINT new_capacity = needed + needed / 2 + 256;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth           = new_capacity * sizeof(shape_record);
        desc.Usage               = D3D11_USAGE_DYNAMIC;
        desc.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(shape_record);
        ComPtr<ID3D11Buffer> fresh;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &fresh))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format             = DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension      = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0;
        sv.Buffer.NumElements  = new_capacity;
        ComPtr<ID3D11ShaderResourceView> fresh_srv;
        if (FAILED(device->CreateShaderResourceView(fresh.Get(), &sv, &fresh_srv))) {
            return false;
        }
        shape_buf      = std::move(fresh);
        shape_srv      = std::move(fresh_srv);
        shape_capacity = new_capacity;
        uploaded_hash  = 0;
        return true;
    }
};

d3d11_renderer::d3d11_renderer() noexcept = default;
d3d11_renderer::~d3d11_renderer()
{
    destroy();
}
d3d11_renderer::d3d11_renderer(d3d11_renderer&&) noexcept            = default;
d3d11_renderer& d3d11_renderer::operator=(d3d11_renderer&&) noexcept = default;

bool d3d11_renderer::create(ID3D11Device* device, ID3D11DeviceContext* context, const font_atlas& atlas)
{
    destroy();
    auto p = std::make_unique<impl>();
    p->device  = device;
    p->context = context;

    if (FAILED(device->CreateVertexShader(g_strata_ui_vs, sizeof(g_strata_ui_vs), nullptr, &p->vs)) ||
        FAILED(device->CreatePixelShader(g_strata_ui_ps, sizeof(g_strata_ui_ps), nullptr, &p->ps)) ||
        FAILED(device->CreatePixelShader(g_strata_ui_ps_image, sizeof(g_strata_ui_ps_image), nullptr, &p->ps_image))) {
        return false;
    }

    // blur pipeline: optional, the ui works without it (backdrop panels then draw as flat tints)
    p->blur_supported =
        SUCCEEDED(device->CreatePixelShader(g_strata_ui_ps_backdrop, sizeof(g_strata_ui_ps_backdrop), nullptr, &p->ps_backdrop)) &&
        SUCCEEDED(device->CreateVertexShader(g_strata_ui_vs_fullscreen, sizeof(g_strata_ui_vs_fullscreen), nullptr, &p->vs_fullscreen)) &&
        SUCCEEDED(device->CreatePixelShader(g_strata_ui_ps_blur_down, sizeof(g_strata_ui_ps_blur_down), nullptr, &p->ps_blur_down)) &&
        SUCCEEDED(device->CreatePixelShader(g_strata_ui_ps_blur_gauss, sizeof(g_strata_ui_ps_blur_gauss), nullptr, &p->ps_blur_gauss));
    if (p->blur_supported) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(impl::blur_constants);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_BLEND_DESC off{};
        off.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        p->blur_supported = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &p->cb_blur)) &&
                            SUCCEEDED(device->CreateBlendState(&off, &p->blend_off));
    }

    static constexpr D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(vertex, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_UINT,    0, offsetof(vertex, u),   D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(vertex, col), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(layout, 3, g_strata_ui_vs, sizeof(g_strata_ui_vs), &p->layout))) {
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth      = sizeof(internal::ui_constants);
    cb_desc.Usage          = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &p->cb))) {
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    // the shader outputs premultiplied alpha
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &p->blend))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = TRUE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &p->raster))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable    = FALSE;
    dd.StencilEnable  = FALSE;
    if (FAILED(device->CreateDepthStencilState(&dd, &p->depth))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.MaxLOD         = D3D11_FLOAT32_MAX; // (images pick their own mip level in the shader)
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateSamplerState(&sd, &p->sampler))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = atlas.width();
    td.Height           = atlas.height();
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem     = atlas.pixels().data();
    sub.SysMemPitch = atlas.width();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sub, &tex)) ||
        FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &p->atlas_srv))) {
        return false;
    }

    impl_ = std::move(p);
    return true;
}

bool d3d11_renderer::update_atlas(const font_atlas& atlas)
{
    if (impl_ == nullptr || atlas.pixels().empty()) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = atlas.width();
    td.Height           = atlas.height();
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem     = atlas.pixels().data();
    sub.SysMemPitch = atlas.width();
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(impl_->device->CreateTexture2D(&td, &sub, &tex)) ||
        FAILED(impl_->device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        return false;
    }
    impl_->atlas_srv = std::move(srv);
    return true;
}

texture_id d3d11_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

namespace {

[[nodiscard]] DXGI_FORMAT dxgi_format_of(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case texture_layout::rgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                      return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

} // namespace

texture_id d3d11_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (impl_ == nullptr) {
        return 0;
    }
    texture_image image;
    if (!image.create(desc, pixels)) {
        return 0;
    }
    const u32 levels = image.level_count();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = image.width();
    td.Height           = image.height();
    td.MipLevels        = levels;
    td.ArraySize        = 1;
    td.Format           = dxgi_format_of(image.layout());
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> sub(levels);
    for (u32 k = 0; k < levels; ++k) {
        sub[k].pSysMem     = image.pixels(k).data();
        sub[k].SysMemPitch = image.pitch(k);
    }

    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(impl_->device->CreateTexture2D(&td, sub.data(), &tex)) ||
        FAILED(impl_->device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        image.wipe();
        return 0;
    }

    impl::texture_slot slot;
    slot.tex       = std::move(tex);
    slot.srv       = std::move(srv);
    slot.updatable = desc.updatable;
    if (desc.updatable) {
        slot.image = std::move(image);
    } else {
        image.wipe(); // the gpu has its copy
    }

    auto& slots = impl_->textures;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].srv == nullptr) {
            slots[i] = std::move(slot);
            return static_cast<texture_id>(i + 1);
        }
    }
    slots.push_back(std::move(slot));
    return static_cast<texture_id>(slots.size());
}

bool d3d11_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (impl_ == nullptr || id == 0 || id > impl_->textures.size()) {
        return false;
    }
    impl::texture_slot& slot = impl_->textures[id - 1];
    if (slot.srv == nullptr || !slot.updatable) {
        return false;
    }
    std::vector<texture_image::region> dirty;
    if (!slot.image.update(x, y, width, height, pixels, dirty)) {
        return false;
    }
    const u32 bpp = texture_layout_bytes(slot.image.layout());
    for (const texture_image::region& r : dirty) {
        const D3D11_BOX box{r.x, r.y, 0, r.x + r.w, r.y + r.h, 1};
        const u32 pitch = slot.image.pitch(r.level);
        const u8* first = slot.image.pixels(r.level).data() + static_cast<std::size_t>(r.y) * pitch + static_cast<std::size_t>(r.x) * bpp;
        impl_->context->UpdateSubresource(slot.tex.Get(), r.level, &box, first, pitch, 0);
    }
    return true;
}

void d3d11_renderer::destroy_texture(texture_id id) noexcept
{
    if (impl_ != nullptr && id != 0 && id <= impl_->textures.size()) {
        impl::texture_slot& slot = impl_->textures[id - 1];
        slot.image.wipe();
        slot = {};
    }
}

void d3d11_renderer::destroy() noexcept
{
    if (impl_ != nullptr) {
        impl_->wipe();
    }
    impl_.reset();
}

void d3d11_renderer::set_state_restore(bool on) noexcept
{
    if (impl_ != nullptr) { impl_->restore_state = on; }
}

bool d3d11_renderer::state_restore() const noexcept
{
    return impl_ != nullptr && impl_->restore_state;
}

void d3d11_renderer::set_output(const output_desc& output) noexcept
{
    if (impl_ != nullptr) { impl_->output = output; }
}

output_desc d3d11_renderer::output() const noexcept
{
    return impl_ != nullptr ? impl_->output : output_desc{};
}

bool d3d11_renderer::device_lost() const noexcept
{
    return impl_ != nullptr && FAILED(impl_->device->GetDeviceRemovedReason());
}

void d3d11_renderer::render(const draw_data& data)
{
    if (impl_ == nullptr || data.commands.empty() || data.display_size.x <= 0 || data.display_size.y <= 0) {
        return;
    }
    impl& s = *impl_;
    ID3D11DeviceContext* ctx = s.context.Get();

    const auto vcount = static_cast<UINT>(data.vertices.size());
    const auto icount = static_cast<UINT>(data.indices.size());
    const auto scount = static_cast<UINT>(data.shapes.size());
    if (!s.ensure_buffer(s.vb, s.vb_capacity, vcount, sizeof(vertex), D3D11_BIND_VERTEX_BUFFER) ||
        !s.ensure_buffer(s.ib, s.ib_capacity, icount, sizeof(index_t), D3D11_BIND_INDEX_BUFFER) ||
        !s.ensure_shapes(std::max(scount, 1u))) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    // unchanged hash: the buffers already hold this frame, so skip three Map(WRITE_DISCARD) + memcpy (most of a static
    // panel's cpu cost). the draws still happen: the target was cleared or redrawn by a game.
    const bool have_geometry = data.content_hash != 0 && data.content_hash == s.uploaded_hash;
    if (!have_geometry) {
        if (FAILED(ctx->Map(s.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
        std::memcpy(map.pData, data.vertices.data(), data.vertices.size_bytes());
        ctx->Unmap(s.vb.Get(), 0);

        if (FAILED(ctx->Map(s.ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
        std::memcpy(map.pData, data.indices.data(), data.indices.size_bytes());
        ctx->Unmap(s.ib.Get(), 0);

        if (scount != 0) {
            if (FAILED(ctx->Map(s.shape_buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
            std::memcpy(map.pData, data.shapes.data(), data.shapes.size_bytes());
            ctx->Unmap(s.shape_buf.Get(), 0);
        }

        s.uploaded_hash = data.content_hash;
    }
    // transform and output encoding are separate from the geometry: either can change without it
    const internal::ui_constants constants = internal::make_ui_constants(data, s.output);
    if (!s.cb_valid || std::memcmp(&constants, &s.cb_written, sizeof constants) != 0) {
        if (FAILED(ctx->Map(s.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { return; }
        std::memcpy(map.pData, &constants, sizeof constants);
        ctx->Unmap(s.cb.Get(), 0);
        s.cb_written = constants;
        s.cb_valid   = true;
    }

    // the state guard is for overlays; device owners can skip it (see set_state_restore)
    std::optional<state_guard> guard;
    if (s.restore_state) {
        guard.emplace(ctx);
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, data.display_size.x, data.display_size.y, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(s.raster.Get());

    const float blend_factor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(s.blend.Get(), blend_factor, 0xffffffffu);
    ctx->OMSetDepthStencilState(s.depth.Get(), 0);

    ctx->IASetInputLayout(s.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(vertex);
    const UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, s.vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(s.ib.Get(), DXGI_FORMAT_R16_UINT, 0);

    ctx->VSSetShader(s.vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    // stages the ui does not use but a game may leave bound (tessellation, GS, stream output, predicate) would bend,
    // drop or divert its triangles
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->SOSetTargets(0, nullptr, nullptr);
    ctx->SetPredication(nullptr, FALSE);
    ctx->PSSetShader(s.ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, s.cb.GetAddressOf()); // (the output encoding)
    ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = {s.atlas_srv.Get(), s.shape_srv.Get()};
    ctx->PSSetShaderResources(0, 2, srvs);

    enum class ps_mode { main, image, backdrop };
    ps_mode    mode  = ps_mode::main;
    texture_id bound = 0;

    // the target the ui draws into, fetched when the first backdrop panel needs it
    ComPtr<ID3D11RenderTargetView> host_rtv;
    ComPtr<ID3D11DepthStencilView> host_dsv;
    bool  host_rt_fetched = false;
    bool  blur_dirty      = true; // something was drawn since the blur was made
    float blur_radius     = -1.0f;
    rect  blur_exact{};           // where the blur that was made is exact: a panel inside it can use it again
    UINT  blur_vw = 0, blur_vh = 0;

    for (const draw_cmd& cmd : data.commands) {
        if (cmd.idx_count == 0 || cmd.clip.empty()) {
            continue;
        }

        ps_mode want = cmd.texture != 0 ? ps_mode::image : ps_mode::main;
        if (cmd.blur > 0.0f && s.blur_supported) {
            if (!host_rt_fetched) {
                ctx->OMGetRenderTargets(1, &host_rtv, &host_dsv);
                host_rt_fetched = true;
            }
            if (host_rtv != nullptr) {
                const rect panel = internal::backdrop_bounds(data, cmd);
                if (blur_dirty || cmd.blur != blur_radius || !internal::rect_inside(panel, blur_exact)) {
                    blur_dirty = true;
                    if (!panel.empty() && s.run_blur(host_rtv.Get(), host_dsv.Get(), cmd.blur, panel, vp, blur_vw, blur_vh, blur_exact)) {
                        blur_dirty  = false;
                        blur_radius = cmd.blur;
                        mode  = ps_mode::main; // run_blur left the pipeline in the ui's state, but not the shaders / views
                        bound = 0;
                        ctx->PSSetShader(s.ps.Get(), nullptr, 0);
                        ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
                        ID3D11ShaderResourceView* srvs_again[2] = {s.atlas_srv.Get(), s.shape_srv.Get()};
                        ctx->PSSetShaderResources(0, 2, srvs_again);
                    }
                }
                if (!blur_dirty) { want = ps_mode::backdrop; }
            }
        }

        if (want == ps_mode::image) {
            ID3D11ShaderResourceView* srv = cmd.texture <= s.textures.size() ? s.textures[cmd.texture - 1].srv.Get() : nullptr;
            if (srv == nullptr) {
                continue; // a texture that no longer exists: draw nothing rather than the wrong image
            }
            if (mode != ps_mode::image) { ctx->PSSetShader(s.ps_image.Get(), nullptr, 0); }
            if (mode != ps_mode::image || bound != cmd.texture) { ctx->PSSetShaderResources(2, 1, &srv); }
            bound = cmd.texture;
        } else if (want == ps_mode::backdrop) {
            if (mode != ps_mode::backdrop) {
                ctx->PSSetShader(s.ps_backdrop.Get(), nullptr, 0);
                ID3D11ShaderResourceView* blurred = s.blur_srv[0].Get();
                ctx->PSSetShaderResources(3, 1, &blurred);
                impl::blur_constants c;
                c.bp0[0] = data.display_size.x; c.bp0[1] = data.display_size.y;
                c.bp0[2] = static_cast<float>(blur_vw) / static_cast<float>(s.blur_w);
                c.bp0[3] = static_cast<float>(blur_vh) / static_cast<float>(s.blur_h);
                s.set_blur_constants(c);
                ctx->PSSetConstantBuffers(1, 1, s.cb_blur.GetAddressOf());
            }
            bound = 0;
        } else if (mode != ps_mode::main) {
            ctx->PSSetShader(s.ps.Get(), nullptr, 0);
            bound = 0;
        }
        if (want == ps_mode::main && cmd.texture == 0) { blur_dirty = true; }
        if (want == ps_mode::image) { blur_dirty = true; }
        mode = want;
        const D3D11_RECT scissor{static_cast<LONG>(cmd.clip.min.x), static_cast<LONG>(cmd.clip.min.y),
                                 static_cast<LONG>(cmd.clip.max.x), static_cast<LONG>(cmd.clip.max.y)};
        ctx->RSSetScissorRects(1, &scissor);
        ctx->DrawIndexed(cmd.idx_count, cmd.idx_offset, static_cast<INT>(cmd.vtx_offset));
    }
}

} // namespace strata

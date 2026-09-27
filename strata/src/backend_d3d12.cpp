#include "strata/backend/d3d12.hpp"

#include "blur_plan.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "g_strata_ui_vs.h"
#include "g_strata_ui_ps.h"
#include "g_strata_ui_ps_image.h"
#include "g_strata_ui_ps_backdrop.h"
#include "g_strata_ui_vs_fullscreen.h"
#include "g_strata_ui_ps_blur_down.h"
#include "g_strata_ui_ps_blur_gauss.h"

namespace strata {

using Microsoft::WRL::ComPtr;

namespace {

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) noexcept
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type                 = type;
    p.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask     = 1;
    p.VisibleNodeMask      = 1;
    return p;
}

[[nodiscard]] DXGI_FORMAT dxgi_format_of(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case texture_layout::rgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                      return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

[[nodiscard]] D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) noexcept
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width            = bytes;
    d.Height           = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_upload_buffer(ID3D12Device* dev, UINT64 bytes) noexcept
{
    const auto props = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc  = buffer_desc(bytes);
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

struct frame_buffers {
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    ComPtr<ID3D12Resource> shapes;
    UINT                   vb_capacity{};
    UINT                   ib_capacity{};
    UINT                   shape_capacity{};
    // content hash of this slot's buffers. per slot because with frames in flight consecutive frames use different
    // slots, so a slot can be several frames behind. 0 = empty / reset by reallocation.
    u64                    uploaded_hash{};
};

} // namespace

// 32-bit root constants of the blur / backdrop parameters (b1): impl::blur_constants
constexpr UINT blur_constant_count = 20;

struct d3d12_renderer::impl {
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12RootSignature>       root;
    ComPtr<ID3D12PipelineState>       pso;
    ComPtr<ID3D12PipelineState>       pso_image;
    ComPtr<ID3D12DescriptorHeap>      srv_heap;   // slot 0: the font atlas, slot n: texture n
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12Resource>            atlas;
    std::array<ComPtr<ID3D12Resource>, d3d12_renderer::max_textures> textures;
    struct texture_meta {
        texture_image image; // kept (for update_texture) only when the texture is updatable
        bool          updatable{};
        u32           cooldown{}; // destroyed texture's slot: renders left before frames in flight stop reading it
    };
    // a texture copy staged by create_texture / update_texture, recorded at the start of the next render()
    struct pending_copy {
        ComPtr<ID3D12Resource>              texture;
        ComPtr<ID3D12Resource>              upload;
        std::vector<texture_image::region>  regions;
        std::vector<std::pair<UINT64, UINT>> places; // offset and row pitch of each region in `upload`
        DXGI_FORMAT                         format{};
        D3D12_RESOURCE_STATES               before{};
        D3D12_RESOURCE_STATES               after{};
    };
    std::vector<pending_copy>         pending;
    std::array<texture_meta, d3d12_renderer::max_textures> texture_meta_;
    UINT                              srv_size{};
    std::vector<frame_buffers>        frames;

    // backdrop blur. heap slots after the textures: the frame snapshot, then the two blur targets
    static constexpr u32 snap_slot = 1 + d3d12_renderer::max_textures;
    ComPtr<ID3D12RootSignature>       root_blur;
    ComPtr<ID3D12PipelineState>       pso_backdrop;
    ComPtr<ID3D12PipelineState>       pso_blur_down;
    ComPtr<ID3D12PipelineState>       pso_blur_gauss;
    ComPtr<ID3D12DescriptorHeap>      rtv_heap;
    ComPtr<ID3D12Resource>            snap;
    ComPtr<ID3D12Resource>            blur_tex[2];
    UINT                              snap_w{}, snap_h{}, blur_w{}, blur_h{};
    DXGI_FORMAT                       snap_format{};
    DXGI_FORMAT                       blur_format{};
    DXGI_FORMAT                       blur_rt_format{DXGI_FORMAT_R8G8B8A8_UNORM}; // the blur's own targets: see wide_format
    output_desc                       output{};
    UINT                              rtv_size{};
    u32                               frames_in_flight{};
    bool                              blur_supported{};
    // resources replaced while the gpu may read them, released frames_in_flight + 1 renders later (the host waits on a
    // slot's fence before reuse). staging buffers are zeroed first so uploads do not linger in freed memory
    struct grave {
        ComPtr<ID3D12Resource> res;
        u32                    renders{};
        bool                   wipe{};
    };
    std::vector<grave> graveyard;

    void retire(ComPtr<ID3D12Resource>& r, bool wipe = false)
    {
        if (r != nullptr) { graveyard.push_back({std::move(r), frames_in_flight + 1, wipe}); }
    }
    static void wipe_upload(ID3D12Resource* r) noexcept
    {
        void* mapped{};
        if (SUCCEEDED(r->Map(0, nullptr, &mapped))) {
            ::SecureZeroMemory(mapped, static_cast<SIZE_T>(r->GetDesc().Width));
            r->Unmap(0, nullptr);
        }
    }
    void tick_graveyard()
    {
        for (grave& g : graveyard) {
            if (g.renders > 0) { --g.renders; }
            if (g.renders == 0 && g.wipe) { wipe_upload(g.res.Get()); }
        }
        std::erase_if(graveyard, [](const grave& g) { return g.renders == 0; });
        for (texture_meta& t : texture_meta_) { if (t.cooldown > 0) { --t.cooldown; } }
    }

    static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) noexcept
    {
        D3D12_RESOURCE_BARRIER rb{};
        rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource   = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter  = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        l->ResourceBarrier(1, &rb);
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_slot(u32 index) const noexcept
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(index) * srv_size;
        return h;
    }

    [[nodiscard]] bool ensure_blur(const D3D12_RESOURCE_DESC& td) noexcept
    {
        if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.SampleDesc.Count != 1 || td.DepthOrArraySize != 1) {
            return false;
        }
        const auto default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels     = 1;

        if (snap == nullptr || snap_w != td.Width || snap_h != td.Height || snap_format != td.Format) {
            retire(snap);
            D3D12_RESOURCE_DESC d = td;
            d.Alignment = 0;
            d.MipLevels = 1;
            d.Flags     = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &d,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&snap)))) {
                return false;
            }
            sv.Format = td.Format;
            D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(snap_slot) * srv_size;
            device->CreateShaderResourceView(snap.Get(), &sv, h);
            snap_w = static_cast<UINT>(td.Width);
            snap_h = td.Height;
            snap_format = td.Format;
        }

        const UINT lw = (static_cast<UINT>(td.Width) + 1) / 2;
        const UINT lh = (td.Height + 1) / 2;
        const DXGI_FORMAT bf = blur_rt_format; // (fixed at create(): the blur pipelines are built for it)
        if (blur_tex[0] == nullptr || blur_w != lw || blur_h != lh || blur_format != bf) {
            retire(blur_tex[0]);
            retire(blur_tex[1]);
            D3D12_RESOURCE_DESC d{};
            d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width            = lw;
            d.Height           = lh;
            d.DepthOrArraySize = 1;
            d.MipLevels        = 1;
            d.Format           = bf;
            d.SampleDesc.Count = 1;
            d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            for (u32 k = 0; k < 2; ++k) {
                if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &d,
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&blur_tex[k])))) {
                    blur_tex[0].Reset();
                    return false;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE sh = srv_heap->GetCPUDescriptorHandleForHeapStart();
                sh.ptr += static_cast<SIZE_T>(snap_slot + 1 + k) * srv_size;
                sv.Format = bf;
                device->CreateShaderResourceView(blur_tex[k].Get(), &sv, sh);
                D3D12_CPU_DESCRIPTOR_HANDLE rh = rtv_heap->GetCPUDescriptorHandleForHeapStart();
                rh.ptr += static_cast<SIZE_T>(k) * rtv_size;
                device->CreateRenderTargetView(blur_tex[k].Get(), nullptr, rh);
            }
            blur_w      = lw;
            blur_h      = lh;
            blur_format = bf;
        }
        return true;
    }

    struct blur_constants {
        float bp0[4]{};
        float bp1[4]{};
        float bp2[4]{};
        float bp3[4]{};
        float bp4[4]{};
    };
    static_assert(sizeof(blur_constants) == blur_constant_count * 4);

    // one full-screen triangle into blur_tex[dst], scissored to the plan's low-res region
    void blur_pass(ID3D12GraphicsCommandList* list, u32 dst, UINT vw, UINT vh, D3D12_GPU_DESCRIPTOR_HANDLE src,
                   const blur_constants& c, ID3D12PipelineState* state, const internal::blur_plan& plan) noexcept
    {
        barrier(list, blur_tex[dst].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rh.ptr += static_cast<SIZE_T>(dst) * rtv_size;
        list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
        const D3D12_RECT     sc{static_cast<LONG>(plan.lx0), static_cast<LONG>(plan.ly0), static_cast<LONG>(plan.lx1), static_cast<LONG>(plan.ly1)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(state);
        list->SetGraphicsRoot32BitConstants(0, blur_constant_count, &c, 0);
        list->SetGraphicsRootDescriptorTable(1, src);
        list->DrawInstanced(3, 1, 0, 0);
        barrier(list, blur_tex[dst].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // copies the target around `panel` (physical), boxes it down and blurs it; on success blur_tex[0] holds the result
    // in its top-left (vw, vh), exact inside `out_exact`
    [[nodiscard]] bool run_blur(ID3D12GraphicsCommandList* list, const d3d12_target& t, float radius_px, const rect& panel,
                                UINT& out_vw, UINT& out_vh, rect& out_exact) noexcept
    {
        const D3D12_RESOURCE_DESC td = t.resource->GetDesc();
        if (!ensure_blur(td)) {
            return false;
        }
        const internal::blur_plan plan = internal::plan_blur(radius_px, panel, snap_w, snap_h);
        if (plan.x1 <= plan.x0 || plan.y1 <= plan.y0 || plan.lx1 <= plan.lx0 || plan.ly1 <= plan.ly0) {
            return false;
        }

        barrier(list, t.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list, snap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = snap.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = t.resource;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{plan.x0, plan.y0, 0, plan.x1, plan.y1, 1};
        list->CopyTextureRegion(&dst, plan.x0, plan.y0, 0, &src, &box);
        barrier(list, t.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(list, snap.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        const UINT  ds = plan.ds;
        const UINT  vw = std::max(1u, (snap_w + ds - 1) / ds);
        const UINT  vh = std::max(1u, (snap_h + ds - 1) / ds);
        const float tw = static_cast<float>(blur_w);
        const float th = static_cast<float>(blur_h);

        list->SetGraphicsRootSignature(root_blur.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        blur_constants c;
        c.bp0[0] = static_cast<float>(vw); c.bp0[1] = static_cast<float>(vh); c.bp0[2] = 1.0f; c.bp0[3] = 1.0f;
        c.bp1[0] = 1.0f / static_cast<float>(snap_w); c.bp1[1] = 1.0f / static_cast<float>(snap_h);
        c.bp1[2] = 1.0f - 0.5f * c.bp1[0]; c.bp1[3] = 1.0f - 0.5f * c.bp1[1];
        c.bp2[0] = static_cast<float>(ds) * 0.25f;
        blur_pass(list, 0, vw, vh, gpu_slot(snap_slot), c, pso_blur_down.Get(), plan);

        c.bp0[2] = static_cast<float>(vw) / tw; c.bp0[3] = static_cast<float>(vh) / th;
        c.bp1[2] = (static_cast<float>(vw) - 0.5f) / tw; c.bp1[3] = (static_cast<float>(vh) - 0.5f) / th;
        c.bp2[2] = plan.w0;
        c.bp3[0] = plan.w[0]; c.bp3[1] = plan.o[0]; c.bp3[2] = plan.w[1]; c.bp3[3] = plan.o[1];
        c.bp4[0] = plan.w[2]; c.bp4[1] = plan.o[2];
        for (int it = 0; it < plan.iterations; ++it) {
            c.bp1[0] = 1.0f / tw; c.bp1[1] = 0.0f;
            blur_pass(list, 1, vw, vh, gpu_slot(snap_slot + 1), c, pso_blur_gauss.Get(), plan);
            c.bp1[0] = 0.0f; c.bp1[1] = 1.0f / th;
            blur_pass(list, 0, vw, vh, gpu_slot(snap_slot + 2), c, pso_blur_gauss.Get(), plan);
        }

        D3D12_CPU_DESCRIPTOR_HANDLE host{};
        host.ptr = static_cast<SIZE_T>(t.rtv);
        list->OMSetRenderTargets(1, &host, FALSE, nullptr);
        out_vw    = vw;
        out_vh    = vh;
        out_exact = plan.exact;
        return true;
    }

    // zero the persistently mapped upload buffers; the host must have idled the gpu first
    void wipe() noexcept
    {
        for (texture_meta& t : texture_meta_) { t.image.wipe(); } // (the copies kept for update_texture)
        for (pending_copy& pc : pending) { wipe_upload(pc.upload.Get()); } // staged, never recorded
        for (grave& g : graveyard) { if (g.wipe) { wipe_upload(g.res.Get()); } }
        for (frame_buffers& f : frames) {
            for (ID3D12Resource* res : {f.vb.Get(), f.ib.Get(), f.shapes.Get()}) {
                if (res == nullptr) { continue; }
                void* mapped{};
                const D3D12_RANGE none{0, 0};
                if (SUCCEEDED(res->Map(0, &none, &mapped))) {
                    std::memset(mapped, 0, static_cast<std::size_t>(res->GetDesc().Width));
                    res->Unmap(0, nullptr);
                }
            }
        }
    }

    [[nodiscard]] bool ensure(ComPtr<ID3D12Resource>& res, UINT& capacity, UINT needed, UINT stride) noexcept
    {
        if (res != nullptr && capacity >= needed) {
            return true;
        }
        const UINT new_capacity = needed + needed / 2 + 1024;
        auto fresh = create_upload_buffer(device.Get(), static_cast<UINT64>(new_capacity) * stride);
        if (fresh == nullptr) {
            return false;
        }
        // the previous buffer of this frame slot is no longer in flight (host waited on its fence)
        res      = std::move(fresh);
        capacity = new_capacity;
        return true;
    }

    // default-heap texture filled from `pixels` (tightly packed, `bytes_per_pixel`) by a blocking queue copy; staging is
    // zeroed afterwards
    [[nodiscard]] bool upload_texture(ComPtr<ID3D12Resource>& texture, u32 width, u32 height, DXGI_FORMAT format,
                                      u32 bytes_per_pixel, const u8* pixels) noexcept
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = width;
        td.Height           = height;
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = format;
        td.SampleDesc.Count = 1;
        td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ComPtr<ID3D12Resource> tex;
        const auto default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) {
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT   rows{};
        UINT64 row_bytes{};
        UINT64 total{};
        device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &row_bytes, &total);

        auto upload = create_upload_buffer(device.Get(), total);
        if (upload == nullptr) {
            return false;
        }

        void* mapped{};
        if (FAILED(upload->Map(0, nullptr, &mapped))) {
            return false;
        }
        const std::size_t row_bytes_used = static_cast<std::size_t>(width) * bytes_per_pixel;
        for (UINT y = 0; y < rows; ++y) {
            std::memcpy(static_cast<std::byte*>(mapped) + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                        pixels + static_cast<std::size_t>(y) * row_bytes_used, row_bytes_used);
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator>    alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence>               fence;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = tex.Get();
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = tex.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), 1))) {
            return false;
        }
        HANDLE event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return false;
        }
        if (fence->GetCompletedValue() < 1) {
            fence->SetEventOnCompletion(1, event);
            ::WaitForSingleObject(event, INFINITE);
        }
        ::CloseHandle(event);

        // the staging copy must not outlive the upload
        if (SUCCEEDED(upload->Map(0, nullptr, &mapped))) {
            ::SecureZeroMemory(mapped, static_cast<SIZE_T>(total));
            upload->Unmap(0, nullptr);
        }
        texture = std::move(tex);
        return true;
    }

    // stages regions of `image`'s levels for `tex`: one upload buffer, one copy per region, recorded by the next
    // render() on the host's command list (no gpu wait). transitions `before` -> COPY_DEST -> `after` there
    [[nodiscard]] bool copy_image_regions(const ComPtr<ID3D12Resource>& tex, const texture_image& image,
                                          std::span<const texture_image::region> regions, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) noexcept
    {
        const DXGI_FORMAT format = dxgi_format_of(image.layout());
        const u32         bpp    = texture_layout_bytes(image.layout());
        struct placement { UINT64 offset; UINT pitch; };
        std::vector<placement> places;
        places.reserve(regions.size());
        UINT64 total = 0;
        for (const texture_image::region& r : regions) {
            total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            const UINT pitch = (r.w * bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~static_cast<UINT>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            places.push_back({total, pitch});
            total += static_cast<UINT64>(pitch) * r.h;
        }
        auto upload = create_upload_buffer(device.Get(), total);
        if (upload == nullptr) {
            return false;
        }
        void* mapped{};
        if (FAILED(upload->Map(0, nullptr, &mapped))) {
            return false;
        }
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const texture_image::region& r = regions[i];
            const u32 level_pitch = image.pitch(r.level);
            const u8* first = image.pixels(r.level).data() + static_cast<std::size_t>(r.y) * level_pitch + static_cast<std::size_t>(r.x) * bpp;
            for (u32 row = 0; row < r.h; ++row) {
                std::memcpy(static_cast<std::byte*>(mapped) + places[i].offset + static_cast<std::size_t>(row) * places[i].pitch,
                            first + static_cast<std::size_t>(row) * level_pitch, static_cast<std::size_t>(r.w) * bpp);
            }
        }
        upload->Unmap(0, nullptr);

        pending_copy pc;
        pc.texture = tex;
        pc.upload  = std::move(upload);
        pc.regions.assign(regions.begin(), regions.end());
        for (const placement& pl : places) { pc.places.emplace_back(pl.offset, pl.pitch); }
        pc.format = format;
        pc.before = before;
        pc.after  = after;
        pending.push_back(std::move(pc));
        return true;
    }

    // records what create_texture / update_texture staged since the last render, ahead of the ui
    void record_pending(ID3D12GraphicsCommandList* list) noexcept
    {
        for (pending_copy& pc : pending) {
            if (pc.before != D3D12_RESOURCE_STATE_COPY_DEST) {
                barrier(list, pc.texture.Get(), pc.before, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            for (std::size_t i = 0; i < pc.regions.size(); ++i) {
                const texture_image::region& r = pc.regions[i];
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource        = pc.texture.Get();
                dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = r.level;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource                          = pc.upload.Get();
                src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset             = pc.places[i].first;
                src.PlacedFootprint.Footprint.Format   = pc.format;
                src.PlacedFootprint.Footprint.Width    = r.w;
                src.PlacedFootprint.Footprint.Height   = r.h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pc.places[i].second;
                list->CopyTextureRegion(&dst, r.x, r.y, 0, &src, nullptr);
            }
            barrier(list, pc.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, pc.after);
            retire(pc.upload, true); // the gpu reads it during this frame: zeroed and released once that is done
        }
        pending.clear();
    }

    // a texture that is going away, with the copies still staged for it
    void drop_pending(ID3D12Resource* tex) noexcept
    {
        for (pending_copy& pc : pending) {
            if (pc.texture.Get() == tex) { wipe_upload(pc.upload.Get()); }
        }
        std::erase_if(pending, [tex](const pending_copy& pc) { return pc.texture.Get() == tex; });
    }

    [[nodiscard]] bool upload_atlas(const font_atlas& font) noexcept
    {
        return upload_texture(atlas, font.width(), font.height(), DXGI_FORMAT_R8_UNORM, 1, font.pixels().data());
    }
};

d3d12_renderer::d3d12_renderer() noexcept = default;
d3d12_renderer::~d3d12_renderer()
{
    destroy();
}
d3d12_renderer::d3d12_renderer(d3d12_renderer&&) noexcept            = default;
d3d12_renderer& d3d12_renderer::operator=(d3d12_renderer&&) noexcept = default;

bool d3d12_renderer::create(const d3d12_init_info& info, const font_atlas& font)
{
    destroy();
    if (info.device == nullptr || info.queue == nullptr || info.frames_in_flight == 0) {
        return false;
    }

    auto p = std::make_unique<impl>();
    p->device = info.device;
    p->queue  = info.queue;
    p->frames.resize(info.frames_in_flight);

    // root signature: [0] eight 32-bit constants (transform, output encoding), [1] atlas srv t0, [2] shape table t1,
    // [3] image srv t2 (all ps), static linear-clamp sampler
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = 1;
    range.BaseShaderRegister                = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE image_range = range;
    image_range.BaseShaderRegister     = 2;
    D3D12_DESCRIPTOR_RANGE blur_range = range;
    blur_range.BaseShaderRegister      = 3;

    D3D12_ROOT_PARAMETER params[6]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(internal::ui_constants) / 4;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL; // (the pixel shader reads the output encoding)
    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV; // shape table, t1
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &image_range;
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // blurred frame, t3
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges   = &blur_range;
    params[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    params[5].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // blur / backdrop parameters, b1
    params[5].Constants.ShaderRegister = 1;
    params[5].Constants.Num32BitValues = blur_constant_count;
    params[5].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters     = 6;
    rs.pParameters       = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers   = &sampler;
    rs.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(info.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p->root)))) {
        return false;
    }

    static constexpr D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(vertex, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R16G16_UINT,    0, offsetof(vertex, u),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(vertex, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = p->root.Get();
    pd.VS = {g_strata_ui_vs, sizeof(g_strata_ui_vs)};
    pd.PS = {g_strata_ui_ps, sizeof(g_strata_ui_ps)};
    pd.BlendState.RenderTarget[0].BlendEnable           = TRUE;
    // the shader outputs premultiplied alpha
    pd.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask                     = 0xffffffffu;
    pd.RasterizerState.FillMode       = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode       = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable   = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.InputLayout                    = {layout, 3};
    pd.PrimitiveTopologyType          = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets               = 1;
    pd.RTVFormats[0]                  = static_cast<DXGI_FORMAT>(info.rtv_format);
    pd.SampleDesc.Count               = 1;
    if (FAILED(info.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p->pso)))) {
        return false;
    }
    pd.PS = {g_strata_ui_ps_image, sizeof(g_strata_ui_ps_image)};
    if (FAILED(info.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p->pso_image)))) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1 + max_textures + 3; // atlas, textures, then the blur snapshot and its two targets
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(info.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p->srv_heap)))) {
        return false;
    }
    p->frames_in_flight = info.frames_in_flight;

    // backdrop blur (optional: without it backdrop panels draw as flat tints)
    {
        D3D12_ROOT_PARAMETER bp[2]{};
        bp[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bp[0].Constants.ShaderRegister = 1;
        bp[0].Constants.Num32BitValues = blur_constant_count;
        bp[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;
        bp[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bp[1].DescriptorTable.NumDescriptorRanges = 1;
        bp[1].DescriptorTable.pDescriptorRanges   = &blur_range;
        bp[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC brs{};
        brs.NumParameters     = 2;
        brs.pParameters       = bp;
        brs.NumStaticSamplers = 1;
        brs.pStaticSamplers   = &sampler;
        ComPtr<ID3DBlob> bblob;
        ComPtr<ID3DBlob> berror;

        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rd.NumDescriptors = 2;
        p->rtv_size = info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC fp{};
        fp.SampleMask                      = 0xffffffffu;
        fp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        fp.RasterizerState.FillMode        = D3D12_FILL_MODE_SOLID;
        fp.RasterizerState.CullMode        = D3D12_CULL_MODE_NONE;
        fp.RasterizerState.DepthClipEnable = TRUE;
        fp.PrimitiveTopologyType           = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        fp.NumRenderTargets                = 1;
        p->blur_rt_format = internal::wide_format(static_cast<int>(info.rtv_format)) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                                    : DXGI_FORMAT_R8G8B8A8_UNORM;
        fp.RTVFormats[0]                   = p->blur_rt_format;
        fp.SampleDesc.Count                = 1;
        fp.VS = {g_strata_ui_vs_fullscreen, sizeof(g_strata_ui_vs_fullscreen)};

        D3D12_GRAPHICS_PIPELINE_STATE_DESC bd = pd; // the ui pipeline, with the backdrop shader
        bd.PS = {g_strata_ui_ps_backdrop, sizeof(g_strata_ui_ps_backdrop)};

        p->blur_supported =
            SUCCEEDED(D3D12SerializeRootSignature(&brs, D3D_ROOT_SIGNATURE_VERSION_1, &bblob, &berror)) &&
            SUCCEEDED(info.device->CreateRootSignature(0, bblob->GetBufferPointer(), bblob->GetBufferSize(), IID_PPV_ARGS(&p->root_blur))) &&
            SUCCEEDED(info.device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&p->rtv_heap))) &&
            SUCCEEDED(info.device->CreateGraphicsPipelineState(&bd, IID_PPV_ARGS(&p->pso_backdrop)));
        if (p->blur_supported) {
            fp.pRootSignature = p->root_blur.Get();
            fp.PS = {g_strata_ui_ps_blur_down, sizeof(g_strata_ui_ps_blur_down)};
            p->blur_supported = SUCCEEDED(info.device->CreateGraphicsPipelineState(&fp, IID_PPV_ARGS(&p->pso_blur_down)));
            fp.PS = {g_strata_ui_ps_blur_gauss, sizeof(g_strata_ui_ps_blur_gauss)};
            p->blur_supported = p->blur_supported && SUCCEEDED(info.device->CreateGraphicsPipelineState(&fp, IID_PPV_ARGS(&p->pso_blur_gauss)));
        }
    }

    p->srv_size = info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!p->upload_atlas(font)) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format                  = DXGI_FORMAT_R8_UNORM;
    sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels     = 1;
    info.device->CreateShaderResourceView(p->atlas.Get(), &sv, p->srv_heap->GetCPUDescriptorHandleForHeapStart());

    impl_ = std::move(p);
    return true;
}

void d3d12_renderer::set_output(const output_desc& output) noexcept
{
    if (impl_ != nullptr) { impl_->output = output; }
}

output_desc d3d12_renderer::output() const noexcept
{
    return impl_ != nullptr ? impl_->output : output_desc{};
}

bool d3d12_renderer::device_lost() const noexcept
{
    return impl_ != nullptr && FAILED(impl_->device->GetDeviceRemovedReason());
}

bool d3d12_renderer::update_atlas(const font_atlas& font)
{
    if (impl_ == nullptr || font.pixels().empty()) {
        return false;
    }
    impl& s = *impl_;
    ComPtr<ID3D12Resource> fresh;
    if (!s.upload_texture(fresh, font.width(), font.height(), DXGI_FORMAT_R8_UNORM, 1, font.pixels().data())) {
        return false;
    }
    s.atlas = std::move(fresh);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format                  = DXGI_FORMAT_R8_UNORM;
    sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels     = 1;
    s.device->CreateShaderResourceView(s.atlas.Get(), &sv, s.srv_heap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

texture_id d3d12_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id d3d12_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (impl_ == nullptr) {
        return 0;
    }
    impl& s = *impl_;
    u32 slot = 0;
    while (slot < max_textures && (s.textures[slot] != nullptr || s.texture_meta_[slot].cooldown > 0)) { ++slot; }
    if (slot == max_textures) {
        return 0;
    }
    texture_image image;
    if (!image.create(desc, pixels)) {
        return 0;
    }
    const u32 levels = image.level_count();
    const DXGI_FORMAT format = dxgi_format_of(image.layout());

    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = image.width();
    td.Height           = image.height();
    td.DepthOrArraySize = 1;
    td.MipLevels        = static_cast<UINT16>(levels);
    td.Format           = format;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ComPtr<ID3D12Resource> tex;
    const auto default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(s.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&tex)))) {
        image.wipe();
        return 0;
    }
    std::vector<texture_image::region> all;
    for (u32 k = 0; k < levels; ++k) { all.push_back({k, 0, 0, image.width(k), image.height(k)}); }
    if (!s.copy_image_regions(tex, image, all, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
        image.wipe();
        return 0;
    }
    s.textures[slot] = std::move(tex);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format                  = format;
    sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels     = levels;
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot + 1) * s.srv_size; // slot 0 of the heap is the font atlas
    s.device->CreateShaderResourceView(s.textures[slot].Get(), &sv, h);

    s.texture_meta_[slot].updatable = desc.updatable;
    if (desc.updatable) {
        s.texture_meta_[slot].image = std::move(image);
    } else {
        image.wipe(); // the gpu has its copy
    }
    return slot + 1;
}

bool d3d12_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (impl_ == nullptr || id == 0 || id > max_textures || impl_->textures[id - 1] == nullptr || !impl_->texture_meta_[id - 1].updatable) {
        return false;
    }
    impl::texture_meta& meta = impl_->texture_meta_[id - 1];
    std::vector<texture_image::region> dirty;
    if (!meta.image.update(x, y, width, height, pixels, dirty)) {
        return false;
    }
    return impl_->copy_image_regions(impl_->textures[id - 1], meta.image, dirty, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void d3d12_renderer::destroy_texture(texture_id id) noexcept
{
    if (impl_ != nullptr && id != 0 && id <= max_textures && impl_->textures[id - 1] != nullptr) {
        impl& s = *impl_;
        s.drop_pending(s.textures[id - 1].Get());
        s.retire(s.textures[id - 1]); // frames in flight may still draw it: released once they are done
        s.texture_meta_[id - 1].image.wipe();
        s.texture_meta_[id - 1].updatable = false;
        s.texture_meta_[id - 1].cooldown  = s.frames_in_flight + 1; // ... and its descriptor is not rewritten before then
    }
}

void d3d12_renderer::destroy() noexcept
{
    if (impl_ != nullptr) {
        impl_->wipe();
    }
    impl_.reset();
}

void d3d12_renderer::render(const draw_data& data, ID3D12GraphicsCommandList* list, u32 frame_index, const d3d12_target* target)
{
    if (impl_ == nullptr || data.commands.empty() || data.display_size.x <= 0 || data.display_size.y <= 0) {
        return;
    }
    impl& s = *impl_;
    s.tick_graveyard();
    if (frame_index >= s.frames.size()) {
        return;
    }
    s.record_pending(list); // staged texture copies, ahead of the draws that read them
    frame_buffers& fb = s.frames[frame_index];

    const auto vcount = static_cast<UINT>(data.vertices.size());
    const auto icount = static_cast<UINT>(data.indices.size());
    const auto scount = static_cast<UINT>(data.shapes.size());
    const UINT was_vb = fb.vb_capacity, was_ib = fb.ib_capacity, was_sh = fb.shape_capacity;
    if (!s.ensure(fb.vb, fb.vb_capacity, vcount, sizeof(vertex)) ||
        !s.ensure(fb.ib, fb.ib_capacity, icount, sizeof(index_t)) ||
        !s.ensure(fb.shapes, fb.shape_capacity, std::max(scount, 1u), sizeof(shape_record))) {
        return;
    }
    if (was_vb != fb.vb_capacity || was_ib != fb.ib_capacity || was_sh != fb.shape_capacity) {
        fb.uploaded_hash = 0; // a buffer was replaced: this slot holds nothing
    }

    // this slot may already hold these exact bytes: skip the upload. the host waited on its fence, so the gpu is done.
    if (data.content_hash == 0 || data.content_hash != fb.uploaded_hash) {
        void* mapped{};
        if (FAILED(fb.vb->Map(0, nullptr, &mapped))) { return; }
        std::memcpy(mapped, data.vertices.data(), data.vertices.size_bytes());
        fb.vb->Unmap(0, nullptr);
        if (FAILED(fb.ib->Map(0, nullptr, &mapped))) { return; }
        std::memcpy(mapped, data.indices.data(), data.indices.size_bytes());
        fb.ib->Unmap(0, nullptr);
        if (scount != 0) {
            if (FAILED(fb.shapes->Map(0, nullptr, &mapped))) { return; }
            std::memcpy(mapped, data.shapes.data(), data.shapes.size_bytes());
            fb.shapes->Unmap(0, nullptr);
        }
        fb.uploaded_hash = data.content_hash;
    }

    const D3D12_VIEWPORT vp{0.0f, 0.0f, data.display_size.x, data.display_size.y, 0.0f, 1.0f};
    const D3D12_GPU_DESCRIPTOR_HANDLE heap_start = s.srv_heap->GetGPUDescriptorHandleForHeapStart();

    // the ui pipeline state; also restores it after a blur pass
    const auto bind_main = [&] {
        list->RSSetViewports(1, &vp);
        list->SetGraphicsRootSignature(s.root.Get());
        list->SetPipelineState(s.pso.Get());
        const internal::ui_constants constants = internal::make_ui_constants(data, s.output);
        list->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        list->SetGraphicsRootDescriptorTable(1, heap_start);
        list->SetGraphicsRootDescriptorTable(3, heap_start); // t2 / t3 are only read by ps_image / ps_backdrop;
        list->SetGraphicsRootDescriptorTable(4, heap_start); // keep them valid regardless
        list->SetGraphicsRootShaderResourceView(2, fb.shapes->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW vbv{fb.vb->GetGPUVirtualAddress(), static_cast<UINT>(data.vertices.size_bytes()), sizeof(vertex)};
        const D3D12_INDEX_BUFFER_VIEW  ibv{fb.ib->GetGPUVirtualAddress(), static_cast<UINT>(data.indices.size_bytes()), DXGI_FORMAT_R16_UINT};
        list->IASetVertexBuffers(0, 1, &vbv);
        list->IASetIndexBuffer(&ibv);
        const float blend_factor[4] = {0, 0, 0, 0};
        list->OMSetBlendFactor(blend_factor);
    };
    ID3D12DescriptorHeap* heaps[] = {s.srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    bind_main();

    const bool can_blur = s.blur_supported && target != nullptr && target->resource != nullptr && target->rtv != 0;
    enum class ps_mode { main, image, backdrop };
    ps_mode    mode  = ps_mode::main;
    texture_id bound = 0;
    bool  blur_dirty  = true; // something was drawn since the blur was made
    bool  blur_ready  = false;
    float blur_radius = -1.0f;
    rect  blur_exact{};           // where the blur that was made is exact: a panel inside it can use it again
    UINT  blur_vw = 0, blur_vh = 0;

    for (const draw_cmd& cmd : data.commands) {
        if (cmd.idx_count == 0 || cmd.clip.empty()) {
            continue;
        }

        ps_mode want = cmd.texture != 0 ? ps_mode::image : ps_mode::main;
        if (cmd.blur > 0.0f && can_blur) {
            const rect panel = internal::backdrop_bounds(data, cmd);
            if (blur_dirty || cmd.blur != blur_radius || !internal::rect_inside(panel, blur_exact)) {
                blur_dirty = true;
                blur_ready = !panel.empty() && s.run_blur(list, *target, cmd.blur, panel, blur_vw, blur_vh, blur_exact);
                if (blur_ready) {
                    blur_dirty  = false;
                    blur_radius = cmd.blur;
                    bind_main(); // (this resets the pipeline to ps_main)
                    mode  = ps_mode::main;
                    bound = 0;
                }
            }
            if (blur_ready && !blur_dirty) { want = ps_mode::backdrop; }
        }

        if (want == ps_mode::image) {
            if (cmd.texture > max_textures || s.textures[cmd.texture - 1] == nullptr) {
                continue; // a texture that no longer exists: draw nothing rather than the wrong image
            }
            D3D12_GPU_DESCRIPTOR_HANDLE h = heap_start;
            h.ptr += static_cast<UINT64>(cmd.texture) * s.srv_size;
            if (mode != ps_mode::image) { list->SetPipelineState(s.pso_image.Get()); }
            if (mode != ps_mode::image || bound != cmd.texture) { list->SetGraphicsRootDescriptorTable(3, h); }
            bound = cmd.texture;
        } else if (want == ps_mode::backdrop) {
            if (mode != ps_mode::backdrop) {
                list->SetPipelineState(s.pso_backdrop.Get());
                impl::blur_constants c;
                c.bp0[0] = data.display_size.x; c.bp0[1] = data.display_size.y;
                c.bp0[2] = static_cast<float>(blur_vw) / static_cast<float>(s.blur_w);
                c.bp0[3] = static_cast<float>(blur_vh) / static_cast<float>(s.blur_h);
                list->SetGraphicsRoot32BitConstants(5, blur_constant_count, &c, 0);
                list->SetGraphicsRootDescriptorTable(4, s.gpu_slot(impl::snap_slot + 1)); // blur_tex[0]
            }
            bound = 0;
        } else if (mode != ps_mode::main) {
            list->SetPipelineState(s.pso.Get());
            bound = 0;
        }
        if (want != ps_mode::backdrop) { blur_dirty = true; }
        mode = want;

        const D3D12_RECT scissor{static_cast<LONG>(cmd.clip.min.x), static_cast<LONG>(cmd.clip.min.y),
                                 static_cast<LONG>(cmd.clip.max.x), static_cast<LONG>(cmd.clip.max.y)};
        list->RSSetScissorRects(1, &scissor);
        list->DrawIndexedInstanced(cmd.idx_count, 1, cmd.idx_offset, static_cast<INT>(cmd.vtx_offset), 0);
    }
}

} // namespace strata

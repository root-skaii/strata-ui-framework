#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

void wait_fence12(UINT64 value, DWORD timeout_ms = 2000)
{
    if (g.fence12 == nullptr || value == 0 || g.fence12->GetCompletedValue() >= value) { return; }
    if (g.fence12->SetEventOnCompletion(value, g.fence_event12) == S_OK) { ::WaitForSingleObject(g.fence_event12, timeout_ms); }
}

} // namespace

void wait_idle12()
{
    if (g.queue12 == nullptr || g.fence12 == nullptr) { return; }
    wait_fence12(g.fence_next12);
}

namespace {

void barrier12(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// descriptors (one per swap chain buffer), allocators and fence; remade when the buffers are recreated
bool ensure_targets12(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) { return false; }
    if (g.targets12_valid && d.BufferCount == g.buffers12 && d.BufferDesc.Width == g.w12 && d.BufferDesc.Height == g.h12) { return true; }

    wait_idle12();
    g.targets12_valid = false;
    g.buffers12 = d.BufferCount;
    g.w12 = d.BufferDesc.Width;
    g.h12 = d.BufferDesc.Height;
    g.format12 = d.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = g.buffers12;
    g.rtv_heap12.Reset();
    if (FAILED(g.dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap12)))) { return false; }
    g.rtv_size12 = g.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtv_heap12->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g.buffers12; ++i) {
        ComPtr<ID3D12Resource> buf;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&buf)))) { return false; }
        g.dev12->CreateRenderTargetView(buf.Get(), nullptr, h);
        h.ptr += g.rtv_size12;
    }
    if (g.frames12.size() != g.buffers12) {
        g.frames12.clear();
        g.frames12.resize(g.buffers12);
        for (auto& f : g.frames12) {
            if (FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) { return false; }
        }
    }
    g.targets12_valid = true;
    return true;
}

// the back buffer (with ui) as rgba: the copy joins the frame's command list; the caller waits and reads
struct readback12 {
    ComPtr<ID3D12Resource>             buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT                               width{}, height{};
    DXGI_FORMAT                        format{};
};

bool prepare_readback12(ID3D12Resource* back, readback12& rb)
{
    const D3D12_RESOURCE_DESC rd = back->GetDesc();
    UINT64 total = 0;
    g.dev12->GetCopyableFootprints(&rd, 0, 1, 0, &rb.footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rb.width  = static_cast<UINT>(rd.Width);
    rb.height = rd.Height;
    rb.format = rd.Format;
    return SUCCEEDED(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.buffer)));
}

void write_readback12(const readback12& rb)
{
    const bool bgra = rb.format == DXGI_FORMAT_B8G8R8A8_UNORM || rb.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = rb.format == DXGI_FORMAT_R8G8B8A8_UNORM || rb.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) { set_error("capture: back buffer format not supported"); return; }
    void* mapped = nullptr;
    if (FAILED(rb.buffer->Map(0, nullptr, &mapped))) { return; }
    std::vector<u8> px(static_cast<std::size_t>(rb.width) * rb.height * 4);
    for (UINT y = 0; y < rb.height; ++y) {
        const u8* src = static_cast<const u8*>(mapped) + rb.footprint.Offset + static_cast<std::size_t>(y) * rb.footprint.Footprint.RowPitch;
        u8* dst = px.data() + static_cast<std::size_t>(y) * rb.width * 4;
        for (UINT x = 0; x < rb.width; ++x) {
            dst[4 * x]     = bgra ? src[4 * x + 2] : src[4 * x];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = bgra ? src[4 * x] : src[4 * x + 2];
            dst[4 * x + 3] = 255;
        }
    }
    rb.buffer->Unmap(0, nullptr);
    if (!write_png(g.opt.capture_path.c_str(), px, rb.width, rb.height)) { set_error("capture: cannot write the png"); }
}

} // namespace

void render12(IDXGISwapChain* sc, const draw_data& data, bool capture)
{
    if (!ensure_targets12(sc)) { log_line("render12: no targets"); return; }
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) { return; }
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    if (idx >= g.frames12.size()) { return; }
    ID3D12CommandQueue* queue = g.queue12.Get();
    auto& fr = g.frames12[idx];
    wait_fence12(fr.fence_value); // (this frame slot's previous command list and upload buffers have finished)

    ComPtr<ID3D12Resource> back;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&back)))) { return; }
    if (FAILED(fr.alloc->Reset()) || FAILED(g.list12->Reset(fr.alloc.Get(), nullptr))) { return; }

    barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap12->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * g.rtv_size12;
    g.list12->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.renderer12.render(data, g.list12.Get(), idx % g.renderer_frames12, nullptr);

    readback12 rb;
    const bool capturing = capture && prepare_readback12(back.Get(), rb);
    if (capturing) {
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = rb.buffer.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = rb.footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = back.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        g.list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (FAILED(g.list12->Close())) { return; }
    ID3D12CommandList* lists[] = {g.list12.Get()};
    queue->ExecuteCommandLists(1, lists);
    fr.fence_value = ++g.fence_next12;
    queue->Signal(g.fence12.Get(), fr.fence_value);
    if (capturing) {
        wait_fence12(fr.fence_value);
        write_readback12(rb);
    }
}

// objects on the game's device: dropped when it replaces its device or on uninstall. the ui context survives a
// new device (windows, places, typed text)
void release_device_objects()
{
    wait_idle12();
    g.renderer.destroy();
    g.renderer12.destroy();
    g.list12.Reset();
    g.frames12.clear();
    g.rtv_heap12.Reset();
    g.fence12.Reset();
    if (g.fence_event12 != nullptr) { ::CloseHandle(g.fence_event12); g.fence_event12 = nullptr; }
    g.fence_next12    = 0;
    g.queue12.Reset();
    g.dev12.Reset();
    g.targets12_valid = false;
    g.renderer_format12 = DXGI_FORMAT_UNKNOWN;
    g.ctx.Reset();
    g.device.Reset();
    g.backend = state::api::none;
}

// the d3d12 renderer bakes the back buffer format into its pipelines; remade when it changes
bool create_renderer12(DXGI_FORMAT format, UINT buffers)
{
    wait_idle12();
    g.renderer12.destroy();
    g.renderer_frames12 = std::max<UINT>(buffers, 2);
    d3d12_init_info info;
    info.device           = g.dev12.Get();
    info.queue            = g.queue12.Get();
    info.frames_in_flight = g.renderer_frames12;
    info.rtv_format       = static_cast<u32>(format);
    if (!g.renderer12.create(info, g.ui->font())) { return false; }
    g.renderer_format12 = format;
    g.output_dirty.store(true);
    if (g.fence12 == nullptr) {
        g.fence_event12 = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<ID3D12CommandAllocator> first;
        if (g.fence_event12 == nullptr || FAILED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12))) ||
            FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&first))) ||
            FAILED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, first.Get(), nullptr, IID_PPV_ARGS(&g.list12)))) {
            return false;
        }
        g.list12->Close();
    }
    return true;
}

} // namespace strata::overlay::detail

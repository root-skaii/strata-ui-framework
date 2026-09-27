#include "gfx_host.hpp"

#include <strata/backend/d3d12.hpp>

#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;

constexpr strata::u32 frame_count = 2;
constexpr DXGI_FORMAT back_format = DXGI_FORMAT_R8G8B8A8_UNORM;

class d3d12_host final : public gfx_host {
public:
    ~d3d12_host() override
    {
        wait_idle();
        if (fence_event_ != nullptr) {
            ::CloseHandle(fence_event_);
        }
    }

    bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool vsync, const strata::font_atlas& atlas) override
    {
        vsync_ = vsync;

        UINT factory_flags = 0;
#ifndef NDEBUG
        {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
                factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#endif
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory))) ||
            FAILED(::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            return false;
        }
        device_.As(&info_queue_);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width            = width;
        sd.Height           = height;
        sd.Format           = back_format;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = frame_count;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1)) ||
            FAILED(sc1.As(&swap_chain_))) {
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = frame_count;
        if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)))) {
            return false;
        }
        rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (auto& alloc : allocators_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
                return false;
            }
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                              IID_PPV_ARGS(&list_))) ||
            FAILED(list_->Close()) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            return false;
        }
        fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr || !create_targets()) {
            return false;
        }

        strata::d3d12_init_info info;
        info.device           = device_.Get();
        info.queue            = queue_.Get();
        info.frames_in_flight = frame_count;
        info.rtv_format       = static_cast<strata::u32>(back_format);
        return ui_.create(info, atlas);
    }

    void resize(strata::u32 width, strata::u32 height) override
    {
        if (swap_chain_ == nullptr || width == 0 || height == 0) {
            return;
        }
        wait_idle();
        for (auto& rt : targets_) {
            rt.Reset();
        }
        if (SUCCEEDED(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            (void)create_targets();
        }
    }

    void render(const strata::draw_data& ui, strata::color clear) override
    {
        // wait until the gpu is done with this frame slot
        if (fence_->GetCompletedValue() < slot_fence_[slot_]) {
            fence_->SetEventOnCompletion(slot_fence_[slot_], fence_event_);
            ::WaitForSingleObject(fence_event_, INFINITE);
        }

        auto& alloc = allocators_[slot_];
        if (FAILED(alloc->Reset()) || FAILED(list_->Reset(alloc.Get(), nullptr))) {
            return;
        }

        const UINT bb = swap_chain_->GetCurrentBackBufferIndex();
        ID3D12Resource* target = targets_[bb].Get();

        transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(bb) * rtv_size_;
        const float c[4] = {clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f};
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list_->ClearRenderTargetView(rtv, c, 0, nullptr);

        const strata::d3d12_target ui_target{target, static_cast<std::uintptr_t>(rtv.ptr)};
        ui_.render(ui, list_.Get(), slot_, &ui_target);

        const bool capturing = capture_requested_ && record_capture(target);
        capture_requested_ = false;
        transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (FAILED(list_->Close())) {
            return;
        }

        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        swap_chain_->Present(vsync_ ? 1 : 0, 0);

        slot_fence_[slot_] = ++fence_value_;
        queue_->Signal(fence_.Get(), fence_value_);
        slot_ = (slot_ + 1) % frame_count;
        if (capturing) {
            wait_idle();
            finish_capture();
        }

        drain_messages();
    }

    strata::texture_id create_texture(strata::u32 width, strata::u32 height, std::span<const strata::u8> rgba) override
    {
        return ui_.create_texture(width, height, rgba);
    }
    strata::texture_id create_texture(const strata::texture_desc& desc, std::span<const strata::u8> pixels) override
    {
        return ui_.create_texture(desc, pixels);
    }
    bool update_texture(strata::texture_id id, strata::u32 x, strata::u32 y, strata::u32 width, strata::u32 height,
                        std::span<const strata::u8> pixels) override
    {
        return ui_.update_texture(id, x, y, width, height, pixels);
    }

    const char* name() const noexcept override { return "direct3d 12"; }

    bool update_atlas(const strata::font_atlas& atlas) override
    {
        wait_idle(); // the gpu must be done with the old atlas
        return ui_.update_atlas(atlas);
    }

    void request_capture() override { capture_requested_ = true; }
    bool take_capture(std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height) override
    {
        if (captured_.empty()) { return false; }
        rgba   = std::move(captured_);
        width  = captured_w_;
        height = captured_h_;
        captured_.clear();
        return true;
    }

private:
    bool create_targets()
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (strata::u32 i = 0; i < frame_count; ++i) {
            if (FAILED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&targets_[i])))) {
                return false;
            }
            device_->CreateRenderTargetView(targets_[i].Get(), nullptr, handle);
            handle.ptr += rtv_size_;
        }
        return true;
    }

    void transition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(1, &b);
    }

    // records a copy of the target into a readback buffer (target must be in RENDER_TARGET state)
    bool record_capture(ID3D12Resource* target)
    {
        const D3D12_RESOURCE_DESC td = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT   rows{};
        UINT64 row_bytes{};
        UINT64 total{};
        device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);

        if (readback_ == nullptr || readback_->GetDesc().Width < total) {
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
            readback_.Reset();
            if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(&readback_)))) {
                return false;
            }
        }
        capture_footprint_ = fp;
        captured_w_ = static_cast<strata::u32>(td.Width);
        captured_h_ = td.Height;

        transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback_.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = target;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return true;
    }

    void finish_capture()
    {
        void* mapped{};
        if (readback_ == nullptr || FAILED(readback_->Map(0, nullptr, &mapped))) { return; }
        captured_.resize(static_cast<std::size_t>(captured_w_) * captured_h_ * 4);
        for (strata::u32 y = 0; y < captured_h_; ++y) {
            std::memcpy(captured_.data() + static_cast<std::size_t>(y) * captured_w_ * 4,
                        static_cast<const strata::u8*>(mapped) + capture_footprint_.Offset +
                            static_cast<std::size_t>(y) * capture_footprint_.Footprint.RowPitch,
                        static_cast<std::size_t>(captured_w_) * 4);
        }
        readback_->Unmap(0, nullptr);
    }

    void wait_idle()
    {
        if (queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr) {
            return;
        }
        const strata::u64 value = ++fence_value_;
        queue_->Signal(fence_.Get(), value);
        if (fence_->GetCompletedValue() < value) {
            fence_->SetEventOnCompletion(value, fence_event_);
            ::WaitForSingleObject(fence_event_, INFINITE);
        }
    }

    void drain_messages()
    {
        if (info_queue_ == nullptr) {
            return;
        }
        const UINT64 count = info_queue_->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size{};
            info_queue_->GetMessage(i, nullptr, &size);
            auto* msg = static_cast<D3D12_MESSAGE*>(::HeapAlloc(::GetProcessHeap(), 0, size));
            if (msg != nullptr && SUCCEEDED(info_queue_->GetMessage(i, msg, &size))) {
                std::fprintf(stderr, "[d3d12] %s\n", msg->pDescription);
            }
            ::HeapFree(::GetProcessHeap(), 0, msg);
        }
        info_queue_->ClearStoredMessages();
    }

    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12InfoQueue>           info_queue_;
    ComPtr<ID3D12CommandQueue>        queue_;
    ComPtr<IDXGISwapChain3>           swap_chain_;
    ComPtr<ID3D12DescriptorHeap>      rtv_heap_;
    std::array<ComPtr<ID3D12Resource>, frame_count>         targets_;
    std::array<ComPtr<ID3D12CommandAllocator>, frame_count> allocators_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence>               fence_;
    HANDLE                            fence_event_{};
    std::array<strata::u64, frame_count> slot_fence_{};
    strata::u64                       fence_value_{};
    strata::u32                       slot_{};
    UINT                              rtv_size_{};
    strata::d3d12_renderer            ui_;
    bool                              vsync_{true};
    bool                              capture_requested_{};
    ComPtr<ID3D12Resource>            readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT capture_footprint_{};
    std::vector<strata::u8>           captured_;
    strata::u32                       captured_w_{}, captured_h_{};
};

} // namespace

std::unique_ptr<gfx_host> make_d3d12_host()
{
    return std::make_unique<d3d12_host>();
}

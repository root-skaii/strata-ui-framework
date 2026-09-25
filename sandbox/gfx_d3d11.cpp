#include "gfx_host.hpp"

#include <strata/backend/d3d11.hpp>

#include <windows.h>

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;

class d3d11_host final : public gfx_host {
public:
    bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool vsync, const strata::font_atlas& atlas) override
    {
        vsync_ = vsync;

        UINT flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
                                         D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
            // sdk layers not installed
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags & ~UINT{D3D11_CREATE_DEVICE_DEBUG},
                                     levels, 1, D3D11_SDK_VERSION, &device_, nullptr, &context_);
        }
        if (FAILED(hr)) {
            return false;
        }
        device_.As(&info_queue_);

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width            = width;
        sd.Height           = height;
        sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_chain_))) {
            return false;
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        return create_target() && ui_.create(device_.Get(), context_.Get(), atlas);
    }

    void resize(strata::u32 width, strata::u32 height) override
    {
        if (swap_chain_ == nullptr || width == 0 || height == 0) {
            return;
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        rtv_.Reset();
        if (SUCCEEDED(swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            (void)create_target();
        }
    }

    void render(const strata::draw_data& ui, strata::color clear) override
    {
        if (rtv_ == nullptr) {
            return;
        }
        const float c[4] = {clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f};
        ID3D11RenderTargetView* rtv = rtv_.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->ClearRenderTargetView(rtv, c);
        ui_.render(ui);
        if (capture_requested_) {
            capture_requested_ = false;
            read_back();
        }
        swap_chain_->Present(vsync_ ? 1 : 0, 0);
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

    const char* name() const noexcept override { return "direct3d 11"; }

    bool update_atlas(const strata::font_atlas& atlas) override { return ui_.update_atlas(atlas); }

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
    bool create_target()
    {
        ComPtr<ID3D11Texture2D> back;
        return SUCCEEDED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back))) &&
               SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
    }

    // copies the back buffer to a staging texture and out to captured_ (rgba8)
    void read_back()
    {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back)))) { return; }
        D3D11_TEXTURE2D_DESC desc{};
        back->GetDesc(&desc);
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.BindFlags      = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags      = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) { return; }
        context_->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { return; }
        captured_w_ = desc.Width;
        captured_h_ = desc.Height;
        captured_.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) {
            std::memcpy(captured_.data() + static_cast<std::size_t>(y) * desc.Width * 4,
                        static_cast<const strata::u8*>(map.pData) + static_cast<std::size_t>(y) * map.RowPitch,
                        static_cast<std::size_t>(desc.Width) * 4);
        }
        context_->Unmap(staging.Get(), 0);
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
            auto* msg = static_cast<D3D11_MESSAGE*>(::HeapAlloc(::GetProcessHeap(), 0, size));
            if (msg != nullptr && SUCCEEDED(info_queue_->GetMessage(i, msg, &size))) {
                std::fprintf(stderr, "[d3d11] %s\n", msg->pDescription);
            }
            ::HeapFree(::GetProcessHeap(), 0, msg);
        }
        info_queue_->ClearStoredMessages();
    }

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swap_chain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11InfoQueue>        info_queue_;
    strata::d3d11_renderer         ui_;
    bool                           vsync_{true};
    bool                           capture_requested_{};
    std::vector<strata::u8>        captured_;
    strata::u32                    captured_w_{}, captured_h_{};
};

} // namespace

std::unique_ptr<gfx_host> make_d3d11_host()
{
    return std::make_unique<d3d11_host>();
}

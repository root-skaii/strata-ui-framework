#include "xr_main.hpp"

#include <strata/backend/d3d11.hpp>
#include <strata/backend/openxr.hpp>
#include <strata/strata.hpp>
#include <strata/themes.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

// the runtime dictates which adapter the d3d11 device must be created on (xr_session::adapter_luid(), only known
// after create_instance()) -- enumerate until the LUID matches, same as any multi-gpu-aware d3d11 app would.
[[nodiscard]] ComPtr<IDXGIAdapter1> find_adapter(const LUID& luid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return nullptr; }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) { return nullptr; }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, &luid, sizeof(LUID)) == 0) { return adapter; }
    }
}

} // namespace

int run_xr_sandbox(const std::string& face, float size, int theme_index, strata::u32 panel_width, strata::u32 panel_height)
{
    strata::xr_session session;
    if (!session.create_instance()) {
        std::fprintf(stderr, "[xr] %s\n", session.last_error());
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter = find_adapter(session.adapter_luid());
    if (adapter == nullptr) {
        std::fprintf(stderr, "[xr] could not find the adapter the openxr runtime asked for\n");
        return 1;
    }

    UINT device_flags = 0;
#ifndef NDEBUG
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, device_flags, levels, 1,
                                     D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr) && (device_flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, device_flags & ~UINT{D3D11_CREATE_DEVICE_DEBUG},
                                 levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "[xr] D3D11CreateDevice failed (hr 0x%08lx)\n", static_cast<unsigned long>(hr));
        return 1;
    }

    strata::xr_panel panel;
    panel.pixel_width   = panel_width;
    panel.pixel_height  = panel_height;
    panel.width_meters  = 0.6f;
    panel.height_meters = panel.width_meters * static_cast<float>(panel_height) / static_cast<float>(panel_width);
    panel.pose.position = {0.0f, 0.0f, -1.0f}; // 1m ahead of where the session started

    if (!session.create_session(device.Get(), panel)) {
        std::fprintf(stderr, "[xr] %s\n", session.last_error());
        return 1;
    }

    strata::context_config config;
    config.font.face         = face;
    config.font.pixel_height = size;
    if (theme_index >= 0) {
        const auto names = strata::themes::names();
        if (static_cast<std::size_t>(theme_index) < names.size()) { (void)strata::themes::by_name(names[static_cast<std::size_t>(theme_index)], config.theme); }
    }
    std::expected<strata::context, strata::font_error> created = strata::context::create(config);
    if (!created) {
        std::fprintf(stderr, "[xr] failed to build the font atlas (font_error %d)\n", static_cast<int>(created.error()));
        return 1;
    }
    strata::context ui = std::move(*created);

    strata::d3d11_renderer renderer;
    if (!renderer.create(device.Get(), context.Get(), ui.font())) {
        std::fprintf(stderr, "[xr] d3d11_renderer::create failed\n");
        return 1;
    }

    std::fprintf(stderr, "[xr] session created, panel %ux%u (%.2fx%.2fm) -- waiting for the headset\n",
                 panel_width, panel_height, panel.width_meters, panel.height_meters);

    int counter = 0;
    bool toggle_value = false;

    for (;;) {
        if (!session.poll_events()) { break; }
        if (!session.session_running()) { ::Sleep(16); continue; }

        bool should_render = false;
        if (!session.begin_frame(should_render)) { break; }

        strata::input_state input{};
        input.display_size = {static_cast<float>(panel_width), static_cast<float>(panel_height)};
        input.delta_time    = 1.0f / 90.0f;
        input = strata::xr_pointer_input(session.controllers(), session.panel(), input);

        ui.begin_frame(std::move(input));
        if (auto w = ui.window("Strata VR", {24.0f, 24.0f},
                               {static_cast<float>(panel_width) - 48.0f, static_cast<float>(panel_height) - 48.0f},
                               strata::window_flags::no_move | strata::window_flags::no_collapse)) {
            ui.text("strata rendering inside a headset via openxr");
            ui.separator();
            ui.textf("button presses: {}", counter);
            if (ui.button("Press me")) { ++counter; }
            ui.checkbox("a checkbox", toggle_value);
        }
        ui.end_frame();

        bool did_render = false;
        if (should_render) {
            if (ID3D11Texture2D* target = session.acquire_target(); target != nullptr) {
                ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(device->CreateRenderTargetView(target, nullptr, &rtv))) {
                    ID3D11RenderTargetView* rtv_raw = rtv.Get();
                    const float clear[4] = {0.05f, 0.05f, 0.08f, 1.0f};
                    context->OMSetRenderTargets(1, &rtv_raw, nullptr);
                    context->ClearRenderTargetView(rtv_raw, clear);
                    renderer.render(ui.render_data());
                    did_render = true;
                }
            }
        }
        session.end_frame(did_render);
    }

    std::fprintf(stderr, "[xr] session ended\n");
    return 0;
}

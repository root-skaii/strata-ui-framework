#include "openvr_main.hpp"

#include <strata/backend/d3d11.hpp>
#include <strata/backend/openvr.hpp>
#include <strata/strata.hpp>
#include <strata/themes.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <openvr.h>
#include <span>
#include <wrl/client.h>

namespace {

// rotates v by q, for the debug fwd=... readout below (not exposed by the library).
[[nodiscard]] strata::vr_vec3 rotate(strata::vr_quat q, strata::vr_vec3 v) noexcept
{
    const strata::vr_vec3 qv{q.x, q.y, q.z};
    const strata::vr_vec3 t{qv.y * v.z - qv.z * v.y, qv.z * v.x - qv.x * v.z, qv.x * v.y - qv.y * v.x};
    const strata::vr_vec3 t2{t.x + v.x * q.w, t.y + v.y * q.w, t.z + v.z * q.w};
    const strata::vr_vec3 c{qv.y * t2.z - qv.z * t2.y, qv.z * t2.x - qv.x * t2.z, qv.x * t2.y - qv.y * t2.x};
    return {v.x + 2.0f * c.x, v.y + 2.0f * c.y, v.z + 2.0f * c.z};
}

} // namespace

int run_openvr_sandbox(const std::string& face, float size, int theme_index, strata::u32 panel_width, strata::u32 panel_height)
{
    using Microsoft::WRL::ComPtr;

    if (strata::detect_vr() == strata::vr_kind::openxr) {
        std::fprintf(stderr, "[openvr] openxr_loader.dll is already loaded in this process -- run --xr instead\n");
    }

    UINT device_flags = 0;
#ifndef NDEBUG
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags, levels, 1,
                                     D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr) && (device_flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags & ~UINT{D3D11_CREATE_DEVICE_DEBUG},
                                 levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "[openvr] D3D11CreateDevice failed (hr 0x%08lx)\n", static_cast<unsigned long>(hr));
        return 1;
    }

    float panel_distance_m = 1.2f; // how far in front of the hmd the head-locked panel sits; left joystick adjusts it

    strata::vr_panel panel;
    panel.width_meters      = 0.6f;
    panel.head_locked       = true;
    panel.pose.position     = {0.0f, 0.0f, -panel_distance_m}; // hmd-local offset: straight ahead (see vr_panel)

    strata::vr_overlay overlay;
    if (!overlay.create("strata.sandbox", "Strata Sandbox", panel)) {
        std::fprintf(stderr, "[openvr] %s\n", overlay.last_error());
        return 1;
    }

    strata::context_config config;
    config.font.face         = face;
    config.font.pixel_height = size;
    if (theme_index >= 0) {
        const auto names = strata::themes::names();
        if (static_cast<std::size_t>(theme_index) < names.size()) {
            (void)strata::themes::by_name(names[static_cast<std::size_t>(theme_index)], config.theme);
        }
    }
    std::expected<strata::context, strata::font_error> created = strata::context::create(config);
    if (!created) {
        std::fprintf(stderr, "[openvr] failed to build the font atlas (font_error %d)\n", static_cast<int>(created.error()));
        return 1;
    }
    strata::context ui = std::move(*created);

    strata::d3d11_renderer renderer;
    if (!renderer.create(device.Get(), context.Get(), ui.font())) {
        std::fprintf(stderr, "[openvr] d3d11_renderer::create failed\n");
        return 1;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = panel_width;
    td.Height           = panel_height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &texture))) {
        std::fprintf(stderr, "[openvr] CreateTexture2D failed\n");
        return 1;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv))) {
        std::fprintf(stderr, "[openvr] CreateRenderTargetView failed\n");
        return 1;
    }

    overlay.show();
    std::fprintf(stderr, "[openvr] overlay created, panel %ux%u (%.2fm wide) -- open SteamVR to see it. Esc quits.\n",
                 panel_width, panel_height, panel.width_meters);

    int  counter       = 0;
    bool toggle_value  = false;

    for (;;) {
        if (vr::IVRSystem* sys = vr::VRSystem(); sys != nullptr) {
            vr::VREvent_t ev{};
            while (sys->PollNextEvent(&ev, sizeof(ev))) {
                if (ev.eventType == vr::VREvent_Quit) {
                    sys->AcknowledgeQuit_Exiting();
                    std::fprintf(stderr, "[openvr] steamvr asked this app to quit\n");
                    return 0;
                }
            }
        }
        if ((::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) { break; }

        overlay.update_controllers();
        const std::span<const strata::vr_controller> controllers = overlay.controllers();

        // debug control: left joystick/trackpad y pushes the panel closer/farther, smoothly (frame_dt-scaled, not
        // a step per press) -- remove once there's a real settings ui for panel distance.
        bool distance_changed = false;
        if (!controllers.empty()) {
            constexpr float joystick_deadzone = 0.15f;
            constexpr float zoom_speed_mps    = 1.5f; // metres/second of travel at full stick deflection
            constexpr float min_distance_m    = 0.4f;
            constexpr float max_distance_m    = 3.5f;
            constexpr float frame_dt          = 1.0f / 90.0f; // matches the Sleep(11) pacing below
            const float     stick_y           = controllers[0].joystick.y;
            if (std::fabs(stick_y) > joystick_deadzone) {
                panel_distance_m += stick_y * zoom_speed_mps * frame_dt;
                panel_distance_m  = std::clamp(panel_distance_m, min_distance_m, max_distance_m);
                distance_changed  = true;
            }
        }

        // head_locked panel: steamvr itself keeps this glued to the hmd's latest pose every frame (see vr_panel),
        // so unlike a world-space panel this only needs re-submitting when the offset itself actually changes.
        if (distance_changed) {
            panel.pose.position.z = -panel_distance_m;
            overlay.set_panel(panel);
        }

        strata::input_state input{};
        input.display_size = {static_cast<float>(panel_width), static_cast<float>(panel_height)};
        input.delta_time   = 1.0f / 60.0f;
        input = strata::vr_pointer_input(controllers, input);
        const strata::vec2 mouse_pos_debug  = input.mouse_pos; // input is moved into begin_frame below
        const bool          mouse_down_debug = input.mouse_down[0];

        ui.begin_frame(std::move(input));
        if (auto w = ui.window("Strata OpenVR", {24.0f, 24.0f},
                               {static_cast<float>(panel_width) - 48.0f, static_cast<float>(panel_height) - 48.0f},
                               strata::window_flags::no_move | strata::window_flags::no_collapse)) {
            ui.text("strata rendering into a SteamVR overlay");
            ui.separator();
            ui.textf("button presses: {}", counter);
            const bool pressed = ui.button("Press me");
            const bool press_hovered = ui.item_hovered();
            const strata::rect press_rect = ui.item_rect();
            if (pressed) { ++counter; }
            ui.checkbox("a checkbox", toggle_value);
            ui.separator();
            ui.textf("debug: panel hmd-local offset=({:.2f},{:.2f},{:.2f}) dist={:.2f}m input_system={}",
                    overlay.panel().pose.position.x, overlay.panel().pose.position.y,
                    overlay.panel().pose.position.z, panel_distance_m, overlay.input_system_ready());
            ui.textf("debug: mouse=({:.0f},{:.0f}) down={} press_hovered={}", mouse_pos_debug.x, mouse_pos_debug.y,
                    mouse_down_debug, press_hovered);
            ui.textf("debug: press_rect=({:.0f},{:.0f})-({:.0f},{:.0f})", press_rect.min.x, press_rect.min.y,
                    press_rect.max.x, press_rect.max.y);
            for (std::size_t i = 0; i < controllers.size(); ++i) {
                const strata::vr_controller& c = controllers[i];
                const strata::vr_vec3 fwd = rotate(c.aim.orientation, {0.0f, 0.0f, -1.0f});
                ui.textf("{}: trk={} hit={} trig={} tip={} uv=({:.2f},{:.2f})", i == 0 ? "L" : "R", c.tracked,
                        c.hit, c.trigger_down, c.aim_from_tip, c.hit_uv.x, c.hit_uv.y);
                ui.textf("   pos=({:.2f},{:.2f},{:.2f}) fwd=({:.2f},{:.2f},{:.2f})", c.aim.position.x,
                        c.aim.position.y, c.aim.position.z, fwd.x, fwd.y, fwd.z);
                ui.textf("   quat=({:.2f},{:.2f},{:.2f},{:.2f})", c.aim.orientation.x, c.aim.orientation.y,
                        c.aim.orientation.z, c.aim.orientation.w);
            }
        }
        ui.end_frame();

        ID3D11RenderTargetView* rtv_raw = rtv.Get();
        const float clear[4]            = {0.0f, 0.0f, 0.0f, 0.0f}; // alpha 0: transparent outside the window itself
        context->OMSetRenderTargets(1, &rtv_raw, nullptr);
        context->ClearRenderTargetView(rtv_raw, clear);
        renderer.render(ui.render_data());
        overlay.submit_texture(texture.Get());

        ::Sleep(11); // ~90 hz; nothing to wait on for pacing, unlike xrWaitFrame
    }

    std::fprintf(stderr, "[openvr] closed\n");
    return 0;
}

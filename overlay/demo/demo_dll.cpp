// sample overlay dll: inject into a d3d11 game (overlay/tools/injector.cpp or any injector) and press F1. shows a
// docked tabbed window, a log other threads write to (the C api below), a theme switcher and self-removal.
//
//   STRATA_OVERLAY_SHOW=1        start with the overlay open
//   STRATA_OVERLAY_CAPTURE=path  write a png of the frame (ui included) after 30 visible frames (for tests)

#include <strata/overlay/overlay.hpp>
#if STRATA_HAS_OPENVR
#include <strata/backend/d3d11.hpp>
#include <strata/backend/openvr.hpp>
#endif

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#if STRATA_HAS_OPENVR
#include <d3d11.h>
#include <openvr.h>
#include <optional>
#include <wrl/client.h>
#endif

namespace {

using namespace strata;
#if STRATA_HAS_OPENVR
using Microsoft::WRL::ComPtr;
#endif

log_queue                                        g_log_in{10000}; // lines from any thread (strata_overlay_log)
log_buffer                                       g_log{2000};
int                                              g_tab   = 0;
int                                              g_theme = 0;
std::atomic<bool>                                g_eject{false}; // (written by the render thread, read by the worker: not a plain bool)
bool                                             g_demo_flag = true;
float                                            g_slider    = 0.35f;
HMODULE                                          g_module{};

#if STRATA_HAS_OPENVR
// openvr example: a second, independent strata panel mirrored into a SteamVR overlay, built off the same d3d11
// device the flat overlay is already attached to (overlay::device11()). only attempted, and only updated, while
// the flat overlay is visible -- overlay_ui() (this file's only render-thread hook) is not called otherwise, so
// the vr panel blinks with F1 too. a real integration wanting the vr panel independent of the flat one would need
// strata::overlay to expose an "every frame regardless of visibility" hook, which it does not today.
constexpr strata::u32 vr_panel_width  = 800;
constexpr strata::u32 vr_panel_height = 500;
bool                            g_vr_tried = false;
bool                            g_vr_ready = false; // g_vr_ui/g_vr_renderer/g_vr_texture/g_vr_rtv are all live iff this is true
vr_overlay                      g_vr;
std::optional<context>          g_vr_ui;
d3d11_renderer                  g_vr_renderer;
ComPtr<ID3D11Texture2D>         g_vr_texture;
ComPtr<ID3D11RenderTargetView>  g_vr_rtv;

void update_vr_overlay()
{
    auto* device = static_cast<ID3D11Device*>(overlay::device11());
    if (device == nullptr) { return; } // d3d12 game, or not attached yet

    if (!g_vr_ready) {
        if (g_vr_tried || detect_vr() != vr_kind::openvr) { return; } // tried and failed, or steamvr isn't up
        g_vr_tried = true;

        vr_panel panel;
        panel.width_meters  = 0.5f;
        panel.pose.position = {0.3f, 1.6f, -0.8f}; // placeholder; corrected below once vr_init has run
        if (!g_vr.create("strata.overlay.demo", "Strata Overlay Demo", panel)) {
            g_log.addf(log_level::warn, "openvr overlay: {}", g_vr.last_error());
            return;
        }
        // TrackingUniverseStanding's origin is the room's chaperone origin, not the user -- place the panel
        // relative to the hmd's current pose instead (see vr_pose_in_front_of_hmd); leaves the placeholder if
        // the hmd isn't tracked yet.
        if (const auto p = vr_pose_in_front_of_hmd(0.5f)) {
            panel.pose = *p;
            g_vr.set_panel(panel);
        }

        context_config cfg;
        cfg.font.face         = "Segoe UI";
        cfg.font.pixel_height = 14.0f;
        std::expected<context, font_error> created = context::create(cfg);
        ComPtr<ID3D11DeviceContext> ctx;
        device->GetImmediateContext(&ctx);
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = vr_panel_width;
        td.Height           = vr_panel_height;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!created ||
            FAILED(device->CreateTexture2D(&td, nullptr, &g_vr_texture)) ||
            FAILED(device->CreateRenderTargetView(g_vr_texture.Get(), nullptr, &g_vr_rtv))) {
            g_log.add(log_level::warn, "openvr overlay: failed to set up its own font/texture, staying off");
            g_vr.destroy();
            return;
        }
        g_vr_ui.emplace(std::move(*created));
        if (!g_vr_renderer.create(device, ctx.Get(), g_vr_ui->font())) {
            g_log.add(log_level::warn, "openvr overlay: d3d11_renderer::create failed, staying off");
            g_vr_ui.reset();
            g_vr.destroy();
            return;
        }
        g_vr.show();
        g_vr_ready = true;
        g_log.addf(log_level::info, "openvr overlay created ({}x{})", vr_panel_width, vr_panel_height);
    }

    g_vr.update_controllers();
    input_state input{};
    input.display_size = {static_cast<f32>(vr_panel_width), static_cast<f32>(vr_panel_height)};
    input.delta_time    = 1.0f / 60.0f;
    input = vr_pointer_input(g_vr.controllers(), input);

    g_vr_ui->begin_frame(std::move(input));
    if (auto w = g_vr_ui->window("VR", {20.0f, 20.0f}, {vr_panel_width - 40.0f, vr_panel_height - 40.0f},
                                 window_flags::no_move)) {
        g_vr_ui->text("the strata overlay, mirrored into a SteamVR panel");
        g_vr_ui->textf("game frames: {}", overlay::frames());
    }
    g_vr_ui->end_frame();

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    ID3D11RenderTargetView* rtv   = g_vr_rtv.Get();
    const float              clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // alpha 0: transparent outside the window itself
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);
    g_vr_renderer.render(g_vr_ui->render_data());
    g_vr.submit_texture(g_vr_texture.Get());
}
#endif // STRATA_HAS_OPENVR

[[nodiscard]] std::string env(const char* name)
{
    char buf[512];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string{buf, n} : std::string{};
}

void overlay_ui(context& ui)
{
    (void)g_log_in.drain_into(g_log); // lines from other threads (strata_overlay_log) join the log on the render thread
#if STRATA_HAS_OPENVR
    update_vr_overlay(); // openvr example: see its own comment above
#endif

    if (auto w = ui.window("strata overlay", {40.0f, 40.0f}, {460.0f, 0.0f}, window_flags::resizable)) {
        (void)ui.tab_bar("tabs", {"Info", "Log", "Style"}, g_tab);
        ui.spacing();
        if (g_tab == 0) {
            ui.textf("window        {:.0f} x {:.0f}", ui.display_size().x, ui.display_size().y);
            ui.textf("frames drawn  {}", overlay::frames());
            static f64 last_time = 0.0;
            static f32 fps = 0.0f;
            const f64 now = ui.time();
            if (now > last_time) { fps = fps * 0.9f + 0.1f * static_cast<f32>(1.0 / (now - last_time)); }
            last_time = now;
            ui.textf("game / ui     {:.0f} fps", fps);
            ui.separator();
            ui.text_wrapped_colored(ui.theme().text_dim, "F1 hides the overlay. the ui is drawn into the game's back buffer just before Present (direct3d 11 and 12).");
            ui.checkbox("a checkbox", g_demo_flag);
            ui.slider("a slider", g_slider, 0.0f, 1.0f);
            ui.spacing();
            if (ui.button("write a log line")) {
                g_log.addf(log_level::info, "hello from the overlay ({} frames so far)", overlay::frames());
                g_tab = 1;
            }
            ui.same_line();
            if (ui.button("unload overlay")) { g_eject.store(true); }
        } else if (g_tab == 1) {
            ui.log_view("log", g_log, {0.0f, 240.0f});
        } else {
            std::vector<std::string_view> names;
            for (const std::string_view n : themes::names()) { names.push_back(n); }
            if (ui.combo("theme", g_theme, names.data(), names.size())) { (void)themes::by_name(names[static_cast<std::size_t>(g_theme)], ui.theme()); }
            ui.text_dim("themes, fonts and every widget of strata are available here");
        }
    }
}

DWORD WINAPI worker(LPVOID)
{
    overlay::options opt;
    opt.ui = overlay_ui;
    opt.start_visible = env("STRATA_OVERLAY_SHOW") == "1";
    opt.capture_path  = env("STRATA_OVERLAY_CAPTURE");
    if (const std::string f = env("STRATA_OVERLAY_CAPTURE_FRAME"); !f.empty()) { opt.capture_frame = static_cast<unsigned>(std::atoi(f.c_str())); }
    if (!overlay::install(opt)) {
        std::string msg = std::string{"strata overlay: "} + overlay::last_error() + "\n";
        ::OutputDebugStringA(msg.c_str());
        ::FreeLibraryAndExitThread(g_module, 1);
    }
    while (!g_eject.load()) { ::Sleep(100); }
    overlay::uninstall();
    ::FreeLibraryAndExitThread(g_module, 0); // (only when the window procedure could be restored: see uninstall())
}

} // namespace

extern "C" {

__declspec(dllexport) void strata_overlay_show(int visible) { overlay::show(visible != 0); }
__declspec(dllexport) int  strata_overlay_visible() { return overlay::visible() ? 1 : 0; }
__declspec(dllexport) unsigned long long strata_overlay_frames() { return overlay::frames(); }
__declspec(dllexport) const char* strata_overlay_last_error() { return overlay::last_error(); }
__declspec(dllexport) int  strata_overlay_attached() { return overlay::attached() ? 1 : 0; }
// 0 srgb, 1 srgb view, 2 scRGB, 3 HDR10 (strata::output_space)
__declspec(dllexport) int  strata_overlay_output_space() { return static_cast<int>(overlay::output_space_in_use()); }
__declspec(dllexport) void strata_overlay_eject() { g_eject.store(true); }
// level: 0 trace, 1 debug, 2 info, 3 warn, 4 error. callable from any thread
__declspec(dllexport) void strata_overlay_log(int level, const char* utf8)
{
    if (utf8 == nullptr) { return; }
    g_log_in.add(static_cast<log_level>(std::clamp(level, 0, 4)), utf8);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        if (HANDLE t = ::CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) { ::CloseHandle(t); } // (nothing heavy under the loader lock)
    }
    return TRUE;
}

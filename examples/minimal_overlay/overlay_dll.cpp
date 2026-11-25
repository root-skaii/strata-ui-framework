// Minimal in-game overlay DLL using strata::overlay (D3D11, D3D12, or OpenGL).
// Inject into a local test host and press F1 to toggle the tool window.

#include <strata/overlay/overlay.hpp>
#include <strata/themes.hpp>

#include <windows.h>

#include <atomic>

namespace {

HMODULE           g_module{};
std::atomic<bool> g_eject{false};
bool              g_show_hud = true;
float             g_scale    = 100.0f;

DWORD WINAPI overlay_thread(LPVOID)
{
    strata::overlay::options opt;
    opt.backend       = strata::overlay::backend_kind::auto_detect;
    opt.toggle_key    = strata::key::f1;
    opt.scale_hotkeys = true;

    opt.configure_context = [](strata::context_config& cfg) {
        cfg.theme = strata::themes::glass();
    };

    // Always-on HUD (renders even while the interactive menu is hidden).
    opt.hud = [](strata::context& ui) {
        if (!g_show_hud) { return; }
        auto& dl = ui.draw();
        dl.rect_filled({16, 16, 176, 44}, {18, 22, 30, 190}, 6.0f);
        dl.text({26, 23}, {235, 240, 248, 255}, "strata overlay (F1)");
    };

    // Interactive panel shown when toggled open.
    opt.ui = [](strata::context& ui) {
        if (auto w = ui.window("Overlay Tool", {40, 64}, {320, 0})) {
            ui.textf("Frames drawn: {}", strata::overlay::frames());
            ui.checkbox("Show HUD badge", g_show_hud);
            if (ui.slider("UI scale (%)", g_scale, 75.0f, 200.0f)) {
                strata::overlay::set_ui_scale_percent(static_cast<int>(g_scale + 0.5f));
            }
            ui.separator();
            if (ui.button("Unload DLL")) {
                g_eject.store(true, std::memory_order_relaxed);
            }
        }
    };

    if (!strata::overlay::install(opt)) {
        FreeLibraryAndExitThread(g_module, 1);
    }

    while (!g_eject.load(std::memory_order_relaxed)) {
        Sleep(50);
    }

    // Retry while a hooked call is in flight; never unload if another hook chained over ours.
    for (;;) {
        const auto res = strata::overlay::uninstall();
        if (res == strata::overlay::uninstall_result::done) {
            FreeLibraryAndExitThread(g_module, 0);
        }
        if (res == strata::overlay::uninstall_result::pinned) {
            return 0; // stay loaded as a passive pass-through
        }
        Sleep(100);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
    }
    return TRUE;
}

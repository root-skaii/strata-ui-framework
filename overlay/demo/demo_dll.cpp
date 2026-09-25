// the sample overlay dll: load it into a direct3d 11 game (overlay/tools/injector.cpp, or any injector) and press F1.
// it shows the pieces a real tool needs: a docked window with tabs, a log other threads can write to (the C api below), a
// theme switcher, and a way to take the overlay out again.
//
//   STRATA_OVERLAY_SHOW=1        start with the overlay open
//   STRATA_OVERLAY_CAPTURE=path  write a png of the frame (ui included) after 30 visible frames (for tests)

#include <strata/overlay/overlay.hpp>

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace strata;

std::mutex                                       g_log_mutex;
std::vector<std::pair<log_level, std::string>>   g_pending_log;
log_buffer                                       g_log{2000};
int                                              g_tab   = 0;
int                                              g_theme = 0;
std::atomic<bool>                                g_eject{false}; // (written by the render thread, read by the worker: not a plain bool)
bool                                             g_demo_flag = true;
float                                            g_slider    = 0.35f;
HMODULE                                          g_module{};

[[nodiscard]] std::string env(const char* name)
{
    char buf[512];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string{buf, n} : std::string{};
}

void overlay_ui(context& ui)
{
    {   // lines other threads wrote (strata_overlay_log) join the log here, on the render thread
        const std::lock_guard lock{g_log_mutex};
        for (auto& [level, text] : g_pending_log) { g_log.add(level, text); }
        g_pending_log.clear();
    }

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
__declspec(dllexport) void strata_overlay_eject() { g_eject.store(true); }
// level: 0 trace, 1 debug, 2 info, 3 warn, 4 error. callable from any thread
__declspec(dllexport) void strata_overlay_log(int level, const char* utf8)
{
    if (utf8 == nullptr) { return; }
    const std::lock_guard lock{g_log_mutex};
    if (g_pending_log.size() < 10000) { g_pending_log.emplace_back(static_cast<log_level>(std::clamp(level, 0, 4)), utf8); }
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

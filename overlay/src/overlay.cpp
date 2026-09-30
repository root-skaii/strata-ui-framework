// the public functions of overlay.hpp; the work is in the other files of this directory (see internal.hpp)

#include "internal.hpp"

namespace strata::overlay {

using namespace detail;

// ---- public ---------------------------------------------------------------------------------------------------------------

bool install(const options& opt)
{
    if (g.installed.exchange(true)) {
        return true;
    }
    g.opt = opt;
    g.visible.store(opt.start_visible);
    g.draw_hidden.store(opt.draw_hidden || static_cast<bool>(opt.hud));
    switch (install_hooks()) {
    case hook_result::ok:           return true;
    case hook_result::no_vtable:    g.installed.store(false); return false;
    case hook_result::patch_failed: uninstall(); return false;
    }
    return false;
}

uninstall_result uninstall()
{
    log_line("uninstall");
    if (!g.installed.load()) { return g.stay_loaded ? uninstall_result::pinned : uninstall_result::done; }
    // hidden first: the cursor comes back to the game on its own thread
    if (g.hwnd != nullptr && g.subclassed && ::IsWindow(g.hwnd)) {
        g.visible.store(false);
        DWORD_PTR ignored = 0;
        ::SendMessageTimeoutW(g.hwnd, wm_show_changed, 0, 0, SMTO_ABORTIFHUNG, 500, &ignored);
    }
    g.shutting_down.store(true);
    const bool hooks_restored = remove_hooks();
    bool proc_restored = true;
    if (g.subclassed && ::IsWindow(g.hwnd)) {
        if (reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g.hwnd, GWLP_WNDPROC)) == &wnd_proc) {
            ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.orig_proc));
            g.subclassed = false; // (a retry after a timeout below must not see its own restore as someone else's)
        } else {
            proc_restored = false;
        }
    }
    log_line(hooks_restored ? "uninstall: hooks restored" : "uninstall: a hook was chained over ours; it passes through");
    for (int i = 0; i < 100 && g.in_hook.load() > 0; ++i) { ::Sleep(20); } // (a Present on another thread may still be inside a hook)
    if (g.in_hook.load() > 0) { // freeing now would pull the device objects and ui out from under that call
        set_error("a hooked call is still running after 2 s: nothing was freed; call uninstall() again later");
        return uninstall_result::busy;
    }
    ::Sleep(50); // (a call that read the vtable just before it was restored has not counted itself in yet)
    if (!proc_restored) {
        set_error("the window was subclassed on top of the overlay: it stays as a pass-through, do not unload the dll");
        g.stay_loaded = true;
        return uninstall_result::pinned; // (the state has to outlive the window procedure)
    }
    release_device_objects();
    g.last_queue.store(nullptr);
    g.ui.reset();
    g.chain      = nullptr;
    g.hwnd       = nullptr;
    g.subclassed = false;
    g.installed.store(false);
    g.stay_loaded = !hooks_restored;
    if (!hooks_restored) {
        set_error("another hook was installed over the overlay's: it stays as a pass-through, do not unload the dll");
    }
    log_line("uninstall: done");
    return hooks_restored ? uninstall_result::done : uninstall_result::pinned;
}

void show(bool v)
{
    g.visible.store(v);
    if (g.hwnd != nullptr && g.subclassed) { ::PostMessageW(g.hwnd, wm_show_changed, 0, 0); }
}

bool visible() noexcept { return g.visible.load(); }

void set_draw_hidden(bool on) noexcept { g.draw_hidden.store(on); }

void post(std::function<void()> fn)
{
    if (!fn) { return; }
    const std::lock_guard lock{g.post_mutex};
    g.posted.push_back(std::move(fn));
}

void set_ui_scale(float scale) noexcept
{
    g.want_scale.store(std::clamp(scale, 0.5f, 4.0f));
}

float ui_scale() noexcept
{
    const float wanted = g.want_scale.load();
    if (wanted > 0.0f) { return wanted; } // asked for, not applied yet
    return g.ui != nullptr ? g.ui->scale() : g.base_scale;
}
unsigned long long frames() noexcept { return g.frame_count.load(); }
const char* last_error() noexcept { return g.error; }
output_space output_space_in_use() noexcept { return static_cast<output_space>(g.output_now.load()); }
bool attached() noexcept { return g.chain != nullptr && g.subclassed; }
void* window() noexcept { return g.hwnd; }
void* device11() noexcept { return g.backend == state::api::d3d11 ? static_cast<void*>(g.device.Get()) : nullptr; }


} // namespace strata::overlay

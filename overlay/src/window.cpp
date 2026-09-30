#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

[[nodiscard]] bool is_mouse_message(UINT m) noexcept { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSEHWHEEL || m == WM_XBUTTONDBLCLK; }
[[nodiscard]] bool is_key_message(UINT m) noexcept
{
    return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR || m == WM_DEADCHAR || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_SYSCHAR ||
           m == WM_UNICHAR;
}

void apply_cursor_state(bool now_visible)
{
    if (now_visible) {
        // the game hid the cursor: show it (the counter is per thread; this runs on the window's thread)
        if (g.cursor_shown == 0) {
            int count = ::ShowCursor(TRUE);
            g.cursor_shown = 1;
            for (int guard = 0; count < 0 && guard < 32; ++guard) { count = ::ShowCursor(TRUE); ++g.cursor_shown; }
        }
        ::ClipCursor(nullptr);
    } else {
        while (g.cursor_shown > 0) { ::ShowCursor(FALSE); --g.cursor_shown; }
    }
}

void toggle_visible()
{
    log_line("toggle: %d -> %d", g.visible.load() ? 1 : 0, g.visible.load() ? 0 : 1);
    show(!g.visible.load());
}

} // namespace

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const WNDPROC next = g.orig_proc;
    if (g.shutting_down.load()) {
        return ::CallWindowProcW(next, hwnd, msg, wparam, lparam);
    }
    if (msg == wm_show_changed) {
        apply_cursor_state(g.visible.load());
        return 0;
    }
    if (g.opt.toggle_key != key::none && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) && wparam == static_cast<WPARAM>(g.opt.toggle_key)) {
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (lparam & (1 << 30)) == 0) { toggle_visible(); }
        return 0;
    }
    if (g.visible.load()) {
        {
            const std::lock_guard lock{g.platform_mutex};
            (void)g.platform.handle_message(hwnd, msg, wparam, static_cast<std::intptr_t>(lparam));
        }
        if (win32_platform::swallows(msg)) { return 0; }
        const bool block_all = g.opt.block_game_input;
        if (msg == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
            const std::lock_guard lock{g.platform_mutex};
            if (g.platform.apply_cursor()) { return TRUE; }
        }
        if (is_mouse_message(msg) && (block_all || g.capture_mouse.load())) { return 0; }
        if (is_key_message(msg) && (block_all || g.capture_keys.load())) { return 0; }
        if (msg == WM_INPUT && block_all) { return ::DefWindowProcW(hwnd, msg, wparam, lparam); } // (raw mouse for a camera: cleaned up, not delivered)
    }
    return ::CallWindowProcW(next, hwnd, msg, wparam, lparam);
}

void subclass_window(HWND hwnd)
{
    if (g.hwnd == hwnd && g.subclassed) { return; }
    // a game window going away: restore its wndproc if it still has ours
    if (g.subclassed && g.hwnd != nullptr && ::IsWindow(g.hwnd) &&
        reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g.hwnd, GWLP_WNDPROC)) == &wnd_proc) {
        ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.orig_proc));
    }
    g.hwnd = hwnd;
    {
        const std::lock_guard lock{g.platform_mutex};
        g.platform.attach(g.hwnd);
    }
    if (g.ui != nullptr) { g.ui->set_clipboard(g.platform.clipboard()); }
    g.orig_proc  = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc)));
    g.subclassed = g.orig_proc != nullptr;
    if (!g.subclassed) { set_error("could not subclass the game's window"); }
}

} // namespace strata::overlay::detail

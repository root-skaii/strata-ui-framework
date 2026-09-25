#pragma once

#include "strata/context.hpp"

#include <cstdint>

namespace strata {

// translates win32 window messages into an input_state. windows.h is not
// included here; handles travel as void* and message params as integers.
class win32_platform {
public:
    void attach(void* hwnd) noexcept;

    // feed every message from your wndproc. returns true if the message was
    // recognised (it does not mean it must be swallowed).
    bool handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam) noexcept;

    // true for the IME messages the platform consumes completely: return 0 from the window procedure instead of calling
    // DefWindowProc (the ui draws the composition itself). call it after handle_message()
    [[nodiscard]] static bool swallows(std::uint32_t msg) noexcept;

    // call every frame after end_frame: platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height());
    // the IME is active only while a text field has the keyboard, so hotkeys keep working under a CJK layout
    void set_ime(bool wanted, vec2 caret_bottom_left, f32 caret_height) noexcept;

    // snapshot for context::begin_frame
    [[nodiscard]] input_state new_frame() noexcept;

    // the dpi scale of the monitor the window is on (1.0 = 96 dpi, 1.5 = 144 dpi, ...); 1.0 before attach().
    // the process should be per-monitor dpi aware; on WM_DPICHANGED call context::set_scale(dpi_scale()).
    [[nodiscard]] f32 dpi_scale() const noexcept;

    // pointer shape: pass ui.cursor() after end_frame, and call apply_cursor() from WM_SETCURSOR when the hit
    // test says HTCLIENT (return TRUE from the window procedure when it returns true)
    void set_cursor(cursor_kind k) noexcept { cursor_ = k; }
    [[nodiscard]] bool apply_cursor() noexcept;

    // clipboard access for text fields: ui.set_clipboard(platform.clipboard()).
    // this object must stay at a fixed address while the hooks are in use.
    [[nodiscard]] clipboard_hooks clipboard() noexcept;

private:
    void push_key(key k, bool ctrl, bool shift) noexcept;
    void push_text(char32_t cp) noexcept;

    void*               hwnd_{};
    vec2                mouse_{-1.0e6f, -1.0e6f};
    std::array<bool, 3> down_{};
    // a press+release inside one frame would be invisible to polling: the release is deferred to the next frame
    std::array<bool, 3> release_pending_{};
    std::array<bool, 3> press_seen_{};
    f32                 wheel_{};
    std::int64_t        last_ticks_{};
    bool                tracking_leave_{};

    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    char16_t                              high_surrogate_{};
    cursor_kind                           cursor_{};
    u32                                   pressed_key_{};
    std::array<char, 256>                 ime_{};      // the composition in progress (utf-8) ...
    u32                                   ime_len_{};
    u32                                   ime_cursor_{}; // ... and the caret inside it
    bool                                  ime_enabled_{true};
    vec2                                  ime_pos_{-1.0f, -1.0f};
};

} // namespace strata

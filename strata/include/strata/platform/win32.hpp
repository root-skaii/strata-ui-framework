#pragma once

#include "strata/context.hpp"

#include <cstdint>
#include <utility>

namespace strata {

// translates win32 messages into an input_state. no windows.h: handles are void*, params integers.
class win32_platform {
public:
    void attach(void* hwnd) noexcept;

    // feed every wndproc message. true if recognised (not necessarily to be swallowed).
    bool handle_message(void* hwnd, std::uint32_t msg, std::uintptr_t wparam, std::intptr_t lparam) noexcept;

    // IME messages fully consumed: return 0 instead of DefWindowProc (the ui draws the composition).
    // call after handle_message()
    [[nodiscard]] static bool swallows(std::uint32_t msg) noexcept;

    // call every frame after end_frame: platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height());
    // IME is on only while a text field has focus, so hotkeys work under CJK layouts
    void set_ime(bool wanted, vec2 caret_bottom_left, f32 caret_height) noexcept;

    // snapshot for context::begin_frame
    [[nodiscard]] input_state new_frame() noexcept;

    // the window's monitor dpi scale (1.0 = 96 dpi); 1.0 before attach(). be per-monitor dpi aware and call
    // context::set_scale(dpi_scale()) on WM_DPICHANGED.
    [[nodiscard]] f32 dpi_scale() const noexcept;

    // pass ui.cursor() after end_frame; call apply_cursor() on WM_SETCURSOR with HTCLIENT (return TRUE if it does)
    void set_cursor(cursor_kind k) noexcept { cursor_ = k; }
    [[nodiscard]] bool apply_cursor() noexcept;

    // user appearance: dark / light, high contrast, accent (alpha 0 if none).
    // ui.theme() = themes::for_appearance(a.dark, a.high_contrast, a.accent) follows them
    struct appearance_settings {
        bool  dark{true};
        bool  high_contrast{};
        color accent{0, 0, 0, 0};
    };
    [[nodiscard]] static appearance_settings appearance() noexcept;
    // true once after WM_SETTINGCHANGE / WM_SYSCOLORCHANGE: re-read appearance() and re-apply the theme between frames
    [[nodiscard]] bool appearance_changed() noexcept { return std::exchange(appearance_changed_, false); }

    // text-field clipboard: ui.set_clipboard(platform.clipboard()). keep this object at a fixed address while in use.
    [[nodiscard]] clipboard_hooks clipboard() noexcept;

private:
    // key / extra mouse button down: queued with the modifiers held now
    void push_press(key k) noexcept;
    void push_text(char32_t cp) noexcept;

    void*               hwnd_{};
    vec2                mouse_{-1.0e6f, -1.0e6f};
    std::array<bool, 3> down_{};
    // a press + release within one frame would be missed by polling: the release waits a frame
    std::array<bool, 3> release_pending_{};
    std::array<bool, 3> press_seen_{};
    f32                 wheel_{};
    f32                 wheel_x_{};
    std::int64_t        last_ticks_{};
    bool                tracking_leave_{};

    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    char16_t                              high_surrogate_{};
    cursor_kind                           cursor_{};
    std::array<char, 256>                 ime_{};      // the composition in progress (utf-8) ...
    u32                                   ime_len_{};
    u32                                   ime_cursor_{}; // ... and the caret inside it
    bool                                  ime_enabled_{true};
    vec2                                  ime_pos_{-1.0f, -1.0f};
    bool                                  appearance_changed_{};
};

} // namespace strata

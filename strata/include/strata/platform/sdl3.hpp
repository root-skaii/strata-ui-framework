#pragma once

// header-only SDL3 platform adapter satisfying strata::platform_backend.
// include <SDL3/SDL.h> before this header.

#ifndef SDL_h_
#error "Include <SDL3/SDL.h> before <strata/platform/sdl3.hpp>"
#endif

#include "strata/backend/backend.hpp"
#include "strata/context.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace strata {

class sdl3_platform {
public:
    sdl3_platform() noexcept = default;
    ~sdl3_platform() { detach(); }

    sdl3_platform(const sdl3_platform&)            = delete;
    sdl3_platform& operator=(const sdl3_platform&) = delete;

    void attach(SDL_Window* window) noexcept
    {
        detach();
        window_  = window;
        last_ns_ = SDL_GetTicksNS();
        if (window_ == nullptr) { return; }

        cursors_[static_cast<std::size_t>(cursor_kind::arrow)]       = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        cursors_[static_cast<std::size_t>(cursor_kind::text)]        = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        cursors_[static_cast<std::size_t>(cursor_kind::hand)]        = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        cursors_[static_cast<std::size_t>(cursor_kind::not_allowed)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_ew)]   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_ns)]   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_nwse)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_nesw)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
    }

    void detach() noexcept
    {
        for (SDL_Cursor*& c : cursors_) {
            if (c != nullptr) {
                SDL_DestroyCursor(c);
                c = nullptr;
            }
        }
        window_ = nullptr;
    }

    bool handle_event(const SDL_Event& ev)
    {
        switch (ev.type) {
        case SDL_EVENT_MOUSE_MOTION:
            cursor_logical_ = {ev.motion.x, ev.motion.y};
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const SDL_Keymod mod = SDL_GetModState();
            const bool ctrl  = (mod & SDL_KMOD_CTRL) != 0;
            const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
            const bool alt   = (mod & SDL_KMOD_ALT) != 0;
            int idx = -1;
            if (ev.button.button == SDL_BUTTON_LEFT)        { idx = 0; }
            else if (ev.button.button == SDL_BUTTON_RIGHT)  { idx = 1; }
            else if (ev.button.button == SDL_BUTTON_MIDDLE) { idx = 2; }
            if (idx >= 0) {
                const auto i = static_cast<std::size_t>(idx);
                if (down) {
                    down_[i]            = true;
                    press_seen_[i]      = true;
                    release_pending_[i] = false;
                } else if (press_seen_[i]) {
                    release_pending_[i] = true;
                } else {
                    down_[i] = false;
                }
                if (idx == 2) {
                    held_.set_held(key::mouse_middle, down);
                    if (down && key_count_ < keys_.size()) { keys_[key_count_++] = {key::mouse_middle, ctrl, shift, alt}; }
                }
                return true;
            }
            if (ev.button.button == SDL_BUTTON_X1 || ev.button.button == SDL_BUTTON_X2) {
                const key k = ev.button.button == SDL_BUTTON_X1 ? key::mouse_x1 : key::mouse_x2;
                held_.set_held(k, down);
                if (down && key_count_ < keys_.size()) { keys_[key_count_++] = {k, ctrl, shift, alt}; }
                return true;
            }
            return false;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            wheel_   += ev.wheel.y;
            wheel_x_ += ev.wheel.x;
            return true;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const key k = translate_key(ev.key.key);
            if (k == key::none) { return false; }
            const bool down = ev.type == SDL_EVENT_KEY_DOWN;
            held_.set_held(k, down);
            if (down && key_count_ < keys_.size()) {
                keys_[key_count_++] = {k, (ev.key.mod & SDL_KMOD_CTRL) != 0,
                                       (ev.key.mod & SDL_KMOD_SHIFT) != 0, (ev.key.mod & SDL_KMOD_ALT) != 0};
            }
            return true;
        }
        case SDL_EVENT_TEXT_INPUT:
            if (ev.text.text != nullptr) {
                const std::size_t len = std::strlen(ev.text.text);
                if (typed_len_ + len <= typed_.size()) {
                    std::memcpy(typed_.data() + typed_len_, ev.text.text, len);
                    typed_len_ += static_cast<u32>(len);
                }
            }
            ime_len_ = 0;
            return true;
        case SDL_EVENT_TEXT_EDITING:
            if (ev.edit.text != nullptr) {
                const std::size_t len = std::min<std::size_t>(std::strlen(ev.edit.text), ime_.size());
                std::memcpy(ime_.data(), ev.edit.text, len);
                ime_len_    = static_cast<u32>(len);
                ime_cursor_ = static_cast<u32>(std::clamp<int>(ev.edit.start, 0, static_cast<int>(len)));
            } else {
                ime_len_ = 0;
            }
            return true;
        case SDL_EVENT_DROP_FILE:
            if (ev.drop.data != nullptr) { pending_drops_.emplace_back(ev.drop.data); }
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] input_state new_frame() noexcept
    {
        input_state out;
        const std::uint64_t now = SDL_GetTicksNS();
        const double        dt  = static_cast<double>(now - last_ns_) * 1.0e-9;
        last_ns_                = now;
        out.delta_time          = dt > 0.0 && dt < 1.0 ? static_cast<f32>(dt) : 1.0f / 60.0f;

        if (window_ != nullptr) {
            int pw = 0, ph = 0, lw = 0, lh = 0;
            SDL_GetWindowSizeInPixels(window_, &pw, &ph);
            SDL_GetWindowSize(window_, &lw, &lh);
            out.display_size = {static_cast<f32>(pw), static_cast<f32>(ph)};
            const f32 sx     = lw > 0 ? static_cast<f32>(pw) / static_cast<f32>(lw) : 1.0f;
            const f32 sy     = lh > 0 ? static_cast<f32>(ph) / static_cast<f32>(lh) : 1.0f;
            out.mouse_pos    = {cursor_logical_.x * sx, cursor_logical_.y * sy};
        }

        const SDL_Keymod mod = SDL_GetModState();
        out.ctrl       = (mod & SDL_KMOD_CTRL) != 0;
        out.shift      = (mod & SDL_KMOD_SHIFT) != 0;
        out.alt        = (mod & SDL_KMOD_ALT) != 0;
        out.mouse_down = down_;
        for (std::size_t i = 0; i < 3; ++i) {
            press_seen_[i] = false;
            if (release_pending_[i]) {
                down_[i]            = false;
                release_pending_[i] = false;
            }
        }
        out.wheel      = wheel_;
        out.wheel_x    = wheel_x_;
        wheel_         = 0.0f;
        wheel_x_       = 0.0f;
        out.keys       = keys_;
        out.key_count  = key_count_;
        key_count_     = 0;
        out.typed      = typed_;
        out.typed_len  = typed_len_;
        typed_len_     = 0;
        out.ime        = ime_;
        out.ime_len    = ime_len_;
        out.ime_cursor = ime_cursor_;
        out.keys_held  = held_.keys_held;

        frame_drops_.swap(pending_drops_);
        pending_drops_.clear();
        return out;
    }

    [[nodiscard]] f32 dpi_scale() const noexcept
    {
        if (window_ == nullptr) { return 1.0f; }
        const float s = SDL_GetWindowDisplayScale(window_);
        return s > 0.0f ? s : 1.0f;
    }

    void set_cursor(cursor_kind k) noexcept
    {
        if (k == active_cursor_) { return; }
        active_cursor_ = k;
        SDL_Cursor* c  = cursors_[static_cast<std::size_t>(k)];
        if (c != nullptr) { SDL_SetCursor(c); }
    }

    [[nodiscard]] clipboard_hooks clipboard() noexcept
    {
        return {
            [](void*, std::string_view text) noexcept {
                std::string z{text};
                SDL_SetClipboardText(z.c_str());
            },
            [](void*, std::string& out) noexcept -> bool {
                char* s = SDL_GetClipboardText();
                if (s == nullptr) { return false; }
                out.assign(s);
                SDL_free(s);
                return true;
            },
            this,
        };
    }

    void set_ime(bool wanted, vec2 caret_bottom_left, f32 caret_height) noexcept
    {
        if (window_ == nullptr) { return; }
        if (wanted != ime_active_) {
            ime_active_ = wanted;
            if (wanted) { SDL_StartTextInput(window_); }
            else        { SDL_StopTextInput(window_); ime_len_ = 0; }
        }
        if (wanted) {
            const f32 inv = 1.0f / std::max(dpi_scale(), 0.25f);
            SDL_Rect r{static_cast<int>(caret_bottom_left.x * inv),
                       static_cast<int>((caret_bottom_left.y - caret_height) * inv),
                       2, static_cast<int>(std::max(caret_height * inv, 1.0f))};
            SDL_SetTextInputArea(window_, &r, 0);
        }
    }

    [[nodiscard]] std::span<const std::string> dropped_files() const noexcept { return frame_drops_; }

    [[nodiscard]] static key translate_key(SDL_Keycode sk) noexcept
    {
        if (sk >= SDLK_A && sk <= SDLK_Z)     { return static_cast<key>(static_cast<u8>(key::a) + (sk - SDLK_A)); }
        if (sk >= SDLK_0 && sk <= SDLK_9)     { return static_cast<key>(static_cast<u8>(key::d0) + (sk - SDLK_0)); }
        if (sk >= SDLK_F1 && sk <= SDLK_F12)  { return static_cast<key>(static_cast<u8>(key::f1) + (sk - SDLK_F1)); }
        if (sk >= SDLK_F13 && sk <= SDLK_F24) { return static_cast<key>(static_cast<u8>(key::f13) + (sk - SDLK_F13)); }
        switch (sk) {
        case SDLK_BACKSPACE:    return key::backspace;
        case SDLK_TAB:          return key::tab;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:     return key::enter;
        case SDLK_PAUSE:        return key::pause;
        case SDLK_CAPSLOCK:     return key::caps_lock;
        case SDLK_ESCAPE:       return key::escape;
        case SDLK_SPACE:        return key::space;
        case SDLK_PAGEUP:       return key::page_up;
        case SDLK_PAGEDOWN:     return key::page_down;
        case SDLK_END:          return key::end;
        case SDLK_HOME:         return key::home;
        case SDLK_LEFT:         return key::left;
        case SDLK_UP:           return key::up;
        case SDLK_RIGHT:        return key::right;
        case SDLK_DOWN:         return key::down;
        case SDLK_PRINTSCREEN:  return key::print_screen;
        case SDLK_INSERT:       return key::insert;
        case SDLK_DELETE:       return key::del;
        case SDLK_KP_0:         return key::num0;
        case SDLK_KP_1:         return key::num1;
        case SDLK_KP_2:         return key::num2;
        case SDLK_KP_3:         return key::num3;
        case SDLK_KP_4:         return key::num4;
        case SDLK_KP_5:         return key::num5;
        case SDLK_KP_6:         return key::num6;
        case SDLK_KP_7:         return key::num7;
        case SDLK_KP_8:         return key::num8;
        case SDLK_KP_9:         return key::num9;
        case SDLK_KP_MULTIPLY:  return key::num_multiply;
        case SDLK_KP_PLUS:      return key::num_add;
        case SDLK_KP_MINUS:     return key::num_subtract;
        case SDLK_KP_PERIOD:    return key::num_decimal;
        case SDLK_KP_DIVIDE:    return key::num_divide;
        case SDLK_NUMLOCKCLEAR: return key::num_lock;
        case SDLK_SCROLLLOCK:   return key::scroll_lock;
        case SDLK_SEMICOLON:    return key::semicolon;
        case SDLK_EQUALS:       return key::equals;
        case SDLK_COMMA:        return key::comma;
        case SDLK_MINUS:        return key::minus;
        case SDLK_PERIOD:       return key::period;
        case SDLK_SLASH:        return key::slash;
        case SDLK_GRAVE:        return key::grave;
        case SDLK_LEFTBRACKET:  return key::left_bracket;
        case SDLK_BACKSLASH:    return key::backslash;
        case SDLK_RIGHTBRACKET: return key::right_bracket;
        case SDLK_APOSTROPHE:   return key::apostrophe;
        default:                return key::none;
        }
    }

private:
    SDL_Window*                           window_{};
    std::array<SDL_Cursor*, 8>            cursors_{};
    cursor_kind                           active_cursor_{cursor_kind::arrow};
    vec2                                  cursor_logical_{-1.0e6f, -1.0e6f};
    std::array<bool, 3>                   down_{};
    std::array<bool, 3>                   release_pending_{};
    std::array<bool, 3>                   press_seen_{};
    f32                                   wheel_{};
    f32                                   wheel_x_{};
    std::uint64_t                         last_ns_{};
    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    std::array<char, 256>                 ime_{};
    u32                                   ime_len_{};
    u32                                   ime_cursor_{};
    bool                                  ime_active_{};
    input_state                           held_{};
    std::vector<std::string>              pending_drops_;
    std::vector<std::string>              frame_drops_;
};

static_assert(platform_backend<sdl3_platform>);

} // namespace strata

#pragma once

// header-only GLFW 3.3+ platform adapter satisfying strata::platform_backend.
// include <GLFW/glfw3.h> before this header.

#ifndef GLFW_VERSION_MAJOR
#error "Include <GLFW/glfw3.h> before <strata/platform/glfw.hpp>"
#endif

#include "strata/backend/backend.hpp"
#include "strata/context.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace strata {

class glfw_platform {
public:
    glfw_platform() noexcept = default;
    ~glfw_platform() { detach(); }

    glfw_platform(const glfw_platform&)            = delete;
    glfw_platform& operator=(const glfw_platform&) = delete;

    void attach(GLFWwindow* window) noexcept
    {
        detach();
        window_    = window;
        last_time_ = window_ != nullptr ? glfwGetTime() : 0.0;
        if (window_ == nullptr) { return; }

        cursors_[static_cast<std::size_t>(cursor_kind::arrow)]       = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        cursors_[static_cast<std::size_t>(cursor_kind::text)]        = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
        cursors_[static_cast<std::size_t>(cursor_kind::hand)]        = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_ew)]   = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_ns)]   = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
#ifdef GLFW_NOT_ALLOWED_CURSOR
        cursors_[static_cast<std::size_t>(cursor_kind::not_allowed)] = glfwCreateStandardCursor(GLFW_NOT_ALLOWED_CURSOR);
#endif
#ifdef GLFW_RESIZE_NWSE_CURSOR
        cursors_[static_cast<std::size_t>(cursor_kind::resize_nwse)] = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
        cursors_[static_cast<std::size_t>(cursor_kind::resize_nesw)] = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);
#endif
    }

    void detach() noexcept
    {
        for (GLFWcursor*& c : cursors_) {
            if (c != nullptr) {
                glfwDestroyCursor(c);
                c = nullptr;
            }
        }
        window_ = nullptr;
    }

    void on_key(int glfw_key, int /*scancode*/, int action, int mods) noexcept
    {
        const key k = translate_key(glfw_key);
        if (k == key::none) { return; }
        const bool down = action == GLFW_PRESS || action == GLFW_REPEAT;
        held_.set_held(k, down);
        if (down && key_count_ < keys_.size()) {
            keys_[key_count_++] = {k, (mods & GLFW_MOD_CONTROL) != 0, (mods & GLFW_MOD_SHIFT) != 0, (mods & GLFW_MOD_ALT) != 0};
        }
    }

    void on_char(unsigned int codepoint) noexcept
    {
        if (codepoint < 0x20u || codepoint == 0x7fu || codepoint > 0x10ffffu) { return; }
        char buf[4];
        u32  n = 0;
        if (codepoint < 0x80u) {
            buf[0] = static_cast<char>(codepoint);
            n = 1;
        } else if (codepoint < 0x800u) {
            buf[0] = static_cast<char>(0xc0u | (codepoint >> 6));
            buf[1] = static_cast<char>(0x80u | (codepoint & 0x3fu));
            n = 2;
        } else if (codepoint < 0x10000u) {
            buf[0] = static_cast<char>(0xe0u | (codepoint >> 12));
            buf[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu));
            buf[2] = static_cast<char>(0x80u | (codepoint & 0x3fu));
            n = 3;
        } else {
            buf[0] = static_cast<char>(0xf0u | (codepoint >> 18));
            buf[1] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu));
            buf[2] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu));
            buf[3] = static_cast<char>(0x80u | (codepoint & 0x3fu));
            n = 4;
        }
        if (typed_len_ + n <= typed_.size()) {
            std::memcpy(typed_.data() + typed_len_, buf, n);
            typed_len_ += n;
        }
    }

    void on_mouse_button(int button, int action, int mods) noexcept
    {
        const bool down = action == GLFW_PRESS;
        if (button >= 0 && button <= 2) {
            const auto i = static_cast<std::size_t>(button);
            if (down) {
                down_[i]            = true;
                press_seen_[i]      = true;
                release_pending_[i] = false;
            } else if (press_seen_[i]) {
                release_pending_[i] = true;
            } else {
                down_[i] = false;
            }
            if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
                held_.set_held(key::mouse_middle, down);
                if (down && key_count_ < keys_.size()) {
                    keys_[key_count_++] = {key::mouse_middle, (mods & GLFW_MOD_CONTROL) != 0,
                                           (mods & GLFW_MOD_SHIFT) != 0, (mods & GLFW_MOD_ALT) != 0};
                }
            }
        } else if (button == GLFW_MOUSE_BUTTON_4 || button == GLFW_MOUSE_BUTTON_5) {
            const key k = button == GLFW_MOUSE_BUTTON_4 ? key::mouse_x1 : key::mouse_x2;
            held_.set_held(k, down);
            if (down && key_count_ < keys_.size()) {
                keys_[key_count_++] = {k, (mods & GLFW_MOD_CONTROL) != 0, (mods & GLFW_MOD_SHIFT) != 0, (mods & GLFW_MOD_ALT) != 0};
            }
        }
    }

    void on_scroll(double xoffset, double yoffset) noexcept
    {
        wheel_x_ += static_cast<f32>(xoffset);
        wheel_   += static_cast<f32>(yoffset);
    }

    void on_cursor_pos(double x, double y) noexcept
    {
        cursor_logical_ = {static_cast<f32>(x), static_cast<f32>(y)};
        has_cursor_     = true;
    }

    void on_drop(int count, const char** paths)
    {
        for (int i = 0; i < count; ++i) {
            if (paths[i] != nullptr) { pending_drops_.emplace_back(paths[i]); }
        }
    }

    [[nodiscard]] input_state new_frame() noexcept
    {
        input_state out;
        const double now = glfwGetTime();
        const double dt  = now - last_time_;
        last_time_       = now;
        out.delta_time   = dt > 0.0 && dt < 1.0 ? static_cast<f32>(dt) : 1.0f / 60.0f;

        if (window_ != nullptr) {
            int fb_w = 0, fb_h = 0, win_w = 0, win_h = 0;
            glfwGetFramebufferSize(window_, &fb_w, &fb_h);
            glfwGetWindowSize(window_, &win_w, &win_h);
            out.display_size = {static_cast<f32>(fb_w), static_cast<f32>(fb_h)};

            if (!has_cursor_) {
                double cx = 0.0, cy = 0.0;
                glfwGetCursorPos(window_, &cx, &cy);
                cursor_logical_ = {static_cast<f32>(cx), static_cast<f32>(cy)};
            }
            const f32 sx = win_w > 0 ? static_cast<f32>(fb_w) / static_cast<f32>(win_w) : 1.0f;
            const f32 sy = win_h > 0 ? static_cast<f32>(fb_h) / static_cast<f32>(win_h) : 1.0f;
            out.mouse_pos = {cursor_logical_.x * sx, cursor_logical_.y * sy};

            out.ctrl  = glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            out.shift = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
            out.alt   = glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
        }

        out.mouse_down = down_;
        for (std::size_t i = 0; i < 3; ++i) {
            press_seen_[i] = false;
            if (release_pending_[i]) {
                down_[i]            = false;
                release_pending_[i] = false;
            }
        }
        out.wheel     = wheel_;
        out.wheel_x   = wheel_x_;
        wheel_        = 0.0f;
        wheel_x_      = 0.0f;
        out.keys      = keys_;
        out.key_count = key_count_;
        key_count_    = 0;
        out.typed     = typed_;
        out.typed_len = typed_len_;
        typed_len_    = 0;
        out.keys_held = held_.keys_held;

        frame_drops_.swap(pending_drops_);
        pending_drops_.clear();
        return out;
    }

    [[nodiscard]] f32 dpi_scale() const noexcept
    {
        if (window_ == nullptr) { return 1.0f; }
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(window_, &sx, &sy);
        return sx > 0.0f ? sx : 1.0f;
    }

    void set_cursor(cursor_kind k) noexcept
    {
        if (window_ == nullptr || k == active_cursor_) { return; }
        active_cursor_ = k;
        GLFWcursor* c = cursors_[static_cast<std::size_t>(k)];
        glfwSetCursor(window_, c != nullptr ? c : cursors_[0]);
    }

    [[nodiscard]] clipboard_hooks clipboard() noexcept
    {
        return {
            [](void* user, std::string_view text) noexcept {
                auto* self = static_cast<glfw_platform*>(user);
                if (self->window_ == nullptr) { return; }
                std::string z{text};
                glfwSetClipboardString(self->window_, z.c_str());
            },
            [](void* user, std::string& out) noexcept -> bool {
                auto* self = static_cast<glfw_platform*>(user);
                if (self->window_ == nullptr) { return false; }
                const char* s = glfwGetClipboardString(self->window_);
                if (s == nullptr) { return false; }
                out.assign(s);
                return true;
            },
            this,
        };
    }

    void set_ime(bool /*wanted*/, vec2 /*caret_bottom_left*/, f32 /*caret_height*/) noexcept {}

    [[nodiscard]] std::span<const std::string> dropped_files() const noexcept { return frame_drops_; }

    [[nodiscard]] static key translate_key(int gk) noexcept
    {
        if (gk >= GLFW_KEY_A && gk <= GLFW_KEY_Z)   { return static_cast<key>(static_cast<u8>(key::a) + (gk - GLFW_KEY_A)); }
        if (gk >= GLFW_KEY_0 && gk <= GLFW_KEY_9)   { return static_cast<key>(static_cast<u8>(key::d0) + (gk - GLFW_KEY_0)); }
        if (gk >= GLFW_KEY_KP_0 && gk <= GLFW_KEY_KP_9) { return static_cast<key>(static_cast<u8>(key::num0) + (gk - GLFW_KEY_KP_0)); }
        if (gk >= GLFW_KEY_F1 && gk <= GLFW_KEY_F24){ return static_cast<key>(static_cast<u8>(key::f1) + (gk - GLFW_KEY_F1)); }
        switch (gk) {
        case GLFW_KEY_BACKSPACE:     return key::backspace;
        case GLFW_KEY_TAB:           return key::tab;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:      return key::enter;
        case GLFW_KEY_PAUSE:         return key::pause;
        case GLFW_KEY_CAPS_LOCK:     return key::caps_lock;
        case GLFW_KEY_ESCAPE:        return key::escape;
        case GLFW_KEY_SPACE:         return key::space;
        case GLFW_KEY_PAGE_UP:       return key::page_up;
        case GLFW_KEY_PAGE_DOWN:     return key::page_down;
        case GLFW_KEY_END:           return key::end;
        case GLFW_KEY_HOME:          return key::home;
        case GLFW_KEY_LEFT:          return key::left;
        case GLFW_KEY_UP:            return key::up;
        case GLFW_KEY_RIGHT:         return key::right;
        case GLFW_KEY_DOWN:          return key::down;
        case GLFW_KEY_PRINT_SCREEN:  return key::print_screen;
        case GLFW_KEY_INSERT:        return key::insert;
        case GLFW_KEY_DELETE:        return key::del;
        case GLFW_KEY_KP_MULTIPLY:   return key::num_multiply;
        case GLFW_KEY_KP_ADD:        return key::num_add;
        case GLFW_KEY_KP_SUBTRACT:   return key::num_subtract;
        case GLFW_KEY_KP_DECIMAL:    return key::num_decimal;
        case GLFW_KEY_KP_DIVIDE:     return key::num_divide;
        case GLFW_KEY_NUM_LOCK:      return key::num_lock;
        case GLFW_KEY_SCROLL_LOCK:   return key::scroll_lock;
        case GLFW_KEY_SEMICOLON:     return key::semicolon;
        case GLFW_KEY_EQUAL:         return key::equals;
        case GLFW_KEY_COMMA:         return key::comma;
        case GLFW_KEY_MINUS:         return key::minus;
        case GLFW_KEY_PERIOD:        return key::period;
        case GLFW_KEY_SLASH:         return key::slash;
        case GLFW_KEY_GRAVE_ACCENT:  return key::grave;
        case GLFW_KEY_LEFT_BRACKET:  return key::left_bracket;
        case GLFW_KEY_BACKSLASH:     return key::backslash;
        case GLFW_KEY_RIGHT_BRACKET: return key::right_bracket;
        case GLFW_KEY_APOSTROPHE:    return key::apostrophe;
        default:                     return key::none;
        }
    }

private:
    GLFWwindow*                           window_{};
    std::array<GLFWcursor*, 8>            cursors_{};
    cursor_kind                           active_cursor_{cursor_kind::arrow};
    vec2                                  cursor_logical_{-1.0e6f, -1.0e6f};
    bool                                  has_cursor_{};
    std::array<bool, 3>                   down_{};
    std::array<bool, 3>                   release_pending_{};
    std::array<bool, 3>                   press_seen_{};
    f32                                   wheel_{};
    f32                                   wheel_x_{};
    double                                last_time_{};
    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    input_state                           held_{};
    std::vector<std::string>              pending_drops_;
    std::vector<std::string>              frame_drops_;
};

static_assert(platform_backend<glfw_platform>);

} // namespace strata

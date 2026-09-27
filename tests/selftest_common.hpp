#pragma once

// shared by selftest_*.cpp: the check macro and a harness feeding strata::context scripted input

#include <strata/strata.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace strata;

inline int g_failures = 0;
inline int g_checks   = 0;

inline void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL  line %d: %s\n", line, what);
    }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

inline bool near_eq(f32 a, f32 b, f32 eps = 2.0f) { return std::abs(a - b) <= eps; }

struct clip_store {
    std::string data;
};
inline void clip_set(void* user, std::string_view text) noexcept { static_cast<clip_store*>(user)->data.assign(text); }
inline bool clip_get(void* user, std::string& out) noexcept
{
    out = static_cast<clip_store*>(user)->data;
    return true;
}

// one context plus the scripted input it is fed
struct harness {
    context     ui;
    input_state in;
    clip_store  clip;

    harness() : harness(context_config{}) {}
    explicit harness(const context_config& cfg) : ui{context::create(cfg).value()}
    {
        in.display_size = {800.0f, 600.0f};
        in.mouse_pos    = {-500.0f, -500.0f};
        ui.set_clipboard({&clip_set, &clip_get, &clip});
        ui.set_dock_animation(false); // the checks look at where panes end up, not on the way there
        ui.set_scroll_smoothing(false); // ... and where a scroll ends up
    }

    template <class F>
    void frame(F&& build, f32 dt = 1.0f / 60.0f)
    {
        in.delta_time = dt;
        ui.begin_frame(in);
        build();
        ui.end_frame();
        in.key_count   = 0;
        in.typed_len   = 0;
        in.wheel       = 0.0f;
        in.pressed_key = 0;
        in.press_count = 0;
    }
    // a key press queued the way win32_platform does it: with the modifiers held for it
    void press(u32 vk, bool ctrl = false, bool shift = false, bool alt = false)
    {
        if (in.press_count < in.presses.size()) { in.presses[in.press_count++] = {vk, ctrl, shift, alt}; }
    }
    template <class F>
    void frames(F&& build, int n, f32 dt = 1.0f / 60.0f)
    {
        for (int i = 0; i < n; ++i) { frame(build, dt); }
    }

    void move(vec2 p) { in.mouse_pos = p; }
    void down() { in.mouse_down[0] = true; }
    void up() { in.mouse_down[0] = false; }
    // right / middle buttons, scripted like the left one
    void down(int button) { in.mouse_down[static_cast<std::size_t>(button)] = true; }
    void up(int button) { in.mouse_down[static_cast<std::size_t>(button)] = false; }
    // wheel notches this frame (positive = scroll up), and sideways for a tilt wheel
    void wheel(f32 notches) { in.wheel = notches; }
    void wheel_x(f32 notches) { in.wheel_x = notches; }
    void modifiers(bool ctrl = false, bool shift = false, bool alt = false)
    {
        in.ctrl = ctrl;
        in.shift = shift;
        in.alt = alt;
    }
    void key(strata::key k, bool ctrl = false, bool shift = false)
    {
        if (in.key_count < in.keys.size()) { in.keys[in.key_count++] = {k, ctrl, shift}; }
    }
    void type(std::string_view s)
    {
        for (const char c : s) {
            if (in.typed_len < in.typed.size()) { in.typed[in.typed_len++] = c; }
        }
    }

    // move onto a point, press, release: a click that focuses whatever sits there
    template <class F>
    void click(vec2 p, F&& build)
    {
        move(p);
        frame(build); // the ui reads hover from the previous frame: let it see the pointer first
        frame(build);
        down();
        frame(build);
        up();
        frame(build);
    }

    // the same with the right button: what opens a context menu
    template <class F>
    void right_click(vec2 p, F&& build)
    {
        move(p);
        frame(build);
        frame(build);
        down(1);
        frame(build);
        up(1);
        frame(build);
    }

    // one wheel gesture over a point: hover first (scrollers use last frame's rects), then notches, then a frame
    // without them so the scroll is not applied twice
    template <class F>
    void scroll(vec2 p, f32 notches, F&& build)
    {
        move(p);
        frame(build);
        frame(build);
        wheel(notches);
        frame(build);
        frame(build);
    }

    // ... and sideways (a tilt wheel / trackpad swipe): positive is towards the right
    template <class F>
    void scroll_x(vec2 p, f32 notches, F&& build)
    {
        move(p);
        frame(build);
        frame(build);
        wheel_x(notches);
        frame(build);
        frame(build);
    }
};

constexpr window_flags plain_window = window_flags::no_title_bar | window_flags::no_move;


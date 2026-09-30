#pragma once

// internal: per-window persistent state (window_state), the window table, stacking order and this frame's
// hover/focus trackers. included via context_impl.hpp; context::window_state (declared in context.hpp) aliases
// internal::window_state below, so the handful of files holding a window_state*/& need no change.
// context::window_for/window_find/forget_window/z_index/bring_to_front in context.cpp stay put -- they mix in
// other subsystems (m_->dock_, m_->modal_, report_limit) that don't belong here -- and operate on
// m_->win_'s fields directly, the same plain-struct shape as dock_state.hpp / input_frame.hpp.

#include "strata/context.hpp"

#include <array>

namespace strata::internal {

struct window_state {
    id   key{};
    vec2 pos{};
    f32  width{};
    f32  height{};      // 0: follows the content
    f32  content_h{};
    f32  scroll{};
    f32  grab{};        // scrollbar: where in the thumb it was pressed
    bool collapsed{};
    bool resizable{};
    bool size_set{};
    bool overflow{};    // content taller than the body last frame (scrolling windows)
    f32  capped_h{};    // auto-height window capped at the display: shown height, 0 = not capped
    // docking
    u32  dock{};        // 1 + index of the dock node the window sits in, 0 = floating
    u32  dock_order{};  // position among the tabs of its node
    vec2 float_size{};  // size to go back to when un-docked
    bool docked_now{};  // drawn as a docked window this frame
    f32  ghost{};       // 0..1: see-through while carried over a dock target
    f32  title_h{};
    u64  last_frame{};
    id   dock_owner{};     // in a floating dock: that dock's window (they stack together)
    bool passive{};        // window_flags::no_inputs
    bool menubar{};        // the main menu bar (no padding, above the other windows)
    u32  modal_level{};    // 1 + index in the modal stack, 0 = not a modal
    u8   title_len{};
    std::array<char, 48> title{}; // visible part of the title, for dock tabs
};

struct window_registry {
    static constexpr u32 max_windows = 32;           // (context::max_windows is the same)
    static constexpr u32 no_z        = 0xffffffffu;  // (context::no_z is the same)

    std::array<window_state, max_windows> windows_{};

    // back-to-front window stacking, persistent across frames
    std::array<id, max_windows> z_order_{};
    u32                         z_count_{};

    // windows submitted this frame, in call order
    std::array<window_state*, max_windows> frame_windows_{};
    u32                                    frame_window_count_{};

    id  hovered_window_prev_{};
    id  hovered_window_cur_{};
    u32 hovered_z_{no_z};
    id  focused_window_{}; // topmost window of the previous frame
};

} // namespace strata::internal

namespace strata { using internal::window_registry; }

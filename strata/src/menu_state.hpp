#pragma once

// internal: the menu bar / dropdown menu chain, behind context::impl::menu_. context::menu_level/menu_frame
// (declared in context.hpp) alias internal::menu_level/menu_frame below, so context.cpp's and context_menu.cpp's
// unqualified "menu_level& m = m_->menu_.menu_open_[...]" need no change. context_menu.cpp owns the logic.

#include "strata/context.hpp"

#include "core/layout_state.hpp"

#include <array>

namespace strata::internal {

struct menu_level {
    id   key{};
    vec2 pos{};       // where the popup wants to open
    rect anchor{};    // the opener: pressing it is not "outside"
    vec2 size{};      // measured last frame
    rect rect_cur{};
    rect rect_prev{};
    f32  age{};
    bool seen{};      // built this frame
    bool from_bar{};
};

struct menu_frame { // a popup being built
    layout_state saved_layout{};
    u32          prev_owner{};
    f32          saved_spacing{};
    f32          content_w{};
    u32          level{};
    bool         saved_overlay{};
    bool         keys_ok{};   // the deepest open popup: letter keys activate mnemonics here
};

struct menu_state {
    static constexpr u32 max_menu_levels = 4; // (context::max_menu_levels is the same)

    std::array<menu_level, max_menu_levels> menu_open_{};
    std::array<menu_frame, max_menu_levels> menu_stack_{};
    u32  menu_depth_{};
    bool menu_close_all_{};
    bool menu_hit_prev_{};     // pointer over an open menu (last frame's rects)
    bool in_menu_bar_{};
    f32  menu_bar_h_{};
    f32  menu_bar_saved_spacing_{};
};

} // namespace strata::internal

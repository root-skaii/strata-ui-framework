#pragma once

// internal: keyboard navigation state (nav_begin/nav_end), behind context::impl::nav_. included via
// context_impl.hpp; context_nav.cpp owns the logic built on it (m_->nav_.*).

#include "strata/context.hpp"

#include <vector>

namespace strata::internal {

struct nav_item {
    id   key{};
    rect bounds{};
    u32  depth{};
    bool node{};
    bool open{};
};

struct nav_state {
    id                    scope{};      // scope being built, 0 outside nav_begin / nav_end
    id                    scope_key{};  // list owning the cursor (resets for another list)
    id                    cursor{};     // the row the cursor is on, kept across frames
    id                    activate_pending{}; // row Enter picked last frame, reported as a press
    bool                  active{};     // the scope has the keyboard (no text field has it)
    std::vector<nav_item> items;        // every row this frame, culled ones included
};

} // namespace strata::internal

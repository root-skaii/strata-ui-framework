#pragma once

// internal: child regions and cards -- persistent scroll/size state (child_state/card_state) plus the frame
// being built (child_frame/card_frame), behind context::impl::children_/child_stack_/cards_/card_stack_.
// context::child_state/child_frame/card_state/card_frame (declared in context.hpp) alias the types below, so
// context_extra.cpp's, context_log.cpp's, context_nav.cpp's, context_state.cpp's, and context_text.cpp's
// unqualified uses need no change. context_extra.cpp owns the logic.

#include "strata/context.hpp"

#include "core/layout_state.hpp"

#include <array>

namespace strata::internal {

struct child_state { // persists across frames
    // horizontal counterparts of scroll / grab / content_h / overflow (child_flags::horizontal)
    f32  scroll_x{};
    f32  grab_x{};
    f32  content_w{};
    bool overflow_x{};
    id   key{};
    u64  last_frame{};
    f32  scroll{};
    f32  grab{};
    f32  content_h{};
    bool overflow{};
};

struct child_frame { // the child being built
    child_state* state{};
    rect         bounds;
    rect         inner;
    layout_state outer{};
    child_flags  flags{};
};

struct card_state {
    id   key{};
    u64  last_frame{};
    f32  content_h{};
};

struct card_frame {
    card_state*  state{};
    layout_state outer{};
};

struct child_card_state {
    std::array<child_state, 32> children_{};    // (context::max_children is the same)
    std::array<child_frame, 4>  child_stack_{};  // (context::max_child_depth is the same)
    u32 child_depth_{};
    std::array<card_state, 48>  cards_{};        // (context::max_cards is the same)
    std::array<card_frame, 4>   card_stack_{};   // (context::max_card_depth is the same)
    u32 card_depth_{};
};

} // namespace strata::internal

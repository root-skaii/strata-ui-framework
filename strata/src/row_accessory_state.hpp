#pragma once

// internal: row accessories (row_accessory_button/checkbox/toggle) -- the row they attach to (last selectable /
// tree row / custom_item, not the last item, which may be an accessory itself) -- behind
// context::impl::accessory_. context.cpp owns the logic; context_data.cpp reads row_anchor_ for tree rows.

#include "strata/context.hpp"

namespace strata::internal {

struct row_accessory_state {
    f32  next_gutter_{};
    id   row_anchor_{};
    rect row_anchor_rect_{};
    id   accessory_row_{};   // the row the accessories laid out so far belong to
    f32  accessory_x_{};     // left edge, moved left by each accessory
    rect accessory_row_rect_{};
};

} // namespace strata::internal

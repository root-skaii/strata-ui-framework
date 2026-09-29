#pragma once

// internal: the color picker being edited, behind context::impl::pick_. context_data.cpp owns the logic --
// the only file that touches it.

#include "strata/context.hpp"

namespace strata::internal {

struct color_picker_state {
    id    pick_key_{};
    f32   pick_h_{}, pick_s_{}, pick_v_{};
    color pick_last_{};
};

} // namespace strata::internal

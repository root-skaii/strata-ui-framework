#pragma once

// internal: combo_filtered's search box (search text, filtered rows, keyboard row), behind
// context::impl::combo_filter_. context_combo.cpp owns the logic built on this -- the only file that touches it.

#include "strata/context.hpp"

#include <string>
#include <vector>

namespace strata::internal {

struct combo_filter_state {
    std::string      combo_filter_;
    int              combo_filter_hover_{};
    bool             combo_filter_focus_{};
    std::vector<u32> combo_filter_hits_;
};

} // namespace strata::internal

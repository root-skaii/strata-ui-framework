#pragma once

// internal: label_size() cache -- direct-mapped (font, string) -> size, cleared when the atlas changes (rich
// labels skip it) -- behind context::impl::measure_. context::measure_slot (declared in context.hpp) aliases
// the type below; context.cpp owns the logic, the only file that touches this.

#include "strata/context.hpp"

#include <vector>

namespace strata::internal {

struct measure_slot {
    u64  key{};   // 0 = empty
    vec2 size{};
};

struct measure_cache_state {
    std::vector<measure_slot> measure_cache_;
    u32                       measure_cache_gen_{};
};

} // namespace strata::internal

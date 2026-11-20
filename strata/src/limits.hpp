#pragma once

// internal: fixed-size tables (windows, dock panes, nesting ...) drop what does not fit. context::report_limit
// reports it once per limit via the diagnostics hook; without one it is marked on screen (see diag_marks).

#include "strata/types.hpp"

#include <array>
#include <string_view>

namespace strata {
class context;
}

namespace strata::internal {

// STRATA_DIAGNOSTICS=OFF defines STRATA_NO_DIAGNOSTICS: nothing is reported (no hook call, no on-screen marks) and no
// message text is compiled in. Counters (limits_hit, id_collisions) and the repair of unbalanced begin/end are unchanged.
#ifdef STRATA_NO_DIAGNOSTICS
#define STRATA_DIAG(text) ""
#else
#define STRATA_DIAG(text) text

// what went wrong this frame, drawn by end_frame when no diagnostics hook is set: a red frame around the offending item
// (if there is one; shown with its message while the pointer is over it) and a banner for the rest. cleared once drawn.
struct diag_marks {
    static constexpr std::size_t max_entries = 8;
    struct entry {
        rect                  box{};  // empty: not tied to one item
        std::array<char, 160> text{};
        u8                    len{};
    };
    std::array<entry, max_entries> items{};
    u32                            count{};

    void add(std::string_view message, const rect& box) noexcept;
};

void diag_marks_draw(context& ui);
#endif

} // namespace strata::internal

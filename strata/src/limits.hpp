#pragma once

// internal: fixed-size tables (windows, dock panes, nesting ...) drop what does not fit. context::report_limit
// reports it once per limit via the diagnostics hook, or through this without one.

#include <string_view>

namespace strata::internal {

// STRATA_DIAGNOSTICS=OFF defines STRATA_NO_DIAGNOSTICS: nothing is reported (no hook call, no stderr / debugger output)
// and no message text is compiled in. Counters (limits_hit, id_collisions) and the repair of unbalanced begin/end are unchanged.
#ifdef STRATA_NO_DIAGNOSTICS
#define STRATA_DIAG(text) ""
#else
#define STRATA_DIAG(text) text

// "[strata] <line>" to stderr and the debugger
void print_diagnostic(std::string_view line) noexcept;
#endif

} // namespace strata::internal

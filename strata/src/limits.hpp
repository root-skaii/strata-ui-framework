#pragma once

// internal: fixed-size tables (windows, dock panes, nesting ...) drop what does not fit. context::report_limit
// reports it once per limit via the diagnostics hook, or through this without one.

#include <string_view>

namespace strata::internal {

// STRATA_DIAGNOSTIC_TEXT=OFF defines STRATA_NO_DIAGNOSTIC_TEXT: the sentences below are not compiled in. The hook still
// gets the kind, and a "L<source line>" code in place of the text; counters (limits_hit, id_collisions) are unchanged.
#define STRATA_DIAG_STR2(x) #x
#define STRATA_DIAG_STR(x) STRATA_DIAG_STR2(x)
#ifdef STRATA_NO_DIAGNOSTIC_TEXT
#define STRATA_DIAG(text) ("L" STRATA_DIAG_STR(__LINE__))
#define STRATA_DIAG_FMT(full, brief) brief
#else
#define STRATA_DIAG(text) text
#define STRATA_DIAG_FMT(full, brief) full
#endif

// "[strata] <line>" to stderr and the debugger
void print_diagnostic(std::string_view line) noexcept;

} // namespace strata::internal

#pragma once

// internal: fixed-size tables (windows, dock panes, nesting ...) drop what does not fit. context::report_limit
// reports it once per limit via the diagnostics hook, or through this without one.

#include <string_view>

namespace strata::internal {

// "[strata] <line>" to stderr and the debugger
void print_diagnostic(std::string_view line) noexcept;

} // namespace strata::internal

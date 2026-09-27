#pragma once

// internal (not installed): the ui keeps its state in fixed-size tables (windows, dock panes, nesting depths ...). when one
// runs out, what does not fit is dropped, which looks like a widget that is simply missing. context::report_limit says so,
// once per limit, through the application's diagnostics hook -- or, without one, through this.

#include <string_view>

namespace strata::internal {

// "[strata] <line>" to stderr and the debugger output
void print_diagnostic(std::string_view line) noexcept;

} // namespace strata::internal

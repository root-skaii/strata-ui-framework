#pragma once

// internal (not installed): the ui keeps its state in fixed-size tables (windows, dock panes, nesting depths ...). when one
// runs out, what does not fit is dropped, which looks like a widget that is simply missing. this says so, once per limit.

namespace strata::internal {

// prints "[strata] limit reached: <what> (capacity N)" to stderr and the debugger output the first time `what` is reported.
// `what` is a short description of the limit, e.g. "windows"
void limit_reached(const char* what, unsigned capacity) noexcept;

} // namespace strata::internal

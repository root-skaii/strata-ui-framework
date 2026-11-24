#pragma once

#include "showcase/module.hpp"

#include <memory>
#include <vector>

namespace strata::showcase {

// Returns one fresh instance of every built-in showcase module in display order.
[[nodiscard]] std::vector<std::unique_ptr<demo_module>> create_builtin_modules();

} // namespace strata::showcase

#include "limits.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace strata::internal {

void print_diagnostic(std::string_view line) noexcept
{
    char buf[320];
    const int n = std::snprintf(buf, sizeof buf, "[strata] %.*s\n", static_cast<int>(std::min<std::size_t>(line.size(), 300)), line.data());
    if (n <= 0) { return; }
    std::fputs(buf, stderr);
    OutputDebugStringA(buf);
}

} // namespace strata::internal

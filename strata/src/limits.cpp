#include "limits.hpp"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>

namespace strata::internal {

void limit_reached(const char* what, unsigned capacity) noexcept
{
    static std::array<const char*, 32> seen{}; // (the ui runs on one thread; `what` is always a literal)
    for (const char*& s : seen) {
        if (s == nullptr) {
            s = what;
            break;
        }
        if (std::strcmp(s, what) == 0) {
            return;
        }
    }
    char line[192];
    std::snprintf(line, sizeof line, "[strata] limit reached: %s (capacity %u) - what does not fit is dropped\n", what, capacity);
    std::fputs(line, stderr);
    OutputDebugStringA(line);
}

} // namespace strata::internal

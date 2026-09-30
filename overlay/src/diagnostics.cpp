#include "internal.hpp"

namespace strata::overlay::detail {

// never destroyed: game threads may still be inside a hook at process exit, and tearing d3d down then crashes.
// uninstall() frees what matters when the dll is meant to unload.
state& g = *new state;

// STRATA_OVERLAY_LOG=<file>: what the hook is doing, for finding out why it does not attach
void log_line(const char* fmt, ...)
{
    char path[512];
    if (::GetEnvironmentVariableA("STRATA_OVERLAY_LOG", path, sizeof(path)) == 0) { return; }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || f == nullptr) { return; }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

void set_error(const char* what) noexcept
{
    std::snprintf(g.error, sizeof(g.error), "%s", what);
    log_line("error: %s", what);
}

} // namespace strata::overlay::detail

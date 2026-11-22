#include "strata/themes.hpp"

#include <array>

namespace strata::themes {

namespace {

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
        if (x != y) { return false; }
    }
    return true;
}

} // namespace

std::span<const std::string_view> names() noexcept
{
    static constexpr std::array<std::string_view, 12> registry_names = {
        "midnight", "light", "ocean", "rose", "dracula", "nord", "solarized_dark", "solarized_light",
        "high_contrast", "forest", "amber", "glass"};
    return registry_names;
}

bool by_name(std::string_view name, style& out) noexcept
{
    struct named_theme {
        std::string_view name;
        style (*make)() noexcept;
    };
    static constexpr std::array<named_theme, 12> registry = {{
        {"midnight", &midnight},
        {"light", &light},
        {"ocean", &ocean},
        {"rose", &rose},
        {"dracula", &dracula},
        {"nord", &nord},
        {"solarized_dark", &solarized_dark},
        {"solarized_light", &solarized_light},
        {"high_contrast", &high_contrast},
        {"forest", &forest},
        {"amber", &amber},
        {"glass", &glass},
    }};
    for (const named_theme& t : registry) {
        if (iequals(t.name, name)) {
            out = t.make();
            return true;
        }
    }
    return false;
}

} // namespace strata::themes

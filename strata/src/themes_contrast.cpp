#include "strata/themes.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace strata::themes {

namespace {

// relative luminance of an srgb colour (WCAG 2)
[[nodiscard]] f32 luminance(color c) noexcept
{
    const auto lin = [](u8 v) {
        const f32 x = static_cast<f32>(v) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

} // namespace

f32 contrast_ratio(color a, color b) noexcept
{
    const f32 la = luminance(a);
    const f32 lb = luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

std::vector<contrast_issue> check_contrast(const style& s)
{
    struct pair {
        std::string_view name;
        color            fg;
        color            bg;
        f32              required;
    };
    const color base{s.window_bg.r, s.window_bg.g, s.window_bg.b, 255};
    const std::array<pair, 10> pairs = {{
        {"text on window", s.text, base, 4.5f},
        {"text on widget", s.text, s.widget_bg, 4.5f},
        {"text on title bar", s.text, s.title_bg, 4.5f},
        {"dim text on window", s.text_dim, base, 4.5f},
        {"dim text on widget", s.text_dim, s.widget_bg, 4.5f},
        {"accent on window", s.accent, base, 3.0f},
        {"success on window", s.success, base, 3.0f},
        {"warning on window", s.warning, base, 3.0f},
        {"error on window", s.error, base, 3.0f},
        {"border on window", s.widget_border, base, 1.3f},
    }};
    std::vector<contrast_issue> out;
    for (const pair& p : pairs) {
        const f32 ratio = contrast_ratio(p.fg, p.bg);
        if (ratio < p.required) { out.push_back({p.name, ratio, p.required}); }
    }
    return out;
}

} // namespace strata::themes

#pragma once

#include "strata/context.hpp"

#include <span>
#include <string>
#include <string_view>

// ready-made themes and a plain-text theme file. assign with `ui.theme() = strata::themes::light();`
// (apply between frames) or start from one and tweak individual members.
namespace strata::themes {

[[nodiscard]] constexpr style midnight() noexcept
{
    return {}; // the defaults
}

[[nodiscard]] constexpr style light() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0xf6f7fbf6);
    s.title_bg      = color::from_hex(0xe9ecf4ff);
    s.border        = color::from_hex(0xcdd3e2ff);
    s.widget_bg     = color::from_hex(0xe4e8f2ff);
    s.widget_hover  = color::from_hex(0xd8deecff);
    s.widget_active = color::from_hex(0xc8d1e6ff);
    s.widget_border = color::from_hex(0xc3cadcff);
    s.accent        = color::from_hex(0x3b73f0ff);
    s.accent_hover  = color::from_hex(0x5a8bf5ff);
    s.text          = color::from_hex(0x1d2233ff);
    s.text_dim      = color::from_hex(0x6a7390ff);
    s.shadow        = color::from_hex(0x1a25404a);
    s.modal_dim     = color::from_hex(0x1a254070);
    s.gradient      = 0.06f;
    return s;
}

[[nodiscard]] constexpr style ocean() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x0b1a24f2);
    s.title_bg      = color::from_hex(0x10283aff);
    s.border        = color::from_hex(0x1f4258ff);
    s.widget_bg     = color::from_hex(0x15303fff);
    s.widget_hover  = color::from_hex(0x1d4256ff);
    s.widget_active = color::from_hex(0x27556eff);
    s.widget_border = color::from_hex(0x235068ff);
    s.accent        = color::from_hex(0x19c2b4ff);
    s.accent_hover  = color::from_hex(0x3fdccfff);
    s.text          = color::from_hex(0xe3f4f6ff);
    s.text_dim      = color::from_hex(0x7fa5b0ff);
    s.rounding      = 10.0f;
    return s;
}

[[nodiscard]] constexpr style rose() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x1a1219f2);
    s.title_bg      = color::from_hex(0x261a25ff);
    s.border        = color::from_hex(0x4a2c46ff);
    s.widget_bg     = color::from_hex(0x2f2030ff);
    s.widget_hover  = color::from_hex(0x3d2a3eff);
    s.widget_active = color::from_hex(0x52374fff);
    s.widget_border = color::from_hex(0x573a53ff);
    s.accent        = color::from_hex(0xf0568fff);
    s.accent_hover  = color::from_hex(0xff7aa9ff);
    s.text          = color::from_hex(0xf6e8f0ff);
    s.text_dim      = color::from_hex(0xb08aa4ff);
    s.rounding      = 14.0f;
    s.shadow_blur   = 22.0f;
    return s;
}

[[nodiscard]] constexpr style dracula() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x282a36f2);
    s.title_bg      = color::from_hex(0x21222cff);
    s.border        = color::from_hex(0x44475aff);
    s.widget_bg     = color::from_hex(0x343746ff);
    s.widget_hover  = color::from_hex(0x3e4154ff);
    s.widget_active = color::from_hex(0x4d5170ff);
    s.widget_border = color::from_hex(0x44475aff);
    s.accent        = color::from_hex(0xbd93f9ff);
    s.accent_hover  = color::from_hex(0xd0b0ffff);
    s.text          = color::from_hex(0xf8f8f2ff);
    s.text_dim      = color::from_hex(0x8b96c4ff);
    return s;
}

[[nodiscard]] constexpr style nord() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x2e3440f2);
    s.title_bg      = color::from_hex(0x272c36ff);
    s.border        = color::from_hex(0x434c5eff);
    s.widget_bg     = color::from_hex(0x3b4252ff);
    s.widget_hover  = color::from_hex(0x434c5eff);
    s.widget_active = color::from_hex(0x4c566aff);
    s.widget_border = color::from_hex(0x4c566aff);
    s.accent        = color::from_hex(0x88c0d0ff);
    s.accent_hover  = color::from_hex(0xa3d4e2ff);
    s.text          = color::from_hex(0xeceff4ff);
    s.text_dim      = color::from_hex(0x9aa5bbff);
    s.rounding      = 6.0f;
    return s;
}

[[nodiscard]] constexpr style solarized_dark() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x002b36f2);
    s.title_bg      = color::from_hex(0x073642ff);
    s.border        = color::from_hex(0x1c4b58ff);
    s.widget_bg     = color::from_hex(0x0a3d4bff);
    s.widget_hover  = color::from_hex(0x104a5aff);
    s.widget_active = color::from_hex(0x175a6cff);
    s.widget_border = color::from_hex(0x1f5566ff);
    s.accent        = color::from_hex(0x268bd2ff);
    s.accent_hover  = color::from_hex(0x4aa3e6ff);
    s.text          = color::from_hex(0xeee8d5ff);
    s.text_dim      = color::from_hex(0x839496ff);
    return s;
}

[[nodiscard]] constexpr style solarized_light() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0xfdf6e3f6);
    s.title_bg      = color::from_hex(0xeee8d5ff);
    s.border        = color::from_hex(0xd9d2bcff);
    s.widget_bg     = color::from_hex(0xf0e9d2ff);
    s.widget_hover  = color::from_hex(0xe7dfc6ff);
    s.widget_active = color::from_hex(0xdcd3b6ff);
    s.widget_border = color::from_hex(0xd3cbb2ff);
    s.accent        = color::from_hex(0x268bd2ff);
    s.accent_hover  = color::from_hex(0x4aa3e6ff);
    s.text          = color::from_hex(0x586e75ff);
    s.text_dim      = color::from_hex(0x93a1a1ff);
    s.shadow        = color::from_hex(0x50402040);
    s.modal_dim     = color::from_hex(0x50402060);
    s.gradient      = 0.05f;
    return s;
}

[[nodiscard]] constexpr style high_contrast() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x000000ff);
    s.title_bg      = color::from_hex(0x000000ff);
    s.border        = color::from_hex(0xffffffff);
    s.widget_bg     = color::from_hex(0x101010ff);
    s.widget_hover  = color::from_hex(0x303030ff);
    s.widget_active = color::from_hex(0x505050ff);
    s.widget_border = color::from_hex(0xffffffff);
    s.accent        = color::from_hex(0xffdd00ff);
    s.accent_hover  = color::from_hex(0xffee66ff);
    s.text          = color::from_hex(0xffffffff);
    s.text_dim      = color::from_hex(0xc8c8c8ff);
    s.shadow        = color::from_hex(0x00000000);
    s.border_width  = 2.0f;
    s.gradient      = 0.0f;
    s.shadow_blur   = 0.0f;
    return s;
}

[[nodiscard]] constexpr style forest() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x101a14f2);
    s.title_bg      = color::from_hex(0x16261cff);
    s.border        = color::from_hex(0x2b4a36ff);
    s.widget_bg     = color::from_hex(0x1c3324ff);
    s.widget_hover  = color::from_hex(0x25422fff);
    s.widget_active = color::from_hex(0x30553dff);
    s.widget_border = color::from_hex(0x2f5240ff);
    s.accent        = color::from_hex(0x7bc74dff);
    s.accent_hover  = color::from_hex(0x9be070ff);
    s.text          = color::from_hex(0xe6f2e4ff);
    s.text_dim      = color::from_hex(0x8fae94ff);
    s.rounding      = 9.0f;
    return s;
}

// a phosphor terminal: amber on brown-black, square-ish and flat
[[nodiscard]] constexpr style amber() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x140d02f2);
    s.title_bg      = color::from_hex(0x1e1404ff);
    s.border        = color::from_hex(0x4d3408ff);
    s.widget_bg     = color::from_hex(0x2a1c06ff);
    s.widget_hover  = color::from_hex(0x38260aff);
    s.widget_active = color::from_hex(0x4a3210ff);
    s.widget_border = color::from_hex(0x5a3d0cff);
    s.accent        = color::from_hex(0xffb000ff);
    s.accent_hover  = color::from_hex(0xffc940ff);
    s.text          = color::from_hex(0xffd27fff);
    s.text_dim      = color::from_hex(0xa87b2aff);
    s.rounding      = 3.0f;
    s.gradient      = 0.0f;
    return s;
}

// translucent panels meant for `window_flags::acrylic` (blur behind them); flat tints where blur is not available
[[nodiscard]] constexpr style glass() noexcept
{
    style s;
    s.window_bg     = color::from_hex(0x1b2233d8);
    s.title_bg      = color::from_hex(0x232c4290);
    s.border        = color::from_hex(0xffffff30);
    s.widget_bg     = color::from_hex(0xffffff1c);
    s.widget_hover  = color::from_hex(0xffffff2c);
    s.widget_active = color::from_hex(0xffffff3c);
    s.widget_border = color::from_hex(0xffffff28);
    s.accent        = color::from_hex(0x6ea8ffff);
    s.accent_hover  = color::from_hex(0x93bfffff);
    s.text          = color::from_hex(0xf4f7ffff);
    s.text_dim      = color::from_hex(0xb4bfd6ff);
    s.shadow        = color::from_hex(0x00000070);
    s.rounding      = 12.0f;
    s.gradient      = 0.04f;
    s.blur_radius   = 22.0f;
    s.acrylic_alpha = 0.75f;
    s.acrylic_saturation = 1.35f;
    s.acrylic_brightness = 1.05f;
    s.popup_acrylic = 1.0f;
    return s;
}

// built-in themes by name: midnight, light, ocean, rose, dracula, nord, solarized_dark, solarized_light,
// high_contrast, forest, amber, glass
[[nodiscard]] std::span<const std::string_view> names() noexcept;
// case-insensitive; false (and `out` untouched) for an unknown name
[[nodiscard]] bool by_name(std::string_view name, style& out) noexcept;

// theme files: one `key = value` per line; blank lines and lines starting with `#`, `;` or `//` are comments.
//     # my theme
//     base          = nord                (optional: start from a built-in theme, must come first)
//     rounding      = 10
//     accent        = #ff8800             (#rgb, #rgba, #rrggbb or #rrggbbaa)
//     window_bg     = #14161df4
// keys are the style members (frame_padding is split into frame_padding_x / _y).
struct theme_result {
    std::size_t applied{};  // keys that were read
    std::size_t unknown{};  // keys that are not style members
    std::size_t invalid{};  // values that could not be read
    std::size_t first_problem_line{}; // 1-based line of the first unknown / invalid entry, 0 if none
    [[nodiscard]] bool ok() const noexcept { return unknown == 0 && invalid == 0; }
};

// writes every member; `name` becomes a comment on the first line
[[nodiscard]] std::string to_string(const style& s, std::string_view name = {});
// changes only what the text sets, so a file can be a partial theme on top of `s`
theme_result from_string(std::string_view text, style& s);
// utf-8 path. save returns false if the file cannot be written, load if it cannot be read
[[nodiscard]] bool save_file(std::string_view path, const style& s, std::string_view name = {});
[[nodiscard]] bool load_file(std::string_view path, style& s, theme_result* result = nullptr);

} // namespace strata::themes

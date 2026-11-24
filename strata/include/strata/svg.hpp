#pragma once

// Lightweight SVG path (`d="..."`) parser, anti-aliased rasterizer, draw_list helper, and built-in cross-platform
// vector UI icons (24x24 viewBox). Works identically on Windows, Linux, macOS, and Web without OS icon fonts.

#include "strata/draw_list.hpp"
#include "strata/types.hpp"

#include <string_view>
#include <vector>

namespace strata {

struct svg_contour {
    std::vector<vec2> points;
    bool              closed{};
};

// Parses an SVG path data string (M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, A/a, Z/z) into flattened polylines.
[[nodiscard]] std::vector<svg_contour> svg_parse_path(std::string_view d, f32 flatness = 0.25f);

// Rasterises an SVG path into an 8-bit anti-aliased alpha mask (width * height bytes, top row first).
[[nodiscard]] std::vector<u8> svg_rasterize_alpha(std::string_view d, vec2 viewbox, u32 width, u32 height,
                                                  bool even_odd = false);

// Rasterises an SVG path into straight-alpha RGBA8 pixels (width * height * 4 bytes), ready for renderer::create_texture().
[[nodiscard]] std::vector<u8> svg_rasterize_rgba(std::string_view d, vec2 viewbox, u32 width, u32 height,
                                                 color tint = {255, 255, 255, 255}, bool even_odd = false);

// Draws an SVG path directly onto `dl` inside `dst`. stroke_width > 0 strokes the contours; 0 fills convex/fan contours.
void draw_svg_path(draw_list& dl, const rect& dst, std::string_view d, vec2 viewbox, color col, f32 stroke_width = 0.0f);

// Built-in 24x24 vector UI icons (cross-platform alternative to Segoe MDL2 / Fluent icon fonts).
namespace svg_icons {
inline constexpr vec2 viewbox{24.0f, 24.0f};
inline constexpr std::string_view check         = "M4 12.5 L9.5 18 L20 6.5";
inline constexpr std::string_view close         = "M6 6 L18 18 M18 6 L6 18";
inline constexpr std::string_view chevron_right = "M9 6 L15 12 L9 18";
inline constexpr std::string_view chevron_down  = "M6 9 L12 15 L18 9";
inline constexpr std::string_view chevron_left  = "M15 6 L9 12 L15 18";
inline constexpr std::string_view chevron_up    = "M6 15 L12 9 L18 15";
inline constexpr std::string_view plus          = "M12 5 L12 19 M5 12 L19 12";
inline constexpr std::string_view minus         = "M5 12 L19 12";
inline constexpr std::string_view search        = "M10.5 4 A6.5 6.5 0 1 0 10.5 17 A6.5 6.5 0 1 0 10.5 4 Z M15.5 15.5 L20 20";
inline constexpr std::string_view folder        = "M3 6 L9 6 L11 8 L21 8 L21 19 L3 19 Z";
inline constexpr std::string_view file          = "M6 3 L14 3 L19 8 L19 21 L6 21 Z M14 3 L14 8 L19 8";
inline constexpr std::string_view play          = "M7 5 L19 12 L7 19 Z";
inline constexpr std::string_view pause         = "M6 5 L10 5 L10 19 L6 19 Z M14 5 L18 5 L18 19 L14 19 Z";
inline constexpr std::string_view stop          = "M6 6 L18 6 L18 18 L6 18 Z";
inline constexpr std::string_view refresh       = "M19 12 A7 7 0 1 1 16.5 6.5 M19 4 L19 9 L14 9";
inline constexpr std::string_view warning       = "M12 3 L22 20 L2 20 Z M12 10 L12 14 M12 17 L12 17.5";
inline constexpr std::string_view info          = "M12 3 A9 9 0 1 0 12 21 A9 9 0 1 0 12 3 Z M12 10 L12 16 M12 7 L12 7.5";
inline constexpr std::string_view settings      = "M12 8 A4 4 0 1 0 12 16 A4 4 0 1 0 12 8 Z M12 2 L12 5 M12 19 L12 22 M2 12 L5 12 M19 12 L22 12";
inline constexpr std::string_view trash         = "M4 6 L20 6 M9 6 L9 4 L15 4 L15 6 M6 6 L7 20 L17 20 L18 6";
inline constexpr std::string_view copy          = "M8 8 L19 8 L19 20 L8 20 Z M5 16 L5 4 L15 4";
inline constexpr std::string_view eye           = "M2 12 Q12 4 22 12 Q12 20 2 12 Z M12 9 A3 3 0 1 0 12 15 A3 3 0 1 0 12 9 Z";
inline constexpr std::string_view lock          = "M6 11 L18 11 L18 20 L6 20 Z M8 11 L8 8 A4 4 0 0 1 16 8 L16 11";
} // namespace svg_icons

} // namespace strata

#pragma once

// internal: small colour / easing helpers the widget files share

#include "strata/types.hpp"

namespace strata::internal {

[[nodiscard]] constexpr color lighten(color c, f32 k) noexcept { return lerp(c, color{255, 255, 255, c.a}, k); }
[[nodiscard]] constexpr color darken(color c, f32 k) noexcept  { return lerp(c, color{0, 0, 0, c.a}, k); }
// smoothstep over 0..1
[[nodiscard]] constexpr f32 smooth(f32 t) noexcept { return t * t * (3.0f - 2.0f * t); }

} // namespace strata::internal

namespace strata { using internal::darken; using internal::lighten; using internal::smooth; }

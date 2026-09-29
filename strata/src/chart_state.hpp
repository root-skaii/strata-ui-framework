#pragma once

// internal: zoomable chart views (survive between frames) and plot scratch space, behind context::impl::chart_.
// context::chart_view (declared in context.hpp) aliases the type below, so context_plot.cpp's unqualified uses
// need no change -- it's the only file that touches this.

#include "strata/context.hpp"

#include <array>
#include <vector>

namespace strata::internal {

struct chart_view {
    id   key{};
    bool x_set{};
    bool y_set{};
    f32  x_lo{}, x_hi{}, y_lo{}, y_hi{}; // the user's view
    f32  sx_lo{}, sx_hi{}, sy_lo{}, sy_hi{}; // what was drawn last frame
    rect inner;                          // ... and where
    f64  last_click{-10.0};
    u64  last_frame{};
};

struct chart_state {
    std::vector<vec2>         plot_scratch_;
    std::array<chart_view, 16> chart_views_{};
};

} // namespace strata::internal

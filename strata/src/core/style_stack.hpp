#pragma once

// internal: the color/var override stack (push_color/push_var) and the font stack (push_font).
// included via context_impl.hpp. context::color_ref/var_ref resolve a style_color/style_var to the live
// strata::style field and stay on context (they touch m_->style_, not this stack) -- context::push_color/
// pop_color/push_var/pop_var/push_font/pop_font in context.cpp are thin forwards around that resolution.

#include "strata/context.hpp"

#include <array>

namespace strata::internal {

class style_stack {
public:
    static constexpr u32 max_overrides  = 32; // (context::max_overrides is the same)
    static constexpr u32 max_font_depth = 8;  // (context::max_font_depth is the same)

    struct saved_color { style_color which{}; color previous{}; };
    struct saved_var   { style_var   which{}; f32   previous{}; };

    // records `previous` as what `which` restores to; false (nothing pushed) past max_overrides
    [[nodiscard]] bool push_color(style_color which, color previous) noexcept;
    [[nodiscard]] bool push_var(style_var which, f32 previous) noexcept;
    // pops one entry into `out`; false if the stack was already empty
    [[nodiscard]] bool pop_color(saved_color& out) noexcept;
    [[nodiscard]] bool pop_var(saved_var& out) noexcept;

    [[nodiscard]] u32 color_depth() const noexcept { return color_depth_; }
    [[nodiscard]] u32 var_depth() const noexcept { return var_depth_; }

    // font stack: plain font_id values, no external resolver needed
    [[nodiscard]] bool push_font(font_id f) noexcept;
    void pop_font() noexcept { if (font_depth_ > 0) { --font_depth_; } }
    [[nodiscard]] font_id current_font() const noexcept { return font_stack_[font_depth_]; }
    [[nodiscard]] u32 font_depth() const noexcept { return font_depth_; }

    // font selection does not carry across frames (color/var overrides are unwound by pop, not reset here)
    void reset_font() noexcept { font_depth_ = 0; }

private:
    std::array<saved_color, max_overrides> color_stack_{};
    std::array<saved_var, max_overrides>   var_stack_{};
    u32 color_depth_{};
    u32 var_depth_{};

    std::array<font_id, max_font_depth + 1> font_stack_{};
    u32 font_depth_{};
};

} // namespace strata::internal

namespace strata { using internal::style_stack; }

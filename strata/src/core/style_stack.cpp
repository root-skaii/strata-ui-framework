#include "style_stack.hpp"

namespace strata::internal {

bool style_stack::push_color(style_color which, color previous) noexcept
{
    if (color_depth_ >= max_overrides) { return false; }
    color_stack_[color_depth_++] = {which, previous};
    return true;
}

bool style_stack::push_var(style_var which, f32 previous) noexcept
{
    if (var_depth_ >= max_overrides) { return false; }
    var_stack_[var_depth_++] = {which, previous};
    return true;
}

bool style_stack::pop_color(saved_color& out) noexcept
{
    if (color_depth_ == 0) { return false; }
    out = color_stack_[--color_depth_];
    return true;
}

bool style_stack::pop_var(saved_var& out) noexcept
{
    if (var_depth_ == 0) { return false; }
    out = var_stack_[--var_depth_];
    return true;
}

bool style_stack::push_font(font_id f) noexcept
{
    if (font_depth_ >= max_font_depth) { return false; }
    font_stack_[++font_depth_] = f;
    return true;
}

} // namespace strata::internal

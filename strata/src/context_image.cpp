// image widgets: plain images and image buttons

#include "strata/context.hpp"

#include <algorithm>

namespace strata {

void context::image(texture_id tex, vec2 size, vec2 uv0, vec2 uv1, color tint, f32 rounding)
{
    if (cur_ == nullptr || tex == 0) {
        return;
    }
    if (size.x <= 0.0f) {
        size.x = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    }
    layout_.next_width = 0.0f;
    if (size.y <= 0.0f) {
        size.y = size.x;
    }
    const rect r = layout_place(size);
    dl_.image(r, tex, uv0, uv1, tint, radii(rounding));
}

bool context::image_button(std::string_view label, texture_id tex, vec2 size, vec2 uv0, vec2 uv1, color tint)
{
    if (cur_ == nullptr || tex == 0) {
        return false;
    }
    if (size.x <= 0.0f) {
        size.x = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    }
    layout_.next_width = 0.0f;
    if (size.y <= 0.0f) {
        size.y = size.x;
    }

    const id   key = hash_id(label, hash_id({reinterpret_cast<const char*>(&tex), sizeof(tex)}, current_seed()));
    const rect r   = layout_place(size);
    const interaction in = interact(key, r);
    const press_anim  a  = button_anim(key, in);

    const f32 round = style_.rounding * 0.8f;
    const color shown = lerp(tint, color{static_cast<u8>(tint.r * 0.75f), static_cast<u8>(tint.g * 0.75f), static_cast<u8>(tint.b * 0.75f), tint.a}, a.active);
    dl_.image(r, tex, uv0, uv1, shown, radii(round));

    shape_style edge;
    edge.radius       = radii(round);
    edge.border       = lerp(style_.widget_border, style_.accent_hover, std::max(a.hover * 0.85f, a.active));
    edge.border_width = 1.0f + a.hover * 0.75f;
    edge.shadow       = style_.accent.scaled_alpha(0.28f * a.hover);
    edge.shadow_blur  = 8.0f * a.hover;
    dl_.shape(r, edge);
    return in.pressed;
}

} // namespace strata

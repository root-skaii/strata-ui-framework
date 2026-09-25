// small status widgets: spinner, badge, chip

#include "strata/context.hpp"

#include <algorithm>
#include <cmath>

namespace strata {

void context::spinner(f32 diameter, color c)
{
    if (cur_ == nullptr) {
        return;
    }
    const f32  d   = diameter > 0.0f ? diameter : frame_height() - 8.0f;
    const rect r   = layout_place({d, d});
    const color col = c.a == 0 ? style_.accent : c;

    const f32  thick  = std::max(2.0f, d * 0.12f);
    const f32  radius = d * 0.5f - thick * 0.5f;
    const f32  t      = static_cast<f32>(time_);
    const f32  start  = t * 5.0f;
    const f32  sweep  = 1.3f + 0.9f * std::sin(t * 2.4f); // the arc grows and shrinks while it turns
    dl_.circle(r.center(), radius, style_.widget_border, thick);
    dl_.arc(r.center(), radius, start, start + sweep, col, thick);
}

void context::badge(std::string_view text, toast_kind kind)
{
    badge(text, kind_color(kind, style_));
}

void context::badge(std::string_view text, color tint)
{
    if (cur_ == nullptr) {
        return;
    }
    const font_id          f     = current_font();
    const std::string_view shown = visible_label(text);
    const vec2             ts    = label_size(f, shown);
    const f32              h     = ts.y + 4.0f;
    const rect             r     = layout_place({std::max(ts.x + h, h), h});

    shape_style pill;
    pill.radius       = radii(h * 0.5f);
    pill.fill_top     = tint.scaled_alpha(0.20f);
    pill.fill_bottom  = pill.fill_top;
    pill.border       = tint.scaled_alpha(0.55f);
    pill.border_width = 1.0f;
    dl_.shape(r, pill);
    label_draw({r.min.x + (r.width() - ts.x) * 0.5f, r.min.y + (r.height() - ts.y) * 0.5f}, lerp(tint, style_.text, 0.35f), shown, f);
}

chip_result context::chip(std::string_view label, const chip_options& o)
{
    chip_result res;
    if (cur_ == nullptr) {
        return res;
    }
    const font_id          f     = current_font();
    const id               key   = hash_id(label, current_seed());
    const std::string_view shown = visible_label(label);
    const vec2             ts    = label_size(f, shown);
    const f32              h     = frame_height() - 6.0f;
    const f32              side  = h * 0.45f;
    const f32              iw    = o.icon.empty() ? 0.0f : font_.measure(o.icon_font, o.icon).x + 6.0f;
    const f32              cs    = o.closable ? h - 10.0f : 0.0f; // the close button
    const f32              w     = side + iw + ts.x + (o.closable ? 6.0f + cs : 0.0f) + side;
    const rect             r     = layout_place({w, h});

    const rect close_r = o.closable ? rect::from_size({r.max.x - side - cs, r.min.y + (h - cs) * 0.5f}, {cs, cs}) : rect{};
    const rect body_r  = o.closable ? rect{r.min, {close_r.min.x - 2.0f, r.max.y}} : r;

    interaction close_in;
    if (o.closable) {
        close_in = interact(hash_id("##close", key), close_r.expanded(2.0f));
        res.closed = close_in.pressed;
    }
    const interaction in = interact(key, body_r);
    if (in.pressed) {
        res.clicked = true;
        if (o.selected != nullptr) { *o.selected = !*o.selected; }
    }

    const bool  on   = o.selected != nullptr && *o.selected;
    const color tint = o.tint.a == 0 ? style_.accent : o.tint;
    anim_slot&  a    = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, on ? 1.0f : 0.0f);

    shape_style pill;
    pill.radius       = radii(h * 0.5f);
    pill.fill_top     = lerp(lerp(style_.widget_bg, style_.widget_hover, a.hover), tint.scaled_alpha(0.24f), a.toggle);
    pill.fill_bottom  = pill.fill_top;
    pill.border       = lerp(style_.widget_border, tint, a.toggle);
    pill.border_width = style_.border_width;
    dl_.shape(r, pill);

    f32 x = r.min.x + side;
    if (!o.icon.empty()) {
        const f32 ih = font_.line_height(o.icon_font);
        dl_.text({x, r.min.y + (h - ih) * 0.5f}, lerp(style_.text_dim, tint, a.toggle), o.icon, o.icon_font);
        x += iw;
    }
    label_draw({x, r.min.y + (h - ts.y) * 0.5f}, lerp(style_.text_dim, style_.text, std::max(a.toggle, a.hover)), shown, f);

    if (o.closable) {
        const f32   hover = close_in.hovered ? 1.0f : 0.0f;
        const color xc    = lerp(style_.text_dim, style_.text, hover);
        if (close_in.hovered) { dl_.circle_filled(close_r.center(), cs * 0.5f, style_.widget_active.scaled_alpha(0.8f)); }
        const f32 k = cs * 0.22f;
        const vec2 c = close_r.center();
        dl_.line({c.x - k, c.y - k}, {c.x + k, c.y + k}, xc, 1.5f);
        dl_.line({c.x - k, c.y + k}, {c.x + k, c.y - k}, xc, 1.5f);
    }
    return res;
}

} // namespace strata

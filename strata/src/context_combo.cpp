// multi-select dropdown

#include "strata/context.hpp"

#include <algorithm>

namespace strata {

bool context::combo_multi(std::string_view label, bool* selected, const std::string_view* items, std::size_t count,
                          std::string_view placeholder)
{
    if (cur_ == nullptr || count == 0 || selected == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool open = popup_id_ == key;
    if (in.pressed) {
        if (open) {
            popup_id_ = 0;
            open      = false;
        } else {
            popup_id_     = key;
            popup_scroll_ = 0.0f;
            popup_hover_  = 0;
            open          = true;
        }
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(style_.widget_bg, style_.widget_hover, a.hover), style_.rounding * 0.8f);
    field.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
    dl_.shape(box, field);

    // what is chosen, as text that is clipped to the box
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < count; ++i) { chosen += selected[i] ? 1u : 0u; }
    std::string summary;
    color summary_color = style_.text;
    if (chosen == 0) {
        summary       = std::string{placeholder};
        summary_color = style_.text_dim;
    } else if (chosen == count && count > 2) {
        summary = "all (" + std::to_string(count) + ")";
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (selected[i]) {
                if (!summary.empty()) { summary += ", "; }
                summary.append(visible_label(items[i]));
            }
        }
    }
    const vec2 tsize = font_.measure(f, summary);
    dl_.push_clip({{box.min.x, box.min.y}, {box.max.x - style_.frame_padding.x - 14.0f, box.max.y}});
    dl_.text({box.min.x + style_.frame_padding.x, box.min.y + (box.height() - tsize.y) * 0.5f}, summary_color, summary, f);
    dl_.pop_clip();

    const vec2 c{box.max.x - style_.frame_padding.x - 4.0f, box.center().y};
    const f32  s = 4.0f;
    const color chev = lerp(style_.text_dim, style_.text, std::max(a.hover, a.toggle));
    if (open) {
        dl_.triangle_filled({c.x - s, c.y + s * 0.5f}, {c.x, c.y - s * 0.6f}, {c.x + s, c.y + s * 0.5f}, chev);
    } else {
        dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.6f}, chev);
    }
    if (!open) {
        return false;
    }

    // --- the popup: rows with check boxes; clicking a row toggles it and keeps the list open ---------------------
    bool changed = false;
    const f32 item_h   = frame_height() - 4.0f;
    const bool toolbar = count > 4; // "all" / "none"
    const u32  visible = static_cast<u32>(std::min<std::size_t>(count, 8));
    const f32  pad     = 4.0f;
    const f32  tool_h  = toolbar ? item_h : 0.0f;
    const f32  list_h  = tool_h + static_cast<f32>(visible) * item_h + 2.0f * pad;

    rect list = {{box.min.x, box.max.y + 4.0f}, {box.max.x, box.max.y + 4.0f + list_h}};
    if (list.max.y > display_.y - 4.0f && box.min.y - 4.0f - list_h >= 4.0f) {
        list = {{box.min.x, box.min.y - 4.0f - list_h}, {box.max.x, box.min.y - 4.0f}};
    }
    popup_open_cur_   = true;
    popup_rect_cur_   = list;
    popup_anchor_cur_ = box;

    const f32 view_h     = static_cast<f32>(visible) * item_h;
    const f32 max_scroll = std::max(0.0f, static_cast<f32>(count) * item_h - view_h);
    if (list.contains(mouse_) && wheel_ != 0.0f) {
        popup_scroll_ -= wheel_ * item_h * 1.5f;
        wheel_consumed_ = true;
    }
    for (u32 i = 0; i < key_count_; ++i) { // keyboard: Up / Down move, Enter toggles, Esc closes
        switch (keys_[i].k) {
        case key::down:   popup_hover_ = std::min(popup_hover_ + 1, static_cast<int>(count) - 1); break;
        case key::up:     popup_hover_ = std::max(popup_hover_ - 1, 0); break;
        case key::enter:
            if (popup_hover_ >= 0 && popup_hover_ < static_cast<int>(count)) {
                selected[popup_hover_] = !selected[popup_hover_];
                changed = true;
            }
            break;
        case key::escape: popup_id_ = 0; break;
        default: break;
        }
    }
    key_count_ = 0;
    if (popup_hover_ >= 0) {
        const f32 top = static_cast<f32>(popup_hover_) * item_h;
        if (top < popup_scroll_)                   { popup_scroll_ = top; }
        if (top + item_h > popup_scroll_ + view_h) { popup_scroll_ = top + item_h - view_h; }
    }
    popup_scroll_ = std::clamp(popup_scroll_, 0.0f, max_scroll);

    const u32 previous_owner = run_owner_;
    switch_run(run_overlay);
    in_overlay_ = true;
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});

    shape_style body;
    body.radius        = radii(style_.rounding * 0.8f);
    body.fill_top      = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = style_.border;
    body.border_width  = style_.border_width;
    body.shadow        = style_.shadow;
    body.shadow_blur   = style_.shadow_blur * 0.8f;
    body.shadow_offset = {0.0f, style_.shadow_blur * 0.3f};
    popup_panel(list, body);

    dl_.push_clip({{list.min.x, list.min.y + 1.0f}, {list.max.x, list.max.y - 1.0f}});
    f32 y0 = list.min.y + pad;
    if (toolbar) { // two small buttons that set every row at once
        const rect all_r  = {{list.min.x + pad, y0}, {list.center().x - 1.0f, y0 + item_h}};
        const rect none_r = {{list.center().x + 1.0f, y0}, {list.max.x - pad, y0 + item_h}};
        for (int k = 0; k < 2; ++k) {
            const rect& r = k == 0 ? all_r : none_r;
            const interaction ti = interact(hash_id(k == 0 ? "##all" : "##none", key), r);
            if (ti.pressed) {
                for (std::size_t i = 0; i < count; ++i) { selected[i] = k == 0; }
                changed = true;
            }
            if (ti.hovered) {
                shape_style row;
                row.radius      = radii(style_.rounding * 0.55f);
                row.fill_top    = style_.widget_hover.scaled_alpha(0.8f);
                row.fill_bottom = row.fill_top;
                dl_.shape(r, row);
            }
            const std::string_view t = k == 0 ? "select all" : "clear";
            const vec2 ts = font_.measure(f, t);
            dl_.text({r.center().x - ts.x * 0.5f, r.min.y + (r.height() - ts.y) * 0.5f}, ti.hovered ? style_.text : style_.text_dim, t, f);
        }
        y0 += item_h;
    }
    const f32 rows_top = y0;
    dl_.push_clip({{list.min.x, rows_top}, {list.max.x, rows_top + view_h}});
    for (std::size_t i = 0; i < count; ++i) {
        const f32  y = rows_top + static_cast<f32>(i) * item_h - popup_scroll_;
        const rect r = {{list.min.x + pad, y}, {list.max.x - pad - (max_scroll > 0.0f ? 6.0f : 0.0f), y + item_h}};
        if (r.max.y < rows_top || r.min.y > rows_top + view_h) {
            continue;
        }
        const id ik = hash_id({reinterpret_cast<const char*>(&i), sizeof(i)}, key);
        const interaction it = interact(ik, r);
        if (it.hovered) { popup_hover_ = static_cast<int>(i); }
        if (it.pressed) {
            selected[i] = !selected[i];
            changed = true;
        }

        const bool hot = static_cast<int>(i) == popup_hover_;
        if (hot) {
            shape_style row;
            row.radius      = radii(style_.rounding * 0.55f);
            row.fill_top    = style_.accent.scaled_alpha(0.26f);
            row.fill_bottom = row.fill_top;
            dl_.shape(r, row);
        }
        const f32  bs = item_h - 10.0f;
        const rect bx = rect::from_size({r.min.x + 7.0f, r.min.y + (r.height() - bs) * 0.5f}, {bs, bs});
        shape_style cb;
        cb.radius       = radii(3.0f);
        cb.fill_top     = selected[i] ? style_.accent : style_.widget_bg;
        cb.fill_bottom  = cb.fill_top;
        cb.border       = selected[i] ? style_.accent_hover : style_.widget_border;
        cb.border_width = 1.0f;
        dl_.shape(bx, cb);
        if (selected[i]) {
            dl_.line({bx.min.x + bs * 0.24f, bx.min.y + bs * 0.52f}, {bx.min.x + bs * 0.43f, bx.min.y + bs * 0.72f}, color{255, 255, 255, 255}, 1.8f);
            dl_.line({bx.min.x + bs * 0.43f, bx.min.y + bs * 0.72f}, {bx.min.x + bs * 0.78f, bx.min.y + bs * 0.30f}, color{255, 255, 255, 255}, 1.8f);
        }
        const vec2 ts = label_size(f, items[i]);
        label_draw({bx.max.x + 9.0f, r.min.y + (r.height() - ts.y) * 0.5f}, style_.text, items[i], f);
    }
    dl_.pop_clip();
    dl_.pop_clip();

    if (max_scroll > 0.0f) {
        const f32 thumb_h = std::max(16.0f, view_h * view_h / (view_h + max_scroll));
        const f32 thumb_y = rows_top + (view_h - thumb_h) * (popup_scroll_ / max_scroll);
        shape_style thumb;
        thumb.radius      = radii(2.0f);
        thumb.fill_top    = style_.text_dim.scaled_alpha(0.5f);
        thumb.fill_bottom = thumb.fill_top;
        dl_.shape({{list.max.x - 8.0f, thumb_y}, {list.max.x - 4.0f, thumb_y + thumb_h}}, thumb);
    }

    dl_.pop_clip();
    in_overlay_ = false;
    switch_run(previous_owner);
    return changed;
}

} // namespace strata

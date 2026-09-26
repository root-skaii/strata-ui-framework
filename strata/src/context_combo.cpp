// multi-select dropdown, and the dropdown with a filter box

#include "strata/context.hpp"

#include <algorithm>
#include <cctype>

namespace strata {

namespace {

// case-insensitive substring, ascii only (which is what a type or member name is)
[[nodiscard]] bool contains_ci(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lower(haystack[i + k]) == lower(needle[k])) { ++k; }
        if (k == needle.size()) { return true; }
    }
    return false;
}

} // namespace

bool context::combo_filtered(std::string_view label, int& current, const std::string_view* items, std::size_t count,
                             std::string_view hint)
{
    if (cur_ == nullptr || count == 0 || items == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    current = std::clamp(current, 0, static_cast<int>(count) - 1);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool open = popup_id_ == key;
    if (in.pressed) {
        if (open) {
            popup_id_ = 0;
            open      = false;
        } else {
            popup_id_          = key;
            open               = true;
            combo_filter_.clear();
            combo_filter_hover_ = 0;
            combo_filter_focus_ = true; // the search field takes the keyboard on the frame it appears
        }
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(style_.widget_bg, style_.widget_hover, a.hover), style_.rounding * 0.8f);
    field.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
    dl_.shape(box, field);

    const std::string_view shown = items[static_cast<std::size_t>(current)];
    const vec2 tsize = label_size(f, shown);
    dl_.push_clip({{box.min.x, box.min.y}, {box.max.x - style_.frame_padding.x - 14.0f, box.max.y}});
    label_draw({box.min.x + style_.frame_padding.x, box.min.y + (box.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    dl_.pop_clip();

    const vec2  c{box.max.x - style_.frame_padding.x - 4.0f, box.center().y};
    const f32   arrow = 4.0f;
    const color chev  = lerp(style_.text_dim, style_.text, std::max(a.hover, a.toggle));
    if (open) {
        dl_.triangle_filled({c.x - arrow, c.y + arrow * 0.5f}, {c.x, c.y - arrow * 0.6f}, {c.x + arrow, c.y + arrow * 0.5f}, chev);
    } else {
        dl_.triangle_filled({c.x - arrow, c.y - arrow * 0.5f}, {c.x + arrow, c.y - arrow * 0.5f}, {c.x, c.y + arrow * 0.6f}, chev);
    }

    bool changed = false;
    if (!open) {
        return false;
    }

    // what is left after the filter, as indices into `items`
    combo_filter_hits_.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (contains_ci(visible_label(items[i]), combo_filter_)) { combo_filter_hits_.push_back(static_cast<u32>(i)); }
    }
    const int hits = static_cast<int>(combo_filter_hits_.size());
    combo_filter_hover_ = hits == 0 ? 0 : std::clamp(combo_filter_hover_, 0, hits - 1);

    // the keys are read before the search field is submitted, because the field uses its own up on the way past
    int  want_pick = -1;
    bool close_now = false;
    for (u32 i = 0; i < key_count_; ++i) {
        switch (keys_[i].k) {
        case key::down:   combo_filter_hover_ = hits == 0 ? 0 : std::min(combo_filter_hover_ + 1, hits - 1); break;
        case key::up:     combo_filter_hover_ = std::max(combo_filter_hover_ - 1, 0); break;
        case key::page_down: combo_filter_hover_ = hits == 0 ? 0 : std::min(combo_filter_hover_ + 8, hits - 1); break;
        case key::page_up:   combo_filter_hover_ = std::max(combo_filter_hover_ - 8, 0); break;
        case key::enter:
            if (combo_filter_hover_ < hits) { want_pick = static_cast<int>(combo_filter_hits_[static_cast<std::size_t>(combo_filter_hover_)]); }
            break;
        default: break;
        }
    }

    const f32 row_h   = frame_height() - 2.0f;
    const u32 visible = static_cast<u32>(std::min<std::size_t>(std::max<std::size_t>(combo_filter_hits_.size(), 1), 9));
    const f32 list_h  = static_cast<f32>(visible) * (row_h + style_.item_spacing) + 6.0f;
    const f32 width   = std::max(box.width(), 220.0f);
    const f32 height  = frame_height() + style_.item_spacing + list_h + 2.0f * style_.padding;

    if (begin_popup_at(key, box, {width, height})) {
        push_id(label);
        if (combo_filter_focus_) {
            request_text_focus("##filter");
            combo_filter_focus_ = false;
        }
        set_next_item_width(layout_.width);
        (void)input_text("##filter", combo_filter_, hint, input_flags::clear_button, 128);

        if (auto list = child("##hits", {0.0f, list_h}, child_flags::none)) {
            if (hits == 0) {
                text_dim("no match");
            } else {
                list_clipper clip{*this, static_cast<std::size_t>(hits), row_h};
                while (clip.step()) {
                    for (int k = clip.begin(); k < clip.end(); ++k) {
                        const u32 item = combo_filter_hits_[static_cast<std::size_t>(k)];
                        const std::string_view text_of = items[item];
                        if (selectable(text_of, {reinterpret_cast<const char*>(&item), sizeof(item)},
                                       k == combo_filter_hover_)) {
                            want_pick = static_cast<int>(item);
                        }
                        // keep the row the keyboard is on in view
                        if (k == combo_filter_hover_) { ensure_item_visible(); }
                    }
                }
            }
        }
        pop_id();
        end_popup_at();
    } else {
        close_now = true; // Esc closed it
    }

    if (want_pick >= 0) {
        changed   = current != want_pick;
        current   = want_pick;
        popup_id_ = 0;
        focus_id_ = 0;
    } else if (close_now) {
        focus_id_ = 0;
    }
    return changed;
}

bool context::combo_multi(std::string_view label, bool* selected, const std::string_view* items, std::size_t count,
                          std::string_view placeholder)
{
    if (cur_ == nullptr || count == 0 || selected == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);

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

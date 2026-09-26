// tab bars: a row of tabs with a sliding highlight; optionally closable, reorderable, with an add button, and scrolling
// (plus a list of all tabs) when they do not fit

#include "strata/context.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace strata {

bool context::tab_bar(std::string_view id_label, const tab_desc* tabs, std::size_t count, int& selected, font_id icon_font)
{
    return tab_bar(id_label, tabs, count, selected, tab_bar_flags::none, icon_font).changed;
}

tab_events context::tab_bar(std::string_view id_label, const tab_desc* tabs, std::size_t count, int& selected,
                            tab_bar_flags flags, font_id icon_font)
{
    tab_events ev;
    if (cur_ == nullptr || count == 0) {
        return ev;
    }
    push_id(id_label);

    const font_id f = current_font();
    const rect row  = layout_place({layout_.width, frame_height() + 2.0f});

    const std::size_t n = count;
    selected = std::clamp(selected, 0, static_cast<int>(n) - 1);

    const bool closable  = has_flag(flags, tab_bar_flags::closable);
    const bool reorder   = has_flag(flags, tab_bar_flags::reorderable);
    const bool add_tab   = has_flag(flags, tab_bar_flags::add_button);
    const f32  close_w   = closable ? 20.0f : 0.0f; // the room at the right end of a tab for its x
    const f32  add_w     = add_tab ? 26.0f : 0.0f;
    const f32  list_w    = 26.0f;

    // the tabs side by side, in the coordinates of the unscrolled row
    tab_cells_.assign(n, rect{});
    f32 x = row.min.x;
    for (std::size_t i = 0; i < n; ++i) {
        const f32 iw = tabs[i].icon.empty() ? 0.0f : font_.measure(icon_font, tabs[i].icon).x + 7.0f;
        const f32 w  = label_size(f, visible_label(tabs[i].label)).x + iw + 26.0f + close_w;
        tab_cells_[i] = {{x, row.min.y}, {x + w, row.max.y - 2.0f}};
        x += w + 2.0f;
    }
    const f32 content_w = x - 2.0f - row.min.x;
    const bool overflow = content_w + (add_tab ? add_w + 2.0f : 0.0f) > row.width() + 0.5f;

    // when they do not fit the tabs scroll inside `region`; the add button and a list of every tab stay at the right end
    const rect region = overflow ? rect{row.min, {row.max.x - list_w - 2.0f - (add_tab ? add_w + 2.0f : 0.0f), row.max.y}} : row;
    const f32  view_w = region.width();
    const f32  max_scroll = overflow ? std::max(0.0f, content_w - view_w) : 0.0f;

    // scroll: `active` is where it wants to be, `toggle` what is shown, `custom` the tab that was in view last
    anim_slot& sc = anim_for(hash_id("##tscroll", current_seed()));
    if (!sc.custom_init) {
        sc.custom_init = true;
        sc.custom      = -1.0f;
    }
    if (overflow && pointer_over(region) && wheel_ != 0.0f && !wheel_consumed_) {
        sc.active -= wheel_ * 48.0f;
        wheel_consumed_ = true;
    }
    if (static_cast<int>(sc.custom) != selected) { // a new selection scrolls into view
        const rect& c = tab_cells_[static_cast<std::size_t>(selected)];
        if (c.max.x - row.min.x > sc.active + view_w) { sc.active = c.max.x - row.min.x - view_w + 8.0f; }
        if (c.min.x - row.min.x < sc.active)          { sc.active = c.min.x - row.min.x - 8.0f; }
        sc.custom = static_cast<f32>(selected);
    }
    sc.active = std::clamp(sc.active, 0.0f, max_scroll);
    sc.toggle = approach(sc.toggle, sc.active, 18.0f);
    if (!overflow) { sc.toggle = sc.active = 0.0f; }
    const f32 shift = -sc.toggle;

    if (tabdrag_tab_ != 0 && !mouse_down_) {
        tabdrag_tab_ = 0;
    }

    dl_.push_clip(region);
    tab_emph_.assign(n, 0.0f);
    std::vector<rect>& cells = tab_cells_;
    for (rect& c : cells) { c = {{c.min.x + shift, c.min.y}, {c.max.x + shift, c.max.y}}; }

    // the tab being dragged follows the pointer; the others stay in their slots until the caller applies the move
    rect  drag_rect{};
    int   drag_index = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const id   key  = hash_id(tabs[i].key(), current_seed());
        const rect body = {cells[i].min, {cells[i].max.x - close_w, cells[i].max.y}};

        if (closable) {
            const rect x_r = rect::from_size({cells[i].max.x - 18.0f, cells[i].center().y - 7.0f}, {14.0f, 14.0f});
            const interaction xin = interact(hash_id("##x", key), x_r.expanded(1.0f));
            if (xin.pressed) { ev.closed = static_cast<int>(i); }
            anim_slot& xa = anim_for(hash_id("##x", key));
            xa.hover = approach(xa.hover, xin.hovered ? 1.0f : 0.0f);
        }

        const interaction in = interact(key, body);
        if (in.pressed && static_cast<int>(i) != selected) {
            selected   = static_cast<int>(i);
            ev.changed = true;
        }
        if (closable && in.hovered && mouse_middle_pressed_) { ev.closed = static_cast<int>(i); }
        if (reorder) {
            if (active_ == key && mouse_pressed_) {
                tabdrag_cand_    = key;
                tabdrag_press_x_ = mouse_.x;
            }
            if (tabdrag_tab_ == 0 && tabdrag_cand_ == key && active_ == key && mouse_down_ && std::abs(mouse_.x - tabdrag_press_x_) > 5.0f) {
                tabdrag_tab_  = key;
                tabdrag_grab_ = mouse_.x - cells[i].min.x;
                if (static_cast<int>(i) != selected) { selected = static_cast<int>(i); ev.changed = true; }
            }
            if (tabdrag_tab_ == key && mouse_down_) {
                const f32 left = std::clamp(mouse_.x - tabdrag_grab_, region.min.x, std::max(region.min.x, region.max.x - cells[i].width()));
                drag_rect  = rect::from_size({left, cells[i].min.y}, {cells[i].width(), cells[i].height()});
                drag_index = static_cast<int>(i);
            }
        }
        anim_slot& a = anim_for(key);
        a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        a.toggle = approach(a.toggle, static_cast<int>(i) == selected ? 1.0f : 0.0f);
        tab_emph_[i] = std::max(a.toggle, a.hover * 0.75f);

        if (a.hover > 0.01f && static_cast<int>(i) != selected) {
            shape_style hover;
            hover.radius      = radii(style_.rounding * 0.7f);
            hover.fill_top    = style_.widget_hover.scaled_alpha(0.7f * a.hover);
            hover.fill_bottom = hover.fill_top;
            dl_.shape(cells[i], hover);
        }
    }

    // where the dragged tab would land: after every other tab whose middle it has passed
    if (drag_index >= 0) {
        int to = 0;
        for (std::size_t j = 0; j < n; ++j) {
            if (static_cast<int>(j) != drag_index && cells[j].center().x < drag_rect.center().x) { ++to; }
        }
        if (to != drag_index) {
            ev.moved_from = drag_index;
            ev.moved_to   = to;
        }
    }

    const rect sel  = drag_index >= 0 && drag_index == selected ? drag_rect : cells[static_cast<std::size_t>(selected)];
    // (relative to the row and unscrolled: moving or scrolling the window must not make the highlight lag)
    const f32  ix   = row.min.x + animate("tab_x", sel.min.x - shift - row.min.x, 20.0f) + shift;
    const f32  iw   = animate("tab_w", sel.width(), 20.0f);
    f32 pill_x = ix;
    if (drag_index >= 0 && drag_index == selected) { // glued to the pointer while dragging
        pill_x = sel.min.x;
        anim_for(hash_id("tab_x", current_seed())).custom = sel.min.x - shift - row.min.x;
    }
    shape_style pill;
    pill.radius      = radii(style_.rounding * 0.7f);
    pill.fill_top    = style_.accent.scaled_alpha(0.20f);
    pill.fill_bottom = style_.accent.scaled_alpha(0.10f);
    dl_.shape({{pill_x, sel.min.y}, {pill_x + iw, sel.max.y}}, pill);

    dl_.rect_filled({{row.min.x, row.max.y - 1.0f}, {row.max.x, row.max.y}}, style_.border);
    shape_style line;
    line.radius      = radii(1.0f);
    line.fill_top    = style_.accent_hover;
    line.fill_bottom = style_.accent;
    dl_.shape({{pill_x + 8.0f, row.max.y - 3.0f}, {pill_x + iw - 8.0f, row.max.y - 1.0f}}, line);

    for (std::size_t i = 0; i < n; ++i) {
        const rect cell = static_cast<int>(i) == drag_index ? drag_rect : cells[i];
        const color c = lerp(style_.text_dim, style_.text, tab_emph_[i]);
        const std::string_view shown = visible_label(tabs[i].label);
        const vec2 tsize = label_size(f, shown);
        const f32  iwid  = tabs[i].icon.empty() ? 0.0f : font_.measure(icon_font, tabs[i].icon).x;
        const f32  gap   = tabs[i].icon.empty() ? 0.0f : 7.0f;
        f32 tx = cell.min.x + (cell.width() - close_w - (iwid + gap + tsize.x)) * 0.5f;
        if (!tabs[i].icon.empty()) {
            const f32 ih = font_.line_height(icon_font);
            dl_.text({tx, cell.min.y + (cell.height() - ih) * 0.5f},
                     lerp(c, style_.accent_hover, tab_emph_[i] * 0.5f), tabs[i].icon, icon_font);
            tx += iwid + gap;
        }
        label_draw({tx, cell.min.y + (cell.height() - tsize.y) * 0.5f}, c, shown, f);

        if (closable) {
            const id       key = hash_id(tabs[i].key(), current_seed());
            const f32      h   = anim_for(hash_id("##x", key)).hover;
            const vec2     m   = {cell.max.x - 11.0f, cell.center().y};
            const f32      k   = 3.2f;
            const color    xc  = lerp(lerp(style_.text_dim, style_.text, tab_emph_[i]), style_.text, h);
            if (h > 0.01f) { dl_.circle_filled(m, 7.5f, style_.widget_active.scaled_alpha(0.85f * h)); }
            dl_.line({m.x - k, m.y - k}, {m.x + k, m.y + k}, xc, 1.4f);
            dl_.line({m.x - k, m.y + k}, {m.x + k, m.y - k}, xc, 1.4f);
        }
    }
    dl_.pop_clip();

    // "+" after the last tab, or pinned at the right end when the tabs scroll
    if (add_tab) {
        const f32  ax = overflow ? region.max.x + 2.0f : x + 2.0f;
        const rect ar = {{ax, row.min.y + 1.0f}, {ax + add_w - 2.0f, row.max.y - 3.0f}};
        const interaction in = interact(hash_id("##add", current_seed()), ar);
        ev.add = in.pressed;
        anim_slot& a = anim_for(hash_id("##add", current_seed()));
        a.hover = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        if (a.hover > 0.01f) {
            shape_style hover;
            hover.radius      = radii(style_.rounding * 0.7f);
            hover.fill_top    = style_.widget_hover.scaled_alpha(0.8f * a.hover);
            hover.fill_bottom = hover.fill_top;
            dl_.shape(ar, hover);
        }
        const vec2  m  = ar.center();
        const color pc = lerp(style_.text_dim, style_.text, a.hover);
        dl_.line({m.x - 5.0f, m.y}, {m.x + 5.0f, m.y}, pc, 1.5f);
        dl_.line({m.x, m.y - 5.0f}, {m.x, m.y + 5.0f}, pc, 1.5f);
    }

    // a list of every tab: the way to a tab that has scrolled out of view
    if (overflow) {
        const rect lr = {{row.max.x - list_w, row.min.y + 1.0f}, {row.max.x, row.max.y - 3.0f}};
        const interaction in = interact(hash_id("##list", current_seed()), lr);
        if (in.pressed) { toggle_popup("##tablist"); }
        anim_slot& a = anim_for(hash_id("##list", current_seed()));
        a.hover  = approach(a.hover, in.hovered || popup_is_open("##tablist") ? 1.0f : 0.0f);
        if (a.hover > 0.01f) {
            shape_style hover;
            hover.radius      = radii(style_.rounding * 0.7f);
            hover.fill_top    = style_.widget_hover.scaled_alpha(0.8f * a.hover);
            hover.fill_bottom = hover.fill_top;
            dl_.shape(lr, hover);
        }
        const vec2 m = lr.center();
        dl_.triangle_filled({m.x - 4.0f, m.y - 2.0f}, {m.x + 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, lerp(style_.text_dim, style_.text, a.hover));
        if (auto p = popup("##tablist", 240.0f)) {
            const f32 pitch = font_.line_height(f) + 8.0f + style_.item_spacing; // a selectable row and the gap after it
            const f32 shown = std::min(static_cast<f32>(n), 9.0f) * pitch - style_.item_spacing;
            if (auto rows = child("##tablistrows", {0.0f, shown}, child_flags::no_padding)) { // long lists scroll
                for (std::size_t i = 0; i < n; ++i) {
                    push_id(std::to_string(i));
                    if (selectable(tabs[i].label, static_cast<int>(i) == selected)) {
                        if (static_cast<int>(i) != selected) { selected = static_cast<int>(i); ev.changed = true; }
                        sc.custom = -1.0f; // scroll it into view
                        close_popup();
                    }
                    pop_id();
                }
            }
        }
    }

    pop_id();
    return ev;
}

} // namespace strata

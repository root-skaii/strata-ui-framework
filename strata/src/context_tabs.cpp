// tab bars: tabs with a sliding highlight; optionally closable, reorderable, an add button, and scrolling with a
// list of all tabs when they overflow

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "widget_util.hpp"
#include "core/part_id.hpp"

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
    if (m_->cur_ == nullptr || count == 0) {
        return ev;
    }
    push_id(id_label);

    const font_id f = current_font();
    const rect row  = layout_place({m_->layout_.width, frame_height() + 2.0f});

    const std::size_t n = count;
    selected = std::clamp(selected, 0, static_cast<int>(n) - 1);

    const bool closable  = has_flag(flags, tab_bar_flags::closable);
    const bool reorder   = has_flag(flags, tab_bar_flags::reorderable);
    const bool add_tab   = has_flag(flags, tab_bar_flags::add_button);
    const f32  close_w   = closable ? 20.0f : 0.0f; // the room at the right end of a tab for its x
    const f32  add_w     = add_tab ? 26.0f : 0.0f;
    const f32  list_w    = 26.0f;

    // the tabs side by side, in the coordinates of the unscrolled row
    m_->tab_cells_.assign(n, rect{});
    f32 x = row.min.x;
    for (std::size_t i = 0; i < n; ++i) {
        const f32 iw = tabs[i].icon.empty() ? 0.0f : m_->font_.measure(icon_font, tabs[i].icon).x + 7.0f;
        const f32 w  = label_size(f, visible_label(tabs[i].label)).x + iw + 26.0f + close_w;
        m_->tab_cells_[i] = {{x, row.min.y}, {x + w, row.max.y - 2.0f}};
        x += w + 2.0f;
    }
    const f32 content_w = x - 2.0f - row.min.x;
    const bool overflow = content_w + (add_tab ? add_w + 2.0f : 0.0f) > row.width() + 0.5f;

    // overflowing tabs scroll inside `region`; the add and list buttons stay at the right end
    const rect region = overflow ? rect{row.min, {row.max.x - list_w - 2.0f - (add_tab ? add_w + 2.0f : 0.0f), row.max.y}} : row;
    const f32  view_w = region.width();
    const f32  max_scroll = overflow ? std::max(0.0f, content_w - view_w) : 0.0f;

    // scroll: `active` = target, `toggle` = shown, `custom` = last tab in view
    anim_slot& sc = anim_for(part_id(part::tab_scroll, current_seed()));
    if (!sc.custom_init) {
        sc.custom_init = true;
        sc.custom      = -1.0f;
    }
    if (overflow && pointer_over(region) && m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_) {
        sc.active -= wheel_scroll(m_->font_.line_height(0), view_w);
        m_->wheel_consumed_ = true;
    }
    if (static_cast<int>(sc.custom) != selected) { // a new selection scrolls into view
        const rect& c = m_->tab_cells_[static_cast<std::size_t>(selected)];
        if (c.max.x - row.min.x > sc.active + view_w) { sc.active = c.max.x - row.min.x - view_w + 8.0f; }
        if (c.min.x - row.min.x < sc.active)          { sc.active = c.min.x - row.min.x - 8.0f; }
        sc.custom = static_cast<f32>(selected);
    }
    sc.active = std::clamp(sc.active, 0.0f, max_scroll);
    sc.toggle = approach(sc.toggle, sc.active, 18.0f);
    if (!overflow) { sc.toggle = sc.active = 0.0f; }
    const f32 shift = -sc.toggle;

    if (m_->tabdrag_tab_ != 0 && !m_->input_.mouse_down_) {
        m_->tabdrag_tab_ = 0;
    }

    m_->dl_.push_clip(region);
    m_->tab_emph_.assign(n, 0.0f);
    std::vector<rect>& cells = m_->tab_cells_;
    for (rect& c : cells) { c = {{c.min.x + shift, c.min.y}, {c.max.x + shift, c.max.y}}; }

    // the dragged tab follows the pointer; the others keep their slots until the caller applies the move
    rect  drag_rect{};
    int   drag_index = -1;
    for (std::size_t i = 0; i < n; ++i) {
        const id   key  = hash_id(tabs[i].key(), current_seed());
        const rect body = {cells[i].min, {cells[i].max.x - close_w, cells[i].max.y}};

        if (closable) {
            const rect x_r = rect::from_size({cells[i].max.x - 18.0f, cells[i].center().y - 7.0f}, {14.0f, 14.0f});
            const interaction xin = interact(part_id(part::tab_close, key), x_r.expanded(1.0f));
            if (xin.pressed) { ev.closed = static_cast<int>(i); }
            anim_slot& xa = anim_for(part_id(part::tab_close, key));
            xa.hover = approach(xa.hover, xin.hovered ? 1.0f : 0.0f);
        }

        const interaction in = interact(key, body);
        if (in.pressed && static_cast<int>(i) != selected) {
            selected   = static_cast<int>(i);
            ev.changed = true;
        }
        if (closable && in.hovered && m_->input_.mouse_middle_pressed_) { ev.closed = static_cast<int>(i); }
        if (reorder) {
            if (m_->active_ == key && m_->input_.mouse_pressed_) {
                m_->tabdrag_cand_    = key;
                m_->tabdrag_press_x_ = m_->input_.mouse_.x;
            }
            if (m_->tabdrag_tab_ == 0 && m_->tabdrag_cand_ == key && m_->active_ == key && m_->input_.mouse_down_ && std::abs(m_->input_.mouse_.x - m_->tabdrag_press_x_) > 5.0f) {
                m_->tabdrag_tab_  = key;
                m_->tabdrag_grab_ = m_->input_.mouse_.x - cells[i].min.x;
                if (static_cast<int>(i) != selected) { selected = static_cast<int>(i); ev.changed = true; }
            }
            if (m_->tabdrag_tab_ == key && m_->input_.mouse_down_) {
                const f32 left = std::clamp(m_->input_.mouse_.x - m_->tabdrag_grab_, region.min.x, std::max(region.min.x, region.max.x - cells[i].width()));
                drag_rect  = rect::from_size({left, cells[i].min.y}, {cells[i].width(), cells[i].height()});
                drag_index = static_cast<int>(i);
            }
        }
        anim_slot& a = anim_for(key);
        a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        a.toggle = approach(a.toggle, static_cast<int>(i) == selected ? 1.0f : 0.0f);
        m_->tab_emph_[i] = std::max(a.toggle, a.hover * 0.75f);

        if (a.hover > 0.01f && static_cast<int>(i) != selected) {
            shape_style hover;
            hover.radius      = radii(m_->style_.rounding * 0.7f);
            hover.fill_top    = m_->style_.widget_hover.scaled_alpha(0.7f * a.hover);
            hover.fill_bottom = hover.fill_top;
            m_->dl_.shape(cells[i], hover);
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
    // (row-relative and unscrolled, so moving / scrolling the window does not make the highlight lag)
    const f32  ix   = row.min.x + animate("tab_x", sel.min.x - shift - row.min.x, 20.0f) + shift;
    const f32  iw   = animate("tab_w", sel.width(), 20.0f);
    f32 pill_x = ix;
    if (drag_index >= 0 && drag_index == selected) { // glued to the pointer while dragging
        pill_x = sel.min.x;
        anim_for(widget_id("tab_x")).custom = sel.min.x - shift - row.min.x;
    }
    shape_style pill;
    pill.radius      = radii(m_->style_.rounding * 0.7f);
    pill.fill_top    = m_->style_.accent.scaled_alpha(0.20f);
    pill.fill_bottom = m_->style_.accent.scaled_alpha(0.10f);
    m_->dl_.shape({{pill_x, sel.min.y}, {pill_x + iw, sel.max.y}}, pill);

    m_->dl_.rect_filled({{row.min.x, row.max.y - 1.0f}, {row.max.x, row.max.y}}, m_->style_.border);
    shape_style line;
    line.radius      = radii(1.0f);
    line.fill_top    = m_->style_.accent_hover;
    line.fill_bottom = m_->style_.accent;
    m_->dl_.shape({{pill_x + 8.0f, row.max.y - 3.0f}, {pill_x + iw - 8.0f, row.max.y - 1.0f}}, line);

    for (std::size_t i = 0; i < n; ++i) {
        const rect cell = static_cast<int>(i) == drag_index ? drag_rect : cells[i];
        const color c = lerp(m_->style_.text_dim, m_->style_.text, m_->tab_emph_[i]);
        const std::string_view shown = visible_label(tabs[i].label);
        const vec2 tsize = label_size(f, shown);
        const f32  iwid  = tabs[i].icon.empty() ? 0.0f : m_->font_.measure(icon_font, tabs[i].icon).x;
        const f32  gap   = tabs[i].icon.empty() ? 0.0f : 7.0f;
        f32 tx = cell.min.x + (cell.width() - close_w - (iwid + gap + tsize.x)) * 0.5f;
        if (!tabs[i].icon.empty()) {
            const f32 ih = m_->font_.line_height(icon_font);
            m_->dl_.text({tx, cell.min.y + (cell.height() - ih) * 0.5f},
                     lerp(c, m_->style_.accent_hover, m_->tab_emph_[i] * 0.5f), tabs[i].icon, icon_font);
            tx += iwid + gap;
        }
        label_draw({tx, cell.min.y + (cell.height() - tsize.y) * 0.5f}, c, shown, f);

        if (closable) {
            const id       key = hash_id(tabs[i].key(), current_seed());
            const f32      h   = anim_for(part_id(part::tab_close, key)).hover;
            const vec2     m   = {cell.max.x - 11.0f, cell.center().y};
            const f32      k   = 3.2f;
            const color    xc  = lerp(lerp(m_->style_.text_dim, m_->style_.text, m_->tab_emph_[i]), m_->style_.text, h);
            if (h > 0.01f) { m_->dl_.circle_filled(m, 7.5f, m_->style_.widget_active.scaled_alpha(0.85f * h)); }
            m_->dl_.line({m.x - k, m.y - k}, {m.x + k, m.y + k}, xc, 1.4f);
            m_->dl_.line({m.x - k, m.y + k}, {m.x + k, m.y - k}, xc, 1.4f);
        }
    }
    m_->dl_.pop_clip();

    // "+" after the last tab, or pinned at the right end when the tabs scroll
    if (add_tab) {
        const f32  ax = overflow ? region.max.x + 2.0f : x + 2.0f;
        const rect ar = {{ax, row.min.y + 1.0f}, {ax + add_w - 2.0f, row.max.y - 3.0f}};
        const interaction in = interact(part_id(part::tab_add, current_seed()), ar);
        ev.add = in.pressed;
        anim_slot& a = anim_for(part_id(part::tab_add, current_seed()));
        a.hover = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        if (a.hover > 0.01f) {
            shape_style hover;
            hover.radius      = radii(m_->style_.rounding * 0.7f);
            hover.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f * a.hover);
            hover.fill_bottom = hover.fill_top;
            m_->dl_.shape(ar, hover);
        }
        const vec2  m  = ar.center();
        const color pc = lerp(m_->style_.text_dim, m_->style_.text, a.hover);
        m_->dl_.line({m.x - 5.0f, m.y}, {m.x + 5.0f, m.y}, pc, 1.5f);
        m_->dl_.line({m.x, m.y - 5.0f}, {m.x, m.y + 5.0f}, pc, 1.5f);
    }

    // a list of every tab: the way to a tab that has scrolled out of view
    if (overflow) {
        const rect lr = {{row.max.x - list_w, row.min.y + 1.0f}, {row.max.x, row.max.y - 3.0f}};
        const interaction in = interact(part_id(part::tab_list, current_seed()), lr);
        if (in.pressed) { toggle_popup("##tablist"); }
        anim_slot& a = anim_for(part_id(part::tab_list, current_seed()));
        a.hover  = approach(a.hover, in.hovered || popup_open("##tablist") ? 1.0f : 0.0f);
        if (a.hover > 0.01f) {
            shape_style hover;
            hover.radius      = radii(m_->style_.rounding * 0.7f);
            hover.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f * a.hover);
            hover.fill_bottom = hover.fill_top;
            m_->dl_.shape(lr, hover);
        }
        const vec2 m = lr.center();
        m_->dl_.triangle_filled({m.x - 4.0f, m.y - 2.0f}, {m.x + 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, lerp(m_->style_.text_dim, m_->style_.text, a.hover));
        if (auto p = popup("##tablist", 240.0f)) {
            const f32 pitch = m_->font_.line_height(f) + 8.0f + m_->style_.item_spacing; // a selectable row and the gap after it
            const f32 shown = std::min(static_cast<f32>(n), 9.0f) * pitch - m_->style_.item_spacing;
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

bool context::tab_strip(std::string_view id_label, const tab_desc* tabs, std::size_t count, int& selected,
                        font_id icon_font, f32 width, tab_strip_flags flags, f32 height)
{
    if (m_->cur_ == nullptr || count == 0) {
        return false;
    }
    push_id(id_label);

    const bool icons_only = flags == tab_strip_flags::icons_only;
    const font_id f       = current_font();
    const f32  lh         = m_->font_.line_height(f);
    const f32  row_h      = frame_height() + 10.0f;
    const f32  gap        = 3.0f;
    const std::size_t n   = std::min<std::size_t>(count, 16);
    selected = std::clamp(selected, 0, static_cast<int>(n) - 1);

    f32 w = width;
    if (w <= 0.0f) {
        if (icons_only) {
            w = row_h + 12.0f;
        } else {
            f32 widest = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const f32 iw = tabs[i].icon.empty() ? 0.0f : m_->font_.measure(icon_font, tabs[i].icon).x + 12.0f;
                widest = std::max(widest, iw + label_size(f, visible_label(tabs[i].label)).x);
            }
            w = std::max(widest + 44.0f, 120.0f);
        }
    }
    const f32 natural_h = static_cast<f32>(n) * (row_h + gap) + 16.0f;
    f32 h = height;
    if (h <= 0.0f) {
        h = m_->layout_.bound_bottom > 0.0f ? std::max(m_->layout_.bound_bottom - layout_next_y(), natural_h) : natural_h;
    }

    const rect r = layout_place({w, h});
    shape_style panel;
    panel.radius       = radii(m_->style_.rounding);
    panel.fill_top     = lighten(m_->style_.title_bg, m_->style_.gradient * 0.5f).scaled_alpha(0.85f);
    panel.fill_bottom  = darken(m_->style_.title_bg, 0.1f).scaled_alpha(0.85f);
    panel.border       = m_->style_.border;
    panel.border_width = m_->style_.border_width;
    m_->dl_.shape(r, panel);

    std::array<rect, 16> cells{};
    f32 y = r.min.y + 8.0f;
    for (std::size_t i = 0; i < n; ++i) {
        cells[i] = {{r.min.x + 6.0f, y}, {r.max.x - 6.0f, y + row_h}};
        y += row_h + gap;
    }

    bool changed = false;
    std::array<f32, 16> emphasis{};
    for (std::size_t i = 0; i < n; ++i) {
        const id key = widget_id(tabs[i].label);
        const interaction in = interact(key, cells[i]);
        if (in.pressed && static_cast<int>(i) != selected) {
            selected = static_cast<int>(i);
            changed  = true;
        }
        anim_slot& a = anim_for(key);
        a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        a.toggle = approach(a.toggle, static_cast<int>(i) == selected ? 1.0f : 0.0f);
        emphasis[i] = std::max(a.toggle, a.hover * 0.7f);

        if (a.hover > 0.01f && static_cast<int>(i) != selected) {
            shape_style hv;
            hv.radius      = radii(m_->style_.rounding * 0.7f);
            hv.fill_top    = m_->style_.widget_hover.scaled_alpha(0.55f * a.hover);
            hv.fill_bottom = hv.fill_top;
            m_->dl_.shape(cells[i], hv);
        }
        if (icons_only && in.hovered && m_->hover_time_ > 0.35f) {
            draw_tooltip(visible_label(tabs[i].label));
        }
    }

    const rect sel = cells[static_cast<std::size_t>(selected)];
    const f32  iy  = r.min.y + animate("strip_y", sel.min.y - r.min.y, 22.0f); // (relative to the strip: moving the window must not make it lag)
    shape_style pill;
    pill.radius      = radii(m_->style_.rounding * 0.7f);
    pill.fill_top    = m_->style_.accent.scaled_alpha(0.22f);
    pill.fill_bottom = m_->style_.accent.scaled_alpha(0.10f);
    pill.border      = m_->style_.accent.scaled_alpha(0.35f);
    pill.border_width = 1.0f;
    m_->dl_.shape({{sel.min.x, iy}, {sel.max.x, iy + row_h}}, pill);
    shape_style bar;
    bar.radius      = radii(1.5f);
    bar.fill_top    = m_->style_.accent_hover;
    bar.fill_bottom = m_->style_.accent;
    m_->dl_.shape({{sel.min.x + 1.0f, iy + 9.0f}, {sel.min.x + 4.0f, iy + row_h - 9.0f}}, bar);

    for (std::size_t i = 0; i < n; ++i) {
        const color c = lerp(m_->style_.text_dim, m_->style_.text, emphasis[i]);
        const bool has_icon = !tabs[i].icon.empty();
        const vec2 isize = has_icon ? m_->font_.measure(icon_font, tabs[i].icon) : vec2{};
        const color ic   = lerp(c, m_->style_.accent_hover, emphasis[i] * 0.7f);
        if (icons_only) {
            if (has_icon) {
                m_->dl_.text({cells[i].center().x - isize.x * 0.5f, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
            }
        } else {
            f32 x = cells[i].min.x + 16.0f;
            if (has_icon) {
                m_->dl_.text({x, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
                x += isize.x + 12.0f;
            }
            const std::string_view shown = visible_label(tabs[i].label);
            label_draw({x, cells[i].center().y - (shown.empty() ? lh : label_size(f, shown).y) * 0.5f}, c, shown, f);
        }
    }

    pop_id();
    return changed;
}

} // namespace strata

// tables: columns, headers, sorting, resizing, saved layouts

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "core/part_id.hpp"
#include "text_util.hpp"
#include "widget_util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>

namespace strata {

using namespace text;

table_scope::~table_scope()
{
    if (open_) {
        ctx_->end_table();
    }
}

context::table_state* context::table_for(id key) noexcept
{
    table_state* spare = nullptr;
    for (table_state& t : m_->tree_table_.tables_) {
        if (t.key == key) {
            return &t;
        }
        if (spare == nullptr && (t.key == 0 || t.last_frame + 2 < m_->frame_)) {
            spare = &t;
        }
    }
    if (spare == nullptr) {
        report_limit("tables with state (context_config::capacity.tables)", static_cast<u32>(m_->tree_table_.tables_.size()));
    }
    table_state* slot = spare != nullptr ? spare : &m_->tree_table_.tables_[0];
    *slot     = {};
    slot->key = key;
    return slot;
}

bool context::begin_table(std::string_view id_label, u32 columns, table_flags flags, f32 height)
{
    // nested tables are fine (the outer frame is stacked until end_table()). tables repeated per row need distinct
    // ids (push_id(row)) for separate widths and scroll
    if (m_->cur_ != nullptr && columns > max_table_columns) {
        report_limit("table columns (max_table_columns)", max_table_columns);
    }
    if (m_->cur_ != nullptr && m_->tree_table_.table_.active && m_->tree_table_.table_depth_ >= max_table_depth) {
        report_limit("tables inside tables (max_table_depth)", max_table_depth);
    }
    if (m_->cur_ == nullptr || columns == 0 || columns > max_table_columns || (m_->tree_table_.table_.active && m_->tree_table_.table_depth_ >= max_table_depth)) {
        return false;
    }
    const id key = widget_id(id_label);

    if (m_->tree_table_.table_.active) {
        m_->tree_table_.table_stack_[m_->tree_table_.table_depth_++] = m_->tree_table_.table_;
    }
    m_->tree_table_.table_ = {};
    m_->tree_table_.table_.active       = true;
    m_->tree_table_.table_.state        = table_for(key);
    m_->tree_table_.table_.flags        = flags;
    m_->tree_table_.table_.ncols        = columns;
    m_->tree_table_.table_.height_limit = height;

    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    const rect start = layout_place({w, 0.0f});
    m_->tree_table_.table_.origin = start.min;
    m_->tree_table_.table_.width  = std::max(w, 1.0f);
    m_->tree_table_.table_.outer  = m_->layout_;

    m_->tree_table_.table_.pad_x     = 8.0f;
    m_->tree_table_.table_.pad_y     = 3.0f;
    m_->tree_table_.table_.min_row_h = m_->font_.line_height(current_font()) + 2.0f * m_->tree_table_.table_.pad_y;
    m_->tree_table_.table_.row_y     = m_->tree_table_.table_.origin.y;

    table_state& st = *m_->tree_table_.table_.state;
    st.last_frame = m_->frame_;
    if (st.columns != columns) {
        st.columns   = columns;
        st.inited    = false;
        st.row_hint  = m_->tree_table_.table_.min_row_h;
        st.scroll    = 0.0f;
        st.scroll_wanted = -1.0f;
        st.content_h = 0.0f;
    }
    push_id(id_label);
    return true;
}

void context::table_setup_column(std::string_view label, f32 fixed_width, f32 stretch_weight, table_column_flags flags)
{
    if (!m_->tree_table_.table_.active || m_->tree_table_.table_.setup_count >= m_->tree_table_.table_.ncols) {
        return;
    }
    m_->tree_table_.table_.cols[m_->tree_table_.table_.setup_count++] = {label, fixed_width, stretch_weight, flags};
}

// column rects: hidden ones take no room, visible ones share the width by fraction
void context::table_recompute_x() noexcept
{
    const table_state& st = *m_->tree_table_.table_.state;
    f32 total = 0.0f;
    for (u32 c = 0; c < m_->tree_table_.table_.ncols; ++c) {
        if ((st.hidden & (1u << c)) == 0) { total += st.frac[c]; }
    }
    if (total <= 0.0f) { total = 1.0f; }
    m_->tree_table_.table_.frac_total = total;

    f32 x = m_->tree_table_.table_.origin.x;
    u32 nv = 0;
    for (u32 p = 0; p < m_->tree_table_.table_.ncols; ++p) {
        const u32 c = st.order[p];
        if ((st.hidden & (1u << c)) != 0) {
            m_->tree_table_.table_.x0[c] = m_->tree_table_.table_.x1[c] = x;
            continue;
        }
        m_->tree_table_.table_.col_x[nv] = x;
        m_->tree_table_.table_.vis[nv]   = static_cast<u8>(c);
        m_->tree_table_.table_.x0[c]     = x;
        x += st.frac[c] / total * m_->tree_table_.table_.width;
        m_->tree_table_.table_.x1[c]     = x;
        ++nv;
    }
    m_->tree_table_.table_.col_x[nv] = m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width;
    m_->tree_table_.table_.nvis      = nv;
}

// "order=2,0,1;hidden=1;widths=0.3,0.3,0.4" (indices and width fractions); false if it does not fit `ncols`
namespace {

[[nodiscard]] bool parse_table_layout(std::string_view text, u32 ncols, std::array<u8, 16>& order, u16& hidden, std::array<f32, 16>& frac)
{
    std::array<u8, 16>  o{};
    std::array<f32, 16> w{};
    u16 h = 0;
    bool have_order = false, have_widths = false;
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        std::string_view part = text.substr(0, semi);
        text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) { continue; }
        const std::string_view key = part.substr(0, eq);
        std::string_view       val = part.substr(eq + 1);
        u32 n = 0;
        while (!val.empty()) {
            const std::size_t comma = val.find(',');
            const std::string_view item = val.substr(0, comma);
            val = comma == std::string_view::npos ? std::string_view{} : val.substr(comma + 1);
            if (key == "widths") {
                f32 v{};
                if (n >= ncols || std::from_chars(item.data(), item.data() + item.size(), v).ec != std::errc{}) { return false; }
                w[n++] = v;
            } else {
                u32 v{};
                if (item.empty()) { continue; }
                if (std::from_chars(item.data(), item.data() + item.size(), v).ec != std::errc{} || v >= ncols) { return false; }
                if (key == "order") {
                    if (n >= ncols) { return false; }
                    o[n++] = static_cast<u8>(v);
                } else if (key == "hidden") {
                    h = static_cast<u16>(h | (1u << v));
                }
            }
        }
        if (key == "order")  { if (n != ncols) { return false; } have_order = true; }
        if (key == "widths") { if (n != ncols) { return false; } have_widths = true; }
    }
    if (have_order) { // a permutation
        u32 seen = 0;
        for (u32 i = 0; i < ncols; ++i) { seen |= 1u << o[i]; }
        if (seen != (1u << ncols) - 1u) { return false; }
        order = o;
    }
    if (have_widths) {
        f32 sum = 0.0f;
        for (u32 i = 0; i < ncols; ++i) { sum += w[i]; }
        if (sum <= 0.0f) { return false; }
        for (u32 i = 0; i < ncols; ++i) { frac[i] = w[i] / sum; }
    }
    hidden = h;
    return true;
}

} // namespace

void context::table_finalize_columns()
{
    if (m_->tree_table_.table_.columns_ready) {
        return;
    }
    m_->tree_table_.table_.columns_ready = true;
    table_state& st = *m_->tree_table_.table_.state;

    if (!st.inited) {
        f32 fixed   = 0.0f;
        f32 weights = 0.0f;
        for (u32 i = 0; i < m_->tree_table_.table_.ncols; ++i) {
            if (m_->tree_table_.table_.cols[i].fixed > 0.0f) { fixed += m_->tree_table_.table_.cols[i].fixed; }
            else                             { weights += std::max(m_->tree_table_.table_.cols[i].weight, 0.0001f); }
        }
        const f32 rest = std::max(m_->tree_table_.table_.width - fixed, 0.0f);
        f32 sum = 0.0f;
        for (u32 i = 0; i < m_->tree_table_.table_.ncols; ++i) {
            const f32 px = m_->tree_table_.table_.cols[i].fixed > 0.0f
                               ? m_->tree_table_.table_.cols[i].fixed
                               : (weights > 0.0f ? rest * std::max(m_->tree_table_.table_.cols[i].weight, 0.0001f) / weights : 0.0f);
            st.frac[i] = px / m_->tree_table_.table_.width;
            sum += st.frac[i];
        }
        for (u32 i = 0; i < m_->tree_table_.table_.ncols && sum > 0.0f; ++i) { st.frac[i] /= sum; }
        st.hidden = 0;
        for (u32 i = 0; i < m_->tree_table_.table_.ncols; ++i) {
            st.order[i] = static_cast<u8>(i);
            if ((static_cast<u8>(m_->tree_table_.table_.cols[i].flags) & static_cast<u8>(table_column_flags::default_hidden)) != 0) {
                st.hidden = static_cast<u16>(st.hidden | (1u << i));
            }
        }
        st.inited = true;
    }
    // a layout loaded with table_load_layout(): once
    for (auto it = m_->table_pending_.begin(); it != m_->table_pending_.end(); ++it) {
        if (it->first != st.key) { continue; }
        std::array<u8, 16>  order = st.order;
        std::array<f32, 16> frac  = st.frac;
        u16                 hidden = st.hidden;
        if (parse_table_layout(it->second, m_->tree_table_.table_.ncols, order, hidden, frac)) {
            st.order  = order;
            st.frac   = frac;
            st.hidden = hidden;
        }
        m_->table_pending_.erase(it);
        break;
    }
    const u16 all = static_cast<u16>((1u << m_->tree_table_.table_.ncols) - 1u);
    if ((st.hidden & all) == all) { st.hidden = 0; } // at least one column shows
    table_recompute_x();
}

int context::table_headers_row(int sort_column, bool ascending)
{
    if (!m_->tree_table_.table_.active || m_->tree_table_.table_.header_done || m_->tree_table_.table_.body_started) {
        return -1;
    }
    table_finalize_columns();
    table_state& st = *m_->tree_table_.table_.state;
    const font_id f = current_font();
    const f32 lh    = m_->font_.line_height(f);
    const f32 hh    = lh + 2.0f * m_->tree_table_.table_.pad_y + 2.0f;
    const f32 y0    = m_->tree_table_.table_.row_y;
    const rect header_area = {{m_->tree_table_.table_.origin.x, y0}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, y0 + hh}};
    const auto column_flag = [&](u32 c, table_column_flags fl) { return (static_cast<u8>(m_->tree_table_.table_.cols[c].flags) & static_cast<u8>(fl)) != 0; };

    // right-click: a menu of the columns to show
    if (has_flag(m_->tree_table_.table_.flags, table_flags::hideable)) {
        if (m_->input_.mouse_right_pressed_ && pointer_over(header_area)) {
            open_popup_menu("##columns", m_->input_.mouse_);
        }
        if (begin_popup_menu("##columns")) {
            for (u32 p = 0; p < m_->tree_table_.table_.ncols; ++p) {
                const u32 c = st.order[p];
                bool shown = (st.hidden & (1u << c)) == 0;
                const std::string_view name = m_->tree_table_.table_.cols[c].label.empty() ? std::string_view{"(unnamed)"} : visible_label(m_->tree_table_.table_.cols[c].label);
                menu_item_options o;
                o.keep_open = true;
                o.enabled   = !(shown && (m_->tree_table_.table_.nvis <= 1 || column_flag(c, table_column_flags::no_hide)));
                push_id(std::to_string(c));
                if (menu_item(name, shown, o)) {
                    st.hidden = static_cast<u16>(shown ? (st.hidden & ~(1u << c)) : (st.hidden | (1u << c)));
                    table_recompute_x();
                }
                pop_id();
            }
            end_popup_menu();
        }
    }

    // resize handles first: they win the mouse over the header cells beneath them
    if (has_flag(m_->tree_table_.table_.flags, table_flags::resizable)) {
        for (u32 k = 0; k + 1 < m_->tree_table_.table_.nvis; ++k) {
            const u32 a = m_->tree_table_.table_.vis[k];
            const u32 b = m_->tree_table_.table_.vis[k + 1];
            const rect grip = {{m_->tree_table_.table_.col_x[k + 1] - 4.0f, y0}, {m_->tree_table_.table_.col_x[k + 1] + 4.0f, y0 + hh}};
            const interaction in = interact(part_id(part::column_grip, hash_id({reinterpret_cast<const char*>(&a), sizeof(a)}, current_seed())), grip);
            if (in.held && m_->input_.mouse_delta_.x != 0.0f) {
                const f32 per_px   = m_->tree_table_.table_.frac_total / m_->tree_table_.table_.width; // what a pixel is worth in `frac` units
                const f32 min_frac = 36.0f * per_px;
                f32 lo = min_frac - st.frac[a];
                f32 hi = st.frac[b] - min_frac;
                if (lo > hi) { // the pair is already narrower than two minimums (a restored layout, a tiny table)
                    lo = -std::max(st.frac[a], 0.0f);
                    hi = std::max(st.frac[b], 0.0f);
                }
                const f32 dx = std::clamp(m_->input_.mouse_delta_.x * per_px, lo, hi);
                st.frac[a] += dx;
                st.frac[b] -= dx;
                table_recompute_x();
            }
            if (in.hovered || in.held) {
                m_->dl_.rect_filled({{m_->tree_table_.table_.col_x[k + 1] - 1.0f, y0 + 3.0f}, {m_->tree_table_.table_.col_x[k + 1] + 1.0f, y0 + hh - 3.0f}},
                                m_->style_.accent.scaled_alpha(in.held ? 0.9f : 0.6f));
            }
        }
    }

    // the header cells: press = sort, drag = move the column
    struct header_hit { id key{}; bool pressed{}; };
    std::array<header_hit, 16> hits{};
    const u8 was_dragging = st.drag_col1;
    if (!m_->input_.mouse_down_) { st.drag_col1 = 0; st.press_col1 = 0; }
    for (u32 k = 0; k < m_->tree_table_.table_.nvis; ++k) {
        const u32  c    = m_->tree_table_.table_.vis[k];
        const rect cell = {{m_->tree_table_.table_.col_x[k], y0}, {m_->tree_table_.table_.col_x[k + 1], y0 + hh}};
        const id   key  = hash_id(m_->tree_table_.table_.cols[c].label, hash_id({reinterpret_cast<const char*>(&c), sizeof(c)}, current_seed()));
        const interaction in = interact(key, {{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}});
        hits[c] = {key, in.pressed && was_dragging != c + 1};
        anim_slot* a = anim_find(key);
        if (a == nullptr && in.hovered) { a = &anim_for(key); }
        if (a != nullptr) {
            a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f);
            a->last_frame = (a->hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
        }
        if (has_flag(m_->tree_table_.table_.flags, table_flags::reorderable) && !column_flag(c, table_column_flags::no_reorder)) {
            if (m_->active_ == key && m_->input_.mouse_pressed_) {
                st.press_col1 = static_cast<u8>(c + 1);
                st.press_x    = m_->input_.mouse_.x;
            }
            if (st.drag_col1 == 0 && st.press_col1 == c + 1 && m_->active_ == key && m_->input_.mouse_down_ && std::abs(m_->input_.mouse_.x - st.press_x) > 5.0f) {
                st.drag_col1 = static_cast<u8>(c + 1);
            }
        }
    }
    if (st.drag_col1 != 0 && m_->input_.mouse_down_) { // the dragged column takes the place of the one its middle has passed
        const u32 c = st.drag_col1 - 1u;
        u32 slot = 0, target = 0;
        for (u32 k = 0; k < m_->tree_table_.table_.nvis; ++k) {
            if (m_->tree_table_.table_.vis[k] == c) { slot = k; }
            else if ((m_->tree_table_.table_.col_x[k] + m_->tree_table_.table_.col_x[k + 1]) * 0.5f < m_->input_.mouse_.x) { ++target; }
        }
        if (target != slot) {
            const u32 d = m_->tree_table_.table_.vis[target];
            u32 from = 0, to = 0;
            for (u32 p = 0; p < m_->tree_table_.table_.ncols; ++p) {
                if (st.order[p] == c) { from = p; }
                if (st.order[p] == d) { to = p; }
            }
            bool blocked = false; // a pinned column in between cannot be passed
            for (u32 p = std::min(from, to); p <= std::max(from, to); ++p) {
                if (p != from && column_flag(st.order[p], table_column_flags::no_reorder)) { blocked = true; }
            }
            if (!blocked) {
                const u8 moved = st.order[from];
                if (from < to) { for (u32 p = from; p < to; ++p) { st.order[p] = st.order[p + 1]; } }
                else           { for (u32 p = from; p > to; --p) { st.order[p] = st.order[p - 1]; } }
                st.order[to] = moved;
                table_recompute_x();
            }
        }
    }

    shape_style bg;
    bg.radius      = radii(m_->style_.rounding * 0.6f, corners::top);
    bg.fill_top    = lighten(m_->style_.title_bg, m_->style_.gradient * 0.8f);
    bg.fill_bottom = m_->style_.title_bg;
    m_->dl_.shape({{m_->tree_table_.table_.origin.x, y0}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, y0 + hh}}, bg);

    int clicked = -1;
    for (u32 k = 0; k < m_->tree_table_.table_.nvis; ++k) {
        const u32  c    = m_->tree_table_.table_.vis[k];
        const rect cell = {{m_->tree_table_.table_.col_x[k], y0}, {m_->tree_table_.table_.col_x[k + 1], y0 + hh}};
        if (hits[c].pressed) { clicked = static_cast<int>(c); }

        const anim_slot* a = anim_find(hits[c].key);
        const f32 hover = a != nullptr ? a->hover : 0.0f;
        if (hover > 0.01f) {
            m_->dl_.rect_filled(cell, m_->style_.widget_hover.scaled_alpha(0.5f * hover));
        }
        if (st.drag_col1 == c + 1 && m_->input_.mouse_down_) {
            m_->dl_.rect_filled(cell, m_->style_.accent.scaled_alpha(0.22f));
        }

        const std::string_view label = visible_label(m_->tree_table_.table_.cols[c].label);
        const bool sorted = static_cast<int>(c) == sort_column;
        const f32 text_max = cell.width() - 2.0f * m_->tree_table_.table_.pad_x - (sorted ? 14.0f : 0.0f);
        if (m_->rich_.rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
            const vec2 ts = label_size(f, label);
            m_->dl_.push_clip({{cell.min.x + m_->tree_table_.table_.pad_x, cell.min.y}, {cell.min.x + m_->tree_table_.table_.pad_x + std::max(text_max, 0.0f), cell.max.y}});
            label_draw({cell.min.x + m_->tree_table_.table_.pad_x, cell.min.y + (hh - ts.y) * 0.5f}, m_->style_.text, label, f);
            m_->dl_.pop_clip();
        } else {
            std::string cut;
            std::string_view shown = label;
            if (m_->font_.measure(f, label).x > text_max) {
                std::string_view rest = label;
                f32 used = 0.0f;
                while (!rest.empty()) {
                    const std::string_view before = rest;
                    const char32_t cp = decode_utf8(rest);
                    const f32 adv = m_->font_.advance(f, cp);
                    if (used + adv > text_max - 10.0f) { break; }
                    used += adv;
                    cut.append(before.substr(0, before.size() - rest.size()));
                }
                cut += "..";
                shown = cut;
            }
            m_->dl_.text({cell.min.x + m_->tree_table_.table_.pad_x, cell.min.y + (hh - lh) * 0.5f}, m_->style_.text, shown, f);
        }

        if (sorted) {
            const vec2 sc{cell.max.x - m_->tree_table_.table_.pad_x - 3.0f, cell.center().y};
            if (ascending) {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y + 2.5f}, {sc.x, sc.y - 3.0f}, {sc.x + 4.0f, sc.y + 2.5f}, m_->style_.accent_hover);
            } else {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y - 2.5f}, {sc.x + 4.0f, sc.y - 2.5f}, {sc.x, sc.y + 3.0f}, m_->style_.accent_hover);
            }
        }
    }

    m_->dl_.rect_filled({{m_->tree_table_.table_.origin.x, y0 + hh - 1.0f}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, y0 + hh}}, m_->style_.border);
    m_->tree_table_.table_.row_y       = y0 + hh;
    m_->tree_table_.table_.header_done = true;
    return clicked;
}

void context::table_start_body()
{
    if (m_->tree_table_.table_.body_started) {
        return;
    }
    m_->tree_table_.table_.body_started = true;
    m_->tree_table_.table_.body_top     = m_->tree_table_.table_.row_y;

    if (m_->tree_table_.table_.height_limit > 0.0f) {
        table_state& st = *m_->tree_table_.table_.state;
        m_->tree_table_.table_.scroll_mode = true;
        m_->tree_table_.table_.body_h = std::max(m_->tree_table_.table_.origin.y + m_->tree_table_.table_.height_limit - m_->tree_table_.table_.body_top, m_->tree_table_.table_.min_row_h);
        const rect region = {{m_->tree_table_.table_.origin.x, m_->tree_table_.table_.body_top}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, m_->tree_table_.table_.body_top + m_->tree_table_.table_.body_h}};

        if (st.scroll_wanted >= 0.0f) {
            st.scroll         = st.scroll_wanted;
            st.scroll_wanted  = -1.0f;
        }
        st.scroll = std::clamp(st.scroll, 0.0f, std::max(0.0f, st.content_h - m_->tree_table_.table_.body_h));

        m_->dl_.push_clip(region);
        m_->tree_table_.table_.clip_pushed = true;
        m_->tree_table_.table_.row_y       = m_->tree_table_.table_.body_top - st.scroll;
    }
}

void context::table_finish_row()
{
    table_state& st = *m_->tree_table_.table_.state;
    if (m_->tree_table_.table_.cell_clip) { // cell content ends here; the row lines below are not clipped to the cell
        m_->dl_.pop_clip();
        m_->tree_table_.table_.cell_clip = false;
    }
    if (m_->tree_table_.table_.col >= 0 && !m_->layout_.first) {
        m_->tree_table_.table_.row_bottom    = std::max(m_->tree_table_.table_.row_bottom, m_->layout_.bottom + m_->tree_table_.table_.pad_y);
        m_->tree_table_.table_.row_had_content = true;
    }

    const f32 height = m_->tree_table_.table_.row_had_content ? std::max(m_->tree_table_.table_.row_bottom - m_->tree_table_.table_.row_top, m_->tree_table_.table_.min_row_h)
                                              : std::max(st.row_hint, m_->tree_table_.table_.min_row_h);
    if (m_->tree_table_.table_.row_visible && has_flag(m_->tree_table_.table_.flags, table_flags::borders)) {
        m_->dl_.rect_filled({{m_->tree_table_.table_.origin.x, m_->tree_table_.table_.row_top + height - 1.0f}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, m_->tree_table_.table_.row_top + height}},
                        m_->style_.border.scaled_alpha(0.45f));
    }
    if (m_->tree_table_.table_.row_had_content) {
        st.row_hint = height;
    }
    m_->tree_table_.table_.row_y  = m_->tree_table_.table_.row_top + height;
    m_->tree_table_.table_.in_row = false;
    m_->tree_table_.table_.col    = -1;
}

bool context::table_next_row()
{
    if (!m_->tree_table_.table_.active) {
        return false;
    }
    table_finalize_columns();
    if (m_->tree_table_.table_.in_row) {
        table_finish_row();
    }
    table_start_body();

    table_state& st = *m_->tree_table_.table_.state;
    m_->tree_table_.table_.in_row          = true;
    m_->tree_table_.table_.row_top         = m_->tree_table_.table_.row_y;
    m_->tree_table_.table_.row_bottom      = m_->tree_table_.table_.row_top;
    m_->tree_table_.table_.row_had_content = false;
    m_->tree_table_.table_.col             = -1;
    ++m_->tree_table_.table_.row_index;

    // background is drawn now using the previous row's height, before the content goes on top; the same height
    // is seeded into each cell's layout so this row's content can center against it too (table_next_column)
    const f32  hint = std::max(st.row_hint, m_->tree_table_.table_.min_row_h);
    m_->tree_table_.table_.row_hint = hint;
    const rect row  = {{m_->tree_table_.table_.origin.x, m_->tree_table_.table_.row_top}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, m_->tree_table_.table_.row_top + hint}};
    m_->tree_table_.table_.row_visible = m_->dl_.clip().overlaps(row);
    if (m_->tree_table_.table_.row_visible) {
        if (has_flag(m_->tree_table_.table_.flags, table_flags::striped) && (m_->tree_table_.table_.row_index & 1) == 0) {
            m_->dl_.rect_filled(row, m_->style_.widget_bg.scaled_alpha(0.3f));
        }
        if (has_flag(m_->tree_table_.table_.flags, table_flags::row_hover) && pointer_over(row)) {
            m_->dl_.rect_filled(row, m_->style_.accent.scaled_alpha(0.11f));
        }
    }
    return m_->tree_table_.table_.row_visible;
}

bool context::table_next_column()
{
    if (!m_->tree_table_.table_.active) {
        return false;
    }
    if (!m_->tree_table_.table_.in_row) {
        (void)table_next_row();
    }
    if (m_->tree_table_.table_.cell_clip) {
        m_->dl_.pop_clip();
        m_->tree_table_.table_.cell_clip = false;
    }
    if (m_->tree_table_.table_.col >= 0 && !m_->layout_.first) {
        m_->tree_table_.table_.row_bottom      = std::max(m_->tree_table_.table_.row_bottom, m_->layout_.bottom + m_->tree_table_.table_.pad_y);
        m_->tree_table_.table_.row_had_content = true;
    }
    if (m_->tree_table_.table_.col + 1 >= static_cast<int>(m_->tree_table_.table_.ncols)) {
        (void)table_next_row(); // running past the last column wraps to the next row
    }

    ++m_->tree_table_.table_.col;
    const auto c = static_cast<u32>(m_->tree_table_.table_.col);
    m_->layout_             = {};
    m_->layout_.origin      = {m_->tree_table_.table_.x0[c] + m_->tree_table_.table_.pad_x, m_->tree_table_.table_.row_top + m_->tree_table_.table_.pad_y};
    m_->layout_.width       = std::max(m_->tree_table_.table_.x1[c] - m_->tree_table_.table_.x0[c] - 2.0f * m_->tree_table_.table_.pad_x, 1.0f);
    // a plain-text cell next to a taller one (a framed input, a toggle, ...) centers against the row's known
    // height instead of sitting flush at its top
    m_->layout_.line_h_seed = std::max(0.0f, m_->tree_table_.table_.row_hint - 2.0f * m_->tree_table_.table_.pad_y);

    // cell content never spills into the next column (hidden columns show nothing)
    const rect outer_clip = m_->dl_.clip();
    m_->dl_.push_clip({{m_->tree_table_.table_.x0[c], outer_clip.min.y}, {m_->tree_table_.table_.x1[c], outer_clip.max.y}});
    m_->tree_table_.table_.cell_clip = true;
    return m_->tree_table_.table_.x1[c] > m_->tree_table_.table_.x0[c];
}

void context::end_table()
{
    if (!m_->tree_table_.table_.active) {
        return;
    }
    table_finalize_columns();
    if (m_->tree_table_.table_.in_row) {
        table_finish_row();
    }
    table_start_body();
    table_state& st = *m_->tree_table_.table_.state;

    f32 total_h;
    if (m_->tree_table_.table_.scroll_mode) {
        st.content_h = m_->tree_table_.table_.row_y - (m_->tree_table_.table_.body_top - st.scroll);
        if (m_->tree_table_.table_.clip_pushed) {
            m_->dl_.pop_clip();
        }
        total_h = m_->tree_table_.table_.body_top - m_->tree_table_.table_.origin.y + m_->tree_table_.table_.body_h;

        // the wheel goes to the innermost scroller under the pointer; nested tables already had their turn
        const rect region = {{m_->tree_table_.table_.origin.x, m_->tree_table_.table_.body_top}, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, m_->tree_table_.table_.body_top + m_->tree_table_.table_.body_h}};
        if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(region)) {
            st.scroll -= wheel_scroll(st.row_hint, m_->tree_table_.table_.body_h);
            st.scroll_wanted    = -1.0f; // the user's scrolling wins over a table_set_scroll_y() still waiting
            m_->wheel_consumed_ = true;
        }

        const f32 max_scroll = std::max(0.0f, st.content_h - m_->tree_table_.table_.body_h);
        st.scroll = std::clamp(st.scroll, 0.0f, max_scroll);
        if (max_scroll > 0.0f) {
            const f32  track_top = m_->tree_table_.table_.body_top + 2.0f;
            const f32  track_h = m_->tree_table_.table_.body_h - 4.0f;
            const f32  thumb_h = std::max(20.0f, track_h * m_->tree_table_.table_.body_h / st.content_h);
            const f32  x1      = m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width - 3.0f;
            f32 thumb_y = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const interaction in = interact(part_id(part::table_scrollbar, current_seed()), rect{{x1 - 7.0f, thumb_y}, {x1 + 2.0f, thumb_y + thumb_h}});
            st.scroll = thumb_drag(in, st.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, st.scroll);
            if (in.held) { st.scroll_wanted = -1.0f; }
            thumb_y   = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const rect thumb = {{x1 - 5.0f, thumb_y}, {x1, thumb_y + thumb_h}};
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.8f : 0.45f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape(thumb, bar);
        }
    } else {
        total_h = m_->tree_table_.table_.row_y - m_->tree_table_.table_.origin.y;
    }

    const rect bounds = {m_->tree_table_.table_.origin, {m_->tree_table_.table_.origin.x + m_->tree_table_.table_.width, m_->tree_table_.table_.origin.y + total_h}};
    if (has_flag(m_->tree_table_.table_.flags, table_flags::borders)) {
        for (u32 i = 1; i < m_->tree_table_.table_.nvis; ++i) {
            m_->dl_.rect_filled({{std::round(m_->tree_table_.table_.col_x[i]), bounds.min.y}, {std::round(m_->tree_table_.table_.col_x[i]) + 1.0f, bounds.max.y}},
                            m_->style_.border.scaled_alpha(0.55f));
        }
        shape_style edge;
        edge.radius       = radii(m_->style_.rounding * 0.6f);
        edge.border       = m_->style_.border;
        edge.border_width = m_->style_.border_width;
        m_->dl_.shape(bounds, edge);
    }

    m_->layout_            = m_->tree_table_.table_.outer;
    m_->layout_.line_h     = total_h;
    m_->layout_.bottom     = std::max(m_->layout_.bottom, m_->tree_table_.table_.origin.y + total_h);
    m_->layout_.same_line  = false;
    m_->layout_.first      = false;
    pop_id();
    m_->tree_table_.table_ = {};
    if (m_->tree_table_.table_depth_ > 0) { // back to the table this one was nested in
        m_->tree_table_.table_ = m_->tree_table_.table_stack_[--m_->tree_table_.table_depth_];
    }
}

void context::table_skip_rows(int count)
{
    if (!m_->tree_table_.table_.active || count <= 0) {
        return;
    }
    table_finalize_columns();
    if (m_->tree_table_.table_.in_row) {
        table_finish_row();
    }
    table_start_body();
    m_->tree_table_.table_.row_y     += static_cast<f32>(count) * std::max(m_->tree_table_.table_.state->row_hint, m_->tree_table_.table_.min_row_h);
    m_->tree_table_.table_.row_index += static_cast<u32>(count);
}

f32 context::table_scroll_y() const noexcept
{
    return m_->tree_table_.table_.active && m_->tree_table_.table_.scroll_mode ? m_->tree_table_.table_.state->scroll : 0.0f;
}

f32 context::table_scroll_max_y() const noexcept
{
    if (!m_->tree_table_.table_.active || !m_->tree_table_.table_.scroll_mode) {
        return 0.0f;
    }
    return std::max(0.0f, m_->tree_table_.table_.state->content_h - m_->tree_table_.table_.body_h);
}

void context::table_set_scroll_y(f32 y) noexcept
{
    // not st.scroll itself: end_table measures this frame's content from the offset the rows were laid out with
    if (m_->tree_table_.table_.active && m_->tree_table_.table_.scroll_mode) {
        m_->tree_table_.table_.state->scroll_wanted = std::max(0.0f, y);
    }
}

std::string context::table_save_layout(std::string_view id_label) const
{
    const id key = hash_id(id_label, current_seed()); // a query, not a submission: nothing to record
    for (const table_state& t : m_->tree_table_.tables_) {
        if (t.key != key || !t.inited) { continue; }
        return table_layout_text(t);
    }
    return {};
}

std::string context::table_layout_text(const table_state& t)
{
    std::string out = "order=";
    for (u32 p = 0; p < t.columns; ++p) { out += (p ? "," : "") + std::to_string(t.order[p]); }
    out += ";hidden=";
    bool first = true;
    for (u32 c = 0; c < t.columns; ++c) {
        if ((t.hidden & (1u << c)) != 0) { out += (first ? "" : ",") + std::to_string(c); first = false; }
    }
    out += ";widths=";
    for (u32 c = 0; c < t.columns; ++c) { out += (c ? "," : "") + std::format("{:.4f}", t.frac[c]); }
    return out;
}

void context::table_load_layout(std::string_view id_label, std::string_view text)
{
    const id key = widget_id(id_label);
    for (auto& p : m_->table_pending_) {
        if (p.first == key) {
            p.second.assign(text);
            return;
        }
    }
    m_->table_pending_.emplace_back(key, std::string{text});
}

bool context::table_tree_node(std::string_view label, tree_flags flags)
{
    if (m_->cur_ == nullptr || !m_->tree_table_.table_.active || m_->tree_table_.table_.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_.rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(m_->tree_table_.table_.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place({m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - m_->tree_table_.table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m_->tree_table_.table_.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(key, hit);

    bool& open = tree_open_state(key, has_flag(flags, tree_flags::default_open));
    const bool arrow_hit = m_->input_.mouse_.x < row.min.x + 16.0f;
    m_->tree_table_.item_pressed_ = in.pressed;
    if (in.pressed && (!has_flag(flags, tree_flags::arrow_only) || arrow_hit)) {
        open = !open;
    }
    const bool is_open = open;

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, is_open ? 1.0f : 0.0f, m_->style_.anim_speed * 0.9f);

    const bool selected = has_flag(flags, tree_flags::selected);
    if (selected || a.hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * a.hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * a.hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape(hit, bg);
    }

    const vec2 c{row.min.x + 8.0f, row.center().y};
    const f32  ang = a.toggle * std::numbers::pi_v<f32> * 0.5f;
    const f32  cs = std::cos(ang);
    const f32  sn = std::sin(ang);
    const auto rot = [&](vec2 p) { return vec2{c.x + p.x * cs - p.y * sn, c.y + p.x * sn + p.y * cs}; };
    m_->dl_.triangle_filled(rot({-2.5f, -4.0f}), rot({4.0f, 0.0f}), rot({-2.5f, 4.0f}),
                        lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle)));
    const vec2 tsize = label_size(f, shown);
    label_draw({row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f},
               selected ? m_->style_.accent_hover : m_->style_.text, shown, f);

    if (!is_open) {
        return false;
    }
    ++m_->tree_table_.table_.tree_depth;
    push_id(label);
    return true;
}

bool context::table_tree_leaf(std::string_view label, bool selected)
{
    if (m_->cur_ == nullptr || !m_->tree_table_.table_.active || m_->tree_table_.table_.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_.rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(m_->tree_table_.table_.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place({m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - m_->tree_table_.table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m_->tree_table_.table_.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(key, hit);
    m_->tree_table_.item_pressed_ = in.pressed;

    anim_slot* a = anim_find(key);
    if (a == nullptr && in.hovered) { a = &anim_for(key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f);
        hover    = a->hover;
        a->last_frame = (hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
    }
    if (selected || hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape(hit, bg);
    }
    const vec2 tsize = label_size(f, shown);
    label_draw({row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f}, selected ? m_->style_.accent_hover : m_->style_.text, shown, f);
    return in.pressed;
}

void context::table_tree_pop()
{
    if (!m_->tree_table_.table_.active || m_->tree_table_.table_.tree_depth == 0) {
        return;
    }
    --m_->tree_table_.table_.tree_depth;
    pop_id();
}

} // namespace strata

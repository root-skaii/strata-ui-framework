// tables: columns, headers, sorting, resizing, saved layouts

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "context_helpers.hpp"
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
using namespace internal;

[[nodiscard]] static table_state* table_for(context& ui, id key) noexcept;
static void table_recompute_x(context& ui) noexcept;

table_scope::~table_scope()
{
    if (open_) {
        ctx_->end_table();
    }
}

static table_state* table_for(context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    table_state* spare = nullptr;
    for (table_state& t : m.tree_table_.tables_) {
        if (t.key == key) {
            return &t;
        }
        if (spare == nullptr && (t.key == 0 || t.last_frame + 2 < m.frame_)) {
            spare = &t;
        }
    }
    if (spare == nullptr) {
        report_limit(ui, "tables with state (context_config::capacity.tables)", static_cast<u32>(m.tree_table_.tables_.size()));
    }
    table_state* slot = spare != nullptr ? spare : &m.tree_table_.tables_[0];
    *slot     = {};
    slot->key = key;
    return slot;
}

bool context::begin_table(std::string_view id_label, u32 columns, table_flags flags, f32 height)
{
    auto& tbl = m_->tree_table_.table_;
    // nested tables are fine (the outer frame is stacked until end_table()). tables repeated per row need distinct
    // ids (push_id(row)) for separate widths and scroll
    if (m_->cur_ != nullptr && columns > max_table_columns) {
        report_limit(*this, "table columns (max_table_columns)", max_table_columns);
    }
    if (m_->cur_ != nullptr && tbl.active && m_->tree_table_.table_depth_ >= max_table_depth) {
        report_limit(*this, "tables inside tables (max_table_depth)", max_table_depth);
    }
    if (m_->cur_ == nullptr || columns == 0 || columns > max_table_columns || (tbl.active && m_->tree_table_.table_depth_ >= max_table_depth)) {
        return false;
    }
    const id key = widget_id(*this, id_label);

    if (tbl.active) {
        m_->tree_table_.table_stack_[m_->tree_table_.table_depth_++] = tbl;
    }
    tbl = {};
    tbl.active       = true;
    tbl.state        = table_for(*this, key);
    tbl.flags        = flags;
    tbl.ncols        = columns;
    tbl.height_limit = height;

    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    const rect start = layout_place(*this, {w, 0.0f});
    tbl.origin = start.min;
    tbl.width  = std::max(w, 1.0f);
    tbl.outer  = m_->layout_;

    tbl.pad_x     = 8.0f;
    tbl.pad_y     = 3.0f;
    tbl.min_row_h = m_->font_.line_height(current_font()) + 2.0f * tbl.pad_y;
    tbl.row_y     = tbl.origin.y;

    table_state& st = *tbl.state;
    st.last_frame = m_->frame_;
    if (st.columns != columns) {
        st.columns   = columns;
        st.inited    = false;
        st.row_hint  = tbl.min_row_h;
        st.scroll    = 0.0f;
        st.scroll_wanted = -1.0f;
        st.content_h = 0.0f;
    }
    push_id(id_label);
    return true;
}

void context::table_setup_column(std::string_view label, f32 fixed_width, f32 stretch_weight, table_column_flags flags)
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active || tbl.setup_count >= tbl.ncols) {
        return;
    }
    tbl.cols[tbl.setup_count++] = {label, fixed_width, stretch_weight, flags};
}

// column rects: hidden ones take no room, visible ones share the width by fraction
static void table_recompute_x(context& ui) noexcept
{
    auto& m = context_access::of(ui);
    auto& tbl = m.tree_table_.table_;
    const table_state& st = *tbl.state;
    f32 total = 0.0f;
    for (u32 c = 0; c < tbl.ncols; ++c) {
        if ((st.hidden & (1u << c)) == 0) { total += st.frac[c]; }
    }
    if (total <= 0.0f) { total = 1.0f; }
    tbl.frac_total = total;

    const bool sideways = has_flag(tbl.flags, table_flags::scroll_x);
    const f32  span     = sideways ? std::max(st.content_w, tbl.width) : tbl.width;
    tbl.span = span;
    const f32 x_start = tbl.origin.x - (sideways ? std::clamp(st.scroll_x, 0.0f, span - tbl.width) : 0.0f);
    f32 x = x_start;
    u32 nv = 0;
    for (u32 p = 0; p < tbl.ncols; ++p) {
        const u32 c = st.order[p];
        if ((st.hidden & (1u << c)) != 0) {
            tbl.x0[c] = tbl.x1[c] = x;
            continue;
        }
        tbl.col_x[nv] = x;
        tbl.vis[nv]   = static_cast<u8>(c);
        tbl.x0[c]     = x;
        x += st.frac[c] / total * span;
        tbl.x1[c]     = x;
        ++nv;
    }
    tbl.col_x[nv] = x_start + span;
    tbl.nvis      = nv;
}

// "order=2,0,1;hidden=1;widths=0.3,0.3,0.4" (indices and width fractions); false if it does not fit `ncols`
namespace {

[[nodiscard]] bool parse_table_layout(std::string_view text, u32 ncols, std::array<u8, 16>& order, u16& hidden, std::array<f32, 16>& frac, f32& span)
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
        if (key == "span") { // one pixel width (scroll_x tables)
            f32 v{};
            if (std::from_chars(val.data(), val.data() + val.size(), v).ec != std::errc{} || v <= 0.0f) { return false; }
            span = v;
            continue;
        }
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

void internal::table_finalize_columns(context& ui)
{
    auto& m = context_access::of(ui);
    auto& tbl = m.tree_table_.table_;
    if (tbl.columns_ready) {
        return;
    }
    tbl.columns_ready = true;
    table_state& st = *tbl.state;

    if (!st.inited) {
        f32 fixed   = 0.0f;
        f32 weights = 0.0f;
        for (u32 i = 0; i < tbl.ncols; ++i) {
            if (tbl.cols[i].fixed > 0.0f) { fixed += tbl.cols[i].fixed; }
            else                             { weights += std::max(tbl.cols[i].weight, 0.0001f); }
        }
        f32 stretch = 0.0f; // stretch columns, for the minimum width of a sideways-scrolling table
        for (u32 i = 0; i < tbl.ncols; ++i) { stretch += tbl.cols[i].fixed > 0.0f ? 0.0f : 1.0f; }
        const f32 span = has_flag(tbl.flags, table_flags::scroll_x)
                             ? std::max(tbl.width, fixed + 80.0f * stretch)
                             : tbl.width;
        if (has_flag(tbl.flags, table_flags::scroll_x)) { st.content_w = span; }
        const f32 rest = std::max(span - fixed, 0.0f);
        f32 sum = 0.0f;
        for (u32 i = 0; i < tbl.ncols; ++i) {
            const f32 px = tbl.cols[i].fixed > 0.0f
                               ? tbl.cols[i].fixed
                               : (weights > 0.0f ? rest * std::max(tbl.cols[i].weight, 0.0001f) / weights : 0.0f);
            st.frac[i] = px / span;
            sum += st.frac[i];
        }
        for (u32 i = 0; i < tbl.ncols && sum > 0.0f; ++i) { st.frac[i] /= sum; }
        st.hidden = 0;
        for (u32 i = 0; i < tbl.ncols; ++i) {
            st.order[i] = static_cast<u8>(i);
            if ((static_cast<u8>(tbl.cols[i].flags) & static_cast<u8>(table_column_flags::default_hidden)) != 0) {
                st.hidden = static_cast<u16>(st.hidden | (1u << i));
            }
        }
        st.inited = true;
    }
    // a layout loaded with table_load_layout(): once
    for (auto it = m.table_pending_.begin(); it != m.table_pending_.end(); ++it) {
        if (it->first != st.key) { continue; }
        std::array<u8, 16>  order = st.order;
        std::array<f32, 16> frac  = st.frac;
        u16                 hidden = st.hidden;
        f32                 span   = 0.0f;
        if (parse_table_layout(it->second, tbl.ncols, order, hidden, frac, span)) {
            if (span > 0.0f) { st.content_w = span; }
            st.order  = order;
            st.frac   = frac;
            st.hidden = hidden;
        }
        m.table_pending_.erase(it);
        break;
    }
    const u16 all = static_cast<u16>((1u << tbl.ncols) - 1u);
    if ((st.hidden & all) == all) { st.hidden = 0; } // at least one column shows
    table_recompute_x(ui);
}

int context::table_headers_row(int sort_column, bool ascending)
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active || tbl.header_done || tbl.body_started) {
        return -1;
    }
    table_finalize_columns(*this);
    table_state& st = *tbl.state;
    const font_id f = current_font();
    const f32 lh    = m_->font_.line_height(f);
    const f32 hh    = lh + 2.0f * tbl.pad_y + 2.0f;
    const f32 y0    = tbl.row_y;
    const rect header_area = {{tbl.origin.x, y0}, {tbl.origin.x + tbl.width, y0 + hh}};
    const auto column_flag = [&](u32 c, table_column_flags fl) { return (static_cast<u8>(tbl.cols[c].flags) & static_cast<u8>(fl)) != 0; };

    // right-click: a menu of the columns to show
    if (has_flag(tbl.flags, table_flags::hideable)) {
        if (m_->input_.mouse_right_pressed_ && pointer_over(*this, header_area)) {
            open_popup_menu("##columns", m_->input_.mouse_);
        }
        if (begin_popup_menu("##columns")) {
            for (u32 p = 0; p < tbl.ncols; ++p) {
                const u32 c = st.order[p];
                bool shown = (st.hidden & (1u << c)) == 0;
                const std::string_view name = tbl.cols[c].label.empty() ? std::string_view{"(unnamed)"} : visible_label(tbl.cols[c].label);
                menu_item_options o;
                o.keep_open = true;
                o.enabled   = !(shown && (tbl.nvis <= 1 || column_flag(c, table_column_flags::no_hide)));
                push_id(std::to_string(c));
                if (menu_item(name, shown, o)) {
                    st.hidden = static_cast<u16>(shown ? (st.hidden & ~(1u << c)) : (st.hidden | (1u << c)));
                    table_recompute_x(*this);
                }
                pop_id();
            }
            end_popup_menu();
        }
    }

    // columns scrolled out of the table do not draw or take the pointer outside it
    const bool sideways = has_flag(tbl.flags, table_flags::scroll_x);
    if (sideways) { m_->dl_.push_clip(header_area); }

    // resize handles first: they win the mouse over the header cells beneath them
    if (has_flag(tbl.flags, table_flags::resizable)) {
        for (u32 k = 0; k + 1 < tbl.nvis; ++k) {
            const u32 a = tbl.vis[k];
            const u32 b = tbl.vis[k + 1];
            const rect grip = {{tbl.col_x[k + 1] - 4.0f, y0}, {tbl.col_x[k + 1] + 4.0f, y0 + hh}};
            const interaction in = interact(*this, part_id(part::column_grip, hash_id({reinterpret_cast<const char*>(&a), sizeof(a)}, current_seed(*this))), grip);
            if (in.held && m_->input_.mouse_delta_.x != 0.0f) {
                const f32 per_px   = tbl.frac_total / tbl.width; // what a pixel is worth in `frac` units
                const f32 min_frac = 36.0f * per_px;
                if (has_flag(tbl.flags, table_flags::scroll_x)) { // the neighbour keeps its width, the table grows
                    const f32 dx = std::max(m_->input_.mouse_delta_.x * per_px, min_frac - st.frac[a]);
                    st.frac[a] += dx;
                    st.content_w = std::max(tbl.span + dx / per_px, tbl.width);
                } else {
                    f32 lo = min_frac - st.frac[a];
                    f32 hi = st.frac[b] - min_frac;
                    if (lo > hi) { // the pair is already narrower than two minimums (a restored layout, a tiny table)
                        lo = -std::max(st.frac[a], 0.0f);
                        hi = std::max(st.frac[b], 0.0f);
                    }
                    const f32 dx = std::clamp(m_->input_.mouse_delta_.x * per_px, lo, hi);
                    st.frac[a] += dx;
                    st.frac[b] -= dx;
                }
                table_recompute_x(*this);
            }
            if (in.hovered || in.held) {
                m_->dl_.rect_filled({{tbl.col_x[k + 1] - 1.0f, y0 + 3.0f}, {tbl.col_x[k + 1] + 1.0f, y0 + hh - 3.0f}},
                                m_->style_.accent.scaled_alpha(in.held ? 0.9f : 0.6f));
            }
        }
    }

    // the header cells: press = sort, drag = move the column
    struct header_hit { id key{}; bool pressed{}; };
    std::array<header_hit, 16> hits{};
    const u8 was_dragging = st.drag_col1;
    if (!m_->input_.mouse_down_) { st.drag_col1 = 0; st.press_col1 = 0; }
    for (u32 k = 0; k < tbl.nvis; ++k) {
        const u32  c    = tbl.vis[k];
        const rect cell = {{tbl.col_x[k], y0}, {tbl.col_x[k + 1], y0 + hh}};
        const id   key  = hash_id(tbl.cols[c].label, hash_id({reinterpret_cast<const char*>(&c), sizeof(c)}, current_seed(*this)));
        const interaction in = interact(*this, key, {{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}});
        hits[c] = {key, in.pressed && was_dragging != c + 1};
        anim_slot* a = anim_find(*this, key);
        if (a == nullptr && in.hovered) { a = &anim_for(*this, key); }
        if (a != nullptr) {
            a->hover = approach(*this, a->hover, in.hovered ? 1.0f : 0.0f);
            a->last_frame = (a->hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
        }
        if (has_flag(tbl.flags, table_flags::reorderable) && !column_flag(c, table_column_flags::no_reorder)) {
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
        for (u32 k = 0; k < tbl.nvis; ++k) {
            if (tbl.vis[k] == c) { slot = k; }
            else if ((tbl.col_x[k] + tbl.col_x[k + 1]) * 0.5f < m_->input_.mouse_.x) { ++target; }
        }
        if (target != slot) {
            const u32 d = tbl.vis[target];
            u32 from = 0, to = 0;
            for (u32 p = 0; p < tbl.ncols; ++p) {
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
                table_recompute_x(*this);
            }
        }
    }

    shape_style bg;
    bg.radius      = radii(m_->style_.rounding * 0.6f, corners::top);
    bg.fill_top    = lighten(m_->style_.title_bg, m_->style_.gradient * 0.8f);
    bg.fill_bottom = m_->style_.title_bg;
    m_->dl_.shape({{tbl.origin.x, y0}, {tbl.origin.x + tbl.width, y0 + hh}}, bg);

    int clicked = -1;
    for (u32 k = 0; k < tbl.nvis; ++k) {
        const u32  c    = tbl.vis[k];
        const rect cell = {{tbl.col_x[k], y0}, {tbl.col_x[k + 1], y0 + hh}};
        if (hits[c].pressed) { clicked = static_cast<int>(c); }

        const anim_slot* a = anim_find(*this, hits[c].key);
        const f32 hover = a != nullptr ? a->hover : 0.0f;
        if (hover > 0.01f) {
            m_->dl_.rect_filled(cell, m_->style_.widget_hover.scaled_alpha(0.5f * hover));
        }
        if (st.drag_col1 == c + 1 && m_->input_.mouse_down_) {
            m_->dl_.rect_filled(cell, m_->style_.accent.scaled_alpha(0.22f));
        }

        const std::string_view label = visible_label(tbl.cols[c].label);
        const bool sorted = static_cast<int>(c) == sort_column;
        const f32 text_max = cell.width() - 2.0f * tbl.pad_x - (sorted ? 14.0f : 0.0f);
        if (m_->rich_.rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
            const vec2 ts = label_size(*this, f, label);
            m_->dl_.push_clip({{cell.min.x + tbl.pad_x, cell.min.y}, {cell.min.x + tbl.pad_x + std::max(text_max, 0.0f), cell.max.y}});
            label_draw(*this, {cell.min.x + tbl.pad_x, cell.min.y + (hh - ts.y) * 0.5f}, m_->style_.text, label, f);
            m_->dl_.pop_clip();
        } else {
            std::array<char, 128> cut; // (on the stack: this runs every frame for a narrow column)
            std::string_view shown = label;
            if (m_->font_.measure(f, label).x > text_max) {
                std::string_view rest = label;
                std::size_t n = 0;
                f32 used = 0.0f;
                while (!rest.empty()) {
                    const std::string_view before = rest;
                    const char32_t cp = decode_utf8(rest);
                    const f32 adv = m_->font_.advance(f, cp);
                    const std::size_t len = before.size() - rest.size();
                    if (used + adv > text_max - 10.0f || n + len + 2 > cut.size()) { break; }
                    used += adv;
                    std::copy_n(before.data(), len, cut.data() + n);
                    n += len;
                }
                cut[n++] = '.';
                cut[n++] = '.';
                shown = {cut.data(), n};
            }
            m_->dl_.text({cell.min.x + tbl.pad_x, cell.min.y + (hh - lh) * 0.5f}, m_->style_.text, shown, f);
        }

        if (sorted) {
            const vec2 sc{cell.max.x - tbl.pad_x - 3.0f, cell.center().y};
            if (ascending) {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y + 2.5f}, {sc.x, sc.y - 3.0f}, {sc.x + 4.0f, sc.y + 2.5f}, m_->style_.accent_hover);
            } else {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y - 2.5f}, {sc.x + 4.0f, sc.y - 2.5f}, {sc.x, sc.y + 3.0f}, m_->style_.accent_hover);
            }
        }
    }

    m_->dl_.rect_filled({{tbl.origin.x, y0 + hh - 1.0f}, {tbl.origin.x + tbl.width, y0 + hh}}, m_->style_.border);
    tbl.row_y       = y0 + hh;
    tbl.header_done = true;
    if (sideways) { m_->dl_.pop_clip(); }
    return clicked;
}

void internal::table_start_body(context& ui)
{
    auto& m = context_access::of(ui);
    auto& tbl = m.tree_table_.table_;
    if (tbl.body_started) {
        return;
    }
    tbl.body_started = true;
    tbl.body_top     = tbl.row_y;
    const f32 hbar = has_flag(tbl.flags, table_flags::scroll_x) && tbl.span > tbl.width + 0.5f ? 10.0f : 0.0f;

    if (tbl.height_limit > 0.0f) {
        table_state& st = *tbl.state;
        tbl.scroll_mode = true;
        tbl.body_h = std::max(tbl.origin.y + tbl.height_limit - tbl.body_top - hbar, tbl.min_row_h);
        const rect region = {{tbl.origin.x, tbl.body_top}, {tbl.origin.x + tbl.width, tbl.body_top + tbl.body_h}};

        if (st.scroll_wanted >= 0.0f) {
            st.scroll         = st.scroll_wanted;
            st.scroll_wanted  = -1.0f;
        }
        st.scroll = std::clamp(st.scroll, 0.0f, std::max(0.0f, st.content_h - tbl.body_h));

        m.dl_.push_clip(region);
        tbl.clip_pushed = true;
        tbl.row_y       = tbl.body_top - st.scroll;
    } else if (has_flag(tbl.flags, table_flags::scroll_x)) { // (columns can lie outside the table)
        const rect outer = m.dl_.clip();
        m.dl_.push_clip({{tbl.origin.x, outer.min.y}, {tbl.origin.x + tbl.width, outer.max.y}});
        tbl.clip_pushed = true;
    }
}

void internal::table_finish_row(context& ui)
{
    auto& m = context_access::of(ui);
    auto& tbl = m.tree_table_.table_;
    table_state& st = *tbl.state;
    if (tbl.cell_clip) { // cell content ends here; the row lines below are not clipped to the cell
        m.dl_.pop_clip();
        tbl.cell_clip = false;
    }
    if (tbl.col >= 0 && !m.layout_.first) {
        tbl.row_bottom    = std::max(tbl.row_bottom, m.layout_.bottom + tbl.pad_y);
        tbl.row_had_content = true;
    }

    const f32 height = tbl.row_had_content ? std::max(tbl.row_bottom - tbl.row_top, tbl.min_row_h)
                                              : std::max(st.row_hint, tbl.min_row_h);
    if (tbl.row_visible && has_flag(tbl.flags, table_flags::borders)) {
        m.dl_.rect_filled({{tbl.origin.x, tbl.row_top + height - 1.0f}, {tbl.origin.x + tbl.width, tbl.row_top + height}},
                        m.style_.border.scaled_alpha(0.45f));
    }
    if (tbl.row_had_content) {
        st.row_hint = height;
    }
    tbl.row_y  = tbl.row_top + height;
    tbl.in_row = false;
    tbl.col    = -1;
}

bool context::table_next_row()
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active) {
        return false;
    }
    table_finalize_columns(*this);
    if (tbl.in_row) {
        table_finish_row(*this);
    }
    table_start_body(*this);

    table_state& st = *tbl.state;
    tbl.in_row          = true;
    tbl.row_top         = tbl.row_y;
    tbl.row_bottom      = tbl.row_top;
    tbl.row_had_content = false;
    tbl.col             = -1;
    ++tbl.row_index;

    // background is drawn now using the previous row's height, before the content goes on top; the same height
    // is seeded into each cell's layout so this row's content can center against it too (table_next_column)
    const f32  hint = std::max(st.row_hint, tbl.min_row_h);
    tbl.row_hint = hint;
    const rect row  = {{tbl.origin.x, tbl.row_top}, {tbl.origin.x + tbl.width, tbl.row_top + hint}};
    tbl.row_visible = m_->dl_.clip().overlaps(row);
    if (tbl.row_visible) {
        if (has_flag(tbl.flags, table_flags::striped) && (tbl.row_index & 1) == 0) {
            m_->dl_.rect_filled(row, m_->style_.widget_bg.scaled_alpha(0.3f));
        }
        if (has_flag(tbl.flags, table_flags::row_hover) && pointer_over(*this, row)) {
            m_->dl_.rect_filled(row, m_->style_.accent.scaled_alpha(0.11f));
        }
    }
    return tbl.row_visible;
}

bool context::table_next_column()
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active) {
        return false;
    }
    if (!tbl.in_row) {
        (void)table_next_row();
    }
    if (tbl.cell_clip) {
        m_->dl_.pop_clip();
        tbl.cell_clip = false;
    }
    if (tbl.col >= 0 && !m_->layout_.first) {
        tbl.row_bottom      = std::max(tbl.row_bottom, m_->layout_.bottom + tbl.pad_y);
        tbl.row_had_content = true;
    }
    if (tbl.col + 1 >= static_cast<int>(tbl.ncols)) {
        (void)table_next_row(); // running past the last column wraps to the next row
    }

    ++tbl.col;
    const auto c = static_cast<u32>(tbl.col);
    m_->layout_             = {};
    m_->layout_.origin      = {tbl.x0[c] + tbl.pad_x, tbl.row_top + tbl.pad_y};
    m_->layout_.width       = std::max(tbl.x1[c] - tbl.x0[c] - 2.0f * tbl.pad_x, 1.0f);
    // a plain-text cell next to a taller one (a framed input, a toggle, ...) centers against the row's known
    // height instead of sitting flush at its top
    m_->layout_.line_h_seed = std::max(0.0f, tbl.row_hint - 2.0f * tbl.pad_y);

    // cell content never spills into the next column (hidden columns show nothing)
    const rect outer_clip = m_->dl_.clip();
    m_->dl_.push_clip({{tbl.x0[c], outer_clip.min.y}, {tbl.x1[c], outer_clip.max.y}});
    tbl.cell_clip = true;
    return tbl.x1[c] > tbl.x0[c];
}

void context::end_table()
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active) {
        return;
    }
    table_finalize_columns(*this);
    if (tbl.in_row) {
        table_finish_row(*this);
    }
    table_start_body(*this);
    table_state& st = *tbl.state;

    // sideways scrolling: tilt wheel, or Shift + wheel. before the vertical wheel below, which would take it
    const bool sideways = has_flag(tbl.flags, table_flags::scroll_x);
    const f32  max_x    = sideways ? std::max(0.0f, tbl.span - tbl.width) : 0.0f;
    const f32  hbar     = max_x > 0.5f ? 10.0f : 0.0f;
    if (sideways) {
        const f32  bottom = tbl.scroll_mode ? tbl.body_top + tbl.body_h : tbl.row_y;
        const rect all    = {tbl.origin, {tbl.origin.x + tbl.width, bottom}};
        if (max_x > 0.5f && m_->input_.wheel_x_ != 0.0f && !m_->wheel_x_consumed_ && pointer_over(*this, all)) {
            st.scroll_x = st.scroll_x + wheel_scroll_x(*this, st.row_hint, tbl.width);
            m_->wheel_x_consumed_ = true;
        } else if (max_x > 0.5f && m_->input_.mod_shift_ && m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(*this, all)) {
            st.scroll_x = st.scroll_x - wheel_scroll(*this, st.row_hint, tbl.width);
            m_->wheel_consumed_ = true;
        }
        st.scroll_x = std::clamp(st.scroll_x, 0.0f, max_x);
    }

    f32 total_h;
    if (tbl.scroll_mode) {
        st.content_h = tbl.row_y - (tbl.body_top - st.scroll);
        if (tbl.clip_pushed) {
            m_->dl_.pop_clip();
        }
        total_h = tbl.body_top - tbl.origin.y + tbl.body_h;

        // the wheel goes to the innermost scroller under the pointer; nested tables already had their turn
        const rect region = {{tbl.origin.x, tbl.body_top}, {tbl.origin.x + tbl.width, tbl.body_top + tbl.body_h}};
        if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(*this, region)) {
            st.scroll -= wheel_scroll(*this, st.row_hint, tbl.body_h);
            st.scroll_wanted    = -1.0f; // the user's scrolling wins over a table_set_scroll_y() still waiting
            m_->wheel_consumed_ = true;
        }

        const f32 max_scroll = std::max(0.0f, st.content_h - tbl.body_h);
        st.scroll = std::clamp(st.scroll, 0.0f, max_scroll);
        if (max_scroll > 0.0f) {
            const f32  x1    = tbl.origin.x + tbl.width;
            const rect track = {{x1 - 11.0f, tbl.body_top + 2.0f}, {x1, tbl.body_top + tbl.body_h - 2.0f}};
            const f32  was   = st.scroll;
            st.scroll = scrollbar(*this, part_id(part::table_scrollbar, current_seed(*this)), track, false, tbl.body_h, st.content_h, st.scroll, st.grab);
            if (st.scroll != was) { st.scroll_wanted = -1.0f; } // dragging wins over a table_set_scroll_y() still waiting
        }
    } else {
        if (tbl.clip_pushed) {
            m_->dl_.pop_clip();
        }
        total_h = tbl.row_y - tbl.origin.y;
    }

    total_h += hbar;
    const rect bounds = {tbl.origin, {tbl.origin.x + tbl.width, tbl.origin.y + total_h}};
    if (has_flag(tbl.flags, table_flags::borders)) {
        for (u32 i = 1; i < tbl.nvis; ++i) {
            if (tbl.col_x[i] > bounds.max.x || tbl.col_x[i] < bounds.min.x) { continue; } // scrolled out
            m_->dl_.rect_filled({{std::round(tbl.col_x[i]), bounds.min.y}, {std::round(tbl.col_x[i]) + 1.0f, bounds.max.y}},
                            m_->style_.border.scaled_alpha(0.55f));
        }
        shape_style edge;
        edge.radius       = radii(m_->style_.rounding * 0.6f);
        edge.border       = m_->style_.border;
        edge.border_width = m_->style_.border_width;
        m_->dl_.shape(bounds, edge);
    }

    if (hbar > 0.0f) {
        const rect track = {{bounds.min.x + 4.0f, bounds.max.y - 11.0f}, {bounds.max.x - 4.0f, bounds.max.y}};
        st.scroll_x = scrollbar(*this, part_id(part::table_scrollbar_x, current_seed(*this)), track, true, tbl.width, tbl.span, st.scroll_x, st.grab_x);
    }

    m_->layout_            = tbl.outer;
    m_->layout_.line_h     = total_h;
    m_->layout_.bottom     = std::max(m_->layout_.bottom, tbl.origin.y + total_h);
    m_->layout_.same_line  = false;
    m_->layout_.first      = false;
    pop_id();
    tbl = {};
    if (m_->tree_table_.table_depth_ > 0) { // back to the table this one was nested in
        tbl = m_->tree_table_.table_stack_[--m_->tree_table_.table_depth_];
    }
}

void context::table_skip_rows(int count)
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active || count <= 0) {
        return;
    }
    table_finalize_columns(*this);
    if (tbl.in_row) {
        table_finish_row(*this);
    }
    table_start_body(*this);
    tbl.row_y     += static_cast<f32>(count) * std::max(tbl.state->row_hint, tbl.min_row_h);
    tbl.row_index += static_cast<u32>(count);
}

f32 context::table_scroll_y() const noexcept
{
    auto& tbl = m_->tree_table_.table_;
    return tbl.active && tbl.scroll_mode ? tbl.state->scroll : 0.0f;
}

f32 context::table_scroll_max_y() const noexcept
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active || !tbl.scroll_mode) {
        return 0.0f;
    }
    return std::max(0.0f, tbl.state->content_h - tbl.body_h);
}

void context::table_set_scroll_y(f32 y) noexcept
{
    auto& tbl = m_->tree_table_.table_;
    // not st.scroll itself: end_table measures this frame's content from the offset the rows were laid out with
    if (tbl.active && tbl.scroll_mode) {
        tbl.state->scroll_wanted = std::max(0.0f, y);
    }
}

std::string context::table_save_layout(std::string_view id_label) const
{
    const id key = hash_id(id_label, current_seed(*this)); // a query, not a submission: nothing to record
    for (const table_state& t : m_->tree_table_.tables_) {
        if (t.key != key || !t.inited) { continue; }
        return table_layout_text(t);
    }
    return {};
}

std::string internal::table_layout_text(const table_state& t)
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
    if (t.content_w > 0.0f) { out += std::format(";span={:.1f}", t.content_w); } // (scroll_x tables)
    return out;
}

void context::table_load_layout(std::string_view id_label, std::string_view text)
{
    const id key = widget_id(*this, id_label);
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
    auto& tbl = m_->tree_table_.table_;
    if (m_->cur_ == nullptr || !tbl.active || tbl.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(*this, label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_.rich_depth_ > 0 ? label_size(*this, f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(tbl.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place(*this, {m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - tbl.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + tbl.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(*this, key, hit);

    bool& open = tree_open_state(*this, key, has_flag(flags, tree_flags::default_open));
    const bool arrow_hit = m_->input_.mouse_.x < row.min.x + 16.0f;
    m_->tree_table_.item_pressed_ = in.pressed;
    if (in.pressed && (!has_flag(flags, tree_flags::arrow_only) || arrow_hit)) {
        open = !open;
    }
    const bool is_open = open;

    anim_slot& a = anim_for(*this, key);
    a.hover  = approach(*this, a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(*this, a.toggle, is_open ? 1.0f : 0.0f, m_->style_.anim_speed * 0.9f);

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
    const vec2 tsize = label_size(*this, f, shown);
    label_draw(*this, {row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f},
               selected ? m_->style_.accent_hover : m_->style_.text, shown, f);

    if (!is_open) {
        return false;
    }
    ++tbl.tree_depth;
    push_id(label);
    return true;
}

bool context::table_tree_leaf(std::string_view label, bool selected)
{
    auto& tbl = m_->tree_table_.table_;
    if (m_->cur_ == nullptr || !tbl.active || tbl.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(*this, label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_.rich_depth_ > 0 ? label_size(*this, f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(tbl.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place(*this, {m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - tbl.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + tbl.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(*this, key, hit);
    m_->tree_table_.item_pressed_ = in.pressed;

    anim_slot* a = anim_find(*this, key);
    if (a == nullptr && in.hovered) { a = &anim_for(*this, key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(*this, a->hover, in.hovered ? 1.0f : 0.0f);
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
    const vec2 tsize = label_size(*this, f, shown);
    label_draw(*this, {row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f}, selected ? m_->style_.accent_hover : m_->style_.text, shown, f);
    return in.pressed;
}

void context::table_tree_pop()
{
    auto& tbl = m_->tree_table_.table_;
    if (!tbl.active || tbl.tree_depth == 0) {
        return;
    }
    --tbl.tree_depth;
    pop_id();
}

} // namespace strata

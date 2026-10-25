// windows: begin / end, nesting, the window table, move / resize / collapse

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

static void forget_window(context& ui, id key) noexcept;
static void resume_parent_window(context& ui) noexcept; // ends a nested begin_window: the suspended parent continues

window_state* internal::window_for(context& ui, id key, vec2 pos, f32 width) noexcept
{
    auto& m = context_access::of(ui);
    window_state* free_slot = nullptr;
    for (window_state& w : m.win_.windows_) {
        if (w.key == key) {
            return &w;
        }
        if (free_slot == nullptr && w.key == 0) {
            free_slot = &w;
        }
    }
    if (free_slot == nullptr) {
        // table full: the least recently shown window gives up its slot (it restarts where its code places it). exempt:
        // docked windows and floating docks (the layout refers to them), open modals, anything shown this or last frame
        const auto owns_space = [&](id k) {
            return std::ranges::any_of(m.dock_->spaces, [k](const dock_space& sp) { return sp.key != 0 && sp.owner == k; });
        };
        const auto modal_open = [&](id k) {
            return std::find(m.modal_.modal_stack_.begin(), m.modal_.modal_stack_.begin() + m.modal_.modal_count_, k) != m.modal_.modal_stack_.begin() + m.modal_.modal_count_;
        };
        for (window_state& w : m.win_.windows_) {
            if (w.dock != 0 || w.last_frame + 1 >= m.frame_ || owns_space(w.key) || modal_open(w.key)) { continue; }
            if (free_slot == nullptr || w.last_frame < free_slot->last_frame) { free_slot = &w; }
        }
        if (free_slot == nullptr) {
            report_limit(ui, "windows (context_config::capacity.windows)", static_cast<u32>(m.win_.windows_.size()));
            return nullptr;
        }
        forget_window(ui, free_slot->key);
    }
    *free_slot = {.key = key, .pos = pos, .width = width};
    return free_slot;
}

// references to a window by key besides its slot: stacking order and keyboard / hover / focus trackers
static void forget_window(context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    if (const u32 z = z_index(ui, key); z != no_z) {
        std::copy(m.win_.z_order_.begin() + z + 1, m.win_.z_order_.begin() + m.win_.z_count_, m.win_.z_order_.begin() + z);
        --m.win_.z_count_;
    }
    for (id* k : {&m.win_.focused_window_, &m.key_window_, &m.win_.hovered_window_prev_, &m.win_.hovered_window_cur_}) {
        if (*k == key) { *k = 0; }
    }
}

window_state* internal::window_find(context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    for (window_state& w : m.win_.windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

const window_state* internal::window_find(const context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    for (const window_state& w : m.win_.windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

rect context::window_rect(std::string_view title) const noexcept
{
    const window_state* w = window_find(*this, hash_id(title, m_->ids_.root()));
    if (w == nullptr) {
        return {};
    }
    const f32 height = w->collapsed ? w->title_h
                     : w->height > 0.0f ? w->height
                     : w->capped_h > 0.0f ? w->capped_h
                                          : w->title_h + 2.0f * m_->style_.padding + w->content_h;
    return rect::from_size(w->pos, {w->width, height});
}

u32 internal::z_index(const context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    for (u32 i = 0; i < m.win_.z_count_; ++i) {
        if (m.win_.z_order_[i] == key) {
            return i;
        }
    }
    return no_z;
}

void internal::bring_to_front(context& ui, id key) noexcept
{
    auto& m = context_access::of(ui);
    const u32 i = z_index(ui, key);
    if (i == no_z) {
        if (m.win_.z_count_ < m.win_.z_order_.size()) {
            m.win_.z_order_[m.win_.z_count_++] = key;
        } else {
            report_limit(ui, "window stacking order (context_config::capacity.windows)", static_cast<u32>(m.win_.z_order_.size()));
        }
        return;
    }
    std::rotate(m.win_.z_order_.begin() + i, m.win_.z_order_.begin() + i + 1, m.win_.z_order_.begin() + m.win_.z_count_);
}

bool context::begin_window(std::string_view title, vec2 initial_pos, f32 width)
{
    return begin_window(title, initial_pos, vec2{width, 0.0f}, window_flags::none);
}

bool context::begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
{
    const id wid = hash_id(title, m_->ids_.root());
    if (m_->cur_ != nullptr) { // begun inside another window's code: suspend that one until this one ends
        if (m_->nest_depth_ >= impl::max_window_nesting) {
            report_limit(*this, "windows begun inside windows (max_window_nesting)", impl::max_window_nesting);
            ++m_->nest_overflow_;
            return false;
        }
        impl::window_nest& n = m_->nest_[m_->nest_depth_++];
        n = {m_->cur_, m_->cur_window_, m_->cur_flags_, m_->cur_frame_, m_->layout_, m_->window_faded_, m_->run_owner_,
             m_->child_base_, m_->tree_table_.table_, false};
        m_->tree_table_.table_ = {};
        m_->child_base_        = m_->children_cards_.child_depth_;
        m_->window_faded_      = false;
        m_->cur_               = nullptr;
        m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_}); // not clipped to the parent
    }
    window_state* st = window_for(*this, wid, initial_pos, size.x);
    if (st == nullptr) {
        if (m_->nest_depth_ > 0 && m_->cur_ == nullptr) { // put the parent back; this begin's end_window must not close it
            resume_parent_window(*this);
            m_->nest_[m_->nest_depth_++].dud = true;
        }
        return false;
    }
    if (st->content_h > 0.0f) { apply_pending_scroll(*this, wid, st->scroll, &st->scroll_x); } // (once its content has been measured)
    if (!st->size_set) {
        // (the state can exist before the first begin_window when dock_window() got there first)
        st->pos      = initial_pos;
        st->width    = std::max(st->width, size.x);
        st->height   = std::max(size.y, 0.0f);
        st->size_set = true;
        if (st->dock != 0) {
            st->float_size = {st->width, st->height};
        }
    }
    if (has_flag(flags, window_flags::placed)) {
        constexpr u16 drop = static_cast<u16>(window_flags::resizable) | static_cast<u16>(window_flags::drag_by_body);
        flags = static_cast<window_flags>(static_cast<u16>(flags) & ~drop) | window_flags::no_move;
        if (st->dock == 0) { // (a docked window takes its node's rect below)
            st->pos    = initial_pos;
            st->width  = size.x;
            st->height = std::max(size.y, 0.0f);
        }
    }
    st->last_frame = m_->frame_;
    st->passive    = has_flag(flags, window_flags::no_inputs);
    if (st->passive) { // nothing to grab, move, resize or dock
        constexpr u16 drop = static_cast<u16>(window_flags::resizable) | static_cast<u16>(window_flags::drag_by_body) | static_cast<u16>(window_flags::dockable);
        flags = static_cast<window_flags>(static_cast<u16>(flags) & ~drop) | window_flags::no_move | window_flags::no_collapse;
    }
    const bool is_menubar   = std::exchange(m_->next_window_menubar_, false);
    const u32  modal_level  = std::exchange(m_->modal_.next_window_modal_level_, 0);
    st->menubar     = is_menubar;
    st->modal_level = modal_level;
    if (is_menubar) {
        st->pos    = {0.0f, 0.0f};
        st->width  = m_->display_.x;
        st->height = size.y;
    }
    { // the visible part of the title, for the tabs of a dock node
        const std::string_view vt = visible_label(title);
        std::size_t n = std::min(vt.size(), st->title.size() - 1);
        while (n > 0 && n < vt.size() && is_continuation(vt[n])) { --n; }
        std::copy_n(vt.data(), n, st->title.data());
        st->title_len = static_cast<u8>(n);
    }

    // docked: the dock node sets position and size; the tab bar replaces the title bar
    const dock_node*  node  = nullptr;
    const dock_space* space = nullptr;
    if (st->dock != 0 && st->dock <= max_dock_nodes && has_flag(flags, window_flags::dockable)) {
        const dock_node& n = m_->dock_->nodes[st->dock - 1];
        if (n.used && n.leaf() && n.space < max_dock_spaces && m_->dock_->spaces[n.space].set) {
            node  = &n;
            space = &m_->dock_->spaces[n.space];
        }
    }
    const bool docked     = node != nullptr;
    const bool hidden_tab = docked && (node->active != wid || space->hidden);
    st->docked_now = docked;
    st->dock_owner = docked ? space->owner : id{};
    if (docked) {
        constexpr u16 strip = static_cast<u16>(window_flags::resizable) | static_cast<u16>(window_flags::drag_by_body);
        constexpr u16 add   = static_cast<u16>(window_flags::no_title_bar) | static_cast<u16>(window_flags::no_move);
        flags = static_cast<window_flags>((static_cast<u16>(flags) & ~strip) | add);
        st->pos       = node->shown_content.min;
        st->width     = node->shown_content.width();
        st->height    = node->shown_content.height();
        st->collapsed = false;
    }
    st->resizable = has_flag(flags, window_flags::resizable);

    const bool has_title   = !has_flag(flags, window_flags::no_title_bar);
    const bool can_collapse = has_title && !has_flag(flags, window_flags::no_collapse);
    const bool can_move    = !has_flag(flags, window_flags::no_move);
    const bool background  = !has_flag(flags, window_flags::no_background);
    if (!can_collapse) {
        st->collapsed = false;
    }
    m_->cur_flags_ = flags;

    if (m_->nest_depth_ > 0) {
        push_id_value(*this, wid); // the same widget ids as when this window is begun on its own
    } else {
        push_id(title);
    }
    m_->cur_        = st;
    m_->cur_window_ = wid;

    if (z_index(*this, wid) == no_z) {
        bring_to_front(*this, wid); // first appearance: on top
    }
    // a press on the topmost window under the pointer raises it (docked windows stay behind)
    if (m_->input_.mouse_pressed_ && m_->win_.hovered_window_prev_ == wid && (!docked || st->dock_owner != 0)) {
        bring_to_front(*this, docked ? st->dock_owner : wid); // (a window in a floating dock raises the whole dock)
    }
    // ... and focuses it, docked windows too (without restacking), so panel shortcuts know which panel is active
    if (m_->input_.mouse_pressed_ && m_->win_.hovered_window_prev_ == wid && !st->menubar && m_->modal_.modal_top_prev_ == 0) {
        m_->key_window_ = wid;
    }

    // every window's draw commands form their own run so the windows can be restacked
    if (m_->win_.frame_window_count_ < m_->win_.frame_windows_.size()) {
        m_->win_.frame_windows_[m_->win_.frame_window_count_] = st;
        switch_run(*this, m_->win_.frame_window_count_);
        ++m_->win_.frame_window_count_;
    }

    const f32 lh      = m_->font_.line_height(0);
    const f32 title_h = has_title ? lh + 10.0f : 0.0f;
    const f32 pad     = is_menubar ? 0.0f : m_->style_.padding;
    const bool horiz  = has_flag(flags, window_flags::horizontal) && !is_menubar;
    const f32  hbar   = horiz && st->overflow_x && !st->collapsed ? 10.0f : 0.0f; // last frame's, like the vertical bar
    if (!horiz) { st->overflow_x = false; }
    st->title_h       = title_h;

    // collapse arrow + title drag use last frame's geometry
    const rect arrow_hit = rect::from_size(st->pos, {title_h, title_h});
    const rect drag_hit  = {{st->pos.x + (can_collapse ? title_h : 0.0f), st->pos.y}, {st->pos.x + st->width, st->pos.y + title_h}};

    if (can_collapse && interact(*this, part_id(part::collapse_arrow, wid), arrow_hit).pressed) {
        st->collapsed = !st->collapsed;
    }
    interaction drag_in;
    if (has_title && can_move) {
        drag_in = interact(*this, part_id(part::title_drag, wid), drag_hit);
        if (drag_in.held) {
            st->pos += m_->input_.mouse_delta_;
        }
    }
    const id   body_drag     = part_id(part::body_drag, wid);
    const bool body_dragging = m_->active_ == body_drag && m_->input_.mouse_down_ && can_move;
    if (body_dragging) {
        st->pos += m_->input_.mouse_delta_;
    }
    if (!docked && m_->dock_->any_set && has_flag(flags, window_flags::dockable) && (drag_in.held || body_dragging)) {
        if (key_pressed(key::escape)) { m_->dock_->void_key = wid; } // Esc: this drag does not dock
        if (m_->dock_->void_key != wid && !m_->input_.mod_shift_) {             // ... and neither does one with Shift held
            m_->dock_->drag_win   = wid;
            const f32 shown_h = st->height > 0.0f ? st->height : title_h + 2.0f * pad + st->content_h + hbar;
            m_->dock_->target_cur = dock_pick(*this, m_->input_.mouse_, no_node, {st->width, shown_h});
        }
    }

    // resizing: the right edge, the bottom edge and the corner (last frame's geometry)
    if (st->resizable && !st->collapsed) {
        const f32 prev_h = st->height > 0.0f ? st->height
                         : st->capped_h > 0.0f ? st->capped_h
                                               : title_h + pad + st->content_h + pad + hbar;
        const f32 grip   = 6.0f;
        const f32 corner = 16.0f;
        const f32 x1 = st->pos.x + st->width;
        const f32 y1 = st->pos.y + prev_h;

        const interaction right  = interact(*this, part_id(part::resize_right, wid), {{x1 - grip, st->pos.y + title_h}, {x1, y1 - corner}});
        const interaction bottom = interact(*this, part_id(part::resize_bottom, wid), {{st->pos.x, y1 - grip}, {x1 - corner, y1}});
        const interaction edge   = interact(*this, part_id(part::resize_corner, wid), {{x1 - corner, y1 - corner}, {x1, y1}});

        if (right.hovered || right.held)   { m_->cursor_ = cursor_kind::resize_ew; }
        if (bottom.hovered || bottom.held) { m_->cursor_ = cursor_kind::resize_ns; }
        if (edge.hovered || edge.held)     { m_->cursor_ = cursor_kind::resize_nwse; }

        const f32 max_w = m_->display_.x > 0.0f ? m_->display_.x : 4096.0f;
        const f32 max_h = m_->display_.y > 0.0f ? m_->display_.y : 4096.0f;
        if (right.held || edge.held) {
            st->width = std::clamp(st->width + m_->input_.mouse_delta_.x, 150.0f, max_w);
        }
        if (bottom.held || edge.held) {
            st->height = std::clamp(prev_h + m_->input_.mouse_delta_.y, title_h + 48.0f, max_h); // a fixed height from now on
        }
    }

    if (!docked && m_->display_.x > 0 && m_->display_.y > 0) {
        st->pos.x = std::clamp(st->pos.x, 48.0f - st->width, m_->display_.x - 48.0f);
        st->pos.y = std::clamp(st->pos.y, 0.0f, m_->display_.y - std::max(title_h, 24.0f));
    }

    const bool fixed_h = st->height > 0.0f;
    // an auto-height window does not grow past the bottom of the display: it scrolls instead
    st->capped_h = 0.0f;
    if (!fixed_h && !docked && !st->menubar && !st->collapsed && m_->display_.y > 0.0f) {
        const f32 wanted = title_h + pad + st->content_h + pad + hbar;
        const f32 room   = std::max(m_->display_.y - st->pos.y - 8.0f, title_h + 48.0f);
        if (wanted > room) { st->capped_h = room; }
    }
    const bool scrolls = fixed_h || st->capped_h > 0.0f;
    const f32  height  = st->collapsed ? title_h
                       : fixed_h ? st->height
                       : scrolls ? st->capped_h
                                 : title_h + pad + st->content_h + pad + hbar;
    const rect frame   = rect::from_size(st->pos, {st->width, height});
    const rect bar     = rect::from_size(st->pos, {st->width, title_h});
    m_->cur_frame_ = frame;

    // a modal takes the input from everything else
    const bool input_blocked = m_->modal_.modal_top_prev_ != 0 && wid != m_->modal_.modal_top_prev_;
    if (frame.contains(m_->input_.mouse_) && !hidden_tab && !input_blocked && !st->passive) {
        // a floating window beats a docked one, the menu bar beats both, a modal beats them all
        const bool in_float = docked && st->dock_owner != 0;
        const u32 rank = modal_level != 0 ? 0x30000u + modal_level * 0x10000u : (is_menubar ? 0x20000u : (docked && !in_float ? 0u : 0x10000u));
        u32 zi = z_index(*this, in_float ? st->dock_owner : wid);
        if (zi == no_z) { zi = 0; }
        const u32 z = zi * 2 + (in_float ? 1u : 0u) + rank; // a window docked in a floating dock is just above that dock
        if (m_->win_.hovered_z_ == no_z || z > m_->win_.hovered_z_) {
            m_->win_.hovered_window_cur_ = wid; // highest z under the pointer wins
            m_->win_.hovered_z_          = z;
            m_->hovered_docked_cur_ = docked;
        }
    }

    // a window carried over a dock target turns see-through so the target pane shows
    st->ghost = approach(*this, st->ghost, !docked && m_->dock_->drag_prev == wid && m_->dock_->target_prev.valid ? 1.0f : 0.0f);
    if (st->ghost > 0.01f) {
        m_->dl_.push_alpha(1.0f - 0.45f * st->ghost);
        m_->window_faded_ = true;
    }

    const f32 round = m_->style_.rounding;

    if (docked) {
        if (background && !hidden_tab) {
            m_->dl_.rect_filled(frame, m_->style_.window_bg);
        }
    } else if (background) {
        const bool acrylic = has_flag(flags, window_flags::acrylic);
        // body: fill + soft drop shadow in a single shader quad
        shape_style body;
        body.radius        = radii(round);
        body.fill_top      = m_->style_.window_bg;
        body.fill_bottom   = m_->style_.window_bg;
        body.shadow        = m_->style_.shadow;
        body.shadow_blur   = m_->style_.shadow_blur;
        body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.45f};
        if (acrylic) { // the shadow on its own, then the blurred frame with the tint on top
            body.fill_top = body.fill_bottom = color{0, 0, 0, 0};
            m_->dl_.shape(frame, body);
            m_->dl_.backdrop(frame, m_->style_.blur_radius, m_->style_.window_bg.scaled_alpha(m_->style_.acrylic_alpha), radii(round),
                         m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);
        } else {
            m_->dl_.shape(frame, body);
        }

        if (has_title) {
            shape_style head;
            head.radius      = radii(round, st->collapsed ? corners::all : corners::top);
            head.fill_top    = lighten(m_->style_.title_bg, m_->style_.gradient * 0.8f);
            head.fill_bottom = m_->style_.title_bg;
            if (acrylic) {
                head.fill_top    = head.fill_top.scaled_alpha(0.55f);
                head.fill_bottom = head.fill_bottom.scaled_alpha(0.55f);
            }
            m_->dl_.shape(bar, head);
            if (!st->collapsed) {
                m_->dl_.rect_filled({{bar.min.x, bar.max.y - 1.0f}, bar.max}, m_->style_.border);
            }
        }

        shape_style edge;
        edge.radius       = radii(round);
        edge.border       = m_->style_.border;
        edge.border_width = m_->style_.border_width;
        m_->dl_.shape(frame, edge);
    }

    if (st->resizable && !st->collapsed) {
        const bool   hot = m_->cursor_ == cursor_kind::resize_nwse;
        const color  gc  = hot ? m_->style_.accent_hover : m_->style_.text_dim.scaled_alpha(0.55f);
        const vec2   br  = frame.max;
        for (f32 k = 0.0f; k < 3.0f; ++k) {
            const f32 d = 4.0f + k * 4.0f;
            m_->dl_.line({br.x - 3.0f, br.y - d - 1.0f}, {br.x - d - 1.0f, br.y - 3.0f}, gc, 1.5f);
        }
    }

    if (has_title) {
        if (can_collapse) {
            const vec2 c = arrow_hit.center();
            const f32  s = 4.0f;
            if (st->collapsed) {
                m_->dl_.triangle_filled({c.x - s * 0.5f, c.y - s}, {c.x + s * 0.75f, c.y}, {c.x - s * 0.5f, c.y + s}, m_->style_.text_dim);
            } else {
                m_->dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.75f}, m_->style_.text_dim);
            }
        }
        m_->dl_.text({st->pos.x + (can_collapse ? title_h : pad), st->pos.y + (title_h - lh) * 0.5f},
                 wid == m_->win_.focused_window_ || m_->win_.focused_window_ == 0 ? m_->style_.text : m_->style_.text_dim, visible_label(title), 0);
    }

    // content lives below the title bar; fixed-height windows scroll
    if (st->collapsed) {
        m_->dl_.push_clip(bar);
    } else if (hidden_tab) {
        m_->dl_.push_clip({frame.min, frame.min}); // an inactive tab: nothing of it is drawn
    } else {
        m_->dl_.push_clip({{frame.min.x, frame.min.y + title_h}, frame.max});
    }

    if (scrolls) {
        const f32 max_scroll = std::max(0.0f, st->content_h + 2.0f * pad - (height - title_h - hbar));
        st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);
    } else {
        st->scroll = 0.0f;
    }
    if (horiz) {
        const f32 view_w = st->width - 2.0f * pad - (scrolls && st->overflow ? 10.0f : 0.0f);
        st->scroll_x = std::clamp(st->scroll_x, 0.0f, std::max(0.0f, st->content_w - view_w));
    } else {
        st->scroll_x = 0.0f;
    }

    m_->layout_        = {};
    m_->layout_.origin = {st->pos.x + pad - st->scroll_x, st->pos.y + title_h + pad - st->scroll};
    m_->layout_.width  = st->width - 2.0f * pad - (scrolls && st->overflow ? 10.0f : 0.0f);
    m_->layout_.bound_bottom = fixed_h ? st->pos.y + height - pad - hbar : 0.0f;
    return !st->collapsed && !hidden_tab;
}

void context::end_window()
{
    if (m_->nest_overflow_ > 0) {
        --m_->nest_overflow_;
        return;
    }
    if (m_->nest_depth_ > 0 && m_->nest_[m_->nest_depth_ - 1].dud) {
        m_->nest_[--m_->nest_depth_].dud = false;
        return;
    }
    if (m_->cur_ == nullptr) {
        return;
    }
    window_state& w = *m_->cur_;
    const bool has_title = !has_flag(m_->cur_flags_, window_flags::no_title_bar);
    if (!w.collapsed) {
        w.content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;

        const f32  title_h = has_title ? m_->font_.line_height(0) + 10.0f : 0.0f;
        const f32  pad     = w.menubar ? 0.0f : m_->style_.padding;
        const rect frame   = m_->cur_frame_;
        const f32  view_w  = w.width - 2.0f * pad - (w.overflow ? 10.0f : 0.0f);
        if (has_flag(m_->cur_flags_, window_flags::horizontal) && !w.menubar) {
            w.content_w  = m_->layout_.first ? 0.0f : m_->layout_.right - m_->layout_.origin.x;
            w.overflow_x = w.content_w > view_w + 0.5f;
        } else {
            w.overflow_x = false;
        }
        const f32 hbar = w.overflow_x ? 10.0f : 0.0f;
        w.max_scroll_x = w.overflow_x ? w.content_w - view_w : 0.0f;

        if (w.overflow_x) {
            const rect body = {{frame.min.x, frame.min.y + title_h}, frame.max};
            // tilt wheel, or Shift + wheel (the Windows convention)
            if (m_->input_.wheel_x_ != 0.0f && !m_->wheel_x_consumed_ && pointer_over(*this, body)) {
                w.scroll_x = std::clamp(w.scroll_x + wheel_scroll_x(*this, m_->font_.line_height(0), view_w), 0.0f, w.max_scroll_x);
                m_->wheel_x_consumed_ = true;
            } else if (m_->input_.mod_shift_ && m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(*this, body)) {
                w.scroll_x = std::clamp(w.scroll_x - wheel_scroll(*this, m_->font_.line_height(0), view_w), 0.0f, w.max_scroll_x);
                m_->wheel_consumed_ = true;
            }
            const rect track = {{body.min.x + 6.0f, body.max.y - 12.0f}, {body.max.x - 6.0f - (w.overflow ? 10.0f : 0.0f), body.max.y - 1.0f}};
            w.scroll_x = scrollbar(*this, part_id(part::window_scrollbar_x, m_->cur_window_), track, true, view_w, w.content_w, w.scroll_x, w.grab_x);
        } else {
            w.scroll_x = 0.0f;
        }

        const f32 shown_h = w.height > 0.0f ? w.height : w.capped_h;
        if (shown_h > 0.0f) {
            const f32 body_h  = shown_h - title_h - hbar;
            const f32 full_h  = w.content_h + 2.0f * pad;
            w.overflow = full_h > body_h + 0.5f;

            if (w.overflow) {
                const f32  max_scroll = full_h - body_h;
                const rect body = {{w.pos.x, w.pos.y + title_h}, {w.pos.x + w.width, w.pos.y + shown_h}};

                if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(*this, body)) {
                    w.scroll = std::clamp(w.scroll - wheel_scroll(*this, m_->font_.line_height(0), body_h), 0.0f, max_scroll);
                    m_->wheel_consumed_ = true;
                }

                const rect track = {{body.max.x - 13.0f, body.min.y + 6.0f}, {body.max.x - 2.0f, body.min.y + body_h - 6.0f}};
                w.scroll = scrollbar(*this, part_id(part::window_scrollbar, m_->cur_window_), track, false, body_h, full_h, w.scroll, w.grab);
            }
        }
    }

    // an empty spot of a drag_by_body window starts moving the window
    if (has_flag(m_->cur_flags_, window_flags::drag_by_body) && !has_flag(m_->cur_flags_, window_flags::no_move) &&
        m_->input_.mouse_pressed_ && m_->active_ == 0 && !m_->swallow_press_ && m_->win_.hovered_window_prev_ == m_->cur_window_ &&
        m_->cur_frame_.contains(m_->input_.mouse_) && !m_->menu_.menu_hit_prev_ && !m_->popup_.popup_covers(m_->input_.mouse_)) {
        m_->active_ = part_id(part::body_drag, m_->cur_window_);
    }

    m_->dl_.pop_clip();
    if (m_->window_faded_) {
        m_->dl_.pop_alpha();
        m_->window_faded_ = false;
    }
    switch_run(*this, run_base);
    pop_id();
    m_->cur_        = nullptr;
    m_->cur_window_ = 0;
    if (m_->nest_depth_ > 0) {
        resume_parent_window(*this);
    }
}

static void resume_parent_window(context& ui) noexcept
{
    auto& m = context_access::of(ui);
    const auto& n = m.nest_[--m.nest_depth_];
    m.dl_.pop_clip(); // the absolute clip the nested begin pushed
    m.cur_               = n.cur;
    m.cur_window_        = n.window;
    m.cur_flags_         = n.flags;
    m.cur_frame_         = n.frame;
    m.layout_            = n.layout;
    m.window_faded_      = n.faded;
    m.child_base_        = n.child_base;
    m.tree_table_.table_ = n.table;
    switch_run(ui, n.run_owner);
}

} // namespace strata

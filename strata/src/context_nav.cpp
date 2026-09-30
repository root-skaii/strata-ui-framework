// keyboard navigation of lists / trees, and the scroll control that keeps the cursor in view.
// a nav scope collects every row inside it (culled ones too, so the cursor can move off-screen) and resolves keys
// at nav_end(). the move shows next frame (focus ring, Enter as a press): invisible lag instead of a second pass.

#include "strata/context.hpp"

#include "context_impl.hpp"


#include <algorithm>

namespace strata {

void context::push_id_value(id key) noexcept
{
    if (!m_->ids_.push_raw(key)) {
        report_limit("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

// navigation ----------------------------------------------------------------------------------

void context::nav_begin(std::string_view id_label)
{
    const id key = widget_id(id_label);
    if (m_->nav_.scope != 0) {
        report_limit("nav_begin inside another nav scope (only one at a time)", 1);
        return;
    }
    if (m_->nav_.scope_key != key) { // another list: start its cursor over
        m_->nav_.scope_key = key;
        m_->nav_.cursor    = 0;
    }
    m_->nav_.scope = key;
    m_->nav_.items.clear();
    // a text field with the keyboard uses the arrow keys itself
    m_->nav_.active = m_->focus_id_ == 0 && m_->hotkey_.hotkey_capture_ == 0;
}

void context::nav_record(id key, const rect& r, u32 depth, bool node, bool open) noexcept
{
    if (m_->nav_.scope == 0 || key == 0) {
        return;
    }
    m_->nav_.items.push_back({key, r, depth, node, open});
}

// the row the cursor was on when Enter was pressed last frame reports it as a press, once
bool context::nav_take(id key) noexcept
{
    if (m_->nav_.scope == 0 || key == 0 || m_->nav_.activate_pending != key) {
        return false;
    }
    m_->nav_.activate_pending = 0;
    return true;
}

void context::nav_click(id key) noexcept
{
    if (m_->nav_.scope != 0) {
        m_->nav_.cursor = key;
    }
}

void context::nav_end()
{
    if (m_->nav_.scope == 0) {
        return;
    }
    const auto& items = m_->nav_.items;
    if (items.empty()) {
        m_->nav_.scope = 0;
        return;
    }

    // the cursor's index in this frame's sequence; a vanished row resets it to the top
    std::size_t at    = 0;
    bool        found = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].key == m_->nav_.cursor) {
            at    = i;
            found = true;
            break;
        }
    }

    bool moved    = false;
    bool activate = false;
    if (m_->nav_.active) {
        const auto step = [&](std::ptrdiff_t d) {
            const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(items.size());
            at    = static_cast<std::size_t>(std::clamp(static_cast<std::ptrdiff_t>(at) + d, std::ptrdiff_t{0}, n - 1));
            moved = true;
        };
        for (u32 k = 0; k < m_->input_.key_count_; ++k) {
            const key_event& ev = m_->input_.keys_[k];
            if (ev.ctrl) { continue; }
            switch (ev.k) {
            case key::down:
                if (found) { step(1); } else { at = 0; moved = true; found = true; }
                break;
            case key::up:
                if (found) { step(-1); } else { at = items.size() - 1; moved = true; found = true; }
                break;
            case key::page_down: step(10); break;
            case key::page_up:   step(-10); break;
            case key::home:      at = 0; moved = true; found = true; break;
            case key::end:       at = items.size() - 1; moved = true; found = true; break;
            case key::enter:     if (found) { activate = true; } break;
            case key::right:
                if (found && items[at].node) {
                    if (!items[at].open) { tree_open_set(items[at].key, true); moved = true; }
                    else                 { step(1); }
                }
                break;
            case key::left:
                if (found && items[at].node && items[at].open) {
                    tree_open_set(items[at].key, false);
                    moved = true;
                } else if (found && items[at].depth > 0) {
                    // step out: the nearest row above that is one level shallower
                    const u32 want = items[at].depth - 1;
                    for (std::size_t i = at; i-- > 0;) {
                        if (items[i].depth == want) { at = i; moved = true; break; }
                    }
                }
                break;
            default: break;
            }
        }
    }

    if (moved || (activate && found)) {
        m_->nav_.cursor = items[at].key;
    }
    if (activate) {
        m_->nav_.activate_pending = m_->nav_.cursor;
    }
    if (moved) { // keep the row the cursor moved to in view
        scroll_reveal_rect(items[at].bounds, false);
    }
    m_->nav_.scope = 0;
}

// scrolling -----------------------------------------------------------------------------------

// scroll offset and visible rect of the innermost scrolling region (child being built, else window); nullptr if none.
f32* context::scroll_slot(rect& view) noexcept
{
    if (m_->children_cards_.child_depth_ > m_->child_base_) {
        child_frame& cf = m_->children_cards_.child_stack_[m_->children_cards_.child_depth_ - 1];
        if (cf.state == nullptr) { return nullptr; }
        view = cf.inner;
        return &cf.state->scroll;
    }
    if (m_->cur_ == nullptr) {
        return nullptr;
    }
    const f32 shown_h = m_->cur_->height > 0.0f ? m_->cur_->height : m_->cur_->capped_h;
    if (shown_h <= 0.0f) {
        return nullptr;
    }
    const f32 pad = m_->cur_->menubar ? 0.0f : m_->style_.padding;
    view = {{m_->cur_->pos.x, m_->cur_->pos.y + m_->cur_->title_h + pad}, {m_->cur_->pos.x + m_->cur_->width, m_->cur_->pos.y + shown_h - pad}};
    return &m_->cur_->scroll;
}

f32 context::scroll_y() const noexcept
{
    context& self = *const_cast<context*>(this); // the lookup only reads; the slot it finds is what is mutable
    rect     view;
    const f32* scroll = self.scroll_slot(view);
    return scroll != nullptr ? *scroll : 0.0f;
}

f32 context::scroll_max_y() const noexcept
{
    context& self = *const_cast<context*>(this);
    rect     view;
    if (self.scroll_slot(view) == nullptr) {
        return 0.0f;
    }
    // last frame's value: this frame's content is not finished yet
    const f32 content = m_->children_cards_.child_depth_ > m_->child_base_ ? m_->children_cards_.child_stack_[m_->children_cards_.child_depth_ - 1].state->content_h
                                         : m_->cur_->content_h + 2.0f * (m_->cur_->menubar ? 0.0f : m_->style_.padding);
    return std::max(0.0f, content - view.height());
}

void context::set_scroll_y(f32 y) noexcept
{
    rect view;
    if (f32* scroll = scroll_slot(view); scroll != nullptr) {
        *scroll = std::max(0.0f, y); // the owner clamps against this frame's content height
    }
}

// only child_flags::horizontal children scroll sideways: the innermost one owns the x offset
context::child_state* context::horizontal_child() const noexcept
{
    for (u32 d = m_->children_cards_.child_depth_; d-- > m_->child_base_;) {
        if (has_flag(m_->children_cards_.child_stack_[d].flags, child_flags::horizontal)) {
            return m_->children_cards_.child_stack_[d].state;
        }
    }
    return nullptr;
}

f32 context::scroll_x() const noexcept
{
    const child_state* st = horizontal_child();
    return st != nullptr ? st->scroll_x : 0.0f;
}

f32 context::scroll_max_x() const noexcept
{
    const child_state* st = horizontal_child();
    if (st == nullptr) {
        return 0.0f;
    }
    // last frame's value, like scroll_max_y
    for (u32 d = m_->children_cards_.child_depth_; d-- > m_->child_base_;) {
        if (m_->children_cards_.child_stack_[d].state == st) {
            return std::max(0.0f, st->content_w - m_->children_cards_.child_stack_[d].inner.width());
        }
    }
    return 0.0f;
}

void context::set_scroll_x(f32 x) noexcept
{
    if (child_state* st = horizontal_child(); st != nullptr) {
        st->scroll_x = std::max(0.0f, x); // end_child clamps against this frame's content width
    }
}

// scrolls minimally (or centres). this frame is laid out already, so the next frame shows it -- hence callers
// revealing a selection call it every frame
void context::scroll_reveal_rect(const rect& item, bool center) noexcept
{
    rect view;
    f32* scroll = scroll_slot(view);
    if (scroll == nullptr || view.height() <= 0.0f) {
        return;
    }
    f32 delta = 0.0f;
    if (center) {
        delta = item.center().y - view.center().y;
    } else if (item.min.y < view.min.y) {
        delta = item.min.y - view.min.y;
    } else if (item.max.y > view.max.y) {
        delta = item.max.y - view.max.y;
    }
    if (delta != 0.0f) {
        *scroll = std::max(0.0f, *scroll + delta);
    }
}

void context::ensure_item_visible() noexcept
{
    scroll_reveal_rect(m_->last_item_rect_, false);
}

void context::scroll_to_item() noexcept
{
    scroll_reveal_rect(m_->last_item_rect_, true);
}

void context::list_clip_begin(int count, f32 item_height, int& first, int& last)
{
    first = last = 0;
    if (m_->cur_ == nullptr || count <= 0) {
        return;
    }
    f32 pitch;
    f32 y0;
    if (m_->tree_table_.table_.active) {
        table_finalize_columns();
        if (m_->tree_table_.table_.in_row) { table_finish_row(); }
        table_start_body();
        pitch = std::max(m_->tree_table_.table_.state->row_hint, m_->tree_table_.table_.min_row_h);
        y0    = m_->tree_table_.table_.row_y;
    } else {
        pitch = (item_height > 0.0f ? item_height : m_->font_.line_height(current_font())) + m_->style_.item_spacing;
        y0    = layout_next_y();
    }
    const rect view = m_->dl_.clip();
    first = std::clamp(static_cast<int>(std::floor((view.min.y - y0) / pitch)) - 1, 0, count);
    last  = std::clamp(static_cast<int>(std::ceil((view.max.y - y0) / pitch)) + 1, first, count);
    if (first > 0) {
        if (m_->tree_table_.table_.active) { table_skip_rows(first); }
        else               { (void)layout_place({0.0f, static_cast<f32>(first) * pitch - m_->style_.item_spacing}); }
    }
}

void context::list_clip_end(int count, f32 item_height, int last)
{
    if (m_->cur_ == nullptr || last >= count) {
        return;
    }
    const int rest = count - last;
    if (m_->tree_table_.table_.active) {
        table_skip_rows(rest);
    } else {
        const f32 pitch = (item_height > 0.0f ? item_height : m_->font_.line_height(current_font())) + m_->style_.item_spacing;
        (void)layout_place({0.0f, static_cast<f32>(rest) * pitch - m_->style_.item_spacing});
    }
}

} // namespace strata

// child regions and cards

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

child_scope::~child_scope()
{
    if (open_) { ctx_->end_child(); }
}

card_scope::~card_scope()
{
    if (open_) { ctx_->end_card(); }
}

bool context::begin_child(std::string_view id_label, vec2 size, child_flags flags)
{
    if (m_->cur_ != nullptr && m_->children_cards_.child_depth_ >= max_child_depth) {
        report_limit("child regions inside child regions (max_child_depth)", max_child_depth);
    }
    if (m_->cur_ == nullptr || m_->children_cards_.child_depth_ >= max_child_depth) {
        return false;
    }
    const id key = widget_id(id_label);
    bool full = false;
    child_state* st = internal::state_for(std::span{m_->children_cards_.children_}, key, m_->frame_, full);
    if (full) { report_limit("child regions with state (context_config::capacity.children)", static_cast<u32>(m_->children_cards_.children_.size())); }
    if (st->content_h > 0.0f) { apply_pending_scroll(key, st->scroll, &st->scroll_x); } // (once its content has been measured)

    // width: rest of the line; height: down to the window bottom
    f32 w = size.x;
    if (w <= 0.0f) {
        w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
        if (m_->layout_.same_line && !m_->layout_.first) {
            w = m_->layout_.origin.x + m_->layout_.width - (m_->layout_.cursor_x + m_->style_.item_spacing);
        }
    }
    m_->layout_.next_width = 0.0f;
    f32 h = size.y;
    if (h <= 0.0f) {
        h = m_->layout_.bound_bottom > 0.0f ? m_->layout_.bound_bottom - layout_next_y() : 240.0f;
    }
    w = std::max(w, 24.0f);
    h = std::max(h, 24.0f);

    const rect r = layout_place({w, h});

    if (has_flag(flags, child_flags::acrylic)) {
        m_->dl_.backdrop(r, m_->style_.blur_radius, darken(m_->style_.window_bg, 0.25f).scaled_alpha(m_->style_.acrylic_alpha * 0.8f),
                     radii(m_->style_.rounding * 0.8f), m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);
    }
    if (has_flag(flags, child_flags::frame)) {
        shape_style bg;
        bg.radius       = radii(m_->style_.rounding * 0.8f);
        bg.fill_top     = darken(m_->style_.widget_bg, 0.30f).scaled_alpha(0.55f);
        bg.fill_bottom  = bg.fill_top;
        bg.border       = m_->style_.border;
        bg.border_width = m_->style_.border_width;
        m_->dl_.shape(r, bg);
    }

    const f32  pad   = has_flag(flags, child_flags::no_padding) ? 0.0f : m_->style_.padding * 0.7f;
    const rect inner = {{r.min.x + pad, r.min.y + pad}, {r.max.x - pad, r.max.y - pad}};

    const f32 max_scroll = std::max(0.0f, st->content_h - inner.height());
    st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);

    const bool horiz = has_flag(flags, child_flags::horizontal);
    const bool bars  = !has_flag(flags, child_flags::no_scrollbar);
    // the horizontal bar takes a strip off the bottom, shortening the vertical one (both only when scrollable)
    const f32 hbar = horiz && st->overflow_x && bars ? 10.0f : 0.0f;
    if (!horiz) {
        st->scroll_x = 0.0f;
    } else {
        st->scroll_x = std::clamp(st->scroll_x, 0.0f, std::max(0.0f, st->content_w - inner.width()));
    }

    m_->dl_.push_clip({{r.min.x + 1.0f, r.min.y + 1.0f}, {r.max.x - 1.0f, r.max.y - 1.0f}});

    m_->children_cards_.child_stack_[m_->children_cards_.child_depth_++] = {st, r, inner, m_->layout_, flags};
    push_id(id_label);

    m_->layout_              = {};
    m_->layout_.origin       = {inner.min.x - st->scroll_x, inner.min.y - st->scroll};
    m_->layout_.width        = std::max(inner.width() - (st->overflow && bars ? 10.0f : 0.0f), 1.0f);
    m_->layout_.bound_bottom = inner.max.y - hbar;
    return true;
}

void context::end_child()
{
    if (m_->children_cards_.child_depth_ <= m_->child_base_) {
        return;
    }
    const child_frame f = m_->children_cards_.child_stack_[--m_->children_cards_.child_depth_];
    child_state& st     = *f.state;

    const bool horiz = has_flag(f.flags, child_flags::horizontal);
    const bool bars  = !has_flag(f.flags, child_flags::no_scrollbar);

    // content width: layout_.right minus origin.x (which already includes the scroll offset)
    if (horiz) {
        st.content_w  = m_->layout_.first ? 0.0f : m_->layout_.right - m_->layout_.origin.x;
        st.overflow_x = st.content_w > f.inner.width() + 0.5f;
    } else {
        st.content_w  = 0.0f;
        st.overflow_x = false;
    }
    const f32 hbar = st.overflow_x && bars ? 10.0f : 0.0f;

    st.content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    const f32 view_h = f.inner.height() - hbar;
    st.overflow = st.content_h > view_h + 0.5f;

    if (st.overflow_x) {
        const f32 max_x = st.content_w - f.inner.width();
        // tilt wheel, or Shift + wheel (the Windows convention)
        const bool over = pointer_over(f.bounds);
        if (m_->input_.wheel_x_ != 0.0f && !m_->wheel_x_consumed_ && over) {
            st.scroll_x = std::clamp(st.scroll_x + wheel_scroll_x(m_->font_.line_height(0), f.inner.width()), 0.0f, max_x);
            m_->wheel_x_consumed_ = true;
        } else if (m_->input_.mod_shift_ && m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && over) {
            st.scroll_x = std::clamp(st.scroll_x - wheel_scroll(m_->font_.line_height(0), f.inner.width()), 0.0f, max_x);
            m_->wheel_consumed_ = true;
        }
        if (bars) {
            const f32 track_x0 = f.bounds.min.x + 5.0f;
            const f32 track_w  = f.bounds.width() - 10.0f - (st.overflow ? 10.0f : 0.0f);
            const f32 thumb_w  = std::max(20.0f, track_w * f.inner.width() / st.content_w);
            const f32 y0       = f.bounds.max.y - 9.0f;
            f32 thumb_x = track_x0 + (track_w - thumb_w) * (st.scroll_x / max_x);
            const interaction in = interact(part_id(part::child_scrollbar_x, current_seed()), {{track_x0, y0 - 3.0f}, {track_x0 + track_w, y0 + 8.0f}});
            st.scroll_x = thumb_drag_x(in, st.grab_x, thumb_x, thumb_w, track_x0, track_w - thumb_w, max_x, st.scroll_x);
            thumb_x     = track_x0 + (track_w - thumb_w) * (st.scroll_x / max_x);
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.4f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape({{thumb_x, y0}, {thumb_x + thumb_w, f.bounds.max.y - 4.0f}}, bar);
        }
    } else {
        st.scroll_x = 0.0f;
    }

    if (st.overflow) {
        const f32 max_scroll = st.content_h - view_h;
        if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(f.bounds)) {
            st.scroll = std::clamp(st.scroll - wheel_scroll(m_->font_.line_height(0), view_h), 0.0f, max_scroll);
            m_->wheel_consumed_ = true;
        }
        if (bars) {
            const f32  track_top = f.bounds.min.y + 5.0f;
            const f32  track_h = f.bounds.height() - 10.0f - hbar; // room for the horizontal bar, when there is one
            const f32  thumb_h = std::max(20.0f, track_h * view_h / st.content_h);
            const f32  x0      = f.bounds.max.x - 9.0f;
            // the whole track takes the press: clicking beside the thumb jumps there, dragging works from anywhere
            f32 thumb_y = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const interaction in = interact(part_id(part::child_scrollbar, current_seed()), {{x0 - 3.0f, track_top}, {x0 + 8.0f, track_top + track_h}});
            st.scroll = thumb_drag(in, st.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, st.scroll);
            thumb_y   = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const rect thumb = {{x0, thumb_y}, {f.bounds.max.x - 4.0f, thumb_y + thumb_h}};
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.4f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape(thumb, bar);
        }
    } else {
        st.scroll = 0.0f;
    }

    m_->dl_.pop_clip();
    m_->layout_ = f.outer;
    pop_id();
}

bool context::begin_card(std::string_view title, std::string_view icon, font_id icon_font)
{
    if (m_->cur_ != nullptr && m_->children_cards_.card_depth_ >= max_card_depth) {
        report_limit("cards inside cards (max_card_depth)", max_card_depth);
    }
    if (m_->cur_ == nullptr || m_->children_cards_.card_depth_ >= max_card_depth) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(title);
    bool full = false;
    card_state* st  = internal::state_for(std::span{m_->children_cards_.cards_}, key, m_->frame_, full);
    if (full) { report_limit("cards with state (context_config::capacity.cards)", static_cast<u32>(m_->children_cards_.cards_.size())); }

    const std::string_view shown = visible_label(title);
    const bool has_head = !shown.empty() || !icon.empty();
    const f32  lh       = m_->font_.line_height(f);
    const f32  head_h   = has_head ? lh + 16.0f : 0.0f;
    const f32  pad      = m_->style_.padding * 0.85f;

    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    // the height comes from last frame's content; the very first frame shows just the header
    const rect r = layout_place({w, head_h + pad + st->content_h + pad});

    shape_style bg;
    bg.radius       = radii(m_->style_.rounding);
    bg.fill_top     = lighten(m_->style_.widget_bg, m_->style_.gradient * 0.4f).scaled_alpha(0.42f);
    bg.fill_bottom  = darken(m_->style_.widget_bg, 0.15f).scaled_alpha(0.42f);
    bg.border       = m_->style_.border;
    bg.border_width = m_->style_.border_width;
    m_->dl_.shape(r, bg);

    if (has_head) {
        m_->dl_.rect_filled({{r.min.x + 1.0f, r.min.y + head_h}, {r.max.x - 1.0f, r.min.y + head_h + 1.0f}}, m_->style_.border.scaled_alpha(0.7f));
        shape_style mark; // accent tick before the title
        mark.radius      = radii(1.5f);
        mark.fill_top    = m_->style_.accent_hover;
        mark.fill_bottom = m_->style_.accent;
        m_->dl_.shape({{r.min.x + 10.0f, r.min.y + (head_h - lh * 0.75f) * 0.5f}, {r.min.x + 13.0f, r.min.y + (head_h + lh * 0.75f) * 0.5f}}, mark);

        f32 x = r.min.x + 20.0f;
        if (!icon.empty()) {
            const vec2 isize = m_->font_.measure(icon_font, icon);
            m_->dl_.text({x, r.min.y + (head_h - isize.y) * 0.5f}, m_->style_.accent_hover, icon, icon_font);
            x += isize.x + 8.0f;
        }
        const f32 title_h = shown.empty() ? lh : label_size(f, shown).y;
        label_draw({x, r.min.y + (head_h - title_h) * 0.5f}, m_->style_.text, shown, f);
    }

    m_->children_cards_.card_stack_[m_->children_cards_.card_depth_++] = {st, m_->layout_};
    push_id(title);
    m_->layout_              = {};
    m_->layout_.origin       = {r.min.x + pad, r.min.y + head_h + pad};
    m_->layout_.width        = std::max(w - 2.0f * pad, 1.0f);
    return true;
}

void context::end_card()
{
    if (m_->children_cards_.card_depth_ == 0) {
        return;
    }
    const card_frame f = m_->children_cards_.card_stack_[--m_->children_cards_.card_depth_];
    f.state->content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m_->layout_ = f.outer;
    pop_id();
}

} // namespace strata

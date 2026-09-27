// generic popups and drag and drop

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>

namespace strata {

popup_scope::~popup_scope()
{
    if (open_) {
        ctx_->end_popup();
    }
}

drag_source_scope::~drag_source_scope()
{
    if (active_) {
        ctx_->end_drag_source();
    }
}

// popups -----------------------------------------------------------------------------------

void context::open_popup(std::string_view label)
{
    m_->popup_id_      = widget_id(label);
    m_->popup_scroll_  = 0.0f;
    m_->popup_hover_   = -1;
    m_->gpopup_anchor_ = m_->last_item_rect_.width() > 0.0f ? m_->last_item_rect_ : rect{m_->mouse_, m_->mouse_};
    m_->gpopup_size_   = {};
    m_->gpopup_key_    = 0;
}

void context::open_popup(std::string_view label, vec2 pos)
{
    open_popup(label);
    m_->gpopup_anchor_ = {{pos.x, pos.y - 4.0f}, {pos.x, pos.y - 4.0f}}; // the panel sits 4 px below its anchor
}

void context::toggle_popup(std::string_view label)
{
    if (popup_is_open(label)) {
        m_->popup_id_ = 0;
    } else {
        open_popup(label);
    }
}

bool context::popup_is_open(std::string_view label) const noexcept
{
    return m_->popup_id_ != 0 && m_->popup_id_ == hash_id(label, m_->id_stack_[m_->id_depth_]);
}

bool context::begin_popup(std::string_view label, f32 width)
{
    const id key = widget_id(label);
    if (m_->popup_id_ != key) {
        return false;
    }
    const f32  w        = width > 0.0f ? width : std::max(m_->gpopup_anchor_.width(), 180.0f);
    const bool measured = m_->gpopup_key_ == key && m_->gpopup_size_.y > 0.0f;
    const f32  h        = measured ? std::min(m_->gpopup_size_.y, m_->display_.y - 16.0f) : 40.0f;

    m_->gpopup_hidden_ = !measured; // the first frame only finds out how tall the content is
    if (m_->gpopup_hidden_) { m_->dl_.push_alpha(0.0f); }
    if (!begin_popup_at(key, m_->gpopup_anchor_, {w, h})) {
        if (m_->gpopup_hidden_) { m_->dl_.pop_alpha(); }
        return false;
    }
    push_id(label);
    return true;
}

void context::end_popup()
{
    pop_id();
    const f32 pad       = m_->style_.padding;
    const f32 content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m_->gpopup_size_ = {m_->layout_.width + 2.0f * pad, content_h + 2.0f * pad};
    m_->gpopup_key_  = m_->popup_id_;
    end_popup_at();
    if (m_->gpopup_hidden_) { m_->dl_.pop_alpha(); }
}

// drag and drop --------------------------------------------------------------------------

bool context::begin_drag_source()
{
    if (m_->cur_ == nullptr || m_->last_item_key_ == 0) {
        return false;
    }
    const id src = m_->last_item_key_;
    if (!m_->dd_active_ && !m_->dd_cancelled_) {
        if (m_->active_ == src && m_->mouse_pressed_) {
            m_->dd_candidate_ = src;
            m_->dd_press_pos_ = m_->mouse_;
        }
        if (m_->dd_candidate_ == src && m_->active_ == src && m_->mouse_down_) {
            const vec2 d = m_->mouse_ - m_->dd_press_pos_;
            if (d.x * d.x + d.y * d.y > 25.0f) { // a few pixels of travel: a click is not a drag
                m_->dd_active_ = true;
                m_->dd_source_ = src;
                m_->dd_type_.clear();
                m_->dd_data_.clear();
            }
        }
    }
    if (!m_->dd_active_ || m_->dd_source_ != src) {
        return false;
    }
    for (u32 i = 0; i < m_->key_count_; ++i) {
        if (m_->keys_[i].k == key::escape) {
            m_->dd_active_    = false;
            m_->dd_cancelled_ = true;
            return false;
        }
    }
    if (m_->mouse_released_) {
        return false; // the drop frame: the payload stays for the targets, the preview is gone
    }
    m_->cursor_ = cursor_kind::arrow;

    // the drag preview: an overlay panel following the pointer, sized by last frame's content
    m_->dd_saved_overlay_ = m_->in_overlay_;
    m_->dd_saved_layout_  = m_->layout_;
    m_->dd_prev_owner_    = m_->run_owner_;
    switch_run(run_overlay);
    m_->in_overlay_ = true;
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});

    const bool measured = m_->dd_size_.y > 0.0f;
    m_->dd_hidden_ = !measured;
    if (m_->dd_hidden_) { m_->dl_.push_alpha(0.0f); }

    const vec2 size = measured ? m_->dd_size_ : vec2{40.0f, 24.0f};
    vec2 pos = m_->mouse_ + vec2{16.0f, 18.0f};
    pos.x = std::max(4.0f, std::min(pos.x, m_->display_.x - size.x - 4.0f));
    pos.y = std::max(4.0f, std::min(pos.y, m_->display_.y - size.y - 4.0f));
    const rect panel = rect::from_size(pos, size);

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.7f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 235};
    body.fill_bottom   = body.fill_top;
    body.border        = m_->style_.accent.scaled_alpha(0.7f);
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.6f;
    body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.25f};
    m_->dl_.shape(panel, body);
    m_->dl_.push_clip(panel);

    m_->layout_        = {};
    m_->layout_.origin = {panel.min.x + 8.0f, panel.min.y + 6.0f};
    m_->layout_.width  = 360.0f;
    return true;
}

void context::set_drag_payload(std::string_view type, const void* data, std::size_t size)
{
    if (!m_->dd_active_) {
        return;
    }
    m_->dd_type_.assign(type);
    m_->dd_data_.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
}

void context::end_drag_source()
{
    const f32 content_w = std::max(m_->layout_.right - m_->layout_.origin.x, 0.0f);
    const f32 content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m_->dd_size_ = {content_w + 16.0f, content_h + 12.0f};

    m_->layout_ = m_->dd_saved_layout_;
    m_->dl_.pop_clip();
    m_->dl_.pop_clip();
    if (m_->dd_hidden_) { m_->dl_.pop_alpha(); }
    m_->in_overlay_ = m_->dd_saved_overlay_;
    switch_run(m_->dd_prev_owner_);
}

drop_result context::drop_target(std::string_view type, drop_flags flags)
{
    drop_result r;
    if (m_->cur_ == nullptr || !m_->dd_active_ || m_->dd_cancelled_ || m_->dd_type_ != type) {
        return r;
    }
    const rect target = m_->last_item_rect_;
    if (!pointer_over(target)) {
        return r;
    }
    r.hovering = true;
    r.local    = {(m_->mouse_.x - target.min.x) / std::max(target.width(), 1.0f), (m_->mouse_.y - target.min.y) / std::max(target.height(), 1.0f)};
    if (!has_flag(flags, drop_flags::no_highlight)) {
        shape_style outline;
        outline.radius       = radii(m_->style_.rounding * 0.7f);
        outline.fill_top     = m_->style_.accent.scaled_alpha(0.10f);
        outline.fill_bottom  = outline.fill_top;
        outline.border       = m_->style_.accent;
        outline.border_width = 1.5f;
        m_->dl_.shape(target, outline);
    }
    if (m_->mouse_released_) {
        r.dropped = true;
        r.data    = m_->dd_data_;
    }
    return r;
}

} // namespace strata

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

bool context::popup_push(id key) noexcept
{
    // above the popup being drawn; a popup already in the stack reopens at its own level
    const u32 drawing = m_->popup_.popup_drawing();
    u32 level = drawing == popup_stack::no_popup ? 0u : drawing + 1;
    const u32 existing = m_->popup_.popup_level_of(key);
    if (existing != popup_stack::no_popup && existing < level) {
        level = existing;
    }
    if (level >= popup_stack::max_popup_levels) {
        report_limit("nested popups (max_popup_levels)", popup_stack::max_popup_levels);
        return false;
    }
    m_->popup_.popup_close_from(level);
    m_->popup_.popups_[level].key = key;
    m_->popup_.popup_count_       = level + 1;
    return true;
}

void context::popup_enter(u32 level, const rect& r, const rect& anchor) noexcept
{
    if (level < m_->popup_.popup_count_) {
        internal::popup_level& p = m_->popup_.popups_[level];
        p.open_cur   = true;
        p.rect_cur   = r;
        p.anchor_cur = anchor;
    }
    ++m_->popup_.popup_depth_;
    internal::popup_frame& f = m_->popup_.popup_frame_top();
    f.level         = level;
    f.prev_owner    = m_->run_owner_;
    f.saved_overlay = m_->in_overlay_;
    f.outer_hidden  = false;
    f.saved_layout  = m_->layout_;
    switch_run(popup_stack::popup_run(level));
    m_->in_overlay_ = true;
}

void context::popup_leave() noexcept
{
    if (m_->popup_.popup_depth_ == 0) {
        return;
    }
    const internal::popup_frame f = m_->popup_.popup_frame_top();
    --m_->popup_.popup_depth_;
    m_->layout_     = f.saved_layout;
    m_->in_overlay_ = f.saved_overlay;
    switch_run(f.prev_owner);
}

void context::open_popup(std::string_view label)
{
    if (!popup_push(widget_id(label))) {
        return;
    }
    internal::popup_level& p = m_->popup_.popups_[m_->popup_.popup_count_ - 1];
    p.opener   = m_->last_item_rect_.width() > 0.0f ? m_->last_item_rect_ : rect{m_->input_.mouse_, m_->input_.mouse_};
    p.size     = {};
    p.measured = 0;
    m_->popup_.popup_scroll_ = 0.0f;
    m_->popup_.popup_hover_  = -1;
}

void context::open_popup(std::string_view label, vec2 pos)
{
    open_popup(label);
    const u32 level = m_->popup_.popup_level_of(hash_id(label, m_->ids_.current()));
    if (level != popup_stack::no_popup) {
        m_->popup_.popups_[level].opener = {{pos.x, pos.y - 4.0f}, {pos.x, pos.y - 4.0f}}; // the panel sits 4 px below its anchor
    }
}

void context::toggle_popup(std::string_view label)
{
    if (popup_open(label)) {
        m_->popup_.popup_close(hash_id(label, m_->ids_.current()));
    } else {
        open_popup(label);
    }
}

bool context::popup_open(std::string_view label) const noexcept
{
    return m_->popup_.popup_has(hash_id(label, m_->ids_.current()));
}

bool context::begin_popup(std::string_view label, f32 width)
{
    const id  key   = widget_id(label);
    const u32 level = m_->popup_.popup_level_of(key);
    if (level == popup_stack::no_popup) {
        return false;
    }
    const internal::popup_level& p = m_->popup_.popups_[level];
    const f32  w        = width > 0.0f ? width : std::max(p.opener.width(), 180.0f);
    const bool measured = p.measured == key && p.size.y > 0.0f;
    const f32  h        = measured ? std::min(p.size.y, m_->display_.y - 16.0f) : 40.0f;

    // the first frame only finds out how tall the content is (end_popup puts back the flag of a popup around this one)
    const bool outer_hidden = m_->gpopup_hidden_;
    m_->gpopup_hidden_ = !measured;
    if (m_->gpopup_hidden_) { m_->dl_.push_alpha(0.0f); }
    if (!begin_popup_at(key, p.opener, {w, h})) {
        if (m_->gpopup_hidden_) { m_->dl_.pop_alpha(); }
        m_->gpopup_hidden_ = outer_hidden;
        return false;
    }
    m_->popup_.popup_frame_top().outer_hidden = outer_hidden;
    push_id(label);
    return true;
}

void context::end_popup()
{
    pop_id();
    const f32 pad       = m_->style_.padding;
    const f32 content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    const u32 level     = m_->popup_.popup_drawing();
    if (level < m_->popup_.popup_count_) { // (still open: close_popup() from inside empties the level)
        internal::popup_level& p = m_->popup_.popups_[level];
        p.size     = {m_->layout_.width + 2.0f * pad, content_h + 2.0f * pad};
        p.measured = p.key;
    }
    const bool outer_hidden = m_->popup_.popup_depth_ > 0 && m_->popup_.popup_frame_top().outer_hidden;
    end_popup_at();
    if (m_->gpopup_hidden_) { m_->dl_.pop_alpha(); }
    m_->gpopup_hidden_ = outer_hidden;
}

// drag and drop --------------------------------------------------------------------------

bool context::begin_drag_source()
{
    if (m_->cur_ == nullptr || m_->last_item_key_ == 0) {
        return false;
    }
    const id src = m_->last_item_key_;
    if (!m_->dnd_.dd_active_ && !m_->dnd_.dd_cancelled_) {
        if (m_->active_ == src && m_->input_.mouse_pressed_) {
            m_->dnd_.dd_candidate_ = src;
            m_->dnd_.dd_press_pos_ = m_->input_.mouse_;
        }
        if (m_->dnd_.dd_candidate_ == src && m_->active_ == src && m_->input_.mouse_down_) {
            const vec2 d = m_->input_.mouse_ - m_->dnd_.dd_press_pos_;
            if (d.x * d.x + d.y * d.y > 25.0f) { // a few pixels of travel: a click is not a drag
                m_->dnd_.dd_active_ = true;
                m_->dnd_.dd_source_ = src;
                m_->dnd_.dd_type_.clear();
                m_->dnd_.dd_data_.clear();
            }
        }
    }
    if (!m_->dnd_.dd_active_ || m_->dnd_.dd_source_ != src) {
        return false;
    }
    for (u32 i = 0; i < m_->input_.key_count_; ++i) {
        if (m_->input_.keys_[i].k == key::escape) {
            m_->dnd_.dd_active_    = false;
            m_->dnd_.dd_cancelled_ = true;
            return false;
        }
    }
    if (m_->input_.mouse_released_) {
        return false; // the drop frame: the payload stays for the targets, the preview is gone
    }
    m_->cursor_ = cursor_kind::arrow;

    // the drag preview: an overlay panel following the pointer, sized by last frame's content
    m_->dnd_.dd_saved_overlay_ = m_->in_overlay_;
    m_->dnd_.dd_saved_layout_  = m_->layout_;
    m_->dnd_.dd_prev_owner_    = m_->run_owner_;
    switch_run(m_->popup_.overlay_run());
    m_->in_overlay_ = true;
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});

    const bool measured = m_->dnd_.dd_size_.y > 0.0f;
    m_->dnd_.dd_hidden_ = !measured;
    if (m_->dnd_.dd_hidden_) { m_->dl_.push_alpha(0.0f); }

    const vec2 size = measured ? m_->dnd_.dd_size_ : vec2{40.0f, 24.0f};
    vec2 pos = m_->input_.mouse_ + vec2{16.0f, 18.0f};
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
    if (!m_->dnd_.dd_active_) {
        return;
    }
    m_->dnd_.dd_type_.assign(type);
    m_->dnd_.dd_data_.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
}

void context::end_drag_source()
{
    const f32 content_w = std::max(m_->layout_.right - m_->layout_.origin.x, 0.0f);
    const f32 content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m_->dnd_.dd_size_ = {content_w + 16.0f, content_h + 12.0f};

    m_->layout_ = m_->dnd_.dd_saved_layout_;
    m_->dl_.pop_clip();
    m_->dl_.pop_clip();
    if (m_->dnd_.dd_hidden_) { m_->dl_.pop_alpha(); }
    m_->in_overlay_ = m_->dnd_.dd_saved_overlay_;
    switch_run(m_->dnd_.dd_prev_owner_);
}

drop_result context::drop_target(std::string_view type, drop_flags flags)
{
    drop_result r;
    if (m_->cur_ == nullptr || !m_->dnd_.dd_active_ || m_->dnd_.dd_cancelled_ || m_->dnd_.dd_type_ != type) {
        return r;
    }
    const rect target = m_->last_item_rect_;
    if (!pointer_over(target)) {
        return r;
    }
    r.hovering = true;
    r.local    = {(m_->input_.mouse_.x - target.min.x) / std::max(target.width(), 1.0f), (m_->input_.mouse_.y - target.min.y) / std::max(target.height(), 1.0f)};
    if (!has_flag(flags, drop_flags::no_highlight)) {
        shape_style outline;
        outline.radius       = radii(m_->style_.rounding * 0.7f);
        outline.fill_top     = m_->style_.accent.scaled_alpha(0.10f);
        outline.fill_bottom  = outline.fill_top;
        outline.border       = m_->style_.accent;
        outline.border_width = 1.5f;
        m_->dl_.shape(target, outline);
    }
    if (m_->input_.mouse_released_) {
        r.dropped = true;
        r.data    = m_->dnd_.dd_data_;
    }
    return r;
}

bool context::pointer_over(const rect& r) const noexcept
{
    const bool in_window = m_->in_overlay_ || (m_->cur_window_ != 0 && m_->win_.hovered_window_prev_ == m_->cur_window_);
    return in_window && !m_->pointer_blocked() && m_->dl_.clip().contains(m_->input_.mouse_) && r.contains(m_->input_.mouse_);
}

// draws the popup frame in its overlay layer and redirects layout into it until end_popup(). false while closed.
bool context::begin_popup_at(id key, const rect& anchor, vec2 size)
{
    const u32 level = m_->popup_.popup_level_of(key);
    if (level == popup_stack::no_popup) {
        return false;
    }
    // Esc closes the top level only, and not while a text field or a menu (opened from here) has the keyboard
    if (level + 1 == m_->popup_.popup_count_ && m_->focus_id_ == 0 && m_->menu_.menu_open_[0].key == 0 && !m_->popup_.popup_esc_used_) {
        for (u32 i = 0; i < m_->input_.key_count_; ++i) {
            if (m_->input_.keys_[i].k == key::escape) {
                m_->popup_.popup_close_from(level);
                m_->popup_.popup_esc_used_ = true;
                return false;
            }
        }
    }

    rect list = {{anchor.min.x, anchor.max.y + 4.0f}, {anchor.min.x + size.x, anchor.max.y + 4.0f + size.y}};
    if (list.max.y > m_->display_.y - 4.0f && anchor.min.y - 4.0f - size.y >= 4.0f) {
        list = {{anchor.min.x, anchor.min.y - 4.0f - size.y}, {anchor.min.x + size.x, anchor.min.y - 4.0f}};
    }
    if (list.max.x > m_->display_.x - 4.0f) { // keep it on screen horizontally
        const f32 shift = list.max.x - (m_->display_.x - 4.0f);
        list = {{list.min.x - shift, list.min.y}, {list.max.x - shift, list.max.y}};
    }

    popup_enter(level, list, anchor);
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.8f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom   = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.border        = m_->style_.border;
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.8f;
    body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.3f};
    popup_panel(list, body);

    m_->dl_.push_clip({{list.min.x + 1.0f, list.min.y + 1.0f}, {list.max.x - 1.0f, list.max.y - 1.0f}});

    m_->layout_        = {};
    m_->layout_.origin = {list.min.x + m_->style_.padding, list.min.y + m_->style_.padding};
    m_->layout_.width  = size.x - 2.0f * m_->style_.padding;
    return true;
}

void context::end_popup_at()
{
    m_->dl_.pop_clip();
    m_->dl_.pop_clip();
    popup_leave(); // (restores the layout)
}

void context::popup_panel(const rect& r, const shape_style& body)
{
    const f32 k = std::clamp(m_->style_.popup_acrylic, 0.0f, 1.0f);
    if (k <= 0.0f || m_->dl_.alpha() < 0.99f) { // (a popup that is still fading in is drawn opaque: the blur has no fade)
        m_->dl_.shape(r, body);
        return;
    }
    const color clear{0, 0, 0, 0};
    shape_style shadow_only = body; // the shadow first, the glass over it, then the border on top
    shadow_only.fill_top = shadow_only.fill_bottom = clear;
    shadow_only.border = clear;
    shadow_only.border_width = 0.0f;
    m_->dl_.shape(r, shadow_only);

    const color tint = body.fill_top.scaled_alpha(1.0f + (m_->style_.acrylic_alpha - 1.0f) * k);
    m_->dl_.backdrop(r, m_->style_.blur_radius, tint, body.radius, m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);

    shape_style edge = body;
    edge.fill_top = edge.fill_bottom = clear;
    edge.shadow = clear;
    edge.shadow_blur = 0.0f;
    m_->dl_.shape(r, edge);
}

void context::draw_tooltip(std::string_view text)
{
    if (m_->in_overlay_ || text.empty()) {
        return;
    }
    const font_id f  = current_font();
    const vec2 ts    = label_size(f, text);
    const f32  padx  = 9.0f;
    const f32  pady  = 6.0f;
    const vec2 size{ts.x + 2.0f * padx, ts.y + 2.0f * pady};

    vec2 pos = m_->input_.mouse_ + vec2{14.0f, 20.0f};
    if (pos.x + size.x > m_->display_.x - 4.0f) { pos.x = m_->display_.x - 4.0f - size.x; }
    if (pos.y + size.y > m_->display_.y - 4.0f) { pos.y = m_->input_.mouse_.y - size.y - 10.0f; }
    pos.x = std::max(pos.x, 4.0f);
    pos.y = std::max(pos.y, 4.0f);

    const f32 fade = std::clamp((m_->hover_time_ - 0.35f) / 0.12f, 0.0f, 1.0f);
    const u32 previous_owner = m_->run_owner_;
    switch_run(m_->popup_.overlay_run());
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
    m_->dl_.push_alpha(fade);

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.6f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = m_->style_.border;
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.5f;
    body.shadow_offset = {0.0f, 3.0f};
    popup_panel(rect::from_size(pos, size), body);
    label_draw({pos.x + padx, pos.y + pady}, m_->style_.text, text, f);

    m_->dl_.pop_alpha();
    m_->dl_.pop_clip();
    switch_run(previous_owner);
}

void context::tooltip(std::string_view text)
{
    if (m_->last_item_hovered_ && m_->last_item_key_ == m_->hover_key_cur_ && m_->hover_time_ > 0.4f) {
        draw_tooltip(text);
    }
}

} // namespace strata

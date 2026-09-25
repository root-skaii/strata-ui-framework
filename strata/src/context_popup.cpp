// generic popups and drag and drop

#include "strata/context.hpp"

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
    popup_id_      = hash_id(label, current_seed());
    popup_scroll_  = 0.0f;
    popup_hover_   = -1;
    gpopup_anchor_ = last_item_rect_.width() > 0.0f ? last_item_rect_ : rect{mouse_, mouse_};
    gpopup_size_   = {};
    gpopup_key_    = 0;
}

void context::open_popup(std::string_view label, vec2 pos)
{
    open_popup(label);
    gpopup_anchor_ = {{pos.x, pos.y - 4.0f}, {pos.x, pos.y - 4.0f}}; // the panel sits 4 px below its anchor
}

void context::toggle_popup(std::string_view label)
{
    if (popup_is_open(label)) {
        popup_id_ = 0;
    } else {
        open_popup(label);
    }
}

bool context::popup_is_open(std::string_view label) const noexcept
{
    return popup_id_ != 0 && popup_id_ == hash_id(label, id_stack_[id_depth_]);
}

bool context::begin_popup(std::string_view label, f32 width)
{
    const id key = hash_id(label, current_seed());
    if (popup_id_ != key) {
        return false;
    }
    const f32  w        = width > 0.0f ? width : std::max(gpopup_anchor_.width(), 180.0f);
    const bool measured = gpopup_key_ == key && gpopup_size_.y > 0.0f;
    const f32  h        = measured ? std::min(gpopup_size_.y, display_.y - 16.0f) : 40.0f;

    gpopup_hidden_ = !measured; // the first frame only finds out how tall the content is
    if (gpopup_hidden_) { dl_.push_alpha(0.0f); }
    if (!begin_popup_at(key, gpopup_anchor_, {w, h})) {
        if (gpopup_hidden_) { dl_.pop_alpha(); }
        return false;
    }
    push_id(label);
    return true;
}

void context::end_popup()
{
    pop_id();
    const f32 pad       = style_.padding;
    const f32 content_h = layout_.first ? 0.0f : layout_.bottom - layout_.origin.y;
    gpopup_size_ = {layout_.width + 2.0f * pad, content_h + 2.0f * pad};
    gpopup_key_  = popup_id_;
    end_popup_at();
    if (gpopup_hidden_) { dl_.pop_alpha(); }
}

// drag and drop --------------------------------------------------------------------------

bool context::begin_drag_source()
{
    if (cur_ == nullptr || last_item_key_ == 0) {
        return false;
    }
    const id src = last_item_key_;
    if (!dd_active_ && !dd_cancelled_) {
        if (active_ == src && mouse_pressed_) {
            dd_candidate_ = src;
            dd_press_pos_ = mouse_;
        }
        if (dd_candidate_ == src && active_ == src && mouse_down_) {
            const vec2 d = mouse_ - dd_press_pos_;
            if (d.x * d.x + d.y * d.y > 25.0f) { // a few pixels of travel: a click is not a drag
                dd_active_ = true;
                dd_source_ = src;
                dd_type_.clear();
                dd_data_.clear();
            }
        }
    }
    if (!dd_active_ || dd_source_ != src) {
        return false;
    }
    for (u32 i = 0; i < key_count_; ++i) {
        if (keys_[i].k == key::escape) {
            dd_active_    = false;
            dd_cancelled_ = true;
            return false;
        }
    }
    if (mouse_released_) {
        return false; // the drop frame: the payload stays for the targets, the preview is gone
    }
    cursor_ = cursor_kind::arrow;

    // the preview: a small panel in the overlay layer that follows the pointer. its size is the content's of last frame
    dd_saved_overlay_ = in_overlay_;
    dd_saved_layout_  = layout_;
    dd_prev_owner_    = run_owner_;
    switch_run(run_overlay);
    in_overlay_ = true;
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});

    const bool measured = dd_size_.y > 0.0f;
    dd_hidden_ = !measured;
    if (dd_hidden_) { dl_.push_alpha(0.0f); }

    const vec2 size = measured ? dd_size_ : vec2{40.0f, 24.0f};
    vec2 pos = mouse_ + vec2{16.0f, 18.0f};
    pos.x = std::max(4.0f, std::min(pos.x, display_.x - size.x - 4.0f));
    pos.y = std::max(4.0f, std::min(pos.y, display_.y - size.y - 4.0f));
    const rect panel = rect::from_size(pos, size);

    shape_style body;
    body.radius        = radii(style_.rounding * 0.7f);
    body.fill_top      = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 235};
    body.fill_bottom   = body.fill_top;
    body.border        = style_.accent.scaled_alpha(0.7f);
    body.border_width  = style_.border_width;
    body.shadow        = style_.shadow;
    body.shadow_blur   = style_.shadow_blur * 0.6f;
    body.shadow_offset = {0.0f, style_.shadow_blur * 0.25f};
    dl_.shape(panel, body);
    dl_.push_clip(panel);

    layout_        = {};
    layout_.origin = {panel.min.x + 8.0f, panel.min.y + 6.0f};
    layout_.width  = 360.0f;
    return true;
}

void context::set_drag_payload(std::string_view type, const void* data, std::size_t size)
{
    if (!dd_active_) {
        return;
    }
    dd_type_.assign(type);
    dd_data_.assign(static_cast<const u8*>(data), static_cast<const u8*>(data) + size);
}

void context::end_drag_source()
{
    const f32 content_w = std::max(layout_.right - layout_.origin.x, 0.0f);
    const f32 content_h = layout_.first ? 0.0f : layout_.bottom - layout_.origin.y;
    dd_size_ = {content_w + 16.0f, content_h + 12.0f};

    layout_ = dd_saved_layout_;
    dl_.pop_clip();
    dl_.pop_clip();
    if (dd_hidden_) { dl_.pop_alpha(); }
    in_overlay_ = dd_saved_overlay_;
    switch_run(dd_prev_owner_);
}

drop_result context::drop_target(std::string_view type, drop_flags flags)
{
    drop_result r;
    if (cur_ == nullptr || !dd_active_ || dd_cancelled_ || dd_type_ != type) {
        return r;
    }
    const rect target = last_item_rect_;
    if (!pointer_over(target)) {
        return r;
    }
    r.hovering = true;
    r.local    = {(mouse_.x - target.min.x) / std::max(target.width(), 1.0f), (mouse_.y - target.min.y) / std::max(target.height(), 1.0f)};
    if (!has_flag(flags, drop_flags::no_highlight)) {
        shape_style outline;
        outline.radius       = radii(style_.rounding * 0.7f);
        outline.fill_top     = style_.accent.scaled_alpha(0.10f);
        outline.fill_bottom  = outline.fill_top;
        outline.border       = style_.accent;
        outline.border_width = 1.5f;
        dl_.shape(target, outline);
    }
    if (mouse_released_) {
        r.dropped = true;
        r.data    = dd_data_;
    }
    return r;
}

} // namespace strata

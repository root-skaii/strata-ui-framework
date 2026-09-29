// formerly inline members of strata::context that read its state, now in context::impl (context_impl.hpp)

#include "context_impl.hpp"

#include <cmath>

namespace strata {

draw_data context::render_data() const noexcept
{
    draw_data d = m_->dl_.data();
    d.content_hash = m_->geometry_hash_; // lets the renderers skip an upload of geometry they already hold
    d.text_contrast = m_->style_.text_contrast;
    return d;
}

bool context::want_capture_mouse() const noexcept
{
    return m_->win_.hovered_window_prev_ != 0 || m_->active_ != 0 || m_->dock_chrome_prev_ || m_->toast_.toast_hover_prev_ || m_->modal_.modal_count_ != 0 ||
           m_->menu_.menu_hit_prev_ || m_->popup_.popup_covers(m_->input_.mouse_);
}

bool context::want_text_input() const noexcept
{ return m_->focus_id_ != 0 || m_->hotkey_.hotkey_capture_ != 0; }

bool context::popup_open() const noexcept
{ return m_->popup_.popup_any_prev(); }

cursor_kind context::cursor() const noexcept
{ return m_->cursor_; }

bool context::input_submitted() const noexcept
{ return m_->submitted_; }

void context::set_clipboard(const clipboard_hooks& hooks) noexcept
{ m_->clipboard_ = hooks; }

void context::set_diagnostics(const diagnostics_hook& hook) noexcept
{ m_->diag_ = hook; }

f32 context::scale() const noexcept
{ return m_->scale_; }

u32 context::font_generation() const noexcept
{ return m_->font_generation_; }

int context::scale_percent() const noexcept
{ return static_cast<int>(m_->scale_ * 100.0f + 0.5f); }

void context::text(std::string_view s)
{ text_colored(m_->style_.text, s); }

void context::text_dim(std::string_view s)
{ text_colored(m_->style_.text_dim, s); }

void context::text_wrapped(std::string_view s)
{ text_wrapped_colored(m_->style_.text, s); }

void context::same_line() noexcept
{ m_->layout_.same_line = true; }

void context::same_line(f32 offset_x) noexcept
{
    m_->layout_.same_line = true;
    m_->layout_.cursor_x  = m_->layout_.origin.x + offset_x - m_->style_.item_spacing;
}

void context::same_line_right(f32 width) noexcept
{
    m_->layout_.same_line = true;
    m_->layout_.cursor_x  = m_->layout_.origin.x + m_->layout_.width + m_->layout_.gutter - width - m_->style_.item_spacing;
}

void context::set_next_item_width(f32 w) noexcept
{ m_->layout_.next_width = w; }

bool context::ime_wanted() const noexcept
{ return m_->ime_want_; }

vec2 context::ime_position() const noexcept
{ return m_->ime_pos_; }

f32 context::ime_line_height() const noexcept
{ return m_->ime_line_h_; }

bool context::ime_composing() const noexcept
{ return m_->input_.ime_len_ != 0; }

std::string_view context::ime_composition() const noexcept
{ return {m_->input_.ime_text_.data(), m_->input_.ime_len_}; }

bool context::item_pressed() const noexcept
{ return m_->item_pressed_; }

void context::set_next_item_gutter(f32 width) noexcept
{ m_->next_gutter_ = width; }

void context::set_next_item_open(bool open) noexcept
{ m_->next_open_ = open ? 1 : 2; }

void context::set_next_item_open_recursive(bool open) noexcept
{ m_->next_open_ = open ? 3 : 4; }

bool context::nav_active() const noexcept
{ return m_->nav_.active; }

bool context::item_focused() const noexcept
{ return m_->last_item_focused_ || (m_->focus_id_ != 0 && m_->focus_id_ == m_->last_item_key_); }

std::string_view context::rich_link_clicked() const noexcept
{ return m_->rich_clicked_; }

std::string_view context::rich_link_hovered() const noexcept
{ return m_->rich_hovered_; }

void context::push_rich_labels() noexcept
{ ++m_->rich_depth_; }

void context::pop_rich_labels() noexcept
{ if (m_->rich_depth_ > 0) { --m_->rich_depth_; } }

bool context::rich_labels_active() const noexcept
{ return m_->rich_depth_ > 0; }

void context::push_alpha(f32 a) noexcept
{ m_->dl_.push_alpha(a); }

void context::pop_alpha() noexcept
{ m_->dl_.pop_alpha(); }

bool context::item_hovered() const noexcept
{ return m_->last_item_hovered_; }

rect context::item_rect() const noexcept
{ return m_->last_item_rect_; }

bool context::item_double_clicked() const noexcept
{ return m_->last_item_double_; }

bool context::item_active() const noexcept
{ return m_->edit_item_ == m_->last_item_key_ && m_->edit_flags_.active; }

bool context::item_activated() const noexcept
{ return m_->edit_item_ == m_->last_item_key_ && m_->edit_flags_.activated; }

bool context::item_deactivated() const noexcept
{ return m_->edit_item_ == m_->last_item_key_ && m_->edit_flags_.deactivated; }

bool context::item_edited() const noexcept
{ return m_->edit_item_ == m_->last_item_key_ && m_->edit_flags_.edited; }

bool context::item_deactivated_after_edit() const noexcept
{ return m_->edit_item_ == m_->last_item_key_ && m_->edit_flags_.after_edit; }

bool context::item_claimed() const noexcept
{ return m_->overlap_stolen_; }

void context::close_popup() noexcept
{
    const u32 drawing = m_->popup_.popup_drawing();
    if (drawing != popup_stack::no_popup) {
        m_->popup_.popup_close_from(drawing);
    } else if (m_->popup_.popup_count_ > 0) {
        m_->popup_.popup_close_from(m_->popup_.popup_count_ - 1);
    }
}

void context::close_all_popups() noexcept
{ m_->popup_.popup_close_from(0); }

rect context::last_item_rect() const noexcept
{ return m_->last_item_rect_; }

bool context::dragging() const noexcept
{ return m_->dd_active_; }

std::string_view context::drag_payload_type() const noexcept
{ return m_->dd_active_ ? std::string_view{m_->dd_type_} : std::string_view{}; }

void context::push_selectable_text() noexcept
{ ++m_->selectable_depth_; }

void context::pop_selectable_text() noexcept
{ if (m_->selectable_depth_ > 0) { --m_->selectable_depth_; } }

bool context::modal_open() const noexcept
{ return m_->modal_.modal_count_ != 0; }

u64 context::confirm_data() const noexcept
{ return m_->confirm_data_; }

f32 context::main_menu_bar_height() const noexcept
{ return m_->menu_.menu_bar_h_; }

bool context::menu_is_open() const noexcept
{ return m_->menu_.menu_open_[0].key != 0; }

void context::clear_toasts() noexcept
{ m_->toast_.toasts_.clear(); }

void context::set_toast_corner(screen_corner corner) noexcept
{ m_->toast_.toast_corner_ = corner; }

std::size_t context::toast_count() const noexcept
{ return m_->toast_.toasts_.size(); }

void context::set_dock_animation(bool on) noexcept
{ m_->dock_animation_ = on; }

void context::set_scroll_smoothing(bool on) noexcept
{ m_->scroll_smoothing_ = on; }

bool context::key_pressed(key k, bool ctrl, bool shift) const noexcept
{
    for (u32 i = 0; i < m_->input_.key_count_; ++i) {
        if (m_->input_.keys_[i].k == k && m_->input_.keys_[i].ctrl == ctrl && m_->input_.keys_[i].shift == shift) { return true; }
    }
    return false;
}

void context::request_text_focus(std::string_view label) noexcept
{
    const id key = hash_id(label, current_seed());
    if (m_->focus_id_ != key) { m_->focus_request_ = key; }
}

id context::focused_field() const noexcept
{ return m_->focus_id_; }

bool context::field_focused(std::string_view label) const noexcept
{
    return m_->focus_id_ != 0 && m_->focus_id_ == hash_id(label, current_seed());
}

bool context::ctrl_down() const noexcept
{ return m_->input_.mod_ctrl_; }

bool context::shift_down() const noexcept
{ return m_->input_.mod_shift_; }

bool context::alt_down() const noexcept
{ return m_->input_.mod_alt_; }

draw_list& context::draw() noexcept
{ return m_->dl_; }

const frame_stats& context::stats() const noexcept
{ return m_->stats_prev_; }

bool context::frame_unchanged() const noexcept
{ return m_->frame_unchanged_; }

bool context::animations_settling() const noexcept
{ return m_->anim_settling_; }

bool context::can_idle() const noexcept
{ return m_->frame_unchanged_ && !m_->anim_settling_; }

void context::invalidate() noexcept
{ m_->geometry_hash_ = 0; }

f64 context::next_wake_seconds() const noexcept
{ return m_->next_wake_; }

id context::id_collision() const noexcept
{ return m_->ids_.collision(); }

std::string_view context::id_collision_label() const noexcept
{
    return m_->ids_.collision_label();
}

f32 context::content_width() const noexcept
{ return m_->layout_.width; }

bool context::item_arrow_hit() const noexcept
{ return m_->last_item_arrow_; }

bool context::item_truncated() const noexcept
{ return m_->last_item_truncated_; }

f32 context::frame_height() const noexcept
{ return m_->font_.line_height(current_font()) + m_->style_.frame_padding.y * 2.0f; }

vec2 context::mouse_pos() const noexcept
{ return m_->input_.mouse_; }

vec2 context::display_size() const noexcept
{ return m_->display_; }

font_id context::current_font() const noexcept
{ return m_->style_stack_.current_font(); }

bool context::item_enabled() const noexcept
{ return m_->disabled_depth_ == 0; }

strata::style& context::theme() noexcept
{ return m_->style_; }

const strata::style& context::theme() const noexcept
{ return m_->style_; }

const font_atlas& context::font() const noexcept
{ return m_->font_; }

u64 context::frame_index() const noexcept
{ return m_->frame_; }

f64 context::time() const noexcept
{ return m_->time_; }

void context::release_font_pixels() noexcept
{ m_->font_.discard_pixels(); }

id context::current_seed() const noexcept
{ return m_->ids_.current(); }

void context::note_row_anchor(id key, const rect& r) noexcept
{
    m_->row_anchor_      = key;
    m_->row_anchor_rect_ = r;
}

bool context::caret_visible() const noexcept
{
    return m_->caret_blink_ <= 0.0f || std::fmod(m_->time_ - m_->caret_time_, 2.0 * m_->caret_blink_) < m_->caret_blink_;
}

} // namespace strata

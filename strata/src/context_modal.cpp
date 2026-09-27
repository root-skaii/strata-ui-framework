// modal windows and dialogs

#include "strata/context.hpp"

#include "context_impl.hpp"


#include <algorithm>

namespace strata {

void context::open_modal(std::string_view title)
{
    const id wid = hash_id(title, m_->id_stack_[0]);
    for (u32 i = 0; i < m_->modal_count_; ++i) {
        if (m_->modal_stack_[i] == wid) { return; } // already open
    }
    if (m_->modal_count_ >= max_modals) {
        report_limit("modals open at once (max_modals)", max_modals);
        return;
    }
    m_->modal_stack_[m_->modal_count_++] = wid;
    anim_for(hash_id("##modal", wid)).toggle = 0.0f; // fades in from nothing
    // whatever had the keyboard or a popup open gives way
    m_->focus_id_ = 0;
    m_->popup_id_ = 0;
    menu_close_all();
}

void context::close_modal()
{
    if (m_->modal_count_ > 0) {
        --m_->modal_count_;
    }
}

bool context::begin_modal(std::string_view title, vec2 size, modal_flags flags)
{
    if (m_->cur_ != nullptr) {
        return false;
    }
    const id wid = hash_id(title, m_->id_stack_[0]);
    u32 level = 0;
    for (u32 i = 0; i < m_->modal_count_; ++i) {
        if (m_->modal_stack_[i] == wid) { level = i + 1; }
    }
    if (level == 0) {
        return false;
    }
    const bool top = level == m_->modal_count_;

    anim_slot& a = anim_for(hash_id("##modal", wid));
    a.toggle = approach(a.toggle, 1.0f, m_->style_.anim_speed * 0.9f);
    const f32 t = std::clamp(a.toggle, 0.0f, 1.0f);

    // Esc or a click on the dim closes it (unless something inside uses the key / click)
    const rect prev = window_rect(title);
    if (top) {
        const bool busy = m_->focus_id_ != 0 || m_->hotkey_capture_ != 0 || m_->popup_id_ != 0 || m_->menu_open_[0].key != 0;
        if (has_flag(flags, modal_flags::esc_closes) && !busy) {
            for (u32 i = 0; i < m_->key_count_; ++i) {
                if (m_->keys_[i].k == key::escape) {
                    close_modal();
                    return false;
                }
            }
        }
        if (has_flag(flags, modal_flags::backdrop_closes) && m_->mouse_pressed_ && t > 0.6f && prev.width() > 0.0f &&
            !prev.contains(m_->mouse_) && !m_->menu_hit_prev_ && !(m_->popup_open_prev_ && m_->popup_rect_prev_.contains(m_->mouse_))) {
            close_modal();
            return false;
        }
    }

    // the dimmed area, in its own layer right below this modal
    {
        const u32 previous_owner = m_->run_owner_;
        switch_run(run_backdrop + level - 1);
        m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
        m_->dl_.rect_filled({{0.0f, 0.0f}, m_->display_}, m_->style_.modal_dim.scaled_alpha(t));
        m_->dl_.pop_clip();
        switch_run(previous_owner);
    }

    // centred using last frame's size (the first frame is invisible anyway)
    const vec2 win{prev.width() > 0.0f ? prev.width() : size.x, prev.height() > 0.0f ? prev.height() : (size.y > 0.0f ? size.y : 160.0f)};
    // rest on whole pixels for crisp text; the slide-in stays unrounded to avoid one-pixel jumps and a final snap
    const vec2 pos{std::round((m_->display_.x - win.x) * 0.5f), std::round((m_->display_.y - win.y) * 0.42f) + (1.0f - t) * 12.0f};
    if (window_state* st = window_for(wid, pos, size.x)) {
        st->pos = pos;
    }

    window_flags wf = window_flags::no_collapse | window_flags::no_move;
    if (has_flag(flags, modal_flags::no_title_bar)) { wf = wf | window_flags::no_title_bar; }
    if (has_flag(flags, modal_flags::resizable))    { wf = wf | window_flags::resizable; }

    m_->dl_.push_alpha(t);
    m_->modal_frames_[m_->modal_depth_ < max_modals ? m_->modal_depth_ : max_modals - 1].alpha_pushed = true;
    ++m_->modal_depth_;
    m_->next_window_modal_level_ = level;
    if (!begin_window(title, pos, size, wf)) {
        --m_->modal_depth_;
        m_->dl_.pop_alpha();
        return false;
    }
    return true;
}

void context::end_modal()
{
    if (m_->modal_depth_ == 0) {
        return;
    }
    end_window();
    m_->dl_.pop_alpha();
    --m_->modal_depth_;
}

int context::dialog(std::string_view title, std::string_view message, std::initializer_list<std::string_view> buttons, modal_flags flags)
{
    const id wid = hash_id(title, m_->id_stack_[0]);
    bool open = false;
    for (u32 i = 0; i < m_->modal_count_; ++i) { open = open || m_->modal_stack_[i] == wid; }
    if (!open) {
        return 0;
    }
    if (!begin_modal(title, {380.0f, 0.0f}, flags)) {
        return -1; // dismissed with Esc / a click outside
    }

    int result = 0;
    text_wrapped(message);
    spacing(m_->style_.item_spacing * 1.5f);

    f32 total = 0.0f;
    std::array<f32, 6> widths{};
    const std::size_t count = std::min<std::size_t>(buttons.size(), widths.size());
    std::size_t k = 0;
    for (const std::string_view label : buttons) {
        if (k >= count) { break; }
        widths[k] = std::max(label_size(current_font(), visible_label(label)).x + 2.0f * m_->style_.frame_padding.x, 84.0f);
        total += widths[k] + (k > 0 ? m_->style_.item_spacing : 0.0f);
        ++k;
    }
    (void)layout_place({0.0f, frame_height()}); // a fresh line, then start the row at the right place
    m_->layout_.same_line = true;
    m_->layout_.cursor_x  = m_->layout_.origin.x + m_->layout_.width - total - m_->style_.item_spacing;
    k = 0;
    for (const std::string_view label : buttons) {
        if (k >= count) { break; }
        set_next_item_width(widths[k]);
        if (button(label)) { result = static_cast<int>(k) + 1; }
        if (k + 1 < count) { same_line(); }
        ++k;
    }
    end_modal();
    if (result != 0) {
        close_modal();
    }
    return result;
}

// self-contained confirmation holding its open state and subject. ask_confirm() opens it (or answers at once if
// "don't ask again" is ticked), confirm() draws it and reports the button.
void context::ask_confirm(std::string_view id_label, std::string_view message, u64 user_data)
{
    // the id is global like a modal title: ask_confirm() inside a window and confirm() outside must match
    const id key = hash_id(id_label, 0);
    m_->confirm_key_      = key;
    m_->confirm_data_     = user_data;
    m_->confirm_message_.assign(message);
    m_->confirm_remember_ = false;
    m_->confirm_answer_     = 0;
    m_->confirm_answer_key_ = 0;
    m_->confirm_pending_    = true; // opened by the next confirm() call, which knows the options
}

int context::confirm(std::string_view id_label, std::initializer_list<std::string_view> buttons,
                     const confirm_options& options)
{
    const id key = hash_id(id_label, 0);

    // the answer of the frame the button was pressed, handed out once
    if (m_->confirm_answer_key_ == key && m_->confirm_answer_ != 0) {
        const int answer    = m_->confirm_answer_;
        m_->confirm_answer_     = 0;
        m_->confirm_answer_key_ = 0;
        return answer;
    }
    if (m_->confirm_pending_ && m_->confirm_key_ == key) {
        m_->confirm_pending_ = false;
        if (options.remember != nullptr && *options.remember) {
            return options.remembered; // the user asked not to be asked
        }
        open_modal(options.title.empty() ? std::string_view{"Confirm"} : options.title);
        m_->confirm_open_ = key;
    }
    if (m_->confirm_open_ != key) {
        return 0;
    }

    const std::string_view title = options.title.empty() ? std::string_view{"Confirm"} : options.title;
    const id wid = hash_id(title, m_->id_stack_[0]);
    bool open = false;
    for (u32 i = 0; i < m_->modal_count_; ++i) { open = open || m_->modal_stack_[i] == wid; }
    if (!open) {
        m_->confirm_open_ = 0;
        return 0;
    }
    if (!begin_modal(title, {380.0f, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes)) {
        m_->confirm_open_ = 0;
        return -1; // dismissed with Esc / a click outside
    }

    int result = 0;
    text_wrapped(m_->confirm_message_);
    if (options.remember != nullptr) {
        spacing(m_->style_.item_spacing);
        (void)checkbox(options.remember_label, m_->confirm_remember_);
    }
    spacing(m_->style_.item_spacing * 1.5f);

    f32 total = 0.0f;
    std::array<f32, 6> widths{};
    const std::size_t count = std::min<std::size_t>(buttons.size(), widths.size());
    std::size_t k = 0;
    for (const std::string_view label : buttons) {
        if (k >= count) { break; }
        widths[k] = std::max(label_size(current_font(), visible_label(label)).x + 2.0f * m_->style_.frame_padding.x, 84.0f);
        total += widths[k] + (k > 0 ? m_->style_.item_spacing : 0.0f);
        ++k;
    }
    (void)layout_place({0.0f, frame_height()});
    m_->layout_.same_line = true;
    m_->layout_.cursor_x  = m_->layout_.origin.x + m_->layout_.width - total - m_->style_.item_spacing;
    k = 0;
    for (const std::string_view label : buttons) {
        if (k >= count) { break; }
        const bool danger = options.danger == static_cast<int>(k) + 1;
        set_next_item_width(widths[k]);
        if (danger) { push_color(style_color::accent, kind_color(toast_kind::error, m_->style_)); }
        if (button(label)) { result = static_cast<int>(k) + 1; }
        if (danger) { pop_color(); }
        if (k + 1 < count) { same_line(); }
        ++k;
    }
    end_modal();
    if (result != 0) {
        close_modal();
        m_->confirm_open_ = 0;
        if (options.remember != nullptr && m_->confirm_remember_) { *options.remember = true; }
        // reported on the next frame, so it is not mistaken for the click that pressed the button
        m_->confirm_answer_     = result;
        m_->confirm_answer_key_ = key;
    }
    return 0;
}

} // namespace strata

// modal windows and dialogs

#include "strata/context.hpp"

#include "limits.hpp"

#include <algorithm>

namespace strata {

void context::open_modal(std::string_view title)
{
    const id wid = hash_id(title, id_stack_[0]);
    for (u32 i = 0; i < modal_count_; ++i) {
        if (modal_stack_[i] == wid) { return; } // already open
    }
    if (modal_count_ >= max_modals) {
        internal::limit_reached("modals open at once (max_modals)", max_modals);
        return;
    }
    modal_stack_[modal_count_++] = wid;
    anim_for(hash_id("##modal", wid)).toggle = 0.0f; // fades in from nothing
    // whatever had the keyboard or a popup open gives way
    focus_id_ = 0;
    popup_id_ = 0;
    menu_close_all();
}

void context::close_modal()
{
    if (modal_count_ > 0) {
        --modal_count_;
    }
}

bool context::begin_modal(std::string_view title, vec2 size, modal_flags flags)
{
    if (cur_ != nullptr) {
        return false;
    }
    const id wid = hash_id(title, id_stack_[0]);
    u32 level = 0;
    for (u32 i = 0; i < modal_count_; ++i) {
        if (modal_stack_[i] == wid) { level = i + 1; }
    }
    if (level == 0) {
        return false;
    }
    const bool top = level == modal_count_;

    anim_slot& a = anim_for(hash_id("##modal", wid));
    a.toggle = approach(a.toggle, 1.0f, style_.anim_speed * 0.9f);
    const f32 t = std::clamp(a.toggle, 0.0f, 1.0f);

    // Esc, or a click on the dimmed area, closes it (unless something inside it is using the key / click)
    const rect prev = window_rect(title);
    if (top) {
        const bool busy = focus_id_ != 0 || hotkey_capture_ != 0 || popup_id_ != 0 || menu_open_[0].key != 0;
        if (has_flag(flags, modal_flags::esc_closes) && !busy) {
            for (u32 i = 0; i < key_count_; ++i) {
                if (keys_[i].k == key::escape) {
                    close_modal();
                    return false;
                }
            }
        }
        if (has_flag(flags, modal_flags::backdrop_closes) && mouse_pressed_ && t > 0.6f && prev.width() > 0.0f &&
            !prev.contains(mouse_) && !menu_hit_prev_ && !(popup_open_prev_ && popup_rect_prev_.contains(mouse_))) {
            close_modal();
            return false;
        }
    }

    // the dimmed area, in its own layer right below this modal
    {
        const u32 previous_owner = run_owner_;
        switch_run(run_backdrop + level - 1);
        dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
        dl_.rect_filled({{0.0f, 0.0f}, display_}, style_.modal_dim.scaled_alpha(t));
        dl_.pop_clip();
        switch_run(previous_owner);
    }

    // centered on the display, using the size it had last frame (the first frame it is invisible anyway)
    const vec2 win{prev.width() > 0.0f ? prev.width() : size.x, prev.height() > 0.0f ? prev.height() : (size.y > 0.0f ? size.y : 160.0f)};
    // the resting place is on whole pixels (crisp text); the slide-in on top of it is not rounded, or it would move in
    // one-pixel jumps and the last one would look like a snap
    const vec2 pos{std::round((display_.x - win.x) * 0.5f), std::round((display_.y - win.y) * 0.42f) + (1.0f - t) * 12.0f};
    if (window_state* st = window_for(wid, pos, size.x)) {
        st->pos = pos;
    }

    window_flags wf = window_flags::no_collapse | window_flags::no_move;
    if (has_flag(flags, modal_flags::no_title_bar)) { wf = wf | window_flags::no_title_bar; }
    if (has_flag(flags, modal_flags::resizable))    { wf = wf | window_flags::resizable; }

    dl_.push_alpha(t);
    modal_frames_[modal_depth_ < max_modals ? modal_depth_ : max_modals - 1].alpha_pushed = true;
    ++modal_depth_;
    next_window_modal_level_ = level;
    if (!begin_window(title, pos, size, wf)) {
        --modal_depth_;
        dl_.pop_alpha();
        return false;
    }
    return true;
}

void context::end_modal()
{
    if (modal_depth_ == 0) {
        return;
    }
    end_window();
    dl_.pop_alpha();
    --modal_depth_;
}

int context::dialog(std::string_view title, std::string_view message, std::initializer_list<std::string_view> buttons, modal_flags flags)
{
    const id wid = hash_id(title, id_stack_[0]);
    bool open = false;
    for (u32 i = 0; i < modal_count_; ++i) { open = open || modal_stack_[i] == wid; }
    if (!open) {
        return 0;
    }
    if (!begin_modal(title, {380.0f, 0.0f}, flags)) {
        return -1; // dismissed with Esc / a click outside
    }

    int result = 0;
    text_wrapped(message);
    spacing(style_.item_spacing * 1.5f);

    f32 total = 0.0f;
    std::array<f32, 6> widths{};
    const std::size_t count = std::min<std::size_t>(buttons.size(), widths.size());
    std::size_t k = 0;
    for (const std::string_view label : buttons) {
        if (k >= count) { break; }
        widths[k] = std::max(label_size(current_font(), visible_label(label)).x + 2.0f * style_.frame_padding.x, 84.0f);
        total += widths[k] + (k > 0 ? style_.item_spacing : 0.0f);
        ++k;
    }
    (void)layout_place({0.0f, frame_height()}); // a fresh line, then start the row at the right place
    layout_.same_line = true;
    layout_.cursor_x  = layout_.origin.x + layout_.width - total - style_.item_spacing;
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

} // namespace strata

// single-line text fields (input_text); the editing engine and multi-line fields are context_text.cpp

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

bool context::input_text(std::string_view label, std::string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(m_->edit_.edit_buf_.data(), m_->edit_.edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_text(std::string_view label, secure_string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(m_->edit_.edit_buf_.data(), m_->edit_.edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_text(std::string_view label, char* buffer, std::size_t capacity, std::string_view hint,
                         input_flags flags)
{
    if (buffer == nullptr || capacity == 0) {
        return false;
    }
    std::size_t len = 0;
    while (len + 1 < capacity && buffer[len] != '\0') { ++len; }

    if (!input_core(label, {buffer, len}, hint, flags, capacity - 1)) {
        return false;
    }
    const std::size_t n = std::min(m_->edit_.edit_buf_.size(), capacity - 1);
    std::copy_n(m_->edit_.edit_buf_.data(), n, buffer);
    buffer[n] = '\0';
    return true;
}

bool context::input_core(std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (m_->cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = current_font();
    f32           lh       = m_->font_.line_height(fnt);
    f32           asc      = m_->font_.ascent(fnt);
    const bool    password = has_flag(flags, input_flags::password);
    const bool    reveal_btn = password && has_flag(flags, input_flags::reveal);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const id      key      = widget_id(label);
    ed_prepare_spans(m_->focus_id_ == key ? m_->edit_.edit_buf_.size() : current.size(), fnt, lh, asc, password);

    field_layout fl;
    if (m_->input_rect_set_) { // a widget that owns the box (number fields): no caption, no layout
        m_->input_rect_set_ = false;
        fl.control = m_->input_rect_;
    } else {
        fl = layout_field(visible_label(label), frame_height());
    }
    const rect         box = fl.control;

    // password eye button: toggles visibility (state in an animation slot, dropped when the field goes undrawn).
    // tested first so it wins the press
    const f32  reveal_w = reveal_btn ? 26.0f : 0.0f;
    const rect reveal_r = {{box.max.x - reveal_w - 2.0f, box.min.y + 2.0f}, {box.max.x - 3.0f, box.max.y - 2.0f}};
    bool       revealed = false;
    bool       reveal_hot = false;
    if (reveal_btn) {
        anim_slot& ra = anim_for(part_id(part::reveal_button, key));
        const interaction bi = interact(part_id(part::reveal_button, key), reveal_r);
        if (bi.pressed) { ra.active = ra.active > 0.5f ? 0.0f : 1.0f; }
        if (m_->input_.mouse_pressed_ && bi.held) { m_->press_claimed_ = true; } // the field keeps the keyboard if it had it
        revealed   = ra.active > 0.5f;
        reveal_hot = bi.hovered;
    }
    const bool hide = password && !revealed; // what is drawn as bullets

    // clear button (x while the field has text), tested before the field so it takes the press
    const bool has_text  = m_->focus_id_ == key ? !m_->edit_.edit_buf_.empty() : !current.empty();
    const bool clear_btn = has_flag(flags, input_flags::clear_button) && !readonly && has_text;
    const f32  clear_w   = clear_btn ? 22.0f : 0.0f;
    const rect clear_r   = {{box.max.x - reveal_w - clear_w - 2.0f, box.min.y + 2.0f},
                            {box.max.x - reveal_w - 3.0f, box.max.y - 2.0f}};
    bool       clear_hot = false;
    bool       cleared   = false;
    if (clear_btn) {
        const interaction bi = interact(part_id(part::clear_button, key), clear_r);
        clear_hot = bi.hovered;
        cleared   = bi.pressed;
        if (m_->input_.mouse_pressed_ && bi.held) { m_->press_claimed_ = true; } // the field keeps the keyboard if it had it
    }

    const interaction  in  = interact(key, box);
    if (in.hovered || (in.held && m_->focus_id_ == key)) { m_->cursor_ = reveal_hot ? cursor_kind::arrow : cursor_kind::text; }

    const f32  pad_x  = m_->style_.frame_padding.x;
    const rect inner  = {{box.min.x + pad_x, box.min.y}, {box.max.x - pad_x - reveal_w - clear_w, box.max.y}};
    const f32  text_y = box.min.y + (box.height() - lh) * 0.5f;

    // what is shown: the live edit buffer while focused, the caller's text otherwise
    bool focused = m_->focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{m_->edit_.edit_buf_} : current; };

    std::string masked;
    const auto rebuild_mask = [&] {
        if (hide) { masked.assign(count_codepoints(text_now()), '*'); }
    };
    rebuild_mask();

    // right-to-left text: where the caret and the mouse go follows the reordered letters
    bidi_layout bidi;
    std::string bidi_src;
    const auto rtl_layout = [&]() -> const bidi_layout* {
        if (hide) { return nullptr; }
        const std::string_view t = text_now();
        if (!has_rtl_text(t)) { return nullptr; }
        if (bidi_src != t) {
            bidi_src.assign(t);
            bidi.build(m_->font_, fnt, t);
        }
        return &bidi;
    };

    const auto prefix_width = [&](std::size_t byte_index) -> f32 {
        const std::string_view t = text_now();
        if (const bidi_layout* b = rtl_layout()) {
            return b->caret_x(byte_index);
        }
        if (!hide) {
            return ed_measure(fnt, t, 0, byte_index);
        }
        return m_->font_.measure(fnt, std::string_view{masked}.substr(0, count_codepoints(t.substr(0, byte_index)))).x;
    };
    const auto index_at = [&](f32 mouse_x) {
        const std::string_view t = text_now();
        const f32 rel = mouse_x - inner.min.x + m_->edit_.edit_scroll_;
        if (const bidi_layout* b = rtl_layout()) {
            return b->index_at(rel);
        }
        std::size_t best = 0;
        f32         best_d = 1.0e9f;
        for (std::size_t i = 0;; i = next_boundary(t, i)) {
            const f32 d = std::abs(prefix_width(i) - rel);
            if (d < best_d) { best_d = d; best = i; }
            if (i >= t.size()) { break; }
        }
        return best;
    };

    // --- focus and mouse -----------------------------------------------------
    if (m_->focus_request_ == key) { // requested by code (find bar opening): like a click, all selected
        m_->focus_request_ = 0;
        m_->press_claimed_ = true;
        if (m_->focus_id_ != key) {
            m_->focus_id_    = key;
            focused      = true;
            wipe_edit_buffer();
            m_->edit_.edit_buf_.assign(current);
            m_->edit_.edit_scroll_ = 0.0f;
            m_->edit_.edit_cursor_ = m_->edit_.edit_buf_.size();
            m_->edit_.edit_anchor_ = 0;
            m_->edit_.caret_time_  = m_->time_;
            rebuild_mask();
        }
    }
    const bool press_here = m_->input_.mouse_pressed_ && in.held;
    if (press_here) {
        m_->press_claimed_ = true;
        if (m_->focus_id_ != key) {
            m_->focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(); // nothing of the previous field's text may stay behind
            m_->edit_.edit_buf_.assign(current);
            m_->edit_.edit_scroll_ = 0.0f;
            m_->edit_.edit_cursor_ = m_->edit_.edit_buf_.size();
            m_->edit_.edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : m_->edit_.edit_cursor_;
            m_->edit_.caret_time_  = m_->time_;
            rebuild_mask();
        }

        const std::size_t idx = index_at(m_->input_.mouse_.x);
        const u32 clicks = register_click();
        if (clicks == 3) { // triple click: the whole line (a single-line field is one line)
            m_->edit_.edit_anchor_ = 0;
            m_->edit_.edit_cursor_ = m_->edit_.edit_buf_.size();
        } else if (clicks == 2) {
            m_->edit_.edit_anchor_ = word_start(m_->edit_.edit_buf_, idx);
            m_->edit_.edit_cursor_ = word_end(m_->edit_.edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || m_->edit_.edit_cursor_ != 0) {
            m_->edit_.edit_cursor_ = idx;
            m_->edit_.edit_anchor_ = idx;
        }
        m_->edit_.caret_time_ = m_->time_;
    } else if (focused && in.held) {
        m_->edit_.edit_cursor_ = index_at(m_->input_.mouse_.x);
        m_->edit_.caret_time_  = m_->time_;
    }
    if (focused) {
        m_->focus_seen_ = true;
    }

    // --- keyboard ----------------------------------------------------------------
    bool changed = false;
    if (cleared) { // empties the field whether or not it has the keyboard
        m_->edit_.edit_buf_.clear();
        m_->edit_.edit_cursor_ = 0;
        m_->edit_.edit_anchor_ = 0;
        m_->edit_.edit_scroll_ = 0.0f;
        edit_history_clear();
        ++m_->edit_.edit_version_;
        changed = true;
    }
    if (focused) {
        m_->edit_.edit_readonly_   = readonly;
        m_->edit_.edit_max_bytes_  = max_bytes;
        m_->edit_.edit_history_on_ = !password && !readonly && m_->edit_mask_.empty();

        if (m_->input_.typed_len_ != 0) {
            changed = edit_insert({m_->input_.typed_.data(), m_->input_.typed_len_}, true) || changed;
            m_->input_.typed_len_  = 0;
            m_->edit_.caret_time_ = m_->time_;
        }

        for (u32 i = 0; i < m_->input_.key_count_ && focused; ++i) {
            const key_event& ev = m_->input_.keys_[i];
            if (ev.alt) { continue; } // Alt + key is a shortcut of the host, not editing
            const std::string_view t = m_->edit_.edit_buf_;
            const bool has_sel = m_->edit_.edit_cursor_ != m_->edit_.edit_anchor_;
            m_->edit_.caret_time_ = m_->time_;
            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_.edit_cursor_ = m_->edit_.edit_anchor_ = std::min(m_->edit_.edit_cursor_, m_->edit_.edit_anchor_);
                } else {
                    m_->edit_.edit_cursor_ = ev.ctrl ? prev_word(t, m_->edit_.edit_cursor_) : prev_boundary(t, m_->edit_.edit_cursor_);
                    if (!ev.shift) { m_->edit_.edit_anchor_ = m_->edit_.edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_.edit_cursor_ = m_->edit_.edit_anchor_ = std::max(m_->edit_.edit_cursor_, m_->edit_.edit_anchor_);
                } else {
                    m_->edit_.edit_cursor_ = ev.ctrl ? next_word(t, m_->edit_.edit_cursor_) : next_boundary(t, m_->edit_.edit_cursor_);
                    if (!ev.shift) { m_->edit_.edit_anchor_ = m_->edit_.edit_cursor_; }
                }
                break;
            case key::home:
                m_->edit_.edit_cursor_ = 0;
                if (!ev.shift) { m_->edit_.edit_anchor_ = m_->edit_.edit_cursor_; }
                break;
            case key::end:
                m_->edit_.edit_cursor_ = t.size();
                if (!ev.shift) { m_->edit_.edit_anchor_ = m_->edit_.edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, m_->edit_.edit_cursor_) : prev_boundary(t, m_->edit_.edit_cursor_);
                        if (a < m_->edit_.edit_cursor_) {
                            changed = edit_replace(a, m_->edit_.edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, m_->edit_.edit_cursor_) : next_boundary(t, m_->edit_.edit_cursor_);
                        if (b > m_->edit_.edit_cursor_) {
                            changed = edit_replace(m_->edit_.edit_cursor_, b - m_->edit_.edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                m_->edit_.submitted_ = true;
                break;
            case key::escape:
            case key::tab:
                m_->focus_id_ = 0;
                focused   = false;
                break;
            case key::a:
            case key::c:
            case key::x:
            case key::v:
            case key::z:
            case key::y:
                changed = edit_shortcut(ev, password, false) || changed;
                break;
            default:
                break;
            }
        }
        m_->input_.key_count_ = 0;
        if (changed && !m_->edit_mask_.empty()) { apply_input_mask(); }
        rebuild_mask();
    }

    if (focused) {
        const f32 view_w  = std::max(inner.width(), 1.0f);
        const bool composing_now = !readonly && !password && m_->input_.ime_len_ != 0;
        const std::string_view comp{m_->input_.ime_text_.data(), m_->input_.ime_len_};
        const f32 caret_x = prefix_width(m_->edit_.edit_cursor_) + (composing_now ? m_->font_.measure(fnt, comp.substr(0, m_->input_.ime_cursor_)).x : 0.0f);
        if (caret_x - m_->edit_.edit_scroll_ > view_w - 2.0f) { m_->edit_.edit_scroll_ = caret_x - view_w + 2.0f; }
        if (caret_x - m_->edit_.edit_scroll_ < 0.0f)          { m_->edit_.edit_scroll_ = caret_x; }
        const bidi_layout* rtl_now = rtl_layout();
        const f32 total = (rtl_now != nullptr ? rtl_now->width() : prefix_width(text_now().size())) + (composing_now ? m_->font_.measure(fnt, comp).x : 0.0f);
        if (total - m_->edit_.edit_scroll_ < view_w - 2.0f)   { m_->edit_.edit_scroll_ = std::max(0.0f, total - view_w + 2.0f); }
    }

    // --- drawing -----------------------------------------------------------------
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, focused ? 1.0f : 0.0f);

    shape_style field;
    field.radius       = radii(m_->style_.rounding * 0.8f);
    field.fill_top     = darken(m_->style_.widget_bg, 0.28f);
    field.fill_bottom  = darken(m_->style_.widget_bg, 0.12f);
    field.border       = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.border_width = 1.0f + a.toggle * 0.5f;
    field.shadow       = m_->style_.accent.scaled_alpha(0.32f * a.toggle);
    field.shadow_blur  = 9.0f * a.toggle;
    m_->dl_.shape(box, field);

    if (clear_btn) { // a small x
        if (clear_hot) {
            shape_style hot;
            hot.radius      = radii(m_->style_.rounding * 0.6f);
            hot.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m_->dl_.shape(clear_r, hot);
        }
        const vec2  c   = clear_r.center();
        const color col = clear_hot ? m_->style_.text : m_->style_.text_dim;
        constexpr f32 arm = 4.0f;
        m_->dl_.line({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, col, 1.4f);
        m_->dl_.line({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, col, 1.4f);
    }

    if (reveal_btn) { // the eye: open when the text is shown, crossed out when it is hidden
        if (reveal_hot) {
            shape_style hot;
            hot.radius      = radii(m_->style_.rounding * 0.6f);
            hot.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m_->dl_.shape(reveal_r, hot);
        }
        const vec2  c   = reveal_r.center();
        const color col = lerp(m_->style_.text_dim, m_->style_.text, reveal_hot || revealed ? 1.0f : 0.0f);
        m_->dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y - 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m_->dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y + 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m_->dl_.circle_filled(c, 2.2f, col);
        if (!revealed) { m_->dl_.line({c.x - 6.0f, c.y + 6.0f}, {c.x + 6.0f, c.y - 6.0f}, col, 1.5f); }
    }

    m_->dl_.push_clip({{inner.min.x, box.min.y + 1.0f}, {inner.max.x, box.max.y - 1.0f}});
    const f32 text_x = inner.min.x - (focused ? m_->edit_.edit_scroll_ : 0.0f);

    const std::string_view shown_text = text_now();
    const bool composing = focused && !readonly && !password && m_->input_.ime_len_ != 0;
    const std::string_view comp{m_->input_.ime_text_.data(), m_->input_.ime_len_};
    std::string disp; // the text with the composition inserted at the caret
    if (composing) {
        const std::size_t at = std::min(m_->edit_.edit_cursor_, shown_text.size());
        disp.assign(shown_text.substr(0, at));
        disp.append(comp);
        disp.append(shown_text.substr(at));
    }
    const auto disp_width = [&](std::size_t n) { return m_->font_.measure(fnt, std::string_view{disp}.substr(0, n)).x; };
    if (focused && m_->edit_.edit_cursor_ != m_->edit_.edit_anchor_ && !composing) {
        const std::size_t lo = std::min(m_->edit_.edit_cursor_, m_->edit_.edit_anchor_);
        const std::size_t hi = std::max(m_->edit_.edit_cursor_, m_->edit_.edit_anchor_);
        shape_style sel;
        sel.radius      = radii(2.0f);
        sel.fill_top    = m_->style_.accent.scaled_alpha(0.45f);
        sel.fill_bottom = m_->style_.accent.scaled_alpha(0.45f);
        const f32 xa = prefix_width(lo);
        const f32 xb = prefix_width(hi);
        m_->dl_.shape({{text_x + std::min(xa, xb), text_y - 1.0f}, {text_x + std::max(xa, xb), text_y + lh + 1.0f}}, sel);
    }

    if (shown_text.empty() && !focused && !hint.empty()) {
        m_->dl_.text({text_x, text_y}, m_->style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    } else if (hide) {
        m_->dl_.text({text_x, text_y}, m_->style_.text, masked, fnt);
    } else if (composing) { // the composition sits in the text, underlined and lightly marked
        const std::size_t at = std::min(m_->edit_.edit_cursor_, shown_text.size());
        const f32 x0 = text_x + disp_width(at);
        const f32 x1 = text_x + disp_width(at + comp.size());
        m_->dl_.rect_filled({{x0, text_y - 1.0f}, {x1, text_y + lh + 1.0f}}, m_->style_.accent.scaled_alpha(0.18f));
        m_->dl_.text({text_x, text_y}, m_->style_.text, disp, fnt);
        m_->dl_.rect_filled({{x0, text_y + lh - 1.0f}, {x1, text_y + lh + 0.5f}}, m_->style_.accent_hover);
    } else if (!m_->edit_.edit_spans_.empty()) {
        ed_draw({text_x, text_y}, asc, m_->style_.text, shown_text, 0, shown_text.size(), fnt);
    } else {
        m_->dl_.text({text_x, text_y}, m_->style_.text, shown_text, fnt);
    }

    if (focused) { // where the input method puts its candidate window
        const f32 caret_x = composing ? text_x + disp_width(std::min(m_->edit_.edit_cursor_, shown_text.size()) + m_->input_.ime_cursor_)
                                      : text_x + prefix_width(m_->edit_.edit_cursor_);
        m_->edit_.ime_want_   = !password; // (a password field turns the input method off)
        m_->edit_.ime_pos_    = {caret_x * m_->scale_, (text_y + lh) * m_->scale_};
        m_->edit_.ime_line_h_ = lh * m_->scale_;
        if (caret_visible()) {
            const f32 cx = std::round(caret_x);
            m_->dl_.rect_filled({{cx, text_y}, {cx + 1.5f, text_y + lh}}, m_->style_.text);
        }
    }
    m_->dl_.pop_clip();

    track_edit(key, changed, m_->focus_id_ == key);
    return changed;
}

} // namespace strata

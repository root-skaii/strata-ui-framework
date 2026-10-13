// single-line text fields (input_text); the editing engine and multi-line fields are context_text.cpp

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

bool context::input_text(std::string_view label, std::string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(*this, label, value, hint, flags, max_bytes)) {
        value.assign(m_->edit_.edit_buf_.data(), m_->edit_.edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_text(std::string_view label, secure_string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(*this, label, value, hint, flags, max_bytes)) {
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

    if (!input_core(*this, label, {buffer, len}, hint, flags, capacity - 1)) {
        return false;
    }
    const std::size_t n = std::min(m_->edit_.edit_buf_.size(), capacity - 1);
    std::copy_n(m_->edit_.edit_buf_.data(), n, buffer);
    buffer[n] = '\0';
    return true;
}

bool internal::input_core(context& ui, std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    auto& m = context_access::of(ui);
    if (m.cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = ui.current_font();
    f32           lh       = m.font_.line_height(fnt);
    f32           asc      = m.font_.ascent(fnt);
    const bool    password = has_flag(flags, input_flags::password);
    const bool    reveal_btn = password && has_flag(flags, input_flags::reveal);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const id      key      = widget_id(ui, label);
    ed_prepare_spans(ui, m.focus_id_ == key ? m.edit_.edit_buf_.size() : current.size(), fnt, lh, asc, password);

    field_layout fl;
    if (m.input_rect_set_) { // a widget that owns the box (number fields): no caption, no layout
        m.input_rect_set_ = false;
        fl.control = m.input_rect_;
    } else {
        fl = layout_field(ui, visible_label(label), ui.frame_height());
    }
    const rect         box = fl.control;

    // password eye button: toggles visibility (state in an animation slot, dropped when the field goes undrawn).
    // tested first so it wins the press
    const f32  reveal_w = reveal_btn ? 26.0f : 0.0f;
    const rect reveal_r = {{box.max.x - reveal_w - 2.0f, box.min.y + 2.0f}, {box.max.x - 3.0f, box.max.y - 2.0f}};
    bool       revealed = false;
    bool       reveal_hot = false;
    if (reveal_btn) {
        anim_slot& ra = anim_for(ui, part_id(part::reveal_button, key));
        const interaction bi = interact(ui, part_id(part::reveal_button, key), reveal_r);
        if (bi.pressed) { ra.active = ra.active > 0.5f ? 0.0f : 1.0f; }
        if (m.input_.mouse_pressed_ && bi.held) { m.press_claimed_ = true; } // the field keeps the keyboard if it had it
        revealed   = ra.active > 0.5f;
        reveal_hot = bi.hovered;
    }
    const bool hide = password && !revealed; // what is drawn as bullets

    // clear button (x while the field has text), tested before the field so it takes the press
    const bool has_text  = m.focus_id_ == key ? !m.edit_.edit_buf_.empty() : !current.empty();
    const bool clear_btn = has_flag(flags, input_flags::clear_button) && !readonly && has_text;
    const f32  clear_w   = clear_btn ? 22.0f : 0.0f;
    const rect clear_r   = {{box.max.x - reveal_w - clear_w - 2.0f, box.min.y + 2.0f},
                            {box.max.x - reveal_w - 3.0f, box.max.y - 2.0f}};
    bool       clear_hot = false;
    bool       cleared   = false;
    if (clear_btn) {
        const interaction bi = interact(ui, part_id(part::clear_button, key), clear_r);
        clear_hot = bi.hovered;
        cleared   = bi.pressed;
        if (m.input_.mouse_pressed_ && bi.held) { m.press_claimed_ = true; } // the field keeps the keyboard if it had it
    }

    const interaction  in  = interact(ui, key, box);
    if (in.hovered || (in.held && m.focus_id_ == key)) { m.cursor_ = reveal_hot ? cursor_kind::arrow : cursor_kind::text; }

    const f32  pad_x  = m.style_.frame_padding.x;
    const rect inner  = {{box.min.x + pad_x, box.min.y}, {box.max.x - pad_x - reveal_w - clear_w, box.max.y}};
    const f32  text_y = box.min.y + (box.height() - lh) * 0.5f;

    // what is shown: the live edit buffer while focused, the caller's text otherwise
    bool focused = m.focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{m.edit_.edit_buf_} : current; };

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
            bidi.build(m.font_, fnt, t);
        }
        return &bidi;
    };

    const auto prefix_width = [&](std::size_t byte_index) -> f32 {
        const std::string_view t = text_now();
        if (const bidi_layout* b = rtl_layout()) {
            return b->caret_x(byte_index);
        }
        if (!hide) {
            return ed_measure(ui, fnt, t, 0, byte_index);
        }
        return m.font_.measure(fnt, std::string_view{masked}.substr(0, count_codepoints(t.substr(0, byte_index)))).x;
    };
    const auto index_at = [&](f32 mouse_x) {
        const std::string_view t = text_now();
        const f32 rel = mouse_x - inner.min.x + m.edit_.edit_scroll_;
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
    if (m.focus_request_ == key) { // requested by code (find bar opening): like a click, all selected
        m.focus_request_ = 0;
        m.press_claimed_ = true;
        if (m.focus_id_ != key) {
            m.focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(ui);
            m.edit_.edit_buf_.assign(current);
            m.edit_.edit_scroll_ = 0.0f;
            m.edit_.edit_cursor_ = m.edit_.edit_buf_.size();
            m.edit_.edit_anchor_ = 0;
            m.edit_.caret_time_  = m.time_;
            rebuild_mask();
        }
    }
    const bool press_here = m.input_.mouse_pressed_ && in.held;
    if (press_here) {
        m.press_claimed_ = true;
        if (m.focus_id_ != key) {
            m.focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(ui); // nothing of the previous field's text may stay behind
            m.edit_.edit_buf_.assign(current);
            m.edit_.edit_scroll_ = 0.0f;
            m.edit_.edit_cursor_ = m.edit_.edit_buf_.size();
            m.edit_.edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : m.edit_.edit_cursor_;
            m.edit_.caret_time_  = m.time_;
            rebuild_mask();
        }

        const std::size_t idx = index_at(m.input_.mouse_.x);
        const u32 clicks = register_click(ui);
        if (clicks == 3) { // triple click: the whole line (a single-line field is one line)
            m.edit_.edit_anchor_ = 0;
            m.edit_.edit_cursor_ = m.edit_.edit_buf_.size();
        } else if (clicks == 2) {
            m.edit_.edit_anchor_ = word_start(m.edit_.edit_buf_, idx);
            m.edit_.edit_cursor_ = word_end(m.edit_.edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || m.edit_.edit_cursor_ != 0) {
            m.edit_.edit_cursor_ = idx;
            m.edit_.edit_anchor_ = idx;
        }
        m.edit_.caret_time_ = m.time_;
    } else if (focused && in.held) {
        m.edit_.edit_cursor_ = index_at(m.input_.mouse_.x);
        m.edit_.caret_time_  = m.time_;
    }
    if (focused) {
        m.focus_seen_ = true;
    }

    // --- keyboard ----------------------------------------------------------------
    bool changed = false;
    if (cleared) { // empties the field whether or not it has the keyboard
        m.edit_.edit_buf_.clear();
        m.edit_.edit_cursor_ = 0;
        m.edit_.edit_anchor_ = 0;
        m.edit_.edit_scroll_ = 0.0f;
        edit_history_clear(ui);
        ++m.edit_.edit_version_;
        changed = true;
    }
    if (focused) {
        m.edit_.edit_readonly_   = readonly;
        m.edit_.edit_max_bytes_  = max_bytes;
        m.edit_.edit_history_on_ = !password && !readonly && m.edit_mask_.empty();

        if (m.input_.typed_len_ != 0) {
            changed = edit_insert(ui, {m.input_.typed_.data(), m.input_.typed_len_}, true) || changed;
            m.input_.typed_len_  = 0;
            m.edit_.caret_time_ = m.time_;
        }

        for (u32 i = 0; i < m.input_.key_count_ && focused; ++i) {
            const key_event& ev = m.input_.keys_[i];
            if (ev.alt) { continue; } // Alt + key is a shortcut of the host, not editing
            const std::string_view t = m.edit_.edit_buf_;
            const bool has_sel = m.edit_.edit_cursor_ != m.edit_.edit_anchor_;
            m.edit_.caret_time_ = m.time_;
            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m.edit_.edit_cursor_ = m.edit_.edit_anchor_ = std::min(m.edit_.edit_cursor_, m.edit_.edit_anchor_);
                } else {
                    m.edit_.edit_cursor_ = ev.ctrl ? prev_word(t, m.edit_.edit_cursor_) : prev_boundary(t, m.edit_.edit_cursor_);
                    if (!ev.shift) { m.edit_.edit_anchor_ = m.edit_.edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m.edit_.edit_cursor_ = m.edit_.edit_anchor_ = std::max(m.edit_.edit_cursor_, m.edit_.edit_anchor_);
                } else {
                    m.edit_.edit_cursor_ = ev.ctrl ? next_word(t, m.edit_.edit_cursor_) : next_boundary(t, m.edit_.edit_cursor_);
                    if (!ev.shift) { m.edit_.edit_anchor_ = m.edit_.edit_cursor_; }
                }
                break;
            case key::home:
                m.edit_.edit_cursor_ = 0;
                if (!ev.shift) { m.edit_.edit_anchor_ = m.edit_.edit_cursor_; }
                break;
            case key::end:
                m.edit_.edit_cursor_ = t.size();
                if (!ev.shift) { m.edit_.edit_anchor_ = m.edit_.edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection(ui)) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, m.edit_.edit_cursor_) : prev_boundary(t, m.edit_.edit_cursor_);
                        if (a < m.edit_.edit_cursor_) {
                            changed = edit_replace(ui, a, m.edit_.edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection(ui)) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, m.edit_.edit_cursor_) : next_boundary(t, m.edit_.edit_cursor_);
                        if (b > m.edit_.edit_cursor_) {
                            changed = edit_replace(ui, m.edit_.edit_cursor_, b - m.edit_.edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                m.edit_.submitted_ = true;
                break;
            case key::escape:
            case key::tab:
                m.focus_id_ = 0;
                focused   = false;
                break;
            case key::a:
            case key::c:
            case key::x:
            case key::v:
            case key::z:
            case key::y:
                changed = edit_shortcut(ui, ev, password, false) || changed;
                break;
            default:
                break;
            }
        }
        m.input_.key_count_ = 0;
        if (changed && !m.edit_mask_.empty()) { apply_input_mask(ui); }
        rebuild_mask();
    }

    if (focused) {
        const f32 view_w  = std::max(inner.width(), 1.0f);
        const bool composing_now = !readonly && !password && m.input_.ime_len_ != 0;
        const std::string_view comp{m.input_.ime_text_.data(), m.input_.ime_len_};
        const f32 caret_x = prefix_width(m.edit_.edit_cursor_) + (composing_now ? m.font_.measure(fnt, comp.substr(0, m.input_.ime_cursor_)).x : 0.0f);
        if (caret_x - m.edit_.edit_scroll_ > view_w - 2.0f) { m.edit_.edit_scroll_ = caret_x - view_w + 2.0f; }
        if (caret_x - m.edit_.edit_scroll_ < 0.0f)          { m.edit_.edit_scroll_ = caret_x; }
        const bidi_layout* rtl_now = rtl_layout();
        const f32 total = (rtl_now != nullptr ? rtl_now->width() : prefix_width(text_now().size())) + (composing_now ? m.font_.measure(fnt, comp).x : 0.0f);
        if (total - m.edit_.edit_scroll_ < view_w - 2.0f)   { m.edit_.edit_scroll_ = std::max(0.0f, total - view_w + 2.0f); }
    }

    // --- drawing -----------------------------------------------------------------
    anim_slot& a = anim_for(ui, key);
    a.hover  = approach(ui, a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(ui, a.toggle, focused ? 1.0f : 0.0f);

    shape_style field;
    field.radius       = radii(m.style_.rounding * 0.8f);
    field.fill_top     = darken(m.style_.widget_bg, 0.28f);
    field.fill_bottom  = darken(m.style_.widget_bg, 0.12f);
    field.border       = lerp(lerp(m.style_.widget_border, m.style_.accent_hover, a.hover * 0.45f), m.style_.accent, a.toggle);
    field.border_width = 1.0f + a.toggle * 0.5f;
    field.shadow       = m.style_.accent.scaled_alpha(0.32f * a.toggle);
    field.shadow_blur  = 9.0f * a.toggle;
    m.dl_.shape(box, field);

    if (clear_btn) { // a small x
        if (clear_hot) {
            shape_style hot;
            hot.radius      = radii(m.style_.rounding * 0.6f);
            hot.fill_top    = m.style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m.dl_.shape(clear_r, hot);
        }
        const vec2  c   = clear_r.center();
        const color col = clear_hot ? m.style_.text : m.style_.text_dim;
        constexpr f32 arm = 4.0f;
        m.dl_.line({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, col, 1.4f);
        m.dl_.line({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, col, 1.4f);
    }

    if (reveal_btn) { // the eye: open when the text is shown, crossed out when it is hidden
        if (reveal_hot) {
            shape_style hot;
            hot.radius      = radii(m.style_.rounding * 0.6f);
            hot.fill_top    = m.style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m.dl_.shape(reveal_r, hot);
        }
        const vec2  c   = reveal_r.center();
        const color col = lerp(m.style_.text_dim, m.style_.text, reveal_hot || revealed ? 1.0f : 0.0f);
        m.dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y - 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m.dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y + 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m.dl_.circle_filled(c, 2.2f, col);
        if (!revealed) { m.dl_.line({c.x - 6.0f, c.y + 6.0f}, {c.x + 6.0f, c.y - 6.0f}, col, 1.5f); }
    }

    m.dl_.push_clip({{inner.min.x, box.min.y + 1.0f}, {inner.max.x, box.max.y - 1.0f}});
    const f32 text_x = inner.min.x - (focused ? m.edit_.edit_scroll_ : 0.0f);

    const std::string_view shown_text = text_now();
    const bool composing = focused && !readonly && !password && m.input_.ime_len_ != 0;
    const std::string_view comp{m.input_.ime_text_.data(), m.input_.ime_len_};
    std::string disp; // the text with the composition inserted at the caret
    if (composing) {
        const std::size_t at = std::min(m.edit_.edit_cursor_, shown_text.size());
        disp.assign(shown_text.substr(0, at));
        disp.append(comp);
        disp.append(shown_text.substr(at));
    }
    const auto disp_width = [&](std::size_t n) { return m.font_.measure(fnt, std::string_view{disp}.substr(0, n)).x; };
    if (focused && m.edit_.edit_cursor_ != m.edit_.edit_anchor_ && !composing) {
        const std::size_t lo = std::min(m.edit_.edit_cursor_, m.edit_.edit_anchor_);
        const std::size_t hi = std::max(m.edit_.edit_cursor_, m.edit_.edit_anchor_);
        shape_style sel;
        sel.radius      = radii(2.0f);
        sel.fill_top    = m.style_.accent.scaled_alpha(0.45f);
        sel.fill_bottom = m.style_.accent.scaled_alpha(0.45f);
        const f32 xa = prefix_width(lo);
        const f32 xb = prefix_width(hi);
        m.dl_.shape({{text_x + std::min(xa, xb), text_y - 1.0f}, {text_x + std::max(xa, xb), text_y + lh + 1.0f}}, sel);
    }

    if (shown_text.empty() && !focused && !hint.empty()) {
        m.dl_.text({text_x, text_y}, m.style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    } else if (hide) {
        m.dl_.text({text_x, text_y}, m.style_.text, masked, fnt);
    } else if (composing) { // the composition sits in the text, underlined and lightly marked
        const std::size_t at = std::min(m.edit_.edit_cursor_, shown_text.size());
        const f32 x0 = text_x + disp_width(at);
        const f32 x1 = text_x + disp_width(at + comp.size());
        m.dl_.rect_filled({{x0, text_y - 1.0f}, {x1, text_y + lh + 1.0f}}, m.style_.accent.scaled_alpha(0.18f));
        m.dl_.text({text_x, text_y}, m.style_.text, disp, fnt);
        m.dl_.rect_filled({{x0, text_y + lh - 1.0f}, {x1, text_y + lh + 0.5f}}, m.style_.accent_hover);
    } else if (!m.edit_.edit_spans_.empty()) {
        ed_draw(ui, {text_x, text_y}, asc, m.style_.text, shown_text, 0, shown_text.size(), fnt);
    } else {
        m.dl_.text({text_x, text_y}, m.style_.text, shown_text, fnt);
    }

    if (focused) { // where the input method puts its candidate window
        const f32 caret_x = composing ? text_x + disp_width(std::min(m.edit_.edit_cursor_, shown_text.size()) + m.input_.ime_cursor_)
                                      : text_x + prefix_width(m.edit_.edit_cursor_);
        m.edit_.ime_want_   = !password; // (a password field turns the input method off)
        m.edit_.ime_pos_    = {caret_x * m.scale_, (text_y + lh) * m.scale_};
        m.edit_.ime_line_h_ = lh * m.scale_;
        if (caret_visible(ui)) {
            const f32 cx = std::round(caret_x);
            m.dl_.rect_filled({{cx, text_y}, {cx + 1.5f, text_y + lh}}, m.style_.text);
        }
    }
    m.dl_.pop_clip();

    track_edit(ui, key, changed, m.focus_id_ == key);
    return changed;
}

} // namespace strata

// drag sliders and number inputs

#include "strata/context.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace strata {

namespace {

[[nodiscard]] std::string_view format_number(std::span<char> buf, f32 value, int decimals) noexcept
{
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed, std::clamp(decimals, 0, 9));
    return {buf.data(), r.ptr};
}

[[nodiscard]] std::string_view trim_spaces(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return s;
}

[[nodiscard]] bool parse_f32(std::string_view s, f32& out) noexcept
{
    s = trim_spaces(s);
    if (!s.empty() && s.front() == '+') { s.remove_prefix(1); }
    if (s.empty()) { return false; }
    f32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}

[[nodiscard]] bool parse_i32(std::string_view s, int& out) noexcept
{
    s = trim_spaces(s);
    if (!s.empty() && s.front() == '+') { s.remove_prefix(1); }
    if (s.empty()) { return false; }
    int v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) { return false; }
    out = v;
    return true;
}

} // namespace

// one number box: dragging changes the value, a click without dragging starts typing. `box` is the rectangle it owns
bool context::drag_box(std::string_view id_label, const rect& box, f32& value, f32 speed, f32 lo, f32 hi, int decimals,
                       std::string_view suffix, bool integer)
{
    const id      wkey     = widget_id(id_label);
    const font_id f        = current_font();
    const bool    bounded  = lo < hi;
    bool          changed  = false;

    // --- typing ------------------------------------------------------------------------------------------------
    if (number_edit_ == wkey) {
        bool esc = false;
        for (u32 i = 0; i < key_count_; ++i) { esc = esc || keys_[i].k == key::escape; }

        input_rect_     = box;
        input_rect_set_ = true;
        (void)input_text(id_label, number_buf_, {}, input_flags::select_all_on_focus);

        const bool enter = submitted_;
        if (enter) { focus_id_ = 0; } // enter ends the entry too (the buffer is wiped at the end of the frame)
        if (focus_id_ != wkey) {       // the entry is over: apply it, unless it was cancelled
            number_edit_ = 0;
            f32 typed{};
            if (!esc && parse_f32(number_buf_, typed)) {
                if (integer) { typed = std::round(typed); }
                if (bounded) { typed = std::clamp(typed, lo, hi); }
                if (typed != value) {
                    value   = typed;
                    changed = true;
                }
            }
            detail::secure_wipe(number_buf_.data(), number_buf_.size());
            number_buf_.clear();
        }
        return changed;
    }

    // --- dragging ------------------------------------------------------------------------------------------------
    const interaction in = interact(wkey, box);
    if (in.hovered || in.held) { cursor_ = cursor_kind::resize_ew; }

    if (in.held) {
        // a press only starts something: nothing changes until the pointer has left a small dead zone around the
        // spot (so a click never nudges the value), then the distance already travelled is applied at once
        f32 dx = 0.0f;
        if (mouse_pressed_) {
            drag_frac_  = 0.0f;
            drag_start_ = mouse_;
            drag_moved_ = false;
        } else if (!drag_moved_) {
            const vec2 d = mouse_ - drag_start_;
            if (dot(d, d) > 25.0f) {
                drag_moved_ = true;
                dx          = d.x;
            }
        } else {
            dx = mouse_delta_.x;
        }
        if (dx != 0.0f) {
            const f32 mul   = mod_shift_ ? 0.1f : (mod_alt_ ? 10.0f : 1.0f);
            // with a range the box is a ruler: its width spans [lo, hi], so the fill follows the pointer 1:1
            const f32 per_pixel = bounded ? (hi - lo) / std::max(box.width(), 1.0f) : speed;
            f32       delta = dx * per_pixel * mul;
            if (integer) { // whole steps only; the rest carries over to the next frame
                drag_frac_ += delta;
                const f32 whole = std::trunc(drag_frac_);
                drag_frac_ -= whole;
                delta = whole;
            }
            f32 next = value + delta;
            if (bounded) { next = std::clamp(next, lo, hi); }
            if (next != value) {
                value   = next;
                changed = true;
            }
        }
    }
    if (in.pressed && !drag_moved_) { // a click: type the number instead
        std::array<char, 40> buf;
        number_buf_.assign(format_number(buf, value, integer ? 0 : decimals));
        number_edit_ = wkey;
        focus_id_    = wkey;
        wipe_edit_buffer();
        edit_buf_.assign(number_buf_);
        edit_cursor_ = edit_buf_.size();
        edit_anchor_ = 0;
        edit_scroll_ = 0.0f;
        caret_time_  = time_;
        focus_seen_  = true;
        press_claimed_ = true;
    }

    // --- drawing --------------------------------------------------------------------------------------------------
    anim_slot& a = anim_for(wkey);
    a.hover  = approach(a.hover, in.hovered || in.held ? 1.0f : 0.0f);
    a.active = approach(a.active, in.held ? 1.0f : 0.0f);

    const f32 radius = style_.rounding * 0.8f;
    shape_style field = widget_shape(lerp(style_.widget_bg, style_.widget_hover, a.hover), radius);
    field.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.5f), style_.accent, a.active);
    dl_.shape(box, field);

    if (bounded) { // where in its range the value sits
        const f32 t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
        const f32 w = box.width() * t;
        if (w > 1.0f) {
            dl_.push_clip(box);
            shape_style fill;
            fill.radius      = radii(radius);
            fill.fill_top    = style_.accent.scaled_alpha(0.20f + 0.10f * a.hover);
            fill.fill_bottom = fill.fill_top;
            dl_.shape({box.min, {box.min.x + w, box.max.y}}, fill);
            dl_.pop_clip();
        }
    }

    std::array<char, 40> buf;
    std::string text{format_number(buf, value, integer ? 0 : decimals)};
    text.append(suffix);
    const vec2 tsize = font_.measure(f, text);
    dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, style_.text, text, f);

    if (a.hover > 0.02f) { // little arrows: this can be dragged
        const color ac = style_.text_dim.scaled_alpha(a.hover);
        const f32   cy = box.center().y;
        const f32   s  = 3.5f;
        dl_.triangle_filled({box.min.x + 9.0f + s, cy - s}, {box.min.x + 9.0f + s, cy + s}, {box.min.x + 9.0f, cy}, ac);
        dl_.triangle_filled({box.max.x - 9.0f - s, cy - s}, {box.max.x - 9.0f - s, cy + s}, {box.max.x - 9.0f, cy}, ac);
    }
    return changed;
}

bool context::drag_float(std::string_view label, f32& value, f32 speed, f32 lo, f32 hi, int decimals, std::string_view suffix)
{
    if (cur_ == nullptr) {
        return false;
    }
    const field_layout fl = layout_field(visible_label(label), frame_height());
    return drag_box(label, fl.control, value, speed, lo, hi, decimals, suffix, false);
}

bool context::drag_int(std::string_view label, int& value, f32 speed, int lo, int hi, std::string_view suffix)
{
    if (cur_ == nullptr) {
        return false;
    }
    const field_layout fl = layout_field(visible_label(label), frame_height());
    f32 tmp = static_cast<f32>(value);
    if (drag_box(label, fl.control, tmp, speed, static_cast<f32>(lo), static_cast<f32>(hi), 0, suffix, true)) {
        value = static_cast<int>(std::lround(tmp));
        return true;
    }
    return false;
}

bool context::drag_float_n(std::string_view label, f32* values, int count, f32 speed, f32 lo, f32 hi, int decimals)
{
    if (cur_ == nullptr || values == nullptr) {
        return false;
    }
    count = std::clamp(count, 1, 4);
    const field_layout fl = layout_field(visible_label(label), frame_height());

    static constexpr std::array<std::string_view, 4> names = {"##x", "##y", "##z", "##w"};
    static constexpr std::array<u32, 4> tints = {0xff6b6bffu, 0x6bd968ffu, 0x6ba7ffffu, 0xb8b8c8ffu};
    constexpr f32 gap = 4.0f;
    const f32 w = (fl.control.width() - gap * static_cast<f32>(count - 1)) / static_cast<f32>(count);

    bool changed = false;
    push_id(label);
    for (int i = 0; i < count; ++i) {
        const f32  x   = fl.control.min.x + static_cast<f32>(i) * (w + gap);
        const rect box = {{x, fl.control.min.y}, {x + w, fl.control.max.y}};
        changed = drag_box(names[static_cast<std::size_t>(i)], box, values[i], speed, lo, hi, decimals, {}, false) || changed;
        // a colored tick on the left edge tells the components apart
        shape_style tick;
        tick.radius      = radii(1.5f);
        tick.fill_top    = color::from_hex(tints[static_cast<std::size_t>(i)]).scaled_alpha(0.85f);
        tick.fill_bottom = tick.fill_top;
        dl_.shape({{box.min.x + 1.0f, box.min.y + 5.0f}, {box.min.x + 3.5f, box.max.y - 5.0f}}, tick);
    }
    pop_id();
    return changed;
}

// - / + buttons of the input fields
static bool step_button(context& ui, draw_list& dl, const style& st, std::string_view id, const rect& r, bool plus, f32 anim_hover,
                        bool hovered, bool held)
{
    (void)ui;
    (void)id;
    shape_style s;
    s.radius       = radii(st.rounding * 0.8f);
    s.fill_top     = lerp(st.widget_bg, st.widget_hover, anim_hover);
    s.fill_bottom  = lerp(st.widget_bg, st.widget_hover, anim_hover);
    s.border       = lerp(st.widget_border, st.accent_hover, held ? 1.0f : (hovered ? 0.5f : 0.0f));
    s.border_width = st.border_width;
    dl.shape(r, s);
    const vec2 c = r.center();
    const color ink = st.text;
    dl.rect_filled({{c.x - 5.0f, c.y - 1.0f}, {c.x + 5.0f, c.y + 1.0f}}, ink);
    if (plus) { dl.rect_filled({{c.x - 1.0f, c.y - 5.0f}, {c.x + 1.0f, c.y + 5.0f}}, ink); }
    return held;
}

bool context::input_float(std::string_view label, f32& value, f32 step, int decimals)
{
    if (cur_ == nullptr) {
        return false;
    }
    const field_layout fl = layout_field(visible_label(label), frame_height());
    rect field = fl.control;
    const f32 bw = fl.control.height();
    if (step > 0.0f) { field.max.x -= 2.0f * (bw + 4.0f); }

    std::array<char, 40> buf;
    std::string shown{format_number(buf, value, decimals)}; // what the field shows while it is not being edited
    bool changed = false;
    input_rect_     = field;
    input_rect_set_ = true;
    if (input_text(label, shown, {}, input_flags::select_all_on_focus)) {
        f32 parsed{};
        if (parse_f32(shown, parsed) && parsed != value) { // half-written numbers ("-", "1e") leave the value alone
            value   = parsed;
            changed = true;
        }
    }
    if (step > 0.0f) {
        push_id(label);
        const rect minus = {{field.max.x + 4.0f, fl.control.min.y}, {field.max.x + 4.0f + bw, fl.control.max.y}};
        const rect plus  = {{minus.max.x + 4.0f, minus.min.y}, {minus.max.x + 4.0f + bw, minus.max.y}};
        const interaction im = interact(widget_id("##minus"), minus);
        const interaction ip = interact(widget_id("##plus"), plus);
        step_button(*this, dl_, style_, "-", minus, false, im.hovered ? 1.0f : 0.0f, im.hovered, im.held);
        step_button(*this, dl_, style_, "+", plus, true, ip.hovered ? 1.0f : 0.0f, ip.hovered, ip.held);
        if (im.pressed) { value -= step; changed = true; }
        if (ip.pressed) { value += step; changed = true; }
        pop_id();
    }
    return changed;
}

bool context::input_int(std::string_view label, int& value, int step)
{
    if (cur_ == nullptr) {
        return false;
    }
    const field_layout fl = layout_field(visible_label(label), frame_height());
    rect field = fl.control;
    const f32 bw = fl.control.height();
    if (step > 0) { field.max.x -= 2.0f * (bw + 4.0f); }

    std::array<char, 40> buf;
    std::string shown{format_number(buf, static_cast<f32>(value), 0)};
    if (std::abs(value) >= (1 << 24)) { // beyond what a float holds exactly
        const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), value);
        shown.assign(buf.data(), r.ptr);
    }
    bool changed = false;
    input_rect_     = field;
    input_rect_set_ = true;
    if (input_text(label, shown, {}, input_flags::select_all_on_focus)) {
        int parsed{};
        if (parse_i32(shown, parsed) && parsed != value) {
            value   = parsed;
            changed = true;
        }
    }
    if (step > 0) {
        push_id(label);
        const rect minus = {{field.max.x + 4.0f, fl.control.min.y}, {field.max.x + 4.0f + bw, fl.control.max.y}};
        const rect plus  = {{minus.max.x + 4.0f, minus.min.y}, {minus.max.x + 4.0f + bw, minus.max.y}};
        const interaction im = interact(widget_id("##minus"), minus);
        const interaction ip = interact(widget_id("##plus"), plus);
        step_button(*this, dl_, style_, "-", minus, false, im.hovered ? 1.0f : 0.0f, im.hovered, im.held);
        step_button(*this, dl_, style_, "+", plus, true, ip.hovered ? 1.0f : 0.0f, ip.hovered, ip.held);
        if (im.pressed) { value -= step; changed = true; }
        if (ip.pressed) { value += step; changed = true; }
        pop_id();
    }
    return changed;
}

} // namespace strata

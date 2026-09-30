// colour edit field and picker

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

namespace {

struct hsv {
    f32 h{}; // 0..1
    f32 s{};
    f32 v{};
};

[[nodiscard]] hsv to_hsv(color c) noexcept
{
    const f32 r = static_cast<f32>(c.r) / 255.0f;
    const f32 g = static_cast<f32>(c.g) / 255.0f;
    const f32 b = static_cast<f32>(c.b) / 255.0f;
    const f32 mx = std::max({r, g, b});
    const f32 mn = std::min({r, g, b});
    const f32 d  = mx - mn;

    hsv out;
    out.v = mx;
    out.s = mx > 0.0f ? d / mx : 0.0f;
    if (d > 0.0f) {
        f32 h;
        if (mx == r)      { h = std::fmod((g - b) / d, 6.0f); }
        else if (mx == g) { h = (b - r) / d + 2.0f; }
        else              { h = (r - g) / d + 4.0f; }
        h /= 6.0f;
        out.h = h < 0.0f ? h + 1.0f : h;
    }
    return out;
}

[[nodiscard]] color from_hsv(hsv c, u8 alpha) noexcept
{
    const f32 h6 = std::clamp(c.h, 0.0f, 0.99999f) * 6.0f;
    const int i  = static_cast<int>(h6);
    const f32 f  = h6 - static_cast<f32>(i);
    const f32 p  = c.v * (1.0f - c.s);
    const f32 q  = c.v * (1.0f - c.s * f);
    const f32 t  = c.v * (1.0f - c.s * (1.0f - f));

    f32 r, g, b;
    switch (i) {
    case 0:  r = c.v; g = t;   b = p;   break;
    case 1:  r = q;   g = c.v; b = p;   break;
    case 2:  r = p;   g = c.v; b = t;   break;
    case 3:  r = p;   g = q;   b = c.v; break;
    case 4:  r = t;   g = p;   b = c.v; break;
    default: r = c.v; g = p;   b = q;   break;
    }
    const auto to_byte = [](f32 x) { return static_cast<u8>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return {to_byte(r), to_byte(g), to_byte(b), alpha};
}

[[nodiscard]] std::optional<color> parse_hex(std::string_view s) noexcept
{
    if (!s.empty() && s.front() == '#') { s.remove_prefix(1); }
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) {
        return std::nullopt;
    }
    std::array<u8, 8> nib{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9')      { nib[i] = static_cast<u8>(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { nib[i] = static_cast<u8>(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { nib[i] = static_cast<u8>(ch - 'A' + 10); }
        else { return std::nullopt; }
    }
    if (s.size() <= 4) { // #rgb / #rgba: each digit doubled
        const auto dbl = [&](std::size_t i) { return static_cast<u8>(nib[i] * 17); };
        return color{dbl(0), dbl(1), dbl(2), s.size() == 4 ? dbl(3) : u8{255}};
    }
    const auto byte = [&](std::size_t i) { return static_cast<u8>(nib[i] * 16 + nib[i + 1]); };
    return color{byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : u8{255}};
}

[[nodiscard]] std::string format_hex(color c, bool with_alpha)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "#";
    const auto put = [&](u8 v) {
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 15]);
    };
    put(c.r);
    put(c.g);
    put(c.b);
    if (with_alpha) { put(c.a); }
    return out;
}

} // namespace

void context::draw_checker(const rect& r, f32 cell)
{
    m_->dl_.rect_filled(r, color{204, 204, 204, 255});
    const color dark{150, 150, 150, 255};
    int row = 0;
    for (f32 y = r.min.y; y < r.max.y; y += cell, ++row) {
        int col = 0;
        for (f32 x = r.min.x; x < r.max.x; x += cell, ++col) {
            if (((row + col) & 1) != 0) {
                m_->dl_.rect_filled({{x, y}, {std::min(x + cell, r.max.x), std::min(y + cell, r.max.y)}}, dark);
            }
        }
    }
}

bool context::picker_body(id key, color& c, color_flags flags)
{
    const bool with_alpha = !has_flag(flags, color_flags::no_alpha);

    // keep the hsv the user is dragging; only reload when the color was changed from outside
    if (m_->pick_.pick_key_ != key || m_->pick_.pick_last_ != c) {
        const hsv h = to_hsv(c);
        // hue / saturation are undefined for grays and black: keep what the user last had
        if (m_->pick_.pick_key_ == key && (c.r == c.g && c.g == c.b)) {
            m_->pick_.pick_v_ = h.v;
            m_->pick_.pick_s_ = 0.0f;
        } else {
            m_->pick_.pick_h_ = h.s > 0.0f ? h.h : (m_->pick_.pick_key_ == key ? m_->pick_.pick_h_ : 0.0f);
            m_->pick_.pick_s_ = h.s;
            m_->pick_.pick_v_ = h.v;
        }
        m_->pick_.pick_key_  = key;
        m_->pick_.pick_last_ = c;
    }

    bool changed = false;
    const f32 w        = m_->layout_.width;
    const f32 sv_h     = 150.0f;
    const f32 bar_h    = 14.0f;
    const id  seed     = current_seed();
    u8 alpha           = c.a;

    {
        const rect r = layout_place({w, sv_h});
        const interaction in = interact(part_id(part::sat_value, seed), r);
        if (in.held) {
            m_->pick_.pick_s_ = std::clamp((m_->input_.mouse_.x - r.min.x) / r.width(), 0.0f, 1.0f);
            m_->pick_.pick_v_ = 1.0f - std::clamp((m_->input_.mouse_.y - r.min.y) / r.height(), 0.0f, 1.0f);
            changed = true;
        }
        const color hue = from_hsv({m_->pick_.pick_h_, 1.0f, 1.0f}, 255);
        m_->dl_.rect_gradient(r, color{255, 255, 255, 255}, hue, hue, color{255, 255, 255, 255});
        m_->dl_.rect_gradient(r, color{0, 0, 0, 0}, color{0, 0, 0, 0}, color{0, 0, 0, 255}, color{0, 0, 0, 255});
        m_->dl_.rect_outline(r, m_->style_.border, 0.0f, 1.0f);

        const vec2 p{r.min.x + m_->pick_.pick_s_ * r.width(), r.min.y + (1.0f - m_->pick_.pick_v_) * r.height()};
        shape_style ring;
        ring.radius       = radii(6.0f);
        ring.fill_top     = from_hsv({m_->pick_.pick_h_, m_->pick_.pick_s_, m_->pick_.pick_v_}, 255);
        ring.fill_bottom  = ring.fill_top;
        ring.border       = color{255, 255, 255, 255};
        ring.border_width = 2.0f;
        ring.shadow       = color{0, 0, 0, 150};
        ring.shadow_blur  = 3.0f;
        m_->dl_.push_clip(r.expanded(6.0f));
        m_->dl_.shape({{p.x - 6.0f, p.y - 6.0f}, {p.x + 6.0f, p.y + 6.0f}}, ring);
        m_->dl_.pop_clip();
    }

    {
        const rect r = layout_place({w, bar_h});
        const interaction in = interact(part_id(part::hue_bar, seed), r);
        if (in.held) {
            m_->pick_.pick_h_ = std::clamp((m_->input_.mouse_.x - r.min.x) / r.width(), 0.0f, 0.9999f);
            changed = true;
        }
        static constexpr std::array<u32, 7> stops = {0xff0000ffu, 0xffff00ffu, 0x00ff00ffu, 0x00ffffffu,
                                                      0x0000ffffu, 0xff00ffffu, 0xff0000ffu};
        for (std::size_t i = 0; i < 6; ++i) {
            const f32 x0 = r.min.x + r.width() * static_cast<f32>(i) / 6.0f;
            const f32 x1 = r.min.x + r.width() * static_cast<f32>(i + 1) / 6.0f;
            const color a = color::from_hex(stops[i]);
            const color b = color::from_hex(stops[i + 1]);
            m_->dl_.rect_gradient({{x0, r.min.y}, {x1, r.max.y}}, a, b, b, a);
        }
        m_->dl_.rect_outline(r, m_->style_.border, 0.0f, 1.0f);

        const f32 x = r.min.x + m_->pick_.pick_h_ * r.width();
        shape_style knob;
        knob.radius       = radii(3.0f);
        knob.fill_top     = from_hsv({m_->pick_.pick_h_, 1.0f, 1.0f}, 255);
        knob.fill_bottom  = knob.fill_top;
        knob.border       = color{255, 255, 255, 255};
        knob.border_width = 2.0f;
        knob.shadow       = color{0, 0, 0, 140};
        knob.shadow_blur  = 3.0f;
        m_->dl_.push_clip(r.expanded(6.0f));
        m_->dl_.shape({{x - 3.5f, r.min.y - 2.0f}, {x + 3.5f, r.max.y + 2.0f}}, knob);
        m_->dl_.pop_clip();
    }

    if (with_alpha) {
        const rect r = layout_place({w, bar_h});
        const interaction in = interact(part_id(part::alpha_bar, seed), r);
        if (in.held) {
            alpha   = static_cast<u8>(std::clamp((m_->input_.mouse_.x - r.min.x) / r.width(), 0.0f, 1.0f) * 255.0f + 0.5f);
            changed = true;
        }
        draw_checker(r, 7.0f);
        const color solid = from_hsv({m_->pick_.pick_h_, m_->pick_.pick_s_, m_->pick_.pick_v_}, 255);
        const color clear{solid.r, solid.g, solid.b, 0};
        m_->dl_.rect_gradient(r, clear, solid, solid, clear);
        m_->dl_.rect_outline(r, m_->style_.border, 0.0f, 1.0f);

        const f32 x = r.min.x + static_cast<f32>(alpha) / 255.0f * r.width();
        shape_style knob;
        knob.radius       = radii(3.0f);
        knob.fill_top     = color{solid.r, solid.g, solid.b, alpha};
        knob.fill_bottom  = knob.fill_top;
        knob.border       = color{255, 255, 255, 255};
        knob.border_width = 2.0f;
        knob.shadow       = color{0, 0, 0, 140};
        knob.shadow_blur  = 3.0f;
        m_->dl_.push_clip(r.expanded(6.0f));
        m_->dl_.shape({{x - 3.5f, r.min.y - 2.0f}, {x + 3.5f, r.max.y + 2.0f}}, knob);
        m_->dl_.pop_clip();
    }

    if (changed) {
        c          = from_hsv({m_->pick_.pick_h_, m_->pick_.pick_s_, m_->pick_.pick_v_}, with_alpha ? alpha : c.a);
        m_->pick_.pick_last_ = c;
    }

    {
        const f32  fh = frame_height();
        const rect sw = layout_place({fh, fh});
        if (c.a != 255) { draw_checker(sw, 6.0f); }
        shape_style chip;
        chip.radius       = radii(m_->style_.rounding * 0.6f);
        chip.fill_top     = c;
        chip.fill_bottom  = c;
        chip.border       = m_->style_.widget_border;
        chip.border_width = m_->style_.border_width;
        m_->dl_.shape(sw, chip);

        same_line();
        set_next_item_width(w - fh - m_->style_.item_spacing);
        std::string hex = format_hex(c, with_alpha);
        if (input_text("##hex", hex, "#rrggbb")) {
            if (const auto parsed = parse_hex(hex)) {
                c = *parsed;
                if (!with_alpha) { c.a = m_->pick_.pick_last_.a; }
                const hsv h = to_hsv(c);
                m_->pick_.pick_h_     = h.s > 0.0f ? h.h : m_->pick_.pick_h_;
                m_->pick_.pick_s_     = h.s;
                m_->pick_.pick_v_     = h.v;
                m_->pick_.pick_last_  = c;
                changed     = true;
            }
        }
    }
    return changed;
}

bool context::color_picker(std::string_view label, color& c, color_flags flags)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const id key = widget_id(label);
    const std::string_view shown = visible_label(label);
    if (!shown.empty()) {
        text_dim(shown);
    }
    push_id(label);
    const bool changed = picker_body(key, c, flags);
    // being edited: a part is held or the hex field has the keyboard (parts are keyed in this scope)
    const id   seed    = current_seed();
    const bool engaged = (m_->active_ != 0 && (m_->active_ == part_id(part::sat_value, seed) || m_->active_ == part_id(part::hue_bar, seed) ||
                                           m_->active_ == part_id(part::alpha_bar, seed))) ||
                         (m_->focus_id_ != 0 && m_->focus_id_ == hash_id("##hex", seed));
    pop_id();
    track_edit(key, changed, engaged);
    return changed;
}

bool context::color_edit(std::string_view label, color& c, color_flags flags)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    const bool with_alpha = !has_flag(flags, color_flags::no_alpha);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool open = m_->popup_.popup_has(key);
    if (in.pressed) {
        if (open) {
            m_->popup_.popup_close(key);
            open = false;
        } else {
            open = popup_push(key);
        }
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    m_->dl_.shape(box, field);

    const f32  inset = 4.0f;
    const rect chip  = {{box.min.x + inset, box.min.y + inset}, {box.min.x + inset + (box.height() - 2.0f * inset) * 1.6f, box.max.y - inset}};
    if (c.a != 255) { draw_checker(chip, 6.0f); }
    shape_style swatch;
    swatch.radius       = radii(m_->style_.rounding * 0.5f);
    swatch.fill_top     = c;
    swatch.fill_bottom  = c;
    swatch.border       = color{0, 0, 0, 90};
    swatch.border_width = 1.0f;
    m_->dl_.shape(chip, swatch);

    const std::string hex = format_hex(c, with_alpha && c.a != 255);
    const vec2 tsize = m_->font_.measure(f, hex);
    m_->dl_.text({chip.max.x + 9.0f, box.min.y + (box.height() - tsize.y) * 0.5f}, m_->style_.text, hex, f);

    bool changed = false;
    if (open) {
        const f32 fh = frame_height();
        f32 h = 2.0f * m_->style_.padding + 150.0f + 14.0f + fh + 2.0f * m_->style_.item_spacing;
        h += m_->style_.item_spacing;
        if (with_alpha) { h += 14.0f + m_->style_.item_spacing; }
        const f32 popup_w = std::max(box.width(), 250.0f);
        if (begin_popup_at(key, box, {popup_w, h})) {
            push_id(label);
            changed = picker_body(key, c, flags);
            pop_id();
            end_popup_at();
        }
    }
    track_edit(key, changed, m_->popup_.popup_has(key));
    return changed;
}

} // namespace strata

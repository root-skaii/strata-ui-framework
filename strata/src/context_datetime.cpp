// date and time pickers: a field that opens a calendar / time grid popup

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace strata {

namespace {

enum class picker_icon { calendar, clock };

} // namespace

// the closed field: like a dropdown, with a small calendar / clock at the right end
void context::picker_field(id key, const rect& box, const interaction& in, bool open, std::string_view text, int icon)
{
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    m_->dl_.shape(box, field);

    const font_id f = current_font();
    const vec2    ts = label_size(f, text);
    m_->dl_.push_clip({box.min, {box.max.x - m_->style_.frame_padding.x - 20.0f, box.max.y}});
    label_draw({box.min.x + m_->style_.frame_padding.x, box.min.y + (box.height() - ts.y) * 0.5f}, m_->style_.text, text, f);
    m_->dl_.pop_clip();

    const color ic = lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle));
    const vec2  c{box.max.x - m_->style_.frame_padding.x - 6.0f, box.center().y};
    if (icon == static_cast<int>(picker_icon::calendar)) {
        const rect page = {{c.x - 7.0f, c.y - 6.0f}, {c.x + 7.0f, c.y + 6.5f}};
        m_->dl_.rect_outline(page, ic, 2.5f, 1.3f);
        m_->dl_.line({page.min.x, c.y - 2.0f}, {page.max.x, c.y - 2.0f}, ic, 1.3f);
        m_->dl_.line({c.x - 3.0f, page.min.y - 1.5f}, {c.x - 3.0f, page.min.y + 1.5f}, ic, 1.3f);
        m_->dl_.line({c.x + 3.0f, page.min.y - 1.5f}, {c.x + 3.0f, page.min.y + 1.5f}, ic, 1.3f);
    } else {
        m_->dl_.circle(c, 6.5f, ic, 1.3f);
        m_->dl_.line(c, {c.x, c.y - 4.0f}, ic, 1.3f);
        m_->dl_.line(c, {c.x + 3.0f, c.y + 1.5f}, ic, 1.3f);
    }
}

// one cell of a grid (a day, an hour, a step button): a rounded highlight, the text centered
bool context::pick_cell(id key, const rect& r, std::string_view text, bool selected, bool faded, bool marked)
{
    const interaction in = interact(key, r);
    const press_anim  pa = button_anim(key, in);
    const f32 radius = m_->style_.rounding * 0.6f;
    if (selected) {
        shape_style s;
        s.radius      = radii(radius);
        s.fill_top    = lerp(m_->style_.accent, m_->style_.accent_hover, pa.hover);
        s.fill_bottom = m_->style_.accent;
        m_->dl_.shape(r, s);
    } else if (pa.hover > 0.01f) {
        shape_style s;
        s.radius      = radii(radius);
        s.fill_top    = m_->style_.widget_hover.scaled_alpha(0.9f * pa.hover);
        s.fill_bottom = s.fill_top;
        m_->dl_.shape(r, s);
    }
    if (marked && !selected) {
        m_->dl_.rect_outline(r, m_->style_.accent, radius, 1.3f);
    }
    if (!text.empty()) {
        const font_id f  = current_font();
        const vec2    ts = label_size(f, text);
        const color   tc = selected ? color{255, 255, 255, 255} : faded ? m_->style_.text_dim.scaled_alpha(0.55f) : m_->style_.text;
        label_draw({r.min.x + (r.width() - ts.x) * 0.5f, r.min.y + (r.height() - ts.y) * 0.5f}, tc, text, f);
    }
    return in.pressed;
}

bool context::calendar_body(date& value, bool close_on_pick)
{
    bool changed = false;
    if (m_->cal_key_ != m_->popup_id_) { // just opened: show the month of the value
        m_->cal_key_   = m_->popup_id_;
        m_->cal_year_  = value.year;
        m_->cal_month_ = value.month;
    }
    const f32 saved_spacing = m_->style_.item_spacing;
    m_->style_.item_spacing = 2.0f;

    const f32  w    = m_->layout_.width;
    const f32  head = frame_height() - 4.0f;
    const rect bar  = layout_place({w, head});
    const font_id f = current_font();

    // « ‹ month year › »
    struct step { f32 x; int months; };
    const std::array<step, 4> steps = {{{bar.min.x, -12}, {bar.min.x + head, -1}, {bar.max.x - 2.0f * head, 1}, {bar.max.x - head, 12}}};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const rect r = rect::from_size({steps[i].x, bar.min.y}, {head, head});
        if (pick_cell(hash_id("##ym", static_cast<id>(i)), r, {}, false, false, false)) {
            const date moved = add_months({m_->cal_year_, m_->cal_month_, 1}, steps[i].months);
            m_->cal_year_  = moved.year;
            m_->cal_month_ = moved.month;
        }
        const vec2  c  = r.center();
        const f32   k  = 3.5f;
        const bool  left = steps[i].months < 0;
        const int   n  = std::abs(steps[i].months) == 12 ? 2 : 1;
        for (int j = 0; j < n; ++j) {
            const f32 ox = static_cast<f32>(j) * 4.0f - (n == 2 ? 2.0f : 0.0f);
            const f32 d  = left ? -1.0f : 1.0f;
            m_->dl_.line({c.x + ox - d * k * 0.5f, c.y - k}, {c.x + ox + d * k * 0.5f, c.y}, m_->style_.text, 1.4f);
            m_->dl_.line({c.x + ox + d * k * 0.5f, c.y}, {c.x + ox - d * k * 0.5f, c.y + k}, m_->style_.text, 1.4f);
        }
    }
    const std::string title = std::format("{} {}", month_name(m_->cal_month_), m_->cal_year_);
    const vec2 tsz = label_size(f, title);
    label_draw({bar.center().x - tsz.x * 0.5f, bar.min.y + (head - tsz.y) * 0.5f}, m_->style_.text, title, f);

    // Mo .. Su
    const f32  cw   = w / 7.0f;
    const f32  lh   = m_->font_.line_height(f);
    const rect days = layout_place({w, lh + 4.0f});
    for (i32 i = 0; i < 7; ++i) {
        const std::string_view name = weekday_short(i);
        const vec2 ns = label_size(f, name);
        label_draw({days.min.x + cw * (static_cast<f32>(i) + 0.5f) - ns.x * 0.5f, days.min.y + 2.0f},
                   i >= 5 ? m_->style_.text_dim.scaled_alpha(0.7f) : m_->style_.text_dim, name, f);
    }

    // six weeks starting on the Monday on or before the 1st
    const date first  = {m_->cal_year_, m_->cal_month_, 1};
    const date start  = add_days(first, -weekday(first));
    const date now_d  = today();
    const f32  ch     = frame_height() - 6.0f;
    for (i32 row = 0; row < 6; ++row) {
        const rect line = layout_place({w, ch});
        for (i32 col = 0; col < 7; ++col) {
            const date d = add_days(start, row * 7 + col);
            const rect r = rect::from_size({line.min.x + cw * static_cast<f32>(col), line.min.y}, {cw, ch});
            const std::string num = std::to_string(d.day);
            const id key = hash_id("##day", static_cast<id>(row * 7 + col));
            if (pick_cell(key, r.expanded(-1.0f), num, d == value, d.month != m_->cal_month_, d == now_d)) {
                value   = d;
                changed = true;
                if (d.month != m_->cal_month_) { m_->cal_year_ = d.year; m_->cal_month_ = d.month; }
                if (close_on_pick) { close_popup(); }
            }
        }
    }

    m_->style_.item_spacing = saved_spacing;
    return changed;
}

bool context::time_body(time_of_day& value, bool seconds)
{
    bool changed = false;
    const f32 saved_spacing = m_->style_.item_spacing;
    m_->style_.item_spacing = 2.0f;

    const f32 w  = m_->layout_.width;
    const f32 ch = frame_height() - 6.0f;

    // a caption, the values in a grid, and (minutes / seconds) a - / + pair for the ones in between
    const auto section = [&](const char* caption, i32 count, i32 step, i32 cols, i32& v, bool fine) {
        text_dim(caption);
        const f32 cw = w / static_cast<f32>(cols);
        for (i32 i = 0; i < count; i += cols) {
            const rect line = layout_place({w, ch});
            for (i32 c = 0; c < cols && i + c < count; ++c) {
                const i32  n = (i + c) * step;
                const rect r = rect::from_size({line.min.x + cw * static_cast<f32>(c), line.min.y}, {cw, ch});
                const id   key = hash_id(caption, static_cast<id>(n + 1000));
                if (pick_cell(key, r.expanded(-1.0f), std::format("{:02}", n), v == n, false, false)) {
                    v       = n;
                    changed = true;
                }
            }
        }
        if (fine) {
            const rect line = layout_place({w, ch});
            const f32  bw   = 34.0f;
            const rect minus = rect::from_size(line.min, {bw, ch});
            const rect plus  = rect::from_size({line.max.x - bw, line.min.y}, {bw, ch});
            const i32  limit = count * step;
            if (pick_cell(hash_id(caption, 2000), minus, "-", false, false, false)) { v = (v + limit - 1) % limit; changed = true; }
            if (pick_cell(hash_id(caption, 2001), plus, "+", false, false, false))  { v = (v + 1) % limit; changed = true; }
            const std::string shown = std::format("{:02}", v);
            const font_id f  = current_font();
            const vec2    ts = label_size(f, shown);
            label_draw({line.center().x - ts.x * 0.5f, line.min.y + (ch - ts.y) * 0.5f}, m_->style_.text, shown, f);
        }
    };
    section("hour", 24, 1, 6, value.hour, false);
    spacing(4.0f);
    section("minute", 12, 5, 6, value.minute, true);
    if (seconds) {
        spacing(4.0f);
        section("second", 12, 5, 6, value.second, true);
    }
    m_->style_.item_spacing = saved_spacing;
    return changed;
}

bool context::date_picker(std::string_view label, date& value)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(value)) { value = clamp_date(value); }
    const id key = widget_id(label);
    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const interaction  in  = interact(key, fl.control);
    push_id(label);
    if (in.pressed) { m_->cal_key_ = 0; toggle_popup("##calendar"); }
    picker_field(key, fl.control, in, popup_is_open("##calendar"), to_string(value), static_cast<int>(picker_icon::calendar));

    bool changed = false;
    if (auto p = popup("##calendar", std::max(fl.control.width(), 260.0f))) {
        changed = calendar_body(value, true);
        spacing(2.0f);
        if (button("Today")) {
            value   = today();
            m_->cal_key_ = 0;
            changed = true;
            close_popup();
        }
    }
    const bool engaged = popup_is_open("##calendar"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(key, changed, engaged);
    return changed;
}

bool context::time_picker(std::string_view label, time_of_day& value, bool seconds)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(value)) { value = clamp_time(value); }
    const id key = widget_id(label);
    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const interaction  in  = interact(key, fl.control);
    push_id(label);
    if (in.pressed) { toggle_popup("##clock"); }
    picker_field(key, fl.control, in, popup_is_open("##clock"), to_string(value, seconds), static_cast<int>(picker_icon::clock));

    bool changed = false;
    if (auto p = popup("##clock", std::max(fl.control.width(), 240.0f))) {
        changed = time_body(value, seconds);
        spacing(2.0f);
        if (button("Now")) {
            value   = now();
            changed = true;
        }
        same_line();
        if (button("Done")) { close_popup(); }
    }
    const bool engaged = popup_is_open("##clock"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(key, changed, engaged);
    return changed;
}

bool context::datetime_picker(std::string_view label, date& d, time_of_day& t, bool seconds)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(d)) { d = clamp_date(d); }
    if (!is_valid(t)) { t = clamp_time(t); }
    const id key = widget_id(label);
    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const interaction  in  = interact(key, fl.control);
    push_id(label);
    if (in.pressed) { m_->cal_key_ = 0; toggle_popup("##datetime"); }
    picker_field(key, fl.control, in, popup_is_open("##datetime"), to_string(d) + " " + to_string(t, seconds),
                 static_cast<int>(picker_icon::calendar));

    bool changed = false;
    if (auto p = popup("##datetime", std::max(fl.control.width(), 260.0f))) {
        changed = calendar_body(d, false);
        separator();
        changed = time_body(t, seconds) || changed;
        spacing(2.0f);
        if (button("Now")) {
            d       = today();
            t       = now();
            m_->cal_key_ = 0;
            changed = true;
        }
        same_line();
        if (button("Done")) { close_popup(); }
    }
    const bool engaged = popup_is_open("##datetime"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(key, changed, engaged);
    return changed;
}

} // namespace strata

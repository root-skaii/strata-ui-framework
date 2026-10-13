// date and time pickers: a field that opens a calendar / time grid popup

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "context_helpers.hpp"
#include "core/part_id.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace strata {

using namespace internal;

static void picker_field(context& ui, id key, const rect& box, const interaction& in, bool open, std::string_view text, int icon);
static bool pick_cell(context& ui, id key, const rect& r, std::string_view text, bool selected, bool faded, bool marked);
static bool calendar_body(context& ui, date& value, bool close_on_pick);
static bool time_body(context& ui, time_of_day& value, bool seconds);

namespace {

enum class picker_icon { calendar, clock };

} // namespace

// the closed field: like a dropdown, with a small calendar / clock at the right end
static void picker_field(context& ui, id key, const rect& box, const interaction& in, bool open, std::string_view text, int icon)
{
    auto& m = context_access::of(ui);
    anim_slot& a = anim_for(ui, key);
    a.hover  = approach(ui, a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(ui, a.toggle, open ? 1.0f : 0.0f);

    shape_style field = widget_shape(ui, lerp(m.style_.widget_bg, m.style_.widget_hover, a.hover), m.style_.rounding * 0.8f);
    field.border = lerp(lerp(m.style_.widget_border, m.style_.accent_hover, a.hover * 0.45f), m.style_.accent, a.toggle);
    m.dl_.shape(box, field);

    const font_id f = ui.current_font();
    const vec2    ts = label_size(ui, f, text);
    m.dl_.push_clip({box.min, {box.max.x - m.style_.frame_padding.x - 20.0f, box.max.y}});
    label_draw(ui, {box.min.x + m.style_.frame_padding.x, box.min.y + (box.height() - ts.y) * 0.5f}, m.style_.text, text, f);
    m.dl_.pop_clip();

    const color ic = lerp(m.style_.text_dim, m.style_.text, std::max(a.hover, a.toggle));
    const vec2  c{box.max.x - m.style_.frame_padding.x - 6.0f, box.center().y};
    if (icon == static_cast<int>(picker_icon::calendar)) {
        const rect page = {{c.x - 7.0f, c.y - 6.0f}, {c.x + 7.0f, c.y + 6.5f}};
        m.dl_.rect_outline(page, ic, 2.5f, 1.3f);
        m.dl_.line({page.min.x, c.y - 2.0f}, {page.max.x, c.y - 2.0f}, ic, 1.3f);
        m.dl_.line({c.x - 3.0f, page.min.y - 1.5f}, {c.x - 3.0f, page.min.y + 1.5f}, ic, 1.3f);
        m.dl_.line({c.x + 3.0f, page.min.y - 1.5f}, {c.x + 3.0f, page.min.y + 1.5f}, ic, 1.3f);
    } else {
        m.dl_.circle(c, 6.5f, ic, 1.3f);
        m.dl_.line(c, {c.x, c.y - 4.0f}, ic, 1.3f);
        m.dl_.line(c, {c.x + 3.0f, c.y + 1.5f}, ic, 1.3f);
    }
}

// one cell of a grid (a day, an hour, a step button): a rounded highlight, the text centered
static bool pick_cell(context& ui, id key, const rect& r, std::string_view text, bool selected, bool faded, bool marked)
{
    auto& m = context_access::of(ui);
    const interaction in = interact(ui, key, r);
    const press_anim  pa = button_anim(ui, key, in);
    const f32 radius = m.style_.rounding * 0.6f;
    if (selected) {
        shape_style s;
        s.radius      = radii(radius);
        s.fill_top    = lerp(m.style_.accent, m.style_.accent_hover, pa.hover);
        s.fill_bottom = m.style_.accent;
        m.dl_.shape(r, s);
    } else if (pa.hover > 0.01f) {
        shape_style s;
        s.radius      = radii(radius);
        s.fill_top    = m.style_.widget_hover.scaled_alpha(0.9f * pa.hover);
        s.fill_bottom = s.fill_top;
        m.dl_.shape(r, s);
    }
    if (marked && !selected) {
        m.dl_.rect_outline(r, m.style_.accent, radius, 1.3f);
    }
    if (!text.empty()) {
        const font_id f  = ui.current_font();
        const vec2    ts = label_size(ui, f, text);
        const color   tc = selected ? color{255, 255, 255, 255} : faded ? m.style_.text_dim.scaled_alpha(0.55f) : m.style_.text;
        label_draw(ui, {r.min.x + (r.width() - ts.x) * 0.5f, r.min.y + (r.height() - ts.y) * 0.5f}, tc, text, f);
    }
    return in.pressed;
}

static bool calendar_body(context& ui, date& value, bool close_on_pick)
{
    auto& m = context_access::of(ui);
    bool changed = false;
    if (m.date_.cal_key_ != m.popup_.popup_drawing_key()) { // just opened: show the month of the value
        m.date_.cal_key_   = m.popup_.popup_drawing_key();
        m.date_.cal_year_  = value.year;
        m.date_.cal_month_ = value.month;
    }
    const f32 saved_spacing = m.style_.item_spacing;
    m.style_.item_spacing = 2.0f;

    const f32  w    = m.layout_.width;
    const f32  head = ui.frame_height() - 4.0f;
    const rect bar  = layout_place(ui, {w, head});
    const font_id f = ui.current_font();

    // « ‹ month year › »
    struct step { f32 x; int months; };
    const std::array<step, 4> steps = {{{bar.min.x, -12}, {bar.min.x + head, -1}, {bar.max.x - 2.0f * head, 1}, {bar.max.x - head, 12}}};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const rect r = rect::from_size({steps[i].x, bar.min.y}, {head, head});
        if (pick_cell(ui, part_id(part::year_month, static_cast<id>(i)), r, {}, false, false, false)) {
            const date moved = add_months({m.date_.cal_year_, m.date_.cal_month_, 1}, steps[i].months);
            m.date_.cal_year_  = moved.year;
            m.date_.cal_month_ = moved.month;
        }
        const vec2  c  = r.center();
        const f32   k  = 3.5f;
        const bool  left = steps[i].months < 0;
        const int   n  = std::abs(steps[i].months) == 12 ? 2 : 1;
        for (int j = 0; j < n; ++j) {
            const f32 ox = static_cast<f32>(j) * 4.0f - (n == 2 ? 2.0f : 0.0f);
            const f32 d  = left ? -1.0f : 1.0f;
            m.dl_.line({c.x + ox - d * k * 0.5f, c.y - k}, {c.x + ox + d * k * 0.5f, c.y}, m.style_.text, 1.4f);
            m.dl_.line({c.x + ox + d * k * 0.5f, c.y}, {c.x + ox - d * k * 0.5f, c.y + k}, m.style_.text, 1.4f);
        }
    }
    const std::string title = std::format("{} {}", m.strings_.months[static_cast<std::size_t>(std::clamp(m.date_.cal_month_, 1, 12) - 1)], m.date_.cal_year_);
    const vec2 tsz = label_size(ui, f, title);
    label_draw(ui, {bar.center().x - tsz.x * 0.5f, bar.min.y + (head - tsz.y) * 0.5f}, m.style_.text, title, f);

    // Mo .. Su
    const f32  cw   = w / 7.0f;
    const f32  lh   = m.font_.line_height(f);
    const rect days = layout_place(ui, {w, lh + 4.0f});
    for (i32 i = 0; i < 7; ++i) {
        const std::string_view name = m.strings_.weekdays[static_cast<std::size_t>(i)];
        const vec2 ns = label_size(ui, f, name);
        label_draw(ui, {days.min.x + cw * (static_cast<f32>(i) + 0.5f) - ns.x * 0.5f, days.min.y + 2.0f},
                   i >= 5 ? m.style_.text_dim.scaled_alpha(0.7f) : m.style_.text_dim, name, f);
    }

    // six weeks starting on the Monday on or before the 1st
    const date first  = {m.date_.cal_year_, m.date_.cal_month_, 1};
    const date start  = add_days(first, -weekday(first));
    const date now_d  = today();
    const f32  ch     = ui.frame_height() - 6.0f;
    for (i32 row = 0; row < 6; ++row) {
        const rect line = layout_place(ui, {w, ch});
        for (i32 col = 0; col < 7; ++col) {
            const date d = add_days(start, row * 7 + col);
            const rect r = rect::from_size({line.min.x + cw * static_cast<f32>(col), line.min.y}, {cw, ch});
            const std::string num = std::to_string(d.day);
            const id key = part_id(part::calendar_day, static_cast<id>(row * 7 + col));
            if (pick_cell(ui, key, r.expanded(-1.0f), num, d == value, d.month != m.date_.cal_month_, d == now_d)) {
                value   = d;
                changed = true;
                if (d.month != m.date_.cal_month_) { m.date_.cal_year_ = d.year; m.date_.cal_month_ = d.month; }
                if (close_on_pick) { ui.close_popup(); }
            }
        }
    }

    m.style_.item_spacing = saved_spacing;
    return changed;
}

static bool time_body(context& ui, time_of_day& value, bool seconds)
{
    auto& m = context_access::of(ui);
    bool changed = false;
    const f32 saved_spacing = m.style_.item_spacing;
    m.style_.item_spacing = 2.0f;

    const f32 w  = m.layout_.width;
    const f32 ch = ui.frame_height() - 6.0f;

    // a caption, the values in a grid, and (minutes / seconds) a - / + pair for the ones in between
    const auto section = [&](const char* caption, i32 count, i32 step, i32 cols, i32& v, bool fine) {
        ui.text_dim(caption);
        const f32 cw = w / static_cast<f32>(cols);
        for (i32 i = 0; i < count; i += cols) {
            const rect line = layout_place(ui, {w, ch});
            for (i32 c = 0; c < cols && i + c < count; ++c) {
                const i32  n = (i + c) * step;
                const rect r = rect::from_size({line.min.x + cw * static_cast<f32>(c), line.min.y}, {cw, ch});
                const id   key = hash_id(caption, static_cast<id>(n + 1000));
                if (pick_cell(ui, key, r.expanded(-1.0f), std::format("{:02}", n), v == n, false, false)) {
                    v       = n;
                    changed = true;
                }
            }
        }
        if (fine) {
            const rect line = layout_place(ui, {w, ch});
            const f32  bw   = 34.0f;
            const rect minus = rect::from_size(line.min, {bw, ch});
            const rect plus  = rect::from_size({line.max.x - bw, line.min.y}, {bw, ch});
            const i32  limit = count * step;
            if (pick_cell(ui, hash_id(caption, 2000), minus, "-", false, false, false)) { v = (v + limit - 1) % limit; changed = true; }
            if (pick_cell(ui, hash_id(caption, 2001), plus, "+", false, false, false))  { v = (v + 1) % limit; changed = true; }
            const std::string shown = std::format("{:02}", v);
            const font_id f  = ui.current_font();
            const vec2    ts = label_size(ui, f, shown);
            label_draw(ui, {line.center().x - ts.x * 0.5f, line.min.y + (ch - ts.y) * 0.5f}, m.style_.text, shown, f);
        }
    };
    section("hour", 24, 1, 6, value.hour, false);
    ui.spacing(4.0f);
    section("minute", 12, 5, 6, value.minute, true);
    if (seconds) {
        ui.spacing(4.0f);
        section("second", 12, 5, 6, value.second, true);
    }
    m.style_.item_spacing = saved_spacing;
    return changed;
}

bool context::date_picker(std::string_view label, date& value)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(value)) { value = clamp_date(value); }
    const id key = widget_id(*this, label);
    const field_layout fl  = layout_field(*this, visible_label(label), frame_height());
    const interaction  in  = interact(*this, key, fl.control);
    push_id(label);
    if (in.pressed) { m_->date_.cal_key_ = 0; toggle_popup("##calendar"); }
    picker_field(*this, key, fl.control, in, popup_open("##calendar"), to_string(value), static_cast<int>(picker_icon::calendar));

    bool changed = false;
    if (auto p = popup("##calendar", std::max(fl.control.width(), 260.0f))) {
        changed = calendar_body(*this, value, true);
        spacing(2.0f);
        if (button(m_->strings_.today)) {
            value   = today();
            m_->date_.cal_key_ = 0;
            changed = true;
            close_popup();
        }
    }
    const bool engaged = popup_open("##calendar"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(*this, key, changed, engaged);
    return changed;
}

bool context::time_picker(std::string_view label, time_of_day& value, bool seconds)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(value)) { value = clamp_time(value); }
    const id key = widget_id(*this, label);
    const field_layout fl  = layout_field(*this, visible_label(label), frame_height());
    const interaction  in  = interact(*this, key, fl.control);
    push_id(label);
    if (in.pressed) { toggle_popup("##clock"); }
    picker_field(*this, key, fl.control, in, popup_open("##clock"), to_string(value, seconds), static_cast<int>(picker_icon::clock));

    bool changed = false;
    if (auto p = popup("##clock", std::max(fl.control.width(), 240.0f))) {
        changed = time_body(*this, value, seconds);
        spacing(2.0f);
        if (button(m_->strings_.now)) {
            value   = now();
            changed = true;
        }
        same_line();
        if (button(m_->strings_.done)) { close_popup(); }
    }
    const bool engaged = popup_open("##clock"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(*this, key, changed, engaged);
    return changed;
}

bool context::datetime_picker(std::string_view label, date& d, time_of_day& t, bool seconds)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    if (!is_valid(d)) { d = clamp_date(d); }
    if (!is_valid(t)) { t = clamp_time(t); }
    const id key = widget_id(*this, label);
    const field_layout fl  = layout_field(*this, visible_label(label), frame_height());
    const interaction  in  = interact(*this, key, fl.control);
    push_id(label);
    if (in.pressed) { m_->date_.cal_key_ = 0; toggle_popup("##datetime"); }
    picker_field(*this, key, fl.control, in, popup_open("##datetime"), to_string(d) + " " + to_string(t, seconds),
                 static_cast<int>(picker_icon::calendar));

    bool changed = false;
    if (auto p = popup("##datetime", std::max(fl.control.width(), 260.0f))) {
        changed = calendar_body(*this, d, false);
        separator();
        changed = time_body(*this, t, seconds) || changed;
        spacing(2.0f);
        if (button(m_->strings_.now)) {
            d       = today();
            t       = now();
            m_->date_.cal_key_ = 0;
            changed = true;
        }
        same_line();
        if (button(m_->strings_.done)) { close_popup(); }
    }
    const bool engaged = popup_open("##datetime"); // (in the widget's id scope: before pop_id)
    pop_id();
    track_edit(*this, key, changed, engaged);
    return changed;
}

} // namespace strata

// date and time pickers: editable text field paired with a calendar / wheel-drum popup

#include "strata/context.hpp"

#include "context_impl.hpp"
#include "context_helpers.hpp"
#include "core/part_id.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace strata {

using namespace internal;

static bool pick_cell(context& ui, id key, const rect& r, std::string_view text, bool selected, bool faded, bool marked);
static bool calendar_body(context& ui, date& value, bool close_on_pick);
static bool time_body(context& ui, time_of_day& value, bool seconds);

namespace {

enum class picker_icon { calendar, clock };

struct editable_picker_result {
    rect        bounds{};
    id          edit_key{};
    bool        text_changed{};
    bool        submitted{};
    bool        cancelled{};
    bool        open_requested{};
    bool        toggle_requested{};
    bool        focused{};
};

void draw_picker_icon(draw_list& dl, const rect& icon_r, picker_icon kind, color col, bool hot, const style& st)
{
    if (hot) {
        shape_style pill;
        pill.radius      = radii(st.rounding * 0.6f);
        pill.fill_top    = st.widget_hover.scaled_alpha(0.85f);
        pill.fill_bottom = pill.fill_top;
        dl.shape(icon_r.expanded(-2.0f), pill);
    }
    const vec2 c = icon_r.center();
    if (kind == picker_icon::calendar) {
        const rect page = {{c.x - 6.5f, c.y - 5.5f}, {c.x + 6.5f, c.y + 6.0f}};
        dl.rect_outline(page, col, 2.5f, 1.3f);
        dl.line({page.min.x, c.y - 1.8f}, {page.max.x, c.y - 1.8f}, col, 1.3f);
        dl.line({c.x - 2.8f, page.min.y - 1.5f}, {c.x - 2.8f, page.min.y + 1.5f}, col, 1.3f);
        dl.line({c.x + 2.8f, page.min.y - 1.5f}, {c.x + 2.8f, page.min.y + 1.5f}, col, 1.3f);
    } else {
        dl.circle(c, 6.2f, col, 1.3f);
        dl.line(c, {c.x, c.y - 3.8f}, col, 1.3f);
        dl.line(c, {c.x + 2.8f, c.y + 1.4f}, col, 1.3f);
    }
}

// Editable field with a clickable calendar/clock icon button at the right end.
// Clicking the text area focuses for direct keyboard typing (auto-formatted by `mask`) and opens the popup;
// clicking the icon toggles the popup.
[[nodiscard]] editable_picker_result editable_picker_field(context& ui, std::string_view label, std::string_view current,
                                                           std::string_view mask, std::string_view hint, picker_icon icon,
                                                           bool popup_is_open)
{
    auto& m = context_access::of(ui);
    editable_picker_result out;

    const field_layout fl = layout_field(ui, visible_label(label), ui.frame_height());
    out.bounds            = fl.control;

    const f32  icon_w  = 28.0f;
    const rect icon_r  = {{fl.control.max.x - icon_w, fl.control.min.y}, fl.control.max};
    const id   btn_key = hash_id("##picker_icon_btn", widget_id(ui, label));

    // Test the icon button before input_core so clicking it toggles the popup cleanly
    const interaction btn_in = interact(ui, btn_key, icon_r);
    if (btn_in.hovered) { m.cursor_ = cursor_kind::hand; }
    if (m.input_.mouse_pressed_ && btn_in.held) { m.press_claimed_ = true; }
    out.toggle_requested = btn_in.pressed;

    const id   edit_key    = widget_id(ui, "##picker_edit");
    const bool was_focused = (m.focus_id_ == edit_key);
    out.edit_key           = edit_key;

    // Give input_core the full control box (it draws the frame, border, focus glow, text, selection, and caret)
    m.input_rect_     = fl.control;
    m.input_rect_set_ = true;
    m.edit_mask_      = mask;
    bool edited = false;
    {
        const edit_mute mute{ui}; // only parsed date/time changes count as edits of the picker
        edited = input_core(ui, "##picker_edit", current, hint, input_flags::select_all_on_focus, mask.size() + 8);
    }
    m.edit_mask_ = {};

    out.focused      = (m.focus_id_ == edit_key);
    out.text_changed = edited;
    out.submitted    = m.edit_.submitted_ && out.focused;
    out.cancelled    = was_focused && !out.focused && !m.input_.mouse_pressed_;

    // Clicking the text portion focuses the field and opens the popup if not already open
    const rect text_area = {fl.control.min, {icon_r.min.x, fl.control.max.y}};
    if (m.input_.mouse_pressed_ && !btn_in.held && pointer_over(ui, text_area) && !popup_is_open) {
        out.open_requested = true;
    }

    // Draw the icon button cleanly over the right end of the input frame
    const color ic = (btn_in.hovered || popup_is_open || out.focused) ? m.style_.text : m.style_.text_dim;
    draw_picker_icon(m.dl_, icon_r, icon, ic, btn_in.hovered, m.style_);

    // Keep item_rect() pointing to the full field box
    m.last_item_key_  = edit_key;
    m.last_item_rect_ = fl.control;
    return out;
}

void sync_edit_buffer(context& ui, id edit_key, std::string_view text)
{
    auto& m = context_access::of(ui);
    if (m.focus_id_ == edit_key) {
        m.edit_.edit_buf_.assign(text);
        m.edit_.edit_cursor_ = m.edit_.edit_buf_.size();
        m.edit_.edit_anchor_ = m.edit_.edit_cursor_;
        m.edit_.edit_scroll_ = 0.0f;
    }
}

[[nodiscard]] bool parse_datetime_str(std::string_view s, date& out_d, time_of_day& out_t) noexcept
{
    while (!s.empty() && s.front() == ' ') { s.remove_prefix(1); }
    while (!s.empty() && s.back() == ' ')  { s.remove_suffix(1); }
    const std::size_t sep = s.find_first_of(" Tt");
    if (sep == std::string_view::npos) { return false; }
    std::string_view ds = s.substr(0, sep);
    std::string_view ts = s.substr(sep + 1);
    while (!ts.empty() && ts.front() == ' ') { ts.remove_prefix(1); }
    date d{};
    time_of_day t{};
    if (!parse_date(ds, d) || !parse_time(ts, t)) { return false; }
    out_d = d;
    out_t = t;
    return true;
}

void draw_picker_footer(context& ui, bool& now_clicked, bool& done_clicked)
{
    auto& m = context_access::of(ui);
    now_clicked = ui.button(ui_str(m.strings_.now, "Now"));
    const std::string_view done_lbl = ui_str(m.strings_.done, "Done");
    const f32 done_w = label_size(ui, ui.current_font(), done_lbl).x + m.style_.frame_padding.x * 2.0f;
    ui.same_line_right(done_w);
    const auto accent_done = ui.style_overrides({
        override_color(style_color::widget_bg, m.style_.accent),
        override_color(style_color::widget_hover, m.style_.accent_hover),
        override_color(style_color::widget_border, m.style_.accent),
        override_color(style_color::text, color{255, 255, 255, 255}),
    });
    done_clicked = ui.button(done_lbl);
}

} // namespace

// one cell of a grid (a calendar day, nav arrow): a rounded highlight, optically centered text
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
        const color   tc = selected ? color{255, 255, 255, 255} : faded ? m.style_.text_dim.scaled_alpha(0.55f) : m.style_.text;
        m.dl_.text_centered(r, tc, text, f);
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
    const i32 m_num = std::clamp(m.date_.cal_month_, 1, 12);
    const std::string title = std::format("{} {}", ui_str(m.strings_.months[static_cast<std::size_t>(m_num - 1)], month_name(m_num)), m.date_.cal_year_);
    m.dl_.text_centered(bar, m.style_.text, title, f);

    // Mo .. Su
    const f32  cw   = w / 7.0f;
    const f32  lh   = m.font_.line_height(f);
    const rect days = layout_place(ui, {w, lh + 4.0f});
    for (i32 i = 0; i < 7; ++i) {
        const std::string_view name = ui_str(m.strings_.weekdays[static_cast<std::size_t>(i)], weekday_short(i));
        const rect dr = rect::from_size({days.min.x + cw * static_cast<f32>(i), days.min.y}, {cw, days.height()});
        m.dl_.text_centered(dr, i >= 5 ? m.style_.text_dim.scaled_alpha(0.7f) : m.style_.text_dim, name, f);
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

// Scrollable wheel-drum columns (HR | MIN [| SEC]) with 7 visible rows centered on a highlighted selection band.
// Supports mouse wheel, clicking any visible neighbor, and vertical drag per column.
static bool time_body(context& ui, time_of_day& value, bool seconds)
{
    auto& m = context_access::of(ui);
    bool changed = false;

    const f32 saved_spacing = m.style_.item_spacing;
    m.style_.item_spacing = 0.0f;

    const font_id f     = ui.current_font();
    const f32     w     = m.layout_.width;
    const int     parts = seconds ? 3 : 2;
    const f32     gap_x = 8.0f;
    const f32     col_w = (w - gap_x * static_cast<f32>(parts - 1)) / static_cast<f32>(parts);
    const f32     cap_h = 16.0f;
    const f32     row_h = 28.0f;
    constexpr int k_rows = 7; // offsets -3 .. +3 around the selected center row
    constexpr int k_mid  = 3;

    // Subtle unit captions above each drum column
    static constexpr std::array<std::string_view, 3> k_units = {"HR", "MIN", "SEC"};
    const rect cap_row = layout_place(ui, {w, cap_h});
    for (int i = 0; i < parts; ++i) {
        const f32  cx = cap_row.min.x + static_cast<f32>(i) * (col_w + gap_x);
        const rect cr = rect::from_size({cx, cap_row.min.y}, {col_w, cap_h});
        m.dl_.text_centered(cr, m.style_.text_dim.scaled_alpha(0.65f), k_units[static_cast<std::size_t>(i)], f);
    }

    (void)layout_place(ui, {w, 4.0f});
    const rect drum_r = layout_place(ui, {w, row_h * static_cast<f32>(k_rows)});

    // Soft centered selection band across the active row
    const rect center_band = {{drum_r.min.x, drum_r.min.y + row_h * static_cast<f32>(k_mid)},
                              {drum_r.max.x, drum_r.min.y + row_h * static_cast<f32>(k_mid + 1)}};
    {
        shape_style band;
        band.radius       = radii(m.style_.rounding * 0.85f);
        band.fill_top     = m.style_.widget_bg.scaled_alpha(0.85f);
        band.fill_bottom  = band.fill_top;
        band.border       = m.style_.border.scaled_alpha(0.75f);
        band.border_width = 1.0f;
        m.dl_.shape(center_band, band);
    }

    i32* const    fields[3] = {&value.hour, &value.minute, &value.second};
    constexpr i32 limits[3] = {24, 60, 60};
    constexpr f32 alphas[4] = {1.0f, 0.82f, 0.46f, 0.22f};

    for (int col = 0; col < parts; ++col) {
        const f32  cx    = drum_r.min.x + static_cast<f32>(col) * (col_w + gap_x);
        const rect col_r = rect::from_size({cx, drum_r.min.y}, {col_w, drum_r.height()});
        const i32  lim   = limits[col];
        i32&       val   = *fields[col];

        // Accumulate smoothed wheel deltas so one physical notch steps by exactly 1 unit
        if (m.input_.wheel_ != 0.0f && !m.wheel_consumed_ && pointer_over(ui, col_r)) {
            const id col_key = hash_id("time_wheel_col", static_cast<id>(col + 1));
            if (m.date_.wheel_col_key_ != col_key || (m.date_.wheel_accum_ * m.input_.wheel_ < 0.0f)) {
                m.date_.wheel_col_key_ = col_key;
                m.date_.wheel_accum_   = 0.0f;
            }
            m.date_.wheel_accum_ += m.input_.wheel_;
            m.wheel_consumed_ = true;
            if (std::abs(m.date_.wheel_accum_) >= 0.999f) {
                const i32 steps = static_cast<i32>(std::trunc(m.date_.wheel_accum_ + (m.date_.wheel_accum_ > 0.0f ? 0.001f : -0.001f)));
                m.date_.wheel_accum_ -= static_cast<f32>(steps);
                if (!m.input_.wheel_moving_ && std::abs(m.date_.wheel_accum_) < 0.05f) {
                    m.date_.wheel_accum_ = 0.0f;
                }
                val     = ((val - steps) % lim + lim) % lim;
                changed = true;
            }
        }

        // Subtle highlight pill on the hovered column's selected center cell
        const bool col_hot = pointer_over(ui, col_r);
        if (col_hot) {
            const rect sel_cell = {{col_r.min.x + 2.0f, center_band.min.y + 2.0f},
                                   {col_r.max.x - 2.0f, center_band.max.y - 2.0f}};
            m.dl_.rect_filled(sel_cell, m.style_.widget_hover.scaled_alpha(0.75f), m.style_.rounding * 0.7f);
        }

        // Draw 7 visible values (-3 .. +3) with click-to-select and vertical drag-to-spin
        for (int r = 0; r < k_rows; ++r) {
            const int  off    = r - k_mid;
            const i32  num    = (val + lim + off) % lim;
            const rect cell_r = rect::from_size({col_r.min.x, col_r.min.y + static_cast<f32>(r) * row_h}, {col_w, row_h});
            const id   ckey   = hash_id("time_wheel_cell", static_cast<id>(col * 16 + r + 1));
            const interaction in = interact(ui, ckey, cell_r);

            if (in.hovered && off != 0) {
                m.dl_.rect_filled(cell_r.expanded(-2.0f), m.style_.widget_hover.scaled_alpha(0.55f),
                                  m.style_.rounding * 0.65f);
            }

            // Vertical drag on any cell spins the column smoothly
            if (in.held) {
                if (m.date_.wheel_drag_key_ != ckey) {
                    m.date_.wheel_drag_key_ = ckey;
                    m.date_.wheel_drag_y_   = m.input_.mouse_.y;
                } else {
                    const f32 dy = m.input_.mouse_.y - m.date_.wheel_drag_y_;
                    constexpr f32 px_per_step = 22.0f;
                    if (std::abs(dy) >= px_per_step) {
                        const i32 steps = static_cast<i32>(dy / px_per_step);
                        val = ((val - steps) % lim + lim) % lim;
                        m.date_.wheel_drag_y_ += static_cast<f32>(steps) * px_per_step;
                        changed = true;
                    }
                }
            } else if (m.date_.wheel_drag_key_ == ckey) {
                m.date_.wheel_drag_key_ = 0;
            }

            // Click on a neighbor row snaps it into the center band (ignore release after a drag)
            if (in.pressed && off != 0) {
                val     = num;
                changed = true;
            }

            const int dist = std::abs(off);
            const color tc = (off == 0)
                ? m.style_.accent
                : (in.hovered ? m.style_.text : m.style_.text_dim.scaled_alpha(alphas[dist]));
            m.dl_.text_centered(cell_r, tc, std::format("{:02}", num), f,
                                off == 0 ? text_flags::bold : text_flags::none);
        }

        // Symmetric colon dots on the center selection band between columns
        if (col + 1 < parts) {
            const f32 dot_x = col_r.max.x + gap_x * 0.5f;
            const f32 dot_y = center_band.center().y;
            const color dc  = m.style_.accent.scaled_alpha(0.75f);
            m.dl_.circle_filled({dot_x, dot_y - 4.0f}, 1.5f, dc);
            m.dl_.circle_filled({dot_x, dot_y + 4.0f}, 1.5f, dc);
        }
    }

    const rect div = layout_place(ui, {w, 12.0f});
    m.dl_.line({div.min.x, div.center().y}, {div.max.x, div.center().y}, m.style_.border.scaled_alpha(0.65f), 1.0f);

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
    push_id(label);

    const std::string formatted = to_string(value);
    const bool was_open = popup_open("##calendar");
    const editable_picker_result ed = editable_picker_field(
        *this, label, formatted, "####-##-##", "YYYY-MM-DD", picker_icon::calendar, was_open);

    bool changed = false;
    if (ed.text_changed || ed.submitted) {
        date parsed{};
        if (parse_date(m_->edit_.edit_buf_, parsed)) {
            if (parsed != value) {
                value = parsed;
                changed = true;
            }
            m_->date_.cal_year_  = value.year;
            m_->date_.cal_month_ = value.month;
        }
        if (ed.submitted) {
            m_->focus_id_ = 0;
            if (was_open) { close_popup(); }
        }
    }
    if (ed.cancelled && was_open) { close_popup(); }
    if (ed.toggle_requested)      { m_->date_.cal_key_ = 0; toggle_popup("##calendar"); }
    else if (ed.open_requested)   { m_->date_.cal_key_ = 0; open_popup("##calendar"); }

    if (auto p = popup("##calendar", std::max(ed.bounds.width(), 260.0f))) {
        if (calendar_body(*this, value, true)) {
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(value));
        }
        spacing(2.0f);
        if (button(ui_str(m_->strings_.today, "Today"))) {
            value   = today();
            m_->date_.cal_key_ = 0;
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(value));
            close_popup();
        }
    }
    const bool engaged = popup_open("##calendar") || ed.focused;
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
    push_id(label);

    const std::string formatted = to_string(value, seconds);
    const bool was_open = popup_open("##clock");
    const editable_picker_result ed = editable_picker_field(
        *this, label, formatted, seconds ? "##:##:##" : "##:##", seconds ? "HH:MM:SS" : "HH:MM",
        picker_icon::clock, was_open);

    bool changed = false;
    if (ed.text_changed || ed.submitted) {
        time_of_day parsed{};
        if (parse_time(m_->edit_.edit_buf_, parsed)) {
            if (!seconds) { parsed.second = 0; }
            if (parsed != value) {
                value = parsed;
                changed = true;
            }
        }
        if (ed.submitted) {
            m_->focus_id_ = 0;
            if (was_open) { close_popup(); }
        }
    }
    if (ed.cancelled && was_open) { close_popup(); }
    if (ed.toggle_requested)      { toggle_popup("##clock"); }
    else if (ed.open_requested)   { open_popup("##clock"); }

    const f32 min_w = seconds ? 236.0f : 204.0f;
    if (auto p = popup("##clock", std::max(ed.bounds.width(), min_w))) {
        if (time_body(*this, value, seconds)) {
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(value, seconds));
        }
        bool now_hit = false, done_hit = false;
        draw_picker_footer(*this, now_hit, done_hit);
        if (now_hit) {
            value   = now();
            if (!seconds) { value.second = 0; }
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(value, seconds));
        }
        if (done_hit) {
            m_->focus_id_ = 0;
            close_popup();
        }
    }
    const bool engaged = popup_open("##clock") || ed.focused;
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
    push_id(label);

    const std::string formatted = to_string(d) + " " + to_string(t, seconds);
    const bool was_open = popup_open("##datetime");
    const editable_picker_result ed = editable_picker_field(
        *this, label, formatted, seconds ? "####-##-## ##:##:##" : "####-##-## ##:##",
        seconds ? "YYYY-MM-DD HH:MM:SS" : "YYYY-MM-DD HH:MM", picker_icon::calendar, was_open);

    bool changed = false;
    if (ed.text_changed || ed.submitted) {
        date pd{};
        time_of_day pt{};
        if (parse_datetime_str(m_->edit_.edit_buf_, pd, pt)) {
            if (!seconds) { pt.second = 0; }
            if (pd != d || pt != t) {
                d = pd;
                t = pt;
                changed = true;
            }
            m_->date_.cal_year_  = d.year;
            m_->date_.cal_month_ = d.month;
        }
        if (ed.submitted) {
            m_->focus_id_ = 0;
            if (was_open) { close_popup(); }
        }
    }
    if (ed.cancelled && was_open) { close_popup(); }
    if (ed.toggle_requested)      { m_->date_.cal_key_ = 0; toggle_popup("##datetime"); }
    else if (ed.open_requested)   { m_->date_.cal_key_ = 0; open_popup("##datetime"); }

    if (auto p = popup("##datetime", std::max(ed.bounds.width(), 264.0f))) {
        if (calendar_body(*this, d, false)) {
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(d) + " " + to_string(t, seconds));
        }
        separator();
        if (time_body(*this, t, seconds)) {
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(d) + " " + to_string(t, seconds));
        }
        bool now_hit = false, done_hit = false;
        draw_picker_footer(*this, now_hit, done_hit);
        if (now_hit) {
            d = today();
            t = now();
            if (!seconds) { t.second = 0; }
            m_->date_.cal_key_ = 0;
            changed = true;
            sync_edit_buffer(*this, ed.edit_key, to_string(d) + " " + to_string(t, seconds));
        }
        if (done_hit) {
            m_->focus_id_ = 0;
            close_popup();
        }
    }
    const bool engaged = popup_open("##datetime") || ed.focused;
    pop_id();
    track_edit(*this, key, changed, engaged);
    return changed;
}

} // namespace strata

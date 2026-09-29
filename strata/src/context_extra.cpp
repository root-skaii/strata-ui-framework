// child regions, group cards, vertical tab strip, tooltips, hotkey binding, page transitions

#include "strata/context.hpp"

#include "context_impl.hpp"

#include "text_util.hpp"

#include <algorithm>
#include <cmath>

namespace strata {

namespace {

[[nodiscard]] constexpr color lighten(color c, f32 k) noexcept { return lerp(c, color{255, 255, 255, c.a}, k); }
[[nodiscard]] constexpr color darken(color c, f32 k) noexcept  { return lerp(c, color{0, 0, 0, c.a}, k); }
[[nodiscard]] constexpr f32   smooth(f32 t) noexcept { return t * t * (3.0f - 2.0f * t); }

using internal::state_for;

} // namespace

// popups -----------------------------------------------------------------------------------

void context::popup_panel(const rect& r, const shape_style& body)
{
    const f32 k = std::clamp(m_->style_.popup_acrylic, 0.0f, 1.0f);
    if (k <= 0.0f || m_->dl_.alpha() < 0.99f) { // (a popup that is still fading in is drawn opaque: the blur has no fade)
        m_->dl_.shape(r, body);
        return;
    }
    const color clear{0, 0, 0, 0};
    shape_style shadow_only = body; // the shadow first, the glass over it, then the border on top
    shadow_only.fill_top = shadow_only.fill_bottom = clear;
    shadow_only.border = clear;
    shadow_only.border_width = 0.0f;
    m_->dl_.shape(r, shadow_only);

    const color tint = body.fill_top.scaled_alpha(1.0f + (m_->style_.acrylic_alpha - 1.0f) * k);
    m_->dl_.backdrop(r, m_->style_.blur_radius, tint, body.radius, m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);

    shape_style edge = body;
    edge.fill_top = edge.fill_bottom = clear;
    edge.shadow = clear;
    edge.shadow_blur = 0.0f;
    m_->dl_.shape(r, edge);
}

// scopes -----------------------------------------------------------------------------------

child_scope::~child_scope()
{
    if (open_) { ctx_->end_child(); }
}

card_scope::~card_scope()
{
    if (open_) { ctx_->end_card(); }
}

transition_scope::~transition_scope()
{
    ctx_->pop_alpha();
}

// key names ----------------------------------------------------------------------------------

std::string_view key_name(u32 vk) noexcept
{
    static constexpr char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr char digits[]  = "0123456789";
    static constexpr std::array<std::string_view, 24> fkeys = {
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
        "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};

    if (vk >= 'A' && vk <= 'Z') { return {&letters[vk - 'A'], 1}; }
    if (vk >= '0' && vk <= '9') { return {&digits[vk - '0'], 1}; }
    if (vk >= 0x70 && vk <= 0x87) { return fkeys[vk - 0x70]; }
    if (vk >= 0x60 && vk <= 0x69) {
        static constexpr std::array<std::string_view, 10> pad = {"Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                                                  "Num 5", "Num 6", "Num 7", "Num 8", "Num 9"};
        return pad[vk - 0x60];
    }
    switch (vk) {
    case 0:    return "None";
    case 0x01: return "Mouse 1";
    case 0x02: return "Mouse 2";
    case 0x04: return "Mouse 3";
    case 0x05: return "Mouse 4";
    case 0x06: return "Mouse 5";
    case 0x08: return "Backspace";
    case 0x09: return "Tab";
    case 0x0d: return "Enter";
    case 0x13: return "Pause";
    case 0x14: return "Caps Lock";
    case 0x1b: return "Esc";
    case 0x20: return "Space";
    case 0x21: return "Page Up";
    case 0x22: return "Page Down";
    case 0x23: return "End";
    case 0x24: return "Home";
    case 0x25: return "Left";
    case 0x26: return "Up";
    case 0x27: return "Right";
    case 0x28: return "Down";
    case 0x2c: return "Print Screen";
    case 0x2d: return "Insert";
    case 0x2e: return "Delete";
    case 0x6a: return "Num *";
    case 0x6b: return "Num +";
    case 0x6d: return "Num -";
    case 0x6e: return "Num .";
    case 0x6f: return "Num /";
    case 0x90: return "Num Lock";
    case 0x91: return "Scroll Lock";
    case 0xba: return ";";
    case 0xbb: return "=";
    case 0xbc: return ",";
    case 0xbd: return "-";
    case 0xbe: return ".";
    case 0xbf: return "/";
    case 0xc0: return "`";
    case 0xdb: return "[";
    case 0xdc: return "\\";
    case 0xdd: return "]";
    case 0xde: return "'";
    default:   return "Key ?";
    }
}

// child regions --------------------------------------------------------------------------------

f32 context::layout_next_y() const noexcept
{
    const layout_state& l = m_->layout_;
    if (l.same_line && !l.first) {
        return l.line_top;
    }
    return l.first ? l.origin.y : l.line_top + l.line_h + m_->style_.item_spacing;
}

bool context::begin_child(std::string_view id_label, vec2 size, child_flags flags)
{
    if (m_->cur_ != nullptr && m_->children_cards_.child_depth_ >= max_child_depth) {
        report_limit("child regions inside child regions (max_child_depth)", max_child_depth);
    }
    if (m_->cur_ == nullptr || m_->children_cards_.child_depth_ >= max_child_depth) {
        return false;
    }
    const id key = widget_id(id_label);
    child_state* st = state_for(m_->children_cards_.children_, key, m_->frame_);
    if (st->content_h > 0.0f) { apply_pending_scroll(key, st->scroll, &st->scroll_x); } // (once its content has been measured)

    // width: rest of the line; height: down to the window bottom
    f32 w = size.x;
    if (w <= 0.0f) {
        w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
        if (m_->layout_.same_line && !m_->layout_.first) {
            w = m_->layout_.origin.x + m_->layout_.width - (m_->layout_.cursor_x + m_->style_.item_spacing);
        }
    }
    m_->layout_.next_width = 0.0f;
    f32 h = size.y;
    if (h <= 0.0f) {
        h = m_->layout_.bound_bottom > 0.0f ? m_->layout_.bound_bottom - layout_next_y() : 240.0f;
    }
    w = std::max(w, 24.0f);
    h = std::max(h, 24.0f);

    const rect r = layout_place({w, h});

    if (has_flag(flags, child_flags::acrylic)) {
        m_->dl_.backdrop(r, m_->style_.blur_radius, darken(m_->style_.window_bg, 0.25f).scaled_alpha(m_->style_.acrylic_alpha * 0.8f),
                     radii(m_->style_.rounding * 0.8f), m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);
    }
    if (has_flag(flags, child_flags::frame)) {
        shape_style bg;
        bg.radius       = radii(m_->style_.rounding * 0.8f);
        bg.fill_top     = darken(m_->style_.widget_bg, 0.30f).scaled_alpha(0.55f);
        bg.fill_bottom  = bg.fill_top;
        bg.border       = m_->style_.border;
        bg.border_width = m_->style_.border_width;
        m_->dl_.shape(r, bg);
    }

    const f32  pad   = has_flag(flags, child_flags::no_padding) ? 0.0f : m_->style_.padding * 0.7f;
    const rect inner = {{r.min.x + pad, r.min.y + pad}, {r.max.x - pad, r.max.y - pad}};

    const f32 max_scroll = std::max(0.0f, st->content_h - inner.height());
    st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);

    const bool horiz = has_flag(flags, child_flags::horizontal);
    const bool bars  = !has_flag(flags, child_flags::no_scrollbar);
    // the horizontal bar takes a strip off the bottom, shortening the vertical one (both only when scrollable)
    const f32 hbar = horiz && st->overflow_x && bars ? 10.0f : 0.0f;
    if (!horiz) {
        st->scroll_x = 0.0f;
    } else {
        st->scroll_x = std::clamp(st->scroll_x, 0.0f, std::max(0.0f, st->content_w - inner.width()));
    }

    m_->dl_.push_clip({{r.min.x + 1.0f, r.min.y + 1.0f}, {r.max.x - 1.0f, r.max.y - 1.0f}});

    m_->children_cards_.child_stack_[m_->children_cards_.child_depth_++] = {st, r, inner, m_->layout_, flags};
    push_id(id_label);

    m_->layout_              = {};
    m_->layout_.origin       = {inner.min.x - st->scroll_x, inner.min.y - st->scroll};
    m_->layout_.width        = std::max(inner.width() - (st->overflow && bars ? 10.0f : 0.0f), 1.0f);
    m_->layout_.bound_bottom = inner.max.y - hbar;
    return true;
}

void context::end_child()
{
    if (m_->children_cards_.child_depth_ == 0) {
        return;
    }
    const child_frame f = m_->children_cards_.child_stack_[--m_->children_cards_.child_depth_];
    child_state& st     = *f.state;

    const bool horiz = has_flag(f.flags, child_flags::horizontal);
    const bool bars  = !has_flag(f.flags, child_flags::no_scrollbar);

    // content width: layout_.right minus origin.x (which already includes the scroll offset)
    if (horiz) {
        st.content_w  = m_->layout_.first ? 0.0f : m_->layout_.right - m_->layout_.origin.x;
        st.overflow_x = st.content_w > f.inner.width() + 0.5f;
    } else {
        st.content_w  = 0.0f;
        st.overflow_x = false;
    }
    const f32 hbar = st.overflow_x && bars ? 10.0f : 0.0f;

    st.content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    const f32 view_h = f.inner.height() - hbar;
    st.overflow = st.content_h > view_h + 0.5f;

    if (st.overflow_x) {
        const f32 max_x = st.content_w - f.inner.width();
        // tilt wheel, or Shift + wheel (the Windows convention)
        const bool over = pointer_over(f.bounds);
        if (m_->input_.wheel_x_ != 0.0f && !m_->wheel_x_consumed_ && over) {
            st.scroll_x = std::clamp(st.scroll_x + wheel_scroll_x(m_->font_.line_height(0), f.inner.width()), 0.0f, max_x);
            m_->wheel_x_consumed_ = true;
        } else if (m_->input_.mod_shift_ && m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && over) {
            st.scroll_x = std::clamp(st.scroll_x - wheel_scroll(m_->font_.line_height(0), f.inner.width()), 0.0f, max_x);
            m_->wheel_consumed_ = true;
        }
        if (bars) {
            const f32 track_x0 = f.bounds.min.x + 5.0f;
            const f32 track_w  = f.bounds.width() - 10.0f - (st.overflow ? 10.0f : 0.0f);
            const f32 thumb_w  = std::max(20.0f, track_w * f.inner.width() / st.content_w);
            const f32 y0       = f.bounds.max.y - 9.0f;
            f32 thumb_x = track_x0 + (track_w - thumb_w) * (st.scroll_x / max_x);
            const interaction in = interact(widget_id("##cscroll_x"), {{track_x0, y0 - 3.0f}, {track_x0 + track_w, y0 + 8.0f}});
            st.scroll_x = thumb_drag_x(in, st.grab_x, thumb_x, thumb_w, track_x0, track_w - thumb_w, max_x, st.scroll_x);
            thumb_x     = track_x0 + (track_w - thumb_w) * (st.scroll_x / max_x);
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.4f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape({{thumb_x, y0}, {thumb_x + thumb_w, f.bounds.max.y - 4.0f}}, bar);
        }
    } else {
        st.scroll_x = 0.0f;
    }

    if (st.overflow) {
        const f32 max_scroll = st.content_h - view_h;
        if (m_->input_.wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(f.bounds)) {
            st.scroll = std::clamp(st.scroll - wheel_scroll(m_->font_.line_height(0), view_h), 0.0f, max_scroll);
            m_->wheel_consumed_ = true;
        }
        if (bars) {
            const f32  track_top = f.bounds.min.y + 5.0f;
            const f32  track_h = f.bounds.height() - 10.0f - hbar; // room for the horizontal bar, when there is one
            const f32  thumb_h = std::max(20.0f, track_h * view_h / st.content_h);
            const f32  x0      = f.bounds.max.x - 9.0f;
            // the whole track takes the press: clicking beside the thumb jumps there, dragging works from anywhere
            f32 thumb_y = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const interaction in = interact(widget_id("##cscroll"), {{x0 - 3.0f, track_top}, {x0 + 8.0f, track_top + track_h}});
            st.scroll = thumb_drag(in, st.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, st.scroll);
            thumb_y   = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const rect thumb = {{x0, thumb_y}, {f.bounds.max.x - 4.0f, thumb_y + thumb_h}};
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.4f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape(thumb, bar);
        }
    } else {
        st.scroll = 0.0f;
    }

    m_->dl_.pop_clip();
    m_->layout_ = f.outer;
    pop_id();
}

// group cards -----------------------------------------------------------------------------------

bool context::begin_card(std::string_view title, std::string_view icon, font_id icon_font)
{
    if (m_->cur_ != nullptr && m_->children_cards_.card_depth_ >= max_card_depth) {
        report_limit("cards inside cards (max_card_depth)", max_card_depth);
    }
    if (m_->cur_ == nullptr || m_->children_cards_.card_depth_ >= max_card_depth) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(title);
    card_state* st  = state_for(m_->children_cards_.cards_, key, m_->frame_);

    const std::string_view shown = visible_label(title);
    const bool has_head = !shown.empty() || !icon.empty();
    const f32  lh       = m_->font_.line_height(f);
    const f32  head_h   = has_head ? lh + 16.0f : 0.0f;
    const f32  pad      = m_->style_.padding * 0.85f;

    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    // the height comes from last frame's content; the very first frame shows just the header
    const rect r = layout_place({w, head_h + pad + st->content_h + pad});

    shape_style bg;
    bg.radius       = radii(m_->style_.rounding);
    bg.fill_top     = lighten(m_->style_.widget_bg, m_->style_.gradient * 0.4f).scaled_alpha(0.42f);
    bg.fill_bottom  = darken(m_->style_.widget_bg, 0.15f).scaled_alpha(0.42f);
    bg.border       = m_->style_.border;
    bg.border_width = m_->style_.border_width;
    m_->dl_.shape(r, bg);

    if (has_head) {
        m_->dl_.rect_filled({{r.min.x + 1.0f, r.min.y + head_h}, {r.max.x - 1.0f, r.min.y + head_h + 1.0f}}, m_->style_.border.scaled_alpha(0.7f));
        shape_style mark; // accent tick before the title
        mark.radius      = radii(1.5f);
        mark.fill_top    = m_->style_.accent_hover;
        mark.fill_bottom = m_->style_.accent;
        m_->dl_.shape({{r.min.x + 10.0f, r.min.y + (head_h - lh * 0.75f) * 0.5f}, {r.min.x + 13.0f, r.min.y + (head_h + lh * 0.75f) * 0.5f}}, mark);

        f32 x = r.min.x + 20.0f;
        if (!icon.empty()) {
            const vec2 isize = m_->font_.measure(icon_font, icon);
            m_->dl_.text({x, r.min.y + (head_h - isize.y) * 0.5f}, m_->style_.accent_hover, icon, icon_font);
            x += isize.x + 8.0f;
        }
        const f32 title_h = shown.empty() ? lh : label_size(f, shown).y;
        label_draw({x, r.min.y + (head_h - title_h) * 0.5f}, m_->style_.text, shown, f);
    }

    m_->children_cards_.card_stack_[m_->children_cards_.card_depth_++] = {st, m_->layout_};
    push_id(title);
    m_->layout_              = {};
    m_->layout_.origin       = {r.min.x + pad, r.min.y + head_h + pad};
    m_->layout_.width        = std::max(w - 2.0f * pad, 1.0f);
    return true;
}

void context::end_card()
{
    if (m_->children_cards_.card_depth_ == 0) {
        return;
    }
    const card_frame f = m_->children_cards_.card_stack_[--m_->children_cards_.card_depth_];
    f.state->content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m_->layout_ = f.outer;
    pop_id();
}

// vertical tab strip ----------------------------------------------------------------------------------

bool context::tab_strip(std::string_view id_label, const tab_desc* tabs, std::size_t count, int& selected,
                        font_id icon_font, f32 width, tab_strip_flags flags, f32 height)
{
    if (m_->cur_ == nullptr || count == 0) {
        return false;
    }
    push_id(id_label);

    const bool icons_only = flags == tab_strip_flags::icons_only;
    const font_id f       = current_font();
    const f32  lh         = m_->font_.line_height(f);
    const f32  row_h      = frame_height() + 10.0f;
    const f32  gap        = 3.0f;
    const std::size_t n   = std::min<std::size_t>(count, 16);
    selected = std::clamp(selected, 0, static_cast<int>(n) - 1);

    f32 w = width;
    if (w <= 0.0f) {
        if (icons_only) {
            w = row_h + 12.0f;
        } else {
            f32 widest = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const f32 iw = tabs[i].icon.empty() ? 0.0f : m_->font_.measure(icon_font, tabs[i].icon).x + 12.0f;
                widest = std::max(widest, iw + label_size(f, visible_label(tabs[i].label)).x);
            }
            w = std::max(widest + 44.0f, 120.0f);
        }
    }
    const f32 natural_h = static_cast<f32>(n) * (row_h + gap) + 16.0f;
    f32 h = height;
    if (h <= 0.0f) {
        h = m_->layout_.bound_bottom > 0.0f ? std::max(m_->layout_.bound_bottom - layout_next_y(), natural_h) : natural_h;
    }

    const rect r = layout_place({w, h});
    shape_style panel;
    panel.radius       = radii(m_->style_.rounding);
    panel.fill_top     = lighten(m_->style_.title_bg, m_->style_.gradient * 0.5f).scaled_alpha(0.85f);
    panel.fill_bottom  = darken(m_->style_.title_bg, 0.1f).scaled_alpha(0.85f);
    panel.border       = m_->style_.border;
    panel.border_width = m_->style_.border_width;
    m_->dl_.shape(r, panel);

    std::array<rect, 16> cells{};
    f32 y = r.min.y + 8.0f;
    for (std::size_t i = 0; i < n; ++i) {
        cells[i] = {{r.min.x + 6.0f, y}, {r.max.x - 6.0f, y + row_h}};
        y += row_h + gap;
    }

    bool changed = false;
    std::array<f32, 16> emphasis{};
    for (std::size_t i = 0; i < n; ++i) {
        const id key = widget_id(tabs[i].label);
        const interaction in = interact(key, cells[i]);
        if (in.pressed && static_cast<int>(i) != selected) {
            selected = static_cast<int>(i);
            changed  = true;
        }
        anim_slot& a = anim_for(key);
        a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
        a.toggle = approach(a.toggle, static_cast<int>(i) == selected ? 1.0f : 0.0f);
        emphasis[i] = std::max(a.toggle, a.hover * 0.7f);

        if (a.hover > 0.01f && static_cast<int>(i) != selected) {
            shape_style hv;
            hv.radius      = radii(m_->style_.rounding * 0.7f);
            hv.fill_top    = m_->style_.widget_hover.scaled_alpha(0.55f * a.hover);
            hv.fill_bottom = hv.fill_top;
            m_->dl_.shape(cells[i], hv);
        }
        if (icons_only && in.hovered && m_->hover_time_ > 0.35f) {
            draw_tooltip(visible_label(tabs[i].label));
        }
    }

    const rect sel = cells[static_cast<std::size_t>(selected)];
    const f32  iy  = r.min.y + animate("strip_y", sel.min.y - r.min.y, 22.0f); // (relative to the strip: moving the window must not make it lag)
    shape_style pill;
    pill.radius      = radii(m_->style_.rounding * 0.7f);
    pill.fill_top    = m_->style_.accent.scaled_alpha(0.22f);
    pill.fill_bottom = m_->style_.accent.scaled_alpha(0.10f);
    pill.border      = m_->style_.accent.scaled_alpha(0.35f);
    pill.border_width = 1.0f;
    m_->dl_.shape({{sel.min.x, iy}, {sel.max.x, iy + row_h}}, pill);
    shape_style bar;
    bar.radius      = radii(1.5f);
    bar.fill_top    = m_->style_.accent_hover;
    bar.fill_bottom = m_->style_.accent;
    m_->dl_.shape({{sel.min.x + 1.0f, iy + 9.0f}, {sel.min.x + 4.0f, iy + row_h - 9.0f}}, bar);

    for (std::size_t i = 0; i < n; ++i) {
        const color c = lerp(m_->style_.text_dim, m_->style_.text, emphasis[i]);
        const bool has_icon = !tabs[i].icon.empty();
        const vec2 isize = has_icon ? m_->font_.measure(icon_font, tabs[i].icon) : vec2{};
        const color ic   = lerp(c, m_->style_.accent_hover, emphasis[i] * 0.7f);
        if (icons_only) {
            if (has_icon) {
                m_->dl_.text({cells[i].center().x - isize.x * 0.5f, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
            }
        } else {
            f32 x = cells[i].min.x + 16.0f;
            if (has_icon) {
                m_->dl_.text({x, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
                x += isize.x + 12.0f;
            }
            const std::string_view shown = visible_label(tabs[i].label);
            label_draw({x, cells[i].center().y - (shown.empty() ? lh : label_size(f, shown).y) * 0.5f}, c, shown, f);
        }
    }

    pop_id();
    return changed;
}

// tooltips ------------------------------------------------------------------------------------------

void context::draw_tooltip(std::string_view text)
{
    if (m_->in_overlay_ || text.empty()) {
        return;
    }
    const font_id f  = current_font();
    const vec2 ts    = label_size(f, text);
    const f32  padx  = 9.0f;
    const f32  pady  = 6.0f;
    const vec2 size{ts.x + 2.0f * padx, ts.y + 2.0f * pady};

    vec2 pos = m_->input_.mouse_ + vec2{14.0f, 20.0f};
    if (pos.x + size.x > m_->display_.x - 4.0f) { pos.x = m_->display_.x - 4.0f - size.x; }
    if (pos.y + size.y > m_->display_.y - 4.0f) { pos.y = m_->input_.mouse_.y - size.y - 10.0f; }
    pos.x = std::max(pos.x, 4.0f);
    pos.y = std::max(pos.y, 4.0f);

    const f32 fade = std::clamp((m_->hover_time_ - 0.35f) / 0.12f, 0.0f, 1.0f);
    const u32 previous_owner = m_->run_owner_;
    switch_run(m_->popup_.overlay_run());
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
    m_->dl_.push_alpha(fade);

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.6f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = m_->style_.border;
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.5f;
    body.shadow_offset = {0.0f, 3.0f};
    popup_panel(rect::from_size(pos, size), body);
    label_draw({pos.x + padx, pos.y + pady}, m_->style_.text, text, f);

    m_->dl_.pop_alpha();
    m_->dl_.pop_clip();
    switch_run(previous_owner);
}

void context::tooltip(std::string_view text)
{
    if (m_->last_item_hovered_ && m_->last_item_key_ == m_->hover_key_cur_ && m_->hover_time_ > 0.4f) {
        draw_tooltip(text);
    }
}

// hotkey binding ------------------------------------------------------------------------------------

bool context::hotkey(std::string_view label, u32& key_code)
{
    return hotkey_field(label, key_code, nullptr);
}

bool context::hotkey_chord(std::string_view label, key_chord& chord)
{
    return hotkey_field(label, chord.key, &chord);
}

bool context::hotkey_sequence(std::string_view label, key_sequence& seq)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool capturing = m_->hotkey_.hotkey_capture_ == key;
    if (in.pressed) {
        capturing       = !capturing;
        m_->hotkey_.hotkey_capture_ = capturing ? key : 0;
        m_->hotkey_.seq_edit_count_ = 0;
    } else if (capturing && m_->input_.mouse_pressed_ && !box.contains(m_->input_.mouse_)) {
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }

    // each step commits at once (no latency for single chords), but capture continues briefly so another key extends
    // it into a sequence, replacing the committed one
    bool changed = false;
    if (capturing && m_->input_.pressed_key_ != 0) {
        const bool bare = !(m_->input_.press_ctrl_ || m_->input_.press_shift_ || m_->input_.press_alt_);
        if (m_->hotkey_.seq_edit_count_ == 0 && m_->input_.pressed_key_ == 0x1b && bare) {                                   // Esc: leave it as it was
            capturing       = false;
            m_->hotkey_.hotkey_capture_ = 0;
        } else if (m_->hotkey_.seq_edit_count_ == 0 && (m_->input_.pressed_key_ == 0x08 || m_->input_.pressed_key_ == 0x2e) && bare) {  // Backspace / Delete: unbind
            changed         = seq.bound();
            seq             = {};
            capturing       = false;
            m_->hotkey_.hotkey_capture_ = 0;
        } else {
            m_->hotkey_.seq_edit_capture_[m_->hotkey_.seq_edit_count_++] = {m_->input_.pressed_key_, m_->input_.press_ctrl_, m_->input_.press_shift_, m_->input_.press_alt_};
            m_->hotkey_.seq_edit_deadline_                   = m_->time_ + key_sequence_timeout;
            key_sequence next;
            for (u8 i = 0; i < m_->hotkey_.seq_edit_count_; ++i) { next.steps[next.count++] = m_->hotkey_.seq_edit_capture_[i]; }
            changed = next != seq;
            seq     = next;
            if (m_->hotkey_.seq_edit_count_ >= key_sequence::max_steps) { // no room for another step: definitely done
                capturing       = false;
                m_->hotkey_.hotkey_capture_ = 0;
            }
        }
        m_->input_.pressed_key_ = 0; // the key is spent either way; do not also fire an accelerator
        m_->input_.key_count_   = 0;
    } else if (capturing && m_->hotkey_.seq_edit_count_ > 0 && m_->time_ >= m_->hotkey_.seq_edit_deadline_) {
        // paused without a further key: what was captured already stands, just stop listening for more
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }
    if (capturing) {
        m_->hotkey_.hotkey_seen_ = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, capturing ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.shadow      = m_->style_.accent.scaled_alpha(0.3f * a.toggle);
    field.shadow_blur = 8.0f * a.toggle;
    m_->dl_.shape(box, field);

    std::string shown_text;
    if (capturing && m_->hotkey_.seq_edit_count_ > 0) {
        shown_text = sequence_to_string(seq) + ", ..."; // committed so far; a further key would extend it
    } else if (!capturing) {
        shown_text = sequence_to_string(seq);
    }
    const std::string_view shown = capturing && m_->hotkey_.seq_edit_count_ == 0 ? std::string_view{"press a key..."}
                                  : !capturing && shown_text.empty() ? std::string_view{"None"}
                                                                     : std::string_view{shown_text};
    color tc = !capturing && !seq.bound() ? m_->style_.text_dim : m_->style_.text;
    if (capturing) {
        const f32 pulse = 0.65f + 0.35f * std::sin(static_cast<f32>(m_->time_) * 7.0f);
        tc = m_->style_.accent_hover.scaled_alpha(pulse);
    }
    const vec2 tsize = m_->font_.measure(f, shown);
    m_->dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, tc, shown, f);
    track_edit(key, changed, m_->hotkey_.hotkey_capture_ == key);
    return changed;
}

// `chord` null = plain key; otherwise key_code is chord->key and the held modifiers are stored too
bool context::hotkey_field(std::string_view label, u32& key_code, key_chord* chord)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool capturing = m_->hotkey_.hotkey_capture_ == key;
    if (in.pressed) {
        capturing       = !capturing;
        m_->hotkey_.hotkey_capture_ = capturing ? key : 0;
    } else if (capturing && m_->input_.mouse_pressed_ && !box.contains(m_->input_.mouse_)) {
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
    }

    bool changed = false;
    if (capturing && m_->input_.pressed_key_ != 0) {
        const bool bare = chord == nullptr || !(m_->input_.press_ctrl_ || m_->input_.press_shift_ || m_->input_.press_alt_);
        if (m_->input_.pressed_key_ == 0x1b && bare) {                                     // Esc: leave it as it was
        } else if ((m_->input_.pressed_key_ == 0x08 || m_->input_.pressed_key_ == 0x2e) && bare) {   // Backspace / Delete: unbind
            changed  = key_code != 0;
            key_code = 0;
            if (chord != nullptr) { *chord = {}; }
        } else {
            const key_chord next{m_->input_.pressed_key_, m_->input_.press_ctrl_, m_->input_.press_shift_, m_->input_.press_alt_};
            changed  = chord != nullptr ? *chord != next : key_code != m_->input_.pressed_key_;
            key_code = m_->input_.pressed_key_;
            if (chord != nullptr) { *chord = next; }
        }
        capturing       = false;
        m_->hotkey_.hotkey_capture_ = 0;
        m_->input_.pressed_key_    = 0;
        m_->input_.key_count_      = 0;
    }
    if (capturing) {
        m_->hotkey_.hotkey_seen_ = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, capturing ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.shadow      = m_->style_.accent.scaled_alpha(0.3f * a.toggle);
    field.shadow_blur = 8.0f * a.toggle;
    m_->dl_.shape(box, field);

    std::string chord_text;
    if (chord != nullptr && !capturing) { chord_text = chord_to_string(*chord); }
    std::string_view shown = capturing ? std::string_view{"press a key..."}
                           : chord != nullptr ? (chord_text.empty() ? std::string_view{"None"} : std::string_view{chord_text})
                                              : key_name(key_code);
    color tc = key_code == 0 && !capturing ? m_->style_.text_dim : m_->style_.text;
    if (capturing) {
        const f32 pulse = 0.65f + 0.35f * std::sin(static_cast<f32>(m_->time_) * 7.0f);
        tc = m_->style_.accent_hover.scaled_alpha(pulse);
    }
    const vec2 tsize = m_->font_.measure(f, shown);
    m_->dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, tc, shown, f);
    track_edit(key, changed, m_->hotkey_.hotkey_capture_ == key);
    return changed;
}

// page transitions -----------------------------------------------------------------------------------

transition_scope context::page_transition(std::string_view key, int page, f32 slide)
{
    anim_slot& a = anim_for(widget_id(key));
    const f32 pf = static_cast<f32>(page);
    if (!a.custom_init) {
        a.custom_init = true;
        a.custom      = pf;
        a.toggle      = 1.0f;
    } else if (a.custom != pf) {
        a.custom = pf; // a new page: start the fade over
        a.toggle = 0.0f;
    } else {
        a.toggle = approach(a.toggle, 1.0f, m_->style_.anim_speed * 0.65f);
    }
    const f32 t = smooth(std::clamp(a.toggle, 0.0f, 1.0f));

    m_->dl_.push_alpha(t);
    if (m_->layout_.first) {
        m_->layout_.origin.y += (1.0f - t) * slide;
    }
    return transition_scope{*this};
}

} // namespace strata

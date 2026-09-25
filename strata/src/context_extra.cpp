// child regions, group cards, vertical tab strip, tooltips, hotkey binding, page transitions

#include "strata/context.hpp"

#include "limits.hpp"
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
    const f32 k = std::clamp(style_.popup_acrylic, 0.0f, 1.0f);
    if (k <= 0.0f || dl_.alpha() < 0.99f) { // (a popup that is still fading in is drawn opaque: the blur has no fade)
        dl_.shape(r, body);
        return;
    }
    const color clear{0, 0, 0, 0};
    shape_style shadow_only = body; // the shadow first, the glass over it, then the border on top
    shadow_only.fill_top = shadow_only.fill_bottom = clear;
    shadow_only.border = clear;
    shadow_only.border_width = 0.0f;
    dl_.shape(r, shadow_only);

    const color tint = body.fill_top.scaled_alpha(1.0f + (style_.acrylic_alpha - 1.0f) * k);
    dl_.backdrop(r, style_.blur_radius, tint, body.radius, style_.acrylic_noise, style_.acrylic_saturation, style_.acrylic_brightness);

    shape_style edge = body;
    edge.fill_top = edge.fill_bottom = clear;
    edge.shadow = clear;
    edge.shadow_blur = 0.0f;
    dl_.shape(r, edge);
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
    const layout_state& l = layout_;
    if (l.same_line && !l.first) {
        return l.line_top;
    }
    return l.first ? l.origin.y : l.line_top + l.line_h + style_.item_spacing;
}

bool context::begin_child(std::string_view id_label, vec2 size, child_flags flags)
{
    if (cur_ != nullptr && child_depth_ >= max_child_depth) {
        internal::limit_reached("child regions inside child regions (max_child_depth)", max_child_depth);
    }
    if (cur_ == nullptr || child_depth_ >= max_child_depth) {
        return false;
    }
    const id key = hash_id(id_label, current_seed());
    child_state* st = state_for(children_, key, frame_);

    // width: the rest of the line (right of the previous item after same_line), height: down to the window bottom
    f32 w = size.x;
    if (w <= 0.0f) {
        w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
        if (layout_.same_line && !layout_.first) {
            w = layout_.origin.x + layout_.width - (layout_.cursor_x + style_.item_spacing);
        }
    }
    layout_.next_width = 0.0f;
    f32 h = size.y;
    if (h <= 0.0f) {
        h = layout_.bound_bottom > 0.0f ? layout_.bound_bottom - layout_next_y() : 240.0f;
    }
    w = std::max(w, 24.0f);
    h = std::max(h, 24.0f);

    const rect r = layout_place({w, h});

    if (has_flag(flags, child_flags::acrylic)) {
        dl_.backdrop(r, style_.blur_radius, darken(style_.window_bg, 0.25f).scaled_alpha(style_.acrylic_alpha * 0.8f),
                     radii(style_.rounding * 0.8f), style_.acrylic_noise, style_.acrylic_saturation, style_.acrylic_brightness);
    }
    if (has_flag(flags, child_flags::frame)) {
        shape_style bg;
        bg.radius       = radii(style_.rounding * 0.8f);
        bg.fill_top     = darken(style_.widget_bg, 0.30f).scaled_alpha(0.55f);
        bg.fill_bottom  = bg.fill_top;
        bg.border       = style_.border;
        bg.border_width = style_.border_width;
        dl_.shape(r, bg);
    }

    const f32  pad   = has_flag(flags, child_flags::no_padding) ? 0.0f : style_.padding * 0.7f;
    const rect inner = {{r.min.x + pad, r.min.y + pad}, {r.max.x - pad, r.max.y - pad}};

    const f32 max_scroll = std::max(0.0f, st->content_h - inner.height());
    st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);

    dl_.push_clip({{r.min.x + 1.0f, r.min.y + 1.0f}, {r.max.x - 1.0f, r.max.y - 1.0f}});

    child_stack_[child_depth_++] = {st, r, inner, layout_, flags};
    push_id(id_label);

    layout_              = {};
    layout_.origin       = {inner.min.x, inner.min.y - st->scroll};
    layout_.width        = std::max(inner.width() - (st->overflow && !has_flag(flags, child_flags::no_scrollbar) ? 10.0f : 0.0f), 1.0f);
    layout_.bound_bottom = inner.max.y;
    return true;
}

void context::end_child()
{
    if (child_depth_ == 0) {
        return;
    }
    const child_frame f = child_stack_[--child_depth_];
    child_state& st     = *f.state;

    st.content_h = layout_.first ? 0.0f : layout_.bottom - layout_.origin.y;
    const f32 view_h = f.inner.height();
    st.overflow = st.content_h > view_h + 0.5f;

    if (st.overflow) {
        const f32 max_scroll = st.content_h - view_h;
        if (wheel_ != 0.0f && !wheel_consumed_ && pointer_over(f.bounds)) {
            st.scroll = std::clamp(st.scroll - wheel_ * 48.0f, 0.0f, max_scroll);
            wheel_consumed_ = true;
        }
        if (!has_flag(f.flags, child_flags::no_scrollbar)) {
            const f32  track_top = f.bounds.min.y + 5.0f;
            const f32  track_h = f.bounds.height() - 10.0f;
            const f32  thumb_h = std::max(20.0f, track_h * view_h / st.content_h);
            const f32  x0      = f.bounds.max.x - 9.0f;
            // the whole track takes the press: a click beside the thumb moves it there, and it can be dragged from anywhere
            f32 thumb_y = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const interaction in = interact(hash_id("##cscroll", current_seed()), {{x0 - 3.0f, track_top}, {x0 + 8.0f, track_top + track_h}});
            st.scroll = thumb_drag(in, st.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, st.scroll);
            thumb_y   = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const rect thumb = {{x0, thumb_y}, {f.bounds.max.x - 4.0f, thumb_y + thumb_h}};
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.4f);
            bar.fill_bottom = bar.fill_top;
            dl_.shape(thumb, bar);
        }
    } else {
        st.scroll = 0.0f;
    }

    dl_.pop_clip();
    layout_ = f.outer;
    pop_id();
}

// group cards -----------------------------------------------------------------------------------

bool context::begin_card(std::string_view title, std::string_view icon, font_id icon_font)
{
    if (cur_ != nullptr && card_depth_ >= max_card_depth) {
        internal::limit_reached("cards inside cards (max_card_depth)", max_card_depth);
    }
    if (cur_ == nullptr || card_depth_ >= max_card_depth) {
        return false;
    }
    const font_id f = current_font();
    const id key    = hash_id(title, current_seed());
    card_state* st  = state_for(cards_, key, frame_);

    const std::string_view shown = visible_label(title);
    const bool has_head = !shown.empty() || !icon.empty();
    const f32  lh       = font_.line_height(f);
    const f32  head_h   = has_head ? lh + 16.0f : 0.0f;
    const f32  pad      = style_.padding * 0.85f;

    const f32 w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    layout_.next_width = 0.0f;
    // the height comes from last frame's content; the very first frame shows just the header
    const rect r = layout_place({w, head_h + pad + st->content_h + pad});

    shape_style bg;
    bg.radius       = radii(style_.rounding);
    bg.fill_top     = lighten(style_.widget_bg, style_.gradient * 0.4f).scaled_alpha(0.42f);
    bg.fill_bottom  = darken(style_.widget_bg, 0.15f).scaled_alpha(0.42f);
    bg.border       = style_.border;
    bg.border_width = style_.border_width;
    dl_.shape(r, bg);

    if (has_head) {
        dl_.rect_filled({{r.min.x + 1.0f, r.min.y + head_h}, {r.max.x - 1.0f, r.min.y + head_h + 1.0f}}, style_.border.scaled_alpha(0.7f));
        shape_style mark; // accent tick before the title
        mark.radius      = radii(1.5f);
        mark.fill_top    = style_.accent_hover;
        mark.fill_bottom = style_.accent;
        dl_.shape({{r.min.x + 10.0f, r.min.y + (head_h - lh * 0.75f) * 0.5f}, {r.min.x + 13.0f, r.min.y + (head_h + lh * 0.75f) * 0.5f}}, mark);

        f32 x = r.min.x + 20.0f;
        if (!icon.empty()) {
            const vec2 isize = font_.measure(icon_font, icon);
            dl_.text({x, r.min.y + (head_h - isize.y) * 0.5f}, style_.accent_hover, icon, icon_font);
            x += isize.x + 8.0f;
        }
        const f32 title_h = shown.empty() ? lh : label_size(f, shown).y;
        label_draw({x, r.min.y + (head_h - title_h) * 0.5f}, style_.text, shown, f);
    }

    card_stack_[card_depth_++] = {st, layout_};
    push_id(title);
    layout_              = {};
    layout_.origin       = {r.min.x + pad, r.min.y + head_h + pad};
    layout_.width        = std::max(w - 2.0f * pad, 1.0f);
    return true;
}

void context::end_card()
{
    if (card_depth_ == 0) {
        return;
    }
    const card_frame f = card_stack_[--card_depth_];
    f.state->content_h = layout_.first ? 0.0f : layout_.bottom - layout_.origin.y;
    layout_ = f.outer;
    pop_id();
}

// vertical tab strip ----------------------------------------------------------------------------------

bool context::tab_strip(std::string_view id_label, const tab_desc* tabs, std::size_t count, int& selected,
                        font_id icon_font, f32 width, tab_strip_flags flags, f32 height)
{
    if (cur_ == nullptr || count == 0) {
        return false;
    }
    push_id(id_label);

    const bool icons_only = flags == tab_strip_flags::icons_only;
    const font_id f       = current_font();
    const f32  lh         = font_.line_height(f);
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
                const f32 iw = tabs[i].icon.empty() ? 0.0f : font_.measure(icon_font, tabs[i].icon).x + 12.0f;
                widest = std::max(widest, iw + label_size(f, visible_label(tabs[i].label)).x);
            }
            w = std::max(widest + 44.0f, 120.0f);
        }
    }
    const f32 natural_h = static_cast<f32>(n) * (row_h + gap) + 16.0f;
    f32 h = height;
    if (h <= 0.0f) {
        h = layout_.bound_bottom > 0.0f ? std::max(layout_.bound_bottom - layout_next_y(), natural_h) : natural_h;
    }

    const rect r = layout_place({w, h});
    shape_style panel;
    panel.radius       = radii(style_.rounding);
    panel.fill_top     = lighten(style_.title_bg, style_.gradient * 0.5f).scaled_alpha(0.85f);
    panel.fill_bottom  = darken(style_.title_bg, 0.1f).scaled_alpha(0.85f);
    panel.border       = style_.border;
    panel.border_width = style_.border_width;
    dl_.shape(r, panel);

    std::array<rect, 16> cells{};
    f32 y = r.min.y + 8.0f;
    for (std::size_t i = 0; i < n; ++i) {
        cells[i] = {{r.min.x + 6.0f, y}, {r.max.x - 6.0f, y + row_h}};
        y += row_h + gap;
    }

    bool changed = false;
    std::array<f32, 16> emphasis{};
    for (std::size_t i = 0; i < n; ++i) {
        const id key = hash_id(tabs[i].label, current_seed());
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
            hv.radius      = radii(style_.rounding * 0.7f);
            hv.fill_top    = style_.widget_hover.scaled_alpha(0.55f * a.hover);
            hv.fill_bottom = hv.fill_top;
            dl_.shape(cells[i], hv);
        }
        if (icons_only && in.hovered && hover_time_ > 0.35f) {
            draw_tooltip(visible_label(tabs[i].label));
        }
    }

    const rect sel = cells[static_cast<std::size_t>(selected)];
    const f32  iy  = r.min.y + animate("strip_y", sel.min.y - r.min.y, 22.0f); // (relative to the strip: moving the window must not make it lag)
    shape_style pill;
    pill.radius      = radii(style_.rounding * 0.7f);
    pill.fill_top    = style_.accent.scaled_alpha(0.22f);
    pill.fill_bottom = style_.accent.scaled_alpha(0.10f);
    pill.border      = style_.accent.scaled_alpha(0.35f);
    pill.border_width = 1.0f;
    dl_.shape({{sel.min.x, iy}, {sel.max.x, iy + row_h}}, pill);
    shape_style bar;
    bar.radius      = radii(1.5f);
    bar.fill_top    = style_.accent_hover;
    bar.fill_bottom = style_.accent;
    dl_.shape({{sel.min.x + 1.0f, iy + 9.0f}, {sel.min.x + 4.0f, iy + row_h - 9.0f}}, bar);

    for (std::size_t i = 0; i < n; ++i) {
        const color c = lerp(style_.text_dim, style_.text, emphasis[i]);
        const bool has_icon = !tabs[i].icon.empty();
        const vec2 isize = has_icon ? font_.measure(icon_font, tabs[i].icon) : vec2{};
        const color ic   = lerp(c, style_.accent_hover, emphasis[i] * 0.7f);
        if (icons_only) {
            if (has_icon) {
                dl_.text({cells[i].center().x - isize.x * 0.5f, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
            }
        } else {
            f32 x = cells[i].min.x + 16.0f;
            if (has_icon) {
                dl_.text({x, cells[i].center().y - isize.y * 0.5f}, ic, tabs[i].icon, icon_font);
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
    if (in_overlay_ || text.empty()) {
        return;
    }
    const font_id f  = current_font();
    const vec2 ts    = label_size(f, text);
    const f32  padx  = 9.0f;
    const f32  pady  = 6.0f;
    const vec2 size{ts.x + 2.0f * padx, ts.y + 2.0f * pady};

    vec2 pos = mouse_ + vec2{14.0f, 20.0f};
    if (pos.x + size.x > display_.x - 4.0f) { pos.x = display_.x - 4.0f - size.x; }
    if (pos.y + size.y > display_.y - 4.0f) { pos.y = mouse_.y - size.y - 10.0f; }
    pos.x = std::max(pos.x, 4.0f);
    pos.y = std::max(pos.y, 4.0f);

    const f32 fade = std::clamp((hover_time_ - 0.35f) / 0.12f, 0.0f, 1.0f);
    const u32 previous_owner = run_owner_;
    switch_run(run_overlay);
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
    dl_.push_alpha(fade);

    shape_style body;
    body.radius        = radii(style_.rounding * 0.6f);
    body.fill_top      = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = style_.border;
    body.border_width  = style_.border_width;
    body.shadow        = style_.shadow;
    body.shadow_blur   = style_.shadow_blur * 0.5f;
    body.shadow_offset = {0.0f, 3.0f};
    popup_panel(rect::from_size(pos, size), body);
    label_draw({pos.x + padx, pos.y + pady}, style_.text, text, f);

    dl_.pop_alpha();
    dl_.pop_clip();
    switch_run(previous_owner);
}

void context::tooltip(std::string_view text)
{
    if (last_item_hovered_ && last_item_key_ == hover_key_cur_ && hover_time_ > 0.4f) {
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

// `chord` is null for a plain key; otherwise key_code is chord->key and the modifiers held with the key are stored too
bool context::hotkey_field(std::string_view label, u32& key_code, key_chord* chord)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool capturing = hotkey_capture_ == key;
    if (in.pressed) {
        capturing       = !capturing;
        hotkey_capture_ = capturing ? key : 0;
    } else if (capturing && mouse_pressed_ && !box.contains(mouse_)) {
        capturing       = false;
        hotkey_capture_ = 0;
    }

    bool changed = false;
    if (capturing && pressed_key_ != 0) {
        const bool bare = chord == nullptr || !(mod_ctrl_ || mod_shift_ || mod_alt_);
        if (pressed_key_ == 0x1b && bare) {                                     // Esc: leave it as it was
        } else if ((pressed_key_ == 0x08 || pressed_key_ == 0x2e) && bare) {   // Backspace / Delete: unbind
            changed  = key_code != 0;
            key_code = 0;
            if (chord != nullptr) { *chord = {}; }
        } else {
            const key_chord next{pressed_key_, mod_ctrl_, mod_shift_, mod_alt_};
            changed  = chord != nullptr ? *chord != next : key_code != pressed_key_;
            key_code = pressed_key_;
            if (chord != nullptr) { *chord = next; }
        }
        capturing       = false;
        hotkey_capture_ = 0;
        pressed_key_    = 0;
        key_count_      = 0;
    }
    if (capturing) {
        hotkey_seen_ = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, capturing ? 1.0f : 0.0f);

    shape_style field = widget_shape(lerp(style_.widget_bg, style_.widget_hover, a.hover), style_.rounding * 0.8f);
    field.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
    field.shadow      = style_.accent.scaled_alpha(0.3f * a.toggle);
    field.shadow_blur = 8.0f * a.toggle;
    dl_.shape(box, field);

    std::string chord_text;
    if (chord != nullptr && !capturing) { chord_text = chord_to_string(*chord); }
    std::string_view shown = capturing ? std::string_view{"press a key..."}
                           : chord != nullptr ? (chord_text.empty() ? std::string_view{"None"} : std::string_view{chord_text})
                                              : key_name(key_code);
    color tc = key_code == 0 && !capturing ? style_.text_dim : style_.text;
    if (capturing) {
        const f32 pulse = 0.65f + 0.35f * std::sin(static_cast<f32>(time_) * 7.0f);
        tc = style_.accent_hover.scaled_alpha(pulse);
    }
    const vec2 tsize = font_.measure(f, shown);
    dl_.text({box.min.x + (box.width() - tsize.x) * 0.5f, box.min.y + (box.height() - tsize.y) * 0.5f}, tc, shown, f);
    return changed;
}

// page transitions -----------------------------------------------------------------------------------

transition_scope context::page_transition(std::string_view key, int page, f32 slide)
{
    anim_slot& a = anim_for(hash_id(key, current_seed()));
    const f32 pf = static_cast<f32>(page);
    if (!a.custom_init) {
        a.custom_init = true;
        a.custom      = pf;
        a.toggle      = 1.0f;
    } else if (a.custom != pf) {
        a.custom = pf; // a new page: start the fade over
        a.toggle = 0.0f;
    } else {
        a.toggle = approach(a.toggle, 1.0f, style_.anim_speed * 0.65f);
    }
    const f32 t = smooth(std::clamp(a.toggle, 0.0f, 1.0f));

    dl_.push_alpha(t);
    if (layout_.first) {
        layout_.origin.y += (1.0f - t) * slide;
    }
    return transition_scope{*this};
}

} // namespace strata

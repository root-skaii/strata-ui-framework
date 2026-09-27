// menus: popup menus (context menus), submenus, the main menu bar

#include "strata/context.hpp"

#include "context_impl.hpp"


#include <algorithm>
#include <cctype>
#include <charconv>

namespace strata {

namespace {

constexpr f32 menu_pad      = 4.0f;  // between the popup's edge and its rows
constexpr f32 check_column  = 26.0f; // room for the check mark / icon on the left of a row
constexpr f32 arrow_column  = 22.0f; // and for the submenu arrow on the right

// "&File": the character after a '&' is the mnemonic; "&&" is a literal '&'
struct mnemonic_label {
    std::string text;       // the label without the markers
    std::size_t at{};       // where the mnemonic character sits in `text` ...
    std::size_t len{};      // ... and how many bytes (0: none)
    char        key{};      // lower-case ASCII letter or digit it stands for, 0 if it is something else
};

[[nodiscard]] mnemonic_label parse_mnemonic(std::string_view s)
{
    mnemonic_label m;
    m.text.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&' && i + 1 < s.size()) {
            if (s[i + 1] == '&') {
                m.text += '&';
                ++i;
                continue;
            }
            if (m.len == 0) {
                const auto lead = static_cast<unsigned char>(s[i + 1]);
                const std::size_t n = lead < 0x80 ? 1u : (lead >= 0xf0 ? 4u : (lead >= 0xe0 ? 3u : 2u));
                m.at  = m.text.size();
                m.len = std::min(n, s.size() - i - 1);
                m.text.append(s.substr(i + 1, m.len));
                if (m.len == 1 && std::isalnum(lead) != 0) { m.key = static_cast<char>(std::tolower(lead)); }
                i += m.len;
            }
            continue; // (a second single '&' is dropped)
        }
        m.text += s[i];
    }
    return m;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
    }
    return true;
}

[[nodiscard]] std::string_view trim_spaces(std::string_view s) noexcept
{
    while (!s.empty() && s.front() == ' ') { s.remove_prefix(1); }
    while (!s.empty() && s.back() == ' ') { s.remove_suffix(1); }
    return s;
}

// the virtual-key code a name of the accelerator syntax stands for; 0 if unknown
[[nodiscard]] u32 key_from_name(std::string_view t) noexcept
{
    if (t.size() == 1) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { return static_cast<u32>(c); }
    }
    std::string lower;
    for (const char c : t) { lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (lower.size() >= 2 && lower[0] == 'f') {
        u32 n = 0;
        const auto r = std::from_chars(lower.data() + 1, lower.data() + lower.size(), n);
        if (r.ec == std::errc{} && r.ptr == lower.data() + lower.size() && n >= 1 && n <= 24) { return 0x70u + n - 1; }
    }
    struct named { std::string_view name; u32 vk; };
    static constexpr named names[] = {
        {"enter", 0x0d}, {"return", 0x0d}, {"esc", 0x1b}, {"escape", 0x1b}, {"space", 0x20}, {"tab", 0x09},
        {"backspace", 0x08}, {"del", 0x2e}, {"delete", 0x2e}, {"ins", 0x2d}, {"insert", 0x2d}, {"home", 0x24},
        {"end", 0x23}, {"pgup", 0x21}, {"pageup", 0x21}, {"pgdn", 0x22}, {"pagedown", 0x22},
        {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
    };
    for (const named& n : names) {
        if (n.name == lower) { return n.vk; }
    }
    for (u32 vk = 1; vk < 0xff; ++vk) { // whatever key_name() prints: "Page Up", "Num +", "Mouse 4", ";" ...
        const std::string_view name = key_name(vk);
        if (name != "Key ?" && iequals(name, t)) { return vk; }
    }
    return 0;
}

} // namespace

std::string chord_to_string(const key_chord& chord)
{
    if (!chord.bound()) { return {}; }
    std::string s;
    if (chord.ctrl)  { s += "Ctrl+"; }
    if (chord.shift) { s += "Shift+"; }
    if (chord.alt)   { s += "Alt+"; }
    s += key_name(chord.key);
    return s;
}

bool chord_from_string(std::string_view text, key_chord& out) noexcept
{
    key_chord c;
    std::string_view rest = trim_spaces(text);
    if (rest.empty()) {
        out = c;
        return true;
    }
    for (;;) { // modifiers, in any order; what is left is the key, which may itself contain a '+' ("Num +")
        const std::size_t plus = rest.find('+');
        if (plus == std::string_view::npos) { break; }
        const std::string_view head = trim_spaces(rest.substr(0, plus));
        bool* mod = iequals(head, "ctrl") || iequals(head, "control") ? &c.ctrl
                  : iequals(head, "shift")                             ? &c.shift
                  : iequals(head, "alt")                               ? &c.alt
                                                                       : nullptr;
        if (mod == nullptr) { break; }
        *mod = true;
        rest = trim_spaces(rest.substr(plus + 1));
    }
    c.key = key_from_name(rest);
    if (c.key == 0) { return false; }
    out = c;
    return true;
}

std::string sequence_to_string(const key_sequence& seq)
{
    std::string s;
    for (u32 i = 0; i < seq.count; ++i) {
        if (i > 0) { s += ", "; }
        s += chord_to_string(seq.steps[i]);
    }
    return s;
}

bool sequence_from_string(std::string_view text, key_sequence& out) noexcept
{
    key_sequence seq;
    std::string_view rest = trim_spaces(text);
    if (rest.empty()) {
        out = seq;
        return true;
    }
    for (;;) {
        if (seq.count >= key_sequence::max_steps) { return false; } // too many steps
        const std::size_t comma = rest.find(',');
        const std::string_view part = trim_spaces(comma == std::string_view::npos ? rest : rest.substr(0, comma));
        key_chord chord;
        if (!chord_from_string(part, chord) || !chord.bound()) { return false; } // a step must be a real chord
        seq.steps[seq.count++] = chord;
        if (comma == std::string_view::npos) { break; }
        rest = trim_spaces(rest.substr(comma + 1));
    }
    out = seq;
    return true;
}

bool context::accelerator(std::string_view combo) const
{
    key_chord c;
    return chord_from_string(combo, c) && c.bound() && chord_pressed(c);
}

bool context::chord_pressed(const key_chord& c) const
{
    const u32  vk    = c.key;
    const bool ctrl  = c.ctrl;
    const bool shift = c.shift;
    const bool alt   = c.alt;
    if (vk == 0 || m_->pressed_key_ != vk || m_->hotkey_capture_ != 0) {
        return false;
    }
    if (m_->press_ctrl_ != ctrl || m_->press_shift_ != shift || m_->press_alt_ != alt) {
        return false;
    }
    if (want_text_input()) { // the text field owns plain keys and its own editing shortcuts
        if (!ctrl && !alt) { return false; }
        if (ctrl && !alt && !shift && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'Y')) { return false; }
    }
    return true;
}

bool context::sequence_pressed(const key_sequence& seq) const
{
    if (seq.count == 0) {
        return false;
    }
    // does this frame continue the prefix that is already pending? (a different sequence sharing that same prefix
    // asked about first is fine: only the steps matter, not which action they belong to. two sequences can also share
    // a prefix and diverge at this very step - the one this frame's key does not complete just returns false below;
    // begin_frame() is what clears a prefix that nothing continued, once every candidate has had a chance to)
    const bool continues = m_->seq_pending_count_ > 0 && m_->seq_pending_count_ <= seq.count &&
                            std::equal(m_->seq_pending_.begin(), m_->seq_pending_.begin() + m_->seq_pending_count_, seq.steps.begin()) &&
                            m_->time_ - m_->seq_pending_time_ <= key_sequence_timeout;
    const u32 at = continues ? m_->seq_pending_count_ : 0u;
    if (!chord_pressed(seq.steps[at])) {
        return false;
    }
    m_->seq_pending_touched_ = true; // this frame's key correctly advanced (or completed) a sequence sharing this prefix
    if (at + 1 == seq.count) {   // that was the last step
        m_->seq_pending_count_ = 0;
        return true;
    }
    m_->seq_pending_[at]    = seq.steps[at]; // one step matched, more to come: remember it and wait for the next
    m_->seq_pending_count_  = static_cast<u8>(at + 1);
    m_->seq_pending_time_   = m_->time_;
    return false;
}

void context::draw_mnemonic(vec2 pos, color c, std::string_view text, std::size_t at, std::size_t len, bool underline, font_id f)
{
    if (len == 0 || !underline || m_->rich_depth_ > 0) {
        label_draw(pos, c, text, f);
        return;
    }
    const std::string_view before = text.substr(0, at);
    const std::string_view mid    = text.substr(at, len);
    const std::string_view after  = text.substr(at + len);
    const f32 x1 = m_->font_.measure(f, before).x;
    const f32 x2 = x1 + m_->font_.measure(f, mid).x;
    if (!before.empty()) { m_->dl_.text(pos, c, before, f); }
    m_->dl_.text({pos.x + x1, pos.y}, c, mid, f, text_flags::underline);
    if (!after.empty()) { m_->dl_.text({pos.x + x2, pos.y}, c, after, f); }
}

void context::menu_close_from(u32 level) noexcept
{
    for (u32 i = level; i < max_menu_levels; ++i) {
        m_->menu_open_[i] = {};
    }
}

bool context::over_open_menu(vec2 p) const noexcept
{
    for (const menu_level& m : m_->menu_open_) {
        if (m.key != 0 && m.rect_prev.contains(p)) { return true; }
    }
    return false;
}

void context::menu_open_root(id key, vec2 pos, const rect& anchor, bool from_bar)
{
    menu_close_all();
    menu_level& m = m_->menu_open_[0];
    m.key      = key;
    m.pos      = pos;
    m.anchor   = anchor;
    m.size     = {};
    m.age      = 0.0f;
    m.from_bar = from_bar;
    m.seen     = true;
}

// draws the popup frame of one level in the overlay layer and redirects the layout into it
bool context::menu_begin_level(u32 level, id menu_key)
{
    if (level >= max_menu_levels || m_->menu_depth_ >= max_menu_levels) {
        return false;
    }
    menu_level& m = m_->menu_open_[level];
    if (m.key != menu_key) {
        return false;
    }
    m.seen = true;

    if (level == 0 && m_->focus_id_ == 0) { // Esc closes everything
        for (u32 i = 0; i < m_->key_count_; ++i) {
            if (m_->keys_[i].k == key::escape) {
                menu_close_all();
                return false;
            }
        }
    }

    const bool measured = m.size.x > 0.0f;
    const vec2 size = measured ? m.size : vec2{160.0f, 40.0f};
    m.age += m_->dt_; // (the opening fade: an animation, so the capped step)

    vec2 pos = m.pos;
    if (pos.x + size.x > m_->display_.x - 4.0f) {
        pos.x = level > 0 ? m.anchor.min.x - size.x + 2.0f : m_->display_.x - 4.0f - size.x; // submenus flip to the left
    }
    pos.x = std::max(pos.x, 4.0f);
    if (pos.y + size.y > m_->display_.y - 4.0f) {
        pos.y = std::max(4.0f, m_->display_.y - 4.0f - size.y);
    }
    m.rect_cur = {pos, pos + size};
    const f32 lin  = std::clamp(m.age / 0.12f, 0.0f, 1.0f);
    const f32 fade = measured ? lin * lin * (3.0f - 2.0f * lin) : 0.0f; // eased at both ends; the first frame only measures

    menu_frame& frame = m_->menu_stack_[m_->menu_depth_];
    frame = {};
    frame.saved_layout  = m_->layout_;
    frame.prev_owner    = m_->run_owner_;
    frame.saved_spacing = m_->style_.item_spacing;
    frame.level         = level;
    frame.saved_overlay = m_->in_overlay_;
    // letter keys reach the deepest popup that is open (and only once it has been measured)
    frame.keys_ok = measured && m_->focus_id_ == 0 && !m_->mod_ctrl_ && !m_->mod_alt_ &&
                    (level + 1 >= max_menu_levels || m_->menu_open_[level + 1].key == 0);

    switch_run(run_overlay);
    m_->in_overlay_ = true;
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
    m_->dl_.push_alpha(fade);

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.8f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom   = body.fill_top;
    body.border        = m_->style_.border;
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.7f;
    body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.25f};
    popup_panel(m.rect_cur, body);

    m_->dl_.push_clip({{pos.x + 1.0f, pos.y + 1.0f}, {pos.x + size.x - 1.0f, pos.y + size.y - 1.0f}});
    m_->style_.item_spacing = 1.0f; // rows touch
    m_->layout_        = {};
    m_->layout_.origin = {pos.x + menu_pad, pos.y + menu_pad};
    m_->layout_.width  = size.x - 2.0f * menu_pad;
    ++m_->menu_depth_;
    return true;
}

void context::menu_end_level()
{
    if (m_->menu_depth_ == 0) {
        return;
    }
    const menu_frame frame = m_->menu_stack_[--m_->menu_depth_];
    menu_level& m = m_->menu_open_[frame.level];

    // the size is only known now; it is used from the next frame on
    const f32 content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;
    m.size = {std::max(frame.content_w + 2.0f * menu_pad, 120.0f), content_h + 2.0f * menu_pad};

    m_->dl_.pop_clip();
    m_->dl_.pop_alpha();
    m_->dl_.pop_clip();
    m_->style_.item_spacing = frame.saved_spacing;
    m_->layout_             = frame.saved_layout;
    m_->in_overlay_         = frame.saved_overlay;
    switch_run(frame.prev_owner);
}

// rows -------------------------------------------------------------------------------------------------

bool context::menu_item(std::string_view label, std::string_view shortcut, bool selected, bool enabled)
{
    menu_item_options o;
    o.shortcut = shortcut;
    o.selected = selected;
    o.enabled  = enabled;
    return menu_item(label, o);
}

bool context::menu_item(std::string_view label, const menu_item_options& o)
{
    if (m_->menu_depth_ == 0) {
        return false;
    }
    menu_frame& frame = m_->menu_stack_[m_->menu_depth_ - 1];
    const font_id f = current_font();
    const id key = widget_id(label);
    const mnemonic_label m = parse_mnemonic(visible_label(label));

    const vec2 ts = label_size(f, m.text);
    const f32  sw = o.shortcut.empty() ? 0.0f : m_->font_.measure(f, o.shortcut).x;
    frame.content_w = std::max(frame.content_w, check_column + ts.x + (o.shortcut.empty() ? 0.0f : 28.0f + sw) + 12.0f);

    const f32  row_h = std::max(frame_height() - 2.0f, ts.y + 6.0f);
    const rect row   = layout_place({m_->layout_.width, row_h});
    const interaction in = o.enabled ? interact(key, row) : interaction{};

    // the mnemonic key: the first enabled row with that letter takes it
    bool by_key = false;
    if (frame.keys_ok && o.enabled && m.key != 0 && m_->typed_len_ > 0 &&
        std::tolower(static_cast<unsigned char>(m_->typed_[0])) == m.key) {
        by_key     = true;
        m_->typed_len_ = 0;
    }

    if (in.hovered) {
        menu_close_from(frame.level + 1); // a plain row closes a submenu that was open beside it
    }
    anim_slot* a = anim_find(key);
    if (a == nullptr && in.hovered) { a = &anim_for(key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f, m_->style_.anim_speed * 1.5f);
        hover    = a->hover;
        a->last_frame = (hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
    }
    if (hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = m_->style_.accent.scaled_alpha(0.30f * hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape(row, bg);
    }

    const color ink = o.enabled ? m_->style_.text : m_->style_.text_dim.scaled_alpha(0.6f);
    const vec2  cc{row.min.x + check_column * 0.5f, row.center().y}; // centre of the icon column
    if (o.image != 0) {
        const rect img = rect::from_size({cc.x - 8.0f, cc.y - 8.0f}, {16.0f, 16.0f});
        m_->dl_.image(img, o.image, {0.0f, 0.0f}, {1.0f, 1.0f}, o.enabled ? color{255, 255, 255, 255} : color{255, 255, 255, 110}, radii(3.0f));
        if (o.selected) {
            shape_style ring;
            ring.radius       = radii(4.0f);
            ring.border       = m_->style_.accent_hover;
            ring.border_width = 1.5f;
            m_->dl_.shape(img.expanded(2.0f), ring);
        }
    } else if (!o.icon.empty()) {
        const vec2 gs = m_->font_.measure(o.icon_font, o.icon);
        if (o.selected) { // a checked row with an icon: the icon sits on an accent chip
            shape_style chip;
            chip.radius      = radii(5.0f);
            chip.fill_top    = m_->style_.accent.scaled_alpha(0.28f);
            chip.fill_bottom = chip.fill_top;
            m_->dl_.shape(rect::from_size({cc.x - 11.0f, cc.y - 11.0f}, {22.0f, 22.0f}), chip);
        }
        const color icol = o.selected ? m_->style_.accent_hover : (o.icon_color.a != 0 ? o.icon_color : ink);
        m_->dl_.text({cc.x - gs.x * 0.5f, cc.y - gs.y * 0.5f}, o.enabled ? icol : icol.scaled_alpha(0.5f), o.icon, o.icon_font);
    } else if (o.selected) {
        m_->dl_.line({cc.x - 4.0f, cc.y}, {cc.x - 1.0f, cc.y + 3.5f}, m_->style_.accent_hover, 1.8f);
        m_->dl_.line({cc.x - 1.0f, cc.y + 3.5f}, {cc.x + 5.0f, cc.y - 3.5f}, m_->style_.accent_hover, 1.8f);
    }
    draw_mnemonic({row.min.x + check_column, row.min.y + (row.height() - ts.y) * 0.5f}, ink, m.text, m.at, m.len, true, f);
    if (!o.shortcut.empty()) {
        m_->dl_.text({row.max.x - 12.0f - sw, row.min.y + (row.height() - m_->font_.line_height(f)) * 0.5f}, m_->style_.text_dim, o.shortcut, f);
    }

    if (in.pressed || by_key) {
        if (!o.keep_open) { m_->menu_close_all_ = true; }
        return true;
    }
    return false;
}

bool context::menu_item(std::string_view label, bool& checked, std::string_view shortcut, bool enabled)
{
    menu_item_options o;
    o.shortcut = shortcut;
    o.enabled  = enabled;
    return menu_item(label, checked, o);
}

bool context::menu_item(std::string_view label, bool& checked, const menu_item_options& options)
{
    menu_item_options o = options;
    o.selected = checked;
    if (menu_item(label, o)) {
        checked = !checked;
        return true;
    }
    return false;
}

void context::menu_separator()
{
    if (m_->menu_depth_ == 0) {
        return;
    }
    const rect r = layout_place({m_->layout_.width, 7.0f});
    m_->dl_.rect_filled({{r.min.x + 6.0f, r.center().y}, {r.max.x - 6.0f, r.center().y + 1.0f}}, m_->style_.border);
}

// menus and submenus ---------------------------------------------------------------------------------------

bool context::begin_menu(std::string_view label, bool enabled)
{
    const font_id f = current_font();
    const id key = widget_id(label);
    const mnemonic_label m = parse_mnemonic(visible_label(label));
    const vec2 ts = label_size(f, m.text);

    if (m_->menu_depth_ > 0) { // a row of the popup that opens a submenu
        menu_frame& frame = m_->menu_stack_[m_->menu_depth_ - 1];
        const u32 level = frame.level;
        if (level + 1 >= max_menu_levels) {
            report_limit("submenu levels (max_menu_levels)", max_menu_levels);
            return false;
        }
        frame.content_w = std::max(frame.content_w, check_column + ts.x + arrow_column + 12.0f);
        const f32  row_h = std::max(frame_height() - 2.0f, ts.y + 6.0f);
        const rect row   = layout_place({m_->layout_.width, row_h});
        const interaction in = enabled ? interact(key, row) : interaction{};

        bool by_key = false;
        if (frame.keys_ok && enabled && m.key != 0 && m_->typed_len_ > 0 &&
            std::tolower(static_cast<unsigned char>(m_->typed_[0])) == m.key) {
            by_key     = true;
            m_->typed_len_ = 0;
        }
        bool open = m_->menu_open_[level + 1].key == key;
        if ((in.hovered || by_key) && !open) {
            menu_close_from(level + 1);
            menu_level& sub = m_->menu_open_[level + 1];
            sub.key    = key;
            sub.pos    = {row.max.x - 2.0f, row.min.y - menu_pad};
            sub.anchor = row;
            sub.size   = {};
            sub.age    = 0.0f;
            open = true;
        }
        if (in.hovered || open) {
            shape_style bg;
            bg.radius      = radii(m_->style_.rounding * 0.55f);
            bg.fill_top    = m_->style_.accent.scaled_alpha(in.hovered ? 0.30f : 0.20f);
            bg.fill_bottom = bg.fill_top;
            m_->dl_.shape(row, bg);
        }
        const color ink = enabled ? m_->style_.text : m_->style_.text_dim.scaled_alpha(0.6f);
        draw_mnemonic({row.min.x + check_column, row.min.y + (row.height() - ts.y) * 0.5f}, ink, m.text, m.at, m.len, true, f);
        const vec2 c{row.max.x - 12.0f, row.center().y};
        m_->dl_.triangle_filled({c.x - 3.0f, c.y - 4.0f}, {c.x + 2.5f, c.y}, {c.x - 3.0f, c.y + 4.0f}, ink);

        if (!open || !menu_begin_level(level + 1, key)) {
            return false;
        }
        push_id(label);
        return true;
    }

    if (!m_->in_menu_bar_ || m_->cur_ == nullptr) { // (only the bar has menus outside of popups)
        return false;
    }
    const rect r = layout_place({ts.x + 22.0f, m_->menu_bar_h_});
    same_line();
    const interaction in = enabled ? interact(key, r) : interaction{};
    bool open = m_->menu_open_[0].key == key;
    const bool bar_active = m_->menu_open_[0].key != 0 && m_->menu_open_[0].from_bar;
    const bool by_alt = enabled && m.key != 0 && m_->press_alt_ && m_->pressed_key_ == static_cast<u32>(std::toupper(static_cast<unsigned char>(m.key)));
    if (in.pressed || by_alt) {
        if (open) { menu_close_all(); } else { menu_open_root(key, {r.min.x, r.max.y}, r, true); }
        open = m_->menu_open_[0].key == key;
    } else if (in.hovered && !open && bar_active) { // while a menu is open, the others open on hover
        menu_open_root(key, {r.min.x, r.max.y}, r, true);
        open = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered || open ? 1.0f : 0.0f);
    if (a.hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.6f);
        bg.fill_top    = (open ? m_->style_.accent : m_->style_.widget_hover).scaled_alpha((open ? 0.30f : 0.75f) * a.hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape({{r.min.x + 1.0f, r.min.y + 2.0f}, {r.max.x - 1.0f, r.max.y - 2.0f}}, bg);
    }
    draw_mnemonic({r.min.x + 11.0f, r.min.y + (r.height() - ts.y) * 0.5f}, enabled ? m_->style_.text : m_->style_.text_dim, m.text, m.at, m.len, m_->mod_alt_, f);

    if (!open || !menu_begin_level(0, key)) {
        return false;
    }
    push_id(label);
    return true;
}

void context::end_menu()
{
    pop_id();
    menu_end_level();
}

// popup menus and context menus --------------------------------------------------------------------------

void context::open_popup_menu(std::string_view id_label, vec2 pos)
{
    const id key = hash_id("##popupmenu", widget_id(id_label));
    menu_open_root(key, pos, {}, false);
}

bool context::begin_popup_menu(std::string_view id_label)
{
    const id key = hash_id("##popupmenu", widget_id(id_label));
    if (m_->menu_open_[0].key != key || !menu_begin_level(0, key)) {
        return false;
    }
    push_id(id_label);
    return true;
}

void context::end_popup_menu()
{
    pop_id();
    menu_end_level();
}

bool context::begin_context_menu(std::string_view id_label)
{
    if (m_->cur_ != nullptr && m_->last_item_hovered_ && m_->mouse_right_pressed_) {
        open_popup_menu(id_label, m_->mouse_);
    }
    return begin_popup_menu(id_label);
}

bool context::begin_context_menu(std::string_view id_label, const rect& area)
{
    if (m_->cur_ != nullptr && m_->mouse_right_pressed_ && pointer_over(area)) {
        open_popup_menu(id_label, m_->mouse_);
    }
    return begin_popup_menu(id_label);
}

// the main menu bar -----------------------------------------------------------------------------------------------

bool context::begin_main_menu_bar()
{
    if (m_->cur_ != nullptr || m_->in_menu_bar_) {
        return false;
    }
    m_->menu_bar_h_ = frame_height();
    m_->next_window_menubar_ = true;
    constexpr window_flags flags = window_flags::no_title_bar | window_flags::no_move | window_flags::no_collapse |
                                   window_flags::no_background;
    if (!begin_window("##mainmenubar", {0.0f, 0.0f}, {m_->display_.x, m_->menu_bar_h_}, flags)) {
        return false;
    }
    const rect r = {{0.0f, 0.0f}, {m_->display_.x, m_->menu_bar_h_}};
    m_->dl_.rect_gradient_v(r, lerp(m_->style_.title_bg, color{255, 255, 255, m_->style_.title_bg.a}, m_->style_.gradient * 0.8f), m_->style_.title_bg);
    m_->dl_.rect_filled({{0.0f, m_->menu_bar_h_ - 1.0f}, {m_->display_.x, m_->menu_bar_h_}}, m_->style_.border);

    m_->in_menu_bar_ = true;
    m_->menu_bar_saved_spacing_ = m_->style_.item_spacing;
    m_->style_.item_spacing = 0.0f;
    m_->layout_.origin.x += 6.0f;
    m_->layout_.width    -= 12.0f;
    return true;
}

void context::end_main_menu_bar()
{
    if (!m_->in_menu_bar_) {
        return;
    }
    m_->in_menu_bar_ = false;
    m_->style_.item_spacing = m_->menu_bar_saved_spacing_;
    end_window();
}

} // namespace strata

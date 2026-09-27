// color picker, trees, selectable rows and tables: the heavier composite widgets of strata::context

#include "strata/context.hpp"

#include "context_impl.hpp"


#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>

namespace strata {

namespace {

[[nodiscard]] constexpr color lighten(color c, f32 k) noexcept { return lerp(c, color{255, 255, 255, c.a}, k); }
[[nodiscard]] constexpr color darken(color c, f32 k) noexcept  { return lerp(c, color{0, 0, 0, c.a}, k); }

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

tree_scope::~tree_scope()
{
    if (open_) {
        ctx_->tree_pop();
    }
}

bool context::pointer_over(const rect& r) const noexcept
{
    const bool in_window = m_->in_overlay_ || (m_->cur_window_ != 0 && m_->hovered_window_prev_ == m_->cur_window_);
    const bool blocked   = !m_->in_overlay_ && ((m_->popup_open_prev_ && m_->popup_rect_prev_.contains(m_->mouse_)) || m_->menu_hit_prev_);
    return in_window && !blocked && m_->dl_.clip().contains(m_->mouse_) && r.contains(m_->mouse_);
}

// generic popup ----------------------------------------------------------------------

// draws the popup frame in the overlay layer (above all windows) and redirects the layout into it,
// so ordinary widgets can be submitted until end_popup(). returns false while the popup is closed.
bool context::begin_popup_at(id key, const rect& anchor, vec2 size)
{
    if (m_->popup_id_ != key) {
        return false;
    }
    if (m_->focus_id_ == 0) {
        for (u32 i = 0; i < m_->key_count_; ++i) {
            if (m_->keys_[i].k == key::escape) {
                m_->popup_id_ = 0;
                return false;
            }
        }
    }

    rect list = {{anchor.min.x, anchor.max.y + 4.0f}, {anchor.min.x + size.x, anchor.max.y + 4.0f + size.y}};
    if (list.max.y > m_->display_.y - 4.0f && anchor.min.y - 4.0f - size.y >= 4.0f) {
        list = {{anchor.min.x, anchor.min.y - 4.0f - size.y}, {anchor.min.x + size.x, anchor.min.y - 4.0f}};
    }
    if (list.max.x > m_->display_.x - 4.0f) { // keep it on screen horizontally
        const f32 shift = list.max.x - (m_->display_.x - 4.0f);
        list = {{list.min.x - shift, list.min.y}, {list.max.x - shift, list.max.y}};
    }

    m_->popup_open_cur_   = true;
    m_->popup_rect_cur_   = list;
    m_->popup_anchor_cur_ = anchor;

    m_->popup_prev_owner_ = m_->run_owner_;
    switch_run(run_overlay);
    m_->in_overlay_ = true;
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});

    shape_style body;
    body.radius        = radii(m_->style_.rounding * 0.8f);
    body.fill_top      = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom   = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.border        = m_->style_.border;
    body.border_width  = m_->style_.border_width;
    body.shadow        = m_->style_.shadow;
    body.shadow_blur   = m_->style_.shadow_blur * 0.8f;
    body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.3f};
    popup_panel(list, body);

    m_->dl_.push_clip({{list.min.x + 1.0f, list.min.y + 1.0f}, {list.max.x - 1.0f, list.max.y - 1.0f}});

    m_->popup_saved_layout_ = m_->layout_;
    m_->layout_             = {};
    m_->layout_.origin      = {list.min.x + m_->style_.padding, list.min.y + m_->style_.padding};
    m_->layout_.width       = size.x - 2.0f * m_->style_.padding;
    return true;
}

void context::end_popup_at()
{
    m_->layout_ = m_->popup_saved_layout_;
    m_->dl_.pop_clip();
    m_->dl_.pop_clip();
    m_->in_overlay_ = false;
    switch_run(m_->popup_prev_owner_);
}

// color ----------------------------------------------------------------------------------

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
    if (m_->pick_key_ != key || m_->pick_last_ != c) {
        const hsv h = to_hsv(c);
        // hue / saturation are undefined for grays and black: keep what the user last had
        if (m_->pick_key_ == key && (c.r == c.g && c.g == c.b)) {
            m_->pick_v_ = h.v;
            m_->pick_s_ = 0.0f;
        } else {
            m_->pick_h_ = h.s > 0.0f ? h.h : (m_->pick_key_ == key ? m_->pick_h_ : 0.0f);
            m_->pick_s_ = h.s;
            m_->pick_v_ = h.v;
        }
        m_->pick_key_  = key;
        m_->pick_last_ = c;
    }

    bool changed = false;
    const f32 w        = m_->layout_.width;
    const f32 sv_h     = 150.0f;
    const f32 bar_h    = 14.0f;
    const id  seed     = current_seed();
    u8 alpha           = c.a;

    {
        const rect r = layout_place({w, sv_h});
        const interaction in = interact(hash_id("##sv", seed), r);
        if (in.held) {
            m_->pick_s_ = std::clamp((m_->mouse_.x - r.min.x) / r.width(), 0.0f, 1.0f);
            m_->pick_v_ = 1.0f - std::clamp((m_->mouse_.y - r.min.y) / r.height(), 0.0f, 1.0f);
            changed = true;
        }
        const color hue = from_hsv({m_->pick_h_, 1.0f, 1.0f}, 255);
        m_->dl_.rect_gradient(r, color{255, 255, 255, 255}, hue, hue, color{255, 255, 255, 255});
        m_->dl_.rect_gradient(r, color{0, 0, 0, 0}, color{0, 0, 0, 0}, color{0, 0, 0, 255}, color{0, 0, 0, 255});
        m_->dl_.rect_outline(r, m_->style_.border, 0.0f, 1.0f);

        const vec2 p{r.min.x + m_->pick_s_ * r.width(), r.min.y + (1.0f - m_->pick_v_) * r.height()};
        shape_style ring;
        ring.radius       = radii(6.0f);
        ring.fill_top     = from_hsv({m_->pick_h_, m_->pick_s_, m_->pick_v_}, 255);
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
        const interaction in = interact(hash_id("##hue", seed), r);
        if (in.held) {
            m_->pick_h_ = std::clamp((m_->mouse_.x - r.min.x) / r.width(), 0.0f, 0.9999f);
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

        const f32 x = r.min.x + m_->pick_h_ * r.width();
        shape_style knob;
        knob.radius       = radii(3.0f);
        knob.fill_top     = from_hsv({m_->pick_h_, 1.0f, 1.0f}, 255);
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
        const interaction in = interact(hash_id("##alpha", seed), r);
        if (in.held) {
            alpha   = static_cast<u8>(std::clamp((m_->mouse_.x - r.min.x) / r.width(), 0.0f, 1.0f) * 255.0f + 0.5f);
            changed = true;
        }
        draw_checker(r, 7.0f);
        const color solid = from_hsv({m_->pick_h_, m_->pick_s_, m_->pick_v_}, 255);
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
        c          = from_hsv({m_->pick_h_, m_->pick_s_, m_->pick_v_}, with_alpha ? alpha : c.a);
        m_->pick_last_ = c;
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
                if (!with_alpha) { c.a = m_->pick_last_.a; }
                const hsv h = to_hsv(c);
                m_->pick_h_     = h.s > 0.0f ? h.h : m_->pick_h_;
                m_->pick_s_     = h.s;
                m_->pick_v_     = h.v;
                m_->pick_last_  = c;
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
    // being edited: one of its parts is held, or its hex field has the keyboard (the parts are keyed in this scope)
    const id   seed    = current_seed();
    const bool engaged = (m_->active_ != 0 && (m_->active_ == hash_id("##sv", seed) || m_->active_ == hash_id("##hue", seed) ||
                                           m_->active_ == hash_id("##alpha", seed))) ||
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

    bool open = m_->popup_id_ == key;
    if (in.pressed) {
        if (open) {
            m_->popup_id_ = 0;
            open      = false;
        } else {
            m_->popup_id_ = key;
            open      = true;
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
    track_edit(key, changed, m_->popup_id_ == key);
    return changed;
}

// trees and lists --------------------------------------------------------------------------

bool& context::tree_open_state(id key, bool default_open)
{
    auto it = std::lower_bound(m_->tree_states_.begin(), m_->tree_states_.end(), key,
                               [](const tree_state& s, id k) { return s.key < k; });
    if (it == m_->tree_states_.end() || it->key != key) {
        // a node that first appears while an open-all is running starts open, so expanding a whole tree does not
        // have to be driven one level per frame from the outside
        bool start_open = default_open;
        if (m_->tree_bulk_ != 0 && id_in_scope(m_->tree_bulk_seed_)) {
            start_open = m_->tree_bulk_ == 1;
        }
        it = m_->tree_states_.insert(it, tree_state{key, current_seed(), start_open});
    } else {
        it->seed = current_seed();
    }
    return it->open;
}

// is `seed` one of the id scopes the item being submitted sits in? 0 is the root scope: everything is in it
bool context::id_in_scope(id seed) const noexcept
{
    if (seed == 0) {
        return true;
    }
    for (u32 i = 0; i <= m_->id_depth_; ++i) {
        if (m_->id_stack_[i] == seed) { return true; }
    }
    return false;
}

// the id scope a node was submitted in, 0 when it has no state (and so no parent we know of)
// sets a node's open state without touching the scope it was submitted in (the keyboard acts on it from outside
// that scope, so tree_open_state would record the wrong parent)
void context::tree_open_set(id key, bool open) noexcept
{
    const auto it = std::lower_bound(m_->tree_states_.begin(), m_->tree_states_.end(), key,
                                     [](const tree_state& s, id k) { return s.key < k; });
    if (it != m_->tree_states_.end() && it->key == key) {
        it->open = open;
    }
}

id context::tree_seed_of(id key) const noexcept
{
    const auto it = std::lower_bound(m_->tree_states_.begin(), m_->tree_states_.end(), key,
                                     [](const tree_state& s, id k) { return s.key < k; });
    return it != m_->tree_states_.end() && it->key == key ? it->seed : id{0};
}

// does the chain of id scopes above `node` reach `root`? (`root` 0 is the whole ui)
bool context::tree_under(id node, id root) const noexcept
{
    if (root == 0) {
        return true;
    }
    id at = tree_seed_of(node);
    for (u32 i = 0; i < max_tree_depth && at != 0; ++i) {
        if (at == root) { return true; }
        at = tree_seed_of(at);
    }
    return false;
}

// open / close every node under `seed` -- the ones that exist right now by walking their scopes, and the ones that
// only appear as their parents open through the pending request, which lives long enough for a tree of the maximum
// depth to unfold
void context::tree_set_bulk(id seed, bool open) noexcept
{
    for (tree_state& st : m_->tree_states_) {
        if (st.key != seed && tree_under(st.key, seed)) { st.open = open; }
    }
    m_->tree_bulk_        = open ? u8{1} : u8{2};
    m_->tree_bulk_seed_   = seed;
    // opening unfolds one level per frame, so the request has to live long enough for the deepest tree; closing
    // hides everything below at once and needs no more than the frame it was asked on
    m_->tree_bulk_frames_ = open ? max_tree_depth : 1;
}

void context::tree_set_recursive(id key, bool open) noexcept
{
    tree_set_bulk(key, open);
}

// set_next_item_open() and a running bulk / recursive request, applied on top of the stored state
bool& context::tree_open_resolved(id key, bool default_open, bool& recursive_out) noexcept
{
    const u8 next = m_->next_open_;
    m_->next_open_    = 0;
    recursive_out = next >= 3;

    // nodes that already exist were set by tree_set_bulk's walk; the pending request only catches the ones that
    // appear later, as their parents open, which tree_open_state applies when it creates their state
    bool& open = tree_open_state(key, default_open);
    if (next != 0) {
        open = next == 1 || next == 3;
        if (recursive_out) { tree_set_recursive(key, open); }
    }
    return open;
}

// a full-width clickable row with hover / selected highlight; the caller draws the content
bool context::row_item(id key, std::string_view shown, bool selected, f32 text_indent, f32 row_height)
{
    const font_id f = current_font();
    const f32 gutter = std::exchange(m_->next_gutter_, 0.0f); // room set_next_item_gutter() reserved for accessories
    const rect row = layout_place({m_->layout_.width, row_height});
    // inside a table cell the highlight reaches over the cell padding so text lines up with its neighbours
    const rect hit = m_->table_.active ? rect{{row.min.x - m_->table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m_->table_.pad_x - 2.0f, row.max.y + 2.0f}}
                                   : row;
    // a row that is scrolled out of view costs nothing beyond the place it takes in the layout
    note_row_anchor(key, hit);
    if (item_culled(hit)) {
        note_culled_item(key, hit);
        nav_record(key, hit, m_->tree_depth_, false, false);
        const bool taken = nav_take(key);
        m_->item_pressed_    = taken;
        return taken;
    }
    const interaction in = interact(key, hit);
    const bool activated = in.pressed || nav_take(key);
    m_->item_pressed_ = activated;
    if (in.pressed) { nav_click(key); }
    nav_record(key, hit, m_->tree_depth_, false, false);

    anim_slot* a = anim_find(key);
    if (a == nullptr && in.hovered) { a = &anim_for(key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f);
        hover    = a->hover;
        a->last_frame = (hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
    }

    const bool focused = m_->last_item_focused_;
    if (selected || focused || hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * hover);
        bg.fill_bottom = bg.fill_top;
        if (focused) { // the keyboard cursor: an outline, so it reads on a selected or hovered row too
            bg.border       = m_->style_.accent_hover;
            bg.border_width = std::max(m_->style_.border_width, 1.0f);
        }
        m_->dl_.shape(hit, bg);
    }
    const f32   indent = m_->table_.active ? 0.0f : text_indent;
    const vec2  tsize  = label_size(f, shown);
    const vec2  at{row.min.x + indent, row.min.y + (row.height() - tsize.y) * 0.5f};
    const color col = selected ? m_->style_.accent_hover : m_->style_.text;
    // the label stops at the end of the row, minus whatever the row reserved for its accessories, instead of running
    // out from under it: item_truncated() then says whether it was cut
    label_clipped(at, std::max(row.max.x - gutter - at.x, 0.0f), col, shown, f);
    return activated;
}

bool context::selectable(std::string_view label, bool selected)
{
    return selectable(label, {}, selected);
}

bool context::selectable(std::string_view label, std::string_view id_extra, bool selected)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const std::string_view shown = visible_label(label);
    const f32 lh  = m_->rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f);
    const id  key = id_extra.empty() ? widget_id(label) : hash_id(id_extra, widget_id(label));
    return row_item(key, shown, selected, 8.0f, m_->table_.active ? lh : lh + 8.0f);
}

bool context::tree_leaf(std::string_view label, bool selected)
{
    return tree_leaf(label, {}, selected);
}

bool context::tree_leaf(std::string_view label, std::string_view id_extra, bool selected)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const std::string_view shown = visible_label(label);
    const f32 h   = (m_->rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f)) + 8.0f;
    const id  key = id_extra.empty() ? widget_id(label) : hash_id(id_extra, widget_id(label));
    return row_item(key, shown, selected, 22.0f, h);
}

bool context::tree_node(std::string_view label, tree_flags flags)
{
    return tree_node(label, {}, flags);
}

bool context::tree_node(std::string_view label, std::string_view id_extra, tree_flags flags)
{
    if (m_->cur_ != nullptr && m_->tree_depth_ >= max_tree_depth) {
        report_limit("tree_node nesting (max_tree_depth)", max_tree_depth);
    }
    if (m_->cur_ == nullptr || m_->tree_depth_ >= max_tree_depth) {
        return false;
    }
    const font_id f = current_font();
    const id key    = id_extra.empty() ? widget_id(label) : hash_id(id_extra, widget_id(label));
    const std::string_view shown = visible_label(label);
    const f32 h     = (m_->rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f)) + 8.0f;

    const f32 gutter = std::exchange(m_->next_gutter_, 0.0f);
    const rect row = layout_place({m_->layout_.width, h});
    bool  recursive = false;
    bool& open      = tree_open_resolved(key, has_flag(flags, tree_flags::default_open), recursive);

    constexpr f32 indent = 18.0f;
    // a node that is scrolled out of view still owns its open / closed state and its place in the layout; only the
    // hit test, the animation slot, the measuring and the geometry go away
    note_row_anchor(key, row);
    if (item_culled(row)) {
        note_culled_item(key, row);
        nav_record(key, row, m_->tree_depth_, true, open);
        (void)nav_take(key);
        if (!open) {
            return false;
        }
        m_->tree_stack_[m_->tree_depth_++] = {row.min.x + 10.0f, row.max.y, m_->layout_.origin.x, m_->layout_.width};
        m_->layout_.origin.x += indent;
        m_->layout_.width    -= indent;
        push_id_value(key);
        return true;
    }

    const interaction in = interact(key, row);

    const bool arrow_hit = m_->mouse_.x < row.min.x + h;
    const bool activated = in.pressed || nav_take(key);
    m_->item_pressed_    = activated;
    m_->last_item_arrow_ = in.pressed && arrow_hit;
    if (in.pressed) { nav_click(key); }
    if (activated && (!has_flag(flags, tree_flags::arrow_only) || arrow_hit)) {
        open = !open;
        // Ctrl or Shift held while it is clicked applies the change to the whole subtree
        if (in.pressed && (m_->mod_ctrl_ || m_->mod_shift_)) { tree_set_recursive(key, open); }
    }
    const bool is_open = open;
    nav_record(key, row, m_->tree_depth_, true, is_open);

    anim_slot* slot = anim_find(key);
    if (slot == nullptr) {
        slot         = &anim_for(key);
        slot->toggle = is_open ? 1.0f : 0.0f; // a node scrolling back into view keeps the arrow it had
    }
    anim_slot& a = *slot;
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, is_open ? 1.0f : 0.0f, m_->style_.anim_speed * 0.9f);

    const bool selected = has_flag(flags, tree_flags::selected);
    const bool focused  = m_->last_item_focused_;
    if (selected || focused || a.hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * a.hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * a.hover);
        bg.fill_bottom = bg.fill_top;
        if (focused) {
            bg.border       = m_->style_.accent_hover;
            bg.border_width = std::max(m_->style_.border_width, 1.0f);
        }
        m_->dl_.shape(row, bg);
    }

    // arrow that rotates from "right" to "down" while opening
    const vec2 c{row.min.x + 10.0f, row.center().y};
    const f32  ang = a.toggle * std::numbers::pi_v<f32> * 0.5f;
    const f32  cs = std::cos(ang);
    const f32  sn = std::sin(ang);
    const auto rot = [&](vec2 p) { return vec2{c.x + p.x * cs - p.y * sn, c.y + p.x * sn + p.y * cs}; };
    m_->dl_.triangle_filled(rot({-2.5f, -4.0f}), rot({4.0f, 0.0f}), rot({-2.5f, 4.0f}),
                        lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle)));

    const vec2  tsize = label_size(f, shown);
    const vec2  at{row.min.x + 22.0f, row.min.y + (row.height() - tsize.y) * 0.5f};
    const color col = selected ? m_->style_.accent_hover : m_->style_.text;
    label_clipped(at, std::max(row.max.x - gutter - at.x, 0.0f), col, shown, f);

    if (!is_open) {
        return false;
    }

    m_->tree_stack_[m_->tree_depth_++] = {c.x, row.max.y, m_->layout_.origin.x, m_->layout_.width};
    m_->layout_.origin.x += indent;
    m_->layout_.width    -= indent;
    push_id_value(key);
    return true;
}

void context::tree_pop()
{
    if (m_->tree_depth_ == 0) {
        return;
    }
    const tree_frame fr = m_->tree_stack_[--m_->tree_depth_];
    pop_id();

    const f32 y0 = fr.children_top + 1.0f;
    const f32 y1 = m_->layout_.bottom - 2.0f;
    if (y1 > y0) {
        m_->dl_.rect_filled({{std::round(fr.arrow_x), y0}, {std::round(fr.arrow_x) + 1.0f, y1}}, m_->style_.border.scaled_alpha(0.8f));
    }
    m_->layout_.origin.x = fr.saved_origin_x;
    m_->layout_.width    = fr.saved_width;
}

void context::text_ellipsis(std::string_view s)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    const f32 avail = m_->layout_.width;
    if (m_->rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
        const vec2 size = label_size(f, s);
        const rect r    = layout_place({std::min(size.x, avail), size.y});
        m_->dl_.push_clip(r);
        label_draw(r.min, m_->style_.text, s, f);
        m_->dl_.pop_clip();
        return;
    }
    if (measure_cached(f, s).x <= avail) {
        text(s);
        return;
    }
    m_->last_item_truncated_ = true; // the caller can add a tooltip with the whole string

    constexpr std::string_view dots = "...";
    const f32 budget = avail - m_->font_.measure(f, dots).x;
    std::string cut;
    f32 used = 0.0f;
    std::string_view rest = s;
    while (!rest.empty()) {
        const std::string_view before = rest;
        const char32_t cp = decode_utf8(rest);
        const f32 adv = m_->font_.advance(f, cp);
        if (used + adv > budget) { break; }
        used += adv;
        cut.append(before.substr(0, before.size() - rest.size()));
    }
    cut += dots;
    text(cut);
    m_->last_item_truncated_ = true;
}

// tables ---------------------------------------------------------------------------------------

context::table_state* context::table_for(id key) noexcept
{
    table_state* spare = nullptr;
    for (table_state& t : m_->tables_) {
        if (t.key == key) {
            return &t;
        }
        if (spare == nullptr && (t.key == 0 || t.last_frame + 2 < m_->frame_)) {
            spare = &t;
        }
    }
    table_state* slot = spare != nullptr ? spare : &m_->tables_[0];
    *slot     = {};
    slot->key = key;
    return slot;
}

bool context::begin_table(std::string_view id_label, u32 columns, table_flags flags, f32 height)
{
    // a table inside a cell of another one is fine: the outer frame is parked on a stack until end_table().
    // tables repeated in rows need distinct ids (push_id(row)) to keep separate column widths and scroll positions
    if (m_->cur_ != nullptr && columns > max_table_columns) {
        report_limit("table columns (max_table_columns)", max_table_columns);
    }
    if (m_->cur_ != nullptr && m_->table_.active && m_->table_depth_ >= max_table_depth) {
        report_limit("tables inside tables (max_table_depth)", max_table_depth);
    }
    if (m_->cur_ == nullptr || columns == 0 || columns > max_table_columns || (m_->table_.active && m_->table_depth_ >= max_table_depth)) {
        return false;
    }
    const id key = widget_id(id_label);

    if (m_->table_.active) {
        m_->table_stack_[m_->table_depth_++] = m_->table_;
    }
    m_->table_ = {};
    m_->table_.active       = true;
    m_->table_.state        = table_for(key);
    m_->table_.flags        = flags;
    m_->table_.ncols        = columns;
    m_->table_.height_limit = height;

    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    const rect start = layout_place({w, 0.0f});
    m_->table_.origin = start.min;
    m_->table_.width  = std::max(w, 1.0f);
    m_->table_.outer  = m_->layout_;

    m_->table_.pad_x     = 8.0f;
    m_->table_.pad_y     = 3.0f;
    m_->table_.min_row_h = m_->font_.line_height(current_font()) + 2.0f * m_->table_.pad_y;
    m_->table_.row_y     = m_->table_.origin.y;

    table_state& st = *m_->table_.state;
    st.last_frame = m_->frame_;
    if (st.columns != columns) {
        st.columns   = columns;
        st.inited    = false;
        st.row_hint  = m_->table_.min_row_h;
        st.scroll    = 0.0f;
        st.content_h = 0.0f;
    }
    push_id(id_label);
    return true;
}

void context::table_setup_column(std::string_view label, f32 fixed_width, f32 stretch_weight, table_column_flags flags)
{
    if (!m_->table_.active || m_->table_.setup_count >= m_->table_.ncols) {
        return;
    }
    m_->table_.cols[m_->table_.setup_count++] = {label, fixed_width, stretch_weight, flags};
}

// where every column is: hidden ones take no room, the visible ones share the width by their fractions
void context::table_recompute_x() noexcept
{
    const table_state& st = *m_->table_.state;
    f32 total = 0.0f;
    for (u32 c = 0; c < m_->table_.ncols; ++c) {
        if ((st.hidden & (1u << c)) == 0) { total += st.frac[c]; }
    }
    if (total <= 0.0f) { total = 1.0f; }
    m_->table_.frac_total = total;

    f32 x = m_->table_.origin.x;
    u32 nv = 0;
    for (u32 p = 0; p < m_->table_.ncols; ++p) {
        const u32 c = st.order[p];
        if ((st.hidden & (1u << c)) != 0) {
            m_->table_.x0[c] = m_->table_.x1[c] = x;
            continue;
        }
        m_->table_.col_x[nv] = x;
        m_->table_.vis[nv]   = static_cast<u8>(c);
        m_->table_.x0[c]     = x;
        x += st.frac[c] / total * m_->table_.width;
        m_->table_.x1[c]     = x;
        ++nv;
    }
    m_->table_.col_x[nv] = m_->table_.origin.x + m_->table_.width;
    m_->table_.nvis      = nv;
}

namespace {

// "order=2,0,1;hidden=1;widths=0.3,0.3,0.4" (column indexes and the widths as fractions); false if it does not fit `ncols`
[[nodiscard]] bool parse_table_layout(std::string_view text, u32 ncols, std::array<u8, 16>& order, u16& hidden, std::array<f32, 16>& frac)
{
    std::array<u8, 16>  o{};
    std::array<f32, 16> w{};
    u16 h = 0;
    bool have_order = false, have_widths = false;
    while (!text.empty()) {
        const std::size_t semi = text.find(';');
        std::string_view part = text.substr(0, semi);
        text = semi == std::string_view::npos ? std::string_view{} : text.substr(semi + 1);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) { continue; }
        const std::string_view key = part.substr(0, eq);
        std::string_view       val = part.substr(eq + 1);
        u32 n = 0;
        while (!val.empty()) {
            const std::size_t comma = val.find(',');
            const std::string_view item = val.substr(0, comma);
            val = comma == std::string_view::npos ? std::string_view{} : val.substr(comma + 1);
            if (key == "widths") {
                f32 v{};
                if (n >= ncols || std::from_chars(item.data(), item.data() + item.size(), v).ec != std::errc{}) { return false; }
                w[n++] = v;
            } else {
                u32 v{};
                if (item.empty()) { continue; }
                if (std::from_chars(item.data(), item.data() + item.size(), v).ec != std::errc{} || v >= ncols) { return false; }
                if (key == "order") {
                    if (n >= ncols) { return false; }
                    o[n++] = static_cast<u8>(v);
                } else if (key == "hidden") {
                    h = static_cast<u16>(h | (1u << v));
                }
            }
        }
        if (key == "order")  { if (n != ncols) { return false; } have_order = true; }
        if (key == "widths") { if (n != ncols) { return false; } have_widths = true; }
    }
    if (have_order) { // a permutation
        u32 seen = 0;
        for (u32 i = 0; i < ncols; ++i) { seen |= 1u << o[i]; }
        if (seen != (1u << ncols) - 1u) { return false; }
        order = o;
    }
    if (have_widths) {
        f32 sum = 0.0f;
        for (u32 i = 0; i < ncols; ++i) { sum += w[i]; }
        if (sum <= 0.0f) { return false; }
        for (u32 i = 0; i < ncols; ++i) { frac[i] = w[i] / sum; }
    }
    hidden = h;
    return true;
}

} // namespace

void context::table_finalize_columns()
{
    if (m_->table_.columns_ready) {
        return;
    }
    m_->table_.columns_ready = true;
    table_state& st = *m_->table_.state;

    if (!st.inited) {
        f32 fixed   = 0.0f;
        f32 weights = 0.0f;
        for (u32 i = 0; i < m_->table_.ncols; ++i) {
            if (m_->table_.cols[i].fixed > 0.0f) { fixed += m_->table_.cols[i].fixed; }
            else                             { weights += std::max(m_->table_.cols[i].weight, 0.0001f); }
        }
        const f32 rest = std::max(m_->table_.width - fixed, 0.0f);
        f32 sum = 0.0f;
        for (u32 i = 0; i < m_->table_.ncols; ++i) {
            const f32 px = m_->table_.cols[i].fixed > 0.0f
                               ? m_->table_.cols[i].fixed
                               : (weights > 0.0f ? rest * std::max(m_->table_.cols[i].weight, 0.0001f) / weights : 0.0f);
            st.frac[i] = px / m_->table_.width;
            sum += st.frac[i];
        }
        for (u32 i = 0; i < m_->table_.ncols && sum > 0.0f; ++i) { st.frac[i] /= sum; }
        st.hidden = 0;
        for (u32 i = 0; i < m_->table_.ncols; ++i) {
            st.order[i] = static_cast<u8>(i);
            if ((static_cast<u8>(m_->table_.cols[i].flags) & static_cast<u8>(table_column_flags::default_hidden)) != 0) {
                st.hidden = static_cast<u16>(st.hidden | (1u << i));
            }
        }
        st.inited = true;
    }
    // a layout loaded with table_load_layout(): once
    for (auto it = m_->table_pending_.begin(); it != m_->table_pending_.end(); ++it) {
        if (it->first != st.key) { continue; }
        std::array<u8, 16>  order = st.order;
        std::array<f32, 16> frac  = st.frac;
        u16                 hidden = st.hidden;
        if (parse_table_layout(it->second, m_->table_.ncols, order, hidden, frac)) {
            st.order  = order;
            st.frac   = frac;
            st.hidden = hidden;
        }
        m_->table_pending_.erase(it);
        break;
    }
    const u16 all = static_cast<u16>((1u << m_->table_.ncols) - 1u);
    if ((st.hidden & all) == all) { st.hidden = 0; } // at least one column shows
    table_recompute_x();
}

int context::table_headers_row(int sort_column, bool ascending)
{
    if (!m_->table_.active || m_->table_.header_done || m_->table_.body_started) {
        return -1;
    }
    table_finalize_columns();
    table_state& st = *m_->table_.state;
    const font_id f = current_font();
    const f32 lh    = m_->font_.line_height(f);
    const f32 hh    = lh + 2.0f * m_->table_.pad_y + 2.0f;
    const f32 y0    = m_->table_.row_y;
    const rect header_area = {{m_->table_.origin.x, y0}, {m_->table_.origin.x + m_->table_.width, y0 + hh}};
    const auto column_flag = [&](u32 c, table_column_flags fl) { return (static_cast<u8>(m_->table_.cols[c].flags) & static_cast<u8>(fl)) != 0; };

    // right-click: a menu of the columns to show
    if (has_flag(m_->table_.flags, table_flags::hideable)) {
        if (m_->mouse_right_pressed_ && pointer_over(header_area)) {
            open_popup_menu("##columns", m_->mouse_);
        }
        if (begin_popup_menu("##columns")) {
            for (u32 p = 0; p < m_->table_.ncols; ++p) {
                const u32 c = st.order[p];
                bool shown = (st.hidden & (1u << c)) == 0;
                const std::string_view name = m_->table_.cols[c].label.empty() ? std::string_view{"(unnamed)"} : visible_label(m_->table_.cols[c].label);
                menu_item_options o;
                o.keep_open = true;
                o.enabled   = !(shown && (m_->table_.nvis <= 1 || column_flag(c, table_column_flags::no_hide)));
                push_id(std::to_string(c));
                if (menu_item(name, shown, o)) {
                    st.hidden = static_cast<u16>(shown ? (st.hidden & ~(1u << c)) : (st.hidden | (1u << c)));
                    table_recompute_x();
                }
                pop_id();
            }
            end_popup_menu();
        }
    }

    // resize handles first: they win the mouse over the header cells beneath them
    if (has_flag(m_->table_.flags, table_flags::resizable)) {
        for (u32 k = 0; k + 1 < m_->table_.nvis; ++k) {
            const u32 a = m_->table_.vis[k];
            const u32 b = m_->table_.vis[k + 1];
            const rect grip = {{m_->table_.col_x[k + 1] - 4.0f, y0}, {m_->table_.col_x[k + 1] + 4.0f, y0 + hh}};
            const interaction in = interact(hash_id("##grip", hash_id({reinterpret_cast<const char*>(&a), sizeof(a)}, current_seed())), grip);
            if (in.held && m_->mouse_delta_.x != 0.0f) {
                const f32 per_px   = m_->table_.frac_total / m_->table_.width; // what a pixel is worth in `frac` units
                const f32 min_frac = 36.0f * per_px;
                const f32 dx = std::clamp(m_->mouse_delta_.x * per_px, min_frac - st.frac[a], st.frac[b] - min_frac);
                st.frac[a] += dx;
                st.frac[b] -= dx;
                table_recompute_x();
            }
            if (in.hovered || in.held) {
                m_->dl_.rect_filled({{m_->table_.col_x[k + 1] - 1.0f, y0 + 3.0f}, {m_->table_.col_x[k + 1] + 1.0f, y0 + hh - 3.0f}},
                                m_->style_.accent.scaled_alpha(in.held ? 0.9f : 0.6f));
            }
        }
    }

    // the header cells: press = sort, drag = move the column
    struct header_hit { id key{}; bool pressed{}; };
    std::array<header_hit, 16> hits{};
    const u8 was_dragging = st.drag_col1;
    if (!m_->mouse_down_) { st.drag_col1 = 0; st.press_col1 = 0; }
    for (u32 k = 0; k < m_->table_.nvis; ++k) {
        const u32  c    = m_->table_.vis[k];
        const rect cell = {{m_->table_.col_x[k], y0}, {m_->table_.col_x[k + 1], y0 + hh}};
        const id   key  = hash_id(m_->table_.cols[c].label, hash_id({reinterpret_cast<const char*>(&c), sizeof(c)}, current_seed()));
        const interaction in = interact(key, {{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}});
        hits[c] = {key, in.pressed && was_dragging != c + 1};
        anim_slot* a = anim_find(key);
        if (a == nullptr && in.hovered) { a = &anim_for(key); }
        if (a != nullptr) {
            a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f);
            a->last_frame = (a->hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
        }
        if (has_flag(m_->table_.flags, table_flags::reorderable) && !column_flag(c, table_column_flags::no_reorder)) {
            if (m_->active_ == key && m_->mouse_pressed_) {
                st.press_col1 = static_cast<u8>(c + 1);
                st.press_x    = m_->mouse_.x;
            }
            if (st.drag_col1 == 0 && st.press_col1 == c + 1 && m_->active_ == key && m_->mouse_down_ && std::abs(m_->mouse_.x - st.press_x) > 5.0f) {
                st.drag_col1 = static_cast<u8>(c + 1);
            }
        }
    }
    if (st.drag_col1 != 0 && m_->mouse_down_) { // the dragged column takes the place of the one its middle has passed
        const u32 c = st.drag_col1 - 1u;
        u32 slot = 0, target = 0;
        for (u32 k = 0; k < m_->table_.nvis; ++k) {
            if (m_->table_.vis[k] == c) { slot = k; }
            else if ((m_->table_.col_x[k] + m_->table_.col_x[k + 1]) * 0.5f < m_->mouse_.x) { ++target; }
        }
        if (target != slot) {
            const u32 d = m_->table_.vis[target];
            u32 from = 0, to = 0;
            for (u32 p = 0; p < m_->table_.ncols; ++p) {
                if (st.order[p] == c) { from = p; }
                if (st.order[p] == d) { to = p; }
            }
            bool blocked = false; // a pinned column in between cannot be passed
            for (u32 p = std::min(from, to); p <= std::max(from, to); ++p) {
                if (p != from && column_flag(st.order[p], table_column_flags::no_reorder)) { blocked = true; }
            }
            if (!blocked) {
                const u8 moved = st.order[from];
                if (from < to) { for (u32 p = from; p < to; ++p) { st.order[p] = st.order[p + 1]; } }
                else           { for (u32 p = from; p > to; --p) { st.order[p] = st.order[p - 1]; } }
                st.order[to] = moved;
                table_recompute_x();
            }
        }
    }

    shape_style bg;
    bg.radius      = radii(m_->style_.rounding * 0.6f, corners::top);
    bg.fill_top    = lighten(m_->style_.title_bg, m_->style_.gradient * 0.8f);
    bg.fill_bottom = m_->style_.title_bg;
    m_->dl_.shape({{m_->table_.origin.x, y0}, {m_->table_.origin.x + m_->table_.width, y0 + hh}}, bg);

    int clicked = -1;
    for (u32 k = 0; k < m_->table_.nvis; ++k) {
        const u32  c    = m_->table_.vis[k];
        const rect cell = {{m_->table_.col_x[k], y0}, {m_->table_.col_x[k + 1], y0 + hh}};
        if (hits[c].pressed) { clicked = static_cast<int>(c); }

        const anim_slot* a = anim_find(hits[c].key);
        const f32 hover = a != nullptr ? a->hover : 0.0f;
        if (hover > 0.01f) {
            m_->dl_.rect_filled(cell, m_->style_.widget_hover.scaled_alpha(0.5f * hover));
        }
        if (st.drag_col1 == c + 1 && m_->mouse_down_) {
            m_->dl_.rect_filled(cell, m_->style_.accent.scaled_alpha(0.22f));
        }

        const std::string_view label = visible_label(m_->table_.cols[c].label);
        const bool sorted = static_cast<int>(c) == sort_column;
        const f32 text_max = cell.width() - 2.0f * m_->table_.pad_x - (sorted ? 14.0f : 0.0f);
        if (m_->rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
            const vec2 ts = label_size(f, label);
            m_->dl_.push_clip({{cell.min.x + m_->table_.pad_x, cell.min.y}, {cell.min.x + m_->table_.pad_x + std::max(text_max, 0.0f), cell.max.y}});
            label_draw({cell.min.x + m_->table_.pad_x, cell.min.y + (hh - ts.y) * 0.5f}, m_->style_.text, label, f);
            m_->dl_.pop_clip();
        } else {
            std::string cut;
            std::string_view shown = label;
            if (m_->font_.measure(f, label).x > text_max) {
                std::string_view rest = label;
                f32 used = 0.0f;
                while (!rest.empty()) {
                    const std::string_view before = rest;
                    const char32_t cp = decode_utf8(rest);
                    const f32 adv = m_->font_.advance(f, cp);
                    if (used + adv > text_max - 10.0f) { break; }
                    used += adv;
                    cut.append(before.substr(0, before.size() - rest.size()));
                }
                cut += "..";
                shown = cut;
            }
            m_->dl_.text({cell.min.x + m_->table_.pad_x, cell.min.y + (hh - lh) * 0.5f}, m_->style_.text, shown, f);
        }

        if (sorted) {
            const vec2 sc{cell.max.x - m_->table_.pad_x - 3.0f, cell.center().y};
            if (ascending) {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y + 2.5f}, {sc.x, sc.y - 3.0f}, {sc.x + 4.0f, sc.y + 2.5f}, m_->style_.accent_hover);
            } else {
                m_->dl_.triangle_filled({sc.x - 4.0f, sc.y - 2.5f}, {sc.x + 4.0f, sc.y - 2.5f}, {sc.x, sc.y + 3.0f}, m_->style_.accent_hover);
            }
        }
    }

    m_->dl_.rect_filled({{m_->table_.origin.x, y0 + hh - 1.0f}, {m_->table_.origin.x + m_->table_.width, y0 + hh}}, m_->style_.border);
    m_->table_.row_y       = y0 + hh;
    m_->table_.header_done = true;
    return clicked;
}

void context::table_start_body()
{
    if (m_->table_.body_started) {
        return;
    }
    m_->table_.body_started = true;
    m_->table_.body_top     = m_->table_.row_y;

    if (m_->table_.height_limit > 0.0f) {
        table_state& st = *m_->table_.state;
        m_->table_.scroll_mode = true;
        m_->table_.body_h = std::max(m_->table_.origin.y + m_->table_.height_limit - m_->table_.body_top, m_->table_.min_row_h);
        const rect region = {{m_->table_.origin.x, m_->table_.body_top}, {m_->table_.origin.x + m_->table_.width, m_->table_.body_top + m_->table_.body_h}};

        st.scroll = std::clamp(st.scroll, 0.0f, std::max(0.0f, st.content_h - m_->table_.body_h));

        m_->dl_.push_clip(region);
        m_->table_.clip_pushed = true;
        m_->table_.row_y       = m_->table_.body_top - st.scroll;
    }
}

void context::table_finish_row()
{
    table_state& st = *m_->table_.state;
    if (m_->table_.cell_clip) { // cell content ends here; the row lines below are not clipped to the cell
        m_->dl_.pop_clip();
        m_->table_.cell_clip = false;
    }
    if (m_->table_.col >= 0 && !m_->layout_.first) {
        m_->table_.row_bottom    = std::max(m_->table_.row_bottom, m_->layout_.bottom + m_->table_.pad_y);
        m_->table_.row_had_content = true;
    }

    const f32 height = m_->table_.row_had_content ? std::max(m_->table_.row_bottom - m_->table_.row_top, m_->table_.min_row_h)
                                              : std::max(st.row_hint, m_->table_.min_row_h);
    if (m_->table_.row_visible && has_flag(m_->table_.flags, table_flags::borders)) {
        m_->dl_.rect_filled({{m_->table_.origin.x, m_->table_.row_top + height - 1.0f}, {m_->table_.origin.x + m_->table_.width, m_->table_.row_top + height}},
                        m_->style_.border.scaled_alpha(0.45f));
    }
    if (m_->table_.row_had_content) {
        st.row_hint = height;
    }
    m_->table_.row_y  = m_->table_.row_top + height;
    m_->table_.in_row = false;
    m_->table_.col    = -1;
}

bool context::table_next_row()
{
    if (!m_->table_.active) {
        return false;
    }
    table_finalize_columns();
    if (m_->table_.in_row) {
        table_finish_row();
    }
    table_start_body();

    table_state& st = *m_->table_.state;
    m_->table_.in_row          = true;
    m_->table_.row_top         = m_->table_.row_y;
    m_->table_.row_bottom      = m_->table_.row_top;
    m_->table_.row_had_content = false;
    m_->table_.col             = -1;
    ++m_->table_.row_index;

    // background is drawn now using the previous row's height, before the content goes on top
    const f32  hint = std::max(st.row_hint, m_->table_.min_row_h);
    const rect row  = {{m_->table_.origin.x, m_->table_.row_top}, {m_->table_.origin.x + m_->table_.width, m_->table_.row_top + hint}};
    m_->table_.row_visible = m_->dl_.clip().overlaps(row);
    if (m_->table_.row_visible) {
        if (has_flag(m_->table_.flags, table_flags::striped) && (m_->table_.row_index & 1) == 0) {
            m_->dl_.rect_filled(row, m_->style_.widget_bg.scaled_alpha(0.3f));
        }
        if (has_flag(m_->table_.flags, table_flags::row_hover) && pointer_over(row)) {
            m_->dl_.rect_filled(row, m_->style_.accent.scaled_alpha(0.11f));
        }
    }
    return m_->table_.row_visible;
}

bool context::table_next_column()
{
    if (!m_->table_.active) {
        return false;
    }
    if (!m_->table_.in_row) {
        (void)table_next_row();
    }
    if (m_->table_.cell_clip) {
        m_->dl_.pop_clip();
        m_->table_.cell_clip = false;
    }
    if (m_->table_.col >= 0 && !m_->layout_.first) {
        m_->table_.row_bottom      = std::max(m_->table_.row_bottom, m_->layout_.bottom + m_->table_.pad_y);
        m_->table_.row_had_content = true;
    }
    if (m_->table_.col + 1 >= static_cast<int>(m_->table_.ncols)) {
        (void)table_next_row(); // running past the last column wraps to the next row
    }

    ++m_->table_.col;
    const auto c = static_cast<u32>(m_->table_.col);
    m_->layout_        = {};
    m_->layout_.origin = {m_->table_.x0[c] + m_->table_.pad_x, m_->table_.row_top + m_->table_.pad_y};
    m_->layout_.width  = std::max(m_->table_.x1[c] - m_->table_.x0[c] - 2.0f * m_->table_.pad_x, 1.0f);

    // cell content never spills into the neighbouring column (a hidden column has no width: nothing shows)
    const rect outer_clip = m_->dl_.clip();
    m_->dl_.push_clip({{m_->table_.x0[c], outer_clip.min.y}, {m_->table_.x1[c], outer_clip.max.y}});
    m_->table_.cell_clip = true;
    return m_->table_.x1[c] > m_->table_.x0[c];
}

void context::end_table()
{
    if (!m_->table_.active) {
        return;
    }
    table_finalize_columns();
    if (m_->table_.in_row) {
        table_finish_row();
    }
    table_start_body();
    table_state& st = *m_->table_.state;

    f32 total_h;
    if (m_->table_.scroll_mode) {
        st.content_h = m_->table_.row_y - (m_->table_.body_top - st.scroll);
        if (m_->table_.clip_pushed) {
            m_->dl_.pop_clip();
        }
        total_h = m_->table_.body_top - m_->table_.origin.y + m_->table_.body_h;

        // the wheel goes to the innermost scroller under the pointer: tables nested in this one finished before it
        const rect region = {{m_->table_.origin.x, m_->table_.body_top}, {m_->table_.origin.x + m_->table_.width, m_->table_.body_top + m_->table_.body_h}};
        if (m_->wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(region)) {
            st.scroll -= m_->wheel_ * st.row_hint * 3.0f;
            m_->wheel_consumed_ = true;
        }

        const f32 max_scroll = std::max(0.0f, st.content_h - m_->table_.body_h);
        st.scroll = std::clamp(st.scroll, 0.0f, max_scroll);
        if (max_scroll > 0.0f) {
            const f32  track_top = m_->table_.body_top + 2.0f;
            const f32  track_h = m_->table_.body_h - 4.0f;
            const f32  thumb_h = std::max(20.0f, track_h * m_->table_.body_h / st.content_h);
            const f32  x1      = m_->table_.origin.x + m_->table_.width - 3.0f;
            f32 thumb_y = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const interaction in = interact(widget_id("##tscroll"), rect{{x1 - 7.0f, thumb_y}, {x1 + 2.0f, thumb_y + thumb_h}});
            st.scroll = thumb_drag(in, st.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, st.scroll);
            thumb_y   = track_top + (track_h - thumb_h) * (st.scroll / max_scroll);
            const rect thumb = {{x1 - 5.0f, thumb_y}, {x1, thumb_y + thumb_h}};
            shape_style bar;
            bar.radius      = radii(2.5f);
            bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.8f : 0.45f);
            bar.fill_bottom = bar.fill_top;
            m_->dl_.shape(thumb, bar);
        }
    } else {
        total_h = m_->table_.row_y - m_->table_.origin.y;
    }

    const rect bounds = {m_->table_.origin, {m_->table_.origin.x + m_->table_.width, m_->table_.origin.y + total_h}};
    if (has_flag(m_->table_.flags, table_flags::borders)) {
        for (u32 i = 1; i < m_->table_.nvis; ++i) {
            m_->dl_.rect_filled({{std::round(m_->table_.col_x[i]), bounds.min.y}, {std::round(m_->table_.col_x[i]) + 1.0f, bounds.max.y}},
                            m_->style_.border.scaled_alpha(0.55f));
        }
        shape_style edge;
        edge.radius       = radii(m_->style_.rounding * 0.6f);
        edge.border       = m_->style_.border;
        edge.border_width = m_->style_.border_width;
        m_->dl_.shape(bounds, edge);
    }

    m_->layout_            = m_->table_.outer;
    m_->layout_.line_h     = total_h;
    m_->layout_.bottom     = std::max(m_->layout_.bottom, m_->table_.origin.y + total_h);
    m_->layout_.same_line  = false;
    m_->layout_.first      = false;
    pop_id();
    m_->table_ = {};
    if (m_->table_depth_ > 0) { // back to the table this one was nested in
        m_->table_ = m_->table_stack_[--m_->table_depth_];
    }
}


void context::table_skip_rows(int count)
{
    if (!m_->table_.active || count <= 0) {
        return;
    }
    table_finalize_columns();
    if (m_->table_.in_row) {
        table_finish_row();
    }
    table_start_body();
    m_->table_.row_y     += static_cast<f32>(count) * std::max(m_->table_.state->row_hint, m_->table_.min_row_h);
    m_->table_.row_index += static_cast<u32>(count);
}

std::string context::table_save_layout(std::string_view id_label) const
{
    const id key = hash_id(id_label, current_seed()); // a query, not a submission: nothing to record
    for (const table_state& t : m_->tables_) {
        if (t.key != key || !t.inited) { continue; }
        return table_layout_text(t);
    }
    return {};
}

std::string context::table_layout_text(const table_state& t)
{
    std::string out = "order=";
    for (u32 p = 0; p < t.columns; ++p) { out += (p ? "," : "") + std::to_string(t.order[p]); }
    out += ";hidden=";
    bool first = true;
    for (u32 c = 0; c < t.columns; ++c) {
        if ((t.hidden & (1u << c)) != 0) { out += (first ? "" : ",") + std::to_string(c); first = false; }
    }
    out += ";widths=";
    for (u32 c = 0; c < t.columns; ++c) { out += (c ? "," : "") + std::format("{:.4f}", t.frac[c]); }
    return out;
}

void context::table_load_layout(std::string_view id_label, std::string_view text)
{
    const id key = widget_id(id_label);
    for (auto& p : m_->table_pending_) {
        if (p.first == key) {
            p.second.assign(text);
            return;
        }
    }
    m_->table_pending_.emplace_back(key, std::string{text});
}

// tree tables --------------------------------------------------------------------------------

bool context::table_tree_node(std::string_view label, tree_flags flags)
{
    if (m_->cur_ == nullptr || !m_->table_.active || m_->table_.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(m_->table_.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place({m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - m_->table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m_->table_.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(key, hit);

    bool& open = tree_open_state(key, has_flag(flags, tree_flags::default_open));
    const bool arrow_hit = m_->mouse_.x < row.min.x + 16.0f;
    m_->item_pressed_ = in.pressed;
    if (in.pressed && (!has_flag(flags, tree_flags::arrow_only) || arrow_hit)) {
        open = !open;
    }
    const bool is_open = open;

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, is_open ? 1.0f : 0.0f, m_->style_.anim_speed * 0.9f);

    const bool selected = has_flag(flags, tree_flags::selected);
    if (selected || a.hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * a.hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * a.hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape(hit, bg);
    }

    const vec2 c{row.min.x + 8.0f, row.center().y};
    const f32  ang = a.toggle * std::numbers::pi_v<f32> * 0.5f;
    const f32  cs = std::cos(ang);
    const f32  sn = std::sin(ang);
    const auto rot = [&](vec2 p) { return vec2{c.x + p.x * cs - p.y * sn, c.y + p.x * sn + p.y * cs}; };
    m_->dl_.triangle_filled(rot({-2.5f, -4.0f}), rot({4.0f, 0.0f}), rot({-2.5f, 4.0f}),
                        lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle)));
    const vec2 tsize = label_size(f, shown);
    label_draw({row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f},
               selected ? m_->style_.accent_hover : m_->style_.text, shown, f);

    if (!is_open) {
        return false;
    }
    ++m_->table_.tree_depth;
    push_id(label);
    return true;
}

bool context::table_tree_leaf(std::string_view label, bool selected)
{
    if (m_->cur_ == nullptr || !m_->table_.active || m_->table_.col < 0) {
        return false;
    }
    const font_id f = current_font();
    const id key    = widget_id(label);
    const std::string_view shown = visible_label(label);
    const f32 h     = m_->rich_depth_ > 0 ? label_size(f, shown).y : m_->font_.line_height(f);

    const f32 indent = 18.0f * static_cast<f32>(m_->table_.tree_depth);
    m_->layout_.origin.x += indent;
    m_->layout_.width     = std::max(m_->layout_.width - indent, 1.0f);
    const rect row = layout_place({m_->layout_.width, h});
    m_->layout_.origin.x -= indent;
    m_->layout_.width    += indent;
    const rect hit = {{row.min.x - indent - m_->table_.pad_x + 2.0f, row.min.y - 2.0f}, {row.max.x + m_->table_.pad_x - 2.0f, row.max.y + 2.0f}};
    const interaction in = interact(key, hit);
    m_->item_pressed_ = in.pressed;

    anim_slot* a = anim_find(key);
    if (a == nullptr && in.hovered) { a = &anim_for(key); }
    f32 hover = 0.0f;
    if (a != nullptr) {
        a->hover = approach(a->hover, in.hovered ? 1.0f : 0.0f);
        hover    = a->hover;
        a->last_frame = (hover == 0.0f && !in.hovered) ? 0 : m_->frame_;
    }
    if (selected || hover > 0.01f) {
        shape_style bg;
        bg.radius      = radii(m_->style_.rounding * 0.55f);
        bg.fill_top    = selected ? m_->style_.accent.scaled_alpha(0.28f + 0.1f * hover)
                                  : m_->style_.widget_hover.scaled_alpha(0.75f * hover);
        bg.fill_bottom = bg.fill_top;
        m_->dl_.shape(hit, bg);
    }
    const vec2 tsize = label_size(f, shown);
    label_draw({row.min.x + 20.0f, row.min.y + (row.height() - tsize.y) * 0.5f}, selected ? m_->style_.accent_hover : m_->style_.text, shown, f);
    return in.pressed;
}

void context::table_tree_pop()
{
    if (!m_->table_.active || m_->table_.tree_depth == 0) {
        return;
    }
    --m_->table_.tree_depth;
    pop_id();
}

// long lists ---------------------------------------------------------------------------------

void context::list_clip_begin(int count, f32 item_height, int& first, int& last)
{
    first = last = 0;
    if (m_->cur_ == nullptr || count <= 0) {
        return;
    }
    f32 pitch;
    f32 y0;
    if (m_->table_.active) {
        table_finalize_columns();
        if (m_->table_.in_row) { table_finish_row(); }
        table_start_body();
        pitch = std::max(m_->table_.state->row_hint, m_->table_.min_row_h);
        y0    = m_->table_.row_y;
    } else {
        pitch = (item_height > 0.0f ? item_height : m_->font_.line_height(current_font())) + m_->style_.item_spacing;
        y0    = layout_next_y();
    }
    const rect view = m_->dl_.clip();
    first = std::clamp(static_cast<int>(std::floor((view.min.y - y0) / pitch)) - 1, 0, count);
    last  = std::clamp(static_cast<int>(std::ceil((view.max.y - y0) / pitch)) + 1, first, count);
    if (first > 0) {
        if (m_->table_.active) { table_skip_rows(first); }
        else               { (void)layout_place({0.0f, static_cast<f32>(first) * pitch - m_->style_.item_spacing}); }
    }
}

void context::list_clip_end(int count, f32 item_height, int last)
{
    if (m_->cur_ == nullptr || last >= count) {
        return;
    }
    const int rest = count - last;
    if (m_->table_.active) {
        table_skip_rows(rest);
    } else {
        const f32 pitch = (item_height > 0.0f ? item_height : m_->font_.line_height(current_font())) + m_->style_.item_spacing;
        (void)layout_place({0.0f, static_cast<f32>(rest) * pitch - m_->style_.item_spacing});
    }
}

} // namespace strata

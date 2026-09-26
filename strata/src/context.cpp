#include "strata/context.hpp"

#include "strata/bidi.hpp"
#include "dock_state.hpp"
#include "limits.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <utility>
#include <vector>

namespace strata {

using namespace text;

namespace {

[[nodiscard]] std::string_view format_fixed(std::span<char> buf, f32 value, int decimals) noexcept
{
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed, decimals);
    return {buf.data(), r.ptr};
}

[[nodiscard]] constexpr color lighten(color c, f32 k) noexcept { return lerp(c, color{255, 255, 255, c.a}, k); }
[[nodiscard]] constexpr color darken(color c, f32 k) noexcept  { return lerp(c, color{0, 0, 0, c.a}, k); }

[[nodiscard]] constexpr f32 smooth(f32 t) noexcept { return t * t * (3.0f - 2.0f * t); }

} // namespace

window_scope::~window_scope()
{
    ctx_->end_window();
}

font_scope::~font_scope()
{
    ctx_->pop_font();
}

rich_scope::~rich_scope()
{
    ctx_->pop_rich_labels();
}

selectable_text_scope::~selectable_text_scope()
{
    ctx_->pop_selectable_text();
}

modal_scope::~modal_scope()
{
    if (open_) { ctx_->end_modal(); }
}

menu_scope::~menu_scope()
{
    if (!open_) { return; }
    switch (kind_) {
    case kind::submenu:  ctx_->end_menu(); break;
    case kind::popup:    ctx_->end_popup_menu(); break;
    case kind::main_bar: ctx_->end_main_menu_bar(); break;
    }
}

style_scope::~style_scope()
{
    ctx_->pop_color(colors_);
    ctx_->pop_var(vars_);
}

std::expected<context, font_error> context::create(const context_config& cfg)
{
    std::vector<font_config> fonts;
    fonts.reserve(1 + cfg.extra_fonts.size());
    fonts.push_back(cfg.font);
    fonts.insert(fonts.end(), cfg.extra_fonts.begin(), cfg.extra_fonts.end());

    auto atlas = font_atlas::build(fonts, cfg.max_atlas_size);
    if (!atlas) {
        return std::unexpected{atlas.error()};
    }
    context ctx{std::move(*atlas), cfg.theme, cfg.limits};
    ctx.max_atlas_size_ = cfg.max_atlas_size;
    ctx.font_sources_.reserve(fonts.size());
    for (const font_config& f : fonts) {
        ctx.font_sources_.push_back({std::string{f.face}, std::string{f.file}, f});
    }
    return ctx;
}

std::expected<void, font_error> context::set_scale(f32 scale)
{
    scale = std::clamp(scale, 0.5f, 4.0f);
    if (scale == scale_) {
        return {};
    }
    if (font_sources_.empty()) {
        return std::unexpected{font_error::no_fonts};
    }
    std::vector<font_config> configs;
    configs.reserve(font_sources_.size());
    for (const font_source& src : font_sources_) {
        font_config c = src.cfg;
        c.face = src.face; // (the views in the stored config may dangle after a move of the context)
        c.file = src.file;
        configs.push_back(c);
    }
    auto atlas = font_atlas::build(configs, max_atlas_size_, scale);
    if (!atlas) {
        return std::unexpected{atlas.error()};
    }
    font_ = std::move(*atlas);
    scale_ = scale;
    ++font_generation_;
    return {};
}

context::context(font_atlas atlas, const strata::style& theme, draw_list_limits limits)
    : font_{std::move(atlas)}
    , dl_{limits}
    , style_{theme}
    , anims_(1024)
    , dock_(std::make_unique<internal::dock_state>())
{}

context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

// frame lifecycle ----------------------------------------------------------

context::~context()
{
    wipe_edit_buffer();
    detail::secure_wipe(typed_.data(), typed_.size());
}

void context::wipe_edit_buffer() noexcept
{
    // also clears the bytes beyond size() that an earlier, longer text left in the buffer
    edit_buf_.resize(edit_buf_.capacity());
    detail::secure_wipe(edit_buf_.data(), edit_buf_.size());
    edit_buf_.clear();
    edit_cursor_ = 0;
    edit_anchor_ = 0;
    edit_pref_x_ = -1.0f;
    ml_cache_key_ = 0;
    edit_history_clear(); // the undo history holds typed text as well
}

void context::begin_frame(input_state&& in)
{
    begin_frame(static_cast<const input_state&>(in));
    detail::secure_wipe(in.typed.data(), in.typed.size());
    in.typed_len = 0;
}

void context::begin_frame(const input_state& in)
{
    assert(cur_ == nullptr && "begin_frame called inside a window");
    ++frame_;

    // undo overrides the previous frame forgot to pop
    pop_color(color_depth_);
    pop_var(var_depth_);

    // only the top is limited (a stall must not fast-forward animations); a lower clamp would make ui time
    // run faster than real time at very high frame rates
    dt_      = std::clamp(in.delta_time, 1.0e-6f, 0.1f);
    time_   += dt_;
    const f32 inv_scale = 1.0f / scale_;
    display_ = in.display_size * inv_scale;
    wheel_   = in.wheel;
    wheel_consumed_ = false;
    cursor_  = cursor_kind::arrow;

    const vec2 mouse_logical = in.mouse_pos * inv_scale;
    mouse_delta_ = have_mouse_ ? mouse_logical - mouse_ : vec2{};
    mouse_       = mouse_logical;
    have_mouse_  = true;

    const bool down = in.mouse_down[0];
    mouse_pressed_  = down && !mouse_down_;
    mouse_released_ = !down && mouse_down_;
    mouse_down_     = down;

    mouse_right_pressed_ = in.mouse_down[1] && !mouse_right_down_;
    mouse_right_down_    = in.mouse_down[1];
    mod_ctrl_  = in.ctrl;
    mod_shift_ = in.shift;
    mod_alt_   = in.alt;

    // a key_sequence's pending prefix is cleared here, one frame after a key failed to advance any sequence sharing
    // it: every sequence_pressed() call for that key had its turn first (that is why this happens at the *next*
    // begin_frame, not inside sequence_pressed itself), so a prefix shared by several sequences is not torn down by
    // the first one that happens not to match, before a later one gets to check the same key
    if (seq_pending_count_ > 0 && pressed_key_ != 0 && !seq_pending_touched_) {
        seq_pending_count_ = 0;
    }
    seq_pending_touched_ = false;

    pressed_key_ = in.pressed_key;
    ime_text_    = in.ime;
    ime_len_     = std::min<u32>(in.ime_len, static_cast<u32>(in.ime.size()));
    ime_cursor_  = std::min(in.ime_cursor, ime_len_);
    ime_want_    = false;
    keys_      = in.keys;
    key_count_ = std::min(in.key_count, max_key_events);
    typed_     = in.typed;
    typed_len_ = std::min(in.typed_len, max_typed_bytes);

    hovered_window_prev_ = hovered_window_cur_;
    hovered_window_cur_  = 0;
    hovered_docked_prev_ = hovered_docked_cur_;
    hovered_docked_cur_  = false;
    hovered_z_           = no_z;
    frame_window_count_  = 0;
    font_depth_          = 0;
    rich_depth_          = 0;
    table_depth_         = 0;
    table_               = {};

    dock_->any_set     = false;
    for (dock_space& sp : dock_->spaces) {
        sp.set    = false;
        sp.hidden = false;
    }
    dock_->target_prev = dock_->target_cur;
    dock_->target_cur  = {};
    dock_->drag_prev   = dock_->drag_win;
    dock_->drag_win    = 0;
    dock_chrome_prev_ = dock_chrome_cur_;
    dock_chrome_cur_  = false;

    focus_seen_    = false;
    hotkey_seen_   = false;
    child_depth_   = 0;
    card_depth_    = 0;
    hover_key_prev_ = hover_key_cur_;
    hover_key_cur_  = 0;
    last_item_hovered_ = false;
    press_claimed_ = false;
    submitted_     = false;

    popup_open_prev_   = popup_open_cur_;
    popup_rect_prev_   = popup_rect_cur_;
    popup_anchor_prev_ = popup_anchor_cur_;
    popup_open_cur_    = false;
    in_overlay_        = false;

    // a press outside an open popup closes it and is consumed by that
    swallow_press_ = false;
    if (popup_open_prev_ && mouse_pressed_ && !popup_rect_prev_.contains(mouse_) && !popup_anchor_prev_.contains(mouse_)) {
        popup_id_      = 0;
        swallow_press_ = true;
    }

    // menus: a press outside every open menu closes them all (and only does that)
    menu_hit_prev_ = false;
    bool menu_any  = false;
    for (menu_level& m : menu_open_) {
        m.rect_prev = m.rect_cur;
        m.rect_cur  = {};
        m.seen      = false;
        if (m.key != 0) {
            menu_any       = true;
            menu_hit_prev_ = menu_hit_prev_ || m.rect_prev.contains(mouse_);
        }
    }
    menu_depth_ = 0;
    if (menu_any && (mouse_pressed_ || mouse_right_pressed_) && !menu_hit_prev_ && !menu_open_[0].anchor.contains(mouse_)) {
        menu_close_all();
        menu_hit_prev_ = false;
        swallow_press_ = true;
    }
    toast_hover_prev_ = toast_hover_cur_;
    toast_hover_cur_  = false;
    modal_top_prev_   = modal_count_ > 0 ? modal_stack_[modal_count_ - 1] : id{};
    modal_depth_      = 0;

    id_depth_    = 0;
    id_stack_[0] = 0;

    dl_.begin(in.display_size, font_, scale_);

    run_count_     = 0;
    run_owner_     = run_base;
    run_start_     = 0;
    runs_overflow_ = false;
}

void context::end_frame()
{
    assert(cur_ == nullptr && "missing end_window");
    dock_end_frame();
    toast_end_frame();
    // a menu level that was not built this frame (its code stopped running) closes with everything above it
    for (u32 i = 0; i < max_menu_levels; ++i) {
        if (menu_open_[i].key != 0 && !menu_open_[i].seen) {
            menu_close_from(i);
            break;
        }
    }
    if (menu_close_all_) {
        menu_close_all();
        menu_close_all_ = false;
    }
    apply_layer_order();

    if (!mouse_down_) {
        active_       = 0; // a widget that vanished mid-drag must not stay active forever
        dd_active_    = false;
        dd_candidate_ = 0;
        dd_cancelled_ = false;
    }
    // a press that no text field claimed takes keyboard focus away; so does a field that was not drawn
    if ((mouse_pressed_ && !press_claimed_) || (focus_id_ != 0 && !focus_seen_)) {
        focus_id_ = 0;
    }
    if (focus_id_ == 0 && !edit_buf_.empty()) {
        wipe_edit_buffer();
    }
    detail::secure_wipe(typed_.data(), typed_.size());
    typed_len_ = 0;
    if (popup_id_ != 0 && !popup_open_cur_) {
        popup_id_ = 0;
    }
    if (hotkey_capture_ != 0 && !hotkey_seen_) {
        hotkey_capture_ = 0;
    }
    hover_time_ = (hover_key_cur_ != 0 && hover_key_cur_ == hover_key_prev_) ? hover_time_ + dt_ : 0.0f;
}

// state --------------------------------------------------------------------

// animation state: an open-addressing table keyed by widget id. slots not touched for a couple of frames are stale and
// reusable; a full table grows, dropping the stale ones.

context::anim_slot* context::anim_find(id key) noexcept
{
    const u32 mask = static_cast<u32>(anims_.size()) - 1;
    for (u32 i = 0, at = key & mask; i <= mask; ++i, at = (at + 1) & mask) {
        anim_slot& s = anims_[at];
        if (s.key == key) { return &s; }
        if (s.key == 0)   { return nullptr; }
    }
    return nullptr;
}

void context::anim_rehash() noexcept
{
    std::size_t live = 0;
    for (const anim_slot& s : anims_) {
        live += s.key != 0 && s.last_frame + 2 >= frame_;
    }

    std::size_t size = 1024;
    while (size < live * 4 + 1) { size *= 2; }

    std::vector<anim_slot> old = std::move(anims_);
    anims_.assign(size, anim_slot{});
    anim_used_ = 0;

    const u32 mask = static_cast<u32>(size) - 1;
    for (const anim_slot& s : old) {
        if (s.key == 0 || s.last_frame + 2 < frame_) { continue; }
        u32 at = s.key & mask;
        while (anims_[at].key != 0) { at = (at + 1) & mask; }
        anims_[at] = s;
        ++anim_used_;
    }
}

context::press_anim context::button_anim(id key, const interaction& in) noexcept
{
    anim_slot* a = anim_find(key);
    if (a == nullptr) {
        if (!in.hovered && !in.held) {
            return {}; // idle and untracked: costs nothing
        }
        a = &anim_for(key);
    }
    a->hover  = approach(a->hover, in.hovered ? 1.0f : 0.0f);
    a->active = approach(a->active, in.held ? 1.0f : 0.0f);

    const press_anim out{a->hover, a->active};
    // back to rest: let the slot go stale so the table doesn't fill up with idle buttons
    a->last_frame = (out.hover == 0.0f && out.active == 0.0f && !in.hovered && !in.held) ? 0 : frame_;
    return out;
}

context::anim_slot& context::anim_for(id key) noexcept
{
    if (static_cast<std::size_t>(anim_used_) * 4 >= anims_.size() * 3) {
        anim_rehash();
    }

    const u32 mask = static_cast<u32>(anims_.size()) - 1;
    anim_slot* stale = nullptr;
    u32 at = key & mask;
    for (;;) {
        anim_slot& s = anims_[at];
        if (s.key == key) {
            s.last_frame = frame_;
            return s;
        }
        if (s.key == 0) {
            anim_slot& target = stale != nullptr ? *stale : s;
            if (stale == nullptr) { ++anim_used_; }
            target            = {};
            target.key        = key;
            target.last_frame = frame_;
            return target;
        }
        if (stale == nullptr && s.last_frame + 2 < frame_) { stale = &s; }
        at = (at + 1) & mask;
    }
}

f32 context::approach(f32 current, f32 target, f32 speed) const noexcept
{
    const f32 k = 1.0f - std::exp(-(speed > 0.0f ? speed : style_.anim_speed) * dt_);
    const f32 v = current + (target - current) * k;
    return std::abs(target - v) < 0.002f ? target : v;
}

f32 context::animate(std::string_view key, f32 target, f32 speed)
{
    anim_slot& a = anim_for(hash_id(key, current_seed()));
    if (!a.custom_init) {
        a.custom      = target;
        a.custom_init = true;
    } else {
        a.custom = approach(a.custom, target, speed);
    }
    return a.custom;
}

context::window_state* context::window_for(id key, vec2 pos, f32 width) noexcept
{
    window_state* free_slot = nullptr;
    for (window_state& w : windows_) {
        if (w.key == key) {
            return &w;
        }
        if (free_slot == nullptr && w.key == 0) {
            free_slot = &w;
        }
    }
    if (free_slot == nullptr) {
        internal::limit_reached("windows (max_windows)", max_windows);
        return nullptr;
    }
    *free_slot = {.key = key, .pos = pos, .width = width};
    return free_slot;
}

context::window_state* context::window_find(id key) noexcept
{
    for (window_state& w : windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

const context::window_state* context::window_find(id key) const noexcept
{
    for (const window_state& w : windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

rect context::window_rect(std::string_view title) const noexcept
{
    const window_state* w = window_find(hash_id(title, id_stack_[0]));
    if (w == nullptr) {
        return {};
    }
    const f32 height = w->collapsed ? w->title_h
                     : w->height > 0.0f ? w->height
                     : w->capped_h > 0.0f ? w->capped_h
                                          : w->title_h + 2.0f * style_.padding + w->content_h;
    return rect::from_size(w->pos, {w->width, height});
}

void context::push_id(std::string_view s) noexcept
{
    if (id_depth_ < max_id_depth) {
        const id next = hash_id(s, current_seed());
        id_stack_[++id_depth_] = next;
    } else {
        internal::limit_reached("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

void context::pop_id() noexcept
{
    if (id_depth_ > 0) {
        --id_depth_;
    }
}

void context::push_font(font_id f) noexcept
{
    if (f < font_.font_count() && font_depth_ < max_font_depth) {
        font_stack_[++font_depth_] = f;
    } else if (f < font_.font_count()) {
        internal::limit_reached("push_font nesting (max_font_depth)", max_font_depth);
    }
}

void context::pop_font() noexcept
{
    if (font_depth_ > 0) {
        --font_depth_;
    }
}

// layers: window stacking and popups ---------------------------------------------

u32 context::z_index(id key) const noexcept
{
    for (u32 i = 0; i < z_count_; ++i) {
        if (z_order_[i] == key) {
            return i;
        }
    }
    return no_z;
}

void context::bring_to_front(id key) noexcept
{
    const u32 i = z_index(key);
    if (i == no_z) {
        if (z_count_ < max_windows) {
            z_order_[z_count_++] = key;
        } else {
            internal::limit_reached("window stacking order (max_windows)", max_windows);
        }
        return;
    }
    std::rotate(z_order_.begin() + i, z_order_.begin() + i + 1, z_order_.begin() + z_count_);
}

// closes the current run of draw commands (if it produced any) and starts a new one
void context::switch_run(u32 owner) noexcept
{
    dl_.break_command();
    const u32 now = dl_.command_count();
    if (now > run_start_) {
        if (run_count_ < max_runs) {
            runs_[run_count_++] = {run_start_, now - run_start_, run_owner_};
        } else {
            runs_overflow_ = true;
            internal::limit_reached("draw runs per frame (max_runs): windows will not stack correctly", max_runs);
        }
    }
    run_owner_ = owner;
    run_start_ = now;
}

// draw order: drawing outside windows first, then the windows by stacking order
// (each window's runs kept in emission order), then popups on top of everything.
void context::apply_layer_order()
{
    switch_run(run_base);

    const u32 n = frame_window_count_;
    focused_window_ = 0;

    std::array<u32, max_windows> order{};
    for (u32 i = 0; i < n; ++i) { order[i] = i; }
    // docked windows are part of the background, then the floating ones, the menu bar, and the modals last
    const auto rank = [](const window_state& w) -> u32 {
        if (w.modal_level != 0) { return 3 + w.modal_level; }
        if (w.menubar)          { return 2; }
        return w.docked_now && w.dock_owner == 0 ? 0u : 1u;
    };
    // a window docked in a floating dock stacks with that dock, right above it
    const auto stack_z = [this](const window_state& w) -> u32 {
        const u32 z = z_index(w.docked_now && w.dock_owner != 0 ? w.dock_owner : w.key);
        return z == no_z ? 0u : z;
    };
    std::sort(order.begin(), order.begin() + n, [&](u32 a, u32 b) {
        const window_state& wa = *frame_windows_[a];
        const window_state& wb = *frame_windows_[b];
        if (rank(wa) != rank(wb)) { return rank(wa) < rank(wb); }
        if (stack_z(wa) != stack_z(wb)) { return stack_z(wa) < stack_z(wb); }
        return (wa.dock_owner != 0 && wa.docked_now ? 1 : 0) < (wb.dock_owner != 0 && wb.docked_now ? 1 : 0);
    });
    for (u32 i = n; i-- > 0;) { // the topmost window; the menu bar does not take the focus from the windows below it
        if (!frame_windows_[order[i]]->menubar) {
            focused_window_ = frame_windows_[order[i]]->key;
            break;
        }
    }
    if (runs_overflow_ || run_count_ == 0) {
        return;
    }

    std::array<draw_list::command_range, max_runs> ranges;
    u32 count = 0;
    const auto add_owner = [&](u32 owner) {
        for (u32 i = 0; i < run_count_; ++i) {
            if (runs_[i].owner == owner) {
                ranges[count++] = {runs_[i].first, runs_[i].count};
            }
        }
    };
    add_owner(run_base);
    u32 backdrops_done = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 level = frame_windows_[order[i]]->modal_level;
        while (level > backdrops_done) { // the dimmed area goes right below the first window of its level
            ++backdrops_done;
            add_owner(run_backdrop + backdrops_done - 1);
        }
        add_owner(order[i]);
    }
    add_owner(run_overlay);

    // nothing to do when the emission order already is the draw order
    u32  cursor   = 0;
    bool identity = true;
    for (u32 i = 0; i < count; ++i) {
        identity = identity && ranges[i].first == cursor;
        cursor  += ranges[i].count;
    }
    if (!identity) {
        dl_.reorder_commands({ranges.data(), count});
    }
}

// style overrides ----------------------------------------------------------

color& context::color_ref(style_color which) noexcept
{
    switch (which) {
    case style_color::window_bg:     return style_.window_bg;
    case style_color::title_bg:      return style_.title_bg;
    case style_color::border:        return style_.border;
    case style_color::widget_bg:     return style_.widget_bg;
    case style_color::widget_hover:  return style_.widget_hover;
    case style_color::widget_active: return style_.widget_active;
    case style_color::widget_border: return style_.widget_border;
    case style_color::accent:        return style_.accent;
    case style_color::accent_hover:  return style_.accent_hover;
    case style_color::text:          return style_.text;
    case style_color::text_dim:      return style_.text_dim;
    case style_color::shadow:        return style_.shadow;
    case style_color::modal_dim:     return style_.modal_dim;
    default:                         return style_.text;
    }
}

f32& context::var_ref(style_var which) noexcept
{
    switch (which) {
    case style_var::padding:         return style_.padding;
    case style_var::item_spacing:    return style_.item_spacing;
    case style_var::rounding:        return style_.rounding;
    case style_var::border_width:    return style_.border_width;
    case style_var::shadow_blur:     return style_.shadow_blur;
    case style_var::gradient:        return style_.gradient;
    case style_var::anim_speed:      return style_.anim_speed;
    case style_var::frame_padding_x: return style_.frame_padding.x;
    case style_var::frame_padding_y: return style_.frame_padding.y;
    case style_var::blur_radius:     return style_.blur_radius;
    case style_var::acrylic_alpha:   return style_.acrylic_alpha;
    case style_var::acrylic_noise:   return style_.acrylic_noise;
    case style_var::acrylic_saturation: return style_.acrylic_saturation;
    case style_var::acrylic_brightness: return style_.acrylic_brightness;
    case style_var::popup_acrylic:   return style_.popup_acrylic;
    default:                         return style_.rounding;
    }
}

void context::push_color(style_color which, color c) noexcept
{
    if (color_depth_ >= max_overrides) {
        internal::limit_reached("push_color nesting (max_overrides)", max_overrides);
        return;
    }
    color& slot = color_ref(which);
    color_stack_[color_depth_++] = {which, slot};
    slot = c;
}

void context::pop_color(u32 count) noexcept
{
    while (count-- > 0 && color_depth_ > 0) {
        const saved_color& s = color_stack_[--color_depth_];
        color_ref(s.which) = s.previous;
    }
}

void context::push_var(style_var which, f32 value) noexcept
{
    if (var_depth_ >= max_overrides) {
        internal::limit_reached("push_var nesting (max_overrides)", max_overrides);
        return;
    }
    f32& slot = var_ref(which);
    var_stack_[var_depth_++] = {which, slot};
    slot = value;
}

void context::pop_var(u32 count) noexcept
{
    while (count-- > 0 && var_depth_ > 0) {
        const saved_var& s = var_stack_[--var_depth_];
        var_ref(s.which) = s.previous;
    }
}

style_scope context::style_overrides(std::initializer_list<style_override> list) noexcept
{
    u32 colors = 0;
    u32 vars   = 0;
    for (const style_override& o : list) {
        if (o.is_color) {
            const u32 before = color_depth_;
            push_color(static_cast<style_color>(o.which), o.c);
            colors += color_depth_ - before;
        } else {
            const u32 before = var_depth_;
            push_var(static_cast<style_var>(o.which), o.v);
            vars += var_depth_ - before;
        }
    }
    return {*this, colors, vars};
}

shape_style context::widget_shape(color base, f32 radius) const noexcept
{
    shape_style s;
    s.radius       = radii(radius);
    s.fill_top     = lighten(base, style_.gradient);
    s.fill_bottom  = darken(base, style_.gradient * 0.6f);
    s.border       = style_.widget_border;
    s.border_width = style_.border_width;
    return s;
}

// layout & input -----------------------------------------------------------

rect context::layout_place(vec2 size) noexcept
{
    layout_state& l = layout_;
    const bool first_item = l.first;
    vec2 pos;
    if (l.same_line && !l.first) {
        pos      = {l.cursor_x + style_.item_spacing, l.line_top};
        l.line_h = std::max(l.line_h, size.y);
    } else {
        pos        = {l.origin.x, l.first ? l.origin.y : l.line_top + l.line_h + style_.item_spacing};
        l.line_top = pos.y;
        l.line_h   = size.y;
    }
    l.first     = false;
    l.same_line = false;
    l.cursor_x  = pos.x + size.x;
    // the first item starts the extent: a layout that is scrolled above the top of the screen has only negative
    // positions, and a `bottom` that started at 0 would stay there and make the content look taller than it is
    l.bottom    = first_item ? pos.y + size.y : std::max(l.bottom, pos.y + size.y);
    l.right     = first_item ? pos.x + size.x : std::max(l.right, pos.x + size.x);
    return rect::from_size(pos, size);
}

// a control with an optional caption above it; the caption is drawn here
context::field_layout context::layout_field(std::string_view shown, f32 control_height)
{
    const font_id f = current_font();
    const f32 w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    layout_.next_width = 0.0f;

    const f32  lh      = shown.empty() ? font_.line_height(f) : label_size(f, shown).y;
    const f32  label_h = shown.empty() ? 0.0f : lh + 3.0f;
    const rect all     = layout_place({w, label_h + control_height});

    field_layout out;
    out.has_label = !shown.empty();
    out.label_row = {all.min, {all.max.x, all.min.y + lh}};
    out.control   = {{all.min.x, all.min.y + label_h}, all.max};
    if (out.has_label) {
        label_draw({all.min.x + 1.0f, all.min.y}, style_.text_dim, shown, f);
    }
    return out;
}

context::interaction context::interact(id key, const rect& r) noexcept
{
    // popup content ignores window hover (it extends past the window)
    return interact_impl(key, r, in_overlay_ || (cur_window_ != 0 && hovered_window_prev_ == cur_window_));
}

context::interaction context::interact_impl(id key, const rect& r, bool in_window) noexcept
{
    interaction out;
    if (dl_.alpha() < 0.1f) {
        return out; // (nearly) invisible content, e.g. mid page transition
    }

    // everything but popup content is blocked while the pointer is over an open popup
    const bool blocked   = !in_overlay_ && ((popup_open_prev_ && popup_rect_prev_.contains(mouse_)) || menu_hit_prev_);
    const bool over      = in_window && !blocked && dl_.clip().contains(mouse_) && r.contains(mouse_);

    out.hovered = over && (active_ == 0 || active_ == key);
    if (out.hovered && mouse_pressed_ && active_ == 0 && !swallow_press_) {
        active_ = key;
    }
    if (active_ == key) {
        out.held = mouse_down_;
        if (mouse_released_) {
            out.pressed = over && !(dd_active_ && dd_source_ == key); // the release that ends a drag is not a click
            out.held    = false;
            active_     = 0;
        }
    }
    last_item_key_     = key;
    last_item_rect_    = r;
    last_item_hovered_ = out.hovered;
    if (out.hovered) {
        hover_key_cur_ = key;
    }
    return out;
}

item_result context::custom_item(std::string_view label, vec2 size)
{
    if (cur_ == nullptr) {
        return {};
    }
    if (size.x <= 0.0f) {
        size.x = layout_.width;
    }
    const id   key = hash_id(label, current_seed());
    const rect r   = layout_place(size);
    const interaction in = interact(key, r);
    return {r, in.hovered, in.held, in.pressed};
}

// windows ------------------------------------------------------------------

bool context::begin_window(std::string_view title, vec2 initial_pos, f32 width)
{
    return begin_window(title, initial_pos, vec2{width, 0.0f}, window_flags::none);
}

f32 context::thumb_drag(const interaction& in, f32& grab, f32 thumb_y, f32 thumb_h, f32 track_top, f32 travel,
                        f32 max_scroll, f32 scroll) const noexcept
{
    if (!in.held || travel <= 0.0f) {
        return scroll;
    }
    if (mouse_pressed_) {
        const bool on_thumb = mouse_.y >= thumb_y && mouse_.y <= thumb_y + thumb_h;
        grab = on_thumb ? mouse_.y - thumb_y : thumb_h * 0.5f;
    }
    return std::clamp((mouse_.y - grab - track_top) / travel, 0.0f, 1.0f) * max_scroll;
}

bool context::begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
{
    assert(cur_ == nullptr && "nested windows are not supported");

    const id wid = hash_id(title, id_stack_[0]);
    window_state* st = window_for(wid, initial_pos, size.x);
    if (st == nullptr) {
        return false;
    }
    if (!st->size_set) {
        // (the state can exist before the first begin_window when dock_window() got there first)
        st->pos      = initial_pos;
        st->width    = std::max(st->width, size.x);
        st->height   = std::max(size.y, 0.0f);
        st->size_set = true;
        if (st->dock != 0) {
            st->float_size = {st->width, st->height};
        }
    }
    st->last_frame = frame_;
    const bool is_menubar   = std::exchange(next_window_menubar_, false);
    const u32  modal_level  = std::exchange(next_window_modal_level_, 0);
    st->menubar     = is_menubar;
    st->modal_level = modal_level;
    if (is_menubar) {
        st->pos    = {0.0f, 0.0f};
        st->width  = display_.x;
        st->height = size.y;
    }
    { // the visible part of the title, for the tabs of a dock node
        const std::string_view vt = visible_label(title);
        std::size_t n = std::min(vt.size(), st->title.size() - 1);
        while (n > 0 && n < vt.size() && is_continuation(vt[n])) { --n; }
        std::copy_n(vt.data(), n, st->title.data());
        st->title_len = static_cast<u8>(n);
    }

    // docked: the dock node decides where the window is and how big; the tab bar replaces its title bar
    const dock_node*  node  = nullptr;
    const dock_space* space = nullptr;
    if (st->dock != 0 && st->dock <= max_dock_nodes && has_flag(flags, window_flags::dockable)) {
        const dock_node& n = dock_->nodes[st->dock - 1];
        if (n.used && n.leaf() && n.space < max_dock_spaces && dock_->spaces[n.space].set) {
            node  = &n;
            space = &dock_->spaces[n.space];
        }
    }
    const bool docked     = node != nullptr;
    const bool hidden_tab = docked && (node->active != wid || space->hidden);
    st->docked_now = docked;
    st->dock_owner = docked ? space->owner : id{};
    if (docked) {
        constexpr u8 strip = static_cast<u8>(window_flags::resizable) | static_cast<u8>(window_flags::drag_by_body);
        constexpr u8 add   = static_cast<u8>(window_flags::no_title_bar) | static_cast<u8>(window_flags::no_move);
        flags = static_cast<window_flags>((static_cast<u8>(flags) & ~strip) | add);
        st->pos       = node->shown_content.min;
        st->width     = node->shown_content.width();
        st->height    = node->shown_content.height();
        st->collapsed = false;
    }
    st->resizable = has_flag(flags, window_flags::resizable);

    const bool has_title   = !has_flag(flags, window_flags::no_title_bar);
    const bool can_collapse = has_title && !has_flag(flags, window_flags::no_collapse);
    const bool can_move    = !has_flag(flags, window_flags::no_move);
    const bool background  = !has_flag(flags, window_flags::no_background);
    if (!can_collapse) {
        st->collapsed = false;
    }
    cur_flags_ = flags;

    push_id(title);
    cur_        = st;
    cur_window_ = wid;

    if (z_index(wid) == no_z) {
        bring_to_front(wid); // first appearance: on top
    }
    // press anywhere on the topmost window under the pointer raises it (docked windows stay in the background)
    if (mouse_pressed_ && hovered_window_prev_ == wid && (!docked || st->dock_owner != 0)) {
        bring_to_front(docked ? st->dock_owner : wid); // (a window in a floating dock raises the whole dock)
    }

    // every window's draw commands form their own run so the windows can be restacked
    if (frame_window_count_ < max_windows) {
        frame_windows_[frame_window_count_] = st;
        switch_run(frame_window_count_);
        ++frame_window_count_;
    }

    const f32 lh      = font_.line_height(0);
    const f32 title_h = has_title ? lh + 10.0f : 0.0f;
    const f32 pad     = is_menubar ? 0.0f : style_.padding;
    st->title_h       = title_h;

    // collapse arrow + title drag use last frame's geometry
    const rect arrow_hit = rect::from_size(st->pos, {title_h, title_h});
    const rect drag_hit  = {{st->pos.x + (can_collapse ? title_h : 0.0f), st->pos.y}, {st->pos.x + st->width, st->pos.y + title_h}};

    if (can_collapse && interact(hash_id("##collapse", wid), arrow_hit).pressed) {
        st->collapsed = !st->collapsed;
    }
    interaction drag_in;
    if (has_title && can_move) {
        drag_in = interact(hash_id("##drag", wid), drag_hit);
        if (drag_in.held) {
            st->pos += mouse_delta_;
        }
    }
    const id   body_drag     = hash_id("##bodydrag", wid);
    const bool body_dragging = active_ == body_drag && mouse_down_ && can_move;
    if (body_dragging) {
        st->pos += mouse_delta_;
    }
    if (!docked && dock_->any_set && has_flag(flags, window_flags::dockable) && (drag_in.held || body_dragging)) {
        if (key_pressed(key::escape)) { dock_->void_key = wid; } // Esc: this drag does not dock
        if (dock_->void_key != wid && !mod_shift_) {             // ... and neither does one with Shift held
            dock_->drag_win   = wid;
            const f32 shown_h = st->height > 0.0f ? st->height : title_h + 2.0f * pad + st->content_h;
            dock_->target_cur = dock_pick(mouse_, no_node, {st->width, shown_h});
        }
    }

    // resizing: the right edge, the bottom edge and the corner (last frame's geometry)
    if (st->resizable && !st->collapsed) {
        const f32 prev_h = st->height > 0.0f ? st->height
                         : st->capped_h > 0.0f ? st->capped_h
                                               : title_h + pad + st->content_h + pad;
        const f32 grip   = 6.0f;
        const f32 corner = 16.0f;
        const f32 x1 = st->pos.x + st->width;
        const f32 y1 = st->pos.y + prev_h;

        const interaction right  = interact(hash_id("##rsz_r", wid), {{x1 - grip, st->pos.y + title_h}, {x1, y1 - corner}});
        const interaction bottom = interact(hash_id("##rsz_b", wid), {{st->pos.x, y1 - grip}, {x1 - corner, y1}});
        const interaction edge   = interact(hash_id("##rsz_c", wid), {{x1 - corner, y1 - corner}, {x1, y1}});

        if (right.hovered || right.held)   { cursor_ = cursor_kind::resize_ew; }
        if (bottom.hovered || bottom.held) { cursor_ = cursor_kind::resize_ns; }
        if (edge.hovered || edge.held)     { cursor_ = cursor_kind::resize_nwse; }

        const f32 max_w = display_.x > 0.0f ? display_.x : 4096.0f;
        const f32 max_h = display_.y > 0.0f ? display_.y : 4096.0f;
        if (right.held || edge.held) {
            st->width = std::clamp(st->width + mouse_delta_.x, 150.0f, max_w);
        }
        if (bottom.held || edge.held) {
            st->height = std::clamp(prev_h + mouse_delta_.y, title_h + 48.0f, max_h); // a fixed height from now on
        }
    }

    if (!docked && display_.x > 0 && display_.y > 0) {
        st->pos.x = std::clamp(st->pos.x, 48.0f - st->width, display_.x - 48.0f);
        st->pos.y = std::clamp(st->pos.y, 0.0f, display_.y - std::max(title_h, 24.0f));
    }

    const bool fixed_h = st->height > 0.0f;
    // an auto-height window does not grow past the bottom of the display: it scrolls instead
    st->capped_h = 0.0f;
    if (!fixed_h && !docked && !st->menubar && !st->collapsed && display_.y > 0.0f) {
        const f32 wanted = title_h + pad + st->content_h + pad;
        const f32 room   = std::max(display_.y - st->pos.y - 8.0f, title_h + 48.0f);
        if (wanted > room) { st->capped_h = room; }
    }
    const bool scrolls = fixed_h || st->capped_h > 0.0f;
    const f32  height  = st->collapsed ? title_h
                       : fixed_h ? st->height
                       : scrolls ? st->capped_h
                                 : title_h + pad + st->content_h + pad;
    const rect frame   = rect::from_size(st->pos, {st->width, height});
    const rect bar     = rect::from_size(st->pos, {st->width, title_h});
    cur_frame_ = frame;

    // a modal takes the input from everything else
    const bool input_blocked = modal_top_prev_ != 0 && wid != modal_top_prev_;
    if (frame.contains(mouse_) && !hidden_tab && !input_blocked) {
        // a floating window beats a docked one, the menu bar beats both, a modal beats them all
        const bool in_float = docked && st->dock_owner != 0;
        const u32 rank = modal_level != 0 ? 0x30000u + modal_level * 0x10000u : (is_menubar ? 0x20000u : (docked && !in_float ? 0u : 0x10000u));
        u32 zi = z_index(in_float ? st->dock_owner : wid);
        if (zi == no_z) { zi = 0; }
        const u32 z = zi * 2 + (in_float ? 1u : 0u) + rank; // a window docked in a floating dock is just above that dock
        if (hovered_z_ == no_z || z > hovered_z_) {
            hovered_window_cur_ = wid; // highest z under the pointer wins
            hovered_z_          = z;
            hovered_docked_cur_ = docked;
        }
    }

    // a window carried to a dock target goes see-through, so the pane it is aimed at shows through it
    st->ghost = approach(st->ghost, !docked && dock_->drag_prev == wid && dock_->target_prev.valid ? 1.0f : 0.0f);
    if (st->ghost > 0.01f) {
        dl_.push_alpha(1.0f - 0.45f * st->ghost);
        window_faded_ = true;
    }

    const f32 round = style_.rounding;

    if (docked) {
        if (background && !hidden_tab) {
            dl_.rect_filled(frame, style_.window_bg);
        }
    } else if (background) {
        const bool acrylic = has_flag(flags, window_flags::acrylic);
        // body: fill + soft drop shadow in a single shader quad
        shape_style body;
        body.radius        = radii(round);
        body.fill_top      = style_.window_bg;
        body.fill_bottom   = style_.window_bg;
        body.shadow        = style_.shadow;
        body.shadow_blur   = style_.shadow_blur;
        body.shadow_offset = {0.0f, style_.shadow_blur * 0.45f};
        if (acrylic) { // the shadow on its own, then the blurred frame with the tint on top
            body.fill_top = body.fill_bottom = color{0, 0, 0, 0};
            dl_.shape(frame, body);
            dl_.backdrop(frame, style_.blur_radius, style_.window_bg.scaled_alpha(style_.acrylic_alpha), radii(round),
                         style_.acrylic_noise, style_.acrylic_saturation, style_.acrylic_brightness);
        } else {
            dl_.shape(frame, body);
        }

        if (has_title) {
            shape_style head;
            head.radius      = radii(round, st->collapsed ? corners::all : corners::top);
            head.fill_top    = lighten(style_.title_bg, style_.gradient * 0.8f);
            head.fill_bottom = style_.title_bg;
            if (acrylic) {
                head.fill_top    = head.fill_top.scaled_alpha(0.55f);
                head.fill_bottom = head.fill_bottom.scaled_alpha(0.55f);
            }
            dl_.shape(bar, head);
            if (!st->collapsed) {
                dl_.rect_filled({{bar.min.x, bar.max.y - 1.0f}, bar.max}, style_.border);
            }
        }

        shape_style edge;
        edge.radius       = radii(round);
        edge.border       = style_.border;
        edge.border_width = style_.border_width;
        dl_.shape(frame, edge);
    }

    if (st->resizable && !st->collapsed) {
        const bool   hot = cursor_ == cursor_kind::resize_nwse;
        const color  gc  = hot ? style_.accent_hover : style_.text_dim.scaled_alpha(0.55f);
        const vec2   br  = frame.max;
        for (f32 k = 0.0f; k < 3.0f; ++k) {
            const f32 d = 4.0f + k * 4.0f;
            dl_.line({br.x - 3.0f, br.y - d - 1.0f}, {br.x - d - 1.0f, br.y - 3.0f}, gc, 1.5f);
        }
    }

    if (has_title) {
        if (can_collapse) {
            const vec2 c = arrow_hit.center();
            const f32  s = 4.0f;
            if (st->collapsed) {
                dl_.triangle_filled({c.x - s * 0.5f, c.y - s}, {c.x + s * 0.75f, c.y}, {c.x - s * 0.5f, c.y + s}, style_.text_dim);
            } else {
                dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.75f}, style_.text_dim);
            }
        }
        dl_.text({st->pos.x + (can_collapse ? title_h : pad), st->pos.y + (title_h - lh) * 0.5f},
                 wid == focused_window_ || focused_window_ == 0 ? style_.text : style_.text_dim, visible_label(title), 0);
    }

    // content lives below the title bar; fixed-height windows scroll
    if (st->collapsed) {
        dl_.push_clip(bar);
    } else if (hidden_tab) {
        dl_.push_clip({frame.min, frame.min}); // an inactive tab: nothing of it is drawn
    } else {
        dl_.push_clip({{frame.min.x, frame.min.y + title_h}, frame.max});
    }

    if (scrolls) {
        const f32 max_scroll = std::max(0.0f, st->content_h + 2.0f * pad - (height - title_h));
        st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);
    } else {
        st->scroll = 0.0f;
    }

    layout_        = {};
    layout_.origin = {st->pos.x + pad, st->pos.y + title_h + pad - st->scroll};
    layout_.width  = st->width - 2.0f * pad - (scrolls && st->overflow ? 10.0f : 0.0f);
    layout_.bound_bottom = fixed_h ? st->pos.y + height - pad : 0.0f;
    return !st->collapsed && !hidden_tab;
}

void context::end_window()
{
    if (cur_ == nullptr) {
        return;
    }
    window_state& w = *cur_;
    const bool has_title = !has_flag(cur_flags_, window_flags::no_title_bar);
    if (!w.collapsed) {
        w.content_h = layout_.first ? 0.0f : layout_.bottom - layout_.origin.y;

        const f32 shown_h = w.height > 0.0f ? w.height : w.capped_h;
        if (shown_h > 0.0f) {
            const f32 title_h = has_title ? font_.line_height(0) + 10.0f : 0.0f;
            const f32 pad     = w.menubar ? 0.0f : style_.padding;
            const f32 body_h  = shown_h - title_h;
            const f32 full_h  = w.content_h + 2.0f * pad;
            w.overflow = full_h > body_h + 0.5f;

            if (w.overflow) {
                const f32  max_scroll = full_h - body_h;
                const rect body = {{w.pos.x, w.pos.y + title_h}, {w.pos.x + w.width, w.pos.y + shown_h}};

                if (wheel_ != 0.0f && !wheel_consumed_ && pointer_over(body)) {
                    w.scroll = std::clamp(w.scroll - wheel_ * 48.0f, 0.0f, max_scroll);
                    wheel_consumed_ = true;
                }

                const f32  track_top = body.min.y + 6.0f;
                const f32  track_h = body_h - 12.0f;
                const f32  thumb_h = std::max(20.0f, track_h * body_h / full_h);
                f32 thumb_y = track_top + (track_h - thumb_h) * (w.scroll / max_scroll);
                const interaction in = interact(hash_id("##wscroll", cur_window_), {{body.max.x - 13.0f, track_top}, {body.max.x - 2.0f, track_top + track_h}});
                w.scroll = thumb_drag(in, w.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, w.scroll);
                thumb_y  = track_top + (track_h - thumb_h) * (w.scroll / max_scroll);
                const rect thumb = {{body.max.x - 10.0f, thumb_y}, {body.max.x - 5.0f, thumb_y + thumb_h}};
                shape_style bar;
                bar.radius      = radii(2.5f);
                bar.fill_top    = style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.45f);
                bar.fill_bottom = bar.fill_top;
                dl_.shape(thumb, bar);
            }
        }
    }

    // an empty spot of a drag_by_body window starts moving the window
    if (has_flag(cur_flags_, window_flags::drag_by_body) && !has_flag(cur_flags_, window_flags::no_move) &&
        mouse_pressed_ && active_ == 0 && !swallow_press_ && hovered_window_prev_ == cur_window_ &&
        cur_frame_.contains(mouse_) && !menu_hit_prev_ && !(popup_open_prev_ && popup_rect_prev_.contains(mouse_))) {
        active_ = hash_id("##bodydrag", cur_window_);
    }

    dl_.pop_clip();
    if (window_faded_) {
        dl_.pop_alpha();
        window_faded_ = false;
    }
    switch_run(run_base);
    pop_id();
    cur_        = nullptr;
    cur_window_ = 0;
}

// basic widgets ------------------------------------------------------------

void context::text_colored(color c, std::string_view s)
{
    if (cur_ == nullptr) {
        return;
    }
    if (selectable_depth_ > 0 && !s.empty()) {
        ml_color_ = c;
        text_selectable(s, s);
        return;
    }
    const font_id f = current_font();
    const vec2 size = label_size(f, s);
    const rect r    = layout_place(size);
    // text sharing a line with taller widgets is centered on that line
    const f32 dy = std::max(0.0f, (layout_.line_h - size.y) * 0.5f);
    label_draw({r.min.x, r.min.y + dy}, c, s, f);
}

bool context::button(std::string_view label)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());
    const std::string_view shown = visible_label(label);

    const vec2 tsize = label_size(f, shown);
    f32 w = tsize.x + style_.frame_padding.x * 2.0f;
    if (layout_.next_width > 0.0f) {
        w = layout_.next_width;
        layout_.next_width = 0.0f;
    }

    const rect r = layout_place({w, std::max(frame_height(), tsize.y + style_.frame_padding.y * 2.0f)});
    const interaction in = interact(key, r);

    const press_anim a = button_anim(key, in);

    const color base = lerp(lerp(style_.widget_bg, style_.widget_hover, a.hover), style_.accent, a.active);
    shape_style s = widget_shape(base, style_.rounding * 0.8f);
    s.border = lerp(style_.widget_border, style_.accent_hover, std::max(a.hover * 0.55f, a.active));
    dl_.shape(r, s);
    label_draw({r.min.x + (r.width() - tsize.x) * 0.5f, r.min.y + (r.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    return in.pressed;
}

bool context::icon_button(font_id icon_font, std::string_view icon, std::string_view label)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(icon, hash_id(label, current_seed()));
    const std::string_view shown = visible_label(label);

    const vec2 isize = font_.measure(icon_font, icon);
    const vec2 tsize = shown.empty() ? vec2{} : label_size(f, shown);
    const f32  gap   = shown.empty() ? 0.0f : 7.0f;
    const f32  inner = isize.x + gap + tsize.x;

    f32 w = inner + style_.frame_padding.x * 2.0f;
    if (layout_.next_width > 0.0f) {
        w = layout_.next_width;
        layout_.next_width = 0.0f;
    }
    const rect r = layout_place({w, frame_height()});
    const interaction in = interact(key, r);

    const press_anim a = button_anim(key, in);

    const color base = lerp(lerp(style_.widget_bg, style_.widget_hover, a.hover), style_.accent, a.active);
    shape_style s = widget_shape(base, style_.rounding * 0.8f);
    s.border = lerp(style_.widget_border, style_.accent_hover, std::max(a.hover * 0.55f, a.active));
    dl_.shape(r, s);

    const f32 x = r.min.x + (r.width() - inner) * 0.5f;
    dl_.text({x, r.min.y + (r.height() - isize.y) * 0.5f}, lerp(style_.text, style_.accent_hover, a.hover * 0.6f), icon, icon_font);
    if (!shown.empty()) {
        label_draw({x + isize.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    }
    return in.pressed;
}

void context::icon_label(font_id icon_font, std::string_view icon, std::string_view label)
{
    if (cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    const vec2 isize = font_.measure(icon_font, icon);
    const vec2 tsize = label_size(f, label);
    const f32  gap   = 7.0f;
    const f32  h     = std::max(isize.y, tsize.y);
    const rect r     = layout_place({isize.x + gap + tsize.x, h});
    dl_.text({r.min.x, r.min.y + (h - isize.y) * 0.5f}, style_.accent_hover, icon, icon_font);
    label_draw({r.min.x + isize.x + gap, r.min.y + (h - tsize.y) * 0.5f}, style_.text, label, f);
}

bool context::checkbox(std::string_view label, bool& value)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());
    const std::string_view shown = visible_label(label);

    const f32  box  = frame_height() - 6.0f;
    const vec2 tsize = label_size(f, shown);
    const f32  gap  = shown.empty() ? 0.0f : style_.item_spacing + 2.0f;

    const rect r   = layout_place({box + gap + tsize.x, std::max(frame_height(), tsize.y)});
    const rect bx  = rect::from_size({r.min.x, r.min.y + (r.height() - box) * 0.5f}, {box, box});
    const interaction in = interact(key, r);

    bool changed = false;
    if (in.pressed) {
        value   = !value;
        changed = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, value ? 1.0f : 0.0f);

    const color idle = lerp(style_.widget_bg, style_.widget_hover, a.hover);
    shape_style s = widget_shape(lerp(idle, style_.accent, a.toggle), style_.rounding * 0.6f);
    s.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.5f), style_.accent_hover, a.toggle);
    dl_.shape(bx, s);

    if (a.toggle > 0.01f) {
        const color mark = color{255, 255, 255, 255}.scaled_alpha(a.toggle);
        const vec2 p0{bx.min.x + box * 0.24f, bx.min.y + box * 0.52f};
        const vec2 p1{bx.min.x + box * 0.43f, bx.min.y + box * 0.72f};
        const vec2 p2{bx.min.x + box * 0.77f, bx.min.y + box * 0.30f};
        dl_.line(p0, p1, mark, 2.0f);
        dl_.line(p1, p2, mark, 2.0f);
    }

    label_draw({bx.max.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    return changed;
}

bool context::toggle(std::string_view label, bool& value)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());
    const std::string_view shown = visible_label(label);

    const f32  track_h = frame_height() - 6.0f;
    const f32  track_w = track_h * 1.85f;
    const vec2 tsize   = label_size(f, shown);
    const f32  gap     = shown.empty() ? 0.0f : style_.item_spacing + 2.0f;

    const rect r     = layout_place({track_w + gap + tsize.x, std::max(frame_height(), tsize.y)});
    const rect track = rect::from_size({r.min.x, r.min.y + (r.height() - track_h) * 0.5f}, {track_w, track_h});
    const interaction in = interact(key, r);

    bool changed = false;
    if (in.pressed) {
        value   = !value;
        changed = true;
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, value ? 1.0f : 0.0f, style_.anim_speed * 0.75f);

    const f32   t    = smooth(std::clamp(a.toggle, 0.0f, 1.0f));
    const color idle = lerp(style_.widget_bg, style_.widget_hover, a.hover);
    shape_style ts = widget_shape(lerp(idle, style_.accent, t), track_h * 0.5f);
    ts.border = lerp(style_.widget_border, style_.accent_hover, std::max(t, a.hover * 0.5f));
    dl_.shape(track, ts);

    const f32  knob_r = track_h * 0.5f - 3.0f;
    const f32  x0     = track.min.x + track_h * 0.5f;
    const f32  x1     = track.max.x - track_h * 0.5f;
    const vec2 kc{x0 + (x1 - x0) * t, track.center().y};
    shape_style ks;
    ks.radius        = radii(knob_r);
    ks.fill_top      = color{255, 255, 255, 255};
    ks.fill_bottom   = color{222, 228, 244, 255};
    ks.shadow        = style_.shadow_blur > 0.0f ? color{0, 0, 0, 110} : color{0, 0, 0, 0};
    ks.shadow_blur   = 5.0f;
    ks.shadow_offset = {0.0f, 1.5f};
    dl_.shape({{kc.x - knob_r, kc.y - knob_r}, {kc.x + knob_r, kc.y + knob_r}}, ks);

    label_draw({track.max.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    return changed;
}

bool context::slider_f32(std::string_view label, f32& value, f32 lo, f32 hi, int decimals)
{
    if (cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());
    const std::string_view shown = visible_label(label);

    constexpr f32 knob_r = 8.0f;
    const field_layout fl = layout_field(shown, knob_r * 2.0f + 6.0f);

    std::array<char, 32> buf;
    const std::string_view vtext = format_fixed(buf, value, decimals);
    const vec2 vsize = font_.measure(f, vtext);

    // without a caption the value sits to the right of the track instead of above it
    rect area = fl.control;
    if (!fl.has_label) {
        area.max.x -= vsize.x + style_.item_spacing + 2.0f;
    }

    const f32 x0 = area.min.x + knob_r;
    const f32 x1 = area.max.x - knob_r;
    const f32 cy = area.center().y;
    const interaction in = interact(key, area);

    bool changed = false;
    if (in.held && hi > lo && x1 > x0) {
        const f32 t    = std::clamp((mouse_.x - x0) / (x1 - x0), 0.0f, 1.0f);
        const f32 next = lo + t * (hi - lo);
        if (next != value) {
            value   = next;
            changed = true;
        }
    }
    value = std::clamp(value, lo, hi);

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered || in.held ? 1.0f : 0.0f);
    a.active = approach(a.active, in.held ? 1.0f : 0.0f);

    const f32 t  = hi > lo ? (value - lo) / (hi - lo) : 0.0f;
    const f32 kx = x0 + t * (x1 - x0);

    constexpr f32 half_track = 3.0f;
    shape_style track;
    track.radius       = radii(half_track);
    track.fill_top     = darken(style_.widget_bg, 0.22f);
    track.fill_bottom  = style_.widget_bg;
    track.border       = style_.widget_border;
    track.border_width = style_.border_width;
    dl_.shape({{area.min.x, cy - half_track}, {area.max.x, cy + half_track}}, track);

    if (kx > area.min.x + 1.0f) {
        shape_style fill;
        fill.radius      = radii(half_track);
        fill.fill_top    = lighten(style_.accent, style_.gradient * 1.5f);
        fill.fill_bottom = darken(style_.accent, style_.gradient);
        dl_.shape({{area.min.x, cy - half_track}, {kx, cy + half_track}}, fill);
    }

    const f32 kr = knob_r + a.active * 1.5f;
    shape_style knob;
    knob.radius        = radii(kr);
    knob.fill_top      = color{255, 255, 255, 255};
    knob.fill_bottom   = color{224, 230, 246, 255};
    knob.border        = lerp(style_.widget_border, style_.accent, a.hover);
    knob.border_width  = 1.0f + a.hover * 0.75f;
    knob.shadow        = lerp(style_.shadow_blur > 0.0f ? color{0, 0, 0, 120} : color{0, 0, 0, 0},
                              style_.accent.scaled_alpha(0.55f), a.hover);
    knob.shadow_blur   = 5.0f + 5.0f * a.hover;
    knob.shadow_offset = {0.0f, 1.5f * (1.0f - a.hover)};
    dl_.shape({{kx - kr, cy - kr}, {kx + kr, cy + kr}}, knob);

    const color vcolor = lerp(style_.text_dim, style_.text, a.hover);
    if (fl.has_label) {
        dl_.text({fl.label_row.max.x - vsize.x - 1.0f, fl.label_row.min.y}, vcolor, vtext, f);
    } else {
        dl_.text({fl.control.max.x - vsize.x, cy - vsize.y * 0.5f}, vcolor, vtext, f);
    }
    return changed;
}

void context::progress_bar(f32 fraction, std::string_view overlay)
{
    if (cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    fraction = std::clamp(fraction, 0.0f, 1.0f);

    const f32  w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    layout_.next_width = 0.0f;
    const f32  h = font_.line_height(f) + 6.0f;
    const rect r = layout_place({w, h});

    dl_.shape(r, widget_shape(style_.widget_bg, h * 0.5f));

    if (fraction > 0.0f) {
        shape_style fill;
        fill.radius       = radii(h * 0.5f);
        fill.fill_top     = lighten(style_.accent, style_.gradient * 1.5f);
        fill.fill_bottom  = darken(style_.accent, style_.gradient);
        fill.border       = style_.accent_hover;
        fill.border_width = style_.border_width;
        dl_.shape({r.min, {r.min.x + std::max(r.width() * fraction, h * 0.6f), r.max.y}}, fill);
    }

    std::array<char, 32> buf;
    std::string_view label = overlay;
    if (label.empty()) {
        const auto res = std::to_chars(buf.data(), buf.data() + buf.size() - 1, static_cast<int>(fraction * 100.0f + 0.5f));
        *res.ptr = '%';
        label = {buf.data(), res.ptr + 1};
    }
    const vec2 tsize = label_size(f, label);
    label_draw({r.min.x + (r.width() - tsize.x) * 0.5f, r.min.y + (r.height() - tsize.y) * 0.5f}, style_.text, label, f);
}

void context::separator()
{
    if (cur_ == nullptr) {
        return;
    }
    const rect r = layout_place({layout_.width, 1.0f});
    dl_.rect_filled(r, style_.border);
}

void context::spacing(f32 height)
{
    if (cur_ == nullptr) {
        return;
    }
    (void)layout_place({0.0f, height > 0.0f ? height : style_.item_spacing});
}

// text input ---------------------------------------------------------------

bool context::input_text(std::string_view label, std::string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(edit_buf_.data(), edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_text(std::string_view label, secure_string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(edit_buf_.data(), edit_buf_.size());
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

    if (!input_core(label, {buffer, len}, hint, flags, capacity - 1)) {
        return false;
    }
    const std::size_t n = std::min(edit_buf_.size(), capacity - 1);
    std::copy_n(edit_buf_.data(), n, buffer);
    buffer[n] = '\0';
    return true;
}

bool context::input_core(std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = current_font();
    f32           lh       = font_.line_height(fnt);
    f32           asc      = font_.ascent(fnt);
    const bool    password = has_flag(flags, input_flags::password);
    const bool    reveal_btn = password && has_flag(flags, input_flags::reveal);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const id      key      = hash_id(label, current_seed());
    ed_prepare_spans(focus_id_ == key ? edit_buf_.size() : current.size(), fnt, lh, asc, password);

    field_layout fl;
    if (input_rect_set_) { // a widget that owns the box (number fields): no caption, no layout
        input_rect_set_ = false;
        fl.control = input_rect_;
    } else {
        fl = layout_field(visible_label(label), frame_height());
    }
    const rect         box = fl.control;

    // a password field with an eye button: pressing it shows or hides the text (its state lives in an animation slot,
    // which is dropped once the field has not been drawn for a moment). it is tested first so it wins the press
    const f32  reveal_w = reveal_btn ? 26.0f : 0.0f;
    const rect reveal_r = {{box.max.x - reveal_w - 2.0f, box.min.y + 2.0f}, {box.max.x - 3.0f, box.max.y - 2.0f}};
    bool       revealed = false;
    bool       reveal_hot = false;
    if (reveal_btn) {
        anim_slot& ra = anim_for(hash_id("##reveal", key));
        const interaction bi = interact(hash_id("##reveal", key), reveal_r);
        if (bi.pressed) { ra.active = ra.active > 0.5f ? 0.0f : 1.0f; }
        if (mouse_pressed_ && bi.held) { press_claimed_ = true; } // the field keeps the keyboard if it had it
        revealed   = ra.active > 0.5f;
        reveal_hot = bi.hovered;
    }
    const bool hide = password && !revealed; // what is drawn as bullets

    const interaction  in  = interact(key, box);
    if (in.hovered || (in.held && focus_id_ == key)) { cursor_ = reveal_hot ? cursor_kind::arrow : cursor_kind::text; }

    const f32  pad_x  = style_.frame_padding.x;
    const rect inner  = {{box.min.x + pad_x, box.min.y}, {box.max.x - pad_x - reveal_w, box.max.y}};
    const f32  text_y = box.min.y + (box.height() - lh) * 0.5f;

    // what is shown: the live edit buffer while focused, the caller's text otherwise
    bool focused = focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{edit_buf_} : current; };

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
            bidi.build(font_, fnt, t);
        }
        return &bidi;
    };

    const auto prefix_width = [&](std::size_t byte_index) -> f32 {
        const std::string_view t = text_now();
        if (const bidi_layout* b = rtl_layout()) {
            return b->caret_x(byte_index);
        }
        if (!hide) {
            return ed_measure(fnt, t, 0, byte_index);
        }
        return font_.measure(fnt, std::string_view{masked}.substr(0, count_codepoints(t.substr(0, byte_index)))).x;
    };
    const auto index_at = [&](f32 mouse_x) {
        const std::string_view t = text_now();
        const f32 rel = mouse_x - inner.min.x + edit_scroll_;
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
    if (focus_request_ == key) { // asked for by the code (a find bar that opens): like a click on the field, all selected
        focus_request_ = 0;
        press_claimed_ = true;
        if (focus_id_ != key) {
            focus_id_    = key;
            focused      = true;
            wipe_edit_buffer();
            edit_buf_.assign(current);
            edit_scroll_ = 0.0f;
            edit_cursor_ = edit_buf_.size();
            edit_anchor_ = 0;
            caret_time_  = time_;
            rebuild_mask();
        }
    }
    const bool press_here = mouse_pressed_ && in.held;
    if (press_here) {
        press_claimed_ = true;
        if (focus_id_ != key) {
            focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(); // nothing of the previous field's text may stay behind
            edit_buf_.assign(current);
            edit_scroll_ = 0.0f;
            edit_cursor_ = edit_buf_.size();
            edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : edit_cursor_;
            caret_time_  = time_;
            rebuild_mask();
        }

        const std::size_t idx = index_at(mouse_.x);
        const u32 clicks = register_click();
        if (clicks == 3) { // triple click: the whole line (a single-line field is one line)
            edit_anchor_ = 0;
            edit_cursor_ = edit_buf_.size();
        } else if (clicks == 2) {
            edit_anchor_ = word_start(edit_buf_, idx);
            edit_cursor_ = word_end(edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || edit_cursor_ != 0) {
            edit_cursor_ = idx;
            edit_anchor_ = idx;
        }
        caret_time_ = time_;
    } else if (focused && in.held) {
        edit_cursor_ = index_at(mouse_.x);
        caret_time_  = time_;
    }
    if (focused) {
        focus_seen_ = true;
    }

    // --- keyboard ----------------------------------------------------------------
    bool changed = false;
    if (focused) {
        edit_readonly_   = readonly;
        edit_max_bytes_  = max_bytes;
        edit_history_on_ = !password && !readonly && edit_mask_.empty();

        if (typed_len_ != 0) {
            changed = edit_insert({typed_.data(), typed_len_}, true) || changed;
            typed_len_  = 0;
            caret_time_ = time_;
        }

        for (u32 i = 0; i < key_count_ && focused; ++i) {
            const key_event& ev = keys_[i];
            const std::string_view t = edit_buf_;
            const bool has_sel = edit_cursor_ != edit_anchor_;
            caret_time_ = time_;
            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    edit_cursor_ = edit_anchor_ = std::min(edit_cursor_, edit_anchor_);
                } else {
                    edit_cursor_ = ev.ctrl ? prev_word(t, edit_cursor_) : prev_boundary(t, edit_cursor_);
                    if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    edit_cursor_ = edit_anchor_ = std::max(edit_cursor_, edit_anchor_);
                } else {
                    edit_cursor_ = ev.ctrl ? next_word(t, edit_cursor_) : next_boundary(t, edit_cursor_);
                    if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                }
                break;
            case key::home:
                edit_cursor_ = 0;
                if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                break;
            case key::end:
                edit_cursor_ = t.size();
                if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, edit_cursor_) : prev_boundary(t, edit_cursor_);
                        if (a < edit_cursor_) {
                            changed = edit_replace(a, edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, edit_cursor_) : next_boundary(t, edit_cursor_);
                        if (b > edit_cursor_) {
                            changed = edit_replace(edit_cursor_, b - edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                submitted_ = true;
                break;
            case key::escape:
            case key::tab:
                focus_id_ = 0;
                focused   = false;
                break;
            case key::a:
            case key::c:
            case key::x:
            case key::v:
            case key::z:
            case key::y:
                changed = edit_shortcut(ev, password, false) || changed;
                break;
            default:
                break;
            }
        }
        key_count_ = 0;
        if (changed && !edit_mask_.empty()) { apply_input_mask(); }
        rebuild_mask();
    }

    if (focused) {
        const f32 view_w  = std::max(inner.width(), 1.0f);
        const bool composing_now = !readonly && !password && ime_len_ != 0;
        const std::string_view comp{ime_text_.data(), ime_len_};
        const f32 caret_x = prefix_width(edit_cursor_) + (composing_now ? font_.measure(fnt, comp.substr(0, ime_cursor_)).x : 0.0f);
        if (caret_x - edit_scroll_ > view_w - 2.0f) { edit_scroll_ = caret_x - view_w + 2.0f; }
        if (caret_x - edit_scroll_ < 0.0f)          { edit_scroll_ = caret_x; }
        const bidi_layout* rtl_now = rtl_layout();
        const f32 total = (rtl_now != nullptr ? rtl_now->width() : prefix_width(text_now().size())) + (composing_now ? font_.measure(fnt, comp).x : 0.0f);
        if (total - edit_scroll_ < view_w - 2.0f)   { edit_scroll_ = std::max(0.0f, total - view_w + 2.0f); }
    }

    // --- drawing -----------------------------------------------------------------
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, focused ? 1.0f : 0.0f);

    shape_style field;
    field.radius       = radii(style_.rounding * 0.8f);
    field.fill_top     = darken(style_.widget_bg, 0.28f);
    field.fill_bottom  = darken(style_.widget_bg, 0.12f);
    field.border       = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
    field.border_width = 1.0f + a.toggle * 0.5f;
    field.shadow       = style_.accent.scaled_alpha(0.32f * a.toggle);
    field.shadow_blur  = 9.0f * a.toggle;
    dl_.shape(box, field);

    if (reveal_btn) { // the eye: open when the text is shown, crossed out when it is hidden
        if (reveal_hot) {
            shape_style hot;
            hot.radius      = radii(style_.rounding * 0.6f);
            hot.fill_top    = style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            dl_.shape(reveal_r, hot);
        }
        const vec2  c   = reveal_r.center();
        const color col = lerp(style_.text_dim, style_.text, reveal_hot || revealed ? 1.0f : 0.0f);
        dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y - 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y + 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        dl_.circle_filled(c, 2.2f, col);
        if (!revealed) { dl_.line({c.x - 6.0f, c.y + 6.0f}, {c.x + 6.0f, c.y - 6.0f}, col, 1.5f); }
    }

    dl_.push_clip({{inner.min.x, box.min.y + 1.0f}, {inner.max.x, box.max.y - 1.0f}});
    const f32 text_x = inner.min.x - (focused ? edit_scroll_ : 0.0f);

    const std::string_view shown_text = text_now();
    const bool composing = focused && !readonly && !password && ime_len_ != 0;
    const std::string_view comp{ime_text_.data(), ime_len_};
    std::string disp; // the text with the composition inserted at the caret
    if (composing) {
        const std::size_t at = std::min(edit_cursor_, shown_text.size());
        disp.assign(shown_text.substr(0, at));
        disp.append(comp);
        disp.append(shown_text.substr(at));
    }
    const auto disp_width = [&](std::size_t n) { return font_.measure(fnt, std::string_view{disp}.substr(0, n)).x; };
    if (focused && edit_cursor_ != edit_anchor_ && !composing) {
        const std::size_t lo = std::min(edit_cursor_, edit_anchor_);
        const std::size_t hi = std::max(edit_cursor_, edit_anchor_);
        shape_style sel;
        sel.radius      = radii(2.0f);
        sel.fill_top    = style_.accent.scaled_alpha(0.45f);
        sel.fill_bottom = style_.accent.scaled_alpha(0.45f);
        const f32 xa = prefix_width(lo);
        const f32 xb = prefix_width(hi);
        dl_.shape({{text_x + std::min(xa, xb), text_y - 1.0f}, {text_x + std::max(xa, xb), text_y + lh + 1.0f}}, sel);
    }

    if (shown_text.empty() && !focused && !hint.empty()) {
        dl_.text({text_x, text_y}, style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    } else if (hide) {
        dl_.text({text_x, text_y}, style_.text, masked, fnt);
    } else if (composing) { // the composition sits in the text, underlined and lightly marked
        const std::size_t at = std::min(edit_cursor_, shown_text.size());
        const f32 x0 = text_x + disp_width(at);
        const f32 x1 = text_x + disp_width(at + comp.size());
        dl_.rect_filled({{x0, text_y - 1.0f}, {x1, text_y + lh + 1.0f}}, style_.accent.scaled_alpha(0.18f));
        dl_.text({text_x, text_y}, style_.text, disp, fnt);
        dl_.rect_filled({{x0, text_y + lh - 1.0f}, {x1, text_y + lh + 0.5f}}, style_.accent_hover);
    } else if (!edit_spans_.empty()) {
        ed_draw({text_x, text_y}, asc, style_.text, shown_text, 0, shown_text.size(), fnt);
    } else {
        dl_.text({text_x, text_y}, style_.text, shown_text, fnt);
    }

    if (focused) { // where the input method puts its candidate window
        const f32 caret_x = composing ? text_x + disp_width(std::min(edit_cursor_, shown_text.size()) + ime_cursor_)
                                      : text_x + prefix_width(edit_cursor_);
        ime_want_   = !password; // (a password field turns the input method off)
        ime_pos_    = {caret_x * scale_, (text_y + lh) * scale_};
        ime_line_h_ = lh * scale_;
        if (std::fmod(time_ - caret_time_, 1.06) < 0.53) { // the usual 530 ms on / 530 ms off
            const f32 cx = std::round(caret_x);
            dl_.rect_filled({{cx, text_y}, {cx + 1.5f, text_y + lh}}, style_.text);
        }
    }
    dl_.pop_clip();

    return changed;
}

// combo box ----------------------------------------------------------------

bool context::combo(std::string_view label, int& current, const std::string_view* items, std::size_t count)
{
    if (cur_ == nullptr || count == 0) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(label, current_seed());
    current = std::clamp(current, 0, static_cast<int>(count) - 1);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool open = popup_id_ == key;
    if (in.pressed) {
        if (open) {
            popup_id_ = 0;
            open      = false;
        } else {
            popup_id_     = key;
            popup_scroll_ = 0.0f;
            popup_hover_  = current;
            open          = true;
        }
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    const color base = lerp(style_.widget_bg, style_.widget_hover, a.hover);
    shape_style field = widget_shape(base, style_.rounding * 0.8f);
    field.border = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
    dl_.shape(box, field);

    const std::string_view shown = items[current];
    const vec2 tsize = label_size(f, shown);
    dl_.push_clip({{box.min.x, box.min.y}, {box.max.x - style_.frame_padding.x - 14.0f, box.max.y}});
    label_draw({box.min.x + style_.frame_padding.x, box.min.y + (box.height() - tsize.y) * 0.5f}, style_.text, shown, f);
    dl_.pop_clip();

    const vec2 c{box.max.x - style_.frame_padding.x - 4.0f, box.center().y};
    const f32  s = 4.0f;
    const color chev = lerp(style_.text_dim, style_.text, std::max(a.hover, a.toggle));
    if (open) {
        dl_.triangle_filled({c.x - s, c.y + s * 0.5f}, {c.x, c.y - s * 0.6f}, {c.x + s, c.y + s * 0.5f}, chev);
    } else {
        dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.6f}, chev);
    }

    bool changed = false;
    if (open) {
        draw_combo_popup(key, box, items, count, current, changed);
    }
    return changed;
}

void context::draw_combo_popup(id key, const rect& anchor, const std::string_view* items, std::size_t count,
                               int& current, bool& changed)
{
    const font_id f       = current_font();
    const f32     item_h  = frame_height() - 4.0f;
    const u32     visible = static_cast<u32>(std::min<std::size_t>(count, 8));
    const f32     pad     = 4.0f;
    const f32     list_h  = static_cast<f32>(visible) * item_h + 2.0f * pad;

    rect list = {{anchor.min.x, anchor.max.y + 4.0f}, {anchor.max.x, anchor.max.y + 4.0f + list_h}};
    if (list.max.y > display_.y - 4.0f && anchor.min.y - 4.0f - list_h >= 4.0f) {
        list = {{anchor.min.x, anchor.min.y - 4.0f - list_h}, {anchor.max.x, anchor.min.y - 4.0f}};
    }
    popup_open_cur_    = true;
    popup_rect_cur_    = list;
    popup_anchor_cur_  = anchor;

    const f32 view_h     = static_cast<f32>(visible) * item_h;
    const f32 max_scroll = std::max(0.0f, static_cast<f32>(count) * item_h - view_h);
    if (list.contains(mouse_) && wheel_ != 0.0f) {
        popup_scroll_ -= wheel_ * item_h * 1.5f;
        wheel_consumed_ = true;
    }

    for (u32 i = 0; i < key_count_; ++i) {
        switch (keys_[i].k) {
        case key::down:
            popup_hover_ = std::min(popup_hover_ + 1, static_cast<int>(count) - 1);
            break;
        case key::up:
            popup_hover_ = std::max(popup_hover_ - 1, 0);
            break;
        case key::enter:
            if (popup_hover_ >= 0 && popup_hover_ < static_cast<int>(count)) {
                changed   = changed || current != popup_hover_;
                current   = popup_hover_;
                popup_id_ = 0;
            }
            break;
        case key::escape:
            popup_id_ = 0;
            break;
        default:
            break;
        }
    }
    key_count_ = 0;
    if (popup_hover_ >= 0) { // keep the highlighted row on screen
        const f32 top = static_cast<f32>(popup_hover_) * item_h;
        if (top < popup_scroll_)                    { popup_scroll_ = top; }
        if (top + item_h > popup_scroll_ + view_h)  { popup_scroll_ = top + item_h - view_h; }
    }
    popup_scroll_ = std::clamp(popup_scroll_, 0.0f, max_scroll);

    // everything below is emitted into the overlay layer, above all windows
    const u32 previous_owner = run_owner_;
    switch_run(run_overlay);
    in_overlay_ = true;
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});

    shape_style body;
    body.radius        = radii(style_.rounding * 0.8f);
    body.fill_top      = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.fill_bottom   = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.border        = style_.border;
    body.border_width  = style_.border_width;
    body.shadow        = style_.shadow;
    body.shadow_blur   = style_.shadow_blur * 0.8f;
    body.shadow_offset = {0.0f, style_.shadow_blur * 0.3f};
    popup_panel(list, body);

    dl_.push_clip({{list.min.x, list.min.y + 1.0f}, {list.max.x, list.max.y - 1.0f}});
    for (std::size_t i = 0; i < count; ++i) {
        const f32  y = list.min.y + pad + static_cast<f32>(i) * item_h - popup_scroll_;
        const rect r = {{list.min.x + pad, y}, {list.max.x - pad - (max_scroll > 0.0f ? 6.0f : 0.0f), y + item_h}};
        if (r.max.y < list.min.y || r.min.y > list.max.y) {
            continue;
        }

        const id ik = hash_id({reinterpret_cast<const char*>(&i), sizeof(i)}, key);
        const interaction it = interact(ik, r);
        if (it.hovered) {
            popup_hover_ = static_cast<int>(i);
        }
        if (it.pressed) {
            changed   = changed || current != static_cast<int>(i);
            current   = static_cast<int>(i);
            popup_id_ = 0;
        }

        const bool selected = static_cast<int>(i) == current;
        const bool hot      = static_cast<int>(i) == popup_hover_;
        if (hot || selected) {
            shape_style row;
            row.radius      = radii(style_.rounding * 0.55f);
            row.fill_top    = style_.accent.scaled_alpha(hot ? 0.34f : 0.16f);
            row.fill_bottom = row.fill_top;
            dl_.shape(r, row);
        }
        const vec2 tsize = label_size(f, items[i]);
        label_draw({r.min.x + 9.0f, r.min.y + (r.height() - tsize.y) * 0.5f},
                   selected ? style_.accent_hover : style_.text, items[i], f);
    }
    dl_.pop_clip();

    if (max_scroll > 0.0f) {
        const f32 track_h = view_h;
        const f32 thumb_h = std::max(16.0f, track_h * view_h / (view_h + max_scroll));
        const f32 thumb_y = list.min.y + pad + (track_h - thumb_h) * (popup_scroll_ / max_scroll);
        shape_style thumb;
        thumb.radius      = radii(2.0f);
        thumb.fill_top    = style_.text_dim.scaled_alpha(0.5f);
        thumb.fill_bottom = thumb.fill_top;
        dl_.shape({{list.max.x - 8.0f, thumb_y}, {list.max.x - 4.0f, thumb_y + thumb_h}}, thumb);
    }

    dl_.pop_clip();
    in_overlay_ = false;
    switch_run(previous_owner);
}

} // namespace strata

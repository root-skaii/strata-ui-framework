#include "strata/context.hpp"

#include "context_impl.hpp"

#include "strata/bidi.hpp"
#include "dock_state.hpp"
#include "hash.hpp"
#include "limits.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ranges>
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

// wall clock for the frame timings in frame_stats; not used for anything the ui shows
[[nodiscard]] f64 now_ms() noexcept
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<f64, std::milli>(t).count();
}

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

nav_scope::~nav_scope()
{
    ctx_->nav_end();
}

gutter_scope::~gutter_scope()
{
    ctx_->pop_right_gutter();
}

disabled_scope::~disabled_scope()
{
    ctx_->end_disabled();
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
    ctx.m_->max_atlas_size_ = cfg.max_atlas_size;
    ctx.m_->diag_           = cfg.diagnostics;
    ctx.m_->font_sources_.reserve(fonts.size());
    for (const font_config& f : fonts) {
        ctx.m_->font_sources_.push_back({std::string{f.face}, std::string{f.file}, f});
    }
    return ctx;
}

std::expected<void, font_error> context::set_scale(f32 scale)
{
    scale = std::clamp(scale, 0.5f, 4.0f);
    if (scale == m_->scale_) {
        return {};
    }
    return build_font_atlas(scale);
}

std::expected<void, font_error> context::rebuild_font_atlas()
{
    return build_font_atlas(m_->scale_);
}

std::expected<void, font_error> context::build_font_atlas(f32 scale)
{
    if (m_->font_sources_.empty()) {
        return std::unexpected{font_error::no_fonts};
    }
    std::vector<font_config> configs;
    configs.reserve(m_->font_sources_.size());
    for (const font_source& src : m_->font_sources_) {
        font_config c = src.cfg;
        c.face = src.face; // (the views in the stored config may dangle after a move of the context)
        c.file = src.file;
        configs.push_back(c);
    }
    auto atlas = font_atlas::build(configs, m_->max_atlas_size_, scale);
    if (!atlas) {
        return std::unexpected{atlas.error()};
    }
    m_->font_ = std::move(*atlas);
    m_->scale_ = scale;
    ++m_->font_generation_;
    return {};
}

context::context(font_atlas atlas, const strata::style& theme, draw_list_limits limits)
    : m_{std::make_unique<impl>(std::move(atlas), theme, limits)}
{}

context::impl::impl(font_atlas atlas, const strata::style& theme, draw_list_limits limits)
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
    if (m_ == nullptr) {
        return; // (moved from)
    }
    wipe_edit_buffer();
    detail::secure_wipe(m_->typed_.data(), m_->typed_.size());
}

void context::wipe_edit_buffer() noexcept
{
    // also clears the bytes beyond size() that an earlier, longer text left in the buffer
    m_->edit_buf_.resize(m_->edit_buf_.capacity());
    detail::secure_wipe(m_->edit_buf_.data(), m_->edit_buf_.size());
    m_->edit_buf_.clear();
    m_->edit_cursor_ = 0;
    m_->edit_anchor_ = 0;
    m_->edit_pref_x_ = -1.0f;
    m_->ml_cache_key_ = 0;
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
    assert(m_->cur_ == nullptr && "begin_frame called inside a window");
    m_->frame_clock_ = now_ms();
    ++m_->frame_;

    // undo overrides the previous frame forgot to pop
    pop_color(m_->color_depth_);
    pop_var(m_->var_depth_);

    // only the top is limited (a stall must not fast-forward animations); a lower clamp would make ui time
    // run faster than real time at very high frame rates
    m_->dt_      = std::clamp(in.delta_time, 1.0e-6f, 0.1f);
    // timers get the whole delta (a host that slept until a deadline must find it reached); an hour caps nonsense
    m_->wall_dt_ = std::clamp(in.delta_time, 1.0e-6f, 3600.0f);
    m_->time_   += m_->wall_dt_;
    m_->caret_blink_  = std::max(in.caret_blink_time, 0.0f);
    m_->wheel_lines_  = std::min(in.wheel_lines, 100.0f); // (<= 0 is "a screenful", see scroll_step)
    m_->double_click_ = std::clamp(static_cast<f64>(in.double_click_time), 0.05, 5.0);
    const f32 inv_scale = 1.0f / m_->scale_;
    // a minimized (or briefly zero-sized) window reports a display size near 0; several window-layout
    // clamps downstream assume it is at least roughly window-sized (their lower bound is a fixed constant
    // like 150 or title_h + 48), and a smaller upper bound than that trips the debug std::clamp assertion.
    // nothing is visible while minimized anyway, so flooring here is free.
    m_->display_ = {std::max(in.display_size.x * inv_scale, 200.0f), std::max(in.display_size.y * inv_scale, 200.0f)};
    // smooth scrolling: the notches join what is still to be let out, and a share of that goes this frame (all of it
    // once little is left). a turn the other way drops what was left of the old direction
    const auto smooth = [this](f32& pending, f32 fresh) noexcept {
        if (!m_->scroll_smoothing_) {
            pending = 0.0f;
            return fresh;
        }
        pending = fresh * pending < 0.0f ? fresh : pending + fresh;
        f32 out = pending * (1.0f - std::exp(-18.0f * m_->dt_));
        if (std::abs(pending - out) < 0.02f) { out = pending; }
        pending -= out;
        return out;
    };
    m_->wheel_        = smooth(m_->wheel_pending_, in.wheel);
    m_->wheel_x_      = smooth(m_->wheel_x_pending_, in.wheel_x);
    m_->wheel_moving_ = m_->wheel_pending_ != 0.0f || m_->wheel_x_pending_ != 0.0f;
    m_->wheel_consumed_ = false;
    m_->wheel_x_consumed_ = false;
    m_->cursor_  = cursor_kind::arrow;

    const vec2 mouse_logical = in.mouse_pos * inv_scale;
    m_->mouse_delta_ = m_->have_mouse_ ? mouse_logical - m_->mouse_ : vec2{};
    m_->mouse_       = mouse_logical;
    m_->have_mouse_  = true;

    const bool down = in.mouse_down[0];
    m_->mouse_pressed_  = down && !m_->mouse_down_;
    m_->mouse_released_ = !down && m_->mouse_down_;
    m_->mouse_down_     = down;

    m_->mouse_right_pressed_ = in.mouse_down[1] && !m_->mouse_right_down_;
    m_->mouse_right_down_    = in.mouse_down[1];
    m_->mouse_middle_pressed_ = in.mouse_down[2] && !m_->mouse_middle_down_;
    m_->mouse_middle_down_    = in.mouse_down[2];
    m_->mod_ctrl_  = in.ctrl;
    m_->mod_shift_ = in.shift;
    m_->mod_alt_   = in.alt;

    // a key_sequence's pending prefix is cleared here, one frame after a key failed to advance any sequence sharing
    // it: every sequence_pressed() call for that key had its turn first (that is why this happens at the *next*
    // begin_frame, not inside sequence_pressed itself), so a prefix shared by several sequences is not torn down by
    // the first one that happens not to match, before a later one gets to check the same key
    if (m_->seq_pending_count_ > 0 && m_->pressed_key_ != 0 && !m_->seq_pending_touched_) {
        m_->seq_pending_count_ = 0;
    }
    m_->seq_pending_touched_ = false;

    // key presses: this frame's join the queue, and the oldest one is this frame's press
    const auto enqueue = [this](const key_press& p) noexcept {
        if (p.key == 0) { return; }
        if (m_->press_queued_ < m_->press_queue_.size()) { m_->press_queue_[m_->press_queued_++] = p; }
    };
    if (in.press_count > 0) {
        for (u32 i = 0; i < std::min(in.press_count, max_key_presses); ++i) { enqueue(in.presses[i]); }
    } else {
        enqueue({in.pressed_key, in.ctrl, in.shift, in.alt});
    }
    if (m_->press_queued_ > 0) {
        const key_press p = m_->press_queue_[0];
        std::copy(m_->press_queue_.begin() + 1, m_->press_queue_.begin() + m_->press_queued_, m_->press_queue_.begin());
        --m_->press_queued_;
        m_->pressed_key_ = p.key;
        m_->press_ctrl_  = p.ctrl;
        m_->press_shift_ = p.shift;
        m_->press_alt_   = p.alt;
    } else {
        m_->pressed_key_ = 0;
        m_->press_ctrl_  = in.ctrl;
        m_->press_shift_ = in.shift;
        m_->press_alt_   = in.alt;
    }
    m_->ime_text_    = in.ime;
    m_->ime_len_     = std::min<u32>(in.ime_len, static_cast<u32>(in.ime.size()));
    m_->ime_cursor_  = std::min(in.ime_cursor, m_->ime_len_);
    m_->ime_want_    = false;
    m_->keys_      = in.keys;
    m_->key_count_ = std::min(in.key_count, max_key_events);
    m_->typed_     = in.typed;
    m_->typed_len_ = std::min(in.typed_len, max_typed_bytes);

    m_->anim_moved_   = m_->wheel_moving_; // (a scroll still being let out asks for the next frame like an animation)
    m_->collision_id_ = 0;
    m_->collision_label_len_ = 0;
    m_->rich_clicked_.clear();
    m_->rich_hovered_.clear();
#ifndef NDEBUG
    // the ids submitted last frame are not the ones submitted this frame
    if (!m_->id_seen_.empty()) {
        std::ranges::fill(m_->id_seen_, id{});
    }
#endif

    m_->hovered_window_prev_ = m_->hovered_window_cur_;
    m_->hovered_window_cur_  = 0;
    m_->hovered_docked_prev_ = m_->hovered_docked_cur_;
    m_->hovered_docked_cur_  = false;
    m_->hovered_z_           = no_z;
    m_->frame_window_count_  = 0;
    m_->font_depth_          = 0;
    m_->rich_depth_          = 0;
    m_->table_depth_         = 0;
    m_->table_               = {};

    m_->dock_->any_set     = false;
    for (dock_space& sp : m_->dock_->spaces) {
        sp.set    = false;
        sp.hidden = false;
    }
    m_->dock_->target_prev = m_->dock_->target_cur;
    m_->dock_->target_cur  = {};
    m_->dock_->drag_prev   = m_->dock_->drag_win;
    m_->dock_->drag_win    = 0;
    m_->dock_chrome_prev_ = m_->dock_chrome_cur_;
    m_->dock_chrome_cur_  = false;

    m_->focus_seen_    = false;
    m_->hotkey_seen_   = false;
    m_->child_depth_   = 0;
    m_->card_depth_    = 0;
    m_->hover_key_prev_ = m_->hover_key_cur_;
    m_->hover_key_cur_  = 0;
    m_->last_item_hovered_ = false;
    m_->last_item_focused_ = false;
    m_->last_item_pressed_ = false;
    m_->last_item_double_  = false;
    m_->press_claimed_ = false;
    m_->submitted_     = false;

    m_->keys_held_      = in.keys_held;
    m_->stats_cur_      = {};
    m_->disabled_depth_ = 0;
    m_->disabled_alpha_ = false;
    m_->disabled_count_ = 0;
    m_->next_gutter_    = 0.0f;
    m_->accessory_row_  = 0;
    m_->row_anchor_     = 0;
    m_->last_item_arrow_     = false;
    m_->last_item_truncated_ = false;
    m_->engaged_prev_        = m_->engaged_cur_;
    m_->engaged_prev_count_  = m_->engaged_cur_count_;
    m_->engaged_cur_count_   = 0;
    m_->edit_item_           = 0;
    m_->edit_flags_          = {};
    m_->edit_muted_          = 0;
    m_->overlap_key_        = 0;
    m_->overlap_rect_       = {};
    m_->overlap_stolen_     = false;
    m_->overlap_taken_prev_ = m_->overlap_taken_cur_;
    m_->overlap_taken_cur_  = 0;
    m_->next_open_          = 0;
    m_->gutter_depth_       = 0;
    m_->nav_.scope          = 0;
    m_->nav_.active         = false;
    m_->nav_.items.clear();
    if (m_->tree_bulk_ != 0 && m_->tree_bulk_frames_ > 0) {
        --m_->tree_bulk_frames_;
    }
    if (m_->tree_bulk_frames_ == 0) {
        m_->tree_bulk_      = 0;
        m_->tree_bulk_seed_ = 0;
    }

    m_->popup_open_prev_   = m_->popup_open_cur_;
    m_->popup_rect_prev_   = m_->popup_rect_cur_;
    m_->popup_anchor_prev_ = m_->popup_anchor_cur_;
    m_->popup_open_cur_    = false;
    m_->in_overlay_        = false;

    // a press outside an open popup closes it and is consumed by that
    m_->swallow_press_ = false;
    if (m_->popup_open_prev_ && m_->mouse_pressed_ && !m_->popup_rect_prev_.contains(m_->mouse_) && !m_->popup_anchor_prev_.contains(m_->mouse_)) {
        m_->popup_id_      = 0;
        m_->swallow_press_ = true;
    }

    // menus: a press outside every open menu closes them all (and only does that)
    m_->menu_hit_prev_ = false;
    bool menu_any  = false;
    for (menu_level& m : m_->menu_open_) {
        m.rect_prev = m.rect_cur;
        m.rect_cur  = {};
        m.seen      = false;
        if (m.key != 0) {
            menu_any       = true;
            m_->menu_hit_prev_ = m_->menu_hit_prev_ || m.rect_prev.contains(m_->mouse_);
        }
    }
    m_->menu_depth_ = 0;
    if (menu_any && (m_->mouse_pressed_ || m_->mouse_right_pressed_) && !m_->menu_hit_prev_ && !m_->menu_open_[0].anchor.contains(m_->mouse_)) {
        menu_close_all();
        m_->menu_hit_prev_ = false;
        m_->swallow_press_ = true;
    }
    m_->toast_hover_prev_ = m_->toast_hover_cur_;
    m_->toast_hover_cur_  = false;
    m_->modal_top_prev_   = m_->modal_count_ > 0 ? m_->modal_stack_[m_->modal_count_ - 1] : id{};
    m_->modal_depth_      = 0;

    m_->id_depth_    = 0;
    m_->id_stack_[0] = 0;

    m_->dl_.begin(in.display_size, m_->font_, m_->scale_);

    m_->run_count_     = 0;
    m_->run_owner_     = run_base;
    m_->run_start_     = 0;
    m_->runs_overflow_ = false;

    m_->begin_frame_ms_ = now_ms() - m_->frame_clock_;
}

void context::end_frame()
{
    const f64 end_start = now_ms();
    assert(m_->cur_ == nullptr && "missing end_window");
    dock_end_frame();
    toast_end_frame();
    // a menu level that was not built this frame (its code stopped running) closes with everything above it
    for (u32 i = 0; i < max_menu_levels; ++i) {
        if (m_->menu_open_[i].key != 0 && !m_->menu_open_[i].seen) {
            menu_close_from(i);
            break;
        }
    }
    if (m_->menu_close_all_) {
        menu_close_all();
        m_->menu_close_all_ = false;
    }
    apply_layer_order();

    if (!m_->mouse_down_) {
        m_->active_       = 0; // a widget that vanished mid-drag must not stay active forever
        m_->dd_active_    = false;
        m_->dd_candidate_ = 0;
        m_->dd_cancelled_ = false;
    }
    // a press that no text field claimed takes keyboard focus away; so does a field that was not drawn
    if ((m_->mouse_pressed_ && !m_->press_claimed_) || (m_->focus_id_ != 0 && !m_->focus_seen_)) {
        m_->focus_id_ = 0;
    }
    if (m_->focus_id_ == 0 && !m_->edit_buf_.empty()) {
        wipe_edit_buffer();
    }
    detail::secure_wipe(m_->typed_.data(), m_->typed_.size());
    m_->typed_len_ = 0;
    if (m_->popup_id_ != 0 && !m_->popup_open_cur_) {
        m_->popup_id_ = 0;
    }
    if (m_->hotkey_capture_ != 0 && !m_->hotkey_seen_) {
        m_->hotkey_capture_ = 0;
    }
    m_->hover_time_ = (m_->hover_key_cur_ != 0 && m_->hover_key_cur_ == m_->hover_key_prev_) ? m_->hover_time_ + m_->wall_dt_ : 0.0f;

    // the animation table keeps stale slots around, so a tree that was fully expanded (and is now culled again) would
    // leave it as big as the whole tree forever. it costs nothing until it is walked, but a table far larger than the
    // live key count is also a cache miss per lookup: compact it back down now and then.
    if (m_->anims_.size() > 1024 && (m_->frame_ & 0xff) == 0) {
        std::size_t live = 0;
        for (const anim_slot& s : m_->anims_) {
            live += s.key != 0 && s.last_frame + 2 >= m_->frame_;
        }
        if (live * 16 < m_->anims_.size()) {
            anim_rehash();
        }
    }

    const draw_data dd = m_->dl_.data();

    // has anything the renderer sees changed? a hash of the four arrays, which is the only honest test: the ui is
    // rebuilt from scratch every frame, so "nothing was touched" is not something the widget code can report.
    // xxh64 over the raw bytes, each array seeded with the hash of the ones before -- they are trivially copyable and
    // packed, and a frame that differs anywhere (a colour, a caret, one pixel of a scrollbar) differs here.
    u64 hash = 0;
    hash = internal::hash_bytes(dd.vertices.data(), dd.vertices.size_bytes(), hash);
    hash = internal::hash_bytes(dd.indices.data(), dd.indices.size_bytes(), hash);
    hash = internal::hash_bytes(dd.commands.data(), dd.commands.size_bytes(), hash);
    hash = internal::hash_bytes(dd.shapes.data(), dd.shapes.size_bytes(), hash);
    hash = internal::hash_bytes(&dd.display_size, sizeof(dd.display_size), hash);
    hash = internal::hash_bytes(&m_->style_.text_contrast, sizeof(m_->style_.text_contrast), hash); // (the shader applies it)
    if (hash == 0) { hash = 1; } // 0 is reserved for "no previous frame" (invalidate)
    m_->frame_unchanged_ = m_->geometry_hash_ != 0 && m_->geometry_hash_ == hash;
    m_->geometry_hash_   = hash;

    // ... and is a later frame going to look different even if nothing is touched? an animation still short of its
    // target, plus the two things driven by a clock rather than by a value: a toast waiting to expire or fade, and
    // the pause before a tooltip appears. without those two a host that idles on can_idle() would freeze a toast on
    // screen forever and never show a tooltip, because the frames that would have advanced them never run.
    const bool tooltip_pending = m_->hover_key_cur_ != 0 && m_->hover_time_ < m_->style_.tooltip_delay_s;
    // toasts: one sliding, fading or with a bar that moves (its timer counting down, a busy bar) changes every frame. one
    // that stays until closed, or whose timer the pointer is holding, changes nothing by itself
    bool toast_moving = false;
    for (const toast_entry& t : m_->toasts_) {
        toast_moving = toast_moving || t.dismissed || t.anim < 1.0f || t.progress == toast_busy || (!t.sticky && !t.paused);
    }
    m_->anim_settling_ = m_->anim_moved_ || toast_moving || tooltip_pending;

    // how long a sleeping host may wait: nothing below changes the picture before its deadline
    f64 wake = no_deadline;
    if (!m_->frame_unchanged_ || m_->anim_moved_ || toast_moving) {
        wake = 0.0;
    } else {
        if (tooltip_pending) { wake = std::min(wake, static_cast<f64>(m_->style_.tooltip_delay_s - m_->hover_time_)); }
        if (m_->focus_id_ != 0 && m_->caret_blink_ > 0.0f) { // the next flip of the caret
            const f64 period = 2.0 * m_->caret_blink_;
            const f64 phase  = std::fmod(m_->time_ - m_->caret_time_, period);
            wake = std::min(wake, phase < m_->caret_blink_ ? m_->caret_blink_ - phase : period - phase);
        }
        if (m_->hotkey_capture_ != 0 && m_->seq_edit_count_ > 0) { wake = std::min(wake, m_->seq_edit_deadline_ - m_->time_); }
        // land just past a deadline, not a hair before it
        if (wake != no_deadline) { wake = std::max(wake, 0.0) + 0.001; }
    }
    m_->next_wake_ = wake;

    m_->stats_cur_.vertices         = static_cast<u32>(dd.vertices.size());
    m_->stats_cur_.indices          = static_cast<u32>(dd.indices.size());
    m_->stats_cur_.draw_calls       = static_cast<u32>(dd.commands.size());
    m_->stats_cur_.anim_slots_used  = m_->anim_used_;
    m_->stats_cur_.anim_slots_total = static_cast<u32>(m_->anims_.size());
    m_->stats_cur_.draw_overflow    = m_->dl_.overflowed() ? 1u : 0u;
    m_->stats_cur_.clip_overflows   = m_->dl_.clip_stack_overflows();
    m_->stats_cur_.alpha_overflows  = m_->dl_.alpha_stack_overflows();
    if (m_->stats_cur_.draw_overflow != 0) {
        diagnose(diagnostic_kind::draw_overflow, "the draw list ran out of room: geometry was dropped (raise context_config::limits)", 1);
    }
    if (m_->stats_cur_.clip_overflows != 0 || m_->stats_cur_.alpha_overflows != 0) {
        diagnose(diagnostic_kind::draw_overflow, "clip / alpha nesting deeper than the draw list holds: inner levels were not clipped / faded", 2);
    }
    m_->stats_cur_.unchanged        = m_->frame_unchanged_;
    m_->stats_cur_.animating        = m_->anim_settling_;
    m_->stats_cur_.begin_frame_ms   = m_->begin_frame_ms_;
    m_->stats_cur_.end_frame_ms     = now_ms() - end_start;
    m_->stats_prev_                 = m_->stats_cur_;
}

// state --------------------------------------------------------------------

// animation state: an open-addressing table keyed by widget id. slots not touched for a couple of frames are stale and
// reusable; a full table grows, dropping the stale ones.

context::anim_slot* context::anim_find(id key) noexcept
{
    const u32 mask = static_cast<u32>(m_->anims_.size()) - 1;
    for (u32 i = 0, at = key & mask; i <= mask; ++i, at = (at + 1) & mask) {
        anim_slot& s = m_->anims_[at];
        if (s.key == key) { return &s; }
        if (s.key == 0)   { return nullptr; }
    }
    return nullptr;
}

void context::anim_rehash() noexcept
{
    std::size_t live = 0;
    for (const anim_slot& s : m_->anims_) {
        live += s.key != 0 && s.last_frame + 2 >= m_->frame_;
    }

    std::size_t size = 1024;
    while (size < live * 4 + 1) { size *= 2; }

    std::vector<anim_slot> old = std::move(m_->anims_);
    m_->anims_.assign(size, anim_slot{});
    m_->anim_used_ = 0;

    const u32 mask = static_cast<u32>(size) - 1;
    for (const anim_slot& s : old) {
        if (s.key == 0 || s.last_frame + 2 < m_->frame_) { continue; }
        u32 at = s.key & mask;
        while (m_->anims_[at].key != 0) { at = (at + 1) & mask; }
        m_->anims_[at] = s;
        ++m_->anim_used_;
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
    a->last_frame = (out.hover == 0.0f && out.active == 0.0f && !in.hovered && !in.held) ? 0 : m_->frame_;
    return out;
}

context::anim_slot& context::anim_for(id key) noexcept
{
    if (static_cast<std::size_t>(m_->anim_used_) * 4 >= m_->anims_.size() * 3) {
        anim_rehash();
    }

    const u32 mask = static_cast<u32>(m_->anims_.size()) - 1;
    anim_slot* stale = nullptr;
    u32 at = key & mask;
    for (;;) {
        anim_slot& s = m_->anims_[at];
        if (s.key == key) {
            s.last_frame = m_->frame_;
            return s;
        }
        if (s.key == 0) {
            anim_slot& target = stale != nullptr ? *stale : s;
            if (stale == nullptr) { ++m_->anim_used_; }
            target            = {};
            target.key        = key;
            target.last_frame = m_->frame_;
            return target;
        }
        if (stale == nullptr && s.last_frame + 2 < m_->frame_) { stale = &s; }
        at = (at + 1) & mask;
    }
}

f32 context::approach(f32 current, f32 target, f32 speed) const noexcept
{
    const f32 k = 1.0f - std::exp(-(speed > 0.0f ? speed : m_->style_.anim_speed) * m_->dt_);
    const f32 v = current + (target - current) * k;
    if (std::abs(target - v) < 0.002f) {
        return target; // close enough: snap, so a settled animation stops producing new geometry
    }
    m_->anim_moved_ = true; // still moving: this frame's geometry is not the last word (see animations_settling)
    return v;
}

f32 context::animate(std::string_view key, f32 target, f32 speed)
{
    anim_slot& a = anim_for(widget_id(key));
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
    for (window_state& w : m_->windows_) {
        if (w.key == key) {
            return &w;
        }
        if (free_slot == nullptr && w.key == 0) {
            free_slot = &w;
        }
    }
    if (free_slot == nullptr) {
        // every slot is taken: the window shown longest ago gives its slot up (were it to come back, it starts over where
        // its code first puts it). kept whatever their age: docked windows and floating docks, which the dock layout refers
        // to, open modals, and anything shown this frame or the last
        const auto owns_space = [&](id k) {
            return std::ranges::any_of(m_->dock_->spaces, [k](const dock_space& sp) { return sp.key != 0 && sp.owner == k; });
        };
        const auto modal_open = [&](id k) {
            return std::find(m_->modal_stack_.begin(), m_->modal_stack_.begin() + m_->modal_count_, k) != m_->modal_stack_.begin() + m_->modal_count_;
        };
        for (window_state& w : m_->windows_) {
            if (w.dock != 0 || w.last_frame + 1 >= m_->frame_ || owns_space(w.key) || modal_open(w.key)) { continue; }
            if (free_slot == nullptr || w.last_frame < free_slot->last_frame) { free_slot = &w; }
        }
        if (free_slot == nullptr) {
            report_limit("windows (max_windows)", max_windows);
            return nullptr;
        }
        forget_window(free_slot->key);
    }
    *free_slot = {.key = key, .pos = pos, .width = width};
    return free_slot;
}

f32 context::scroll_step(f32 unit, f32 page) const noexcept
{
    const f32 lines = m_->wheel_lines_;
    const f32 step  = lines > 0.0f ? lines * unit : (page > 0.0f ? page * 0.9f : unit * 10.0f);
    return step * std::max(m_->style_.scroll_speed, 0.0f);
}

f32 context::wheel_scroll(f32 unit, f32 page) const noexcept { return m_->wheel_ * scroll_step(unit, page); }
f32 context::wheel_scroll_x(f32 unit, f32 page) const noexcept { return m_->wheel_x_ * scroll_step(unit, page); }

void context::report_limit(const char* what, u32 capacity) noexcept
{
    ++m_->stats_cur_.limits_hit;
    char buf[192];
    const int n = std::snprintf(buf, sizeof buf, "limit reached: %s (capacity %u) - what does not fit is dropped", what, capacity);
    diagnose(diagnostic_kind::limit, {buf, n > 0 ? std::min<std::size_t>(static_cast<std::size_t>(n), sizeof buf - 1) : 0},
             hash_id(what));
}

void context::diagnose(diagnostic_kind kind, std::string_view message, u64 once_key) noexcept
{
    const u64 key = once_key ^ (static_cast<u64>(kind) << 56);
    if (std::ranges::find(m_->diag_seen_, key) != m_->diag_seen_.end()) {
        return;
    }
    if (m_->diag_seen_.size() < 256) { // (past that, a program has bigger problems than a repeated line)
        m_->diag_seen_.push_back(key);
    }
    if (m_->diag_.report != nullptr) {
        m_->diag_.report(m_->diag_.user, diagnostic{kind, message});
    } else {
        internal::print_diagnostic(message);
    }
}

void context::track_edit(id session, bool changed, bool engaged) noexcept
{
    if (m_->edit_muted_ > 0) {
        return;
    }
    const bool was = std::find(m_->engaged_prev_.begin(), m_->engaged_prev_.begin() + m_->engaged_prev_count_, session) !=
                     m_->engaged_prev_.begin() + m_->engaged_prev_count_;
    if (engaged && m_->engaged_cur_count_ < m_->engaged_cur_.size()) {
        m_->engaged_cur_[m_->engaged_cur_count_++] = session;
    }
    edit_flags f;
    f.active      = engaged;
    f.activated   = engaged && !was;
    f.deactivated = !engaged && was;
    f.edited      = changed;
    edit_session* open = nullptr;
    for (edit_session& s : m_->edit_sessions_) {
        if (s.key == session) { open = &s; }
    }
    if (f.activated) { // a session starts (a stale one with this key starts over)
        if (open == nullptr) {
            open = &m_->edit_sessions_[0];
            for (edit_session& s : m_->edit_sessions_) {
                if (s.key == 0) { open = &s; break; }
            }
        }
        *open = {session, false};
    }
    if (open != nullptr && (engaged || was)) { open->changed = open->changed || changed; }
    // over: a session that changed something, or a change with no session around it (it started and ended at once)
    f.after_edit = !engaged && (open != nullptr && was ? open->changed : changed);
    if (!engaged && open != nullptr) { *open = {}; }
    m_->edit_flags_ = f;
    m_->edit_item_  = m_->last_item_key_;
}

// what refers to a window by its key, other than its slot: the stacking order and the keyboard / hover / focus trackers
void context::forget_window(id key) noexcept
{
    if (const u32 z = z_index(key); z != no_z) {
        std::copy(m_->z_order_.begin() + z + 1, m_->z_order_.begin() + m_->z_count_, m_->z_order_.begin() + z);
        --m_->z_count_;
    }
    for (id* k : {&m_->focused_window_, &m_->key_window_, &m_->hovered_window_prev_, &m_->hovered_window_cur_}) {
        if (*k == key) { *k = 0; }
    }
}

context::window_state* context::window_find(id key) noexcept
{
    for (window_state& w : m_->windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

const context::window_state* context::window_find(id key) const noexcept
{
    for (const window_state& w : m_->windows_) {
        if (w.key == key) {
            return &w;
        }
    }
    return nullptr;
}

rect context::window_rect(std::string_view title) const noexcept
{
    const window_state* w = window_find(hash_id(title, m_->id_stack_[0]));
    if (w == nullptr) {
        return {};
    }
    const f32 height = w->collapsed ? w->title_h
                     : w->height > 0.0f ? w->height
                     : w->capped_h > 0.0f ? w->capped_h
                                          : w->title_h + 2.0f * m_->style_.padding + w->content_h;
    return rect::from_size(w->pos, {w->width, height});
}

void context::push_id(std::string_view s) noexcept
{
    if (m_->id_depth_ < max_id_depth) {
        const id next = hash_id(s, current_seed());
        m_->id_stack_[++m_->id_depth_] = next;
    } else {
        report_limit("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

void context::push_id(const void* p) noexcept
{
    push_id(static_cast<u64>(reinterpret_cast<std::uintptr_t>(p)));
}

void context::push_id(u64 value) noexcept
{
    if (m_->id_depth_ >= max_id_depth) {
        report_limit("push_id nesting (max_id_depth): ids will collide", max_id_depth);
        return;
    }
    std::array<char, sizeof(u64)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
    m_->id_stack_[++m_->id_depth_] = hash_id({bytes.data(), bytes.size()}, current_seed());
}

void context::pop_id() noexcept
{
    if (m_->id_depth_ > 0) {
        --m_->id_depth_;
    }
}

void context::push_font(font_id f) noexcept
{
    if (f < m_->font_.font_count() && m_->font_depth_ < max_font_depth) {
        m_->font_stack_[++m_->font_depth_] = f;
    } else if (f < m_->font_.font_count()) {
        report_limit("push_font nesting (max_font_depth)", max_font_depth);
    }
}

void context::pop_font() noexcept
{
    if (m_->font_depth_ > 0) {
        --m_->font_depth_;
    }
}

// layers: window stacking and popups ---------------------------------------------

u32 context::z_index(id key) const noexcept
{
    for (u32 i = 0; i < m_->z_count_; ++i) {
        if (m_->z_order_[i] == key) {
            return i;
        }
    }
    return no_z;
}

void context::bring_to_front(id key) noexcept
{
    const u32 i = z_index(key);
    if (i == no_z) {
        if (m_->z_count_ < max_windows) {
            m_->z_order_[m_->z_count_++] = key;
        } else {
            report_limit("window stacking order (max_windows)", max_windows);
        }
        return;
    }
    std::rotate(m_->z_order_.begin() + i, m_->z_order_.begin() + i + 1, m_->z_order_.begin() + m_->z_count_);
}

// closes the current run of draw commands (if it produced any) and starts a new one
void context::switch_run(u32 owner) noexcept
{
    m_->dl_.break_command();
    const u32 now = m_->dl_.command_count();
    if (now > m_->run_start_) {
        if (m_->run_count_ < max_runs) {
            m_->runs_[m_->run_count_++] = {m_->run_start_, now - m_->run_start_, m_->run_owner_};
        } else {
            m_->runs_overflow_ = true;
            report_limit("draw runs per frame (max_runs): windows will not stack correctly", max_runs);
        }
    }
    m_->run_owner_ = owner;
    m_->run_start_ = now;
}

// draw order: drawing outside windows first, then the windows by stacking order
// (each window's runs kept in emission order), then popups on top of everything.
void context::apply_layer_order()
{
    switch_run(run_base);

    const u32 n = m_->frame_window_count_;
    m_->focused_window_ = 0;

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
        const window_state& wa = *m_->frame_windows_[a];
        const window_state& wb = *m_->frame_windows_[b];
        if (rank(wa) != rank(wb)) { return rank(wa) < rank(wb); }
        if (stack_z(wa) != stack_z(wb)) { return stack_z(wa) < stack_z(wb); }
        return (wa.dock_owner != 0 && wa.docked_now ? 1 : 0) < (wb.dock_owner != 0 && wb.docked_now ? 1 : 0);
    });
    for (u32 i = n; i-- > 0;) { // the topmost window; the menu bar does not take the focus from the windows below it
        if (!m_->frame_windows_[order[i]]->menubar) {
            m_->focused_window_ = m_->frame_windows_[order[i]]->key;
            break;
        }
    }
    if (m_->runs_overflow_ || m_->run_count_ == 0) {
        return;
    }

    std::array<draw_list::command_range, max_runs> ranges;
    u32 count = 0;
    const auto add_owner = [&](u32 owner) {
        for (u32 i = 0; i < m_->run_count_; ++i) {
            if (m_->runs_[i].owner == owner) {
                ranges[count++] = {m_->runs_[i].first, m_->runs_[i].count};
            }
        }
    };
    add_owner(run_base);
    u32 backdrops_done = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 level = m_->frame_windows_[order[i]]->modal_level;
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
        m_->dl_.reorder_commands({ranges.data(), count});
    }
}

// style overrides ----------------------------------------------------------

color& context::color_ref(style_color which) noexcept
{
    switch (which) {
    case style_color::window_bg:     return m_->style_.window_bg;
    case style_color::title_bg:      return m_->style_.title_bg;
    case style_color::border:        return m_->style_.border;
    case style_color::widget_bg:     return m_->style_.widget_bg;
    case style_color::widget_hover:  return m_->style_.widget_hover;
    case style_color::widget_active: return m_->style_.widget_active;
    case style_color::widget_border: return m_->style_.widget_border;
    case style_color::accent:        return m_->style_.accent;
    case style_color::accent_hover:  return m_->style_.accent_hover;
    case style_color::text:          return m_->style_.text;
    case style_color::text_dim:      return m_->style_.text_dim;
    case style_color::shadow:        return m_->style_.shadow;
    case style_color::modal_dim:     return m_->style_.modal_dim;
    case style_color::success:       return m_->style_.success;
    case style_color::warning:       return m_->style_.warning;
    case style_color::error:         return m_->style_.error;
    default:                         return m_->style_.text;
    }
}

f32& context::var_ref(style_var which) noexcept
{
    switch (which) {
    case style_var::padding:         return m_->style_.padding;
    case style_var::item_spacing:    return m_->style_.item_spacing;
    case style_var::rounding:        return m_->style_.rounding;
    case style_var::border_width:    return m_->style_.border_width;
    case style_var::shadow_blur:     return m_->style_.shadow_blur;
    case style_var::gradient:        return m_->style_.gradient;
    case style_var::anim_speed:      return m_->style_.anim_speed;
    case style_var::frame_padding_x: return m_->style_.frame_padding.x;
    case style_var::frame_padding_y: return m_->style_.frame_padding.y;
    case style_var::blur_radius:     return m_->style_.blur_radius;
    case style_var::acrylic_alpha:   return m_->style_.acrylic_alpha;
    case style_var::acrylic_noise:   return m_->style_.acrylic_noise;
    case style_var::acrylic_saturation: return m_->style_.acrylic_saturation;
    case style_var::acrylic_brightness: return m_->style_.acrylic_brightness;
    case style_var::popup_acrylic:   return m_->style_.popup_acrylic;
    case style_var::tooltip_delay:   return m_->style_.tooltip_delay_s;
    case style_var::text_contrast:   return m_->style_.text_contrast;
    case style_var::scroll_speed:    return m_->style_.scroll_speed;
    default:                         return m_->style_.rounding;
    }
}

void context::push_color(style_color which, color c) noexcept
{
    if (m_->color_depth_ >= max_overrides) {
        report_limit("push_color nesting (max_overrides)", max_overrides);
        return;
    }
    color& slot = color_ref(which);
    m_->color_stack_[m_->color_depth_++] = {which, slot};
    slot = c;
}

void context::pop_color(u32 count) noexcept
{
    while (count-- > 0 && m_->color_depth_ > 0) {
        const saved_color& s = m_->color_stack_[--m_->color_depth_];
        color_ref(s.which) = s.previous;
    }
}

void context::push_var(style_var which, f32 value) noexcept
{
    if (m_->var_depth_ >= max_overrides) {
        report_limit("push_var nesting (max_overrides)", max_overrides);
        return;
    }
    f32& slot = var_ref(which);
    m_->var_stack_[m_->var_depth_++] = {which, slot};
    slot = value;
}

void context::pop_var(u32 count) noexcept
{
    while (count-- > 0 && m_->var_depth_ > 0) {
        const saved_var& s = m_->var_stack_[--m_->var_depth_];
        var_ref(s.which) = s.previous;
    }
}

style_scope context::style_overrides(std::initializer_list<style_override> list) noexcept
{
    u32 colors = 0;
    u32 vars   = 0;
    for (const style_override& o : list) {
        if (o.is_color) {
            const u32 before = m_->color_depth_;
            push_color(static_cast<style_color>(o.which), o.c);
            colors += m_->color_depth_ - before;
        } else {
            const u32 before = m_->var_depth_;
            push_var(static_cast<style_var>(o.which), o.v);
            vars += m_->var_depth_ - before;
        }
    }
    return {*this, colors, vars};
}

shape_style context::widget_shape(color base, f32 radius) const noexcept
{
    shape_style s;
    s.radius       = radii(radius);
    s.fill_top     = lighten(base, m_->style_.gradient);
    s.fill_bottom  = darken(base, m_->style_.gradient * 0.6f);
    s.border       = m_->style_.widget_border;
    s.border_width = m_->style_.border_width;
    return s;
}

// layout & input -----------------------------------------------------------

rect context::layout_place(vec2 size) noexcept
{
    layout_state& l = m_->layout_;
    const bool first_item = l.first;
    vec2 pos;
    if (l.same_line && !l.first) {
        pos      = {l.cursor_x + m_->style_.item_spacing, l.line_top};
        l.line_h = std::max(l.line_h, size.y);
    } else {
        pos        = {l.origin.x, l.first ? l.origin.y : l.line_top + l.line_h + m_->style_.item_spacing};
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

// rows of a long list or a deep tree are submitted whether or not they can be seen: the caller does not know where the
// view is. everything a row costs -- the hit test, an animation slot, measuring its text, its geometry -- is wasted on
// one that is scrolled out, and a tree with a few thousand open nodes pays all of it thousands of times. the layout has
// already been advanced by the time this is asked, so culling a row changes nothing about where anything sits: only the
// work disappears. the margin keeps a row that is half in view alive.
bool context::item_culled(const rect& r) noexcept
{
    ++m_->stats_cur_.items_submitted;
    // while a popup / drag preview is being measured off-screen nothing is drawn anyway, but the measuring pass needs
    // every item to report its size, so it must not be culled
    if (m_->dd_hidden_ || m_->gpopup_hidden_) {
        return false;
    }
    constexpr f32 margin = 2.0f;
    const rect&   view   = m_->dl_.clip();
    const bool    out    = r.max.y < view.min.y - margin || r.min.y > view.max.y + margin ||
                           r.max.x < view.min.x - margin || r.min.x > view.max.x + margin;
    m_->stats_cur_.items_culled += out ? 1u : 0u;
    return out;
}

void context::note_culled_item(id key, const rect& r) noexcept
{
    m_->last_item_key_     = key;
    m_->last_item_rect_    = r;
    m_->last_item_hovered_ = false;
    m_->last_item_focused_ = m_->nav_.scope != 0 && m_->nav_.cursor == key; // it is still the row the cursor is on
    m_->last_item_pressed_ = false;
    m_->last_item_double_  = false;
    m_->item_pressed_      = false;
}

// the same labels are measured again on every frame of every row; a direct-mapped cache of (font, text) -> size
// removes that walk. a collision simply overwrites the slot, so it never grows and never has to be swept.
vec2 context::measure_cached(font_id f, std::string_view s) noexcept
{
    if (s.empty()) {
        return m_->font_.measure(f, s);
    }
    if (m_->measure_cache_.size() != measure_cache_size || m_->measure_cache_gen_ != m_->font_generation_) {
        m_->measure_cache_.assign(measure_cache_size, measure_slot{});
        m_->measure_cache_gen_ = m_->font_generation_;
    }
    u64 h = 0xcbf29ce484222325ull ^ (static_cast<u64>(f) << 56);
    for (const char c : s) {
        h = (h ^ static_cast<u8>(c)) * 0x100000001b3ull;
    }
    h |= 1ull; // 0 marks an empty slot
    measure_slot& slot = m_->measure_cache_[static_cast<u32>(h >> 20) & (measure_cache_size - 1)];
    if (slot.key == h) {
        ++m_->stats_cur_.measure_hits;
        return slot.size;
    }
    ++m_->stats_cur_.text_measures;
    slot.key  = h;
    slot.size = m_->font_.measure(f, s);
    return slot.size;
}

// a control with an optional caption above it; the caption is drawn here
context::field_layout context::layout_field(std::string_view shown, f32 control_height)
{
    const font_id f = current_font();
    const f32 w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;

    const f32  lh      = shown.empty() ? m_->font_.line_height(f) : label_size(f, shown).y;
    const f32  label_h = shown.empty() ? 0.0f : lh + 3.0f;
    const rect all     = layout_place({w, label_h + control_height});

    field_layout out;
    out.has_label = !shown.empty();
    out.label_row = {all.min, {all.max.x, all.min.y + lh}};
    out.control   = {{all.min.x, all.min.y + label_h}, all.max};
    if (out.has_label) {
        label_draw({all.min.x + 1.0f, all.min.y}, m_->style_.text_dim, shown, f);
    }
    return out;
}

// widget ids ---------------------------------------------------------------

id context::widget_id(std::string_view label) noexcept
{
    const id key = hash_id(label, current_seed());
#ifndef NDEBUG
    // remember which label produced this id, so a collision on it can be named. the visible part only: "a##b" and
    // "a##c" are deliberately different ids and both report as "a", which is the wrong thing to blame, so the raw
    // string is what is kept.
    if (m_->id_labels_.size() != id_label_size) {
        m_->id_labels_.assign(id_label_size, id_label_slot{});
    }
    id_label_slot& slot = m_->id_labels_[key & (id_label_size - 1)];
    if (slot.key != key) {
        slot.key = key;
        const std::size_t n = std::min(label.size(), slot.text.size());
        std::copy_n(label.data(), n, slot.text.data());
        slot.len = static_cast<u8>(n);
    }
#endif
    return key;
}

void context::check_id([[maybe_unused]] id key) noexcept
{
#ifndef NDEBUG
    if (key == 0) {
        return;
    }
    if (m_->id_seen_.size() != id_seen_size) {
        m_->id_seen_.assign(id_seen_size, id{});
    }
    id& slot = m_->id_seen_[key & (id_seen_size - 1)];
    if (slot == key) {
        ++m_->stats_cur_.id_collisions;
        if (m_->collision_id_ == 0) { // the first one of the frame is the one worth naming
            m_->collision_id_ = key;
            m_->collision_label_len_ = 0;
            if (m_->id_labels_.size() == id_label_size) {
                const id_label_slot& ls = m_->id_labels_[key & (id_label_size - 1)];
                if (ls.key == key) {
                    const std::size_t n = std::min<std::size_t>(ls.len, m_->collision_label_.size());
                    std::copy_n(ls.text.data(), n, m_->collision_label_.data());
                    m_->collision_label_len_ = static_cast<u32>(n);
                }
            }
        }
        std::string_view label;
        if (m_->id_labels_.size() == id_label_size) {
            const id_label_slot& ls = m_->id_labels_[key & (id_label_size - 1)];
            if (ls.key == key) { label = {ls.text.data(), ls.len}; }
        }
        const std::string message = std::format("duplicate widget id \"{}\": give one of them a \"##suffix\" or push_id() around it", label);
        diagnose(diagnostic_kind::id_collision, message, key);
        return;
    }
    slot = key;
#endif
}

context::interaction context::interact(id key, const rect& r) noexcept
{
    // popup content ignores window hover (it extends past the window)
    return interact_impl(key, r, m_->in_overlay_ || (m_->cur_window_ != 0 && m_->hovered_window_prev_ == m_->cur_window_));
}

context::interaction context::interact_impl(id key, const rect& r, bool in_window) noexcept
{
    interaction out;
    check_id(key); // before the early-out: a widget hidden behind a transition still owns its id
    if (m_->dl_.alpha() < 0.1f) {
        return out; // (nearly) invisible content, e.g. mid page transition
    }

    // everything but popup content is blocked while the pointer is over an open popup
    const bool blocked   = !m_->in_overlay_ && ((m_->popup_open_prev_ && m_->popup_rect_prev_.contains(m_->mouse_)) || m_->menu_hit_prev_);
    const bool over      = in_window && !blocked && m_->dl_.clip().contains(m_->mouse_) && r.contains(m_->mouse_);

    // an item that offered its rectangle with allow_item_overlap() loses the press to anything submitted on top of
    // it. the press is exact (it happens in this same frame, before the offering item can act on it); the hover
    // highlight can only be taken away one frame late, because the item above has not been submitted yet when the
    // one below is drawn -- so that part goes by what covered it last frame.
    const bool ceded = m_->overlap_key_ != 0 && key != m_->overlap_key_ && m_->overlap_rect_.contains(m_->mouse_);
    if (ceded && over) {
        m_->overlap_taken_cur_ = m_->overlap_key_;
    }
    const bool suppressed = key != 0 && key == m_->overlap_taken_prev_;

    // a disabled item is still "the thing under the pointer", so a tooltip can explain why it cannot be used, but
    // nothing else happens to it: no hover highlight, no press, no keyboard
    if (m_->disabled_depth_ > 0) {
        m_->last_item_key_       = key;
        m_->last_item_rect_      = r;
        m_->last_item_hovered_   = over;
        m_->last_item_pressed_   = false;
        m_->last_item_double_    = false;
        m_->last_item_focused_   = false;
        // the pointer says so too: a disabled item that only fails to react is indistinguishable from a broken one
        if (over) { m_->cursor_ = cursor_kind::not_allowed; }
        if (over) { m_->hover_key_cur_ = key; }
        return out;
    }

    out.hovered = over && !suppressed && (m_->active_ == 0 || m_->active_ == key || (ceded && m_->active_ == m_->overlap_key_));
    if (out.hovered && m_->mouse_pressed_ && !m_->swallow_press_ && (m_->active_ == 0 || (ceded && m_->active_ == m_->overlap_key_))) {
        if (m_->active_ != 0) { m_->overlap_stolen_ = true; }
        m_->active_ = key;
        // a second press on the same spot within the double-click time; reported when the press completes below.
        // kept apart from register_click(), whose run of 1 / 2 / 3 belongs to the focused text field
        const vec2 moved  = m_->mouse_ - m_->item_click_pos_;
        m_->item_dbl_pending_ = key == m_->item_click_key_ && m_->time_ - m_->item_click_time_ < m_->double_click_ && dot(moved, moved) < 25.0f;
        m_->item_click_key_   = key;
        m_->item_click_time_  = m_->time_;
        m_->item_click_pos_   = m_->mouse_;
    }
    if (m_->active_ == key) {
        out.held = m_->mouse_down_;
        if (m_->mouse_released_) {
            out.pressed = over && !(m_->dd_active_ && m_->dd_source_ == key); // the release that ends a drag is not a click
            out.held    = false;
            m_->active_     = 0;
        }
    }
    m_->last_item_key_     = key;
    m_->last_item_rect_    = r;
    m_->last_item_hovered_ = out.hovered;
    m_->last_item_pressed_ = out.pressed;
    m_->last_item_double_  = out.pressed && m_->item_dbl_pending_;
    m_->last_item_focused_ = m_->nav_.scope != 0 && m_->nav_.cursor == key;
    if (out.hovered) {
        m_->hover_key_cur_ = key;
    }
    return out;
}

void context::begin_disabled(bool disabled) noexcept
{
    // nesting only tightens: an enabled scope inside a disabled one stays disabled
    const bool now = m_->disabled_depth_ > 0 || disabled;
    if (m_->disabled_count_ < max_disabled_depth) {
        m_->disabled_stack_[m_->disabled_count_++] = now;
    }
    if (!now) {
        return;
    }
    if (m_->disabled_depth_ == 0) {
        m_->dl_.push_alpha(0.45f);
        m_->disabled_alpha_ = true;
    }
    ++m_->disabled_depth_;
}

void context::end_disabled() noexcept
{
    if (m_->disabled_count_ == 0) {
        return;
    }
    if (!m_->disabled_stack_[--m_->disabled_count_] || m_->disabled_depth_ == 0) {
        return;
    }
    --m_->disabled_depth_;
    if (m_->disabled_depth_ == 0 && m_->disabled_alpha_) {
        m_->dl_.pop_alpha();
        m_->disabled_alpha_ = false;
    }
}

bool context::key_pressed(u32 virtual_key, bool ctrl, bool shift, bool alt) const noexcept
{
    if (virtual_key == 0 || m_->pressed_key_ != virtual_key || m_->hotkey_capture_ != 0) {
        return false;
    }
    if (m_->press_ctrl_ != ctrl || m_->press_shift_ != shift || m_->press_alt_ != alt) {
        return false;
    }
    // a text field owns the plain keys while it is being typed into, and its own Ctrl shortcuts
    if (want_text_input()) {
        if (!ctrl && !alt) { return false; }
        if (ctrl && !alt && !shift &&
            (virtual_key == 'A' || virtual_key == 'C' || virtual_key == 'V' || virtual_key == 'X' ||
             virtual_key == 'Z' || virtual_key == 'Y')) {
            return false;
        }
    }
    return true;
}

bool context::key_down(u32 virtual_key) const noexcept
{
    if (virtual_key >= 256 || want_text_input()) {
        return false;
    }
    return (m_->keys_held_[virtual_key >> 3] & (1u << (virtual_key & 7))) != 0;
}

bool context::mouse_down(int button) const noexcept
{
    switch (button) {
    case 1:  return m_->mouse_right_down_;
    case 2:  return m_->mouse_middle_down_;
    default: return m_->mouse_down_;
    }
}

bool context::mouse_clicked(int button) const noexcept
{
    switch (button) {
    case 1:  return m_->mouse_right_pressed_;
    case 2:  return m_->mouse_middle_pressed_;
    default: return m_->mouse_pressed_;
    }
}

bool context::mouse_released(int button) const noexcept
{
    return button == 0 && m_->mouse_released_;
}

bool context::window_focused() const noexcept
{
    if (m_->cur_window_ == 0) {
        return false;
    }
    // nothing has been clicked yet: the topmost window of the last frame has it
    return m_->key_window_ != 0 ? m_->key_window_ == m_->cur_window_ : m_->focused_window_ == m_->cur_window_;
}

bool context::is_window_focused(std::string_view title) const noexcept
{
    const id key = hash_id(title, 0);
    return m_->key_window_ != 0 ? m_->key_window_ == key : m_->focused_window_ == key;
}

rect context::content_rect() const noexcept
{
    context& self = *const_cast<context*>(this); // the lookup only reads
    rect     view;
    if (self.scroll_slot(view) != nullptr) {
        return view;
    }
    return m_->dl_.clip();
}

bool context::copy_text(std::string_view text) const
{
    if (m_->clipboard_.set == nullptr) {
        return false;
    }
    m_->clipboard_.set(m_->clipboard_.user, text);
    return true;
}

bool context::paste_text(std::string& out) const
{
    return m_->clipboard_.get != nullptr && m_->clipboard_.get(m_->clipboard_.user, out);
}

bool context::selection_click(selection_state& sel, int index) const
{
    if (m_->mod_shift_ && sel.anchor() >= 0) {
        const int from = sel.anchor();
        if (!m_->mod_ctrl_) { sel.clear(); }
        sel.add_range(std::min(from, index), std::max(from, index));
        sel.set_anchor(from); // the range keeps growing from where it started
        return true;
    }
    if (m_->mod_ctrl_) {
        sel.toggle(index);
        return true;
    }
    if (sel.size() == 1 && sel.contains(index)) {
        return false; // already the only one selected
    }
    sel.select_one(index);
    return true;
}

// where the next accessory of the row just submitted goes: from the right end of that row leftwards, inside the
// scrollbar inset. False when there is no row to hang it on, or no room left on it.
bool context::accessory_slot(f32 width, rect& out) noexcept
{
    if (m_->cur_ == nullptr || m_->row_anchor_ == 0) {
        return false;
    }
    if (m_->accessory_row_ != m_->row_anchor_) { // the first accessory of this row
        m_->accessory_row_      = m_->row_anchor_;
        m_->accessory_row_rect_ = m_->row_anchor_rect_;
        m_->accessory_x_        = std::min(m_->row_anchor_rect_.max.x, m_->layout_.origin.x + m_->layout_.width + m_->layout_.gutter);
        allow_item_overlap_at(m_->row_anchor_, m_->row_anchor_rect_); // the row lets go of the press where they sit
    }
    const rect& row = m_->accessory_row_rect_;
    const f32   h   = std::min(width, std::max(row.height() - 4.0f, 4.0f));
    const f32   x1  = m_->accessory_x_ - 2.0f;
    const f32   x0  = x1 - width;
    if (x0 < row.min.x) {
        return false; // the row is too narrow for one more
    }
    m_->accessory_x_ = x0;
    out = {{x0, row.min.y + (row.height() - h) * 0.5f}, {x1, row.min.y + (row.height() + h) * 0.5f}};
    return true;
}

void context::allow_item_overlap() noexcept
{
    allow_item_overlap_at(m_->last_item_key_, m_->last_item_rect_);
}

void context::allow_item_overlap_at(id key, const rect& r) noexcept
{
    m_->overlap_key_    = key;
    m_->overlap_rect_   = r;
    m_->overlap_stolen_ = false;
}

bool context::item_clicked(mouse_button b) const noexcept
{
    switch (b) {
    case mouse_button::right:  return m_->last_item_hovered_ && m_->mouse_right_pressed_;
    case mouse_button::middle: return m_->last_item_hovered_ && m_->mouse_middle_pressed_;
    default:                   return m_->last_item_pressed_;
    }
}

void context::push_right_gutter(f32 w) noexcept
{
    if (m_->gutter_depth_ >= max_gutter_depth) {
        report_limit("push_right_gutter nesting (max_gutter_depth)", max_gutter_depth);
        return;
    }
    w = std::clamp(w, 0.0f, std::max(m_->layout_.width - 16.0f, 0.0f));
    m_->gutter_stack_[m_->gutter_depth_++] = w;
    m_->layout_.width  -= w;
    m_->layout_.gutter += w;
}

void context::pop_right_gutter() noexcept
{
    if (m_->gutter_depth_ == 0) {
        return;
    }
    const f32 w = m_->gutter_stack_[--m_->gutter_depth_];
    m_->layout_.width  += w;
    m_->layout_.gutter -= w;
}

// a small square control laid into the right end of the row just submitted
bool context::row_accessory_button(font_id icon_font, std::string_view icon, std::string_view id_extra)
{
    rect box;
    const f32 side = frame_height() - 6.0f;
    if (!accessory_slot(side, box)) {
        return false;
    }
    const id key = hash_id(icon, hash_id(id_extra, hash_id("##acc", m_->accessory_row_)));
    const interaction in = interact(key, box);
    const press_anim  a  = button_anim(key, in);
    if (a.hover > 0.01f || a.active > 0.01f) {
        shape_style hot;
        hot.radius      = radii(m_->style_.rounding * 0.6f);
        hot.fill_top    = lerp(m_->style_.widget_hover, m_->style_.accent, a.active).scaled_alpha(0.25f + 0.6f * a.hover);
        hot.fill_bottom = hot.fill_top;
        m_->dl_.shape(box, hot);
    }
    const vec2 size = m_->font_.measure(icon_font, icon);
    m_->dl_.push_clip(box);
    m_->dl_.text({box.min.x + (box.width() - size.x) * 0.5f, box.min.y + (box.height() - size.y) * 0.5f},
             lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.active)), icon, icon_font);
    m_->dl_.pop_clip();
    return in.pressed;
}

bool context::row_accessory_checkbox(std::string_view id_extra, bool& value)
{
    rect box;
    const f32 side = std::min(frame_height() - 8.0f, 16.0f);
    if (!accessory_slot(side, box)) {
        return false;
    }
    const id key = hash_id(id_extra, hash_id("##accbox", m_->accessory_row_));
    const interaction in = interact(key, box);
    if (in.pressed) { value = !value; }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, value ? 1.0f : 0.0f);

    shape_style bx = widget_shape(lerp(m_->style_.widget_bg, m_->style_.accent, a.toggle), m_->style_.rounding * 0.5f);
    bx.border = lerp(m_->style_.widget_border, m_->style_.accent_hover, std::max(a.hover, a.toggle));
    m_->dl_.shape(box, bx);
    if (a.toggle > 0.05f) { // the tick, drawn as it appears
        const vec2 c = box.center();
        const f32  k = box.width() * 0.5f;
        const f32  t = a.toggle;
        m_->dl_.line({c.x - k * 0.45f, c.y}, {c.x - k * 0.1f, c.y + k * 0.35f}, m_->style_.text.scaled_alpha(t), 1.8f);
        m_->dl_.line({c.x - k * 0.1f, c.y + k * 0.35f}, {c.x + k * 0.5f, c.y - k * 0.4f}, m_->style_.text.scaled_alpha(t), 1.8f);
    }
    track_edit(key, in.pressed, m_->active_ == key);
    return in.pressed;
}

bool context::row_accessory_toggle(std::string_view id_extra, bool& value)
{
    rect box;
    const f32 h = std::min(frame_height() - 10.0f, 15.0f);
    if (!accessory_slot(h * 1.9f, box)) {
        return false;
    }
    const id key = hash_id(id_extra, hash_id("##accsw", m_->accessory_row_));
    const interaction in = interact(key, box);
    if (in.pressed) { value = !value; }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, value ? 1.0f : 0.0f);

    const f32   r  = box.height() * 0.5f;
    shape_style tr;
    tr.radius      = radii(r);
    tr.fill_top    = lerp(m_->style_.widget_bg, m_->style_.accent, a.toggle);
    tr.fill_bottom = tr.fill_top;
    tr.border      = lerp(m_->style_.widget_border, m_->style_.accent_hover, std::max(a.hover, a.toggle));
    tr.border_width = m_->style_.border_width;
    m_->dl_.shape(box, tr);
    const f32 kx = box.min.x + r + (box.width() - 2.0f * r) * a.toggle;
    m_->dl_.circle_filled({kx, box.center().y}, r - 2.5f, m_->style_.text);
    track_edit(key, in.pressed, m_->active_ == key);
    return in.pressed;
}

void context::skip_item(f32 height)
{
    if (m_->cur_ == nullptr || height <= 0.0f) {
        return;
    }
    (void)layout_place({0.0f, height});
}

void context::skip_items(int count, f32 item_height)
{
    if (m_->cur_ == nullptr || count <= 0) {
        return;
    }
    const f32 pitch = (item_height > 0.0f ? item_height : m_->font_.line_height(current_font())) + m_->style_.item_spacing;
    (void)layout_place({0.0f, static_cast<f32>(count) * pitch - m_->style_.item_spacing});
}

item_result context::custom_item(std::string_view label, vec2 size)
{
    return custom_item(label, {}, size);
}

item_result context::custom_item(std::string_view label, std::string_view id_extra, vec2 size)
{
    if (m_->cur_ == nullptr) {
        return {};
    }
    if (size.x <= 0.0f) {
        size.x = m_->layout_.width;
    }
    const id   key = id_extra.empty() ? widget_id(label) : hash_id(id_extra, widget_id(label));
    const rect r   = layout_place(size);
    if (item_culled(r)) {
        note_culled_item(key, r);
        note_row_anchor(key, r);
        return {r, false, false, false};
    }
    const interaction in = interact(key, r);
    note_row_anchor(key, r);
    return {r, in.hovered, in.held, in.pressed};
}

f32 context::label_clipped(vec2 pos, f32 max_width, color c, std::string_view s, font_id f)
{
    m_->last_item_truncated_ = false; // the answer is about this label, not about whatever came before it
    if (s.empty() || max_width <= 0.0f) {
        return 0.0f;
    }
    if (m_->rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
        const vec2 size = label_size(f, s);
        m_->dl_.push_clip({pos, {pos.x + max_width, pos.y + size.y}});
        label_draw(pos, c, s, f);
        m_->dl_.pop_clip();
        return std::min(size.x, max_width);
    }
    const f32 full = measure_cached(f, s).x;
    if (full <= max_width) {
        label_draw(pos, c, s, f);
        return full;
    }
    m_->last_item_truncated_ = true;

    constexpr std::string_view dots = "...";
    const f32 budget = max_width - measure_cached(f, dots).x;
    std::string_view rest = s;
    f32 used = 0.0f;
    std::size_t taken = 0;
    while (!rest.empty()) {
        const std::string_view before = rest;
        const char32_t cp = decode_utf8(rest);
        const f32 adv = m_->font_.advance(f, cp);
        if (used + adv > budget) { break; }
        used += adv;
        taken += before.size() - rest.size();
    }
    std::array<char, 256> buf;
    const std::size_t n = std::min(taken, buf.size() - dots.size());
    std::memcpy(buf.data(), s.data(), n);
    std::memcpy(buf.data() + n, dots.data(), dots.size());
    const std::string_view cut{buf.data(), n + dots.size()};
    label_draw(pos, c, cut, f);
    return used + (max_width - budget);
}

// windows ------------------------------------------------------------------

bool context::begin_window(std::string_view title, vec2 initial_pos, f32 width)
{
    return begin_window(title, initial_pos, vec2{width, 0.0f}, window_flags::none);
}

// one axis of a scrollbar thumb: `along` is the pointer on the track's axis, everything else measured along it too.
// the two wrappers below are the only difference between a vertical and a horizontal bar.
f32 context::thumb_drag_along(f32 along, const interaction& in, f32& grab, f32 thumb_lo, f32 thumb_len, f32 track_lo,
                              f32 travel, f32 max_scroll, f32 scroll) const noexcept
{
    if (!in.held || travel <= 0.0f) {
        return scroll;
    }
    if (m_->mouse_pressed_) {
        const bool on_thumb = along >= thumb_lo && along <= thumb_lo + thumb_len;
        grab = on_thumb ? along - thumb_lo : thumb_len * 0.5f;
    }
    return std::clamp((along - grab - track_lo) / travel, 0.0f, 1.0f) * max_scroll;
}

f32 context::thumb_drag(const interaction& in, f32& grab, f32 thumb_y, f32 thumb_h, f32 track_top, f32 travel,
                        f32 max_scroll, f32 scroll) const noexcept
{
    return thumb_drag_along(m_->mouse_.y, in, grab, thumb_y, thumb_h, track_top, travel, max_scroll, scroll);
}

f32 context::thumb_drag_x(const interaction& in, f32& grab, f32 thumb_x, f32 thumb_w, f32 track_left, f32 travel,
                          f32 max_scroll, f32 scroll) const noexcept
{
    return thumb_drag_along(m_->mouse_.x, in, grab, thumb_x, thumb_w, track_left, travel, max_scroll, scroll);
}

bool context::begin_window(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
{
    assert(m_->cur_ == nullptr && "nested windows are not supported");

    const id wid = hash_id(title, m_->id_stack_[0]);
    window_state* st = window_for(wid, initial_pos, size.x);
    if (st == nullptr) {
        return false;
    }
    if (st->content_h > 0.0f) { apply_pending_scroll(wid, st->scroll, nullptr); } // (once its content has been measured)
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
    st->last_frame = m_->frame_;
    const bool is_menubar   = std::exchange(m_->next_window_menubar_, false);
    const u32  modal_level  = std::exchange(m_->next_window_modal_level_, 0);
    st->menubar     = is_menubar;
    st->modal_level = modal_level;
    if (is_menubar) {
        st->pos    = {0.0f, 0.0f};
        st->width  = m_->display_.x;
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
        const dock_node& n = m_->dock_->nodes[st->dock - 1];
        if (n.used && n.leaf() && n.space < max_dock_spaces && m_->dock_->spaces[n.space].set) {
            node  = &n;
            space = &m_->dock_->spaces[n.space];
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
    m_->cur_flags_ = flags;

    push_id(title);
    m_->cur_        = st;
    m_->cur_window_ = wid;

    if (z_index(wid) == no_z) {
        bring_to_front(wid); // first appearance: on top
    }
    // press anywhere on the topmost window under the pointer raises it (docked windows stay in the background)
    if (m_->mouse_pressed_ && m_->hovered_window_prev_ == wid && (!docked || st->dock_owner != 0)) {
        bring_to_front(docked ? st->dock_owner : wid); // (a window in a floating dock raises the whole dock)
    }
    // ... and takes the keyboard, which docked windows do too even though they do not restack: a panel shortcut has
    // to be able to tell whether the user is looking at it
    if (m_->mouse_pressed_ && m_->hovered_window_prev_ == wid && !st->menubar && m_->modal_top_prev_ == 0) {
        m_->key_window_ = wid;
    }

    // every window's draw commands form their own run so the windows can be restacked
    if (m_->frame_window_count_ < max_windows) {
        m_->frame_windows_[m_->frame_window_count_] = st;
        switch_run(m_->frame_window_count_);
        ++m_->frame_window_count_;
    }

    const f32 lh      = m_->font_.line_height(0);
    const f32 title_h = has_title ? lh + 10.0f : 0.0f;
    const f32 pad     = is_menubar ? 0.0f : m_->style_.padding;
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
            st->pos += m_->mouse_delta_;
        }
    }
    const id   body_drag     = hash_id("##bodydrag", wid);
    const bool body_dragging = m_->active_ == body_drag && m_->mouse_down_ && can_move;
    if (body_dragging) {
        st->pos += m_->mouse_delta_;
    }
    if (!docked && m_->dock_->any_set && has_flag(flags, window_flags::dockable) && (drag_in.held || body_dragging)) {
        if (key_pressed(key::escape)) { m_->dock_->void_key = wid; } // Esc: this drag does not dock
        if (m_->dock_->void_key != wid && !m_->mod_shift_) {             // ... and neither does one with Shift held
            m_->dock_->drag_win   = wid;
            const f32 shown_h = st->height > 0.0f ? st->height : title_h + 2.0f * pad + st->content_h;
            m_->dock_->target_cur = dock_pick(m_->mouse_, no_node, {st->width, shown_h});
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

        if (right.hovered || right.held)   { m_->cursor_ = cursor_kind::resize_ew; }
        if (bottom.hovered || bottom.held) { m_->cursor_ = cursor_kind::resize_ns; }
        if (edge.hovered || edge.held)     { m_->cursor_ = cursor_kind::resize_nwse; }

        const f32 max_w = m_->display_.x > 0.0f ? m_->display_.x : 4096.0f;
        const f32 max_h = m_->display_.y > 0.0f ? m_->display_.y : 4096.0f;
        if (right.held || edge.held) {
            st->width = std::clamp(st->width + m_->mouse_delta_.x, 150.0f, max_w);
        }
        if (bottom.held || edge.held) {
            st->height = std::clamp(prev_h + m_->mouse_delta_.y, title_h + 48.0f, max_h); // a fixed height from now on
        }
    }

    if (!docked && m_->display_.x > 0 && m_->display_.y > 0) {
        st->pos.x = std::clamp(st->pos.x, 48.0f - st->width, m_->display_.x - 48.0f);
        st->pos.y = std::clamp(st->pos.y, 0.0f, m_->display_.y - std::max(title_h, 24.0f));
    }

    const bool fixed_h = st->height > 0.0f;
    // an auto-height window does not grow past the bottom of the display: it scrolls instead
    st->capped_h = 0.0f;
    if (!fixed_h && !docked && !st->menubar && !st->collapsed && m_->display_.y > 0.0f) {
        const f32 wanted = title_h + pad + st->content_h + pad;
        const f32 room   = std::max(m_->display_.y - st->pos.y - 8.0f, title_h + 48.0f);
        if (wanted > room) { st->capped_h = room; }
    }
    const bool scrolls = fixed_h || st->capped_h > 0.0f;
    const f32  height  = st->collapsed ? title_h
                       : fixed_h ? st->height
                       : scrolls ? st->capped_h
                                 : title_h + pad + st->content_h + pad;
    const rect frame   = rect::from_size(st->pos, {st->width, height});
    const rect bar     = rect::from_size(st->pos, {st->width, title_h});
    m_->cur_frame_ = frame;

    // a modal takes the input from everything else
    const bool input_blocked = m_->modal_top_prev_ != 0 && wid != m_->modal_top_prev_;
    if (frame.contains(m_->mouse_) && !hidden_tab && !input_blocked) {
        // a floating window beats a docked one, the menu bar beats both, a modal beats them all
        const bool in_float = docked && st->dock_owner != 0;
        const u32 rank = modal_level != 0 ? 0x30000u + modal_level * 0x10000u : (is_menubar ? 0x20000u : (docked && !in_float ? 0u : 0x10000u));
        u32 zi = z_index(in_float ? st->dock_owner : wid);
        if (zi == no_z) { zi = 0; }
        const u32 z = zi * 2 + (in_float ? 1u : 0u) + rank; // a window docked in a floating dock is just above that dock
        if (m_->hovered_z_ == no_z || z > m_->hovered_z_) {
            m_->hovered_window_cur_ = wid; // highest z under the pointer wins
            m_->hovered_z_          = z;
            m_->hovered_docked_cur_ = docked;
        }
    }

    // a window carried to a dock target goes see-through, so the pane it is aimed at shows through it
    st->ghost = approach(st->ghost, !docked && m_->dock_->drag_prev == wid && m_->dock_->target_prev.valid ? 1.0f : 0.0f);
    if (st->ghost > 0.01f) {
        m_->dl_.push_alpha(1.0f - 0.45f * st->ghost);
        m_->window_faded_ = true;
    }

    const f32 round = m_->style_.rounding;

    if (docked) {
        if (background && !hidden_tab) {
            m_->dl_.rect_filled(frame, m_->style_.window_bg);
        }
    } else if (background) {
        const bool acrylic = has_flag(flags, window_flags::acrylic);
        // body: fill + soft drop shadow in a single shader quad
        shape_style body;
        body.radius        = radii(round);
        body.fill_top      = m_->style_.window_bg;
        body.fill_bottom   = m_->style_.window_bg;
        body.shadow        = m_->style_.shadow;
        body.shadow_blur   = m_->style_.shadow_blur;
        body.shadow_offset = {0.0f, m_->style_.shadow_blur * 0.45f};
        if (acrylic) { // the shadow on its own, then the blurred frame with the tint on top
            body.fill_top = body.fill_bottom = color{0, 0, 0, 0};
            m_->dl_.shape(frame, body);
            m_->dl_.backdrop(frame, m_->style_.blur_radius, m_->style_.window_bg.scaled_alpha(m_->style_.acrylic_alpha), radii(round),
                         m_->style_.acrylic_noise, m_->style_.acrylic_saturation, m_->style_.acrylic_brightness);
        } else {
            m_->dl_.shape(frame, body);
        }

        if (has_title) {
            shape_style head;
            head.radius      = radii(round, st->collapsed ? corners::all : corners::top);
            head.fill_top    = lighten(m_->style_.title_bg, m_->style_.gradient * 0.8f);
            head.fill_bottom = m_->style_.title_bg;
            if (acrylic) {
                head.fill_top    = head.fill_top.scaled_alpha(0.55f);
                head.fill_bottom = head.fill_bottom.scaled_alpha(0.55f);
            }
            m_->dl_.shape(bar, head);
            if (!st->collapsed) {
                m_->dl_.rect_filled({{bar.min.x, bar.max.y - 1.0f}, bar.max}, m_->style_.border);
            }
        }

        shape_style edge;
        edge.radius       = radii(round);
        edge.border       = m_->style_.border;
        edge.border_width = m_->style_.border_width;
        m_->dl_.shape(frame, edge);
    }

    if (st->resizable && !st->collapsed) {
        const bool   hot = m_->cursor_ == cursor_kind::resize_nwse;
        const color  gc  = hot ? m_->style_.accent_hover : m_->style_.text_dim.scaled_alpha(0.55f);
        const vec2   br  = frame.max;
        for (f32 k = 0.0f; k < 3.0f; ++k) {
            const f32 d = 4.0f + k * 4.0f;
            m_->dl_.line({br.x - 3.0f, br.y - d - 1.0f}, {br.x - d - 1.0f, br.y - 3.0f}, gc, 1.5f);
        }
    }

    if (has_title) {
        if (can_collapse) {
            const vec2 c = arrow_hit.center();
            const f32  s = 4.0f;
            if (st->collapsed) {
                m_->dl_.triangle_filled({c.x - s * 0.5f, c.y - s}, {c.x + s * 0.75f, c.y}, {c.x - s * 0.5f, c.y + s}, m_->style_.text_dim);
            } else {
                m_->dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.75f}, m_->style_.text_dim);
            }
        }
        m_->dl_.text({st->pos.x + (can_collapse ? title_h : pad), st->pos.y + (title_h - lh) * 0.5f},
                 wid == m_->focused_window_ || m_->focused_window_ == 0 ? m_->style_.text : m_->style_.text_dim, visible_label(title), 0);
    }

    // content lives below the title bar; fixed-height windows scroll
    if (st->collapsed) {
        m_->dl_.push_clip(bar);
    } else if (hidden_tab) {
        m_->dl_.push_clip({frame.min, frame.min}); // an inactive tab: nothing of it is drawn
    } else {
        m_->dl_.push_clip({{frame.min.x, frame.min.y + title_h}, frame.max});
    }

    if (scrolls) {
        const f32 max_scroll = std::max(0.0f, st->content_h + 2.0f * pad - (height - title_h));
        st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);
    } else {
        st->scroll = 0.0f;
    }

    m_->layout_        = {};
    m_->layout_.origin = {st->pos.x + pad, st->pos.y + title_h + pad - st->scroll};
    m_->layout_.width  = st->width - 2.0f * pad - (scrolls && st->overflow ? 10.0f : 0.0f);
    m_->layout_.bound_bottom = fixed_h ? st->pos.y + height - pad : 0.0f;
    return !st->collapsed && !hidden_tab;
}

void context::end_window()
{
    if (m_->cur_ == nullptr) {
        return;
    }
    window_state& w = *m_->cur_;
    const bool has_title = !has_flag(m_->cur_flags_, window_flags::no_title_bar);
    if (!w.collapsed) {
        w.content_h = m_->layout_.first ? 0.0f : m_->layout_.bottom - m_->layout_.origin.y;

        const f32 shown_h = w.height > 0.0f ? w.height : w.capped_h;
        if (shown_h > 0.0f) {
            const f32 title_h = has_title ? m_->font_.line_height(0) + 10.0f : 0.0f;
            const f32 pad     = w.menubar ? 0.0f : m_->style_.padding;
            const f32 body_h  = shown_h - title_h;
            const f32 full_h  = w.content_h + 2.0f * pad;
            w.overflow = full_h > body_h + 0.5f;

            if (w.overflow) {
                const f32  max_scroll = full_h - body_h;
                const rect body = {{w.pos.x, w.pos.y + title_h}, {w.pos.x + w.width, w.pos.y + shown_h}};

                if (m_->wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(body)) {
                    w.scroll = std::clamp(w.scroll - wheel_scroll(m_->font_.line_height(0), body_h), 0.0f, max_scroll);
                    m_->wheel_consumed_ = true;
                }

                const f32  track_top = body.min.y + 6.0f;
                const f32  track_h = body_h - 12.0f;
                const f32  thumb_h = std::max(20.0f, track_h * body_h / full_h);
                f32 thumb_y = track_top + (track_h - thumb_h) * (w.scroll / max_scroll);
                const interaction in = interact(hash_id("##wscroll", m_->cur_window_), {{body.max.x - 13.0f, track_top}, {body.max.x - 2.0f, track_top + track_h}});
                w.scroll = thumb_drag(in, w.grab, thumb_y, thumb_h, track_top, track_h - thumb_h, max_scroll, w.scroll);
                thumb_y  = track_top + (track_h - thumb_h) * (w.scroll / max_scroll);
                const rect thumb = {{body.max.x - 10.0f, thumb_y}, {body.max.x - 5.0f, thumb_y + thumb_h}};
                shape_style bar;
                bar.radius      = radii(2.5f);
                bar.fill_top    = m_->style_.text_dim.scaled_alpha(in.hovered || in.held ? 0.85f : 0.45f);
                bar.fill_bottom = bar.fill_top;
                m_->dl_.shape(thumb, bar);
            }
        }
    }

    // an empty spot of a drag_by_body window starts moving the window
    if (has_flag(m_->cur_flags_, window_flags::drag_by_body) && !has_flag(m_->cur_flags_, window_flags::no_move) &&
        m_->mouse_pressed_ && m_->active_ == 0 && !m_->swallow_press_ && m_->hovered_window_prev_ == m_->cur_window_ &&
        m_->cur_frame_.contains(m_->mouse_) && !m_->menu_hit_prev_ && !(m_->popup_open_prev_ && m_->popup_rect_prev_.contains(m_->mouse_))) {
        m_->active_ = hash_id("##bodydrag", m_->cur_window_);
    }

    m_->dl_.pop_clip();
    if (m_->window_faded_) {
        m_->dl_.pop_alpha();
        m_->window_faded_ = false;
    }
    switch_run(run_base);
    pop_id();
    m_->cur_        = nullptr;
    m_->cur_window_ = 0;
}

// basic widgets ------------------------------------------------------------

void context::text_colored(color c, std::string_view s)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    if (m_->selectable_depth_ > 0 && !s.empty()) {
        m_->ml_color_ = c;
        text_selectable(s, s);
        return;
    }
    const font_id f = current_font();
    const vec2 size = label_size(f, s);
    const rect r    = layout_place(size);
    // text sharing a line with taller widgets is centered on that line
    const f32 dy = std::max(0.0f, (m_->layout_.line_h - size.y) * 0.5f);
    label_draw({r.min.x, r.min.y + dy}, c, s, f);
    // plain text is an item too, so item_hovered(), item_rect(), tooltip() and context_menu() work after it. It
    // takes no press, so nothing about clicking changes: it only becomes the thing those questions are about.
    note_passive_item(widget_id(s), r);
}

// registers a rectangle as "the last item" for the questions that follow it, without taking the press
void context::note_passive_item(id key, const rect& r) noexcept
{
    const bool over = m_->cur_window_ != 0 && m_->hovered_window_prev_ == m_->cur_window_ && m_->active_ == 0 &&
                      !(m_->popup_open_prev_ && m_->popup_rect_prev_.contains(m_->mouse_)) && !m_->menu_hit_prev_ &&
                      m_->dl_.clip().contains(m_->mouse_) && r.contains(m_->mouse_);
    m_->last_item_key_       = key;
    m_->last_item_rect_      = r;
    m_->last_item_hovered_   = over && m_->dl_.alpha() >= 0.1f;
    m_->last_item_pressed_   = false;
    m_->last_item_double_    = false;
    m_->last_item_focused_   = false;
    m_->last_item_arrow_     = false;
    m_->last_item_truncated_ = false;
    if (m_->last_item_hovered_) { m_->hover_key_cur_ = key; }
}

bool context::button(std::string_view label)
{
    return button(label, {});
}

bool context::button(std::string_view label, std::string_view id_extra)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = id_extra.empty() ? widget_id(label) : hash_id(id_extra, widget_id(label));
    const std::string_view shown = visible_label(label);

    const vec2 tsize = label_size(f, shown);
    f32 w = tsize.x + m_->style_.frame_padding.x * 2.0f;
    if (m_->layout_.next_width > 0.0f) {
        w = m_->layout_.next_width;
        m_->layout_.next_width = 0.0f;
    }

    const rect r = layout_place({w, std::max(frame_height(), tsize.y + m_->style_.frame_padding.y * 2.0f)});
    const interaction in = interact(key, r);

    const press_anim a = button_anim(key, in);

    const color base = lerp(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.accent, a.active);
    shape_style s = widget_shape(base, m_->style_.rounding * 0.8f);
    s.border = lerp(m_->style_.widget_border, m_->style_.accent_hover, std::max(a.hover * 0.55f, a.active));
    m_->dl_.shape(r, s);
    label_draw({r.min.x + (r.width() - tsize.x) * 0.5f, r.min.y + (r.height() - tsize.y) * 0.5f}, m_->style_.text, shown, f);
    return in.pressed;
}

bool context::icon_button(font_id icon_font, std::string_view icon, std::string_view label)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = hash_id(icon, widget_id(label));
    const std::string_view shown = visible_label(label);

    const vec2 isize = m_->font_.measure(icon_font, icon);
    const vec2 tsize = shown.empty() ? vec2{} : label_size(f, shown);
    const f32  gap   = shown.empty() ? 0.0f : 7.0f;
    const f32  inner = isize.x + gap + tsize.x;

    f32 w = inner + m_->style_.frame_padding.x * 2.0f;
    if (m_->layout_.next_width > 0.0f) {
        w = m_->layout_.next_width;
        m_->layout_.next_width = 0.0f;
    }
    const rect r = layout_place({w, frame_height()});
    const interaction in = interact(key, r);

    const press_anim a = button_anim(key, in);

    const color base = lerp(lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover), m_->style_.accent, a.active);
    shape_style s = widget_shape(base, m_->style_.rounding * 0.8f);
    s.border = lerp(m_->style_.widget_border, m_->style_.accent_hover, std::max(a.hover * 0.55f, a.active));
    m_->dl_.shape(r, s);

    const f32 x = r.min.x + (r.width() - inner) * 0.5f;
    m_->dl_.text({x, r.min.y + (r.height() - isize.y) * 0.5f}, lerp(m_->style_.text, m_->style_.accent_hover, a.hover * 0.6f), icon, icon_font);
    if (!shown.empty()) {
        label_draw({x + isize.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, m_->style_.text, shown, f);
    }
    return in.pressed;
}

void context::icon_label(font_id icon_font, std::string_view icon, std::string_view label)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    const vec2 isize = m_->font_.measure(icon_font, icon);
    const vec2 tsize = label_size(f, label);
    const f32  gap   = 7.0f;
    const f32  h     = std::max(isize.y, tsize.y);
    const rect r     = layout_place({isize.x + gap + tsize.x, h});
    m_->dl_.text({r.min.x, r.min.y + (h - isize.y) * 0.5f}, m_->style_.accent_hover, icon, icon_font);
    label_draw({r.min.x + isize.x + gap, r.min.y + (h - tsize.y) * 0.5f}, m_->style_.text, label, f);
}

bool context::checkbox(std::string_view label, bool& value)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    const std::string_view shown = visible_label(label);

    const f32  box  = frame_height() - 6.0f;
    const vec2 tsize = label_size(f, shown);
    const f32  gap  = shown.empty() ? 0.0f : m_->style_.item_spacing + 2.0f;

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

    const color idle = lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover);
    shape_style s = widget_shape(lerp(idle, m_->style_.accent, a.toggle), m_->style_.rounding * 0.6f);
    s.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.5f), m_->style_.accent_hover, a.toggle);
    m_->dl_.shape(bx, s);

    if (a.toggle > 0.01f) {
        const color mark = color{255, 255, 255, 255}.scaled_alpha(a.toggle);
        const vec2 p0{bx.min.x + box * 0.24f, bx.min.y + box * 0.52f};
        const vec2 p1{bx.min.x + box * 0.43f, bx.min.y + box * 0.72f};
        const vec2 p2{bx.min.x + box * 0.77f, bx.min.y + box * 0.30f};
        m_->dl_.line(p0, p1, mark, 2.0f);
        m_->dl_.line(p1, p2, mark, 2.0f);
    }

    label_draw({bx.max.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, m_->style_.text, shown, f);
    track_edit(key, changed, m_->active_ == key);
    return changed;
}

bool context::toggle(std::string_view label, bool& value)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    const std::string_view shown = visible_label(label);

    const f32  track_h = frame_height() - 6.0f;
    const f32  track_w = track_h * 1.85f;
    const vec2 tsize   = label_size(f, shown);
    const f32  gap     = shown.empty() ? 0.0f : m_->style_.item_spacing + 2.0f;

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
    a.toggle = approach(a.toggle, value ? 1.0f : 0.0f, m_->style_.anim_speed * 0.75f);

    const f32   t    = smooth(std::clamp(a.toggle, 0.0f, 1.0f));
    const color idle = lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover);
    shape_style ts = widget_shape(lerp(idle, m_->style_.accent, t), track_h * 0.5f);
    ts.border = lerp(m_->style_.widget_border, m_->style_.accent_hover, std::max(t, a.hover * 0.5f));
    m_->dl_.shape(track, ts);

    const f32  knob_r = track_h * 0.5f - 3.0f;
    const f32  x0     = track.min.x + track_h * 0.5f;
    const f32  x1     = track.max.x - track_h * 0.5f;
    const vec2 kc{x0 + (x1 - x0) * t, track.center().y};
    shape_style ks;
    ks.radius        = radii(knob_r);
    ks.fill_top      = color{255, 255, 255, 255};
    ks.fill_bottom   = color{222, 228, 244, 255};
    ks.shadow        = m_->style_.shadow_blur > 0.0f ? color{0, 0, 0, 110} : color{0, 0, 0, 0};
    ks.shadow_blur   = 5.0f;
    ks.shadow_offset = {0.0f, 1.5f};
    m_->dl_.shape({{kc.x - knob_r, kc.y - knob_r}, {kc.x + knob_r, kc.y + knob_r}}, ks);

    label_draw({track.max.x + gap, r.min.y + (r.height() - tsize.y) * 0.5f}, m_->style_.text, shown, f);
    track_edit(key, changed, m_->active_ == key);
    return changed;
}

bool context::slider_f32(std::string_view label, f32& value, f32 lo, f32 hi, int decimals)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    const std::string_view shown = visible_label(label);

    constexpr f32 knob_r = 8.0f;
    const field_layout fl = layout_field(shown, knob_r * 2.0f + 6.0f);

    std::array<char, 32> buf;
    const std::string_view vtext = format_fixed(buf, value, decimals);
    const vec2 vsize = m_->font_.measure(f, vtext);

    // without a caption the value sits to the right of the track instead of above it
    rect area = fl.control;
    if (!fl.has_label) {
        area.max.x -= vsize.x + m_->style_.item_spacing + 2.0f;
    }

    const f32 x0 = area.min.x + knob_r;
    const f32 x1 = area.max.x - knob_r;
    const f32 cy = area.center().y;
    const interaction in = interact(key, area);

    bool changed = false;
    if (in.held && hi > lo && x1 > x0) {
        const f32 t    = std::clamp((m_->mouse_.x - x0) / (x1 - x0), 0.0f, 1.0f);
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
    track.fill_top     = darken(m_->style_.widget_bg, 0.22f);
    track.fill_bottom  = m_->style_.widget_bg;
    track.border       = m_->style_.widget_border;
    track.border_width = m_->style_.border_width;
    m_->dl_.shape({{area.min.x, cy - half_track}, {area.max.x, cy + half_track}}, track);

    if (kx > area.min.x + 1.0f) {
        shape_style fill;
        fill.radius      = radii(half_track);
        fill.fill_top    = lighten(m_->style_.accent, m_->style_.gradient * 1.5f);
        fill.fill_bottom = darken(m_->style_.accent, m_->style_.gradient);
        m_->dl_.shape({{area.min.x, cy - half_track}, {kx, cy + half_track}}, fill);
    }

    const f32 kr = knob_r + a.active * 1.5f;
    shape_style knob;
    knob.radius        = radii(kr);
    knob.fill_top      = color{255, 255, 255, 255};
    knob.fill_bottom   = color{224, 230, 246, 255};
    knob.border        = lerp(m_->style_.widget_border, m_->style_.accent, a.hover);
    knob.border_width  = 1.0f + a.hover * 0.75f;
    knob.shadow        = lerp(m_->style_.shadow_blur > 0.0f ? color{0, 0, 0, 120} : color{0, 0, 0, 0},
                              m_->style_.accent.scaled_alpha(0.55f), a.hover);
    knob.shadow_blur   = 5.0f + 5.0f * a.hover;
    knob.shadow_offset = {0.0f, 1.5f * (1.0f - a.hover)};
    m_->dl_.shape({{kx - kr, cy - kr}, {kx + kr, cy + kr}}, knob);

    const color vcolor = lerp(m_->style_.text_dim, m_->style_.text, a.hover);
    if (fl.has_label) {
        m_->dl_.text({fl.label_row.max.x - vsize.x - 1.0f, fl.label_row.min.y}, vcolor, vtext, f);
    } else {
        m_->dl_.text({fl.control.max.x - vsize.x, cy - vsize.y * 0.5f}, vcolor, vtext, f);
    }
    track_edit(key, changed, m_->active_ == key);
    return changed;
}

void context::progress_bar(f32 fraction, std::string_view overlay)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    const font_id f = current_font();
    fraction = std::clamp(fraction, 0.0f, 1.0f);

    const f32  w = m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width;
    m_->layout_.next_width = 0.0f;
    const f32  h = m_->font_.line_height(f) + 6.0f;
    const rect r = layout_place({w, h});

    m_->dl_.shape(r, widget_shape(m_->style_.widget_bg, h * 0.5f));

    if (fraction > 0.0f) {
        shape_style fill;
        fill.radius       = radii(h * 0.5f);
        fill.fill_top     = lighten(m_->style_.accent, m_->style_.gradient * 1.5f);
        fill.fill_bottom  = darken(m_->style_.accent, m_->style_.gradient);
        fill.border       = m_->style_.accent_hover;
        fill.border_width = m_->style_.border_width;
        m_->dl_.shape({r.min, {r.min.x + std::max(r.width() * fraction, h * 0.6f), r.max.y}}, fill);
    }

    std::array<char, 32> buf;
    std::string_view label = overlay;
    if (label.empty()) {
        const auto res = std::to_chars(buf.data(), buf.data() + buf.size() - 1, static_cast<int>(fraction * 100.0f + 0.5f));
        *res.ptr = '%';
        label = {buf.data(), res.ptr + 1};
    }
    const vec2 tsize = label_size(f, label);
    label_draw({r.min.x + (r.width() - tsize.x) * 0.5f, r.min.y + (r.height() - tsize.y) * 0.5f}, m_->style_.text, label, f);
}

void context::separator()
{
    if (m_->cur_ == nullptr) {
        return;
    }
    const rect r = layout_place({m_->layout_.width, 1.0f});
    m_->dl_.rect_filled(r, m_->style_.border);
}

void context::spacing(f32 height)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    (void)layout_place({0.0f, height > 0.0f ? height : m_->style_.item_spacing});
}

// text input ---------------------------------------------------------------

bool context::input_text(std::string_view label, std::string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(m_->edit_buf_.data(), m_->edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_text(std::string_view label, secure_string& value, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (input_core(label, value, hint, flags, max_bytes)) {
        value.assign(m_->edit_buf_.data(), m_->edit_buf_.size());
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
    const std::size_t n = std::min(m_->edit_buf_.size(), capacity - 1);
    std::copy_n(m_->edit_buf_.data(), n, buffer);
    buffer[n] = '\0';
    return true;
}

bool context::input_core(std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
                         std::size_t max_bytes)
{
    if (m_->cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = current_font();
    f32           lh       = m_->font_.line_height(fnt);
    f32           asc      = m_->font_.ascent(fnt);
    const bool    password = has_flag(flags, input_flags::password);
    const bool    reveal_btn = password && has_flag(flags, input_flags::reveal);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const id      key      = widget_id(label);
    ed_prepare_spans(m_->focus_id_ == key ? m_->edit_buf_.size() : current.size(), fnt, lh, asc, password);

    field_layout fl;
    if (m_->input_rect_set_) { // a widget that owns the box (number fields): no caption, no layout
        m_->input_rect_set_ = false;
        fl.control = m_->input_rect_;
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
        if (m_->mouse_pressed_ && bi.held) { m_->press_claimed_ = true; } // the field keeps the keyboard if it had it
        revealed   = ra.active > 0.5f;
        reveal_hot = bi.hovered;
    }
    const bool hide = password && !revealed; // what is drawn as bullets

    // a clear button: an x at the right end while the field has text. like the eye it is tested before the field, so
    // it takes the press instead of putting the caret somewhere
    const bool has_text  = m_->focus_id_ == key ? !m_->edit_buf_.empty() : !current.empty();
    const bool clear_btn = has_flag(flags, input_flags::clear_button) && !readonly && has_text;
    const f32  clear_w   = clear_btn ? 22.0f : 0.0f;
    const rect clear_r   = {{box.max.x - reveal_w - clear_w - 2.0f, box.min.y + 2.0f},
                            {box.max.x - reveal_w - 3.0f, box.max.y - 2.0f}};
    bool       clear_hot = false;
    bool       cleared   = false;
    if (clear_btn) {
        const interaction bi = interact(hash_id("##clear", key), clear_r);
        clear_hot = bi.hovered;
        cleared   = bi.pressed;
        if (m_->mouse_pressed_ && bi.held) { m_->press_claimed_ = true; } // the field keeps the keyboard if it had it
    }

    const interaction  in  = interact(key, box);
    if (in.hovered || (in.held && m_->focus_id_ == key)) { m_->cursor_ = reveal_hot ? cursor_kind::arrow : cursor_kind::text; }

    const f32  pad_x  = m_->style_.frame_padding.x;
    const rect inner  = {{box.min.x + pad_x, box.min.y}, {box.max.x - pad_x - reveal_w - clear_w, box.max.y}};
    const f32  text_y = box.min.y + (box.height() - lh) * 0.5f;

    // what is shown: the live edit buffer while focused, the caller's text otherwise
    bool focused = m_->focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{m_->edit_buf_} : current; };

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
            bidi.build(m_->font_, fnt, t);
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
        return m_->font_.measure(fnt, std::string_view{masked}.substr(0, count_codepoints(t.substr(0, byte_index)))).x;
    };
    const auto index_at = [&](f32 mouse_x) {
        const std::string_view t = text_now();
        const f32 rel = mouse_x - inner.min.x + m_->edit_scroll_;
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
    if (m_->focus_request_ == key) { // asked for by the code (a find bar that opens): like a click on the field, all selected
        m_->focus_request_ = 0;
        m_->press_claimed_ = true;
        if (m_->focus_id_ != key) {
            m_->focus_id_    = key;
            focused      = true;
            wipe_edit_buffer();
            m_->edit_buf_.assign(current);
            m_->edit_scroll_ = 0.0f;
            m_->edit_cursor_ = m_->edit_buf_.size();
            m_->edit_anchor_ = 0;
            m_->caret_time_  = m_->time_;
            rebuild_mask();
        }
    }
    const bool press_here = m_->mouse_pressed_ && in.held;
    if (press_here) {
        m_->press_claimed_ = true;
        if (m_->focus_id_ != key) {
            m_->focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(); // nothing of the previous field's text may stay behind
            m_->edit_buf_.assign(current);
            m_->edit_scroll_ = 0.0f;
            m_->edit_cursor_ = m_->edit_buf_.size();
            m_->edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : m_->edit_cursor_;
            m_->caret_time_  = m_->time_;
            rebuild_mask();
        }

        const std::size_t idx = index_at(m_->mouse_.x);
        const u32 clicks = register_click();
        if (clicks == 3) { // triple click: the whole line (a single-line field is one line)
            m_->edit_anchor_ = 0;
            m_->edit_cursor_ = m_->edit_buf_.size();
        } else if (clicks == 2) {
            m_->edit_anchor_ = word_start(m_->edit_buf_, idx);
            m_->edit_cursor_ = word_end(m_->edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || m_->edit_cursor_ != 0) {
            m_->edit_cursor_ = idx;
            m_->edit_anchor_ = idx;
        }
        m_->caret_time_ = m_->time_;
    } else if (focused && in.held) {
        m_->edit_cursor_ = index_at(m_->mouse_.x);
        m_->caret_time_  = m_->time_;
    }
    if (focused) {
        m_->focus_seen_ = true;
    }

    // --- keyboard ----------------------------------------------------------------
    bool changed = false;
    if (cleared) { // empties the field whether or not it has the keyboard
        m_->edit_buf_.clear();
        m_->edit_cursor_ = 0;
        m_->edit_anchor_ = 0;
        m_->edit_scroll_ = 0.0f;
        edit_history_clear();
        ++m_->edit_version_;
        changed = true;
    }
    if (focused) {
        m_->edit_readonly_   = readonly;
        m_->edit_max_bytes_  = max_bytes;
        m_->edit_history_on_ = !password && !readonly && m_->edit_mask_.empty();

        if (m_->typed_len_ != 0) {
            changed = edit_insert({m_->typed_.data(), m_->typed_len_}, true) || changed;
            m_->typed_len_  = 0;
            m_->caret_time_ = m_->time_;
        }

        for (u32 i = 0; i < m_->key_count_ && focused; ++i) {
            const key_event& ev = m_->keys_[i];
            if (ev.alt) { continue; } // Alt + key is a shortcut of the host, not editing
            const std::string_view t = m_->edit_buf_;
            const bool has_sel = m_->edit_cursor_ != m_->edit_anchor_;
            m_->caret_time_ = m_->time_;
            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_cursor_ = m_->edit_anchor_ = std::min(m_->edit_cursor_, m_->edit_anchor_);
                } else {
                    m_->edit_cursor_ = ev.ctrl ? prev_word(t, m_->edit_cursor_) : prev_boundary(t, m_->edit_cursor_);
                    if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_cursor_ = m_->edit_anchor_ = std::max(m_->edit_cursor_, m_->edit_anchor_);
                } else {
                    m_->edit_cursor_ = ev.ctrl ? next_word(t, m_->edit_cursor_) : next_boundary(t, m_->edit_cursor_);
                    if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                }
                break;
            case key::home:
                m_->edit_cursor_ = 0;
                if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                break;
            case key::end:
                m_->edit_cursor_ = t.size();
                if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, m_->edit_cursor_) : prev_boundary(t, m_->edit_cursor_);
                        if (a < m_->edit_cursor_) {
                            changed = edit_replace(a, m_->edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, m_->edit_cursor_) : next_boundary(t, m_->edit_cursor_);
                        if (b > m_->edit_cursor_) {
                            changed = edit_replace(m_->edit_cursor_, b - m_->edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                m_->submitted_ = true;
                break;
            case key::escape:
            case key::tab:
                m_->focus_id_ = 0;
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
        m_->key_count_ = 0;
        if (changed && !m_->edit_mask_.empty()) { apply_input_mask(); }
        rebuild_mask();
    }

    if (focused) {
        const f32 view_w  = std::max(inner.width(), 1.0f);
        const bool composing_now = !readonly && !password && m_->ime_len_ != 0;
        const std::string_view comp{m_->ime_text_.data(), m_->ime_len_};
        const f32 caret_x = prefix_width(m_->edit_cursor_) + (composing_now ? m_->font_.measure(fnt, comp.substr(0, m_->ime_cursor_)).x : 0.0f);
        if (caret_x - m_->edit_scroll_ > view_w - 2.0f) { m_->edit_scroll_ = caret_x - view_w + 2.0f; }
        if (caret_x - m_->edit_scroll_ < 0.0f)          { m_->edit_scroll_ = caret_x; }
        const bidi_layout* rtl_now = rtl_layout();
        const f32 total = (rtl_now != nullptr ? rtl_now->width() : prefix_width(text_now().size())) + (composing_now ? m_->font_.measure(fnt, comp).x : 0.0f);
        if (total - m_->edit_scroll_ < view_w - 2.0f)   { m_->edit_scroll_ = std::max(0.0f, total - view_w + 2.0f); }
    }

    // --- drawing -----------------------------------------------------------------
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, focused ? 1.0f : 0.0f);

    shape_style field;
    field.radius       = radii(m_->style_.rounding * 0.8f);
    field.fill_top     = darken(m_->style_.widget_bg, 0.28f);
    field.fill_bottom  = darken(m_->style_.widget_bg, 0.12f);
    field.border       = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    field.border_width = 1.0f + a.toggle * 0.5f;
    field.shadow       = m_->style_.accent.scaled_alpha(0.32f * a.toggle);
    field.shadow_blur  = 9.0f * a.toggle;
    m_->dl_.shape(box, field);

    if (clear_btn) { // a small x
        if (clear_hot) {
            shape_style hot;
            hot.radius      = radii(m_->style_.rounding * 0.6f);
            hot.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m_->dl_.shape(clear_r, hot);
        }
        const vec2  c   = clear_r.center();
        const color col = clear_hot ? m_->style_.text : m_->style_.text_dim;
        constexpr f32 arm = 4.0f;
        m_->dl_.line({c.x - arm, c.y - arm}, {c.x + arm, c.y + arm}, col, 1.4f);
        m_->dl_.line({c.x - arm, c.y + arm}, {c.x + arm, c.y - arm}, col, 1.4f);
    }

    if (reveal_btn) { // the eye: open when the text is shown, crossed out when it is hidden
        if (reveal_hot) {
            shape_style hot;
            hot.radius      = radii(m_->style_.rounding * 0.6f);
            hot.fill_top    = m_->style_.widget_hover.scaled_alpha(0.8f);
            hot.fill_bottom = hot.fill_top;
            m_->dl_.shape(reveal_r, hot);
        }
        const vec2  c   = reveal_r.center();
        const color col = lerp(m_->style_.text_dim, m_->style_.text, reveal_hot || revealed ? 1.0f : 0.0f);
        m_->dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y - 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m_->dl_.bezier_quadratic({c.x - 7.0f, c.y}, {c.x, c.y + 8.0f}, {c.x + 7.0f, c.y}, col, 1.3f);
        m_->dl_.circle_filled(c, 2.2f, col);
        if (!revealed) { m_->dl_.line({c.x - 6.0f, c.y + 6.0f}, {c.x + 6.0f, c.y - 6.0f}, col, 1.5f); }
    }

    m_->dl_.push_clip({{inner.min.x, box.min.y + 1.0f}, {inner.max.x, box.max.y - 1.0f}});
    const f32 text_x = inner.min.x - (focused ? m_->edit_scroll_ : 0.0f);

    const std::string_view shown_text = text_now();
    const bool composing = focused && !readonly && !password && m_->ime_len_ != 0;
    const std::string_view comp{m_->ime_text_.data(), m_->ime_len_};
    std::string disp; // the text with the composition inserted at the caret
    if (composing) {
        const std::size_t at = std::min(m_->edit_cursor_, shown_text.size());
        disp.assign(shown_text.substr(0, at));
        disp.append(comp);
        disp.append(shown_text.substr(at));
    }
    const auto disp_width = [&](std::size_t n) { return m_->font_.measure(fnt, std::string_view{disp}.substr(0, n)).x; };
    if (focused && m_->edit_cursor_ != m_->edit_anchor_ && !composing) {
        const std::size_t lo = std::min(m_->edit_cursor_, m_->edit_anchor_);
        const std::size_t hi = std::max(m_->edit_cursor_, m_->edit_anchor_);
        shape_style sel;
        sel.radius      = radii(2.0f);
        sel.fill_top    = m_->style_.accent.scaled_alpha(0.45f);
        sel.fill_bottom = m_->style_.accent.scaled_alpha(0.45f);
        const f32 xa = prefix_width(lo);
        const f32 xb = prefix_width(hi);
        m_->dl_.shape({{text_x + std::min(xa, xb), text_y - 1.0f}, {text_x + std::max(xa, xb), text_y + lh + 1.0f}}, sel);
    }

    if (shown_text.empty() && !focused && !hint.empty()) {
        m_->dl_.text({text_x, text_y}, m_->style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    } else if (hide) {
        m_->dl_.text({text_x, text_y}, m_->style_.text, masked, fnt);
    } else if (composing) { // the composition sits in the text, underlined and lightly marked
        const std::size_t at = std::min(m_->edit_cursor_, shown_text.size());
        const f32 x0 = text_x + disp_width(at);
        const f32 x1 = text_x + disp_width(at + comp.size());
        m_->dl_.rect_filled({{x0, text_y - 1.0f}, {x1, text_y + lh + 1.0f}}, m_->style_.accent.scaled_alpha(0.18f));
        m_->dl_.text({text_x, text_y}, m_->style_.text, disp, fnt);
        m_->dl_.rect_filled({{x0, text_y + lh - 1.0f}, {x1, text_y + lh + 0.5f}}, m_->style_.accent_hover);
    } else if (!m_->edit_spans_.empty()) {
        ed_draw({text_x, text_y}, asc, m_->style_.text, shown_text, 0, shown_text.size(), fnt);
    } else {
        m_->dl_.text({text_x, text_y}, m_->style_.text, shown_text, fnt);
    }

    if (focused) { // where the input method puts its candidate window
        const f32 caret_x = composing ? text_x + disp_width(std::min(m_->edit_cursor_, shown_text.size()) + m_->ime_cursor_)
                                      : text_x + prefix_width(m_->edit_cursor_);
        m_->ime_want_   = !password; // (a password field turns the input method off)
        m_->ime_pos_    = {caret_x * m_->scale_, (text_y + lh) * m_->scale_};
        m_->ime_line_h_ = lh * m_->scale_;
        if (caret_visible()) {
            const f32 cx = std::round(caret_x);
            m_->dl_.rect_filled({{cx, text_y}, {cx + 1.5f, text_y + lh}}, m_->style_.text);
        }
    }
    m_->dl_.pop_clip();

    track_edit(key, changed, m_->focus_id_ == key);
    return changed;
}

// combo box ----------------------------------------------------------------

bool context::combo(std::string_view label, int& current, const std::string_view* items, std::size_t count)
{
    if (m_->cur_ == nullptr || count == 0) {
        return false;
    }
    const font_id f = current_font();
    const id key = widget_id(label);
    current = std::clamp(current, 0, static_cast<int>(count) - 1);

    const field_layout fl  = layout_field(visible_label(label), frame_height());
    const rect         box = fl.control;
    const interaction  in  = interact(key, box);

    bool open = m_->popup_id_ == key;
    if (in.pressed) {
        if (open) {
            m_->popup_id_ = 0;
            open      = false;
        } else {
            m_->popup_id_     = key;
            m_->popup_scroll_ = 0.0f;
            m_->popup_hover_  = current;
            open          = true;
        }
    }

    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, open ? 1.0f : 0.0f);

    const color base = lerp(m_->style_.widget_bg, m_->style_.widget_hover, a.hover);
    shape_style field = widget_shape(base, m_->style_.rounding * 0.8f);
    field.border = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
    m_->dl_.shape(box, field);

    const std::string_view shown = items[current];
    const vec2 tsize = label_size(f, shown);
    m_->dl_.push_clip({{box.min.x, box.min.y}, {box.max.x - m_->style_.frame_padding.x - 14.0f, box.max.y}});
    label_draw({box.min.x + m_->style_.frame_padding.x, box.min.y + (box.height() - tsize.y) * 0.5f}, m_->style_.text, shown, f);
    m_->dl_.pop_clip();

    const vec2 c{box.max.x - m_->style_.frame_padding.x - 4.0f, box.center().y};
    const f32  s = 4.0f;
    const color chev = lerp(m_->style_.text_dim, m_->style_.text, std::max(a.hover, a.toggle));
    if (open) {
        m_->dl_.triangle_filled({c.x - s, c.y + s * 0.5f}, {c.x, c.y - s * 0.6f}, {c.x + s, c.y + s * 0.5f}, chev);
    } else {
        m_->dl_.triangle_filled({c.x - s, c.y - s * 0.5f}, {c.x + s, c.y - s * 0.5f}, {c.x, c.y + s * 0.6f}, chev);
    }

    bool changed = false;
    if (open) {
        draw_combo_popup(key, box, items, count, current, changed);
    }
    track_edit(key, changed, m_->popup_id_ == key);
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
    if (list.max.y > m_->display_.y - 4.0f && anchor.min.y - 4.0f - list_h >= 4.0f) {
        list = {{anchor.min.x, anchor.min.y - 4.0f - list_h}, {anchor.max.x, anchor.min.y - 4.0f}};
    }
    m_->popup_open_cur_    = true;
    m_->popup_rect_cur_    = list;
    m_->popup_anchor_cur_  = anchor;

    const f32 view_h     = static_cast<f32>(visible) * item_h;
    const f32 max_scroll = std::max(0.0f, static_cast<f32>(count) * item_h - view_h);
    if (list.contains(m_->mouse_) && m_->wheel_ != 0.0f) {
        m_->popup_scroll_ -= wheel_scroll(item_h * 0.5f, list.height());
        m_->wheel_consumed_ = true;
    }

    for (u32 i = 0; i < m_->key_count_; ++i) {
        switch (m_->keys_[i].k) {
        case key::down:
            m_->popup_hover_ = std::min(m_->popup_hover_ + 1, static_cast<int>(count) - 1);
            break;
        case key::up:
            m_->popup_hover_ = std::max(m_->popup_hover_ - 1, 0);
            break;
        case key::enter:
            if (m_->popup_hover_ >= 0 && m_->popup_hover_ < static_cast<int>(count)) {
                changed   = changed || current != m_->popup_hover_;
                current   = m_->popup_hover_;
                m_->popup_id_ = 0;
            }
            break;
        case key::escape:
            m_->popup_id_ = 0;
            break;
        default:
            break;
        }
    }
    m_->key_count_ = 0;
    if (m_->popup_hover_ >= 0) { // keep the highlighted row on screen
        const f32 top = static_cast<f32>(m_->popup_hover_) * item_h;
        if (top < m_->popup_scroll_)                    { m_->popup_scroll_ = top; }
        if (top + item_h > m_->popup_scroll_ + view_h)  { m_->popup_scroll_ = top + item_h - view_h; }
    }
    m_->popup_scroll_ = std::clamp(m_->popup_scroll_, 0.0f, max_scroll);

    // everything below is emitted into the overlay layer, above all windows
    const u32 previous_owner = m_->run_owner_;
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

    m_->dl_.push_clip({{list.min.x, list.min.y + 1.0f}, {list.max.x, list.max.y - 1.0f}});
    for (std::size_t i = 0; i < count; ++i) {
        const f32  y = list.min.y + pad + static_cast<f32>(i) * item_h - m_->popup_scroll_;
        const rect r = {{list.min.x + pad, y}, {list.max.x - pad - (max_scroll > 0.0f ? 6.0f : 0.0f), y + item_h}};
        if (r.max.y < list.min.y || r.min.y > list.max.y) {
            continue;
        }

        const id ik = hash_id({reinterpret_cast<const char*>(&i), sizeof(i)}, key);
        const interaction it = interact(ik, r);
        if (it.hovered) {
            m_->popup_hover_ = static_cast<int>(i);
        }
        if (it.pressed) {
            changed   = changed || current != static_cast<int>(i);
            current   = static_cast<int>(i);
            m_->popup_id_ = 0;
        }

        const bool selected = static_cast<int>(i) == current;
        const bool hot      = static_cast<int>(i) == m_->popup_hover_;
        if (hot || selected) {
            shape_style row;
            row.radius      = radii(m_->style_.rounding * 0.55f);
            row.fill_top    = m_->style_.accent.scaled_alpha(hot ? 0.34f : 0.16f);
            row.fill_bottom = row.fill_top;
            m_->dl_.shape(r, row);
        }
        const vec2 tsize = label_size(f, items[i]);
        label_draw({r.min.x + 9.0f, r.min.y + (r.height() - tsize.y) * 0.5f},
                   selected ? m_->style_.accent_hover : m_->style_.text, items[i], f);
    }
    m_->dl_.pop_clip();

    if (max_scroll > 0.0f) {
        const f32 track_h = view_h;
        const f32 thumb_h = std::max(16.0f, track_h * view_h / (view_h + max_scroll));
        const f32 thumb_y = list.min.y + pad + (track_h - thumb_h) * (m_->popup_scroll_ / max_scroll);
        shape_style thumb;
        thumb.radius      = radii(2.0f);
        thumb.fill_top    = m_->style_.text_dim.scaled_alpha(0.5f);
        thumb.fill_bottom = thumb.fill_top;
        m_->dl_.shape({{list.max.x - 8.0f, thumb_y}, {list.max.x - 4.0f, thumb_y + thumb_h}}, thumb);
    }

    m_->dl_.pop_clip();
    m_->in_overlay_ = false;
    switch_run(previous_owner);
}

} // namespace strata

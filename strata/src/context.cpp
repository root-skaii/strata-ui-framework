#include "strata/context.hpp"

#include "context_impl.hpp"
#include "core/part_id.hpp"
#include "widget_util.hpp"

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

layer_scope::~layer_scope()
{
    ctx_->end_layer();
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
    context ctx{std::move(*atlas), cfg.theme, cfg.limits, cfg.capacity};
    ctx.m_->max_atlas_size_ = cfg.max_atlas_size;
    ctx.m_->diag_           = cfg.diagnostics;
    ctx.m_->strings_        = cfg.strings;
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

context::context(font_atlas atlas, const strata::style& theme, draw_list_limits limits, const capacity_config& capacity)
    : m_{std::make_unique<impl>(std::move(atlas), theme, limits, capacity)}
{}

context::impl::impl(font_atlas atlas, const strata::style& theme, draw_list_limits limits, const capacity_config& capacity)
    : font_{std::move(atlas)}
    , dl_{limits}
    , style_{theme}
    , dock_(std::make_unique<internal::dock_state>())
{
    const u32 windows = std::clamp<u32>(capacity.windows, 1, max_windows);
    win_.windows_.resize(windows);
    win_.z_order_.resize(windows);
    win_.frame_windows_.resize(windows);
    tree_table_.tables_.resize(std::max<u32>(capacity.tables, 1));
    children_cards_.children_.resize(std::max<u32>(capacity.children, 1));
    children_cards_.cards_.resize(std::max<u32>(capacity.cards, 1));
}

context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

// frame lifecycle ----------------------------------------------------------

context::~context()
{
    if (m_ == nullptr) {
        return; // (moved from)
    }
    wipe_edit_buffer();
    detail::secure_wipe(m_->input_.typed_.data(), m_->input_.typed_.size());
}

void context::wipe_edit_buffer() noexcept
{
    // also clears the bytes beyond size() that an earlier, longer text left in the buffer
    m_->edit_.edit_buf_.resize(m_->edit_.edit_buf_.capacity());
    detail::secure_wipe(m_->edit_.edit_buf_.data(), m_->edit_.edit_buf_.size());
    m_->edit_.edit_buf_.clear();
    m_->edit_.edit_cursor_ = 0;
    m_->edit_.edit_anchor_ = 0;
    m_->edit_.edit_pref_x_ = -1.0f;
    m_->edit_.ml_cache_key_ = 0;
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
    pop_color(m_->style_stack_.color_depth());
    pop_var(m_->style_stack_.var_depth());

    // only the top is clamped (stalls must not fast-forward animations); a lower clamp would speed up ui time at
    // high frame rates
    m_->dt_      = std::clamp(in.delta_time, 1.0e-6f, 0.1f);
    // timers take the whole delta (a host sleeping to a deadline must find it reached); one hour caps nonsense
    m_->wall_dt_ = std::clamp(in.delta_time, 1.0e-6f, 3600.0f);
    m_->time_   += m_->wall_dt_;
    m_->caret_blink_  = std::max(in.caret_blink_time, 0.0f);
    m_->input_.wheel_lines_  = std::min(in.wheel_lines, 100.0f); // (<= 0 is "a screenful", see scroll_step)
    m_->double_click_ = std::clamp(static_cast<f64>(in.double_click_time), 0.05, 5.0);
    const f32 inv_scale = 1.0f / m_->scale_;
    // a minimized window reports a near-zero display size, but layout clamps downstream assume roughly window-sized
    // bounds (fixed lower bounds like 150 or title_h + 48) and would trip std::clamp's debug assertion. nothing is
    // visible while minimized, so flooring is free.
    m_->display_ = {std::max(in.display_size.x * inv_scale, 200.0f), std::max(in.display_size.y * inv_scale, 200.0f)};
    // smooth scrolling: notches add to the backlog and a share of it is applied per frame (all of it when small).
    // reversing direction drops the old backlog
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
    m_->input_.wheel_        = smooth(m_->input_.wheel_pending_, in.wheel);
    m_->input_.wheel_x_      = smooth(m_->input_.wheel_x_pending_, in.wheel_x);
    m_->input_.wheel_moving_ = m_->input_.wheel_pending_ != 0.0f || m_->input_.wheel_x_pending_ != 0.0f;
    m_->wheel_consumed_ = false;
    m_->wheel_x_consumed_ = false;
    m_->cursor_  = cursor_kind::arrow;

    const vec2 mouse_logical = in.mouse_pos * inv_scale;
    m_->input_.mouse_delta_ = m_->input_.have_mouse_ ? mouse_logical - m_->input_.mouse_ : vec2{};
    m_->input_.mouse_       = mouse_logical;
    m_->input_.have_mouse_  = true;

    const bool down = in.mouse_down[0];
    m_->input_.mouse_pressed_  = down && !m_->input_.mouse_down_;
    m_->input_.mouse_released_ = !down && m_->input_.mouse_down_;
    m_->input_.mouse_down_     = down;

    m_->input_.mouse_right_pressed_   = in.mouse_down[1] && !m_->input_.mouse_right_down_;
    m_->input_.mouse_right_released_  = !in.mouse_down[1] && m_->input_.mouse_right_down_;
    m_->input_.mouse_right_down_      = in.mouse_down[1];
    m_->input_.mouse_middle_pressed_  = in.mouse_down[2] && !m_->input_.mouse_middle_down_;
    m_->input_.mouse_middle_released_ = !in.mouse_down[2] && m_->input_.mouse_middle_down_;
    m_->input_.mouse_middle_down_     = in.mouse_down[2];
    m_->input_.mod_ctrl_  = in.ctrl;
    m_->input_.mod_shift_ = in.shift;
    m_->input_.mod_alt_   = in.alt;

    // a key_sequence's pending prefix is cleared one frame after a key failed to advance it, so every
    // sequence_pressed() call sharing that prefix sees the key before it is dropped. do not clear it inside
    // sequence_pressed: the first non-matching sequence would tear down a prefix a later one still matches
    if (m_->hotkey_.seq_pending_count_ > 0 && m_->input_.pressed_key_ != key::none && !m_->hotkey_.seq_pending_touched_) {
        m_->hotkey_.seq_pending_count_ = 0;
    }
    m_->hotkey_.seq_pending_touched_ = false;

    // key presses: this frame's join the queue, and the oldest one is this frame's press
    // (text fields and lists read this frame's presses directly: the editing keys, and Ctrl + A / C / V / X / Z / Y)
    const u32 key_count = std::min(in.key_count, max_key_events);
    m_->input_.key_count_ = 0;
    for (u32 i = 0; i < key_count; ++i) {
        const key_event& e = in.keys[i];
        if (e.k == key::none) { continue; }
        if (m_->input_.press_queued_ < m_->input_.press_queue_.size()) { m_->input_.press_queue_[m_->input_.press_queued_++] = e; }
        const bool editing = e.k == key::left || e.k == key::right || e.k == key::up || e.k == key::down || e.k == key::home ||
                             e.k == key::end || e.k == key::backspace || e.k == key::del || e.k == key::enter ||
                             e.k == key::escape || e.k == key::tab || e.k == key::page_up || e.k == key::page_down;
        const bool shortcut = e.ctrl && (e.k == key::a || e.k == key::c || e.k == key::v || e.k == key::x || e.k == key::z || e.k == key::y);
        if (editing || shortcut) { m_->input_.keys_[m_->input_.key_count_++] = e; }
    }
    if (m_->input_.press_queued_ > 0) {
        const key_event p = m_->input_.press_queue_[0];
        std::copy(m_->input_.press_queue_.begin() + 1, m_->input_.press_queue_.begin() + m_->input_.press_queued_, m_->input_.press_queue_.begin());
        --m_->input_.press_queued_;
        m_->input_.pressed_key_ = p.k;
        m_->input_.press_ctrl_  = p.ctrl;
        m_->input_.press_shift_ = p.shift;
        m_->input_.press_alt_   = p.alt;
    } else {
        m_->input_.pressed_key_ = key::none;
        m_->input_.press_ctrl_  = in.ctrl;
        m_->input_.press_shift_ = in.shift;
        m_->input_.press_alt_   = in.alt;
    }
    m_->input_.ime_text_    = in.ime;
    m_->input_.ime_len_     = std::min<u32>(in.ime_len, static_cast<u32>(in.ime.size()));
    m_->input_.ime_cursor_  = std::min(in.ime_cursor, m_->input_.ime_len_);
    m_->edit_.ime_want_    = false;
    m_->input_.typed_     = in.typed;
    m_->input_.typed_len_ = std::min(in.typed_len, max_typed_bytes);

    m_->anim_moved_   = m_->input_.wheel_moving_; // (a scroll still being let out asks for the next frame like an animation)
    m_->rich_.rich_clicked_.clear();
    m_->rich_.rich_hovered_.clear();

    m_->win_.hovered_window_prev_ = m_->win_.hovered_window_cur_;
    m_->win_.hovered_window_cur_  = 0;
    m_->hovered_docked_prev_ = m_->hovered_docked_cur_;
    m_->hovered_docked_cur_  = false;
    m_->win_.hovered_z_           = no_z;
    m_->win_.frame_window_count_  = 0;
    m_->style_stack_.reset_font();
    m_->rich_.rich_depth_          = 0;
    m_->tree_table_.table_depth_         = 0;
    m_->tree_table_.table_               = {};

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
    m_->hotkey_.hotkey_seen_   = false;
    m_->children_cards_.child_depth_   = 0;
    m_->child_base_     = 0;
    m_->nest_depth_     = 0;
    m_->nest_overflow_  = 0;
    m_->children_cards_.card_depth_    = 0;
    m_->hover_key_prev_ = m_->hover_key_cur_;
    m_->hover_key_cur_  = 0;
    m_->last_item_hovered_ = false;
    m_->last_item_focused_ = false;
    m_->last_item_pressed_ = false;
    m_->last_item_double_  = false;
    m_->press_claimed_ = false;
    m_->edit_.submitted_     = false;

    m_->input_.keys_held_      = in.keys_held;
    m_->stats_cur_      = {};
    m_->disabled_depth_ = 0;
    m_->disabled_alpha_ = false;
    m_->disabled_count_ = 0;
    m_->accessory_.next_gutter_    = 0.0f;
    m_->accessory_.accessory_row_  = 0;
    m_->accessory_.row_anchor_     = 0;
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

    for (internal::popup_level& p : m_->popup_.popups_) {
        p.open_prev   = p.open_cur;
        p.rect_prev   = p.rect_cur;
        p.anchor_prev = p.anchor_cur;
        p.open_cur    = false;
    }
    m_->popup_.popup_depth_    = 0;
    m_->popup_.popup_esc_used_ = false;
    m_->in_overlay_     = false;
    m_->swallow_press_  = false;

    // menus: a press outside every open menu closes them all (and only does that)
    m_->menu_.menu_hit_prev_ = false;
    bool menu_any  = false;
    for (menu_level& m : m_->menu_.menu_open_) {
        m.rect_prev = m.rect_cur;
        m.rect_cur  = {};
        m.seen      = false;
        if (m.key != 0) {
            menu_any       = true;
            m_->menu_.menu_hit_prev_ = m_->menu_.menu_hit_prev_ || m.rect_prev.contains(m_->input_.mouse_);
        }
    }
    m_->menu_.menu_depth_ = 0;
    if (menu_any && (m_->input_.mouse_pressed_ || m_->input_.mouse_right_pressed_) && !m_->menu_.menu_hit_prev_ && !m_->menu_.menu_open_[0].anchor.contains(m_->input_.mouse_)) {
        menu_close_all();
        m_->menu_.menu_hit_prev_ = false;
        m_->swallow_press_ = true;
    }

    // popups: a press closes the levels above the highest one it lands in (a level's own opener counts as in it:
    // that widget toggles it itself). outside all of them it closes every one. either way the press only does that.
    // a press on an open menu (e.g. one opened from a popup) is the menu's.
    if (m_->input_.mouse_pressed_ && !m_->menu_.menu_hit_prev_ && m_->popup_.popup_any_prev()) {
        u32 keep = 0; // levels that stay open
        for (u32 i = 0; i < m_->popup_.popup_count_; ++i) {
            const internal::popup_level& p = m_->popup_.popups_[i];
            if (p.open_prev && (p.rect_prev.contains(m_->input_.mouse_) || p.anchor_prev.contains(m_->input_.mouse_))) { keep = i + 1; }
        }
        if (keep < m_->popup_.popup_count_) {
            m_->popup_.popup_close_from(keep);
            m_->swallow_press_ = true;
        }
    }
    m_->toast_.toast_hover_prev_ = m_->toast_.toast_hover_cur_;
    m_->toast_.toast_hover_cur_  = false;
    m_->modal_.modal_top_prev_   = m_->modal_.modal_count_ > 0 ? m_->modal_.modal_stack_[m_->modal_.modal_count_ - 1] : id{};
    m_->modal_.modal_depth_      = 0;

    m_->ids_.begin_frame(); // id stack, duplicate-id tracking, this frame's collision report

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
    // unbalanced begin / end: reported once per kind. the window is closed here (it would otherwise swallow the next
    // frame); begin_frame resets the other stacks, so only this frame's later widgets were placed wrong.
    const auto unbalanced = [&](u32 depth, const char* missing) {
        if (depth == 0) { return; }
        char buf[128];
        const int n = std::snprintf(buf, sizeof buf, "%s missing at end_frame (%u open)", missing, depth);
        diagnose(diagnostic_kind::misuse, {buf, n > 0 ? std::min<std::size_t>(static_cast<std::size_t>(n), sizeof buf - 1) : 0},
                 hash_id(missing));
    };
    unbalanced(m_->popup_.popup_depth_, "end_popup()");
    unbalanced(m_->menu_.menu_depth_, "end_menu() / end_popup_menu()");
    unbalanced(m_->modal_.modal_depth_, "end_modal()");
    unbalanced(m_->children_cards_.child_depth_, "end_child()");
    unbalanced(m_->children_cards_.card_depth_, "end_card()");
    unbalanced(m_->tree_table_.table_depth_, "end_table()");
    unbalanced(m_->tree_table_.tree_depth_, "tree_pop()");
    unbalanced(m_->style_stack_.font_depth(), "pop_font()");
    unbalanced(m_->disabled_count_, "end_disabled()");
    unbalanced(m_->gutter_depth_, "pop_right_gutter()");
    if (m_->cur_ != nullptr) {
        unbalanced(1, "end_window()");
        while (m_->cur_ != nullptr) { end_window(); } // (a nested window resumes its parent)
    }
    unbalanced(m_->ids_.depth(), "pop_id()");
    m_->tree_table_.tree_depth_ = 0;
    m_->popup_.popup_depth_     = 0;
    dock_end_frame();
    toast_end_frame();
    // a menu level not built this frame closes along with everything above it
    for (u32 i = 0; i < max_menu_levels; ++i) {
        if (m_->menu_.menu_open_[i].key != 0 && !m_->menu_.menu_open_[i].seen) {
            menu_close_from(i);
            break;
        }
    }
    if (m_->menu_.menu_close_all_) {
        menu_close_all();
        m_->menu_.menu_close_all_ = false;
    }
    apply_layer_order();

    if (!m_->input_.mouse_down_) {
        m_->active_       = 0; // a widget that vanished mid-drag must not stay active forever
        m_->dnd_.dd_active_    = false;
        m_->dnd_.dd_candidate_ = 0;
        m_->dnd_.dd_cancelled_ = false;
    }
    // an unclaimed press drops keyboard focus; so does a field that was not drawn
    if ((m_->input_.mouse_pressed_ && !m_->press_claimed_) || (m_->focus_id_ != 0 && !m_->focus_seen_)) {
        m_->focus_id_ = 0;
    }
    if (m_->focus_id_ == 0 && !m_->edit_.edit_buf_.empty()) {
        wipe_edit_buffer();
    }
    detail::secure_wipe(m_->input_.typed_.data(), m_->input_.typed_.size());
    m_->input_.typed_len_ = 0;
    // a popup level not drawn this frame closes along with everything above it
    for (u32 i = 0; i < m_->popup_.popup_count_; ++i) {
        if (!m_->popup_.popups_[i].open_cur) {
            m_->popup_.popup_close_from(i);
            break;
        }
    }
    if (m_->hotkey_.hotkey_capture_ != 0 && !m_->hotkey_.hotkey_seen_) {
        m_->hotkey_.hotkey_capture_ = 0;
    }
    m_->hover_time_ = (m_->hover_key_cur_ != 0 && m_->hover_key_cur_ == m_->hover_key_prev_) ? m_->hover_time_ + m_->wall_dt_ : 0.0f;

    m_->anim_.maybe_compact(m_->frame_);

    const draw_data dd = m_->dl_.data();

    // changed? the ui is rebuilt every frame, so only a hash can tell: xxh64 over the four packed, trivially copyable
    // arrays, each seeded with the previous hash. any visible difference changes it.
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

    // will a later frame differ untouched? animations short of their target, plus the clock-driven cases the values
    // cannot show: toasts expiring / fading and the tooltip delay. without them an idling host would freeze toasts
    // and never show tooltips.
    const bool tooltip_pending = m_->hover_key_cur_ != 0 && m_->hover_time_ < m_->style_.tooltip_delay_s;
    // toasts: sliding, fading or with a moving bar (countdown, busy) change every frame; sticky or hover-paused ones
    // do not
    bool toast_moving = false;
    for (const toast_entry& t : m_->toast_.toasts_) {
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
            const f64 phase  = std::fmod(m_->time_ - m_->edit_.caret_time_, period);
            wake = std::min(wake, phase < m_->caret_blink_ ? m_->caret_blink_ - phase : period - phase);
        }
        if (m_->hotkey_.hotkey_capture_ != 0 && m_->hotkey_.seq_edit_count_ > 0) { wake = std::min(wake, m_->hotkey_.seq_edit_deadline_ - m_->time_); }
        // land just past a deadline, not a hair before it
        if (wake != no_deadline) { wake = std::max(wake, 0.0) + 0.001; }
    }
    m_->next_wake_ = wake;

    m_->stats_cur_.vertices         = static_cast<u32>(dd.vertices.size());
    m_->stats_cur_.indices          = static_cast<u32>(dd.indices.size());
    m_->stats_cur_.draw_calls       = static_cast<u32>(dd.commands.size());
    m_->stats_cur_.anim_slots_used  = m_->anim_.used();
    m_->stats_cur_.anim_slots_total = m_->anim_.capacity();
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

// animation state: m_->anim_ is an open-addressing table keyed by widget id (src/core/animation.hpp). slots
// untouched for a couple of frames are stale and reusable; a full table grows, dropping stale ones.

context::anim_slot* context::anim_find(id key) noexcept
{
    return m_->anim_.find(key);
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
    return m_->anim_.for_key(key, m_->frame_);
}

f32 context::approach(f32 current, f32 target, f32 speed) const noexcept
{
    const f32 k = 1.0f - std::exp(-(speed > 0.0f ? speed : m_->style_.anim_speed) * m_->dt_);
    const f32 v = current + (target - current) * k;
    if (std::abs(target - v) < 0.002f) {
        return target; // close enough: snap, so a settled animation stops producing new geometry
    }
    m_->anim_moved_ = true; // still moving (see animations_settling)
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

f32 context::scroll_step(f32 unit, f32 page) const noexcept
{
    const f32 lines = m_->input_.wheel_lines_;
    const f32 step  = lines > 0.0f ? lines * unit : (page > 0.0f ? page * 0.9f : unit * 10.0f);
    return step * std::max(m_->style_.scroll_speed, 0.0f);
}

f32 context::wheel_scroll(f32 unit, f32 page) const noexcept { return m_->input_.wheel_ * scroll_step(unit, page); }
f32 context::wheel_scroll_x(f32 unit, f32 page) const noexcept { return m_->input_.wheel_x_ * scroll_step(unit, page); }

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
    // over: a session that changed something, or a sessionless change (started and ended at once)
    f.after_edit = !engaged && (open != nullptr && was ? open->changed : changed);
    if (!engaged && open != nullptr) { *open = {}; }
    m_->edit_flags_ = f;
    m_->edit_item_  = m_->last_item_key_;
}

void context::push_id(std::string_view s) noexcept
{
    if (!m_->ids_.push(s)) {
        report_limit("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

void context::push_id(const void* p) noexcept
{
    push_id(static_cast<u64>(reinterpret_cast<std::uintptr_t>(p)));
}

void context::push_id(u64 value) noexcept
{
    if (!m_->ids_.push(value)) {
        report_limit("push_id nesting (max_id_depth): ids will collide", max_id_depth);
    }
}

void context::pop_id() noexcept
{
    m_->ids_.pop();
}

void context::push_font(font_id f) noexcept
{
    if (f < m_->font_.font_count() && !m_->style_stack_.push_font(f)) {
        report_limit("push_font nesting (max_font_depth)", max_font_depth);
    }
}

void context::pop_font() noexcept
{
    m_->style_stack_.pop_font();
}

// layers: window stacking and popups ---------------------------------------------

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

// draw order: outside-window drawing, then windows by stacking order (runs in emission order), then popups.
void context::apply_layer_order()
{
    switch_run(run_base);

    const u32 n = m_->win_.frame_window_count_;
    m_->win_.focused_window_ = 0;

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
        const window_state& wa = *m_->win_.frame_windows_[a];
        const window_state& wb = *m_->win_.frame_windows_[b];
        if (rank(wa) != rank(wb)) { return rank(wa) < rank(wb); }
        if (stack_z(wa) != stack_z(wb)) { return stack_z(wa) < stack_z(wb); }
        return (wa.dock_owner != 0 && wa.docked_now ? 1 : 0) < (wb.dock_owner != 0 && wb.docked_now ? 1 : 0);
    });
    for (u32 i = n; i-- > 0;) { // topmost window; the menu bar does not take focus from windows below
        if (!m_->win_.frame_windows_[order[i]]->menubar && !m_->win_.frame_windows_[order[i]]->passive) {
            m_->win_.focused_window_ = m_->win_.frame_windows_[order[i]]->key;
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
        const u32 level = m_->win_.frame_windows_[order[i]]->modal_level;
        while (level > backdrops_done) { // the dimmed area goes right below the first window of its level
            ++backdrops_done;
            add_owner(run_backdrop + backdrops_done - 1);
        }
        add_owner(order[i]);
    }
    add_owner(run_overlay);
    for (u32 level = 1; level < popup_stack::max_popup_levels; ++level) { // nested popups, each above its parent
        add_owner(popup_stack::popup_run(level));
    }
    add_owner(run_foreground);

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
    color& slot = color_ref(which);
    if (!m_->style_stack_.push_color(which, slot)) {
        report_limit("push_color nesting (max_overrides)", max_overrides);
        return;
    }
    slot = c;
}

void context::pop_color(u32 count) noexcept
{
    style_stack::saved_color s;
    while (count-- > 0 && m_->style_stack_.pop_color(s)) {
        color_ref(s.which) = s.previous;
    }
}

void context::push_var(style_var which, f32 value) noexcept
{
    f32& slot = var_ref(which);
    if (!m_->style_stack_.push_var(which, slot)) {
        report_limit("push_var nesting (max_overrides)", max_overrides);
        return;
    }
    slot = value;
}

void context::pop_var(u32 count) noexcept
{
    style_stack::saved_var s;
    while (count-- > 0 && m_->style_stack_.pop_var(s)) {
        var_ref(s.which) = s.previous;
    }
}

style_scope context::style_overrides(std::initializer_list<style_override> list) noexcept
{
    u32 colors = 0;
    u32 vars   = 0;
    for (const style_override& o : list) {
        if (o.is_color) {
            const u32 before = m_->style_stack_.color_depth();
            push_color(static_cast<style_color>(o.which), o.c);
            colors += m_->style_stack_.color_depth() - before;
        } else {
            const u32 before = m_->style_stack_.var_depth();
            push_var(static_cast<style_var>(o.which), o.v);
            vars += m_->style_stack_.var_depth() - before;
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
    f32  target_h; // the height to center this item against: siblings already placed on this line, or (for a
                    // line's first item) a caller-supplied expectation such as a table row's known height. Purely
                    // cosmetic: it nudges where the item is *drawn*, never how much room the line is deemed to take.
    if (l.same_line && !l.first) {
        pos      = {l.cursor_x + m_->style_.item_spacing, l.line_top};
        target_h = l.line_h;
        l.line_h = std::max(l.line_h, size.y);
    } else {
        pos        = {l.origin.x, l.first ? l.origin.y : l.line_top + l.line_h + m_->style_.item_spacing};
        l.line_top = pos.y;
        target_h   = l.line_h_seed;
        l.line_h   = size.y; // the line's real height starts at this item's own; a seed only ever centers, it
                              // never inflates this past what the line actually contains
        // a zero-height item is a structural placeholder (begin_table's start marker, say), not visible content:
        // it takes no centering itself, and leaves the seed for whatever real content follows it on this line
        if (size.y > 0.0f) { l.line_h_seed = 0.0f; }
    }
    l.first     = false;
    l.same_line = false;
    l.cursor_x  = pos.x + size.x;
    // the first item starts the extent: a layout scrolled above the screen has only negative positions, and a
    // `bottom` starting at 0 would inflate the content height. Uses the un-centered position: a short item next
    // to a tall one must not be seen as taking more room just because it was drawn lower to center it.
    l.bottom    = first_item ? pos.y + size.y : std::max(l.bottom, pos.y + size.y);
    l.right     = first_item ? pos.x + size.x : std::max(l.right, pos.x + size.x);
    // shorter items sharing a line with a taller one are centered on it, not stuck at its top; a zero-height
    // placeholder has nothing to center and must not be nudged by a seed meant for the content drawn after it
    const f32 dy = size.y > 0.0f ? std::max(0.0f, (target_h - size.y) * 0.5f) : 0.0f;
    return rect::from_size({pos.x, pos.y + dy}, size);
}

// rows are submitted whether visible or not (the caller does not know the view), so culled rows skip all their
// work: hit test, animation slot, text measuring, geometry. the layout was already advanced, so positions do not
// change. the margin keeps half-visible rows alive.
bool context::item_culled(const rect& r) noexcept
{
    ++m_->stats_cur_.items_submitted;
    // off-screen measuring passes (popup / drag previews) need every item's size: no culling
    if (m_->dnd_.dd_hidden_ || m_->gpopup_hidden_) {
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
    m_->tree_table_.item_pressed_      = false;
}

// the same labels are measured every frame: a direct-mapped (font, text) -> size cache. collisions overwrite, so it
// never grows or needs sweeping.
vec2 context::measure_cached(font_id f, std::string_view s) noexcept
{
    if (s.empty()) {
        return m_->font_.measure(f, s);
    }
    if (m_->measure_.measure_cache_.size() != measure_cache_size || m_->measure_.measure_cache_gen_ != m_->font_generation_) {
        m_->measure_.measure_cache_.assign(measure_cache_size, measure_slot{});
        m_->measure_.measure_cache_gen_ = m_->font_generation_;
    }
    u64 h = 0xcbf29ce484222325ull ^ (static_cast<u64>(f) << 56);
    for (const char c : s) {
        h = (h ^ static_cast<u8>(c)) * 0x100000001b3ull;
    }
    h |= 1ull; // 0 marks an empty slot
    measure_slot& slot = m_->measure_.measure_cache_[static_cast<u32>(h >> 20) & (measure_cache_size - 1)];
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
    return m_->ids_.widget_id(label);
}

void context::check_id([[maybe_unused]] id key) noexcept
{
#ifndef NDEBUG
    const auto dup = m_->ids_.check_duplicate(key);
    if (dup.is_duplicate) {
        ++m_->stats_cur_.id_collisions;
        const std::string message = std::format("duplicate widget id \"{}\": give one of them a \"##suffix\" or push_id() around it", dup.label);
        diagnose(diagnostic_kind::id_collision, message, key);
    }
#endif
}

context::interaction context::interact(id key, const rect& r) noexcept
{
    // popup content ignores window hover (it extends past the window)
    return interact_impl(key, r, m_->in_overlay_ || (m_->cur_window_ != 0 && m_->win_.hovered_window_prev_ == m_->cur_window_));
}

context::interaction context::interact_impl(id key, const rect& r, bool in_window) noexcept
{
    interaction out;
    check_id(key); // before the early-out: a hidden widget still owns its id
    if (m_->dl_.alpha() < 0.1f) {
        return out; // (nearly) invisible content, e.g. mid page transition
    }

    // what lies under an open popup (or a popup level above this one, or a menu) gets no pointer
    const bool over      = in_window && !m_->pointer_blocked() && m_->dl_.clip().contains(m_->input_.mouse_) && r.contains(m_->input_.mouse_);

    // an allow_item_overlap() item loses the press to anything submitted over it. the press is resolved this frame;
    // the hover can only be removed a frame late (the covering item is submitted after), so it uses last frame's cover.
    const bool ceded = m_->overlap_key_ != 0 && key != m_->overlap_key_ && m_->overlap_rect_.contains(m_->input_.mouse_);
    if (ceded && over) {
        m_->overlap_taken_cur_ = m_->overlap_key_;
    }
    const bool suppressed = key != 0 && key == m_->overlap_taken_prev_;

    // a disabled item is still "under the pointer" (for a tooltip explaining why), but gets no hover, press or keyboard
    if (m_->disabled_depth_ > 0) {
        m_->last_item_key_       = key;
        m_->last_item_rect_      = r;
        m_->last_item_hovered_   = over;
        m_->last_item_pressed_   = false;
        m_->last_item_double_    = false;
        m_->last_item_focused_   = false;
        // and a not-allowed cursor, so it does not look broken
        if (over) { m_->cursor_ = cursor_kind::not_allowed; }
        if (over) { m_->hover_key_cur_ = key; }
        return out;
    }

    out.hovered = over && !suppressed && (m_->active_ == 0 || m_->active_ == key || (ceded && m_->active_ == m_->overlap_key_));
    if (out.hovered && m_->input_.mouse_pressed_ && !m_->swallow_press_ && (m_->active_ == 0 || (ceded && m_->active_ == m_->overlap_key_))) {
        if (m_->active_ != 0) { m_->overlap_stolen_ = true; }
        m_->active_ = key;
        // a second press on the same spot within the double-click time, reported on completion below. separate from
        // register_click(), whose 1 / 2 / 3 run belongs to the focused text field
        const vec2 moved  = m_->input_.mouse_ - m_->item_click_pos_;
        m_->item_dbl_pending_ = key == m_->item_click_key_ && m_->time_ - m_->item_click_time_ < m_->double_click_ && dot(moved, moved) < 25.0f;
        m_->item_click_key_   = key;
        m_->item_click_time_  = m_->time_;
        m_->item_click_pos_   = m_->input_.mouse_;
    }
    if (m_->active_ == key) {
        out.held = m_->input_.mouse_down_;
        if (m_->input_.mouse_released_) {
            out.pressed = over && !(m_->dnd_.dd_active_ && m_->dnd_.dd_source_ == key); // the release that ends a drag is not a click
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

bool context::key_pressed(key k, bool ctrl, bool shift, bool alt) const noexcept
{
    if (k == key::none || m_->input_.pressed_key_ != k || m_->hotkey_.hotkey_capture_ != 0) {
        return false;
    }
    if (m_->input_.press_ctrl_ != ctrl || m_->input_.press_shift_ != shift || m_->input_.press_alt_ != alt) {
        return false;
    }
    // a text field being typed into owns what types, moves the caret or deletes, and its own Ctrl shortcuts
    if (want_text_input() && !alt) {
        const auto v = static_cast<u32>(k);
        const bool caret = k == key::left || k == key::right || k == key::home || k == key::end || k == key::backspace ||
                           k == key::del;
        const bool types = !ctrl && (k == key::space || (v >= 0x30 && v <= 0x39) || (v >= 0x41 && v <= 0x5a) ||
                                     (v >= 0x60 && v <= 0x6f) || (v >= 0xba && v <= 0xc0) || (v >= 0xdb && v <= 0xde));
        const bool edit_shortcut = ctrl && !shift && (k == key::a || k == key::c || k == key::v || k == key::x || k == key::z || k == key::y);
        if (caret || types || edit_shortcut) { return false; }
    }
    return true;
}

bool context::key_down(key k) const noexcept
{
    if (want_text_input()) {
        return false;
    }
    const auto i = static_cast<u32>(k);
    return (m_->input_.keys_held_[i >> 3] & (1u << (i & 7))) != 0;
}

bool context::mouse_down(mouse_button b) const noexcept
{
    switch (b) {
    case mouse_button::right:  return m_->input_.mouse_right_down_;
    case mouse_button::middle: return m_->input_.mouse_middle_down_;
    default:                   return m_->input_.mouse_down_;
    }
}

bool context::mouse_clicked(mouse_button b) const noexcept
{
    switch (b) {
    case mouse_button::right:  return m_->input_.mouse_right_pressed_;
    case mouse_button::middle: return m_->input_.mouse_middle_pressed_;
    default:                   return m_->input_.mouse_pressed_;
    }
}

bool context::mouse_released(mouse_button b) const noexcept
{
    switch (b) {
    case mouse_button::right:  return m_->input_.mouse_right_released_;
    case mouse_button::middle: return m_->input_.mouse_middle_released_;
    default:                   return m_->input_.mouse_released_;
    }
}

bool context::window_focused() const noexcept
{
    if (m_->cur_window_ == 0) {
        return false;
    }
    // nothing has been clicked yet: the topmost window of the last frame has it
    return m_->key_window_ != 0 ? m_->key_window_ == m_->cur_window_ : m_->win_.focused_window_ == m_->cur_window_;
}

bool context::window_focused(std::string_view title) const noexcept
{
    const id key = hash_id(title, 0);
    return m_->key_window_ != 0 ? m_->key_window_ == key : m_->win_.focused_window_ == key;
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

// slot for the next accessory of the row just submitted, right to left inside the scrollbar inset. false with no
// row or no room.
bool context::accessory_slot(f32 width, rect& out, f32 inset) noexcept
{
    if (m_->cur_ == nullptr || m_->accessory_.row_anchor_ == 0) {
        return false;
    }
    if (m_->accessory_.accessory_row_ != m_->accessory_.row_anchor_) { // the first accessory of this row
        m_->accessory_.accessory_row_      = m_->accessory_.row_anchor_;
        m_->accessory_.accessory_row_rect_ = m_->accessory_.row_anchor_rect_;
        m_->accessory_.accessory_x_        = std::min(m_->accessory_.row_anchor_rect_.max.x, m_->layout_.origin.x + m_->layout_.width + m_->layout_.gutter);
        allow_item_overlap_at(m_->accessory_.row_anchor_, m_->accessory_.row_anchor_rect_); // the row lets go of the press where they sit
        m_->accessory_.accessory_x_ -= inset; // (the first control only: the others follow it)
    }
    const rect& row = m_->accessory_.accessory_row_rect_;
    const f32   h   = std::min(width, std::max(row.height() - 4.0f, 4.0f));
    const f32   x1  = m_->accessory_.accessory_x_ - 2.0f;
    const f32   x0  = x1 - width;
    if (x0 < row.min.x) {
        return false; // the row is too narrow for one more
    }
    m_->accessory_.accessory_x_ = x0;
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
    case mouse_button::right:  return m_->last_item_hovered_ && m_->input_.mouse_right_pressed_;
    case mouse_button::middle: return m_->last_item_hovered_ && m_->input_.mouse_middle_pressed_;
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
    const id key = hash_id(icon, hash_id(id_extra, part_id(part::accessory_button, m_->accessory_.accessory_row_)));
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

bool context::row_accessory_checkbox(std::string_view id_extra, bool& value, f32 inset)
{
    rect box;
    const f32 side = std::min(frame_height() - 8.0f, 16.0f);
    if (!accessory_slot(side, box, inset)) {
        return false;
    }
    const id key = hash_id(id_extra, part_id(part::accessory_checkbox, m_->accessory_.accessory_row_));
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
    const id key = hash_id(id_extra, part_id(part::accessory_toggle, m_->accessory_.accessory_row_));
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
    if (m_->rich_.rich_depth_ > 0) { // markup cannot be cut safely: clip it instead
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

// scrollbar thumbs ---------------------------------------------------------

// one axis of a scrollbar thumb; `along` and everything else are measured on the track's axis.
// the wrappers below are all that differs between vertical and horizontal.
f32 context::thumb_drag_along(f32 along, const interaction& in, f32& grab, f32 thumb_lo, f32 thumb_len, f32 track_lo,
                              f32 travel, f32 max_scroll, f32 scroll) const noexcept
{
    if (!in.held || travel <= 0.0f) {
        return scroll;
    }
    if (m_->input_.mouse_pressed_) {
        const bool on_thumb = along >= thumb_lo && along <= thumb_lo + thumb_len;
        grab = on_thumb ? along - thumb_lo : thumb_len * 0.5f;
    }
    return std::clamp((along - grab - track_lo) / travel, 0.0f, 1.0f) * max_scroll;
}

f32 context::thumb_drag(const interaction& in, f32& grab, f32 thumb_y, f32 thumb_h, f32 track_top, f32 travel,
                        f32 max_scroll, f32 scroll) const noexcept
{
    return thumb_drag_along(m_->input_.mouse_.y, in, grab, thumb_y, thumb_h, track_top, travel, max_scroll, scroll);
}

f32 context::thumb_drag_x(const interaction& in, f32& grab, f32 thumb_x, f32 thumb_w, f32 track_left, f32 travel,
                          f32 max_scroll, f32 scroll) const noexcept
{
    return thumb_drag_along(m_->input_.mouse_.x, in, grab, thumb_x, thumb_w, track_left, travel, max_scroll, scroll);
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
    const rect r    = layout_place(size); // centered on the line by layout_place itself
    label_draw(r.min, c, s, f);
    // plain text is an item too, so item_hovered(), item_rect(), tooltip() and context_menu() work after it. it takes
    // no press.
    note_passive_item(widget_id(s), r);
}

// makes a rect "the last item" without taking the press
void context::note_passive_item(id key, const rect& r) noexcept
{
    const bool over = m_->cur_window_ != 0 && m_->win_.hovered_window_prev_ == m_->cur_window_ && m_->active_ == 0 &&
                      !m_->popup_.popup_covers(m_->input_.mouse_) && !m_->menu_.menu_hit_prev_ &&
                      m_->dl_.clip().contains(m_->input_.mouse_) && r.contains(m_->input_.mouse_);
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
        const f32 t    = std::clamp((m_->input_.mouse_.x - x0) / (x1 - x0), 0.0f, 1.0f);
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

// page transitions -------------------------------------------------------

transition_scope::~transition_scope()
{
    ctx_->pop_alpha();
}

f32 context::layout_next_y() const noexcept
{
    const layout_state& l = m_->layout_;
    if (l.same_line && !l.first) {
        return l.line_top;
    }
    return l.first ? l.origin.y : l.line_top + l.line_h + m_->style_.item_spacing;
}

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

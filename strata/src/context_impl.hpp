#pragma once

// internal: strata::context's private state, held by pointer so it can change without touching the public header.

#include "strata/context.hpp"

#include "core/animation.hpp"
#include "core/id_stack.hpp"
#include "core/input_frame.hpp"
#include "core/layout_state.hpp"
#include "core/style_stack.hpp"
#include "core/window_registry.hpp"
#include "chart_state.hpp"
#include "child_card_state.hpp"
#include "code_state.hpp"
#include "color_picker_state.hpp"
#include "combo_filter_state.hpp"
#include "confirm_state.hpp"
#include "date_picker_state.hpp"
#include "dock_state.hpp"
#include "drag_drop_state.hpp"
#include "edit_state.hpp"
#include "hotkey_state.hpp"
#include "measure_cache_state.hpp"
#include "menu_state.hpp"
#include "modal_state.hpp"
#include "nav_state.hpp"
#include "popup_state.hpp"
#include "rich_text_state.hpp"
#include "row_accessory_state.hpp"
#include "table_tree_state.hpp"
#include "toast_state.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace strata {

struct context::interaction {
    bool hovered{};
    bool held{};
    bool pressed{};
};

// a run of draw commands and its owner: windows restack / popups lift by reordering runs
struct context::cmd_run {
    u32 first{};
    u32 count{};
    u32 owner{};
};

struct context::field_layout {
    rect control;
    rect label_row;
    bool has_label{};
};




struct context::press_anim { f32 hover{}; f32 active{}; };

// a composite reports for its parts: they stay quiet while this lives

// what set_scale needs to rebuild the atlas (owned strings; font_config views re-pointed)
struct context::font_source {
    std::string face;
    std::string file;
    font_config cfg;
};


   // bulk open also applies to nodes revealed later


// edit sessions (track_edit): last widget's flags, the open session and whether it changed, engaged widgets this /
// last frame
struct context::edit_flags {
    bool active{}, activated{}, deactivated{}, edited{}, after_edit{};
};

 // the item (last_item_key_) these flags belong to
struct context::edit_session {
    id   key{};
    bool changed{};
};

struct context::impl {
    impl(font_atlas atlas, const strata::style& theme, draw_list_limits limits);

    std::vector<std::pair<u64, int>> toast_results_;

    font_atlas font_;
    draw_list  dl_;
    strata::style style_;
    std::vector<font_source> font_sources_;
    u32  max_atlas_size_{4096};
    f32  scale_{1.0f};
    u32  font_generation_{1};

    u64  frame_{};
    f64  time_{};
    f32  dt_{1.0f / 60.0f};       // animation step: the frame delta, at most 0.1 s
    f32  wall_dt_{1.0f / 60.0f};  // timers: the full frame delta
    f32  caret_blink_{0.53f};     // input_state::caret_blink_time
    f64  double_click_{0.35};     // input_state::double_click_time
    f64  next_wake_{};            // see next_wake_seconds()
    vec2 display_{};

    internal::input_frame input_;

    clipboard_hooks                       clipboard_{};
    diagnostics_hook                      diag_{};
    std::vector<u64>                      diag_seen_; // what was reported already (a few dozen at most)

    id active_{};
    cursor_kind cursor_{};
    bool        wheel_consumed_{}; // an inner scroller used this frame's wheel
    bool        wheel_x_consumed_{};

    window_state* cur_{};
    id            cur_window_{};
    layout_state  layout_{};

    internal::id_stack       ids_;
    internal::style_stack    style_stack_;
    internal::window_registry win_;

    // draw-command runs of this frame, in emission order
    std::array<cmd_run, max_runs> runs_{};
    u32                           run_count_{};
    u32                           run_owner_{run_base};
    u32                           run_start_{};
    bool                          runs_overflow_{};
    u32                           layer_saved_owner_{run_base}; // run to return to when a layer scope ends
    bool                          in_layer_{};


    // number widgets
    id            number_edit_{};       // the drag field being typed into
    std::string   number_buf_;
    f32           drag_frac_{};         // integer drags: the part of a step not applied yet
    vec2          drag_start_{};        // where the drag field was pressed
    bool          drag_moved_{};        // left the dead zone: a drag, not a click
    bool          input_rect_set_{};    // next input_core uses this rect instead of laying out
    rect          input_rect_{};

    // selectable text
    u32           selectable_depth_{};
    color         ml_color_{0, 0, 0, 0}; // text colour for the next input_multiline_core (alpha 0: theme)
    internal::chart_state chart_;
    internal::modal_state modal_;
    bool          next_window_menubar_{};

    internal::menu_state menu_;

    internal::toast_state toast_;

    // docking animation
    bool          dock_animation_{true};
    // smooth scrolling: wheel notches still to apply
    bool          scroll_smoothing_{true};

    // focus/press gates read by every widget kind (nav, menus, modals, rich-text links), not just text fields
    id          focus_id_{};
    bool        focus_seen_{};
    bool        press_claimed_{};

    internal::edit_state edit_;

    // table layouts loaded early, applied when the table first draws
    std::vector<std::pair<id, std::string>> table_pending_;
    // load_state scroll offsets (y, x), applied when the region appears
    std::vector<std::pair<id, vec2>>        scroll_pending_;

    // tab bars: cell scratch for the bar being built, and the tab being dragged
    std::vector<rect> tab_cells_;
    std::vector<f32>  tab_emph_;
    id                tabdrag_tab_{};
    id                tabdrag_cand_{};
    f32               tabdrag_grab_{};
    f32               tabdrag_press_x_{};

    // input masks and code fields: extra behaviour for the field being built
    std::string_view edit_mask_;         // input_masked: the mask (only while the call runs)
    id               focus_request_{};   // field to focus when next drawn
    internal::code_mode code_;
    std::vector<internal::code_state>      code_states_;
    std::vector<std::pair<u32, u32>>       code_marks_;

    internal::date_picker_state date_;

    // generic popups (open_popup): this frame's popup is only being measured (its size is kept per popup_level)
    bool gpopup_hidden_{};

    internal::drag_drop_state dnd_;
    rect            last_item_rect_{};

    internal::popup_stack popup_;
    bool in_overlay_{};
    bool swallow_press_{};     // this frame's press only closed a popup: ignore it

    // widgets under the pointer that must not react: popup content is blocked by the levels above it and by an open
    // menu (unless it is that menu's own content); window content by every popup and menu. other overlay content
    // (tooltips, toasts, menus outside popups) is not blocked.
    [[nodiscard]] bool pointer_blocked() const noexcept
    {
        const bool popup_content = popup_.popup_depth_ > 0;
        if (in_overlay_ && !popup_content) { return false; }
        return popup_.popup_covers(input_.mouse_) || (menu_.menu_hit_prev_ && menu_.menu_depth_ == 0);
    }

    internal::color_picker_state pick_;

    // trees, tables
    internal::table_tree_state tree_table_;

    internal::rich_text_state rich_;

    // docking
    std::unique_ptr<internal::dock_state>  dock_;
    bool                                   dock_chrome_cur_{};
    bool                                   dock_chrome_prev_{};
    bool                                   hovered_docked_cur_{};
    bool                                   hovered_docked_prev_{};
    bool                                   window_faded_{};   // window being built pushed an alpha for end_window to pop
    internal::child_card_state children_cards_;

    // flags and frame of the window being built (for drag_by_body)
    window_flags cur_flags_{};
    rect         cur_frame_{};

    // instrumentation: the frame being built, and the one stats() reports
    frame_stats stats_cur_{};
    frame_stats stats_prev_{};
    f64         begin_frame_ms_{};   // how long the last begin_frame took
    f64         frame_clock_{};      // when it started

    // idling: hash of everything the renderer sees. 0 = no previous frame (invalidate() forces a change).
    u64  geometry_hash_{};
    bool frame_unchanged_{};
    bool anim_settling_{};
    // set by approach() when short of its target: next frame will differ. mutable because approach() is const.
    mutable bool anim_moved_{};

    internal::measure_cache_state measure_;

    // items submitted over an earlier allow_item_overlap() one
    id   overlap_key_{};        // the item that offered its rectangle, this frame
    rect overlap_rect_{};
    bool overlap_stolen_{};     // a later item took the press from it
    id   overlap_taken_cur_{};  // ... or merely hovering it: its hover is suppressed next frame
    id   overlap_taken_prev_{};

    // set_next_item_open: 0 none, 1 open, 2 close, 3 open recursively, 4 close recursively
    u8   next_open_{};
    // open_all / close_all_tree_nodes or a recursive arrow click: 0 none, 1 open, 2 close
    u8   tree_bulk_{};
    id   tree_bulk_seed_{};
    u32  tree_bulk_frames_{};
    internal::nav_state nav_{};
    std::array<f32, max_gutter_depth> gutter_stack_{};
    u32                               gutter_depth_{};
    u32                                disabled_depth_{};
    bool                               disabled_alpha_{};
    std::array<bool, max_disabled_depth> disabled_stack_{};
    u32                                disabled_count_{};

    // window owning the keyboard: the last one pressed in, docked included (focused_window_ is only the topmost)
    id   key_window_{};

    internal::row_accessory_state accessory_;
    internal::confirm_state confirm_;

    internal::combo_filter_state combo_;

    // tooltips: hover target
    id   last_item_key_{};
    bool last_item_hovered_{};
    bool last_item_focused_{};  // ... and it is where the nav cursor is
    bool last_item_pressed_{};
    bool last_item_double_{};   // the press ending on it was a double click
    bool last_item_arrow_{};    // ... and it landed on a tree row's arrow
    bool last_item_truncated_{}; // the last label did not fit and was cut
    // widget double clicks, separate from register_click() (text fields' 1 / 2 / 3 run)
    id   item_click_key_{};
    f64  item_click_time_{-10.0};
    vec2 item_click_pos_{};
    bool item_dbl_pending_{};
    id   hover_key_cur_{};
    id   hover_key_prev_{};
    f32  hover_time_{};
    edit_flags         edit_flags_{};
    id                 edit_item_{};
    std::array<edit_session, 4> edit_sessions_{}; // open ones (key 0 = free); focus survives a slider drag
    u32                edit_muted_{};
    std::array<id, 8>  engaged_cur_{};
    std::array<id, 8>  engaged_prev_{};
    u32                engaged_cur_count_{};
    u32                engaged_prev_count_{};

    // hotkey binding: m_->input_.pressed_key_ / press_* is this frame's press and its modifiers (a queued
    // press may be handled a frame late); unhandled presses wait in m_->input_.press_queue_
    internal::hotkey_state hotkey_;

    internal::animation                   anim_;
};

// a composite reports for its parts: they stay quiet while this lives
struct context::edit_mute {
    context& ctx;
    explicit edit_mute(context& c) noexcept : ctx{c} { ++ctx.m_->edit_muted_; }
    ~edit_mute() { --ctx.m_->edit_muted_; }
    edit_mute(const edit_mute&)            = delete;
    edit_mute& operator=(const edit_mute&) = delete;
};

} // namespace strata

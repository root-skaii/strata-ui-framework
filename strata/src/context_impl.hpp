#pragma once

// internal: strata::context's private state, held by pointer so it can change without touching the public header.

#include "strata/context.hpp"

#include "dock_state.hpp"

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

struct context::anim_slot {
    id   key{};
    u64  last_frame{};
    f32  hover{};
    f32  active{};
    f32  toggle{};
    f32  custom{};
    bool custom_init{};
};

struct context::window_state {
    id   key{};
    vec2 pos{};
    f32  width{};
    f32  height{};      // 0: follows the content
    f32  content_h{};
    f32  scroll{};
    f32  grab{};        // scrollbar: where in the thumb it was pressed
    bool collapsed{};
    bool resizable{};
    bool size_set{};
    bool overflow{};    // content taller than the body last frame (scrolling windows)
    f32  capped_h{};    // auto-height window capped at the display: shown height, 0 = not capped
    // docking
    u32  dock{};        // 1 + index of the dock node the window sits in, 0 = floating
    u32  dock_order{};  // position among the tabs of its node
    vec2 float_size{};  // size to go back to when un-docked
    bool docked_now{};  // drawn as a docked window this frame
    f32  ghost{};       // 0..1: see-through while carried over a dock target
    f32  title_h{};
    u64  last_frame{};
    id   dock_owner{};     // in a floating dock: that dock's window (they stack together)
    bool menubar{};        // the main menu bar (no padding, above the other windows)
    u32  modal_level{};    // 1 + index in the modal stack, 0 = not a modal
    u8   title_len{};
    std::array<char, 48> title{}; // visible part of the title, for dock tabs
};

struct context::layout_state {
    vec2 origin{};
    f32  width{};
    f32  cursor_x{};
    f32  line_top{};
    f32  line_h{};
    f32  bottom{};
    f32  next_width{};
    f32  bound_bottom{}; // bottom limit for fill-height children / strips (0 = none)
    f32  right{};        // the furthest right edge of anything placed
    f32  gutter{};       // taken off `width` by push_right_gutter (same_line_right adds it back)
    bool same_line{};
    bool first{true};
};

struct context::saved_color {
    style_color which{};
    color       previous{};
};

struct context::saved_var {
    style_var which{};
    f32       previous{};
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

struct context::tree_frame {
    f32 arrow_x{};
    f32 children_top{};
    f32 saved_origin_x{};
    f32 saved_width{};
};

struct context::tree_state {
    id   key{};
    id   seed{};  // id scope it was submitted in (parent node), for recursive open / close
    bool open{};
};

struct context::table_state { // persists across frames
    id                                 key{};
    u64                                last_frame{};
    u32                                columns{};
    std::array<f32, max_table_columns> frac{}; // column widths as fractions of the table width
    std::array<u8, max_table_columns>  order{}; // the column shown at each position (reorderable tables)
    u16                                hidden{}; // a bit per column
    u8                                 press_col1{}; // 1 + header pressed / being dragged, 0 = none
    u8                                 drag_col1{};
    f32                                press_x{};
    f32                                row_hint{};
    f32                                scroll{};
    f32                                grab{};
    f32                                content_h{};
    bool                               inited{};
};

struct context::table_column {
    std::string_view   label;
    f32                fixed{};
    f32                weight{1.0f};
    table_column_flags flags{};
};

struct context::table_frame { // the table being built this frame
    bool                                      active{};
    table_state*                              state{};
    table_flags                               flags{};
    u32                                       ncols{};
    u32                                       setup_count{};
    std::array<table_column, max_table_columns> cols{};
    std::array<f32, max_table_columns + 1>    col_x{};   // the edges of the visible columns, left to right
    std::array<f32, max_table_columns>        x0{}, x1{}; // column rects by declared index (hidden: empty)
    std::array<u8, max_table_columns>         vis{};     // declared index of each visible column, left to right
    u32                                       nvis{};
    f32                                       frac_total{1.0f}; // the widths of the visible columns add up to this
    u32                                       tree_depth{};
    f32                                       height_limit{};
    vec2                                      origin{};
    f32                                       width{};
    layout_state                              outer{};
    f32                                       pad_x{}, pad_y{}, min_row_h{};
    f32                                       row_y{};
    f32                                       row_top{}, row_bottom{};
    u32                                       row_index{};
    int                                       col{-1};
    bool                                      columns_ready{};
    bool                                      header_done{};
    bool                                      body_started{};
    bool                                      in_row{};
    bool                                      row_visible{};
    bool                                      row_had_content{};
    bool                                      scroll_mode{};
    bool                                      clip_pushed{};
    bool                                      cell_clip{}; // the current cell has its own clip rectangle
    f32                                       body_top{}, body_h{};
};

struct context::child_state { // persists across frames
    // horizontal counterparts of scroll / grab / content_h / overflow (child_flags::horizontal)
    f32  scroll_x{};
    f32  grab_x{};
    f32  content_w{};
    bool overflow_x{};
    id   key{};
    u64  last_frame{};
    f32  scroll{};
    f32  grab{};
    f32  content_h{};
    bool overflow{};
};

struct context::child_frame { // the child being built
    child_state* state{};
    rect         bounds;
    rect         inner;
    layout_state outer{};
    child_flags  flags{};
};

struct context::card_state {
    id   key{};
    u64  last_frame{};
    f32  content_h{};
};

struct context::card_frame {
    card_state*  state{};
    layout_state outer{};
};

// undo history of the focused field
enum class context::edit_kind : u8 { other, typing, erase_back, erase_fwd };

struct context::edit_op {
    std::size_t pos{};      // where the change happened
    std::size_t off{};      // in history text: removed bytes, then inserted
    std::size_t rem_len{};
    std::size_t ins_len{};
    std::size_t cursor_before{};
    std::size_t anchor_before{};
    std::size_t cursor_after{};
    edit_kind   kind{};
    f64         time{};
};

struct context::edit_history {
    std::vector<edit_op> ops;
    secure_string        text; // typed text lives here so it is zeroed like the edit buffer
};

struct context::ml_line {
    u32 start{}; // byte range of the line, without its newline
    u32 end{};
};

// rich text layout scratch
struct context::rich_run {
    std::string_view text;
    font_id          font{};
    color            col;
    text_flags       style{};
    std::string_view link;      // href of the enclosing <a=...>, empty if not a link
    bool             own_col{}; // a <c=> in the link set the colour: keep it
};

struct context::rich_seg {
    std::string_view text;
    font_id          font{};
    color            col;
    text_flags       style{};
    f32              x{};
    std::string_view link;
    bool             own_col{};
};

struct context::rich_line {
    u32 first{};
    u32 count{};
    f32 width{};
    f32 asc{};
    f32 height{};
    f32 y{};
};

struct context::menu_level {
    id   key{};
    vec2 pos{};       // where the popup wants to open
    rect anchor{};    // the opener: pressing it is not "outside"
    vec2 size{};      // measured last frame
    rect rect_cur{};
    rect rect_prev{};
    f32  age{};
    bool seen{};      // built this frame
    bool from_bar{};
};

struct context::menu_frame { // a popup being built
    layout_state saved_layout{};
    u32          prev_owner{};
    f32          saved_spacing{};
    f32          content_w{};
    u32          level{};
    bool         saved_overlay{};
    bool         keys_ok{};   // the deepest open popup: letter keys activate mnemonics here
};

struct context::toast_entry {
    std::string title;
    std::string text;
    toast_kind  kind{};
    f32         duration{};
    f32         age{};
    f32         anim{};   // slide / fade in, 0..1
    bool        dismissed{};
    u64         seq{};
    std::vector<std::string> actions;
    f32         progress{-1.0f};
    bool        sticky{};       // no timer until it completes / is closed
    f32         busy_phase{};
    bool        paused{};       // the pointer is on it: its timer stands still
};

struct context::press_anim { f32 hover{}; f32 active{}; };

// a composite reports for its parts: they stay quiet while this lives

// what set_scale needs to rebuild the atlas (owned strings; font_config views re-pointed)
struct context::font_source {
    std::string face;
    std::string file;
    font_config cfg;
};

// zoomable chart views (survive between frames)
struct context::chart_view {
    id   key{};
    bool x_set{};
    bool y_set{};
    f32  x_lo{}, x_hi{}, y_lo{}, y_hi{}; // the user's view
    f32  sx_lo{}, sx_hi{}, sy_lo{}, sy_hi{}; // what was drawn last frame
    rect inner;                          // ... and where
    f64  last_click{-10.0};
    u64  last_frame{};
};

struct context::modal_frame {
    bool alpha_pushed{};
};

   // field to focus when next drawn
struct context::code_mode {
    bool                                          on{};
    code_flags                                    flags{};
    int                                           tab_size{4};
    std::span<const std::pair<u32, u32>>          marks{}; // find matches (start, length), sorted
    int                                           mark_active{-1};
    std::size_t                                   goto_offset{~std::size_t{0}};
};

struct context::code_state { // per code field, across frames
    id          key{};
    u64         last_frame{};
    bool        find_open{};
    bool        replace_open{};
    bool        match_case{};
    bool        open_request{}; // code_find(): give the find field the keyboard
    int         active{-1};
    int         want_line{};
    std::string find;
    std::string replace;
};

// ... and each id's label, for the report
struct context::id_label_slot {
    id                   key{};
    std::array<char, 48> text{};
    u8                   len{};
};

// label_size() cache: direct-mapped (font, string) -> size, cleared when the atlas changes. rich labels skip it.
struct context::measure_slot {
    u64  key{};   // 0 = empty
    vec2 size{};
};

   // bulk open also applies to nodes revealed later

// keyboard navigation (nav_begin / nav_end)
struct context::nav_item {
    id   key{};
    rect bounds{};
    u32  depth{};
    bool node{};
    bool open{};
};

struct context::nav_state {
    id                    scope{};      // scope being built, 0 outside nav_begin / nav_end
    id                    scope_key{};  // list owning the cursor (resets for another list)
    id                    cursor{};     // the row the cursor is on, kept across frames
    id                    activate_pending{}; // row Enter picked last frame, reported as a press
    bool                  active{};     // the scope has the keyboard (no text field has it)
    std::vector<nav_item> items;        // every row this frame, culled ones included
};

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
    f32  wheel_lines_{3.0f};      // input_state::wheel_lines
    f64  double_click_{0.35};     // input_state::double_click_time
    f64  next_wake_{};            // see next_wake_seconds()
    vec2 display_{};
    vec2 mouse_{};
    vec2 mouse_delta_{};
    f32  wheel_{};
    f32  wheel_x_{};   // horizontal wheel this frame (see input_state::wheel_x)
    bool mouse_down_{};
    bool mouse_pressed_{};
    bool mouse_released_{};
    bool have_mouse_{};

    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    clipboard_hooks                       clipboard_{};
    diagnostics_hook                      diag_{};
    std::vector<u64>                      diag_seen_; // what was reported already (a few dozen at most)

    id active_{};
    cursor_kind cursor_{};
    bool        wheel_consumed_{}; // an inner scroller used this frame's wheel
    bool        wheel_x_consumed_{};
    id hovered_window_prev_{};
    id hovered_window_cur_{};
    u32 hovered_z_{no_z};
    id  focused_window_{}; // topmost window of the previous frame

    window_state* cur_{};
    id            cur_window_{};
    layout_state  layout_{};

    std::array<id, max_id_depth + 1> id_stack_{};
    u32                              id_depth_{};

    std::array<saved_color, max_overrides> color_stack_{};
    std::array<saved_var, max_overrides>   var_stack_{};
    u32                                    color_depth_{};
    u32                                    var_depth_{};

    std::array<font_id, max_font_depth + 1> font_stack_{};
    u32                                     font_depth_{};

    // back-to-front window stacking, persistent across frames
    std::array<id, max_windows> z_order_{};
    u32                         z_count_{};
    // windows submitted this frame, in call order
    std::array<window_state*, max_windows> frame_windows_{};
    u32                                    frame_window_count_{};

    // draw-command runs of this frame, in emission order
    std::array<cmd_run, max_runs> runs_{};
    u32                           run_count_{};
    u32                           run_owner_{run_base};
    u32                           run_start_{};
    bool                          runs_overflow_{};

    // input extras
    bool          mouse_right_down_{};
    bool          mouse_right_pressed_{};
    bool          mouse_middle_down_{};
    bool          mouse_middle_pressed_{};
    bool          mod_ctrl_{};
    bool          mod_shift_{};
    bool          mod_alt_{};

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
    std::vector<vec2> plot_scratch_;
    std::array<chart_view, 16> chart_views_{};
    std::array<id, max_modals> modal_stack_{};
    u32           modal_count_{};
    u32           next_window_modal_level_{};
    bool          next_window_menubar_{};
    std::array<modal_frame, max_modals> modal_frames_{};
    u32           modal_depth_{};       // modals being built right now
    id            modal_top_prev_{};    // modal with the input (as of last frame)

    // menus
    std::array<menu_level, max_menu_levels> menu_open_{};
    std::array<menu_frame, max_menu_levels> menu_stack_{};
    u32           menu_depth_{};
    bool          menu_close_all_{};
    bool          menu_hit_prev_{};     // pointer over an open menu (last frame's rects)
    bool          in_menu_bar_{};
    f32           menu_bar_h_{};
    f32           menu_bar_saved_spacing_{};

    // toasts
    std::vector<toast_entry> toasts_;
    screen_corner toast_corner_{screen_corner::bottom_right};
    bool          toast_hover_cur_{};
    bool          toast_hover_prev_{};
    u64           toast_seq_{};

    // docking animation
    bool          dock_animation_{true};
    // smooth scrolling: wheel notches still to apply
    bool          scroll_smoothing_{true};
    f32           wheel_pending_{};
    f32           wheel_x_pending_{};
    bool          wheel_moving_{};

    // text input
    edit_history  undo_;
    edit_history  redo_;
    bool          edit_history_on_{};
    bool          edit_readonly_{};
    std::size_t   edit_max_bytes_{};
    u64           edit_version_{};     // bumped on every change of the edit buffer
    f32           edit_pref_x_{-1.0f}; // multi-line: column kept while moving up / down
    std::vector<ml_line> ml_lines_;
    u64           ml_cache_key_{};
    id          focus_id_{};
    bool        focus_seen_{};
    bool        press_claimed_{};
    bool        submitted_{};
    secure_string edit_buf_; // live text of the focused field; zeroed on blur
    std::size_t edit_cursor_{};
    std::size_t edit_anchor_{};
    f32         edit_scroll_{};
    f64         caret_time_{};
    f64         last_click_time_{-10.0};
    vec2        last_click_pos_{};
    u32         click_count_{};

    // styled contents (input_spans) for the field being built
    std::vector<text_span> edit_spans_pending_;
    std::vector<text_span> edit_spans_;
    u64         edit_spans_hash_{};

    // IME: composition (from input_state) and what the focused field reports to the host
    std::array<char, 256> ime_text_{};
    u32         ime_len_{};
    u32         ime_cursor_{};
    bool        ime_want_{};
    vec2        ime_pos_{};
    f32         ime_line_h_{};

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
    id               focus_request_{};
    code_mode code_;
    std::vector<code_state>                code_states_;
    std::vector<std::pair<u32, u32>>       code_marks_;

    // date picker: shown month and owning popup
    id  cal_key_{};
    i32 cal_year_{};
    i32 cal_month_{};

    // generic popups (open_popup): opener and last frame's content size
    rect gpopup_anchor_{};
    vec2 gpopup_size_{};
    id   gpopup_key_{};       // the popup gpopup_size_ was measured for
    bool gpopup_hidden_{};    // this frame's popup is only being measured

    // drag and drop
    bool            dd_active_{};     // a payload is being dragged
    bool            dd_cancelled_{};  // Esc: ignored until the button is let go
    id              dd_source_{};     // the widget it was picked up from
    id              dd_candidate_{};  // pressed widget that becomes a drag source once moved
    vec2            dd_press_pos_{};
    std::string     dd_type_;
    std::vector<u8> dd_data_;
    vec2            dd_size_{};       // the preview panel, measured last frame
    bool            dd_hidden_{};
    bool            dd_saved_overlay_{};
    u32             dd_prev_owner_{};
    layout_state    dd_saved_layout_{};
    rect            last_item_rect_{};

    // popup (combo)
    id   popup_id_{};
    bool in_overlay_{};
    bool popup_open_cur_{};
    bool popup_open_prev_{};
    rect popup_rect_cur_{};
    rect popup_rect_prev_{};
    rect popup_anchor_cur_{};  // the combo box that owns the popup
    rect popup_anchor_prev_{};
    bool swallow_press_{};     // this frame's press only closed a popup: ignore it
    f32  popup_scroll_{};
    int  popup_hover_{-1};

    // color picker (the one being edited)
    id    pick_key_{};
    f32   pick_h_{}, pick_s_{}, pick_v_{};
    color pick_last_{};
    // generic popup content
    layout_state popup_saved_layout_{};
    u32          popup_prev_owner_{run_base};

    // trees, tables
    std::array<tree_frame, max_tree_depth> tree_stack_{};
    u32                                    tree_depth_{};
    std::vector<tree_state>                tree_states_; // sorted by key
    bool                                   item_pressed_{};
    std::array<table_state, max_tables>    tables_{};
    table_frame                            table_{};
    std::array<table_frame, max_table_depth> table_stack_{}; // the tables around the current one
    u32                                    table_depth_{};

    // rich text
    u32                                    rich_depth_{};
    std::vector<rich_run>                  rich_runs_;
    std::vector<rich_seg>                  rich_segs_;
    std::vector<rich_line>                 rich_lines_;

    // docking
    std::unique_ptr<internal::dock_state>  dock_;
    bool                                   dock_chrome_cur_{};
    bool                                   dock_chrome_prev_{};
    bool                                   hovered_docked_cur_{};
    bool                                   hovered_docked_prev_{};
    bool                                   window_faded_{};   // window being built pushed an alpha for end_window to pop
    // child regions, cards
    std::array<child_state, max_children>  children_{};
    std::array<child_frame, max_child_depth> child_stack_{};
    u32                                    child_depth_{};
    std::array<card_state, max_cards>      cards_{};
    std::array<card_frame, max_card_depth> card_stack_{};
    u32                                    card_depth_{};

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

    // rich-text links. reported hrefs are copied out of the caller's markup into reused strings. `rich_links_live_`
    // is set only by rich_text / rich_text_wrapped: links in captions must not steal the widget's click.
    std::string rich_clicked_;
    std::string rich_hovered_;
    bool        rich_links_live_{}; // power of two
    std::vector<id>       id_seen_;
    id                    collision_id_{};
    std::array<char, 64>  collision_label_{};
    u32                   collision_label_len_{}; // power of two
    std::vector<id_label_slot> id_labels_; // power of two
    std::vector<measure_slot> measure_cache_;
    u32                       measure_cache_gen_{};

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
    nav_state nav_{};
    std::array<f32, max_gutter_depth> gutter_stack_{};
    u32                               gutter_depth_{};
    u32                                disabled_depth_{};
    bool                               disabled_alpha_{};
    std::array<bool, max_disabled_depth> disabled_stack_{};
    u32                                disabled_count_{};

    // keys held this frame, from input_state
    std::array<u8, 32> keys_held_{};
    // window owning the keyboard: the last one pressed in, docked included (focused_window_ is only the topmost)
    id   key_window_{};

    // row accessories: the row they attach to (last selectable / tree row / custom_item, not the last item, which may
    // be an accessory itself)
    f32  next_gutter_{};
    id   row_anchor_{};
    rect row_anchor_rect_{};
    id   accessory_row_{};   // the row the accessories laid out so far belong to
    f32  accessory_x_{};     // left edge, moved left by each accessory
    rect accessory_row_rect_{};

    // ask_confirm / confirm state
    id          confirm_key_{};
    u64         confirm_data_{};
    std::string confirm_message_;
    int         confirm_answer_{};   // set when a button is pressed, read once by confirm()
    id          confirm_answer_key_{};
    id          confirm_open_{};     // the confirm() whose modal is up
    bool        confirm_pending_{};  // ask_confirm() ran; the next confirm() with this id opens
    bool        confirm_remember_{}; // "don't ask again" checkbox state while open

    // combo_filtered: search text, filtered rows and the keyboard row
    std::string      combo_filter_;
    int              combo_filter_hover_{};
    bool             combo_filter_focus_{};
    std::vector<u32> combo_filter_hits_;

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

    // hotkey binding. pressed_key_ / press_* = this frame's press and its modifiers (a queued press may be handled a
    // frame late); unhandled presses wait in the queue
    u32  pressed_key_{};
    bool press_ctrl_{};
    bool press_shift_{};
    bool press_alt_{};
    std::array<key_press, 64> press_queue_{};
    u32  press_queued_{};
    id   hotkey_capture_{};
    bool hotkey_seen_{};
    // hotkey_sequence(): steps captured so far by the capturing field (hotkey_capture_ keeps it to one)
    std::array<key_chord, key_sequence::max_steps> seq_edit_capture_{};
    u8   seq_edit_count_{};
    f64  seq_edit_deadline_{};
    // sequence_pressed(): prefix matched so far across the sequences asked about each frame. mutable so it can stay
    // const like chord_pressed
    mutable std::array<key_chord, key_sequence::max_steps - 1> seq_pending_{};
    mutable u8   seq_pending_count_{};
    mutable f64  seq_pending_time_{};
    mutable bool seq_pending_touched_{}; // a call advanced / completed the prefix this frame (see begin_frame)

    std::array<window_state, max_windows> windows_{};
    std::vector<anim_slot>                anims_;     // open addressing, power-of-two size, grows on demand
    u32                                   anim_used_{};
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

#pragma once

// internal: helpers of strata::context used by more than one src/context_*.cpp, as free functions so changing
// one does not rebuild everything that includes strata/context.hpp. helpers used by one file are declared at the
// top of that file.

#include "context_impl.hpp"

namespace strata::internal {

// context.cpp
// widget id from a label in the current scope; debug builds also record the label for collision reports
[[nodiscard]] id widget_id(context& ui, std::string_view label) noexcept;
[[nodiscard]] anim_slot& anim_for(context& ui, id key) noexcept; // finds or creates (and keeps alive)
[[nodiscard]] anim_slot* anim_find(context& ui, id key) noexcept; // finds only
// idle buttons own no animation state
[[nodiscard]] press_anim button_anim(context& ui, id key, const interaction& in) noexcept;
[[nodiscard]] f32 approach(const context& ui, f32 current, f32 target, f32 speed = 0.0f) noexcept;
// report edit state: `session` id, `changed` return value, `engaged` held / typed / popup open now.
// feeds item_activated() ... item_deactivated_after_edit()
void track_edit(context& ui, id session, bool changed, bool engaged) noexcept;
// a fixed-size table / stack ran out: counted in frame_stats, reported once per `what`
void report_limit(context& ui, const char* what, u32 capacity) noexcept;
[[nodiscard]] rect layout_place(context& ui, vec2 size) noexcept;
// rows outside the clip rect do no work (no hit test, animation, measuring or geometry); layout already advanced,
// so scrolling and tree state are unaffected
[[nodiscard]] bool item_culled(context& ui, const rect& r) noexcept;
// bookkeeping for a culled row: it becomes the inert "last item"
void note_culled_item(context& ui, id key, const rect& r) noexcept;
[[nodiscard]] vec2 measure_cached(context& ui, font_id f, std::string_view s) noexcept;
[[nodiscard]] f32 wheel_scroll(const context& ui, f32 unit, f32 page = 0.0f) noexcept;
[[nodiscard]] f32 wheel_scroll_x(const context& ui, f32 unit, f32 page = 0.0f) noexcept;
[[nodiscard]] field_layout layout_field(context& ui, std::string_view shown_label, f32 control_height);
[[nodiscard]] interaction interact(context& ui, id key, const rect& r) noexcept;
[[nodiscard]] shape_style widget_shape(const context& ui, color base, f32 radius) noexcept;
void switch_run(context& ui, u32 owner) noexcept;
// the scrollbar of child regions, windows and tables, along `track` (its hit area); returns the new offset.
// `grab` persists the press point in the thumb between frames
[[nodiscard]] f32 scrollbar(context& ui, id key, const rect& track, bool horizontal, f32 view, f32 content, f32 scroll, f32& grab);
[[nodiscard]] f32 layout_next_y(const context& ui) noexcept;
void wipe_edit_buffer(context& ui) noexcept;
[[nodiscard]] interaction interact_impl(context& ui, id key, const rect& r, bool in_window) noexcept;

// context_access.cpp
[[nodiscard]] id current_seed(const context& ui) noexcept;
// a row accessories can attach to (see row_anchor_)
void note_row_anchor(context& ui, id key, const rect& r) noexcept;
// caret blink phase (always on when blinking is disabled)
[[nodiscard]] bool caret_visible(const context& ui) noexcept;

// context_code.cpp
void apply_input_mask(context& ui);
bool edit_indent_lines(context& ui, bool unindent, int tab_size);

// context_dock.cpp
[[nodiscard]] dock_target dock_pick(const context& ui, vec2 pointer, u8 exclude_leaf = no_node, vec2 want = {}) noexcept; // want: the size the window has
void dock_end_frame(context& ui);

// context_input.cpp
bool input_core(context& ui, std::string_view label, std::string_view current, std::string_view hint, input_flags flags,
    std::size_t max_bytes);

// context_menu.cpp
void menu_close_from(context& ui, u32 level) noexcept;

// context_nav.cpp
// scroll offset / view of the innermost scrolling region, or nullptr
[[nodiscard]] f32* scroll_slot(context& ui, rect& view) noexcept;
// nav: every row of an open nav scope registers here, culled or not
void nav_record(context& ui, id key, const rect& r, u32 depth, bool node, bool open) noexcept;
[[nodiscard]] bool nav_take(context& ui, id key) noexcept; // Enter was pressed with the cursor on this row
void nav_click(context& ui, id key) noexcept; // a click moves the cursor
void push_id_value(context& ui, id key) noexcept; // an id that is already hashed (a row's own key)

// context_popup.cpp
[[nodiscard]] bool pointer_over(const context& ui, const rect& r) noexcept;
bool begin_popup_at(context& ui, id key, const rect& anchor, vec2 size);
void end_popup_at(context& ui);
// popup stack: opens `key` above the popup being drawn (from outside every popup: replaces the stack). false when
// the stack is full
bool popup_push(context& ui, id key) noexcept;
// enters the draw layer of `level` (records its rect as drawn this frame); popup_leave() restores what was before
void popup_enter(context& ui, u32 level, const rect& r, const rect& anchor) noexcept;
void popup_leave(context& ui) noexcept;
void draw_tooltip(context& ui, std::string_view text);
// popup background: the opaque shape, or acrylic when style_.popup_acrylic > 0
void popup_panel(context& ui, const rect& r, const shape_style& body);

// context_rich.cpp
// rich_layout fills rich_lines_ / rich_segs_ and returns the size; rich_draw paints them
vec2 rich_layout(context& ui, std::string_view text, font_id base, color base_col, f32 wrap_width, bool markup);
void rich_draw(context& ui, vec2 pos);
[[nodiscard]] vec2 label_size(context& ui, font_id f, std::string_view s);
void label_draw(context& ui, vec2 pos, color c, std::string_view s, font_id f);

// context_state.cpp
void apply_pending_scroll(context& ui, id key, f32& scroll_y, f32* scroll_x) noexcept;

// context_table.cpp
void table_finalize_columns(context& ui);
void table_start_body(context& ui);
void table_finish_row(context& ui);
[[nodiscard]] std::string table_layout_text(const table_state& t);

// context_text.cpp
// all edits of the focused field go through edit_replace (records undo)
bool edit_replace(context& ui, std::size_t pos, std::size_t len, std::string_view with, edit_kind kind);
bool edit_delete_selection(context& ui);
bool edit_insert(context& ui, std::string_view s, bool typed);
bool edit_shortcut(context& ui, const key_event& ev, bool password, bool multiline); // ctrl+a / c / x / v / z / y
void edit_history_clear(context& ui) noexcept;
bool input_multiline_core(context& ui, std::string_view label, std::string_view current, vec2 size, input_flags flags,
    std::string_view hint, std::size_t max_bytes);
[[nodiscard]] u32 register_click(context& ui) noexcept; // 1 / 2 / 3 = single / double / triple click
void ed_prepare_spans(context& ui, std::size_t text_size, font_id base, f32& line_h, f32& ascent, bool ignore);
[[nodiscard]] f32 ed_measure(const context& ui, font_id base, std::string_view t, std::size_t a, std::size_t b);
void ed_draw(context& ui, vec2 pos, f32 line_ascent, color col, std::string_view t, std::size_t a, std::size_t b, font_id base);

// context_toast.cpp
void toast_end_frame(context& ui);

// context_tree.cpp
void tree_open_set(context& ui, id key, bool open) noexcept;
[[nodiscard]] bool& tree_open_state(context& ui, id key, bool default_open);

// context_window.cpp
[[nodiscard]] window_state* window_for(context& ui, id key, vec2 pos, f32 width) noexcept;
[[nodiscard]] u32 z_index(const context& ui, id key) noexcept;
void bring_to_front(context& ui, id key) noexcept;
[[nodiscard]] window_state* window_find(context& ui, id key) noexcept;
[[nodiscard]] const window_state* window_find(const context& ui, id key) noexcept;

inline void menu_close_all(context& ui) noexcept { menu_close_from(ui, 0); }

} // namespace strata::internal

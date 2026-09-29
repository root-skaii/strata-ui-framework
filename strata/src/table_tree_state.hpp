#pragma once

// internal: trees and tables, behind context::impl::tree_table_. context::tree_frame/tree_state/table_state/
// table_column/table_frame (declared in context.hpp) alias the types below, so context_data.cpp's and
// context_state.cpp's unqualified uses need no change. context_data.cpp owns the logic.

#include "strata/context.hpp"

#include "core/layout_state.hpp"

#include <array>
#include <vector>

namespace strata::internal {

inline constexpr u32 max_table_columns = 16; // (context::max_table_columns is the same)

struct tree_frame {
    f32 arrow_x{};
    f32 children_top{};
    f32 saved_origin_x{};
    f32 saved_width{};
};

struct tree_state {
    id   key{};
    id   seed{};  // id scope it was submitted in (parent node), for recursive open / close
    bool open{};
};

struct table_state { // persists across frames
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
    f32                                scroll_wanted{-1.0f}; // table_set_scroll_y(), for the next body; < 0 none
    f32                                grab{};
    f32                                content_h{};
    bool                               inited{};
};

struct table_column {
    std::string_view   label;
    f32                fixed{};
    f32                weight{1.0f};
    table_column_flags flags{};
};

struct table_frame { // the table being built this frame
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
    f32                                       row_hint{}; // this row's expected height (from row_had_content ? the
                                                          // previous row : last frame's), seeded into each cell
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

struct table_tree_state {
    std::array<tree_frame, 16> tree_stack_{};  // (context::max_tree_depth is the same)
    u32                        tree_depth_{};
    std::vector<tree_state>    tree_states_; // sorted by key
    bool                       item_pressed_{}; // last tree row / selectable was clicked

    std::array<table_state, 32> tables_{};      // (context::max_tables is the same)
    table_frame                 table_{};       // the table being built this frame
    std::array<table_frame, 4>  table_stack_{}; // (context::max_table_depth is the same) the tables around the current one
    u32                         table_depth_{};
};

} // namespace strata::internal

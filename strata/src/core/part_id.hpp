#pragma once

// internal: ids for the pieces of a widget (its close box, a scrollbar, a resize edge ...), derived from the
// widget's own id. a named part instead of hashing a "##salt" string every frame: a typo is a compile error, and
// the pieces are easy to find. the values only have to differ from each other; nothing persists them.

#include "strata/types.hpp"

namespace strata::internal {

enum class part : u32 {
    // windows
    collapse_arrow = 1, title_drag, body_drag, resize_right, resize_bottom, resize_corner, window_scrollbar,
    // docking
    dock_tab, dock_splitter, dock_grip, edge_splitter,
    // row accessories
    accessory_button, accessory_checkbox, accessory_toggle,
    // text fields
    clear_button, reveal_button, text_scrollbar,
    // children, tables, tabs, logs
    child_scrollbar, child_scrollbar_x, table_scrollbar, tab_scroll, tab_close, tab_add, tab_list, log_rows,
    // number fields, colour picker
    step_down, step_up, sat_value, hue_bar, alpha_bar,
    // pickers, lists, menus, popups
    year_month, calendar_day, select_all, select_none, popup_menu, modal_fade, column_grip, chip_close,
    // toasts
    toast, toast_button, toast_close,
};

// the id of `p` inside the widget `owner` (splitmix64 finaliser over both: well spread, never 0)
[[nodiscard]] constexpr id part_id(part p, id owner) noexcept
{
    u64 z = owner ^ (static_cast<u64>(p) * 0x9e3779b97f4a7c15ull);
    z     = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z     = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return z != 0 ? z : 1u;
}

} // namespace strata::internal

namespace strata { using internal::part; using internal::part_id; }

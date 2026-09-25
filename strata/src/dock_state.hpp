#pragma once

// internal (not installed): the state of the docking system. context keeps it behind a pointer (context::dock_), so this can
// change without recompiling everything that includes strata/context.hpp; only context_dock.cpp and context.cpp include it.

#include "strata/context.hpp"

namespace strata::internal {

inline constexpr u32 max_dock_nodes  = 32;
inline constexpr u32 max_dock_spaces = 8;
inline constexpr u8  no_node         = 0xff; // (context::no_node is the same)

// dock tree: leaves hold windows as tabs, split nodes divide their area between two children
struct dock_node {
    bool used{};
    u8   parent{no_node};
    std::array<u8, 2> child{no_node, no_node}; // both no_node: a leaf
    bool vertical{};   // false: children side by side, true: stacked
    f32  ratio{0.5f};  // share of the first child
    id   active{};     // leaf: the selected tab (window key)
    rect area;         // this frame: the whole node (the layout's target)
    rect content;      // this frame, leaf: below the tab bar
    rect shown_area;   // what is drawn: follows `area`, animated when dock animation is on
    rect shown_content;
    bool fresh{true};  // no shown_* yet
    u8   space{0};     // the dock space this node belongs to
    [[nodiscard]] bool leaf() const noexcept { return child[0] == no_node; }
};

// where a dragged window would go
struct dock_target {
    bool      valid{};
    u8        space{no_node};
    u8        node{no_node}; // no_node: the space is empty
    dock_zone zone{dock_zone::center};
    bool      outer{};       // splits the root instead of the node under the pointer
    rect      preview;
    f32       share{};       // splits: the part of the pane / space the new window takes
    u8        leaf{no_node}; // the pane under the pointer (its drop guides are shown), even when nothing can be dropped there
    bool      on_guide{};    // the pointer is on one of the drop guides
    int       tab{-1};       // joining a pane's tabs: the place among them (-1 = last)
    rect      marker;        // ... and where that is in the tab bar
};

// the buttons shown while a window is dragged over a pane: a cross in its middle, and one per border of the space
struct dock_guide {
    dock_zone zone{dock_zone::center};
    bool      outer{};
    rect      r;
};

// a dock space: one tree of panes over a region. the main area, named areas, edge docks and floating docks are all spaces
struct dock_space {
    id   key{};          // 0 = free slot
    u8   root{no_node};
    rect area;           // where its panes are laid out this frame
    rect drop;           // where a dragged window is accepted (a strip for an empty edge dock)
    rect panel;          // the whole panel an empty edge dock would take: the drop preview
    f32  edge_size{};    // edge docks: their width / height
    id   owner{};        // a floating dock: the key of its window
    bool set{};          // given a rectangle this frame
    bool hidden{};       // a collapsed floating dock: what is docked in it is hidden
    bool edge{};
    u64  last_frame{};
};

struct dock_state {
    std::array<dock_node, max_dock_nodes>   nodes{};
    std::array<dock_space, max_dock_spaces> spaces{};
    bool        any_set{};        // some space got a rectangle this frame
    u32         counter{};
    dock_target target_cur{};
    dock_target target_prev{};
    id          drag_win{};       // the floating dockable window being dragged this frame
    id          drag_prev{};      // ... and in the previous one (a drop happens on the release frame)
    vec2        press_pos{};
    u8          group_src{no_node}; // a whole pane (its tabs) is being dragged by the grip of its tab bar
    bool        group_moved{};
    id          void_key{};       // a drag that Esc cancelled (window) or a double click replaced (splitter): does nothing until the button is up
    id          click_key{};      // last press on a splitter / tab, for double clicks
    f64         click_time{-10.0};
    vec2        click_pos{};
    bool        splitting{};      // a splitter is being dragged: panes follow the pointer without easing
};

} // namespace strata::internal

namespace strata {

using internal::dock_guide;
using internal::dock_node;
using internal::dock_space;
using internal::dock_target;
using internal::max_dock_nodes;
using internal::max_dock_spaces;

} // namespace strata

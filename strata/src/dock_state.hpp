#pragma once

// internal: docking state, behind context::dock_ so it can change without recompiling context.hpp users.
// included only by context_dock.cpp and context.cpp.

#include "strata/context.hpp"

namespace strata::internal {

inline constexpr u32 max_dock_nodes  = 32;
inline constexpr u32 max_dock_spaces = 8;
inline constexpr u8  no_node         = 0xff; // (context::no_node is the same)

// dock tree: leaves hold tabbed windows, splits divide their area between two children
struct dock_node {
    bool used{};
    u8   parent{no_node};
    std::array<u8, 2> child{no_node, no_node}; // both no_node: a leaf
    bool vertical{};   // false: children side by side, true: stacked
    f32  ratio{0.5f};  // share of the first child
    id   active{};     // leaf: the selected tab (window key)
    rect area;         // this frame's full node rect (layout target)
    rect content;      // this frame, leaf: below the tab bar
    rect shown_area;   // drawn rect: follows `area`, animated if enabled
    rect shown_content;
    bool fresh{true};  // no shown_* yet
    u8   space{0};     // the dock space this node belongs to
    [[nodiscard]] bool leaf() const noexcept { return child[0] == no_node; }
};

// drop target of a dragged window
struct dock_target {
    bool      valid{};
    u8        space{no_node};
    u8        node{no_node}; // no_node: the space is empty
    dock_zone zone{dock_zone::center};
    bool      outer{};       // split the root, not the hovered node
    rect      preview;
    f32       share{};       // splits: share taken by the new window
    u8        leaf{no_node}; // hovered pane (guides shown), even if nothing can drop
    bool      on_guide{};    // the pointer is on one of the drop guides
    int       tab{-1};       // tab join position (-1 = last)
    rect      marker;        // ... and where that is in the tab bar
};

// drop guides while dragging over a pane: centre cross plus one per space border
struct dock_guide {
    dock_zone zone{dock_zone::center};
    bool      outer{};
    rect      r;
};

// a dock space: one pane tree over a region (main, named, edge and floating docks alike)
struct dock_space {
    id   key{};          // 0 = free slot
    u8   root{no_node};
    rect area;           // where its panes are laid out this frame
    rect drop;           // accepting area (a strip for an empty edge dock)
    rect panel;          // full panel of an empty edge dock: the drop preview
    f32  edge_size{};    // edge docks: their width / height
    id   owner{};        // a floating dock: the key of its window
    bool set{};          // given a rectangle this frame
    bool hidden{};       // collapsed floating dock hides its contents
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
    id          drag_win{};       // floating dockable window dragged this frame
    id          drag_prev{};      // ... and last frame (drops happen on release)
    vec2        press_pos{};
    u8          group_src{no_node}; // a whole pane dragged by its tab-bar grip
    bool        group_moved{};
    id          void_key{};       // drag cancelled by Esc / replaced by a double click: inert until release
    id          click_key{};      // last splitter / tab press, for double clicks
    f64         click_time{-10.0};
    vec2        click_pos{};
    bool        splitting{};      // splitter drag: panes follow without easing
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

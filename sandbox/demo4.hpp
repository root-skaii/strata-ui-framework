#pragma once

// sandbox scenes for the tree / list work: the icon set (--scene icons), a deep tree that only pays for the rows in
// view (--scene bigtree), and the row ergonomics -- overlapping items, a right gutter, ellipsized labels and keyboard
// navigation (--scene rows). the state lives in demo4_state, a member of demo2_state.

#include <strata/strata.hpp>

#include <string>
#include <string_view>
#include <vector>

struct demo4_state {
    int  font_icons = -1;    // set by main, -1 when the icon font is not loaded
    int  font_mono  = -1;
    bool deterministic = false;

    // --scene icons: every icons:: constant with its name, and whether the font really has the glyph
    bool show_icons = false;
    int  icon_page  = -1;    // >= 0: show that 256-code-point page instead (a hex start), for picking code points

    // --scene bigtree: a generated namespace tree, all of it expanded
    bool show_bigtree = false;
    struct tree_node_data {
        std::string name;
        int         first_child = 0; // index into nodes, [first_child, first_child + children)
        int         children    = 0;
        int         depth       = 0;
    };
    std::vector<tree_node_data> nodes;   // node 0 is a sentinel root; its children are the top level
    int                         selected = -1;
    bool                        culling_note = true;

    // --scene rows: an inspector-like list of components
    bool show_rows = false;
    struct component {
        std::string name;
        std::string type;
        bool        enabled = true;
    };
    std::vector<component> components;
    int                    removed = -1;
    int                    row_selected = 0;
    std::string            filter;

    // --scene app: the things an app used to hand-roll -- row accessories, disabled items, a self-contained
    // confirmation, a filtered combo, multi-select, raw keys and a runtime ui scale
    bool                     show_app = false;
    std::vector<std::string> objects;
    strata::selection_state  sel;
    std::vector<std::string> types;      // a long list, for combo_filtered
    std::vector<std::string_view> type_views; // views of them, which is what the combo takes
    int                      type_index = 0;
    std::vector<char>        visible;    // one flag per object (vector<bool> has no usable reference)
    bool                     never_ask = false;
    std::string              status = "-";
    int                      ui_scale_percent = 100;
    int                      pending_scale_percent = 0; // > 0: main applies it between frames

    demo4_state();
};

void demo4_icons(strata::context& ui, demo4_state& s);
void demo4_bigtree(strata::context& ui, demo4_state& s);
void demo4_rows(strata::context& ui, demo4_state& s);
void demo4_app(strata::context& ui, demo4_state& s);

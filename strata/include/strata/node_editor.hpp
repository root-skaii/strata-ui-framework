#pragma once

// a node graph canvas: draggable nodes with typed input / output pins, links between them, pan, click / box
// selection. the application owns the graph (node positions and the link list); the editor draws it and reports edits.
// built on context::custom_item and draw(), so it works in any window or child region.
//
//   strata::node_editor graph;                               // kept between frames
//   if (graph.begin(ui, "graph", {0, 400})) {
//       for (node& n : nodes) {
//           graph.begin_node(n.id, n.title, n.pos);           // pos: canvas coordinates, edited by dragging
//           for (pin& p : n.inputs)  { graph.input_pin(p.id, p.name); }
//           for (pin& p : n.outputs) { graph.output_pin(p.id, p.name); }
//           graph.end_node();
//       }
//       for (link& l : links) { graph.link(l.from, l.to); }   // output pin -> input pin
//       graph.end();
//   }
//   if (strata::node_link made; graph.link_created(made)) { links.push_back({made.from, made.to}); }
//   if (strata::node_link gone; graph.link_deleted(gone)) { std::erase_if(links, ...); }
//   if (graph.delete_requested()) { /* remove graph.selected_nodes() and their links */ }
//
// gestures: left-drag a node moves the selection; left-drag from a pin makes a link; left-drag on empty canvas
// box-selects (Ctrl adds); middle-drag or Alt + left-drag pans; the wheel zooms; right-click asks for a context menu;
// Delete reports the selected link or nodes; Esc cancels a link in progress.

#include "strata/context.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace strata {

// an edge from an output pin to an input pin
struct node_link {
    u64 from{};
    u64 to{};

    [[nodiscard]] friend constexpr bool operator==(const node_link&, const node_link&) noexcept = default;
};

enum class node_target : u8 { none, canvas, node, link };

// a right-click, for the application to answer with its own menu
struct node_context_request {
    node_target target{};
    u64         node{};   // target == node (the clicked node, or the node of the clicked pin)
    node_link   link{};   // target == link
    vec2        screen;   // pointer, screen coordinates
    vec2        canvas;   // pointer, canvas coordinates: the `pos` a node created here would get
};

class node_editor {
public:
    node_editor();
    ~node_editor();
    node_editor(node_editor&&) noexcept;
    node_editor& operator=(node_editor&&) noexcept;

    // canvas offset in logical pixels: the canvas origin sits at the top-left of the editor plus `pan`
    vec2 pan{};
    bool show_grid{true};
    f32  grid_spacing{32.0f};
    // wheel zoom of the canvas (nodes and links scale; text stays at the ui font size and hides below ~0.6)
    bool allow_zoom{true};
    f32  zoom{1.0f};
    f32  min_zoom{0.25f};
    f32  max_zoom{2.0f};

    // reserves the canvas (width 0 = full width) and starts collecting nodes; false if the canvas is not visible.
    // identity comes from `label`. end() is needed after every true return
    [[nodiscard]] bool begin(context& ui, std::string_view label, vec2 size = {0.0f, 360.0f});
    void end();

    // a node at `pos` (canvas coordinates; dragging writes it). ids must be unique within the editor and stable
    void begin_node(u64 id, std::string_view title, vec2& pos);
    // pins are listed top to bottom. a link joins an output and an input pin of the same `type`
    void input_pin(u64 pin_id, std::string_view label, u32 type = 0);
    void output_pin(u64 pin_id, std::string_view label, u32 type = 0);
    // a plain line of text in the node body
    void node_text(std::string_view text);
    void end_node();

    // an existing link, drawn between the two pins (ignored if either pin was not submitted)
    void link(u64 output_pin_id, u64 input_pin_id);

    // results of the last end(): valid until the next begin()
    [[nodiscard]] bool link_created(node_link& out) const noexcept;
    [[nodiscard]] bool link_deleted(node_link& out) const noexcept;
    // Delete was pressed with nodes selected (selected_nodes() names them)
    [[nodiscard]] bool delete_requested() const noexcept;
    // a right-click this frame. the clicked node or link is selected; open the application's popup from here
    [[nodiscard]] bool context_requested(node_context_request& out) const noexcept;
    [[nodiscard]] u64  hovered_node() const noexcept; // 0 = none
    [[nodiscard]] u64  hovered_pin() const noexcept;  // 0 = none

    // screen-space geometry of the last end() (for popups, tooltips, overlays). empty / false for unknown ids
    [[nodiscard]] rect canvas_rect() const noexcept;
    [[nodiscard]] rect node_rect(u64 id) const noexcept;
    [[nodiscard]] bool pin_position(u64 pin_id, vec2& out) const noexcept;

    // selection survives between frames; ids of nodes no longer submitted drop out
    [[nodiscard]] std::span<const u64> selected_nodes() const noexcept;
    [[nodiscard]] bool node_selected(u64 id) const noexcept;
    // the selected link, if any (both ids 0 otherwise)
    [[nodiscard]] node_link selected_link() const noexcept;
    void select_node(u64 id, bool additive = false);
    void clear_selection() noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

// docking: dock spaces (the main area, named areas, edge docks, floating docks), each a tree of nodes over a region.
// leaves hold windows as tabs, splits divide their area between two children.
//
// per frame: dock_area / dock_edge / floating_dock give every space its rectangle and lay its tree out; windows in a leaf
// take its content rectangle (begin_window). dock_end_frame() draws the tab bars and splitters, handles their input,
// previews the drop target of a dragged window and applies a drop.

#include "strata/context.hpp"

#include "text_util.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace strata {

namespace {

constexpr f32 splitter_gap = 4.0f;
constexpr f32 min_node_size = 80.0f; // along the split axis
constexpr f32 outer_edge   = 18.0f; // drops this close to the border of a space split its whole tree
constexpr f32 edge_strip   = 22.0f; // the drop strip of an empty edge dock

} // namespace

// spaces ----------------------------------------------------------------------------------------

id context::dock_space_key(std::string_view name) noexcept
{
    return hash_id(name, hash_id("##dockspace"));
}

u8 context::dock_space_at(id key) noexcept
{
    u8 free_slot = no_node;
    u8 stale     = no_node;
    for (u32 i = 0; i < max_dock_spaces; ++i) {
        const dock_space& sp = dock_spaces_[i];
        if (sp.key == key) {
            return static_cast<u8>(i);
        }
        if (free_slot == no_node && sp.key == 0) { free_slot = static_cast<u8>(i); }
        if (stale == no_node && sp.key != 0 && sp.root == no_node && sp.last_frame + 300 < frame_) { stale = static_cast<u8>(i); }
    }
    const u8 pick = free_slot != no_node ? free_slot : stale;
    if (pick == no_node) {
        return no_node;
    }
    dock_spaces_[pick]            = {};
    dock_spaces_[pick].key        = key;
    dock_spaces_[pick].last_frame = frame_;
    return pick;
}

void context::dock_set_space(u8 space, const rect& area, const rect& drop, const rect& panel) noexcept
{
    dock_space& sp = dock_spaces_[space];
    // a floating dock that is dragged carries its panes along rigidly: only changes of the layout itself animate
    if (sp.owner != 0 && sp.root != no_node && sp.last_frame + 1 >= frame_ && sp.last_frame != 0) {
        const vec2 delta = area.min - sp.area.min;
        if (delta.x != 0.0f || delta.y != 0.0f) {
            for (dock_node& n : dock_nodes_) {
                if (n.used && n.space == space) {
                    n.shown_area    = {n.shown_area.min + delta, n.shown_area.max + delta};
                    n.shown_content = {n.shown_content.min + delta, n.shown_content.max + delta};
                }
            }
        }
    }
    sp.area       = area;
    sp.drop       = drop;
    sp.panel      = panel;
    sp.set        = true;
    sp.last_frame = frame_;
    dock_any_set_ = true;
    dock_relayout();
}

void context::dock_area(const rect& area)
{
    dock_area(std::string_view{}, area);
}

void context::dock_area(std::string_view space, const rect& area)
{
    const u8 si = dock_space_at(dock_space_key(space));
    if (si != no_node) {
        dock_set_space(si, area, area, area);
    }
}

// a panel along one side of `region`: it takes room only while something is docked in it
rect context::dock_edge(std::string_view name, dock_side side, f32 size, const rect& region)
{
    const u8 si = dock_space_at(dock_space_key(name));
    if (si == no_node || cur_ != nullptr) {
        return region;
    }
    dock_space& sp = dock_spaces_[si];
    sp.edge = true;
    if (sp.edge_size <= 0.0f) { sp.edge_size = std::max(size, min_node_size); }

    const bool horizontal = side == dock_side::left || side == dock_side::right;
    const f32  extent     = horizontal ? region.width() : region.height();
    const f32  room       = std::max(extent - 160.0f, min_node_size); // the rest of the region keeps some space
    const f32  panel_size = std::clamp(sp.edge_size, min_node_size, room);

    const auto strip = [&](f32 thickness) -> rect {
        switch (side) {
        case dock_side::left:  return {region.min, {region.min.x + thickness, region.max.y}};
        case dock_side::right: return {{region.max.x - thickness, region.min.y}, region.max};
        case dock_side::top:   return {region.min, {region.max.x, region.min.y + thickness}};
        default:               return {{region.min.x, region.max.y - thickness}, region.max};
        }
    };
    const rect panel    = strip(panel_size);
    const bool occupied = sp.root != no_node;
    const bool dragging = dock_drag_prev_ != 0; // a dockable window is being dragged: an empty dock shows a drop strip

    rect remaining = region;
    if (occupied) {
        const f32 taken = panel_size + splitter_gap;
        switch (side) {
        case dock_side::left:   remaining.min.x += taken; break;
        case dock_side::right:  remaining.max.x -= taken; break;
        case dock_side::top:    remaining.min.y += taken; break;
        default:                remaining.max.y -= taken; break;
        }
    }
    dock_set_space(si, occupied ? panel : strip(0.0f), occupied ? panel : (dragging ? strip(edge_strip) : rect{}), panel);
    if (!occupied) {
        return remaining;
    }

    rect gap;
    switch (side) {
    case dock_side::left:  gap = {{panel.max.x, region.min.y}, {panel.max.x + splitter_gap, region.max.y}}; break;
    case dock_side::right: gap = {{panel.min.x - splitter_gap, region.min.y}, {panel.min.x, region.max.y}}; break;
    case dock_side::top:   gap = {{region.min.x, panel.max.y}, {region.max.x, panel.max.y + splitter_gap}}; break;
    default:               gap = {{region.min.x, panel.min.y - splitter_gap}, {region.max.x, panel.min.y}}; break;
    }
    const bool free_pointer = hovered_window_prev_ == 0 || hovered_docked_prev_;
    const interaction in = interact_impl(hash_id("##dockedge", static_cast<id>(si + 1)), gap.expanded(1.5f), free_pointer);
    if (in.hovered || in.held) { cursor_ = horizontal ? cursor_kind::resize_ew : cursor_kind::resize_ns; }
    if (gap.expanded(1.5f).contains(mouse_)) { dock_chrome_cur_ = true; }
    if (in.held) {
        dock_splitting_ = true;
        f32 want = 0.0f;
        switch (side) {
        case dock_side::left:   want = mouse_.x - region.min.x - splitter_gap * 0.5f; break;
        case dock_side::right:  want = region.max.x - mouse_.x - splitter_gap * 0.5f; break;
        case dock_side::top:    want = mouse_.y - region.min.y - splitter_gap * 0.5f; break;
        default:                want = region.max.y - mouse_.y - splitter_gap * 0.5f; break;
        }
        sp.edge_size = std::clamp(want, min_node_size, room);
    }
    const color line = lerp(style_.border, style_.accent, in.held ? 1.0f : (in.hovered ? 0.7f : 0.0f));
    dl_.rect_filled(gap, line.scaled_alpha(in.hovered || in.held ? 0.9f : 0.55f));
    return remaining;
}

// a window that is a dock space: docked windows fill its body and move with it
bool context::floating_dock(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
{
    if (cur_ != nullptr) {
        return false;
    }
    const bool open = begin_window(title, initial_pos, {size.x, size.y > 0.0f ? size.y : 320.0f}, flags | window_flags::resizable);
    const window_state& st = *cur_;

    const u8 si = dock_space_at(dock_space_key(title));
    if (si != no_node) {
        dock_space& sp = dock_spaces_[si];
        sp.owner  = hash_id(title, id_stack_[0]);
        sp.edge   = false;
        const rect frame = cur_frame_;
        const rect body  = {{frame.min.x, frame.min.y + st.title_h}, frame.max};
        dock_set_space(si, body, body, body);
        sp.hidden = st.collapsed;
        if (open && sp.root == no_node) { // nothing in it yet
            const std::string_view hint = "drop windows here";
            const vec2 ts = font_.measure(current_font(), hint);
            dl_.text({body.center().x - ts.x * 0.5f, body.center().y - ts.y * 0.5f}, style_.text_dim.scaled_alpha(0.6f), hint, current_font());
        }
    }
    end_window();
    return open;
}

// tree ------------------------------------------------------------------------------------------

u8 context::dock_new_node() noexcept
{
    for (u32 i = 0; i < max_dock_nodes; ++i) {
        if (!dock_nodes_[i].used) {
            dock_nodes_[i]      = {};
            dock_nodes_[i].used = true;
            return static_cast<u8>(i);
        }
    }
    return no_node;
}

void context::dock_free_node(u8 node) noexcept
{
    if (node != no_node) {
        dock_nodes_[node] = {};
    }
}

void context::dock_compute(u8 index, const rect& area) noexcept
{
    dock_node& n = dock_nodes_[index];
    n.area = area;
    if (n.leaf()) {
        const f32 tab_h = font_.line_height(0) + 10.0f;
        n.content = {{area.min.x, area.min.y + tab_h}, area.max};
        return;
    }
    const f32 span  = (n.vertical ? area.height() : area.width()) - splitter_gap;
    f32       first = span * n.ratio;
    if (span >= 2.0f * min_node_size) { // keep both halves usable
        first = std::clamp(first, min_node_size, span - min_node_size);
    }
    rect a = area;
    rect b = area;
    if (n.vertical) {
        a.max.y = area.min.y + first;
        b.min.y = a.max.y + splitter_gap;
    } else {
        a.max.x = area.min.x + first;
        b.min.x = a.max.x + splitter_gap;
    }
    dock_compute(n.child[0], a);
    dock_compute(n.child[1], b);
}

void context::dock_relayout() noexcept
{
    for (const dock_space& sp : dock_spaces_) {
        if (sp.set && sp.root != no_node) {
            dock_compute(sp.root, sp.area);
        }
    }
    // what is drawn is the layout, unless panes are meant to slide there (dock_animate moves them)
    for (dock_node& n : dock_nodes_) {
        if (n.used && (!dock_animation_ || n.fresh)) {
            n.shown_area    = n.area;
            n.shown_content = n.content;
            n.fresh         = false;
        }
    }
}

// one step of the slide of every pane towards its place in the layout; once per frame
void context::dock_animate()
{
    if (!dock_animation_) {
        return;
    }
    const f32 k = dock_splitting_ ? 1.0f : 1.0f - std::exp(-16.0f * dt_);
    const auto step = [k](rect& shown, const rect& target) {
        const auto toward = [k](f32 a, f32 b) { return std::abs(b - a) < 0.5f ? b : a + (b - a) * k; };
        shown = {{toward(shown.min.x, target.min.x), toward(shown.min.y, target.min.y)},
                 {toward(shown.max.x, target.max.x), toward(shown.max.y, target.max.y)}};
    };
    for (dock_node& n : dock_nodes_) {
        if (n.used) {
            step(n.shown_area, n.area);
            step(n.shown_content, n.content);
        }
    }
}

// puts a window into a leaf (center) or splits a node to make room for a new leaf next to it
void context::dock_attach(window_state& w, u8 space, u8 node, dock_zone zone, bool outer, f32 size) noexcept
{
    if (space >= max_dock_spaces) {
        return;
    }
    if (w.dock != 0) {
        dock_detach(w);
    }
    dock_space& sp = dock_spaces_[space];

    u8 leaf = no_node;
    if (sp.root == no_node) { // the first docked window fills the whole space
        leaf = dock_new_node();
        if (leaf == no_node) { return; }
        dock_nodes_[leaf].space = space;
        sp.root = leaf;
    } else {
        if (outer || node == no_node) { node = sp.root; }
        if (dock_nodes_[node].leaf() && zone == dock_zone::center && !outer) {
            leaf = node;
        } else if (!dock_nodes_[node].leaf() && zone == dock_zone::center) {
            while (!dock_nodes_[node].leaf()) { node = dock_nodes_[node].child[0]; } // a split has no tabs: use its first leaf
            leaf = node;
        } else {
            const u8 split = dock_new_node();
            leaf           = dock_new_node();
            if (split == no_node || leaf == no_node) {
                dock_free_node(split);
                dock_free_node(leaf);
                return;
            }
            dock_node& s = dock_nodes_[split];
            dock_node& n = dock_nodes_[node];
            const bool new_first = zone == dock_zone::left || zone == dock_zone::top;
            s.space    = space;
            s.vertical = zone == dock_zone::top || zone == dock_zone::bottom;
            s.child    = new_first ? std::array<u8, 2>{leaf, node} : std::array<u8, 2>{node, leaf};
            const f32 share = size > 0.0f ? std::clamp(size, 0.1f, 0.9f) : (outer ? 0.25f : 0.5f); // of the new window
            s.ratio    = new_first ? share : 1.0f - share;
            s.parent   = n.parent;
            if (n.parent == no_node) {
                sp.root = split;
            } else {
                dock_node& p = dock_nodes_[n.parent];
                (p.child[0] == node ? p.child[0] : p.child[1]) = split;
            }
            n.parent                 = split;
            dock_nodes_[leaf].parent = split;
            dock_nodes_[leaf].space  = space;
        }
    }

    const bool slide = dock_animation_ && dock_nodes_[leaf].fresh && zone != dock_zone::center;
    w.float_size = {w.width, w.height};
    w.dock       = static_cast<u32>(leaf) + 1;
    w.dock_order = ++dock_counter_;
    dock_nodes_[leaf].active = w.key;
    dock_relayout();
    if (slide) { // the new pane grows out of the edge it was dropped at
        dock_node& l = dock_nodes_[leaf];
        const auto collapsed = [zone](const rect& r) -> rect {
            switch (zone) {
            case dock_zone::left:   return {r.min, {r.min.x, r.max.y}};
            case dock_zone::right:  return {{r.max.x, r.min.y}, r.max};
            case dock_zone::top:    return {r.min, {r.max.x, r.min.y}};
            default:                return {{r.min.x, r.max.y}, r.max};
            }
        };
        l.shown_area    = collapsed(l.area);
        l.shown_content = collapsed(l.content);
        l.fresh         = false;
    }
}

void context::dock_detach(window_state& w) noexcept
{
    if (w.dock == 0) {
        return;
    }
    w.dock       = 0;
    w.dock_owner = 0;
    if (w.float_size.x > 0.0f) {
        w.width  = w.float_size.x;
        w.height = w.float_size.y;
    }
}

// removes leaves nobody sits in any more (and the splits that only wrapped them)
void context::dock_prune() noexcept
{
    bool again = true;
    while (again) {
        again = false;
        for (u32 i = 0; i < max_dock_nodes && !again; ++i) {
            const dock_node& n = dock_nodes_[i];
            if (!n.used || !n.leaf()) { continue; }

            bool occupied = false;
            for (const window_state& w : windows_) {
                occupied = occupied || (w.key != 0 && w.dock == i + 1 && w.last_frame == frame_);
            }
            if (occupied) { continue; }

            for (window_state& w : windows_) { // windows that are not shown right now forget this node as well
                if (w.dock == i + 1) { dock_detach(w); }
            }
            dock_space& sp     = dock_spaces_[n.space];
            const u8    parent = n.parent;
            dock_free_node(static_cast<u8>(i));
            if (parent == no_node) {
                sp.root = no_node;
            } else {
                dock_node& p = dock_nodes_[parent];
                const u8 sibling = p.child[0] == i ? p.child[1] : p.child[0];
                const u8 grand   = p.parent;
                dock_nodes_[sibling].parent = grand;
                if (grand == no_node) {
                    sp.root = sibling;
                } else {
                    dock_node& g = dock_nodes_[grand];
                    (g.child[0] == parent ? g.child[0] : g.child[1]) = sibling;
                }
                dock_free_node(parent);
            }
            again = true;
        }
    }
    dock_relayout();
}

// where a window dropped at `pointer` would go
context::dock_target context::dock_pick(vec2 pointer, u8 exclude_leaf) const noexcept
{
    dock_target t;

    // which space: floating docks are on top of the others (the higher one first)
    int best = -1;
    u32 best_rank = 0;
    for (u32 si = 0; si < max_dock_spaces; ++si) {
        const dock_space& sp = dock_spaces_[si];
        if (!sp.set || sp.hidden || !sp.drop.contains(pointer)) { continue; }
        u32 rank = sp.edge && sp.root == no_node ? 1u : 0u; // the drop strip of an empty edge dock beats the area behind it
        if (sp.owner != 0) {
            const u32 z = z_index(sp.owner);
            rank = 2 + (z == no_z ? 0u : z);
        }
        if (best < 0 || rank >= best_rank) {
            best      = static_cast<int>(si);
            best_rank = rank;
        }
    }
    if (best < 0) {
        return t; // no space under the pointer
    }
    const dock_space& sp = dock_spaces_[static_cast<u32>(best)];
    t.valid = true;
    t.space = static_cast<u8>(best);
    if (sp.root == no_node) { // nothing docked yet: the window would fill the space (an edge dock: the whole panel)
        t.preview = sp.panel;
        return t;
    }

    // (a pane that is being moved cannot be dropped on itself)
    if (exclude_leaf != no_node && sp.root == exclude_leaf) {
        t.valid = false;
        return t;
    }

    // near the border of the space: split its whole tree
    const rect& a = sp.area;
    const f32 dl = pointer.x - a.min.x;
    const f32 dr = a.max.x - pointer.x;
    const f32 dt = pointer.y - a.min.y;
    const f32 db = a.max.y - pointer.y;
    const f32 nearest = std::min({dl, dr, dt, db});
    if (nearest < outer_edge) {
        t.outer = true;
        t.node  = sp.root;
        const f32 fw = a.width() * 0.25f;
        const f32 fh = a.height() * 0.25f;
        if (nearest == dl)      { t.zone = dock_zone::left;   t.preview = {a.min, {a.min.x + fw, a.max.y}}; }
        else if (nearest == dr) { t.zone = dock_zone::right;  t.preview = {{a.max.x - fw, a.min.y}, a.max}; }
        else if (nearest == dt) { t.zone = dock_zone::top;    t.preview = {a.min, {a.max.x, a.min.y + fh}}; }
        else                    { t.zone = dock_zone::bottom; t.preview = {{a.min.x, a.max.y - fh}, a.max}; }
        return t;
    }

    u8 found = no_node;
    for (u32 i = 0; i < max_dock_nodes; ++i) {
        const dock_node& n = dock_nodes_[i];
        if (n.used && n.leaf() && n.space == best && n.area.contains(pointer)) { found = static_cast<u8>(i); break; }
    }
    if (found == no_node || found == exclude_leaf) {
        t.valid = false; // over a splitter, or over the pane being moved
        return t;
    }
    const rect& r = dock_nodes_[found].area;
    t.node = found;
    const f32 rx = (pointer.x - r.min.x) / std::max(r.width(), 1.0f);
    const f32 ry = (pointer.y - r.min.y) / std::max(r.height(), 1.0f);
    if (rx > 0.3f && rx < 0.7f && ry > 0.3f && ry < 0.7f) {
        t.zone    = dock_zone::center;
        t.preview = r;
        return t;
    }
    const f32 l = rx, rr = 1.0f - rx, tt = ry, bb = 1.0f - ry;
    const f32 m = std::min({l, rr, tt, bb});
    const vec2 c = r.center();
    if (m == l)       { t.zone = dock_zone::left;   t.preview = {r.min, {c.x, r.max.y}}; }
    else if (m == rr) { t.zone = dock_zone::right;  t.preview = {{c.x, r.min.y}, r.max}; }
    else if (m == tt) { t.zone = dock_zone::top;    t.preview = {r.min, {r.max.x, c.y}}; }
    else              { t.zone = dock_zone::bottom; t.preview = {{r.min.x, c.y}, r.max}; }
    return t;
}

bool context::dock_window(std::string_view title, dock_zone zone, std::string_view target, f32 size, std::string_view space_name)
{
    const id wid = hash_id(title, id_stack_[0]);
    window_state* w = window_for(wid, {}, 0.0f);
    if (w == nullptr) {
        return false;
    }
    u8 space = no_node;
    u8 node  = no_node;
    bool outer = false;
    if (!target.empty()) {
        const window_state* t = window_find(hash_id(target, id_stack_[0]));
        if (t == nullptr || t->dock == 0 || t->dock > max_dock_nodes || !dock_nodes_[t->dock - 1].used) {
            return false;
        }
        node  = static_cast<u8>(t->dock - 1);
        space = dock_nodes_[node].space;
    } else {
        space = dock_space_at(dock_space_key(space_name));
        outer = zone != dock_zone::center; // relative to the whole space
    }
    if (space == no_node) {
        return false;
    }
    dock_attach(*w, space, node, zone, outer, size);
    return w->dock != 0;
}

void context::undock_window(std::string_view title)
{
    if (window_state* w = window_find(hash_id(title, id_stack_[0]))) {
        dock_detach(*w);
    }
}

bool context::is_docked(std::string_view title) const noexcept
{
    const window_state* w = window_find(hash_id(title, id_stack_[0]));
    return w != nullptr && w->dock != 0;
}

// per frame: chrome, input, drop ---------------------------------------------------------------------

void context::dock_end_frame()
{
    if (!dock_any_set_) {
        dock_splitting_ = false;
        return;
    }
    dock_relayout();

    const f32 lh    = font_.line_height(0);
    const f32 tab_h = lh + 10.0f;

    // chrome is not part of any window: it may only be touched when no floating window covers the pointer
    // (a floating dock's own window does not count: its tab bars sit on its body)
    const bool free_pointer = hovered_window_prev_ == 0 || hovered_docked_prev_;

    for (u32 ni = 0; ni < max_dock_nodes; ++ni) {
        dock_node& n = dock_nodes_[ni];
        if (!n.used) { continue; }
        const dock_space& sp = dock_spaces_[n.space];
        if (!sp.set || sp.hidden) { continue; }

        // the chrome of a floating dock is drawn into that window's layer, so it stacks with it
        u32 layer = run_base;
        if (sp.owner != 0) {
            for (u32 i = 0; i < frame_window_count_; ++i) {
                if (frame_windows_[i]->key == sp.owner) { layer = i; break; }
            }
        }
        switch_run(layer);
        const bool free = free_pointer || (sp.owner != 0 && hovered_window_prev_ == sp.owner);
        const auto chrome = [&](id key, const rect& r) { return interact_impl(key, r, free); };

        if (!n.leaf()) { // splitter between the two children
            const dock_node& a = dock_nodes_[n.child[0]];
            const dock_node& b = dock_nodes_[n.child[1]];
            const rect gap = n.vertical ? rect{{n.shown_area.min.x, a.shown_area.max.y}, {n.shown_area.max.x, b.shown_area.min.y}}
                                        : rect{{a.shown_area.max.x, n.shown_area.min.y}, {b.shown_area.min.x, n.shown_area.max.y}};
            const interaction in = chrome(hash_id("##dsplit", static_cast<id>(ni + 1)), gap.expanded(1.5f));
            if (in.hovered || in.held) {
                cursor_ = n.vertical ? cursor_kind::resize_ns : cursor_kind::resize_ew;
            }
            if (gap.expanded(1.5f).contains(mouse_)) { dock_chrome_cur_ = true; }
            if (in.held) {
                dock_splitting_ = true;
                const f32 span = (n.vertical ? n.area.height() : n.area.width()) - splitter_gap;
                if (span >= 2.0f * min_node_size) {
                    const f32 pos = (n.vertical ? mouse_.y - n.area.min.y : mouse_.x - n.area.min.x) - splitter_gap * 0.5f;
                    n.ratio = std::clamp(pos / span, min_node_size / span, 1.0f - min_node_size / span);
                }
            }
            const color line = lerp(style_.border, style_.accent, in.held ? 1.0f : (in.hovered ? 0.7f : 0.0f));
            dl_.rect_filled(gap, line.scaled_alpha(in.hovered || in.held ? 0.9f : 0.55f));
            continue;
        }

        std::array<window_state*, max_windows> tabs{};
        u32 count = 0;
        for (u32 i = 0; i < frame_window_count_; ++i) {
            window_state* w = frame_windows_[i];
            if (w->dock == ni + 1 && w->docked_now) { tabs[count++] = w; }
        }
        if (count == 0) { continue; }
        std::sort(tabs.begin(), tabs.begin() + count, [](const window_state* a, const window_state* b) {
            return a->dock_order < b->dock_order;
        });
        bool active_found = false;
        for (u32 i = 0; i < count; ++i) { active_found = active_found || tabs[i]->key == n.active; }
        if (!active_found) { n.active = tabs[0]->key; }

        const rect bar = {n.shown_area.min, {n.shown_area.max.x, n.shown_area.min.y + tab_h}};
        if (bar.contains(mouse_)) { dock_chrome_cur_ = true; }
        shape_style head;
        head.fill_top    = lerp(style_.title_bg, color{255, 255, 255, style_.title_bg.a}, style_.gradient * 0.8f);
        head.fill_bottom = style_.title_bg;
        dl_.shape(bar, head);
        dl_.rect_filled({{bar.min.x, bar.max.y - 1.0f}, bar.max}, style_.border);

        // tab widths: as wide as the title, squeezed equally when they do not all fit
        std::array<f32, max_windows> widths{};
        f32 total = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            widths[i] = font_.measure(0, {tabs[i]->title.data(), tabs[i]->title_len}).x + 26.0f;
            total += widths[i];
        }
        const f32 squeeze = total > bar.width() ? bar.width() / total : 1.0f;

        f32 x = bar.min.x;
        for (u32 i = 0; i < count; ++i) {
            window_state& w = *tabs[i];
            const f32 tw = widths[i] * squeeze;
            const rect cell = {{x, bar.min.y}, {x + tw, bar.max.y}};
            x += tw;

            const id key = hash_id("##dtab", w.key);
            const interaction in = chrome(key, cell);
            if (in.hovered && mouse_pressed_) {
                n.active        = w.key;
                dock_press_pos_ = mouse_;
            }
            // dragging a tab away takes the window out of the dock: it floats under the pointer and keeps following it
            if (in.held) {
                const vec2 d = mouse_ - dock_press_pos_;
                if (dot(d, d) > 36.0f) {
                    const f32 fw = w.float_size.x > 0.0f ? w.float_size.x : 320.0f;
                    dock_detach(w);
                    w.pos = {mouse_.x - std::min(fw * 0.25f, 80.0f), mouse_.y - tab_h * 0.5f};
                    bring_to_front(w.key);
                    active_ = hash_id("##drag", w.key);
                    continue;
                }
            }

            const bool sel = w.key == n.active;
            anim_slot& a = anim_for(key);
            a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
            a.toggle = approach(a.toggle, sel ? 1.0f : 0.0f);
            if (a.toggle > 0.01f || a.hover > 0.01f) {
                shape_style bg;
                bg.radius      = radii(style_.rounding * 0.6f, corners::top);
                bg.fill_top    = style_.window_bg.scaled_alpha(a.toggle);
                bg.fill_bottom = bg.fill_top;
                if (!sel) { bg.fill_top = style_.widget_hover.scaled_alpha(0.6f * a.hover); bg.fill_bottom = bg.fill_top; }
                dl_.shape({{cell.min.x + 1.0f, cell.min.y + 3.0f}, {cell.max.x - 1.0f, cell.max.y}}, bg);
            }
            if (sel) {
                dl_.rect_filled({{cell.min.x + 6.0f, cell.max.y - 2.0f}, {cell.max.x - 6.0f, cell.max.y}}, style_.accent);
            }
            dl_.push_clip({{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}});
            dl_.text({cell.min.x + 13.0f, cell.min.y + (tab_h - lh) * 0.5f}, lerp(style_.text_dim, style_.text, std::max(a.toggle, a.hover * 0.7f)),
                     {w.title.data(), w.title_len}, 0);
            dl_.pop_clip();
        }

        // what is left of the bar is a grip: dragging it moves the whole pane, all its tabs, to another place
        const rect grip = {{x, bar.min.y}, bar.max};
        if (grip.width() >= 16.0f) {
            const interaction in = chrome(hash_id("##dgrip", static_cast<id>(ni + 1)), grip);
            if (in.hovered && mouse_pressed_) {
                dock_group_src_   = static_cast<u8>(ni);
                dock_group_moved_ = false;
                dock_press_pos_   = mouse_;
            }
            if (in.held && dock_group_src_ == ni && !dock_group_moved_) {
                const vec2 d = mouse_ - dock_press_pos_;
                dock_group_moved_ = dot(d, d) > 36.0f;
            }
            const bool lit = in.hovered || (in.held && dock_group_src_ == ni);
            const color dots = style_.text_dim.scaled_alpha(lit ? 0.9f : 0.35f);
            const f32 gx = bar.max.x - 12.0f;
            const f32 gy = bar.min.y + (tab_h - 12.0f) * 0.5f;
            for (u32 cx = 0; cx < 2; ++cx) {
                for (u32 cy = 0; cy < 3; ++cy) {
                    const f32 px = gx + static_cast<f32>(cx) * 4.0f;
                    const f32 py = gy + static_cast<f32>(cy) * 4.0f;
                    dl_.rect_filled({{px, py}, {px + 2.0f, py + 2.0f}}, dots);
                }
            }
        }
    }
    switch_run(run_base);

    // a pane being carried by its grip: where it would land, and the drop
    if (dock_group_src_ != no_node) {
        const bool src_ok = dock_nodes_[dock_group_src_].used && dock_nodes_[dock_group_src_].leaf() &&
                            dock_spaces_[dock_nodes_[dock_group_src_].space].set;
        if (!src_ok || (!mouse_down_ && !mouse_released_)) {
            dock_group_src_   = no_node;
            dock_group_moved_ = false;
        } else if (dock_group_moved_) {
            std::array<window_state*, max_windows> group{};
            u32 count = 0;
            for (u32 i = 0; i < frame_window_count_; ++i) {
                if (frame_windows_[i]->dock == dock_group_src_ + 1u && frame_windows_[i]->docked_now) { group[count++] = frame_windows_[i]; }
            }
            std::sort(group.begin(), group.begin() + count, [](const window_state* a, const window_state* b) {
                return a->dock_order < b->dock_order;
            });
            const dock_target t = dock_pick(mouse_, dock_group_src_);
            if (mouse_released_) {
                const id active = dock_nodes_[dock_group_src_].active;
                if (t.valid && count > 0) {
                    dock_attach(*group[0], t.space, t.node, t.zone, t.outer);
                    const u32 leaf = group[0]->dock;
                    for (u32 i = 1; i < count && leaf != 0; ++i) {
                        dock_attach(*group[i], t.space, static_cast<u8>(leaf - 1), dock_zone::center, false);
                    }
                    if (leaf != 0) { dock_nodes_[leaf - 1].active = active; }
                } else {
                    // dropped on nothing: the windows float again, fanned out from the pointer
                    for (u32 i = 0; i < count; ++i) {
                        const f32 fw = group[i]->float_size.x > 0.0f ? group[i]->float_size.x : 320.0f;
                        dock_detach(*group[i]);
                        const f32 fan = static_cast<f32>(i) * 26.0f;
                        group[i]->pos = {mouse_.x - std::min(fw * 0.25f, 80.0f) + fan, mouse_.y - tab_h * 0.5f + fan};
                        bring_to_front(group[i]->key);
                    }
                }
                dock_group_src_   = no_node;
                dock_group_moved_ = false;
                dock_drag_win_    = 0;
                dock_drag_prev_   = 0;
                dock_target_prev_ = {};
            } else {
                dock_target_cur_ = t;
                dock_drag_win_   = group[0] != nullptr ? group[0]->key : dock_nodes_[dock_group_src_].active;
                // the pane follows the pointer as a small ghost of its tab bar
                const u32 saved_owner = run_owner_;
                switch_run(run_overlay);
                dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
                const std::string label = count > 1 ? std::format("{} windows", count)
                                                    : std::string{group[0] != nullptr ? std::string_view{group[0]->title.data(), group[0]->title_len} : ""};
                const f32 gw = font_.measure(0, label).x + 28.0f;
                const rect ghost = rect::from_size({mouse_.x + 12.0f, mouse_.y + 10.0f}, {gw, tab_h});
                shape_style gs;
                gs.radius       = radii(style_.rounding * 0.6f);
                gs.fill_top     = style_.window_bg.scaled_alpha(0.92f);
                gs.fill_bottom  = gs.fill_top;
                gs.border       = style_.accent;
                gs.border_width = 1.5f;
                gs.shadow       = color{0, 0, 0, 110};
                gs.shadow_blur  = 10.0f;
                dl_.shape(ghost, gs);
                dl_.text({ghost.min.x + 14.0f, ghost.min.y + (tab_h - lh) * 0.5f}, style_.text, label, 0);
                dl_.pop_clip();
                switch_run(saved_owner);
            }
        }
    }

    // the drop preview of a window being dragged over a space, and the drop itself
    if (dock_target_cur_.valid) {
        const u32 saved_owner = run_owner_;
        switch_run(run_overlay);
        dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
        shape_style pv;
        pv.radius       = radii(style_.rounding * 0.6f);
        pv.fill_top     = style_.accent.scaled_alpha(0.22f);
        pv.fill_bottom  = style_.accent.scaled_alpha(0.14f);
        pv.border       = style_.accent.scaled_alpha(0.9f);
        pv.border_width = 2.0f;
        dl_.shape(dock_target_cur_.preview.expanded(-3.0f), pv);
        dl_.pop_clip();
        switch_run(saved_owner);
    }
    if (mouse_released_ && dock_target_prev_.valid && dock_drag_prev_ != 0 && dock_target_prev_.space < max_dock_spaces) {
        const dock_space& target_space = dock_spaces_[dock_target_prev_.space];
        if (target_space.set && target_space.drop.contains(mouse_)) {
            // let go over a space: the window that was being dragged joins it
            window_state* dragged = window_find(dock_drag_prev_);
            if (dragged != nullptr && dragged->dock == 0) {
                const dock_target t = dock_target_prev_;
                dock_attach(*dragged, t.space, t.node, t.zone, t.outer);
            }
        }
    }
    dock_prune();
    dock_animate();
    dock_splitting_ = false;
}

// layout text ------------------------------------------------------------------------------------
//   strata-dock 1
//   space <key> <edge size>        one per space that has panes (or is an edge dock); the tree follows in pre-order:
//   S <v|h> <ratio>                a split (v = stacked), then its two children
//   L <active> <n>                 a leaf with n tabs, then n lines
//   W <key> <title>
//   win <key> <x> <y> <w> <h> <collapsed>     every window; w / h are the floating size

namespace {

struct layout_node {
    bool          leaf{};
    bool          vertical{};
    f32           ratio{0.5f};
    id            active{};
    std::vector<id> windows;
    int           child[2]{-1, -1};
};
struct layout_space {
    id  key{};
    f32 edge{};
    int root{-1};
};
struct layout_window {
    id   key{};
    vec2 pos;
    vec2 size;
    bool collapsed{};
};
struct layout_parse {
    std::vector<std::string_view> lines;
    std::size_t                   at{};
    std::vector<layout_node>      nodes;
    std::vector<layout_space>     spaces;
    std::vector<layout_window>    windows;
    bool                          ok{true};
};

[[nodiscard]] std::string_view take(std::string_view& s) noexcept
{
    while (!s.empty() && s.front() == ' ') { s.remove_prefix(1); }
    std::size_t n = 0;
    while (n < s.size() && s[n] != ' ') { ++n; }
    const std::string_view t = s.substr(0, n);
    s.remove_prefix(n);
    return t;
}

[[nodiscard]] bool to_hex(std::string_view t, id& out) noexcept
{
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out, 16);
    return !t.empty() && r.ec == std::errc{} && r.ptr == t.data() + t.size();
}

[[nodiscard]] bool to_float(std::string_view t, f32& out) noexcept
{
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out);
    return !t.empty() && r.ec == std::errc{} && r.ptr == t.data() + t.size() && std::isfinite(out);
}

int parse_layout_node(layout_parse& lp, u32 depth)
{
    if (lp.at >= lp.lines.size() || depth > 32 || lp.nodes.size() >= 64) { lp.ok = false; return -1; }
    std::string_view line = lp.lines[lp.at++];
    const std::string_view tag = take(line);
    layout_node n;
    if (tag == "S") {
        const std::string_view dir = take(line);
        if ((dir != "v" && dir != "h") || !to_float(take(line), n.ratio)) { lp.ok = false; return -1; }
        n.vertical = dir == "v";
        n.ratio    = std::clamp(n.ratio, 0.05f, 0.95f);
        const int self = static_cast<int>(lp.nodes.size());
        lp.nodes.push_back(n);
        const int a = parse_layout_node(lp, depth + 1);
        const int b = a < 0 ? -1 : parse_layout_node(lp, depth + 1);
        if (a < 0 || b < 0) { lp.ok = false; return -1; }
        lp.nodes[static_cast<std::size_t>(self)].child[0] = a;
        lp.nodes[static_cast<std::size_t>(self)].child[1] = b;
        return self;
    }
    if (tag == "L") {
        n.leaf = true;
        id count = 0;
        if (!to_hex(take(line), n.active) || !to_hex(take(line), count) || count > 64) { lp.ok = false; return -1; }
        for (id i = 0; i < count; ++i) {
            if (lp.at >= lp.lines.size()) { lp.ok = false; return -1; }
            std::string_view w = lp.lines[lp.at++];
            id key = 0;
            if (take(w) != "W" || !to_hex(take(w), key)) { lp.ok = false; return -1; }
            n.windows.push_back(key);
        }
        lp.nodes.push_back(std::move(n));
        return static_cast<int>(lp.nodes.size()) - 1;
    }
    lp.ok = false;
    return -1;
}

} // namespace

std::string context::dock_save_layout() const
{
    std::string out = "strata-dock 1\n";
    const auto node_out = [&](auto& self, u8 ni) -> void {
        const dock_node& n = dock_nodes_[ni];
        if (!n.leaf()) {
            out += std::format("S {} {}\n", n.vertical ? 'v' : 'h', n.ratio);
            self(self, n.child[0]);
            self(self, n.child[1]);
            return;
        }
        std::vector<const window_state*> tabs;
        for (const window_state& w : windows_) {
            if (w.key != 0 && w.dock == ni + 1u) { tabs.push_back(&w); }
        }
        std::sort(tabs.begin(), tabs.end(), [](const window_state* a, const window_state* b) { return a->dock_order < b->dock_order; });
        out += std::format("L {:x} {:x}\n", n.active, tabs.size());
        for (const window_state* w : tabs) {
            std::string title{w->title.data(), w->title_len};
            std::ranges::replace(title, '\n', ' ');
            out += std::format("W {:x} {}\n", w->key, title);
        }
    };
    for (const dock_space& sp : dock_spaces_) {
        if (sp.key == 0 || (sp.root == no_node && !sp.edge)) { continue; }
        out += std::format("space {:x} {}\n", sp.key, sp.edge_size);
        if (sp.root != no_node) {
            node_out(node_out, sp.root);
        } else {
            out += "L 0 0\n";
        }
    }
    for (const window_state& w : windows_) {
        if (w.key == 0 || w.menubar || w.modal_level != 0) { continue; }
        const vec2 size = w.dock != 0 ? w.float_size : vec2{w.width, w.height};
        out += std::format("win {:x} {} {} {} {} {}\n", w.key, w.pos.x, w.pos.y, size.x, size.y, w.collapsed ? 1 : 0);
    }
    return out;
}

bool context::dock_load_layout(std::string_view text)
{
    layout_parse lp;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t e = text.find('\n', i);
        if (e == std::string_view::npos) { e = text.size(); }
        std::string_view line = text.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); }
        if (!line.empty()) { lp.lines.push_back(line); }
        i = e + 1;
    }
    if (lp.lines.empty() || lp.lines[0] != "strata-dock 1") {
        return false;
    }
    lp.at = 1;
    while (lp.at < lp.lines.size() && lp.ok) {
        std::string_view line = lp.lines[lp.at++];
        const std::string_view tag = take(line);
        if (tag == "space") {
            layout_space sp;
            if (!to_hex(take(line), sp.key) || !to_float(take(line), sp.edge)) { lp.ok = false; break; }
            const std::size_t first = lp.nodes.size();
            sp.root = parse_layout_node(lp, 0);
            // an edge dock without panes is written as an empty leaf: it only carries the size
            if (sp.root >= 0 && lp.nodes[static_cast<std::size_t>(sp.root)].leaf && lp.nodes[static_cast<std::size_t>(sp.root)].windows.empty()) {
                sp.root = -1;
                lp.nodes.resize(first);
            }
            if (lp.ok && lp.spaces.size() < max_dock_spaces) { lp.spaces.push_back(sp); }
        } else if (tag == "win") {
            layout_window w;
            f32 collapsed = 0.0f;
            if (!to_hex(take(line), w.key) || !to_float(take(line), w.pos.x) || !to_float(take(line), w.pos.y) ||
                !to_float(take(line), w.size.x) || !to_float(take(line), w.size.y) || !to_float(take(line), collapsed)) {
                lp.ok = false;
                break;
            }
            w.collapsed = collapsed != 0.0f;
            if (lp.windows.size() < 4 * max_windows) { lp.windows.push_back(w); }
        } else {
            lp.ok = false;
        }
    }
    if (!lp.ok || lp.nodes.size() > max_dock_nodes) {
        return false;
    }

    // it is valid: replace what there is
    for (window_state& w : windows_) {
        if (w.key != 0 && w.dock != 0) { dock_detach(w); }
    }
    for (dock_node& n : dock_nodes_) { n = {}; }
    for (dock_space& sp : dock_spaces_) { sp.root = no_node; }

    for (const layout_window& lw : lp.windows) {
        window_state* w = window_for(lw.key, lw.pos, lw.size.x);
        if (w == nullptr) { continue; }
        w->pos       = lw.pos;
        w->width     = std::max(lw.size.x, 0.0f);
        w->height    = std::max(lw.size.y, 0.0f);
        w->collapsed = lw.collapsed;
        w->size_set  = true;
    }
    const auto build = [&](auto& self, int index, u8 parent, u8 space) -> u8 {
        const layout_node& pn = lp.nodes[static_cast<std::size_t>(index)];
        const u8 ni = dock_new_node();
        if (ni == no_node) { return no_node; }
        dock_node& n = dock_nodes_[ni];
        n.space  = space;
        n.parent = parent;
        if (!pn.leaf) {
            n.vertical = pn.vertical;
            n.ratio    = pn.ratio;
            const u8 a = self(self, pn.child[0], ni, space);
            const u8 b = self(self, pn.child[1], ni, space);
            dock_nodes_[ni].child = {a, b};
            return ni;
        }
        for (const id key : pn.windows) {
            window_state* w = window_for(key, {}, 0.0f);
            if (w == nullptr) { continue; }
            w->float_size = {w->width, w->height};
            w->dock       = static_cast<u32>(ni) + 1;
            w->dock_order = ++dock_counter_;
        }
        dock_nodes_[ni].active = pn.active;
        return ni;
    };
    for (const layout_space& ls : lp.spaces) {
        const u8 si = dock_space_at(ls.key);
        if (si == no_node) { continue; }
        dock_space& sp = dock_spaces_[si];
        if (ls.edge > 0.0f) {
            sp.edge_size = std::max(ls.edge, min_node_size);
            sp.edge      = true;
        }
        if (ls.root >= 0) { sp.root = build(build, ls.root, no_node, si); }
    }
    dock_relayout();
    return true;
}

} // namespace strata

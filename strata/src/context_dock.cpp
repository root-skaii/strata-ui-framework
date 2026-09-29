// docking: dock spaces (main, named, edge, floating), each a node tree over a region; leaves hold tabbed windows,
// splits divide their area between two children.
// per frame: dock_area / dock_edge / floating_dock place and lay out each space; docked windows take their leaf's
// content rect (begin_window). dock_end_frame() draws tab bars and splitters, handles their input, previews and
// applies drops.

#include "strata/context.hpp"

#include "context_impl.hpp"

#include "dock_state.hpp"
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
constexpr f32 guide_size   = 28.0f; // the drop guides shown while a window is dragged over a pane
constexpr f32 guide_gap    = 4.0f;
constexpr f32 border_guide = 24.0f; // ... and the ones along the border of a space
constexpr f32 tab_pad      = 26.0f; // what a tab takes on top of its title

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
        const dock_space& sp = m_->dock_->spaces[i];
        if (sp.key == key) {
            return static_cast<u8>(i);
        }
        if (free_slot == no_node && sp.key == 0) { free_slot = static_cast<u8>(i); }
        if (stale == no_node && sp.key != 0 && sp.root == no_node && sp.last_frame + 300 < m_->frame_) { stale = static_cast<u8>(i); }
    }
    const u8 pick = free_slot != no_node ? free_slot : stale;
    if (pick == no_node) {
        report_limit("dock spaces (max_dock_spaces)", max_dock_spaces);
        return no_node;
    }
    m_->dock_->spaces[pick]            = {};
    m_->dock_->spaces[pick].key        = key;
    m_->dock_->spaces[pick].last_frame = m_->frame_;
    return pick;
}

void context::dock_set_space(u8 space, const rect& area, const rect& drop, const rect& panel) noexcept
{
    dock_space& sp = m_->dock_->spaces[space];
    // a dragged floating dock moves its panes rigidly: only layout changes animate
    if (sp.owner != 0 && sp.root != no_node && sp.last_frame + 1 >= m_->frame_ && sp.last_frame != 0) {
        const vec2 delta = area.min - sp.area.min;
        if (delta.x != 0.0f || delta.y != 0.0f) {
            for (dock_node& n : m_->dock_->nodes) {
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
    sp.last_frame = m_->frame_;
    m_->dock_->any_set = true;
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
    if (si == no_node || m_->cur_ != nullptr) {
        return region;
    }
    dock_space& sp = m_->dock_->spaces[si];
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
    const bool dragging = m_->dock_->drag_prev != 0; // a dockable window is being dragged: an empty dock shows a drop strip

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
    const bool free_pointer = m_->win_.hovered_window_prev_ == 0 || m_->hovered_docked_prev_;
    const id key = hash_id("##dockedge", static_cast<id>(si + 1));
    const interaction in = interact_impl(key, gap.expanded(1.5f), free_pointer);
    if (in.hovered || in.held) { m_->cursor_ = horizontal ? cursor_kind::resize_ew : cursor_kind::resize_ns; }
    if (gap.expanded(1.5f).contains(m_->input_.mouse_)) { m_->dock_chrome_cur_ = true; }
    if (in.hovered && m_->input_.mouse_pressed_ && dock_double_click(key)) { // a double click gives the panel its usual size back
        sp.edge_size = std::clamp(std::max(size, min_node_size), min_node_size, room);
        m_->dock_->void_key   = key;
    }
    if (in.held && m_->dock_->void_key != key) {
        m_->dock_->splitting = true;
        f32 want = 0.0f;
        switch (side) {
        case dock_side::left:   want = m_->input_.mouse_.x - region.min.x - splitter_gap * 0.5f; break;
        case dock_side::right:  want = region.max.x - m_->input_.mouse_.x - splitter_gap * 0.5f; break;
        case dock_side::top:    want = m_->input_.mouse_.y - region.min.y - splitter_gap * 0.5f; break;
        default:                want = region.max.y - m_->input_.mouse_.y - splitter_gap * 0.5f; break;
        }
        sp.edge_size = std::clamp(want, min_node_size, room);
    }
    const color line = lerp(m_->style_.border, m_->style_.accent, in.held ? 1.0f : (in.hovered ? 0.7f : 0.0f));
    m_->dl_.rect_filled(gap, line.scaled_alpha(in.hovered || in.held ? 0.9f : 0.55f));
    return remaining;
}

// a window that is a dock space: docked windows fill its body and move with it
bool context::floating_dock(std::string_view title, vec2 initial_pos, vec2 size, window_flags flags)
{
    if (m_->cur_ != nullptr) {
        return false;
    }
    const bool open = begin_window(title, initial_pos, {size.x, size.y > 0.0f ? size.y : 320.0f}, flags | window_flags::resizable);
    const window_state& st = *m_->cur_;

    const u8 si = dock_space_at(dock_space_key(title));
    if (si != no_node) {
        dock_space& sp = m_->dock_->spaces[si];
        sp.owner  = hash_id(title, m_->ids_.root());
        sp.edge   = false;
        const rect frame = m_->cur_frame_;
        const rect body  = {{frame.min.x, frame.min.y + st.title_h}, frame.max};
        dock_set_space(si, body, body, body);
        sp.hidden = st.collapsed;
        if (open && sp.root == no_node) { // nothing in it yet
            const std::string_view hint = "drop windows here";
            const vec2 ts = m_->font_.measure(current_font(), hint);
            m_->dl_.text({body.center().x - ts.x * 0.5f, body.center().y - ts.y * 0.5f}, m_->style_.text_dim.scaled_alpha(0.6f), hint, current_font());
        }
    }
    end_window();
    return open;
}

bool context::dock_double_click(id key) noexcept
{
    const vec2 moved = m_->input_.mouse_ - m_->dock_->click_pos;
    const bool twice = m_->dock_->click_key == key && m_->time_ - m_->dock_->click_time < m_->double_click_ && dot(moved, moved) < 25.0f;
    m_->dock_->click_key  = twice ? id{} : key;
    m_->dock_->click_time = m_->time_;
    m_->dock_->click_pos  = m_->input_.mouse_;
    return twice;
}

// tabs ------------------------------------------------------------------------------------------

f32 context::dock_tab_width(const window_state& w) const noexcept
{
    return m_->font_.measure(0, {w.title.data(), w.title_len}).x + tab_pad;
}

// the windows sitting in a leaf as indices into m_->win_.windows_, in the order of their tabs
u32 context::dock_leaf_tabs(u8 leaf, std::array<u8, max_windows>& out, bool shown_only) const noexcept
{
    u32 count = 0;
    for (u32 i = 0; i < max_windows; ++i) {
        const window_state& w = m_->win_.windows_[i];
        if (w.key != 0 && w.dock == leaf + 1u && (!shown_only || w.last_frame + 1 >= m_->frame_)) { out[count++] = static_cast<u8>(i); }
    }
    std::sort(out.begin(), out.begin() + count, [this](u8 a, u8 b) { return m_->win_.windows_[a].dock_order < m_->win_.windows_[b].dock_order; });
    return count;
}

// puts a docked window at a place among the tabs of its pane
void context::dock_move_tab(window_state& w, u32 index) noexcept
{
    if (w.dock == 0 || w.dock > max_dock_nodes) {
        return;
    }
    std::array<u8, max_windows> tabs{};
    const u32 count = dock_leaf_tabs(static_cast<u8>(w.dock - 1), tabs, false);
    std::array<window_state*, max_windows> seq{};
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        if (&m_->win_.windows_[tabs[i]] != &w) { seq[n++] = &m_->win_.windows_[tabs[i]]; }
    }
    index = std::min(index, n);
    for (u32 i = n; i > index; --i) { seq[i] = seq[i - 1]; }
    seq[index] = &w;
    for (u32 i = 0; i <= n; ++i) { seq[i]->dock_order = ++m_->dock_->counter; }
}

// tree ------------------------------------------------------------------------------------------

u8 context::dock_new_node() noexcept
{
    for (u32 i = 0; i < max_dock_nodes; ++i) {
        if (!m_->dock_->nodes[i].used) {
            m_->dock_->nodes[i]      = {};
            m_->dock_->nodes[i].used = true;
            return static_cast<u8>(i);
        }
    }
    return no_node;
}

void context::dock_free_node(u8 node) noexcept
{
    if (node != no_node) {
        m_->dock_->nodes[node] = {};
    }
}

void context::dock_compute(u8 index, const rect& area) noexcept
{
    dock_node& n = m_->dock_->nodes[index];
    n.area = area;
    if (n.leaf()) {
        const f32 tab_h = m_->font_.line_height(0) + 10.0f;
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
    for (const dock_space& sp : m_->dock_->spaces) {
        if (sp.set && sp.root != no_node) {
            dock_compute(sp.root, sp.area);
        }
    }
    // what is drawn is the layout, unless panes are meant to slide there (dock_animate moves them)
    for (dock_node& n : m_->dock_->nodes) {
        if (n.used && (!m_->dock_animation_ || n.fresh)) {
            n.shown_area    = n.area;
            n.shown_content = n.content;
            n.fresh         = false;
        }
    }
}

// one step of the slide of every pane towards its place in the layout; once per frame
void context::dock_animate()
{
    if (!m_->dock_animation_) {
        return;
    }
    const f32 k = m_->dock_->splitting ? 1.0f : 1.0f - std::exp(-16.0f * m_->dt_);
    const auto step = [k](rect& shown, const rect& target) {
        const auto toward = [k](f32 a, f32 b) { return std::abs(b - a) < 0.5f ? b : a + (b - a) * k; };
        shown = {{toward(shown.min.x, target.min.x), toward(shown.min.y, target.min.y)},
                 {toward(shown.max.x, target.max.x), toward(shown.max.y, target.max.y)}};
    };
    for (dock_node& n : m_->dock_->nodes) {
        if (n.used) {
            step(n.shown_area, n.area);
            step(n.shown_content, n.content);
        }
    }
}

// puts a window into a leaf (center) or splits a node to make room for a new leaf next to it
void context::dock_attach(window_state& w, u8 space, u8 node, dock_zone zone, bool outer, f32 size, int tab) noexcept
{
    if (space >= max_dock_spaces) {
        return;
    }
    if (w.dock != 0) {
        dock_detach(w);
    }
    dock_space& sp = m_->dock_->spaces[space];

    u8 leaf = no_node;
    if (sp.root == no_node) { // the first docked window fills the whole space
        leaf = dock_new_node();
        if (leaf == no_node) {
            report_limit("dock panes (max_dock_nodes)", max_dock_nodes);
            return;
        }
        m_->dock_->nodes[leaf].space = space;
        sp.root = leaf;
    } else {
        if (outer || node == no_node) { node = sp.root; }
        if (m_->dock_->nodes[node].leaf() && zone == dock_zone::center && !outer) {
            leaf = node;
        } else if (!m_->dock_->nodes[node].leaf() && zone == dock_zone::center) {
            while (!m_->dock_->nodes[node].leaf()) { node = m_->dock_->nodes[node].child[0]; } // a split has no tabs: use its first leaf
            leaf = node;
        } else {
            const u8 split = dock_new_node();
            leaf           = dock_new_node();
            if (split == no_node || leaf == no_node) {
                report_limit("dock panes (max_dock_nodes)", max_dock_nodes);
                dock_free_node(split);
                dock_free_node(leaf);
                return;
            }
            dock_node& s = m_->dock_->nodes[split];
            dock_node& n = m_->dock_->nodes[node];
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
                dock_node& p = m_->dock_->nodes[n.parent];
                (p.child[0] == node ? p.child[0] : p.child[1]) = split;
            }
            n.parent                 = split;
            m_->dock_->nodes[leaf].parent = split;
            m_->dock_->nodes[leaf].space  = space;
        }
    }

    const bool slide = m_->dock_animation_ && m_->dock_->nodes[leaf].fresh && zone != dock_zone::center;
    w.float_size = {w.width, w.height};
    w.dock       = static_cast<u32>(leaf) + 1;
    w.dock_order = ++m_->dock_->counter;
    m_->dock_->nodes[leaf].active = w.key;
    if (tab >= 0) { dock_move_tab(w, static_cast<u32>(tab)); }
    dock_relayout();
    if (slide) { // the new pane grows out of the edge it was dropped at
        dock_node& l = m_->dock_->nodes[leaf];
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
            const dock_node& n = m_->dock_->nodes[i];
            if (!n.used || !n.leaf()) { continue; }

            bool occupied = false;
            for (const window_state& w : m_->win_.windows_) {
                occupied = occupied || (w.key != 0 && w.dock == i + 1 && w.last_frame == m_->frame_);
            }
            if (occupied) { continue; }

            for (window_state& w : m_->win_.windows_) { // windows that are not shown right now forget this node as well
                if (w.dock == i + 1) { dock_detach(w); }
            }
            dock_space& sp     = m_->dock_->spaces[n.space];
            const u8    parent = n.parent;
            dock_free_node(static_cast<u8>(i));
            if (parent == no_node) {
                sp.root = no_node;
            } else {
                dock_node& p = m_->dock_->nodes[parent];
                const u8 sibling = p.child[0] == i ? p.child[1] : p.child[0];
                const u8 grand   = p.parent;
                m_->dock_->nodes[sibling].parent = grand;
                if (grand == no_node) {
                    sp.root = sibling;
                } else {
                    dock_node& g = m_->dock_->nodes[grand];
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
dock_target context::dock_pick(vec2 pointer, u8 exclude_leaf, vec2 want) const noexcept
{
    dock_target t;

    // which space: floating docks are on top of the others (the higher one first)
    int best = -1;
    u32 best_rank = 0;
    for (u32 si = 0; si < max_dock_spaces; ++si) {
        const dock_space& sp = m_->dock_->spaces[si];
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
    const dock_space& sp = m_->dock_->spaces[static_cast<u32>(best)];
    t.valid = true;
    t.space = static_cast<u8>(best);
    if (sp.root == no_node) { // empty space: the window would fill it (edge dock: the whole panel)
        t.preview = sp.panel;
        return t;
    }

    // (a pane that is being moved cannot be dropped on itself)
    if (exclude_leaf != no_node && sp.root == exclude_leaf) {
        t.valid = false;
        return t;
    }

    u8 found = no_node;
    for (u32 i = 0; i < max_dock_nodes; ++i) {
        const dock_node& n = m_->dock_->nodes[i];
        if (n.used && n.leaf() && n.space == best && n.area.contains(pointer)) { found = static_cast<u8>(i); break; }
    }
    if (found != exclude_leaf) { t.leaf = found; }

    // share of `whole` a new pane on side `z` takes: about the window's size (small windows do not grab half a big
    // pane), within limits; without a size, half a pane or a quarter of a space
    const rect& a = sp.area;
    const auto share_of = [want](dock_zone z, bool outer, const rect& whole) -> f32 {
        const bool horizontal = z == dock_zone::left || z == dock_zone::right;
        const f32  have  = horizontal ? want.x : want.y;
        const f32  total = horizontal ? whole.width() : whole.height();
        if (have <= 0.0f || total <= 0.0f) { return outer ? 0.25f : 0.5f; }
        return outer ? std::clamp(have / total, 0.15f, 0.4f) : std::clamp(have / total, 0.2f, 0.5f);
    };
    // the whole space, or the top / bottom / left / right part of `r`
    const auto part_of = [](const rect& r, dock_zone z, f32 share) -> rect {
        const f32 fw = r.width() * share;
        const f32 fh = r.height() * share;
        switch (z) {
        case dock_zone::center: return r;
        case dock_zone::left:   return {r.min, {r.min.x + fw, r.max.y}};
        case dock_zone::right:  return {{r.max.x - fw, r.min.y}, r.max};
        case dock_zone::top:    return {r.min, {r.max.x, r.min.y + fh}};
        default:                return {{r.min.x, r.max.y - fh}, r.max};
        }
    };
    // the zone, and with it what the drop does: the share of the new pane and the preview of it
    const auto set_zone = [&](dock_zone z, bool outer, const rect& whole) {
        t.zone    = z;
        t.outer   = outer;
        t.share   = z == dock_zone::center ? 0.0f : share_of(z, outer, whole);
        t.preview = part_of(whole, z, t.share);
    };

    // the guides are big targets and win over the rest
    std::array<dock_guide, 9> guides;
    const u32 guide_count = dock_guides(sp, t.leaf, guides);
    for (u32 i = 0; i < guide_count; ++i) {
        const dock_guide& g = guides[i];
        if (!g.r.contains(pointer)) { continue; }
        t.on_guide = true;
        t.node     = g.outer ? sp.root : t.leaf;
        set_zone(g.zone, g.outer, g.outer ? a : m_->dock_->nodes[t.leaf].area);
        return t;
    }

    // near the space border: split the whole tree (a top pane's tab bar is for tabs; the guide owns the top border)
    const f32 tab_h   = m_->font_.line_height(0) + 10.0f;
    const bool on_bar = t.leaf != no_node && pointer.y < m_->dock_->nodes[t.leaf].area.min.y + tab_h;
    const f32 dl = pointer.x - a.min.x;
    const f32 dr = a.max.x - pointer.x;
    const f32 dt = on_bar ? a.height() : pointer.y - a.min.y;
    const f32 db = a.max.y - pointer.y;
    const f32 nearest = std::min({dl, dr, dt, db});
    if (nearest < outer_edge) {
        t.node = sp.root;
        set_zone(nearest == dl ? dock_zone::left : nearest == dr ? dock_zone::right : nearest == dt ? dock_zone::top : dock_zone::bottom, true, a);
        return t;
    }

    if (found == no_node || found == exclude_leaf) {
        t.valid = false; // over a splitter, or over the pane being moved
        return t;
    }
    const rect& r = m_->dock_->nodes[found].area;
    t.node = found;

    // over the tab bar of a pane: join its tabs, at the place under the pointer
    if (on_bar) {
        std::array<u8, max_windows> tabs{};
        const u32 count = dock_leaf_tabs(found, tabs, true);
        f32 total = 0.0f;
        for (u32 i = 0; i < count; ++i) { total += dock_tab_width(m_->win_.windows_[tabs[i]]); }
        const f32 squeeze = total > r.width() ? r.width() / total : 1.0f;
        f32 x = r.min.x;
        u32 index = count;
        for (u32 i = 0; i < count; ++i) {
            const f32 w = dock_tab_width(m_->win_.windows_[tabs[i]]) * squeeze;
            if (pointer.x < x + w * 0.5f) { index = i; break; }
            x += w;
        }
        x = std::clamp(x - 1.5f, r.min.x, r.max.x - 3.0f); // (x is where the new tab starts)
        t.zone    = dock_zone::center;
        t.tab     = static_cast<int>(index);
        t.marker  = {{x, r.min.y + 3.0f}, {x + 3.0f, r.min.y + tab_h - 3.0f}};
        t.preview = r;
        return t;
    }

    const f32 rx = (pointer.x - r.min.x) / std::max(r.width(), 1.0f);
    const f32 ry = (pointer.y - r.min.y) / std::max(r.height(), 1.0f);
    if (rx > 0.3f && rx < 0.7f && ry > 0.3f && ry < 0.7f) {
        t.zone    = dock_zone::center;
        t.preview = r;
        return t;
    }
    const f32 rr = 1.0f - rx;
    const f32 bb = 1.0f - ry;
    const f32 m  = std::min({rx, rr, ry, bb});
    set_zone(m == rx ? dock_zone::left : m == rr ? dock_zone::right : m == ry ? dock_zone::top : dock_zone::bottom, false, r);
    return t;
}

// drop guides: a five-button cross in the pane body, plus the space borders
u32 context::dock_guides(const dock_space& sp, u8 leaf, std::array<dock_guide, 9>& out) const noexcept
{
    u32 n = 0;
    if (sp.root == no_node) {
        return 0;
    }
    const auto add = [&](dock_zone z, bool outer, vec2 center, f32 size) {
        out[n++] = {z, outer, rect::from_size({center.x - size * 0.5f, center.y - size * 0.5f}, {size, size})};
    };
    const rect& a = sp.area;
    if (a.width() >= 480.0f && a.height() >= 360.0f) { // (on a small space they would crowd the cross of its panes)
        const f32 m = 8.0f + border_guide * 0.5f;
        const vec2 c = a.center();
        add(dock_zone::left,   true, {a.min.x + m, c.y}, border_guide);
        add(dock_zone::right,  true, {a.max.x - m, c.y}, border_guide);
        add(dock_zone::top,    true, {c.x, a.min.y + m}, border_guide);
        add(dock_zone::bottom, true, {c.x, a.max.y - m}, border_guide);
    }
    if (leaf != no_node) {
        const rect& body = m_->dock_->nodes[leaf].content;
        const f32 step = guide_size + guide_gap;
        if (body.width() >= 3.0f * step + 24.0f && body.height() >= 3.0f * step + 24.0f) {
            const vec2 c = body.center();
            add(dock_zone::center, false, c, guide_size);
            add(dock_zone::left,   false, {c.x - step, c.y}, guide_size);
            add(dock_zone::right,  false, {c.x + step, c.y}, guide_size);
            add(dock_zone::top,    false, {c.x, c.y - step}, guide_size);
            add(dock_zone::bottom, false, {c.x, c.y + step}, guide_size);
        }
    }
    return n;
}

bool context::dock_window(std::string_view title, dock_zone zone, std::string_view target, f32 size, std::string_view space_name)
{
    const id wid = hash_id(title, m_->ids_.root());
    window_state* w = window_for(wid, {}, 0.0f);
    if (w == nullptr) {
        return false;
    }
    u8 space = no_node;
    u8 node  = no_node;
    bool outer = false;
    if (!target.empty()) {
        const window_state* t = window_find(hash_id(target, m_->ids_.root()));
        if (t == nullptr || t->dock == 0 || t->dock > max_dock_nodes || !m_->dock_->nodes[t->dock - 1].used) {
            return false;
        }
        node  = static_cast<u8>(t->dock - 1);
        space = m_->dock_->nodes[node].space;
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
    if (window_state* w = window_find(hash_id(title, m_->ids_.root()))) {
        dock_detach(*w);
    }
}

bool context::is_docked(std::string_view title) const noexcept
{
    const window_state* w = window_find(hash_id(title, m_->ids_.root()));
    return w != nullptr && w->dock != 0;
}

// per frame: chrome, input, drop ---------------------------------------------------------------------

void context::dock_end_frame()
{
    if (!m_->dock_->any_set) {
        m_->dock_->splitting = false;
        if (!m_->input_.mouse_down_) { m_->dock_->void_key = 0; }
        return;
    }
    dock_relayout();

    const f32 lh    = m_->font_.line_height(0);
    const f32 tab_h = lh + 10.0f;

    // dock chrome belongs to no window: usable only when no floating window covers the pointer (a floating dock's own
    // window excepted: its tab bars sit on its body)
    const bool free_pointer = m_->win_.hovered_window_prev_ == 0 || m_->hovered_docked_prev_;

    for (u32 ni = 0; ni < max_dock_nodes; ++ni) {
        dock_node& n = m_->dock_->nodes[ni];
        if (!n.used) { continue; }
        const dock_space& sp = m_->dock_->spaces[n.space];
        if (!sp.set || sp.hidden) { continue; }

        // the chrome of a floating dock is drawn into that window's layer, so it stacks with it
        u32 layer = run_base;
        if (sp.owner != 0) {
            for (u32 i = 0; i < m_->win_.frame_window_count_; ++i) {
                if (m_->win_.frame_windows_[i]->key == sp.owner) { layer = i; break; }
            }
        }
        switch_run(layer);
        const bool free = free_pointer || (sp.owner != 0 && m_->win_.hovered_window_prev_ == sp.owner);
        const auto chrome = [&](id key, const rect& r) { return interact_impl(key, r, free); };

        if (!n.leaf()) { // splitter between the two children
            const dock_node& a = m_->dock_->nodes[n.child[0]];
            const dock_node& b = m_->dock_->nodes[n.child[1]];
            const rect gap = n.vertical ? rect{{n.shown_area.min.x, a.shown_area.max.y}, {n.shown_area.max.x, b.shown_area.min.y}}
                                        : rect{{a.shown_area.max.x, n.shown_area.min.y}, {b.shown_area.min.x, n.shown_area.max.y}};
            const id skey = hash_id("##dsplit", static_cast<id>(ni + 1));
            const interaction in = chrome(skey, gap.expanded(1.5f));
            if (in.hovered || in.held) {
                m_->cursor_ = n.vertical ? cursor_kind::resize_ns : cursor_kind::resize_ew;
            }
            if (gap.expanded(1.5f).contains(m_->input_.mouse_)) { m_->dock_chrome_cur_ = true; }
            if (in.hovered && m_->input_.mouse_pressed_ && dock_double_click(skey)) { // a double click shares the space equally again
                n.ratio    = 0.5f;
                m_->dock_->void_key = skey;
            }
            if (in.held && m_->dock_->void_key != skey) {
                m_->dock_->splitting = true;
                const f32 span = (n.vertical ? n.area.height() : n.area.width()) - splitter_gap;
                if (span >= 2.0f * min_node_size) {
                    const f32 pos = (n.vertical ? m_->input_.mouse_.y - n.area.min.y : m_->input_.mouse_.x - n.area.min.x) - splitter_gap * 0.5f;
                    n.ratio = std::clamp(pos / span, min_node_size / span, 1.0f - min_node_size / span);
                }
            }
            const color line = lerp(m_->style_.border, m_->style_.accent, in.held ? 1.0f : (in.hovered ? 0.7f : 0.0f));
            m_->dl_.rect_filled(gap, line.scaled_alpha(in.hovered || in.held ? 0.9f : 0.55f));
            continue;
        }

        std::array<window_state*, max_windows> tabs{};
        u32 count = 0;
        for (u32 i = 0; i < m_->win_.frame_window_count_; ++i) {
            window_state* w = m_->win_.frame_windows_[i];
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
        if (bar.contains(m_->input_.mouse_)) { m_->dock_chrome_cur_ = true; }
        shape_style head;
        head.fill_top    = lerp(m_->style_.title_bg, color{255, 255, 255, m_->style_.title_bg.a}, m_->style_.gradient * 0.8f);
        head.fill_bottom = m_->style_.title_bg;
        m_->dl_.shape(bar, head);
        m_->dl_.rect_filled({{bar.min.x, bar.max.y - 1.0f}, bar.max}, m_->style_.border);

        // tab widths: as wide as the title, squeezed equally when they do not all fit
        std::array<f32, max_windows> widths{};
        f32 total = 0.0f;
        for (u32 i = 0; i < count; ++i) {
            widths[i] = dock_tab_width(*tabs[i]);
            total += widths[i];
        }
        const f32 squeeze = total > bar.width() ? bar.width() / total : 1.0f;
        std::array<f32, max_windows> centers{}; // (of the tabs as they are laid out now: where a dragged tab goes)
        {
            f32 cx = bar.min.x;
            for (u32 i = 0; i < count; ++i) {
                centers[i] = cx + widths[i] * squeeze * 0.5f;
                cx += widths[i] * squeeze;
            }
        }

        f32 x = bar.min.x;
        for (u32 i = 0; i < count; ++i) {
            window_state& w = *tabs[i];
            const f32 tw = widths[i] * squeeze;
            const rect cell = {{x, bar.min.y}, {x + tw, bar.max.y}};
            x += tw;

            const id key = hash_id("##dtab", w.key);
            const interaction in = chrome(key, cell);
            if (in.hovered && m_->input_.mouse_pressed_) {
                n.active        = w.key;
                m_->dock_->press_pos = m_->input_.mouse_;
                if (dock_double_click(key)) { // a double click floats the window where the pane is
                    dock_detach(w);
                    w.pos = {n.shown_area.min.x + 24.0f, n.shown_area.min.y + 24.0f};
                    bring_to_front(w.key);
                    continue;
                }
            }
            if (in.held) {
                const vec2 d = m_->input_.mouse_ - m_->dock_->press_pos;
                // along its own bar a tab moves among the others (which make room) ...
                const bool in_bar = m_->input_.mouse_.y >= bar.min.y - 14.0f && m_->input_.mouse_.y <= bar.max.y + 14.0f;
                if (count > 1 && in_bar) {
                    u32 want = 0;
                    for (u32 j = 0; j < count; ++j) {
                        if (j != i && m_->input_.mouse_.x > centers[j]) { ++want; }
                    }
                    if (want != i) { dock_move_tab(w, want); }
                } else if (dot(d, d) > 36.0f) {
                    // ... pulled away, it undocks and floats under the pointer
                    const f32 fw = w.float_size.x > 0.0f ? w.float_size.x : 320.0f;
                    dock_detach(w);
                    w.pos = {m_->input_.mouse_.x - std::min(fw * 0.25f, 80.0f), m_->input_.mouse_.y - tab_h * 0.5f};
                    bring_to_front(w.key);
                    m_->active_ = hash_id("##drag", w.key);
                    continue;
                }
            }

            const bool sel = w.key == n.active;
            anim_slot& a = anim_for(key);
            a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
            a.toggle = approach(a.toggle, sel ? 1.0f : 0.0f);
            if (a.toggle > 0.01f || a.hover > 0.01f) {
                shape_style bg;
                bg.radius      = radii(m_->style_.rounding * 0.6f, corners::top);
                bg.fill_top    = m_->style_.window_bg.scaled_alpha(a.toggle);
                bg.fill_bottom = bg.fill_top;
                if (!sel) { bg.fill_top = m_->style_.widget_hover.scaled_alpha(0.6f * a.hover); bg.fill_bottom = bg.fill_top; }
                m_->dl_.shape({{cell.min.x + 1.0f, cell.min.y + 3.0f}, {cell.max.x - 1.0f, cell.max.y}}, bg);
            }
            if (sel) {
                m_->dl_.rect_filled({{cell.min.x + 6.0f, cell.max.y - 2.0f}, {cell.max.x - 6.0f, cell.max.y}}, m_->style_.accent);
            }
            m_->dl_.push_clip({{cell.min.x + 4.0f, cell.min.y}, {cell.max.x - 4.0f, cell.max.y}});
            m_->dl_.text({cell.min.x + 13.0f, cell.min.y + (tab_h - lh) * 0.5f}, lerp(m_->style_.text_dim, m_->style_.text, std::max(a.toggle, a.hover * 0.7f)),
                     {w.title.data(), w.title_len}, 0);
            m_->dl_.pop_clip();
        }

        // the rest of the bar is a grip that drags the whole pane with all its tabs
        const rect grip = {{x, bar.min.y}, bar.max};
        if (grip.width() >= 16.0f) {
            const interaction in = chrome(hash_id("##dgrip", static_cast<id>(ni + 1)), grip);
            if (in.hovered && m_->input_.mouse_pressed_) {
                m_->dock_->group_src   = static_cast<u8>(ni);
                m_->dock_->group_moved = false;
                m_->dock_->press_pos   = m_->input_.mouse_;
            }
            if (in.held && m_->dock_->group_src == ni && !m_->dock_->group_moved) {
                const vec2 d = m_->input_.mouse_ - m_->dock_->press_pos;
                m_->dock_->group_moved = dot(d, d) > 36.0f;
            }
            const bool lit = in.hovered || (in.held && m_->dock_->group_src == ni);
            const color dots = m_->style_.text_dim.scaled_alpha(lit ? 0.9f : 0.35f);
            const f32 gx = bar.max.x - 12.0f;
            const f32 gy = bar.min.y + (tab_h - 12.0f) * 0.5f;
            for (u32 cx = 0; cx < 2; ++cx) {
                for (u32 cy = 0; cy < 3; ++cy) {
                    const f32 px = gx + static_cast<f32>(cx) * 4.0f;
                    const f32 py = gy + static_cast<f32>(cy) * 4.0f;
                    m_->dl_.rect_filled({{px, py}, {px + 2.0f, py + 2.0f}}, dots);
                }
            }
        }
    }
    switch_run(run_base);

    // a pane being carried by its grip: where it would land, and the drop
    if (m_->dock_->group_src != no_node) {
        const bool src_ok = m_->dock_->nodes[m_->dock_->group_src].used && m_->dock_->nodes[m_->dock_->group_src].leaf() &&
                            m_->dock_->spaces[m_->dock_->nodes[m_->dock_->group_src].space].set;
        if (!src_ok || (!m_->input_.mouse_down_ && !m_->input_.mouse_released_) || key_pressed(key::escape)) { // (Esc: the pane stays where it is)
            m_->dock_->group_src   = no_node;
            m_->dock_->group_moved = false;
        } else if (m_->dock_->group_moved) {
            std::array<window_state*, max_windows> group{};
            u32 count = 0;
            for (u32 i = 0; i < m_->win_.frame_window_count_; ++i) {
                if (m_->win_.frame_windows_[i]->dock == m_->dock_->group_src + 1u && m_->win_.frame_windows_[i]->docked_now) { group[count++] = m_->win_.frame_windows_[i]; }
            }
            std::sort(group.begin(), group.begin() + count, [](const window_state* a, const window_state* b) {
                return a->dock_order < b->dock_order;
            });
            const dock_target t = m_->input_.mod_shift_ ? dock_target{} : dock_pick(m_->input_.mouse_, m_->dock_->group_src, group[0] != nullptr ? group[0]->float_size : vec2{}); // (Shift: do not dock, float)
            if (m_->input_.mouse_released_) {
                const id active = m_->dock_->nodes[m_->dock_->group_src].active;
                if (t.valid && count > 0) {
                    dock_attach(*group[0], t.space, t.node, t.zone, t.outer, t.share, t.tab);
                    const u32 leaf = group[0]->dock;
                    for (u32 i = 1; i < count && leaf != 0; ++i) {
                        dock_attach(*group[i], t.space, static_cast<u8>(leaf - 1), dock_zone::center, false, 0.0f,
                                    t.tab >= 0 ? t.tab + static_cast<int>(i) : -1);
                    }
                    if (leaf != 0) { m_->dock_->nodes[leaf - 1].active = active; }
                } else {
                    // dropped on nothing: the windows float again, fanned out from the pointer
                    for (u32 i = 0; i < count; ++i) {
                        const f32 fw = group[i]->float_size.x > 0.0f ? group[i]->float_size.x : 320.0f;
                        dock_detach(*group[i]);
                        const f32 fan = static_cast<f32>(i) * 26.0f;
                        group[i]->pos = {m_->input_.mouse_.x - std::min(fw * 0.25f, 80.0f) + fan, m_->input_.mouse_.y - tab_h * 0.5f + fan};
                        bring_to_front(group[i]->key);
                    }
                }
                m_->dock_->group_src   = no_node;
                m_->dock_->group_moved = false;
                m_->dock_->drag_win    = 0;
                m_->dock_->drag_prev   = 0;
                m_->dock_->target_prev = {};
            } else {
                m_->dock_->target_cur = t;
                m_->dock_->drag_win   = group[0] != nullptr ? group[0]->key : m_->dock_->nodes[m_->dock_->group_src].active;
                // the pane follows the pointer as a small ghost of its tab bar
                const u32 saved_owner = m_->run_owner_;
                switch_run(run_overlay);
                m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
                const std::string label = count > 1 ? std::format("{} windows", count)
                                                    : std::string{group[0] != nullptr ? std::string_view{group[0]->title.data(), group[0]->title_len} : ""};
                const f32 gw = m_->font_.measure(0, label).x + 28.0f;
                const rect ghost = rect::from_size({m_->input_.mouse_.x + 12.0f, m_->input_.mouse_.y + 10.0f}, {gw, tab_h});
                shape_style gs;
                gs.radius       = radii(m_->style_.rounding * 0.6f);
                gs.fill_top     = m_->style_.window_bg.scaled_alpha(0.92f);
                gs.fill_bottom  = gs.fill_top;
                gs.border       = m_->style_.accent;
                gs.border_width = 1.5f;
                gs.shadow       = color{0, 0, 0, 110};
                gs.shadow_blur  = 10.0f;
                m_->dl_.shape(ghost, gs);
                m_->dl_.text({ghost.min.x + 14.0f, ghost.min.y + (tab_h - lh) * 0.5f}, m_->style_.text, label, 0);
                m_->dl_.pop_clip();
                switch_run(saved_owner);
            }
        }
    }

    // the drop preview of a window being dragged over a space, and the drop itself
    if (m_->dock_->target_cur.space < max_dock_spaces) {
        const dock_target& tg = m_->dock_->target_cur;
        const u32 saved_owner = m_->run_owner_;
        switch_run(run_overlay);
        m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
        if (tg.valid) {
            shape_style pv;
            pv.radius       = radii(m_->style_.rounding * 0.6f);
            pv.fill_top     = m_->style_.accent.scaled_alpha(0.22f);
            pv.fill_bottom  = m_->style_.accent.scaled_alpha(0.14f);
            pv.border       = m_->style_.accent.scaled_alpha(0.9f);
            pv.border_width = 2.0f;
            m_->dl_.shape(tg.preview.expanded(-3.0f), pv);
            if (tg.tab >= 0) { m_->dl_.rect_filled(tg.marker, m_->style_.accent); } // where the tab would go
        }
        std::array<dock_guide, 9> guides;
        const u32 guide_count = dock_guides(m_->dock_->spaces[tg.space], tg.leaf, guides);
        for (u32 i = 0; i < guide_count; ++i) {
            const dock_guide& g = guides[i];
            const bool hot = tg.on_guide && tg.valid && tg.zone == g.zone && tg.outer == g.outer;
            shape_style gs;
            gs.radius       = radii(m_->style_.rounding * 0.6f);
            gs.fill_top     = hot ? m_->style_.accent.scaled_alpha(0.95f) : m_->style_.window_bg.scaled_alpha(0.9f);
            gs.fill_bottom  = gs.fill_top;
            gs.border       = m_->style_.accent.scaled_alpha(hot ? 1.0f : 0.65f);
            gs.border_width = 1.5f;
            gs.shadow       = color{0, 0, 0, 90};
            gs.shadow_blur  = 6.0f;
            m_->dl_.shape(g.r, gs);
            // what it does, drawn small: the part of the pane the new window takes (all of it: a tab)
            const rect in = g.r.expanded(-7.0f);
            const vec2 c  = in.center();
            rect part = in;
            switch (g.zone) {
            case dock_zone::left:   part.max.x = c.x; break;
            case dock_zone::right:  part.min.x = c.x; break;
            case dock_zone::top:    part.max.y = c.y; break;
            case dock_zone::bottom: part.min.y = c.y; break;
            default:                part = in.expanded(-2.0f); break;
            }
            m_->dl_.rect_filled(in, (hot ? m_->style_.window_bg : m_->style_.text_dim).scaled_alpha(0.3f));
            m_->dl_.rect_filled(part, hot ? color{255, 255, 255, 240} : m_->style_.accent);
        }
        m_->dl_.pop_clip();
        switch_run(saved_owner);
    }
    if (m_->input_.mouse_released_ && m_->dock_->target_prev.valid && m_->dock_->drag_prev != 0 && m_->dock_->drag_prev != m_->dock_->void_key &&
        m_->dock_->target_prev.space < max_dock_spaces) {
        const dock_space& target_space = m_->dock_->spaces[m_->dock_->target_prev.space];
        if (target_space.set && target_space.drop.contains(m_->input_.mouse_)) {
            // let go over a space: the window that was being dragged joins it
            window_state* dragged = window_find(m_->dock_->drag_prev);
            if (dragged != nullptr && dragged->dock == 0) {
                const dock_target t = m_->dock_->target_prev;
                dock_attach(*dragged, t.space, t.node, t.zone, t.outer, t.share, t.tab);
            }
        }
    }
    dock_prune();
    dock_animate();
    m_->dock_->splitting = false;
    if (!m_->input_.mouse_down_) { m_->dock_->void_key = 0; }
}

// layout text ------------------------------------------------------------------------------------
//   strata-dock 2
//   space <key> <edge size>        per space with panes (or edge dock); its tree follows in pre-order:
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
    std::string out = "strata-dock 2\n";
    const auto node_out = [&](auto& self, u8 ni) -> void {
        const dock_node& n = m_->dock_->nodes[ni];
        if (!n.leaf()) {
            out += std::format("S {} {}\n", n.vertical ? 'v' : 'h', n.ratio);
            self(self, n.child[0]);
            self(self, n.child[1]);
            return;
        }
        std::vector<const window_state*> tabs;
        for (const window_state& w : m_->win_.windows_) {
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
    for (const dock_space& sp : m_->dock_->spaces) {
        if (sp.key == 0 || (sp.root == no_node && !sp.edge)) { continue; }
        out += std::format("space {:x} {}\n", sp.key, sp.edge_size);
        if (sp.root != no_node) {
            node_out(node_out, sp.root);
        } else {
            out += "L 0 0\n";
        }
    }
    for (const window_state& w : m_->win_.windows_) {
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
    if (lp.lines.empty() || lp.lines[0] != "strata-dock 2") {
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
    for (window_state& w : m_->win_.windows_) {
        if (w.key != 0 && w.dock != 0) { dock_detach(w); }
    }
    for (dock_node& n : m_->dock_->nodes) { n = {}; }
    for (dock_space& sp : m_->dock_->spaces) { sp.root = no_node; }

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
        dock_node& n = m_->dock_->nodes[ni];
        n.space  = space;
        n.parent = parent;
        if (!pn.leaf) {
            n.vertical = pn.vertical;
            n.ratio    = pn.ratio;
            const u8 a = self(self, pn.child[0], ni, space);
            const u8 b = self(self, pn.child[1], ni, space);
            m_->dock_->nodes[ni].child = {a, b};
            return ni;
        }
        for (const id key : pn.windows) {
            window_state* w = window_for(key, {}, 0.0f);
            if (w == nullptr) { continue; }
            w->float_size = {w->width, w->height};
            w->dock       = static_cast<u32>(ni) + 1;
            w->dock_order = ++m_->dock_->counter;
        }
        m_->dock_->nodes[ni].active = pn.active;
        return ni;
    };
    for (const layout_space& ls : lp.spaces) {
        const u8 si = dock_space_at(ls.key);
        if (si == no_node) { continue; }
        dock_space& sp = m_->dock_->spaces[si];
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

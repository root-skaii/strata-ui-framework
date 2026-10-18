#include "strata/node_editor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata {

namespace {

constexpr f32 pin_radius   = 5.0f;
constexpr f32 pin_hit      = 9.0f;
constexpr f32 pin_gap      = 6.0f;
constexpr f32 node_pad     = 9.0f;
constexpr f32 min_node_w   = 110.0f;
constexpr f32 link_hit     = 6.0f;
constexpr u32 curve_points = 24;

enum class row_kind : u8 { input, output, text };

struct row_rec {
    row_kind kind{};
    u64      pin{};
    u32      type{};
    u32      text_at{};
    u32      text_len{};
};

struct node_rec {
    u64  id{};
    vec2* pos{};
    u32  title_at{}, title_len{};
    u32  first_row{}, row_count{};
    rect box;
};

struct pin_rec {
    u64  id{};
    vec2 pos;
    u32  node{};
    u32  type{};
    bool input{};
    bool linked{};
};

struct link_rec {
    node_link ids;
    u32       from{}; // pin_rec indices
    u32       to{};
    std::array<vec2, 4> curve;
};

[[nodiscard]] std::array<vec2, 4> link_curve(vec2 a, vec2 b, f32 z) noexcept
{
    const f32 reach = std::max(50.0f * z, std::abs(b.x - a.x) * 0.5f);
    return {a, a + vec2{reach, 0.0f}, b - vec2{reach, 0.0f}, b};
}

[[nodiscard]] vec2 curve_at(const std::array<vec2, 4>& c, f32 t) noexcept
{
    const f32 u = 1.0f - t;
    return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

[[nodiscard]] f32 distance_to_segment(vec2 p, vec2 a, vec2 b) noexcept
{
    const vec2 ab = b - a;
    const f32  len2 = dot(ab, ab);
    const f32  t = len2 > 0.0f ? std::clamp(dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const vec2 d = p - (a + ab * t);
    return std::sqrt(dot(d, d));
}

[[nodiscard]] f32 distance_to_curve(vec2 p, const std::array<vec2, 4>& c) noexcept
{
    f32  best = 1.0e9f;
    vec2 prev = c[0];
    for (u32 i = 1; i <= curve_points; ++i) {
        const vec2 next = curve_at(c, static_cast<f32>(i) / static_cast<f32>(curve_points));
        best = std::min(best, distance_to_segment(p, prev, next));
        prev = next;
    }
    return best;
}

[[nodiscard]] color type_color(const style& st, u32 type) noexcept
{
    if (type == 0) { return st.accent; }
    const color c = st.series[type % st.series.size()];
    return c.a == 0 ? st.accent : c;
}

} // namespace

struct node_editor::impl {
    // set by begin(), cleared by end()
    context* ui{};
    rect     bounds;
    bool     hovered{};

    // collected while the nodes are submitted
    std::string                 text;
    std::vector<node_rec>       nodes;
    std::vector<row_rec>        rows;
    std::vector<node_link>      submitted_links;
    bool                        in_node{};

    // rebuilt by end()
    std::vector<pin_rec>              pins;
    std::unordered_map<u64, u32>      pin_index;
    std::vector<link_rec>             links;

    // state between frames
    std::vector<u64> selected;
    node_link        selected_link{};
    bool             focus{};
    bool             dragging_nodes{};
    bool             panning{};
    bool             boxing{};
    vec2             box_origin;
    u64              link_from{}; // pin being dragged from
    vec2             last_mouse;

    // results
    node_link created{};
    node_link deleted{};
    bool      want_delete{};
    u64       hover_node{};
    u64       hover_pin{};
    node_context_request request{};
    bool                 request_made{};

    [[nodiscard]] bool is_selected(u64 id) const noexcept
    {
        return std::find(selected.begin(), selected.end(), id) != selected.end();
    }
    void select(u64 id, bool additive)
    {
        if (!additive) { selected.clear(); }
        if (!is_selected(id)) { selected.push_back(id); }
        selected_link = {};
    }
    [[nodiscard]] std::string_view view(u32 at, u32 len) const noexcept { return std::string_view{text}.substr(at, len); }
    [[nodiscard]] u32 add_text(std::string_view s)
    {
        const auto at = static_cast<u32>(text.size());
        text.append(s);
        return at;
    }
    void reset_frame()
    {
        text.clear();
        nodes.clear();
        rows.clear();
        submitted_links.clear();
        created = deleted = {};
        want_delete = false;
        hover_node = hover_pin = 0;
        request_made = false;
        in_node = false;
    }
};

node_editor::node_editor() : impl_{std::make_unique<impl>()} {}
node_editor::~node_editor()                                  = default;
node_editor::node_editor(node_editor&&) noexcept             = default;
node_editor& node_editor::operator=(node_editor&&) noexcept  = default;

bool node_editor::begin(context& ui, std::string_view label, vec2 size)
{
    impl& s = *impl_;
    s.reset_frame();
    s.ui = nullptr;
    const item_result item = ui.custom_item(label, size);
    if (item.bounds.empty()) { return false; }
    s.ui      = &ui;
    s.bounds  = item.bounds;
    s.hovered = item.hovered;
    return true;
}

void node_editor::begin_node(u64 id, std::string_view title, vec2& pos)
{
    impl& s = *impl_;
    if (s.ui == nullptr) { return; }
    s.nodes.push_back({id, &pos, s.add_text(title), static_cast<u32>(title.size()), static_cast<u32>(s.rows.size()), 0, {}});
    s.in_node = true;
}

void node_editor::input_pin(u64 pin_id, std::string_view label, u32 type)
{
    impl& s = *impl_;
    if (!s.in_node) { return; }
    s.rows.push_back({row_kind::input, pin_id, type, s.add_text(label), static_cast<u32>(label.size())});
    ++s.nodes.back().row_count;
}

void node_editor::output_pin(u64 pin_id, std::string_view label, u32 type)
{
    impl& s = *impl_;
    if (!s.in_node) { return; }
    s.rows.push_back({row_kind::output, pin_id, type, s.add_text(label), static_cast<u32>(label.size())});
    ++s.nodes.back().row_count;
}

void node_editor::node_text(std::string_view text)
{
    impl& s = *impl_;
    if (!s.in_node) { return; }
    s.rows.push_back({row_kind::text, 0, 0, s.add_text(text), static_cast<u32>(text.size())});
    ++s.nodes.back().row_count;
}

void node_editor::end_node() { impl_->in_node = false; }

void node_editor::link(u64 output_pin_id, u64 input_pin_id)
{
    if (impl_->ui != nullptr) { impl_->submitted_links.push_back({output_pin_id, input_pin_id}); }
}

void node_editor::end()
{
    impl& s = *impl_;
    if (s.ui == nullptr) { return; }
    context& ui = *s.ui;
    s.ui = nullptr;
    s.in_node = false;

    const style&      st    = ui.theme();
    const font_atlas& atlas = ui.font();
    const font_id     f     = ui.current_font();
    const f32         lh    = atlas.line_height(f);
    const vec2        mouse   = ui.mouse_pos();
    const bool        inside  = s.hovered && s.bounds.contains(mouse);

    // wheel zoom keeps the canvas point under the pointer where it is
    zoom = std::clamp(zoom, min_zoom, max_zoom);
    if (allow_zoom && inside) {
        if (const f32 notches = ui.take_wheel(); notches != 0.0f) {
            const f32  next   = std::clamp(zoom * std::pow(1.1f, notches), min_zoom, max_zoom);
            const vec2 anchor = (mouse - (s.bounds.min + pan)) * (1.0f / zoom);
            pan += anchor * (zoom - next);
            zoom = next;
        }
    }
    const f32 z       = zoom;
    const f32 title_h = (lh + 10.0f) * z;
    const f32 row_h   = (lh + 6.0f) * z;
    const f32 pad     = node_pad * z;
    const f32 pin_r   = std::max(pin_radius * z, 2.5f);
    const bool labels = z >= 0.6f; // smaller than this the text would be unreadable

    // continuing gestures first, so nodes are laid out and drawn where the drag put them this frame
    if (s.panning) {
        if (ui.mouse_down(mouse_button::middle) || (ui.mouse_down() && ui.alt_down())) { pan += mouse - s.last_mouse; }
        else { s.panning = false; }
    }
    if (s.dragging_nodes) {
        if (ui.mouse_down()) {
            const vec2 delta = mouse - s.last_mouse;
            for (const node_rec& n : s.nodes) {
                if (s.is_selected(n.id)) { *n.pos += delta * (1.0f / z); }
            }
        } else {
            s.dragging_nodes = false;
        }
    }

    // layout: node boxes and pin positions
    const vec2 origin = s.bounds.min + pan;
    s.pins.clear();
    s.pin_index.clear();
    for (u32 ni = 0; ni < s.nodes.size(); ++ni) {
        node_rec& n = s.nodes[ni];
        f32 in_w = 0.0f, out_w = 0.0f, text_w = 0.0f;
        for (u32 r = 0; r < n.row_count; ++r) {
            const row_rec& row = s.rows[n.first_row + r];
            const f32 w = atlas.measure(f, s.view(row.text_at, row.text_len)).x;
            if (row.kind == row_kind::input)       { in_w = std::max(in_w, w + pin_radius + pin_gap); }
            else if (row.kind == row_kind::output) { out_w = std::max(out_w, w + pin_radius + pin_gap); }
            else                                   { text_w = std::max(text_w, w); }
        }
        const f32 title_w = atlas.measure(f, s.view(n.title_at, n.title_len)).x;
        const f32 width = std::max({min_node_w, title_w + 2.0f * node_pad, in_w + out_w + 3.0f * node_pad, text_w + 2.0f * node_pad}) * z;
        const f32 height = title_h + static_cast<f32>(n.row_count) * row_h + pad * 0.5f;
        n.box = rect::from_size(origin + *n.pos * z, {width, height});

        for (u32 r = 0; r < n.row_count; ++r) {
            const row_rec& row = s.rows[n.first_row + r];
            if (row.kind == row_kind::text) { continue; }
            const bool input = row.kind == row_kind::input;
            const vec2 at{input ? n.box.min.x : n.box.max.x, n.box.min.y + title_h + (static_cast<f32>(r) + 0.5f) * row_h};
            s.pin_index[row.pin] = static_cast<u32>(s.pins.size());
            s.pins.push_back({row.pin, at, ni, row.type, input, false});
        }
    }

    s.links.clear();
    for (const node_link& l : s.submitted_links) {
        const auto a = s.pin_index.find(l.from);
        const auto b = s.pin_index.find(l.to);
        if (a == s.pin_index.end() || b == s.pin_index.end()) { continue; }
        s.pins[a->second].linked = s.pins[b->second].linked = true;
        s.links.push_back({l, a->second, b->second, link_curve(s.pins[a->second].pos, s.pins[b->second].pos, z)});
    }

    // what the pointer is over: pins win over nodes, nodes over links
    i32 over_pin = -1, over_node = -1, over_link = -1;
    if (inside) {
        f32 best = pin_hit;
        for (u32 i = 0; i < s.pins.size(); ++i) {
            const vec2 d = mouse - s.pins[i].pos;
            const f32  dist = std::sqrt(dot(d, d));
            if (dist <= best) { best = dist; over_pin = static_cast<i32>(i); }
        }
        for (u32 i = static_cast<u32>(s.nodes.size()); i-- > 0 && over_pin < 0;) {
            if (s.nodes[i].box.contains(mouse)) { over_node = static_cast<i32>(i); break; }
        }
        if (over_pin < 0 && over_node < 0) {
            f32 nearest = link_hit;
            for (u32 i = 0; i < s.links.size(); ++i) {
                const f32 d = distance_to_curve(mouse, s.links[i].curve);
                if (d <= nearest) { nearest = d; over_link = static_cast<i32>(i); }
            }
        }
    }
    if (over_pin >= 0)  { s.hover_pin = s.pins[static_cast<u32>(over_pin)].id; }
    if (over_node >= 0) { s.hover_node = s.nodes[static_cast<u32>(over_node)].id; }
    else if (over_pin >= 0) { s.hover_node = s.nodes[s.pins[static_cast<u32>(over_pin)].node].id; }

    // pressing
    if (ui.mouse_clicked(mouse_button::middle) && inside) { s.panning = true; s.focus = true; }
    if (inside && ui.mouse_clicked(mouse_button::right)) {
        s.focus = true;
        node_context_request req;
        req.screen = mouse;
        req.canvas = (mouse - origin) * (1.0f / z);
        if (over_pin >= 0 || over_node >= 0) {
            const u32 ni = over_node >= 0 ? static_cast<u32>(over_node) : s.pins[static_cast<u32>(over_pin)].node;
            req.target = node_target::node;
            req.node   = s.nodes[ni].id;
            if (!s.is_selected(req.node)) { s.select(req.node, false); }
        } else if (over_link >= 0) {
            req.target      = node_target::link;
            req.link        = s.links[static_cast<u32>(over_link)].ids;
            s.selected.clear();
            s.selected_link = req.link;
        } else {
            req.target = node_target::canvas;
        }
        s.request      = req;
        s.request_made = true;
    }
    if (ui.mouse_clicked()) {
        if (!inside) {
            s.focus = false;
        } else if (ui.alt_down()) {
            s.panning = true;
            s.focus   = true;
        } else {
            s.focus = true;
            const bool additive = ui.ctrl_down();
            if (over_pin >= 0) {
                s.link_from = s.pins[static_cast<u32>(over_pin)].id;
            } else if (over_node >= 0) {
                const u64 id = s.nodes[static_cast<u32>(over_node)].id;
                if (additive && s.is_selected(id)) {
                    s.selected.erase(std::find(s.selected.begin(), s.selected.end(), id));
                } else {
                    if (!s.is_selected(id)) { s.select(id, additive); }
                    s.selected_link  = {};
                    s.dragging_nodes = true;
                }
            } else if (over_link >= 0) {
                s.selected.clear();
                s.selected_link = s.links[static_cast<u32>(over_link)].ids;
            } else {
                if (!additive) { s.selected.clear(); }
                s.selected_link = {};
                s.boxing        = true;
                s.box_origin    = mouse;
            }
        }
    }

    // finishing a link or a box
    if (s.link_from != 0) {
        if (ui.key_pressed(key::escape) || !s.pin_index.contains(s.link_from)) {
            s.link_from = 0;
        } else if (!ui.mouse_down()) {
            const pin_rec& from = s.pins[s.pin_index[s.link_from]];
            if (over_pin >= 0) {
                const pin_rec& to = s.pins[static_cast<u32>(over_pin)];
                const bool ok = to.input != from.input && to.node != from.node && to.type == from.type;
                const node_link made = from.input ? node_link{to.id, from.id} : node_link{from.id, to.id};
                if (ok && std::find(s.submitted_links.begin(), s.submitted_links.end(), made) == s.submitted_links.end()) {
                    s.created = made;
                }
            }
            s.link_from = 0;
        }
    }
    rect box_rect;
    if (s.boxing) {
        box_rect = {{std::min(s.box_origin.x, mouse.x), std::min(s.box_origin.y, mouse.y)},
                    {std::max(s.box_origin.x, mouse.x), std::max(s.box_origin.y, mouse.y)}};
        if (!ui.mouse_down()) {
            for (const node_rec& n : s.nodes) {
                if (n.box.overlaps(box_rect) && !s.is_selected(n.id)) { s.selected.push_back(n.id); }
            }
            s.boxing = false;
        }
    }

    // ids that were not submitted this frame are gone
    std::erase_if(s.selected, [&](u64 id) {
        return std::none_of(s.nodes.begin(), s.nodes.end(), [&](const node_rec& n) { return n.id == id; });
    });
    if (s.selected_link != node_link{} &&
        std::none_of(s.links.begin(), s.links.end(), [&](const link_rec& l) { return l.ids == s.selected_link; })) {
        s.selected_link = {};
    }

    if (s.focus && ui.key_pressed(key::del)) {
        if (s.selected_link != node_link{}) { s.deleted = s.selected_link; s.selected_link = {}; }
        else if (!s.selected.empty())       { s.want_delete = true; }
    }

    // drawing
    draw_list& dl = ui.draw();
    dl.push_clip(s.bounds);
    dl.rect_filled(s.bounds, color{st.window_bg.r, st.window_bg.g, st.window_bg.b, 255}, st.rounding * 0.5f);

    const f32 gs = grid_spacing * z;
    if (show_grid && gs >= 6.0f) {
        const color grid = st.border.scaled_alpha(0.45f);
        const f32 gx = s.bounds.min.x + std::fmod(std::fmod(pan.x, gs) + gs, gs);
        const f32 gy = s.bounds.min.y + std::fmod(std::fmod(pan.y, gs) + gs, gs);
        for (f32 x = gx; x < s.bounds.max.x; x += gs) { dl.line({x, s.bounds.min.y}, {x, s.bounds.max.y}, grid, 1.0f); }
        for (f32 y = gy; y < s.bounds.max.y; y += gs) { dl.line({s.bounds.min.x, y}, {s.bounds.max.x, y}, grid, 1.0f); }
    }

    for (u32 i = 0; i < s.links.size(); ++i) {
        const link_rec& l = s.links[i];
        const bool chosen = l.ids == s.selected_link;
        const color c = chosen ? st.text : type_color(st, s.pins[l.from].type);
        dl.bezier_cubic(l.curve[0], l.curve[1], l.curve[2], l.curve[3],
                        static_cast<i32>(i) == over_link && !chosen ? c.scaled_alpha(0.75f) : c, (chosen ? 3.5f : 2.5f) * std::max(z, 0.6f));
    }

    const rect view = s.bounds.expanded(8.0f);
    for (u32 ni = 0; ni < s.nodes.size(); ++ni) {
        const node_rec& n = s.nodes[ni];
        if (!n.box.overlaps(view)) { continue; }
        const bool chosen = s.is_selected(n.id);
        const f32  round  = st.rounding * 0.6f;

        shape_style body;
        body.radius        = radii(round);
        body.fill_top      = st.widget_bg;
        body.fill_bottom   = st.widget_bg;
        body.border        = chosen ? st.accent : st.widget_border;
        body.border_width  = chosen ? 2.0f : st.border_width;
        body.shadow        = st.shadow;
        body.shadow_blur   = st.shadow_blur * 0.5f;
        body.shadow_offset = {0.0f, 3.0f};
        dl.shape(n.box, body);

        const rect head{n.box.min, {n.box.max.x, n.box.min.y + title_h}};
        dl.rect_filled(head, st.title_bg, round, corners::top);
        const auto put = [&](vec2 at, color c, std::string_view t) {
            if (!labels) { return; }
            dl.push_clip(n.box);
            dl.text(at, c, t, f);
            dl.pop_clip();
        };
        put({n.box.min.x + pad, n.box.min.y + (title_h - lh) * 0.5f}, st.text, s.view(n.title_at, n.title_len));

        for (u32 r = 0; r < n.row_count; ++r) {
            const row_rec& row = s.rows[n.first_row + r];
            const std::string_view label = s.view(row.text_at, row.text_len);
            const f32 y = n.box.min.y + title_h + static_cast<f32>(r) * row_h + (row_h - lh) * 0.5f;
            if (row.kind == row_kind::text) {
                put({n.box.min.x + pad, y}, st.text_dim, label);
                continue;
            }
            const bool input = row.kind == row_kind::input;
            const pin_rec& pin = s.pins[s.pin_index[row.pin]];
            const bool live = s.link_from == pin.id || pin.id == s.hover_pin;
            const color c = type_color(st, pin.type);
            if (pin.linked || live) { dl.circle_filled(pin.pos, pin_r, live ? lerp(c, st.text, 0.4f) : c); }
            else                    { dl.circle_filled(pin.pos, pin_r, st.widget_bg); dl.circle(pin.pos, pin_r, c, 1.5f); }
            const f32 w = atlas.measure(f, label).x;
            const f32 x = input ? n.box.min.x + pin_r + pin_gap * z : n.box.max.x - pin_r - pin_gap * z - w;
            put({x, y}, st.text, label);
        }
    }

    if (s.link_from != 0) {
        const pin_rec& from = s.pins[s.pin_index[s.link_from]];
        vec2 end_at = mouse;
        if (over_pin >= 0) {
            const pin_rec& to = s.pins[static_cast<u32>(over_pin)];
            if (to.input != from.input && to.node != from.node && to.type == from.type) { end_at = to.pos; }
        }
        const auto curve = from.input ? link_curve(end_at, from.pos, z) : link_curve(from.pos, end_at, z);
        dl.bezier_cubic(curve[0], curve[1], curve[2], curve[3], type_color(st, from.type).scaled_alpha(0.85f), 2.5f);
    }
    if (s.boxing) {
        dl.rect_filled(box_rect, st.accent.scaled_alpha(0.16f));
        dl.rect_outline(box_rect, st.accent.scaled_alpha(0.7f));
    }
    dl.pop_clip();
    dl.rect_outline(s.bounds, st.border, st.rounding * 0.5f, st.border_width);

    s.last_mouse = mouse;
}

bool node_editor::link_created(node_link& out) const noexcept
{
    out = impl_->created;
    return impl_->created != node_link{};
}

bool node_editor::link_deleted(node_link& out) const noexcept
{
    out = impl_->deleted;
    return impl_->deleted != node_link{};
}

bool node_editor::delete_requested() const noexcept { return impl_->want_delete; }

bool node_editor::context_requested(node_context_request& out) const noexcept
{
    out = impl_->request;
    return impl_->request_made;
}

u64  node_editor::hovered_node() const noexcept { return impl_->hover_node; }
u64  node_editor::hovered_pin() const noexcept { return impl_->hover_pin; }

rect node_editor::canvas_rect() const noexcept { return impl_->bounds; }

rect node_editor::node_rect(u64 id) const noexcept
{
    for (const node_rec& n : impl_->nodes) {
        if (n.id == id) { return n.box; }
    }
    return {};
}

bool node_editor::pin_position(u64 pin_id, vec2& out) const noexcept
{
    const auto it = impl_->pin_index.find(pin_id);
    if (it == impl_->pin_index.end()) { return false; }
    out = impl_->pins[it->second].pos;
    return true;
}

std::span<const u64> node_editor::selected_nodes() const noexcept { return impl_->selected; }
bool node_editor::node_selected(u64 id) const noexcept { return impl_->is_selected(id); }
node_link node_editor::selected_link() const noexcept { return impl_->selected_link; }
void node_editor::select_node(u64 id, bool additive) { impl_->select(id, additive); }

void node_editor::clear_selection() noexcept
{
    impl_->selected.clear();
    impl_->selected_link = {};
}

} // namespace strata

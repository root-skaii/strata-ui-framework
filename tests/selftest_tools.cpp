// the node editor (drag, link, select, delete, pan) and the widget inspector

#include "selftest_common.hpp"

#include <cstring>

namespace {

struct graph_fixture {
    harness        h;
    node_editor    ed;
    vec2           pos_a{40.0f, 40.0f};
    vec2           pos_b{300.0f, 60.0f};
    std::vector<node_link> links;
    node_link      made;
    bool           made_flag{};
    bool           delete_flag{};
    node_context_request req;
    bool           req_flag{};

    void build()
    {
        if (auto w = h.ui.window("graph", {0.0f, 0.0f}, {700.0f, 500.0f})) {
            if (ed.begin(h.ui, "canvas", {0.0f, 400.0f})) {
                ed.begin_node(1, "A", pos_a);
                ed.input_pin(11, "in", 0);
                ed.output_pin(12, "out", 0);
                ed.output_pin(13, "typed", 7);
                ed.end_node();
                ed.begin_node(2, "B", pos_b);
                ed.input_pin(21, "in", 0);
                ed.input_pin(22, "typed", 7);
                ed.end_node();
                for (const node_link& l : links) { ed.link(l.from, l.to); }
                ed.end();
            }
            made_flag   = ed.link_created(made);
            delete_flag = ed.delete_requested();
            req_flag    = ed.context_requested(req);
            if (made_flag) { links.push_back(made); }
        }
    }
    void frame() { h.frame([&] { build(); }); }
    void settle() { frame(); frame(); }

    [[nodiscard]] vec2 pin(u64 id) const
    {
        vec2 p;
        CHECK(ed.pin_position(id, p));
        return p;
    }
    [[nodiscard]] vec2 title_of(u64 node) const
    {
        const rect r = ed.node_rect(node);
        return {r.center().x, r.min.y + 8.0f};
    }
    void drag(vec2 from, vec2 to)
    {
        h.move(from);
        settle();
        h.down();
        frame();
        h.move(to);
        frame();
        h.up();
        frame();
        frame();
    }
};

void test_node_drag_and_select()
{
    std::fprintf(stderr, "[node editor: drag, select, delete]\n");
    graph_fixture g;
    g.settle();
    CHECK(g.ed.node_rect(1).width() > 100.0f);
    CHECK(g.ed.node_rect(2).min.x > g.ed.node_rect(1).max.x);

    const vec2 before = g.pos_a;
    const vec2 grab   = g.title_of(1);
    g.drag(grab, grab + vec2{60.0f, 25.0f});
    CHECK(near_eq(g.pos_a.x, before.x + 60.0f, 1.0f) && near_eq(g.pos_a.y, before.y + 25.0f, 1.0f));
    CHECK(g.pos_b == vec2(300.0f, 60.0f));
    CHECK(g.ed.node_selected(1) && !g.ed.node_selected(2));

    // Delete with a focused selection reports it; the application removes the nodes
    g.h.key(key::del);
    g.frame();
    CHECK(g.delete_flag);

    // a click on empty canvas clears the selection
    const rect canvas = g.ed.canvas_rect();
    g.h.move({canvas.min.x + 20.0f, canvas.max.y - 20.0f});
    g.settle();
    g.h.down();
    g.frame();
    g.h.up();
    g.settle();
    CHECK(g.ed.selected_nodes().empty());
}

void test_node_box_select_and_pan()
{
    std::fprintf(stderr, "[node editor: box select, pan]\n");
    graph_fixture g;
    g.settle();

    // a box over both nodes, started on empty canvas
    const rect canvas = g.ed.canvas_rect();
    const vec2 from{canvas.min.x + 2.0f, canvas.max.y - 4.0f};
    const vec2 to{canvas.max.x - 4.0f, canvas.min.y + 4.0f};
    g.drag(from, to);
    CHECK(g.ed.node_selected(1) && g.ed.node_selected(2));

    // middle-drag moves the whole canvas
    const rect a0 = g.ed.node_rect(1);
    g.h.move({canvas.min.x + 200.0f, canvas.max.y - 30.0f});
    g.settle();
    g.h.down(2);
    g.frame();
    g.h.move({canvas.min.x + 230.0f, canvas.max.y - 10.0f});
    g.frame();
    g.h.up(2);
    g.settle();
    CHECK(near_eq(g.ed.pan.x, 30.0f, 1.0f) && near_eq(g.ed.pan.y, 20.0f, 1.0f));
    CHECK(near_eq(g.ed.node_rect(1).min.x - a0.min.x, 30.0f, 1.0f));
    CHECK(g.pos_a == vec2(40.0f, 40.0f)); // positions are canvas coordinates: only the view moved
}

void test_node_links()
{
    std::fprintf(stderr, "[node editor: linking pins]\n");
    graph_fixture g;
    g.settle();

    // output -> input of the same type
    g.drag(g.pin(12), g.pin(21));
    CHECK(g.links.size() == 1);
    CHECK(g.links[0] == node_link(12, 21));

    // the same link again is not reported twice
    g.drag(g.pin(12), g.pin(21));
    CHECK(g.links.size() == 1);

    // mismatched pin types, same-node pins and output -> output are refused
    g.drag(g.pin(12), g.pin(22));
    g.drag(g.pin(12), g.pin(11));
    g.drag(g.pin(13), g.pin(12));
    CHECK(g.links.size() == 1);

    // matching typed pins link, and dragging from the input side gives the same (output, input) order
    g.drag(g.pin(22), g.pin(13));
    CHECK(g.links.size() == 2);
    CHECK(g.links[1] == node_link(13, 22));

    // a link can be selected by clicking on it, then reported as deleted
    vec2 a = g.pin(12), b = g.pin(21);
    const vec2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; // the curve passes through its midpoint
    g.h.move(mid);
    g.settle();
    g.h.down();
    g.frame();
    g.h.up();
    g.frame();
    CHECK(g.ed.selected_link() == node_link(12, 21));
    g.h.key(key::del);
    node_link gone;
    bool reported = false;
    g.h.frame([&] {
        g.build();
        reported = reported || g.ed.link_deleted(gone);
    });
    CHECK(reported && gone == node_link(12, 21));
}

void test_inspector()
{
    std::fprintf(stderr, "[inspector: records widgets while open]\n");
    harness h;
    bool open = true;
    int  clicks = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("app", {20.0f, 20.0f}, {300.0f, 200.0f})) {
            if (h.ui.button("alpha")) { ++clicks; }
            h.ui.button("beta");
        }
        h.ui.debug_inspector_window(open);
    };
    h.frames(build, 4);
    CHECK(clicks == 0);

    const std::size_t shown = h.ui.render_data().vertices.size();

    // closed: nothing is drawn for it, and the recording stops
    open = false;
    h.frames(build, 3);
    const std::size_t hidden = h.ui.render_data().vertices.size();
    CHECK(hidden < shown);

    // reopened: its item list is back after the frame that records
    open = true;
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() == shown);
}


void test_node_zoom_and_context()
{
    std::fprintf(stderr, "[node editor: zoom, context requests]\n");
    graph_fixture g;
    g.settle();
    const rect canvas = g.ed.canvas_rect();
    const f32  width0 = g.ed.node_rect(1).width();

    // the wheel zooms about the pointer: the canvas point under it stays under it
    const vec2 at = g.title_of(1);
    const vec2 canvas_before = (at - (canvas.min + g.ed.pan)) * (1.0f / g.ed.zoom);
    g.h.move(at);
    g.settle();
    g.h.wheel(2.0f);
    g.frame();
    CHECK(g.ed.zoom > 1.2f);
    CHECK(near_eq(g.ed.node_rect(1).width(), width0 * g.ed.zoom, 1.0f));
    const vec2 canvas_after = (at - (canvas.min + g.ed.pan)) * (1.0f / g.ed.zoom);
    CHECK(near_eq(canvas_after.x, canvas_before.x, 0.5f) && near_eq(canvas_after.y, canvas_before.y, 0.5f));
    CHECK(g.pos_a == vec2(40.0f, 40.0f)); // node positions stay in canvas units

    g.h.wheel(-40.0f);
    g.frame();
    g.frame();
    CHECK(near_eq(g.ed.zoom, g.ed.min_zoom, 0.001f)); // clamped

    g.ed.allow_zoom = false;
    const f32 locked = g.ed.zoom;
    g.h.wheel(3.0f);
    g.frame();
    CHECK(g.ed.zoom == locked);
    g.ed.allow_zoom = true;
    g.ed.zoom = 1.0f;
    g.ed.pan  = {};
    g.settle();

    // right-click on a node: the request names it and selects it
    g.h.move(g.title_of(2));
    g.settle();
    g.h.down(1);
    g.frame();
    CHECK(g.req_flag && g.req.target == node_target::node && g.req.node == 2);
    CHECK(g.ed.node_selected(2));
    g.h.up(1);
    g.settle();
    CHECK(!g.req_flag); // one frame only

    // on empty canvas it carries the canvas coordinates a new node would get
    const rect c = g.ed.canvas_rect();
    const vec2 empty{c.min.x + 30.0f, c.max.y - 30.0f};
    g.h.move(empty);
    g.settle();
    g.h.down(1);
    g.frame();
    CHECK(g.req_flag && g.req.target == node_target::canvas);
    CHECK(near_eq(g.req.canvas.x, empty.x - c.min.x, 0.5f) && near_eq(g.req.canvas.y, empty.y - c.min.y, 0.5f));
    g.h.up(1);
    g.settle();
}

void test_input_record()
{
    std::fprintf(stderr, "[input record / replay]\n");
    input_recorder rec;
    CHECK(rec.frames() == 0);

    input_state a;
    a.mouse_pos    = {12.5f, 40.0f};
    a.mouse_down   = {true, false, true};
    a.wheel        = -2.0f;
    a.display_size = {800.0f, 600.0f};
    a.ctrl         = true;
    a.press(key::a, true, false, false);
    a.press(key::enter);
    a.typed_len = 3;
    std::memcpy(a.typed.data(), "h\xc3\xa9", 3);
    a.set_held(key::a, true);
    rec.record(a);
    input_state b = a;
    b.key_count = 0;
    b.typed_len = 0;
    b.mouse_pos = {99.0f, 1.0f};
    rec.record(b);
    CHECK(rec.frames() == 2);

    input_replayer play;
    CHECK(play.load(rec.data()));
    CHECK(play.frames() == 2);
    input_state out;
    CHECK(play.next(out));
    CHECK(out.mouse_pos == a.mouse_pos && out.mouse_down == a.mouse_down && out.wheel == a.wheel);
    CHECK(out.display_size == a.display_size && out.ctrl && !out.shift);
    CHECK(out.key_count == 2 && out.keys[0].k == key::a && out.keys[0].ctrl && out.keys[1].k == key::enter);
    CHECK(out.typed_len == 3 && std::memcmp(out.typed.data(), "h\xc3\xa9", 3) == 0);
    CHECK(out.held(key::a));
    CHECK(play.next(out) && out.mouse_pos == vec2(99.0f, 1.0f) && out.key_count == 0);
    CHECK(play.finished());
    CHECK(!play.next(out) && out.mouse_pos == vec2(99.0f, 1.0f)); // the final frame repeats, one-shots cleared
    play.rewind();
    CHECK(play.next(out) && out.mouse_pos == a.mouse_pos);

    // damaged input is refused whole
    std::vector<u8> bytes(rec.data().begin(), rec.data().end());
    CHECK(!play.load(std::span<const u8>{bytes.data(), bytes.size() - 1}));
    CHECK(!play.load(std::span<const u8>{bytes.data(), 5}));
    bytes[0] = 'X';
    CHECK(!play.load(bytes));
    CHECK(!play.load({}));
    CHECK(play.frames() == 2); // the earlier good load is still there

    // the same recording drives a ui to the same state: a click on a button, then a typed field
    harness live;
    int live_clicks = 0;
    std::string live_text;
    const auto script = [&](harness& h, int& clicks, std::string& text) {
        return [&] {
            if (auto w = h.ui.window("w", {10.0f, 10.0f}, {300.0f, 200.0f})) {
                if (h.ui.button("go")) { ++clicks; }
                (void)h.ui.input_text("field", text);
            }
        };
    };
    input_recorder session;
    const auto step = [&](harness& h, auto&& build) {
        h.frame(build);
    };
    const auto build_live = script(live, live_clicks, live_text);
    live.move({40.0f, 40.0f});
    for (int i = 0; i < 4; ++i) { step(live, build_live); session.record(live.in); }
    live.down();
    step(live, build_live);
    session.record(live.in);
    live.up();
    for (int i = 0; i < 3; ++i) { step(live, build_live); session.record(live.in); }

    harness again;
    int again_clicks = 0;
    std::string again_text;
    const auto build_again = script(again, again_clicks, again_text);
    input_replayer rp;
    CHECK(rp.load(session.data()));
    input_state fed;
    while (rp.next(fed)) {
        again.in = fed;
        again.frame(build_again);
    }
    CHECK(live_clicks == again_clicks);
    CHECK(live.ui.render_data().vertices.size() == again.ui.render_data().vertices.size());
}

void test_table_grip_cursor()
{
    std::fprintf(stderr, "[table: the column grip shows the resize cursor]\n");
    harness h;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {10.0f, 10.0f}, {320.0f, 200.0f})) {
            if (h.ui.begin_table("cols", 3, table_flags::resizable | table_flags::borders)) {
                h.ui.table_setup_column("a", 100.0f);
                h.ui.table_setup_column("b", 100.0f);
                h.ui.table_setup_column("c");
                h.ui.table_headers_row();
                h.ui.end_table();
            }
        }
    };
    bool found = false;
    for (f32 x = 30.0f; x < 300.0f && !found; x += 2.0f) {
        for (f32 y = 36.0f; y < 80.0f && !found; y += 6.0f) {
            h.move({x, y});
            h.frames(build, 2);
            found = h.ui.cursor() == cursor_kind::resize_ew;
        }
    }
    CHECK(found);
    h.move({-500.0f, -500.0f});
    h.frames(build, 2);
    CHECK(h.ui.cursor() != cursor_kind::resize_ew);
}

} // namespace

void run_tool_tests()
{
    test_node_drag_and_select();
    test_node_box_select_and_pan();
    test_node_links();
    test_inspector();
    test_node_zoom_and_context();
    test_input_record();
    test_table_grip_cursor();
}

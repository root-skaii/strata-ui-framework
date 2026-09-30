// headless self-test: docking: tabs, splits, spaces, layouts, panes, animation

#include "selftest_common.hpp"

namespace {

void test_docking()
{
    std::fprintf(stderr, "[docking]\n");
    harness h;
    bool show_b = true;
    const auto build = [&] {
        h.ui.dock_area({{0, 0}, {800, 600}});
        constexpr window_flags flags = window_flags::dockable | window_flags::resizable;
        if (auto w = h.ui.window("A", {100, 100}, {200, 150}, flags)) { h.ui.text("window a"); }
        if (show_b) {
            if (auto w = h.ui.window("B", {400, 100}, {200, 150}, flags)) { h.ui.text("window b"); }
        }
        if (auto w = h.ui.window("C", {500, 300}, {200, 150}, flags)) { h.ui.text("window c"); }
    };
    h.frames(build, 3);
    const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
    CHECK(!h.ui.window_docked("A"));

    // programmatic docking: A fills the area, B splits it on the right
    CHECK(h.ui.dock_window("A", dock_zone::center));
    h.frames(build, 2);
    CHECK(h.ui.window_docked("A"));
    rect a = h.ui.window_rect("A");
    CHECK(near_eq(a.min.x, 0) && near_eq(a.max.x, 800) && near_eq(a.min.y, tab_h) && near_eq(a.max.y, 600));

    CHECK(h.ui.dock_window("B", dock_zone::right, "A"));
    h.frames(build, 2);
    a = h.ui.window_rect("A");
    rect b = h.ui.window_rect("B");
    CHECK(a.max.x < b.min.x);
    CHECK(near_eq(a.width(), b.width(), 6.0f));
    CHECK(near_eq(a.min.y, tab_h) && near_eq(b.min.y, tab_h));
    CHECK(near_eq(b.max.x, 800));

    // a window can only be docked next to another one that is docked itself
    CHECK(!h.ui.dock_window("C", dock_zone::left, "not a window"));

    // C joins A's tabs: both keep the node's rectangle, C is the selected one
    CHECK(h.ui.dock_window("C", dock_zone::center, "A"));
    h.frames(build, 2);
    rect c = h.ui.window_rect("C");
    a = h.ui.window_rect("A");
    CHECK(near_eq(c.min.x, a.min.x) && near_eq(c.max.x, a.max.x));

    // the pointer over a tab belongs to the ui
    const f32 wa = h.ui.font().measure(0, "A").x + 26.0f;
    const f32 wc = h.ui.font().measure(0, "C").x + 26.0f;
    const vec2 tab_c{wa + wc * 0.5f, tab_h * 0.5f};
    h.move({700, 580});
    h.frames(build, 2);
    h.move(tab_c);
    h.frames(build, 2);
    CHECK(h.ui.want_capture_mouse());

    // drag C's tab out: it becomes a floating window that follows the pointer
    h.down();
    h.frame(build);
    CHECK(h.ui.window_docked("C"));
    h.move({tab_c.x + 30.0f, tab_c.y + 80.0f});
    h.frame(build);
    h.frame(build);
    CHECK(!h.ui.window_docked("C"));
    h.move({tab_c.x + 60.0f, tab_c.y + 120.0f});
    h.frames(build, 2);
    c = h.ui.window_rect("C");
    CHECK(c.min.y > 60.0f); // it went along with the pointer

    // ... and dropping it on the left part of A's area docks it there, splitting that area in two
    h.move({40.0f, 300.0f});
    h.frames(build, 3);
    h.up();
    h.frames(build, 3);
    CHECK(h.ui.window_docked("C"));
    c = h.ui.window_rect("C");
    a = h.ui.window_rect("A");
    b = h.ui.window_rect("B");
    CHECK(near_eq(c.min.x, 0.0f));
    CHECK(c.max.x < a.min.x && a.max.x < b.min.x);
    CHECK(near_eq(c.width(), a.width(), 8.0f));

    // the splitter between C and A moves with the pointer
    const f32 old_c_width = c.width();
    const vec2 grab{(c.max.x + a.min.x) * 0.5f, 300.0f};
    h.move(grab);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({grab.x + 50.0f, grab.y});
    h.frames(build, 3);
    h.up();
    h.frames(build, 2);
    c = h.ui.window_rect("C");
    a = h.ui.window_rect("A");
    CHECK(near_eq(c.width(), old_c_width + 50.0f, 8.0f));
    CHECK(a.min.x > c.max.x);

    // a window that stops being submitted leaves the dock and its space goes to the others
    show_b = false;
    h.frames(build, 3);
    CHECK(!h.ui.window_docked("B"));
    a = h.ui.window_rect("A");
    CHECK(near_eq(a.max.x, 800.0f));
    show_b = true;
    h.frames(build, 2);
    CHECK(!h.ui.window_docked("B")); // it comes back floating

    h.ui.undock_window("A");
    CHECK(!h.ui.window_docked("A"));
}

void test_dock_spaces()
{
    std::fprintf(stderr, "[dock spaces: edge docks, floating docks]\n");
    {
        harness h;
        const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
        rect main_rect{};
        const auto build = [&] {
            rect client{{0, 0}, {800, 600}};
            client = h.ui.dock_edge("left", dock_side::left, 250.0f, client);
            client = h.ui.dock_edge("bottom", dock_side::bottom, 150.0f, client);
            h.ui.dock_area(client);
            main_rect = client;
            (void)h.ui.floating_dock("Tools", {500, 100}, {260, 300});
            constexpr window_flags f = window_flags::dockable | window_flags::resizable;
            for (const char* name : {"A", "B", "C", "D"}) {
                if (auto w = h.ui.window(name, {560, 440}, {200, 150}, f)) { h.ui.text(name); }
            }
        };
        h.frames(build, 3);
        // empty edge docks take no room
        CHECK(near_eq(main_rect.min.x, 0.0f) && near_eq(main_rect.max.x, 800.0f) && near_eq(main_rect.max.y, 600.0f));

        // docking into the left edge dock shrinks the main area by its width plus the handle
        CHECK(h.ui.dock_window("A", dock_zone::center, {}, 0.5f, "left"));
        h.frames(build, 3);
        CHECK(near_eq(main_rect.min.x, 254.0f, 1.0f));
        rect a = h.ui.window_rect("A");
        CHECK(near_eq(a.min.x, 0.0f) && near_eq(a.max.x, 250.0f) && near_eq(a.min.y, tab_h) && near_eq(a.max.y, 600.0f));

        // and the bottom one takes its share of what is left
        CHECK(h.ui.dock_window("B", dock_zone::center, {}, 0.5f, "bottom"));
        h.frames(build, 3);
        CHECK(near_eq(main_rect.max.y, 446.0f, 1.0f));
        rect b = h.ui.window_rect("B");
        CHECK(near_eq(b.min.x, 254.0f, 1.0f) && near_eq(b.max.x, 800.0f) && near_eq(b.max.y, 600.0f));

        // the main area fills the rest
        CHECK(h.ui.dock_window("C", dock_zone::center));
        h.frames(build, 3);
        rect c = h.ui.window_rect("C");
        CHECK(near_eq(c.min.x, 254.0f, 1.0f) && near_eq(c.max.x, 800.0f) && near_eq(c.max.y, 446.0f));
        CHECK(h.ui.window_docked("A") && h.ui.window_docked("B") && h.ui.window_docked("C"));

        // the handle of an edge dock resizes it
        const vec2 grab{252.0f, 200.0f};
        h.move(grab);
        h.frames(build, 3);
        h.down();
        h.frame(build);
        h.move({grab.x + 100.0f, grab.y});
        h.frames(build, 3);
        h.up();
        h.frames(build, 3);
        a = h.ui.window_rect("A");
        CHECK(near_eq(a.max.x, 350.0f, 2.0f));
        CHECK(near_eq(main_rect.min.x, 354.0f, 2.0f));

        // a floating dock is a space of its own: windows docked in it fill its body ...
        CHECK(h.ui.dock_window("D", dock_zone::center, {}, 0.5f, "Tools"));
        h.frames(build, 3);
        const rect tools = h.ui.window_rect("Tools");
        rect d = h.ui.window_rect("D");
        CHECK(near_eq(d.min.x, tools.min.x, 1.0f) && near_eq(d.max.x, tools.max.x, 1.0f));
        CHECK(near_eq(d.min.y, tools.min.y + tab_h + tab_h, 1.5f) && near_eq(d.max.y, tools.max.y, 1.5f));

        // ... and move with it when the dock is dragged by its title bar
        const vec2 title{tools.min.x + 120.0f, tools.min.y + tab_h * 0.5f};
        h.move(title);
        h.frames(build, 3);
        h.down();
        h.frame(build);
        h.move({title.x + 80.0f, title.y + 40.0f});
        h.frames(build, 3);
        h.up();
        h.frames(build, 3);
        const rect moved = h.ui.window_rect("Tools");
        const rect d2 = h.ui.window_rect("D");
        CHECK(near_eq(moved.min.x, tools.min.x + 80.0f, 2.0f) && near_eq(moved.min.y, tools.min.y + 40.0f, 2.0f));
        CHECK(near_eq(d2.min.x, d.min.x + 80.0f, 2.0f) && near_eq(d2.min.y, d.min.y + 40.0f, 2.0f));
        CHECK(h.ui.window_docked("D"));

        // a window no longer submitted leaves its space; an emptied edge dock gives its room back
        h.ui.undock_window("A");
        h.frames(build, 4);
        CHECK(!h.ui.window_docked("A"));
        CHECK(near_eq(main_rect.min.x, 0.0f, 1.0f));
    }
    {
        // dragging a window to the side of the app docks it into the (empty) edge dock there
        harness h;
        rect main_rect{};
        const auto build = [&] {
            rect client{{0, 0}, {800, 600}};
            client = h.ui.dock_edge("left", dock_side::left, 250.0f, client);
            h.ui.dock_area(client);
            main_rect = client;
            if (auto w = h.ui.window("E", {300, 300}, {200, 150}, window_flags::dockable | window_flags::resizable)) { h.ui.text("e"); }
        };
        const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
        h.frames(build, 3);
        const vec2 title{300.0f + 60.0f, 300.0f + tab_h * 0.5f};
        h.move(title);
        h.frames(build, 3);
        h.down();
        h.frame(build);
        h.move({200.0f, 300.0f});
        h.frames(build, 2);
        h.move({40.0f, 300.0f});
        h.frames(build, 2);
        h.move({8.0f, 300.0f}); // the drop strip along the edge
        h.frames(build, 3);
        h.up();
        h.frames(build, 3);
        CHECK(h.ui.window_docked("E"));
        const rect e = h.ui.window_rect("E");
        CHECK(near_eq(e.min.x, 0.0f) && near_eq(e.max.x, 250.0f));
        CHECK(near_eq(main_rect.min.x, 254.0f, 1.0f));
    }
}

// text styles: bold / italic / underline / strike-through --------------------------------------------------------

void test_dock_layout()
{
    std::fprintf(stderr, "[dock layout save / restore]\n");
    constexpr window_flags flags = window_flags::dockable | window_flags::resizable;
    const auto build_for = [&](harness& h) {
        return [&h, flags] {
            rect client{{0, 0}, {800, 600}};
            client = h.ui.dock_edge("side", dock_side::left, 200.0f, client);
            h.ui.dock_area(client);
            (void)h.ui.floating_dock("Tools", {500, 60}, {240, 260});
            for (const char* name : {"A", "B", "C", "D", "E"}) {
                if (auto w = h.ui.window(name, {560, 400}, {200, 120}, flags)) { h.ui.text(name); }
            }
        };
    };

    harness h;
    const auto build = build_for(h);
    h.frames(build, 3);
    CHECK(h.ui.dock_window("A", dock_zone::center));
    CHECK(h.ui.dock_window("B", dock_zone::right, "A", 0.4f));
    CHECK(h.ui.dock_window("C", dock_zone::center, "B"));
    CHECK(h.ui.dock_window("D", dock_zone::center, {}, 0.5f, "side"));
    CHECK(h.ui.dock_window("E", dock_zone::center, {}, 0.5f, "Tools"));
    h.frames(build, 4);
    const std::array<const char*, 5> names = {"A", "B", "C", "D", "E"};
    std::array<rect, 5> before{};
    for (std::size_t i = 0; i < names.size(); ++i) { before[i] = h.ui.window_rect(names[i]); }
    const std::string text = h.ui.dock_save_layout();
    CHECK(text.starts_with("strata-dock 2"));
    CHECK(text.find("space ") != std::string::npos && text.find("S h ") != std::string::npos && text.find("W ") != std::string::npos);

    // take it apart ...
    for (const char* n : names) { h.ui.undock_window(n); }
    h.frames(build, 4);
    for (const char* n : names) { CHECK(!h.ui.window_docked(n)); }

    // ... and get it back
    CHECK(h.ui.dock_load_layout(text));
    h.frames(build, 4);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(h.ui.window_docked(names[i]));
        const rect r = h.ui.window_rect(names[i]);
        CHECK(near_eq(r.min.x, before[i].min.x, 1.0f) && near_eq(r.max.x, before[i].max.x, 1.0f));
        CHECK(near_eq(r.min.y, before[i].min.y, 1.0f) && near_eq(r.max.y, before[i].max.y, 1.0f));
    }
    CHECK(h.ui.dock_save_layout() == text); // saving what was loaded gives the same text

    // a new context that has not shown anything yet takes it as well
    {
        harness g;
        const auto build_g = build_for(g);
        CHECK(g.ui.dock_load_layout(text));
        g.frames(build_g, 4);
        for (std::size_t i = 0; i < names.size(); ++i) {
            CHECK(g.ui.window_docked(names[i]));
            const rect r = g.ui.window_rect(names[i]);
            CHECK(near_eq(r.min.x, before[i].min.x, 1.0f) && near_eq(r.max.y, before[i].max.y, 1.0f));
        }
    }

    // bad text changes nothing
    CHECK(!h.ui.dock_load_layout(""));
    CHECK(!h.ui.dock_load_layout("not a layout"));
    CHECK(!h.ui.dock_load_layout("strata-dock 2\nspace zz 0\n"));
    CHECK(!h.ui.dock_load_layout("strata-dock 2\nspace 1 0\nS h 0.5\nL 0 0\n")); // a split needs two children
    CHECK(!h.ui.dock_load_layout(text.substr(0, text.find("\nL ") + 5)));         // cut off inside a pane
    h.frames(build, 2);
    for (const char* n : names) { CHECK(h.ui.window_docked(n)); }

    // a window that is not in the text keeps floating; one that is not shown any more is skipped
    std::string partial = text;
    const std::size_t pos = partial.find("W ");
    CHECK(pos != std::string::npos);
    CHECK(h.ui.dock_load_layout(partial));
    h.frames(build, 3);
}

// docking comfort: tabs (bar drop, reorder, double click), drop guides, Shift / Esc, splitter reset ----------------

void test_dock_comfort()
{
    std::fprintf(stderr, "[dock comfort: tab bar drops, reordering, guides, Shift / Esc, double clicks]\n");
    constexpr window_flags flags = window_flags::dockable | window_flags::resizable;
    harness h;
    const auto build = [&] {
        h.ui.dock_area({{0, 0}, {800, 600}});
        for (const char* name : {"Alpha", "Beta", "Gamma", "Delta"}) {
            if (auto w = h.ui.window(name, {520, 300}, {220, 150}, flags)) { h.ui.text(name); }
        }
    };
    h.frames(build, 3);
    const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
    const f32 w_alpha = h.ui.font().measure(0, "Alpha").x + 26.0f;
    const f32 w_beta  = h.ui.font().measure(0, "Beta").x + 26.0f;
    // where the titles come in the saved layout: the order of the tabs
    const auto before = [&](const char* first, const char* second) {
        const std::string text = h.ui.dock_save_layout();
        const std::size_t a = text.find(std::string(" ") + first + "\n");
        const std::size_t b = text.find(std::string(" ") + second + "\n");
        return a != std::string::npos && b != std::string::npos && a < b;
    };
    // a title bar drag of a floating window, up to (not including) the release
    const auto grab_title = [&](const char* name, vec2 to, bool shift, bool esc) {
        const rect r = h.ui.window_rect(name);
        const vec2 from{r.min.x + r.width() * 0.5f, r.min.y + tab_h * 0.5f};
        h.move(from);
        h.frames(build, 3);
        h.down();
        h.frame(build);
        h.move({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
        h.frames(build, 2);
        h.move(to);
        h.in.shift = shift;
        h.frames(build, 3);
        if (esc) {
            h.key(key::escape);
            h.frames(build, 2);
        }
    };
    const auto release = [&] {
        h.up();
        h.frames(build, 4);
        h.in.shift = false;
    };

    // Alpha and Beta are tabs of the left pane, Gamma is the pane to its right
    CHECK(h.ui.dock_window("Alpha", dock_zone::center));
    CHECK(h.ui.dock_window("Beta", dock_zone::center, "Alpha"));
    CHECK(h.ui.dock_window("Gamma", dock_zone::right, "Alpha"));
    h.frames(build, 3);
    CHECK(before("Alpha", "Beta"));

    // a tab dragged along its bar moves among the others instead of leaving the dock
    h.move({700, 580});
    h.frames(build, 2);
    const vec2 beta_tab{w_alpha + w_beta * 0.5f, tab_h * 0.5f};
    h.move(beta_tab);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({w_alpha * 0.25f, tab_h * 0.5f + 4.0f}); // (a little off the bar's line does not tear it off)
    h.frames(build, 3);
    CHECK(h.ui.window_docked("Beta"));
    CHECK(before("Beta", "Alpha"));
    h.up();
    h.frames(build, 2);
    CHECK(h.ui.window_docked("Beta") && h.ui.window_docked("Alpha"));

    // ... and back again
    const vec2 beta_now{w_beta * 0.5f, tab_h * 0.5f};
    h.move(beta_now);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({w_beta + w_alpha * 0.8f, tab_h * 0.5f});
    h.frames(build, 3);
    CHECK(before("Alpha", "Beta"));
    h.up();
    h.frames(build, 2);

    // a window dropped on a pane's tab bar joins its tabs at the drop position (even at the area's very top)
    rect alpha = h.ui.window_rect("Alpha");
    grab_title("Delta", {24.0f, tab_h * 0.5f}, false, false);
    release();
    CHECK(h.ui.window_docked("Delta"));
    rect delta = h.ui.window_rect("Delta");
    CHECK(near_eq(delta.min.x, alpha.min.x) && near_eq(delta.max.x, alpha.max.x) && near_eq(delta.min.y, alpha.min.y));
    CHECK(before("Delta", "Alpha") && before("Alpha", "Beta"));

    // a double click on a tab floats the window
    const f32 w_delta = h.ui.font().measure(0, "Delta").x + 26.0f;
    const vec2 delta_tab{w_delta * 0.5f, tab_h * 0.5f};
    h.move(delta_tab);
    h.frames(build, 3);
    for (int i = 0; i < 2; ++i) {
        h.down();
        h.frame(build);
        h.up();
        h.frame(build);
    }
    h.frames(build, 2);
    CHECK(!h.ui.window_docked("Delta"));
    CHECK(h.ui.window_docked("Alpha") && h.ui.window_docked("Beta"));

    // Shift while dragging: no docking (nothing to preview, nothing dropped) ...
    alpha = h.ui.window_rect("Alpha");
    const vec2 middle{alpha.center().x, alpha.center().y};
    grab_title("Delta", middle, true, false);
    release();
    CHECK(!h.ui.window_docked("Delta"));
    // ... and Esc cancels the drop of that drag
    grab_title("Delta", middle, false, true);
    release();
    CHECK(!h.ui.window_docked("Delta"));
    // the next drag docks again
    grab_title("Delta", middle, false, false);
    release();
    CHECK(h.ui.window_docked("Delta"));
    h.ui.undock_window("Delta");
    h.frames(build, 2);

    // the guide left of a pane's centre is a large split target, where the middle would only add a tab
    alpha = h.ui.window_rect("Alpha");
    const vec2 c = alpha.center();
    const f32 step = 28.0f + 4.0f;
    grab_title("Delta", {c.x - step, c.y}, false, false);
    release();
    CHECK(h.ui.window_docked("Delta"));
    delta = h.ui.window_rect("Delta");
    alpha = h.ui.window_rect("Alpha");
    CHECK(near_eq(delta.min.x, 0.0f) && delta.max.x < alpha.min.x);
    h.ui.undock_window("Delta");
    h.frames(build, 3);

    // the guide at the border of the space splits the whole tree
    grab_title("Delta", {400.0f, 600.0f - 20.0f}, false, false); // (bottom guide: 8 + 12 from the border)
    release();
    CHECK(h.ui.window_docked("Delta"));
    delta = h.ui.window_rect("Delta");
    CHECK(near_eq(delta.min.x, 0.0f) && near_eq(delta.max.x, 800.0f) && near_eq(delta.max.y, 600.0f));
    CHECK(delta.min.y > 400.0f);
    h.ui.undock_window("Delta");
    h.frames(build, 3);

    // a double click on a splitter shares the space equally again
    alpha = h.ui.window_rect("Alpha");
    rect gamma = h.ui.window_rect("Gamma");
    const vec2 grab{(alpha.max.x + gamma.min.x) * 0.5f, 300.0f};
    h.move(grab);
    h.frames(build, 3);
    h.down();
    h.frame(build);
    h.move({grab.x + 120.0f, grab.y});
    h.frames(build, 3);
    h.up();
    h.frames(build, 3);
    alpha = h.ui.window_rect("Alpha");
    gamma = h.ui.window_rect("Gamma");
    CHECK(alpha.width() > gamma.width() + 100.0f);
    const vec2 split{(alpha.max.x + gamma.min.x) * 0.5f, 300.0f};
    h.move(split);
    h.frames(build, 3);
    for (int i = 0; i < 2; ++i) {
        h.down();
        h.frame(build);
        h.up();
        h.frame(build);
    }
    h.frames(build, 3);
    alpha = h.ui.window_rect("Alpha");
    gamma = h.ui.window_rect("Gamma");
    CHECK(near_eq(alpha.width(), gamma.width(), 6.0f));

    // an edge dock goes back to its size on a double click of its handle
    {
        harness g;
        rect main_area{};
        const auto build_g = [&] {
            rect client{{0, 0}, {800, 600}};
            client = g.ui.dock_edge("side", dock_side::left, 200.0f, client);
            g.ui.dock_area(client);
            main_area = client;
            if (auto w = g.ui.window("E", {400, 300}, {200, 150}, flags)) { g.ui.text("e"); }
        };
        g.frames(build_g, 3);
        CHECK(g.ui.dock_window("E", dock_zone::center, {}, 0.5f, "side"));
        g.frames(build_g, 3);
        CHECK(near_eq(main_area.min.x, 204.0f, 1.0f));
        const vec2 handle{202.0f, 300.0f};
        g.move(handle);
        g.frames(build_g, 3);
        g.down();
        g.frame(build_g);
        g.move({handle.x + 100.0f, handle.y});
        g.frames(build_g, 3);
        g.up();
        g.frames(build_g, 3);
        CHECK(near_eq(main_area.min.x, 304.0f, 2.0f));
        g.move({main_area.min.x - 2.0f, 300.0f});
        g.frames(build_g, 3);
        for (int i = 0; i < 2; ++i) {
            g.down();
            g.frame(build_g);
            g.up();
            g.frame(build_g);
        }
        g.frames(build_g, 3);
        CHECK(near_eq(main_area.min.x, 204.0f, 1.0f));
    }

    // a window docks at about its own size (within limits), not at half of the pane
    {
        harness g;
        const auto build_g = [&] {
            g.ui.dock_area({{0, 0}, {800, 600}});
            if (auto w = g.ui.window("Big", {300, 200}, {300, 200}, flags)) { g.ui.text("big"); }
            if (auto w = g.ui.window("Small", {300, 200}, {150, 100}, flags)) { g.ui.text("small"); }
        };
        g.frames(build_g, 3);
        CHECK(g.ui.dock_window("Big", dock_zone::center));
        g.frames(build_g, 3);
        const rect big = g.ui.window_rect("Big");
        const rect sm  = g.ui.window_rect("Small");
        const vec2 from{sm.min.x + sm.width() * 0.5f, sm.min.y + tab_h * 0.5f};
        const vec2 to{big.center().x - 32.0f, big.center().y}; // the left guide of the pane
        g.move(from);
        g.frames(build_g, 3);
        g.down();
        g.frame(build_g);
        g.move({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
        g.frames(build_g, 2);
        g.move(to);
        g.frames(build_g, 3);
        g.up();
        g.frames(build_g, 4);
        CHECK(g.ui.window_docked("Small"));
        const rect docked = g.ui.window_rect("Small");
        CHECK(docked.width() > 130.0f && docked.width() < 190.0f); // 150 wide window, 800 wide pane: the smallest share (20%) or a bit less
        CHECK(near_eq(docked.min.x, 0.0f));
    }
}

void test_dock_group_drag()
{
    std::fprintf(stderr, "[dock: dragging a whole pane]\n");
    constexpr window_flags flags = window_flags::dockable | window_flags::resizable;
    rect area{{0, 0}, {800, 600}};
    const auto make = [&](harness& h) {
        return [&h, &area, flags] {
            h.ui.dock_area(area);
            for (const char* name : {"A", "B", "C"}) {
                if (auto w = h.ui.window(name, {560, 400}, {200, 120}, flags)) { h.ui.text(name); }
            }
        };
    };
    const auto drag = [&](harness& h, auto& build, vec2 from, vec2 to) {
        h.move(from);
        h.frames(build, 3);
        h.down();
        h.frame(build);
        h.move({from.x + 20.0f, from.y + 20.0f});
        h.frames(build, 2);
        h.move({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f});
        h.frames(build, 2);
        h.move(to);
        h.frames(build, 3);
        h.up();
        h.frames(build, 4);
    };

    {
        // A and B are tabs of the left pane, C the right: pane A+B is dropped on the right one
        harness h;
        const auto build = make(h);
        h.frames(build, 3);
        const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
        CHECK(h.ui.dock_window("A", dock_zone::center));
        CHECK(h.ui.dock_window("C", dock_zone::right, "A"));
        CHECK(h.ui.dock_window("B", dock_zone::center, "A"));
        h.frames(build, 3);
        const rect c0 = h.ui.window_rect("C");
        CHECK(c0.min.x > 300.0f);
        const f32 wa = h.ui.font().measure(0, "A").x + 26.0f;
        const f32 wb = h.ui.font().measure(0, "B").x + 26.0f;
        drag(h, build, {wa + wb + 40.0f, tab_h * 0.5f}, {c0.center().x, c0.center().y});
        for (const char* n : {"A", "B", "C"}) { CHECK(h.ui.window_docked(n)); }
        const rect a = h.ui.window_rect("A");
        const rect b = h.ui.window_rect("B");
        const rect c = h.ui.window_rect("C");
        CHECK(near_eq(a.min.x, 0.0f) && near_eq(a.max.x, 800.0f)); // one pane with three tabs
        CHECK(near_eq(a.min.x, b.min.x) && near_eq(a.max.x, c.max.x) && near_eq(b.min.y, c.min.y));
    }
    {
        // dropped on the edge of the other pane: the tabs stay together and split it
        harness h;
        const auto build = make(h);
        h.frames(build, 3);
        const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
        CHECK(h.ui.dock_window("A", dock_zone::center));
        CHECK(h.ui.dock_window("C", dock_zone::right, "A"));
        CHECK(h.ui.dock_window("B", dock_zone::center, "A"));
        h.frames(build, 3);
        const rect c0 = h.ui.window_rect("C");
        const f32 wa = h.ui.font().measure(0, "A").x + 26.0f;
        const f32 wb = h.ui.font().measure(0, "B").x + 26.0f;
        // ... to the bottom half of C: A + B end up under C
        drag(h, build, {wa + wb + 40.0f, tab_h * 0.5f}, {c0.center().x, c0.max.y - 20.0f});
        const rect a = h.ui.window_rect("A");
        const rect b = h.ui.window_rect("B");
        const rect c = h.ui.window_rect("C");
        for (const char* n : {"A", "B", "C"}) { CHECK(h.ui.window_docked(n)); }
        CHECK(near_eq(a.min.x, b.min.x) && near_eq(a.max.y, b.max.y));
        CHECK(a.min.y > c.max.y - 2.0f);
        CHECK(near_eq(c.min.x, 0.0f) && near_eq(c.max.x, 800.0f)); // both panes now span the whole width
    }
    {
        // dropped where there is no dock: the windows float again, fanned out
        harness h;
        area = {{0, 0}, {800, 300}};
        const auto build = make(h);
        h.frames(build, 3);
        const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
        CHECK(h.ui.dock_window("A", dock_zone::center));
        CHECK(h.ui.dock_window("B", dock_zone::center, "A"));
        h.frames(build, 3);
        const f32 wa = h.ui.font().measure(0, "A").x + 26.0f;
        const f32 wb = h.ui.font().measure(0, "B").x + 26.0f;
        drag(h, build, {wa + wb + 40.0f, tab_h * 0.5f}, {400.0f, 450.0f});
        CHECK(!h.ui.window_docked("A") && !h.ui.window_docked("B"));
        const rect a = h.ui.window_rect("A");
        const rect b = h.ui.window_rect("B");
        CHECK(a.min.y > 300.0f && b.min.y > a.min.y);
        area = {{0, 0}, {800, 600}};
    }
}

// charts: ticks, units, area fills, zoom and pan ---------------------------------------------------------------------


// with dock animation on, the panes of a floating dock still move rigidly with it
void test_floating_dock_animated_move()
{
    std::fprintf(stderr, "[floating dock: rigid move with animation]\n");
    harness h;
    h.ui.set_dock_animation(true);
    const auto build = [&] {
        h.ui.dock_area({{0, 0}, {800, 600}});
        (void)h.ui.floating_dock("Tools", {300, 100}, {260, 260});
        if (auto w = h.ui.window("D", {10, 400}, {200, 120}, window_flags::dockable | window_flags::resizable)) { h.ui.text("d"); }
    };
    h.frames(build, 3);
    CHECK(h.ui.dock_window("D", dock_zone::center, {}, 0.5f, "Tools"));
    h.frames(build, 60); // settled
    const f32 tab_h = h.ui.font().line_height(0) + 10.0f;
    const rect t0 = h.ui.window_rect("Tools");
    const f32 offset_x = h.ui.window_rect("D").min.x - t0.min.x;
    const f32 offset_y = h.ui.window_rect("D").min.y - t0.min.y;

    vec2 grab{t0.min.x + 120.0f, t0.min.y + tab_h * 0.5f};
    h.move(grab);
    h.frames(build, 3);
    h.down();
    h.frame(build);
    f32 worst = 0.0f;
    for (int i = 1; i <= 12; ++i) {
        h.move({grab.x + 12.0f * static_cast<f32>(i), grab.y + 7.0f * static_cast<f32>(i)});
        h.frame(build);
        const rect t = h.ui.window_rect("Tools");
        const rect d = h.ui.window_rect("D");
        worst = std::max(worst, std::abs((d.min.x - t.min.x) - offset_x) + std::abs((d.min.y - t.min.y) - offset_y));
    }
    h.up();
    h.frames(build, 3);
    CHECK(h.ui.window_rect("Tools").min.x > t0.min.x + 100.0f); // it did move
    CHECK(worst < 1.0f);                                        // and D was never behind
}

void test_dock_animation_and_combo_multi()
{
    std::fprintf(stderr, "[dock animation, multi-select]\n");
    {
        harness h;
        h.ui.set_dock_animation(true);
        const auto build = [&] {
            h.ui.dock_area({{0, 0}, {800, 600}});
            constexpr window_flags f = window_flags::dockable | window_flags::resizable;
            if (auto w = h.ui.window("A", {100, 100}, {200, 150}, f)) { h.ui.text("a"); }
            if (auto w = h.ui.window("B", {400, 100}, {200, 150}, f)) { h.ui.text("b"); }
        };
        h.frames(build, 2);
        CHECK(h.ui.dock_window("A", dock_zone::center));
        h.frames(build, 90);
        CHECK(near_eq(h.ui.window_rect("A").max.x, 800.0f, 1.5f));
        CHECK(h.ui.dock_window("B", dock_zone::right, "A"));
        h.frame(build); // one frame: the panes have only started to move
        const rect early = h.ui.window_rect("A");
        CHECK(early.max.x > 500.0f);
        h.frames(build, 120); // the animation is over: A has its half
        const rect late = h.ui.window_rect("A");
        CHECK(near_eq(late.max.x, 398.0f, 4.0f));
        CHECK(near_eq(h.ui.window_rect("B").min.x, 402.0f, 4.0f) && near_eq(h.ui.window_rect("B").max.x, 800.0f, 1.5f));
    }
    {
        harness h;
        bool sel[4] = {false, true, false, false};
        bool changed_seen = false;
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
                changed_seen = h.ui.combo_multi("##m", sel, {"one", "two", "three", "four"}) || changed_seen;
            }
        };
        const f32 fh = h.ui.frame_height();
        h.click({100.0f, 12.0f + fh * 0.5f}, build); // open
        h.frames(build, 2);
        CHECK(h.ui.any_popup_open());
        const f32 item_h = fh - 4.0f;
        const auto row = [&](int i) { return vec2{120.0f, 12.0f + fh + 4.0f + 4.0f + item_h * (static_cast<f32>(i) + 0.5f)}; };
        h.click(row(0), build);
        CHECK(sel[0] && sel[1] && changed_seen);
        CHECK(h.ui.any_popup_open()); // the list stays open
        h.click(row(2), build);
        CHECK(sel[2]);
        h.click(row(1), build); // toggles off
        CHECK(!sel[1]);
        // the toolbar ("select all / clear") shows for longer lists; Esc closes
        h.key(key::escape);
        h.frames(build, 3);
        CHECK(!h.ui.any_popup_open());
        // keyboard: open, Down, Enter toggles the highlighted row
        sel[0] = sel[1] = sel[2] = sel[3] = false;
        h.click({100.0f, 12.0f + fh * 0.5f}, build);
        h.frames(build, 2);
        h.key(key::down);
        h.key(key::enter);
        h.frame(build);
        CHECK(sel[1] && !sel[0]);
    }
}


// tab bar / strip highlights move rigidly with the window
void test_tab_highlight_follows_window()
{
    std::fprintf(stderr, "[tab highlight follows a moved window]\n");
    harness h;
    int strip = 1, bar = 1;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {100, 100}, {400, 300}, window_flags::none)) {
            (void)h.ui.tab_bar("tb", {"one", "two", "three"}, bar);
            (void)h.ui.tab_strip("ts", {"a", "b", "c"}, strip, 0, 120.0f);
        }
    };
    h.frames(build, 60); // the highlights have settled
    const auto snapshot = [&] {
        const auto sh = h.ui.render_data().shapes;
        return std::vector<shape_record>(sh.begin(), sh.end());
    };
    const std::vector<shape_record> before = snapshot();
    const rect r0 = h.ui.window_rect("w");

    const vec2 grab{r0.min.x + 200.0f, r0.min.y + 10.0f};
    h.move(grab);
    h.frames(build, 3);
    h.down();
    h.frame(build);
    h.move({grab.x + 70.0f, grab.y + 40.0f});
    h.frame(build); // a frame after the jump: an absolute animation would lag behind
    const rect r1 = h.ui.window_rect("w");
    const vec2 delta{r1.min.x - r0.min.x, r1.min.y - r0.min.y};
    CHECK(delta.x > 30.0f && delta.y > 20.0f);

    const std::vector<shape_record> after = snapshot();
    CHECK(after.size() == before.size());
    f32 worst = 0.0f;
    for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i) {
        worst = std::max(worst, std::abs(after[i].center.x - before[i].center.x - delta.x) + std::abs(after[i].center.y - before[i].center.y - delta.y));
    }
    CHECK(worst < 1.0f);
    h.up();
    h.frames(build, 2);
}

// textures on the cpu: formats, mip maps, updates ------------------------------------------------------------------------

} // namespace

void run_dock_tests()
{
    test_docking();
    test_dock_spaces();
    test_dock_layout();
    test_dock_comfort();
    test_dock_group_drag();
    test_floating_dock_animated_move();
    test_dock_animation_and_combo_multi();
    test_tab_highlight_follows_window();
}

// checks for the list / tree work: off-screen rows doing no work, the extra-id overloads, overlapping items,
// right gutters, keyboard navigation, scroll control and the icon set

#include "selftest_common.hpp"

#include <strata/icons.hpp>

#include <set>
#include <string>

namespace {

// a tree of `depth` levels with `fanout` children each, all open, drawn inside a scrolling child
void deep_tree(context& ui, int depth, int fanout, int& rows, int index = 0)
{
    for (int i = 0; i < fanout; ++i) {
        const int key = index * 31 + i + 1;
        ++rows;
        if (depth <= 1) {
            (void)ui.tree_leaf("leaf", {reinterpret_cast<const char*>(&key), sizeof(key)}, false);
        } else if (ui.tree_node("node", {reinterpret_cast<const char*>(&key), sizeof(key)}, tree_flags::default_open)) {
            deep_tree(ui, depth - 1, fanout, rows, key);
            ui.tree_pop();
        }
    }
}

void test_culling()
{
    harness h;
    int rows = 0;
    f32 max_scroll = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {400.0f, 400.0f}, plain_window)) {
            if (auto c = h.ui.child("list", {0.0f, 200.0f})) {
                rows = 0;
                deep_tree(h.ui, 3, 12, rows); // 12 + 144 + 1728 = 1884 rows
                max_scroll = h.ui.scroll_max_y(); // asked inside the region it belongs to
            }
        }
    };
    h.frames(build, 3);

    const frame_stats& st = h.ui.stats();
    CHECK(rows == 1884);
    CHECK(st.items_submitted >= 1884);
    // a 200 px viewport holds roughly a dozen rows; everything else must have been skipped
    const u32 drawn = st.items_submitted - st.items_culled;
    CHECK(drawn > 0 && drawn < 40);
    CHECK(st.items_culled > 1800);
    // the rows that were skipped did not take an animation slot either
    CHECK(st.anim_slots_used < 64);
    // ... and their labels were not measured
    CHECK(st.text_measures + st.measure_hits < 64);

    // the scrollbar still sees the whole list: the layout was advanced for every row
    CHECK(max_scroll > 1884.0f * 10.0f);
}

// culling must not change what is drawn: the same tree in a viewport tall enough to hold it produces the same
// geometry as one where nothing is off-screen
void test_culling_is_invisible()
{
    const auto count = [](bool tall) {
        harness h;
        h.in.display_size = {800.0f, 600.0f};
        int rows = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("w", {0.0f, 0.0f}, {400.0f, 560.0f}, plain_window)) {
                if (auto c = h.ui.child("list", {0.0f, tall ? 500.0f : 60.0f})) {
                    rows = 0;
                    deep_tree(h.ui, 2, 3, rows);
                }
            }
        };
        h.frames(build, 3);
        return h.ui.stats();
    };
    const frame_stats tall = count(true);
    const frame_stats shrt = count(false);
    CHECK(tall.items_culled == 0);
    CHECK(shrt.items_culled > 0);
    CHECK(tall.vertices > shrt.vertices); // fewer rows drawn means less geometry, nothing else
}

void test_extra_id()
{
    harness h;
    // two rows with the same label are two rows, not one, when they carry different ids
    int hits = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (h.ui.selectable("same", "a", false)) { hits += 1; }
            if (h.ui.selectable("same", "b", false)) { hits += 10; }
        }
    };
    h.frames(build, 2);
    const rect second = h.ui.item_rect();
    h.click(second.center(), build);
    CHECK(hits == 10); // the second row, not the first

    // push_id(const void*) / push_id(u64) separate otherwise identical subtrees
    int a = 0, b = 0;
    int x = 0, y = 0;
    const auto build2 = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            h.ui.push_id(&x);
            if (h.ui.selectable("row", false)) { ++a; }
            h.ui.pop_id();
            h.ui.push_id(&y);
            if (h.ui.selectable("row", false)) { ++b; }
            h.ui.pop_id();
        }
    };
    h.frames(build2, 2);
    const rect row_b = h.ui.item_rect();
    h.click(row_b.center(), build2);
    CHECK(a == 0 && b == 1);
}

void test_item_overlap()
{
    harness h;
    int row_hits = 0;
    int btn_hits = 0;
    bool claimed = false;
    rect btn{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            const item_result row = h.ui.custom_item("row", {0.0f, 30.0f});
            h.ui.allow_item_overlap();
            h.ui.same_line_right(40.0f);
            if (h.ui.button("x##btn")) { ++btn_hits; }
            btn = h.ui.item_rect();
            if (row.pressed) { ++row_hits; }
            claimed = h.ui.item_claimed();
        }
    };
    h.frames(build, 2);

    // the button is drawn on top of the full-width row and submitted after it: the press is the button's
    h.click(btn.center(), build);
    CHECK(btn_hits == 1);
    CHECK(row_hits == 0);

    // without the opt-in the row (submitted first) keeps the press, which is the old behaviour
    harness h2;
    int row2 = 0, btn2 = 0;
    rect btn2r{};
    const auto build2 = [&] {
        if (auto w = h2.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            const item_result row = h2.ui.custom_item("row", {0.0f, 30.0f});
            h2.ui.same_line_right(40.0f);
            if (h2.ui.button("x##btn")) { ++btn2; }
            btn2r = h2.ui.item_rect();
            if (row.pressed) { ++row2; }
        }
    };
    h2.frames(build2, 2);
    h2.click(btn2r.center(), build2);
    CHECK(row2 == 1 && btn2 == 0);
}

void test_double_click_and_buttons()
{
    harness h;
    int doubles = 0;
    int rights  = 0;
    rect row{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h.ui.selectable("row", false);
            row = h.ui.item_rect();
            if (h.ui.item_double_clicked()) { ++doubles; }
            if (h.ui.item_clicked(mouse_button::right)) { ++rights; }
        }
    };
    h.frames(build, 2);
    h.move(row.center());
    h.frame(build);
    h.click(row.center(), build);
    CHECK(doubles == 0);
    h.click(row.center(), build); // the second click of a pair
    CHECK(doubles == 1);

    h.in.mouse_down[1] = true;
    h.frame(build);
    h.in.mouse_down[1] = false;
    h.frame(build);
    CHECK(rights == 1);
}

void test_right_gutter()
{
    harness h;
    rect full{}, gutted{}, right{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h.ui.custom_item("full", {0.0f, 20.0f});
            full = h.ui.item_rect();
            {
                auto g = h.ui.right_gutter(60.0f);
                (void)h.ui.custom_item("gutted", {0.0f, 20.0f});
                gutted = h.ui.item_rect();
            }
            h.ui.same_line_right(30.0f);
            (void)h.ui.custom_item("right", {30.0f, 20.0f});
            right = h.ui.item_rect();
        }
    };
    h.frames(build, 2);
    CHECK(near_eq(full.width() - gutted.width(), 60.0f, 0.5f));
    CHECK(near_eq(right.max.x, full.max.x, 0.5f));      // same_line_right reaches the real edge
    CHECK(near_eq(right.width(), 30.0f, 0.5f));
    CHECK(right.min.x >= gutted.max.x);                 // ... which is inside the gutter, clear of the row
}

void test_navigation()
{
    harness h;
    std::array<int, 6> picked{};
    int focused = -1;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            auto nav = h.ui.navigation("list");
            for (int i = 0; i < 6; ++i) {
                if (h.ui.selectable("row", {reinterpret_cast<const char*>(&i), sizeof(i)}, false)) {
                    ++picked[static_cast<std::size_t>(i)];
                }
                if (h.ui.item_focused()) { focused = i; }
            }
        }
    };
    h.frames(build, 2);
    CHECK(h.ui.nav_active());

    h.key(key::down);
    h.frame(build); // resolves the key
    h.frame(build); // the row draws itself as focused
    CHECK(focused == 0);

    h.key(key::down);
    h.frame(build);
    h.frame(build);
    CHECK(focused == 1);

    h.key(key::end);
    h.frame(build);
    h.frame(build);
    CHECK(focused == 5);

    h.key(key::home);
    h.frame(build);
    h.frame(build);
    CHECK(focused == 0);

    h.key(key::enter);
    h.frame(build);  // the Enter is resolved at nav_end
    h.frame(build);  // ... and reported on the row it landed on
    CHECK(picked[0] == 1);
    for (std::size_t i = 1; i < picked.size(); ++i) { CHECK(picked[i] == 0); }
}

void test_nav_tree_arrows()
{
    harness h;
    bool child_seen = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            auto nav = h.ui.navigation("tree");
            child_seen = false;
            if (h.ui.tree_node("parent", tree_flags::default_open)) {
                child_seen = true;
                (void)h.ui.tree_leaf("child");
                h.ui.tree_pop();
            }
        }
    };
    h.frames(build, 2);
    CHECK(child_seen);

    h.key(key::down);   // the cursor lands on the node
    h.frames(build, 2);
    h.key(key::left);   // ... and closes it
    h.frames(build, 2);
    CHECK(!child_seen);
    h.key(key::right);  // ... and opens it again
    h.frames(build, 2);
    CHECK(child_seen);
}

void test_tree_open_controls()
{
    harness h;
    int depth_seen = 0;
    bool force_open = false;
    bool close_all  = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            if (close_all) { h.ui.close_all_tree_nodes(); close_all = false; }
            depth_seen = 0;
            if (force_open) { h.ui.set_next_item_open(true); }
            if (h.ui.tree_node("a")) {
                depth_seen = 1;
                if (h.ui.tree_node("b")) {
                    depth_seen = 2;
                    h.ui.tree_pop();
                }
                h.ui.tree_pop();
            }
        }
    };
    h.frames(build, 2);
    CHECK(depth_seen == 0); // closed by default

    force_open = true;
    h.frame(build);
    force_open = false;
    h.frames(build, 2);
    CHECK(depth_seen == 1); // set_next_item_open stuck

    // open everything: the inner node only exists once its parent is open, so the request has to outlive one frame
    h.frame([&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            h.ui.open_all_tree_nodes();
            if (h.ui.tree_node("a")) {
                if (h.ui.tree_node("b")) { h.ui.tree_pop(); }
                h.ui.tree_pop();
            }
        }
    });
    h.frames(build, 3);
    CHECK(depth_seen == 2);

    close_all = true;
    h.frames(build, 3);
    CHECK(depth_seen == 0);

    // and the request must not keep overriding what the user does next: a node opened right after a close-all
    // stays open
    force_open = true;
    h.frame(build);
    force_open = false;
    h.frames(build, 3);
    CHECK(depth_seen == 1);
}

// Ctrl held while a node is clicked opens or closes the whole subtree under it, and only that subtree
void test_tree_recursive_click()
{
    harness h;
    int  depth_seen = 0;
    bool other_open = false;
    rect arrow{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            depth_seen = 0;
            if (h.ui.tree_node("a", tree_flags::default_open)) {
                arrow      = h.ui.item_rect();
                depth_seen = 1;
                if (h.ui.tree_node("b", tree_flags::default_open)) {
                    depth_seen = 2;
                    h.ui.tree_pop();
                }
                h.ui.tree_pop();
            }
            other_open = h.ui.tree_node("other", tree_flags::default_open);
            if (other_open) { h.ui.tree_pop(); }
        }
    };
    h.frames(build, 2);
    CHECK(depth_seen == 2 && other_open);

    // a plain click on "a" only closes "a"
    h.click({arrow.min.x + 10.0f, arrow.center().y}, build);
    h.frames(build, 2);
    CHECK(depth_seen == 0);
    h.click({arrow.min.x + 10.0f, arrow.center().y}, build); // open it again: "b" is still open under it
    h.frames(build, 2);
    CHECK(depth_seen == 2);

    // Ctrl + click closes "a" and everything under it
    h.in.ctrl = true;
    h.click({arrow.min.x + 10.0f, arrow.center().y}, build);
    h.in.ctrl = false;
    h.frames(build, 2);
    CHECK(depth_seen == 0);
    CHECK(other_open); // a sibling subtree is untouched
    h.click({arrow.min.x + 10.0f, arrow.center().y}, build);
    h.frames(build, 2);
    CHECK(depth_seen == 1); // "b" was closed by the recursive click, so opening "a" shows only one level
}

void test_scroll()
{
    harness h;
    rect last{};
    bool reveal = false;
    f32  max_scroll = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 400.0f}, plain_window)) {
            if (auto c = h.ui.child("list", {0.0f, 120.0f})) {
                for (int i = 0; i < 60; ++i) {
                    (void)h.ui.selectable("row", {reinterpret_cast<const char*>(&i), sizeof(i)}, false);
                    if (reveal && i == 50) { h.ui.ensure_item_visible(); last = h.ui.item_rect(); }
                }
                max_scroll = h.ui.scroll_max_y();
            }
        }
    };
    h.frames(build, 2);
    CHECK(max_scroll > 100.0f);

    reveal = true;
    h.frames(build, 8); // it moves as far as it has to each frame, so it settles in a few
    // the row is in view now: its rectangle is inside the child, which is 120 px tall at the window top
    CHECK(last.min.y >= 0.0f && last.max.y <= 400.0f);
    CHECK(last.min.y > 0.0f && last.max.y < 150.0f);
}

void test_measure_cache()
{
    harness h;
    // the cached size has to be the size the font really reports, for every font on the stack
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            h.ui.text("the quick brown fox");
            h.ui.text("the quick brown fox"); // a second pass hits the cache
            h.ui.text("another string entirely");
        }
    };
    h.frames(build, 3);
    CHECK(h.ui.stats().measure_hits > 0);

    const vec2 direct = h.ui.font().measure(0, "the quick brown fox");
    harness h2;
    rect r{};
    h2.frames([&] {
        if (auto w = h2.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h2.ui.button("the quick brown fox");
            r = h2.ui.item_rect();
        }
    }, 3);
    CHECK(near_eq(r.width(), direct.x + h2.ui.theme().frame_padding.x * 2.0f, 0.5f));
}

void test_skip_item()
{
    harness h;
    rect a{}, b{};
    h.frames([&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h.ui.custom_item("a", {0.0f, 10.0f});
            a = h.ui.item_rect();
            h.ui.skip_item(100.0f);
            (void)h.ui.custom_item("b", {0.0f, 10.0f});
            b = h.ui.item_rect();
        }
    }, 2);
    CHECK(near_eq(b.min.y - a.max.y, 100.0f + 2.0f * h.ui.theme().item_spacing, 0.5f));
}

void test_input_clear_button()
{
    harness h;
    std::string value = "hello";
    rect box{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h.ui.input_text("f", value, {}, input_flags::clear_button);
            box = h.ui.item_rect();
        }
    };
    h.frames(build, 2);
    // the x sits just inside the right edge of the field
    h.click({box.max.x - 12.0f, box.center().y}, build);
    CHECK(value.empty());
}

void test_icon_set()
{
    // every constant has to be a code point the icon font really has. the sandbox default (Segoe MDL2 Assets) is not
    // always installed, and the Windows 10 build of it is missing "Hide" (see icons.hpp), so this reports rather than
    // fails on those.
    static constexpr std::array<codepoint_range, 1> pua = {glyph_ranges::private_use};
    font_config icon_font;
    icon_font.face         = "Segoe MDL2 Assets";
    icon_font.pixel_height = 16.0f;
    icon_font.ranges       = pua;
    const auto built = font_atlas::build(icon_font, 4096, 1.0f);
    if (!built) {
        std::fprintf(stderr, "  note: Segoe MDL2 Assets is not installed, the icon check was skipped\n");
        return;
    }
    // no two names may be left at the placeholder, and none may repeat by accident
    std::set<std::string_view> names;
    int missing = 0;
    for (const auto& entry : icons::all) {
        CHECK(names.insert(entry.name).second);
        if (!built->has_glyph(0, entry.code)) {
            std::fprintf(stderr, "  note: icons::%s (U+%04X) has no glyph in Segoe MDL2 Assets on this machine\n",
                         std::string{entry.name}.c_str(), static_cast<unsigned>(entry.code));
            ++missing;
        }
    }
    CHECK(missing <= 1); // only eye_off / "Hide" is allowed to be absent on older Windows 10 fonts
}


// a frame that touches thousands of distinct keys grows the animation table to hold them (that is what a fully
// expanded tree used to do on every frame). once the live key count drops back -- which is what culling does -- the
// table has to come back down instead of staying big, and being a cache miss per lookup, forever
void test_anim_table_shrinks()
{
    harness h;
    int live = 2000;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            for (int i = 0; i < live; ++i) {
                (void)h.ui.animate(std::to_string(i), 1.0f);
            }
        }
    };
    h.frames(build, 3);
    const u32 grown = h.ui.stats().anim_slots_total;
    CHECK(grown >= 4096);
    CHECK(h.ui.stats().anim_slots_used >= 2000u);

    live = 4;
    h.frames(build, 300); // the compaction runs on a frame boundary, not on every frame
    const frame_stats& st = h.ui.stats();
    CHECK(st.anim_slots_total < grown);
    CHECK(st.anim_slots_used < 64);
}

// the hover highlight of an item that shares its rectangle goes to whatever is drawn on top of it
void test_overlap_hover()
{
    harness h;
    bool row_hovered = false;
    rect btn{};
    rect row_rect{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            const item_result row = h.ui.custom_item("row", {0.0f, 30.0f});
            row_hovered = row.hovered;
            row_rect    = row.bounds;
            h.ui.allow_item_overlap();
            h.ui.same_line_right(40.0f);
            (void)h.ui.button("x##btn");
            btn = h.ui.item_rect();
        }
    };
    h.frames(build, 2);

    h.move({row_rect.min.x + 5.0f, btn.center().y}); // over the row, clear of the button
    h.frames(build, 2);
    CHECK(row_hovered);

    h.move(btn.center());            // over the button: the row lets go of the highlight
    h.frames(build, 3);              // (it can only notice one frame later, see interact_impl)
    CHECK(!row_hovered);
}

} // namespace

void run_rows_tests()
{
    test_culling();
    test_culling_is_invisible();
    test_extra_id();
    test_item_overlap();
    test_overlap_hover();
    test_double_click_and_buttons();
    test_right_gutter();
    test_navigation();
    test_nav_tree_arrows();
    test_tree_open_controls();
    test_tree_recursive_click();
    test_scroll();
    test_measure_cache();
    test_skip_item();
    test_input_clear_button();
    test_anim_table_shrinks();
    test_icon_set();
}

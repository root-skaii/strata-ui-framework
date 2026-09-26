// checks for the machinery added alongside the draw-list index change: the clip / alpha stacks failing safely, text
// long enough to need several draw commands, horizontal scrolling, rich-text links, duplicate-id detection and the
// idle predicates.

#include "selftest_common.hpp"

namespace {

void test_stack_overflow()
{
    std::fprintf(stderr, "[draw list: clip / alpha stacks fail safely]\n");
    harness h;
    h.frames([&] {}, 1);
    font_atlas atlas = font_atlas::build().value();
    draw_list  dl;
    dl.begin({800, 600}, atlas, 1.0f);

    // the clip stack holds a fixed number of levels. pushing past it must not corrupt the levels that did fit: the
    // bug this replaces stored nothing but applied the new rect anyway, so every later pop restored the rect of a
    // shallower level and the rest of the frame was clipped wrongly.
    const rect outer{{100, 100}, {400, 400}};
    dl.push_clip(outer);
    const rect at_one = dl.clip();
    CHECK(at_one == outer);

    // 200 nested pushes, far past the capacity, then unwind all of them
    for (int i = 0; i < 200; ++i) {
        dl.push_clip({{110.0f + static_cast<f32>(i), 110.0f}, {390.0f, 390.0f}});
    }
    CHECK(dl.clip_stack_overflows() > 0); // it reports that it ran out ...
    for (int i = 0; i < 200; ++i) {
        dl.pop_clip();
    }
    CHECK(dl.clip() == at_one); // ... and the level that did fit is exactly where it was
    dl.pop_clip();
    CHECK(dl.clip().min.x == 0.0f && dl.clip().min.y == 0.0f); // back to the whole display

    // the alpha stack the same way: unwinding has to land back on 1
    dl.push_alpha(0.5f);
    CHECK(near_eq(dl.alpha(), 0.5f, 0.01f));
    for (int i = 0; i < 100; ++i) { dl.push_alpha(0.9f); }
    CHECK(dl.alpha_stack_overflows() > 0);
    for (int i = 0; i < 100; ++i) { dl.pop_alpha(); }
    CHECK(near_eq(dl.alpha(), 0.5f, 0.01f));
    dl.pop_alpha();
    CHECK(near_eq(dl.alpha(), 1.0f, 0.01f));

    // begin() clears the counters for the new frame
    dl.begin({800, 600}, atlas, 1.0f);
    CHECK(dl.clip_stack_overflows() == 0 && dl.alpha_stack_overflows() == 0);
}

void test_long_text_chunks()
{
    std::fprintf(stderr, "[draw list: text longer than one draw command]\n");
    harness h;
    h.frames([&] {}, 1);
    font_atlas atlas = font_atlas::build().value();
    draw_list  dl;
    // tall and wide enough that nothing is clipped away: the point is how many glyphs are emitted, not culling
    dl.begin({20000, 20000}, atlas, 1.0f);

    // one command can only index 65536 vertices, which is 16384 glyphs at 4 each. a string past that has to be split
    // across commands, and every glyph must still come out. laid out as many short lines: a single 30000-character
    // line would run off the right of the clip rectangle and most of it would be culled, which is not what is
    // being measured here.
    constexpr std::size_t per_line = 40;
    constexpr std::size_t lines    = 750;
    constexpr std::size_t glyphs   = per_line * lines; // 30000
    std::string many;
    for (std::size_t i = 0; i < lines; ++i) {
        if (i != 0) { many.push_back('\n'); }
        many.append(per_line, 'm');
    }
    dl.text({0, 0}, color{255, 255, 255, 255}, many);
    const draw_data dd = dl.data();
    CHECK(dd.vertices.size() == glyphs * 4);
    CHECK(dd.indices.size() == glyphs * 6);
    CHECK(dd.commands.size() >= 2); // it did split

    // every command's indices stay inside the 16-bit range, relative to its own vtx_offset -- which is the whole
    // reason for the split. a command that indexed past its slice would draw garbage.
    bool in_range = true;
    std::size_t total = 0;
    for (const draw_cmd& c : dd.commands) {
        total += c.idx_count;
        const std::size_t span = (c.vtx_offset <= dd.vertices.size() ? dd.vertices.size() - c.vtx_offset : 0);
        for (u32 k = 0; k < c.idx_count; ++k) {
            if (dd.indices[c.idx_offset + k] >= std::min<std::size_t>(span, max_command_vertices)) {
                in_range = false;
                break;
            }
        }
        if (!in_range) { break; }
    }
    CHECK(in_range);
    CHECK(total == dd.indices.size()); // the commands cover the index buffer exactly, with no gaps

    // ... and a bold run, which emits two quads per glyph and so splits twice as often
    dl.begin({20000, 20000}, atlas, 1.0f);
    dl.text({0, 0}, color{255, 255, 255, 255}, many, 0, text_flags::bold);
    CHECK(dl.data().vertices.size() == glyphs * 8);
    CHECK(!dl.overflowed());
}

void test_horizontal_scroll()
{
    std::fprintf(stderr, "[horizontal scrolling in child regions]\n");
    harness h;
    f32 seen_max = 0.0f;
    f32 seen_x   = 0.0f;
    // a child 200 wide holding an 800-wide item: 600 of overflow
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 300}, plain_window)) {
            if (h.ui.begin_child("pane", {200, 120}, child_flags::horizontal | child_flags::frame)) {
                seen_max = h.ui.scroll_max_x();
                seen_x   = h.ui.scroll_x();
                h.ui.set_next_item_width(800.0f);
                h.ui.button("wide");
                h.ui.end_child();
            }
        }
    };
    h.frames(build, 3);
    CHECK(seen_max > 400.0f); // it noticed the overflow (content 800 vs a ~186 wide view)
    CHECK(near_eq(seen_x, 0.0f, 0.5f));

    // a tilt wheel over the pane scrolls it sideways
    h.scroll_x({80, 80}, 2.0f, build);
    CHECK(seen_x > 0.0f);
    const f32 after_wheel = seen_x;

    // ... and shift + the ordinary wheel does the same
    h.modifiers(false, true);
    h.scroll({80, 80}, -1.0f, build);
    h.modifiers();
    h.frames(build, 2);
    CHECK(seen_x > after_wheel);

    // set_scroll_x is clamped to the content on the next frame
    const auto build_set = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 300}, plain_window)) {
            if (h.ui.begin_child("pane", {200, 120}, child_flags::horizontal | child_flags::frame)) {
                h.ui.set_scroll_x(100000.0f);
                h.ui.set_next_item_width(800.0f);
                h.ui.button("wide");
                h.ui.end_child();
            }
        }
    };
    h.frames(build_set, 2);
    h.frames(build, 2);
    CHECK(seen_x <= seen_max + 0.5f && seen_x > 0.0f);

    // without the flag there is no horizontal scrolling at all
    f32 plain_max = -1.0f;
    h.frames([&] {
        if (auto w = h.ui.window("t2", {0, 0}, {400, 300}, plain_window)) {
            if (h.ui.begin_child("plain", {200, 120})) {
                plain_max = h.ui.scroll_max_x();
                h.ui.set_next_item_width(800.0f);
                h.ui.button("wide");
                h.ui.end_child();
            }
        }
    }, 3);
    CHECK(plain_max == 0.0f);
}

void test_rich_links()
{
    std::fprintf(stderr, "[rich text links]\n");
    harness h;
    std::string clicked;
    std::string hovered;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {420, 200}, plain_window)) {
            h.ui.rich_text("go to <a=https://example.com>the manual</a> now");
            if (!h.ui.rich_link_clicked().empty()) { clicked.assign(h.ui.rich_link_clicked()); }
            if (!h.ui.rich_link_hovered().empty()) { hovered.assign(h.ui.rich_link_hovered()); }
        }
    };
    h.frames(build, 2);
    CHECK(clicked.empty() && hovered.empty()); // nothing under the pointer yet

    // find the link: the window's content starts one padding in, and the link follows "go to " on the first line
    const f32 pad = h.ui.theme().padding;
    const f32 pre = h.ui.font().measure(0, "go to ").x;
    const f32 mid = pad + pre + h.ui.font().measure(0, "the manual").x * 0.5f;
    const f32 y   = pad + h.ui.font().line_height(0) * 0.5f;
    h.click({mid, y}, build);
    CHECK(clicked == "https://example.com");
    CHECK(hovered == "https://example.com");

    // the frame after the click reports nothing again: it is an event, not a state
    clicked.clear();
    h.frames(build, 1);
    CHECK(clicked.empty());

    // clicking the plain text before the link is not a link click
    clicked.clear();
    h.click({pad + 2.0f, y}, build);
    CHECK(clicked.empty());

    // a link in a widget's caption is drawn but the widget owns the click
    bool pressed = false;
    std::string from_button;
    const auto button_build = [&] {
        if (auto w = h.ui.window("b", {0, 0}, {420, 200}, plain_window)) {
            auto rich = h.ui.rich_labels();
            if (h.ui.button("save <a=cmd:help>?</a>")) { pressed = true; }
            if (!h.ui.rich_link_clicked().empty()) { from_button.assign(h.ui.rich_link_clicked()); }
        }
    };
    h.frames(button_build, 2);
    h.click({pad + 20.0f, pad + h.ui.frame_height() * 0.5f}, button_build);
    CHECK(pressed);
    CHECK(from_button.empty());
}

void test_id_collisions()
{
    std::fprintf(stderr, "[duplicate widget ids are reported]\n");
    harness h;
    // two buttons with the same label in the same scope: the same id, so they share hover / press state
    h.frames([&] {
        if (auto w = h.ui.window("t", {0, 0}, {300, 200}, plain_window)) {
            h.ui.button("save");
            h.ui.button("save");
        }
    }, 2);
#ifndef NDEBUG
    CHECK(h.ui.stats().id_collisions >= 1);
    CHECK(h.ui.id_collision_label() == "save");
#endif

    // "##suffix" and push_id are the two ways out, and neither reports anything
    h.frames([&] {
        if (auto w = h.ui.window("t", {0, 0}, {300, 200}, plain_window)) {
            h.ui.button("save##first");
            h.ui.button("save##second");
            for (int i = 0; i < 3; ++i) {
                h.ui.push_id(i);
                h.ui.button("row");
                h.ui.pop_id();
            }
        }
    }, 2);
    CHECK(h.ui.stats().id_collisions == 0);
    CHECK(h.ui.id_collision() == 0);
}

void test_idle_predicates()
{
    std::fprintf(stderr, "[frame_unchanged / can_idle]\n");
    harness h;
    int counter = 0;
    const auto steady = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {300, 200}, plain_window)) {
            h.ui.text("nothing moves here");
        }
    };
    // the pointer is off-screen in the harness, so nothing hovers and nothing animates
    h.frames(steady, 12);
    CHECK(h.ui.frame_unchanged());
    CHECK(!h.ui.animations_settling());
    CHECK(h.ui.can_idle());
    CHECK(h.ui.stats().unchanged);

    // content that changes is reported as changed
    const auto changing = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {300, 200}, plain_window)) {
            h.ui.textf("frame {}", ++counter);
        }
    };
    h.frames(changing, 3);
    CHECK(!h.ui.frame_unchanged());
    CHECK(!h.ui.can_idle());

    // invalidate() forces the next frame to count as changed even when it is identical
    h.frames(steady, 6);
    CHECK(h.ui.frame_unchanged());
    h.ui.invalidate();
    h.frames(steady, 1);
    CHECK(!h.ui.frame_unchanged());
    h.frames(steady, 1);
    CHECK(h.ui.frame_unchanged());

    // a live toast keeps asking for frames: it has a timer to run down, which produces no new geometry while it
    // sits there but does change what should be on screen later
    h.ui.toast("hello");
    h.frames(steady, 3);
    CHECK(h.ui.animations_settling());
    CHECK(!h.ui.can_idle());
    h.ui.clear_toasts();
    h.frames(steady, 4);
    CHECK(h.ui.can_idle());

    // the content hash reaches the renderer through render_data
    CHECK(h.ui.render_data().content_hash != 0);
}

} // namespace

void run_new_tests()
{
    test_stack_overflow();
    test_long_text_chunks();
    test_horizontal_scroll();
    test_rich_links();
    test_id_collisions();
    test_idle_predicates();
}

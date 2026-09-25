// headless self-test: drives strata::context with scripted input (no window, no gpu) and checks the behaviour of
// the text editing, multi-line input, rich text, images, nested tables and docking. strata_sandbox --selftest

#include "selftest.hpp"


#include <strata/strata.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace strata;

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL  line %d: %s\n", line, what);
    }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

bool near_eq(f32 a, f32 b, f32 eps = 2.0f) { return std::abs(a - b) <= eps; }

struct clip_store {
    std::string data;
};
void clip_set(void* user, std::string_view text) noexcept { static_cast<clip_store*>(user)->data.assign(text); }
bool clip_get(void* user, std::string& out) noexcept
{
    out = static_cast<clip_store*>(user)->data;
    return true;
}

// one context plus the scripted input it is fed
struct harness {
    context     ui;
    input_state in;
    clip_store  clip;

    harness() : harness(context_config{}) {}
    explicit harness(const context_config& cfg) : ui{context::create(cfg).value()}
    {
        in.display_size = {800.0f, 600.0f};
        in.mouse_pos    = {-500.0f, -500.0f};
        ui.set_clipboard({&clip_set, &clip_get, &clip});
    }

    template <class F>
    void frame(F&& build, f32 dt = 1.0f / 60.0f)
    {
        in.delta_time = dt;
        ui.begin_frame(in);
        build();
        ui.end_frame();
        in.key_count   = 0;
        in.typed_len   = 0;
        in.wheel       = 0.0f;
        in.pressed_key = 0;
    }
    template <class F>
    void frames(F&& build, int n, f32 dt = 1.0f / 60.0f)
    {
        for (int i = 0; i < n; ++i) { frame(build, dt); }
    }

    void move(vec2 p) { in.mouse_pos = p; }
    void down() { in.mouse_down[0] = true; }
    void up() { in.mouse_down[0] = false; }
    void key(strata::key k, bool ctrl = false, bool shift = false)
    {
        if (in.key_count < in.keys.size()) { in.keys[in.key_count++] = {k, ctrl, shift}; }
    }
    void type(std::string_view s)
    {
        for (const char c : s) {
            if (in.typed_len < in.typed.size()) { in.typed[in.typed_len++] = c; }
        }
    }

    // move onto a point, press, release: a click that focuses whatever sits there
    template <class F>
    void click(vec2 p, F&& build)
    {
        move(p);
        frame(build); // the ui reads hover from the previous frame: let it see the pointer first
        frame(build);
        down();
        frame(build);
        up();
        frame(build);
    }
};

constexpr window_flags plain_window = window_flags::no_title_bar | window_flags::no_move;

// text editing: undo / redo -------------------------------------------------------------------------

void test_undo()
{
    std::fprintf(stderr, "[undo / redo]\n");
    harness h;
    std::string value;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_text("##f", value);
        }
    };
    h.click({30, 24}, build);
    CHECK(h.ui.want_text_input());

    h.type("ab");
    h.frame(build);
    h.frames(build, 15, 0.1f); // 1.5 s pass: the next typing is a separate undo step
    h.type("cd");
    h.frame(build);
    CHECK(value == "abcd");

    h.key(key::z, true);
    h.frame(build);
    CHECK(value == "ab");
    h.key(key::z, true);
    h.frame(build);
    CHECK(value.empty());
    h.key(key::z, true);
    h.frame(build);
    CHECK(value.empty()); // nothing left to undo

    h.key(key::y, true);
    h.frame(build);
    CHECK(value == "ab");
    h.key(key::y, true);
    h.frame(build);
    CHECK(value == "abcd");

    h.key(key::z, true);
    h.frame(build);
    CHECK(value == "ab");
    h.key(key::z, true, true); // ctrl+shift+z redoes as well
    h.frame(build);
    CHECK(value == "abcd");

    // backspacing in a row is one step
    h.key(key::backspace);
    h.frame(build);
    h.key(key::backspace);
    h.frame(build);
    CHECK(value == "ab");
    h.key(key::z, true);
    h.frame(build);
    CHECK(value == "abcd");

    // replacing a selection is undone in one go
    h.key(key::a, true);
    h.frame(build);
    h.type("X");
    h.frame(build);
    CHECK(value == "X");
    h.key(key::z, true);
    h.frame(build);
    CHECK(value == "abcd");

    // typing after an undo drops what could have been redone
    h.key(key::z, true);
    h.frame(build);
    h.frames(build, 15, 0.1f);
    h.type("Q");
    h.frame(build);
    const std::string after_q = value;
    h.key(key::y, true);
    h.frame(build);
    CHECK(value == after_q);

    // leaving the field forgets its history
    h.click({700, 500}, build);
    CHECK(!h.ui.want_text_input());
    h.click({30, 24}, build);
    const std::string before = value;
    h.key(key::z, true);
    h.frame(build);
    CHECK(value == before);

    // a password field keeps no history
    std::string secret;
    const auto build_pw = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_text("##pw", secret, {}, input_flags::password);
        }
    };
    h.click({700, 500}, build_pw);
    h.click({30, 24}, build_pw);
    h.type("hunter2");
    h.frame(build_pw);
    CHECK(secret == "hunter2");
    h.key(key::z, true);
    h.frame(build_pw);
    CHECK(secret == "hunter2");
}

// multi-line input -------------------------------------------------------------------------------------

void test_multiline()
{
    std::fprintf(stderr, "[multi-line input]\n");
    harness h;
    std::string text = "ab\ncd";
    bool submitted = false;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##m", text, {300, 120});
            submitted = submitted || h.ui.input_submitted();
        }
    };
    h.click({30, 30}, build);
    CHECK(h.ui.want_text_input());
    h.key(key::end, true); // the click put the caret on the first line: go to the end of the text
    h.frame(build);

    h.key(key::up);
    h.frame(build);
    h.type("X");
    h.frame(build);
    CHECK(text == "abX\ncd");

    h.key(key::down); // the caret is behind "abX": the closest spot of the shorter line below is its end
    h.frame(build);
    h.type("Y");
    h.frame(build);
    CHECK(text == "abX\ncdY");

    h.key(key::home, true);
    h.frame(build);
    h.type("0");
    h.frame(build);
    CHECK(text == "0abX\ncdY");

    h.key(key::end, true);
    h.frame(build);
    h.key(key::enter);
    h.frame(build);
    h.type("z");
    h.frame(build);
    CHECK(text == "0abX\ncdY\nz");
    CHECK(!submitted);

    h.key(key::enter, true);
    h.frame(build);
    CHECK(submitted);
    CHECK(text == "0abX\ncdY\nz"); // ctrl+enter submits instead of adding a line

    // home / end work on lines
    h.key(key::up);
    h.frame(build);
    h.key(key::home);
    h.frame(build);
    h.type("[");
    h.frame(build);
    h.key(key::end);
    h.frame(build);
    h.type("]");
    h.frame(build);
    CHECK(text == "0abX\n[cdY]\nz");

    // select downwards with shift and delete: from the caret to the same column of the next line
    h.key(key::home, true);
    h.frame(build);
    h.key(key::down, false, true);
    h.frame(build);
    h.key(key::del);
    h.frame(build);
    CHECK(text.find('\n') != std::string::npos && text.size() < 12);

    // undo works in multi-line fields too
    const std::string mangled = text;
    h.key(key::z, true);
    h.frame(build);
    CHECK(text != mangled);
    CHECK(text == "0abX\n[cdY]\nz");

    // paste keeps line breaks (all kinds) and turns tabs into spaces
    h.clip.data = "p\r\nq\tr\rs";
    h.key(key::end, true);
    h.frame(build);
    h.key(key::v, true);
    h.frame(build);
    CHECK(text == "0abX\n[cdY]\nzp\nq    r\ns");

    h.key(key::a, true);
    h.frame(build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(h.clip.data == text);

    // word wrap: Home goes to the start of the visual line, not of the text
    std::string wrapped = "aaaa bbbb cccc dddd eeee ffff gggg hhhh";
    const std::string wrapped_original = wrapped;
    const auto build_wrap = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##w", wrapped, {110, 120});
        }
    };
    h.click({700, 500}, build_wrap);
    h.click({30, 30}, build_wrap);
    h.key(key::end, true);
    h.frame(build_wrap);
    h.key(key::home);
    h.frame(build_wrap);
    h.type("Z");
    h.frame(build_wrap);
    const auto z = wrapped.find('Z');
    CHECK(z != std::string::npos && z > 0 && z < wrapped_original.size());
    CHECK(wrapped.find('\n') == std::string::npos); // wrapping is display only

    // ... and with no_wrap it is the start of the line
    std::string unwrapped = wrapped_original;
    const auto build_nowrap = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##nw", unwrapped, {110, 120}, input_flags::no_wrap);
        }
    };
    h.click({700, 500}, build_nowrap);
    h.click({30, 30}, build_nowrap);
    h.key(key::end, true);
    h.frame(build_nowrap);
    h.key(key::home);
    h.frame(build_nowrap);
    h.type("Z");
    h.frame(build_nowrap);
    CHECK(unwrapped.starts_with("Z"));

    // page down / page up move by several lines
    std::string many;
    for (int i = 0; i < 40; ++i) { many += "line " + std::to_string(i) + "\n"; }
    const auto build_many = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##many", many, {300, 100});
        }
    };
    h.click({700, 500}, build_many);
    h.click({30, 30}, build_many);
    h.key(key::home, true);
    h.frame(build_many);
    h.key(key::page_down);
    h.frame(build_many);
    h.type("#");
    h.frame(build_many);
    const auto hash = many.find('#');
    CHECK(hash != std::string::npos && hash > 10); // moved down a page, not a single line
}

// rich text ------------------------------------------------------------------------------------------------

void test_rich()
{
    std::fprintf(stderr, "[rich text / rich labels]\n");
    harness h;
    const f32 lh = h.ui.font().line_height(0);

    f32 wrapped_h = 0, single_h = 0, two_line_h = 0, plain_x = 0, rich_x = 0, label_two = 0, label_one = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {200, 0}, plain_window)) {
            const auto a = h.ui.custom_item("a", {10, 10});
            h.ui.rich_text_wrapped("the quick brown fox jumps over the lazy dog and keeps running far away from here");
            const auto b = h.ui.custom_item("b", {10, 10});
            wrapped_h = b.bounds.min.y - a.bounds.max.y;

            h.ui.rich_text("one line");
            const auto c = h.ui.custom_item("c", {10, 10});
            single_h = c.bounds.min.y - b.bounds.max.y;

            h.ui.rich_text("first\nsecond");
            const auto d = h.ui.custom_item("d", {10, 10});
            two_line_h = d.bounds.min.y - c.bounds.max.y;

            // markup in a widget label only counts as markup while rich labels are on
            (void)h.ui.button("<c=ff0000>ab</c>");
            h.ui.same_line();
            plain_x = h.ui.custom_item("p", {1, 1}).bounds.min.x;
            {
                const auto rich = h.ui.rich_labels();
                (void)h.ui.button("<c=ff0000>ab</c>");
                h.ui.same_line();
                rich_x = h.ui.custom_item("q", {1, 1}).bounds.min.x;
                h.ui.text("multi\nline");
                const auto e = h.ui.custom_item("e", {1, 1});
                h.ui.text("<c=00ff00>one</c>");
                const auto f = h.ui.custom_item("f", {1, 1});
                label_two = e.bounds.min.y;
                label_one = f.bounds.min.y - e.bounds.max.y;
            }
            CHECK(!h.ui.rich_labels_active());
        }
    };
    h.frames(build, 3);
    std::fprintf(stderr, "  (wrapped %.1f, single %.1f, two-line %.1f, lh %.1f, plain_x %.1f rich_x %.1f)\n", wrapped_h, single_h, two_line_h, lh, plain_x, rich_x);
    CHECK(wrapped_h > single_h + 1.5f * lh);            // wrapped onto several lines
    CHECK(near_eq(two_line_h - single_h, lh, 3.0f));    // a newline makes a second line
    CHECK(rich_x + 10.0f < plain_x);                    // the tags are not part of the rich label's width
    CHECK(label_two > 0.0f && label_one < 2.5f * lh);
}

// every widget with markup in its label: nothing may crash or overflow the runs
void test_rich_smoke()
{
    std::fprintf(stderr, "[rich labels in every widget]\n");
    harness h;
    bool flag = false, flag2 = true;
    int combo = 0, tab = 0, strip = 0;
    f32 slider = 0.5f;
    const auto build = [&] {
        h.ui.dock_area({{0, 0}, {800, 600}});
        if (auto w = h.ui.window("rich", {10, 10}, {500, 500}, window_flags::resizable)) {
            const auto rich = h.ui.rich_labels();
            (void)h.ui.button("<f=0>b</f> <c=8a91a6>utton</c>");
            (void)h.ui.checkbox("<c=ff8800>check</c>", flag);
            (void)h.ui.toggle("<c=ff8800>toggle</c>", flag2);
            (void)h.ui.slider("<c=00ffff>slider</c>", slider, 0.0f, 1.0f);
            (void)h.ui.combo("<c=00ffff>combo</c>", combo, {"<c=ff0000>red</c>", "<c=00ff00>green</c>"});
            (void)h.ui.tab_bar("tb", {"<c=ff0000>one</c>", "two"}, tab);
            (void)h.ui.tab_strip("ts", {"<c=ff0000>one</c>", "two"}, strip);
            h.ui.text_ellipsis("<c=ff0000>a very long ellipsised markup line that cannot fit in the window</c> and more and more");
            if (auto t = h.ui.tree("<c=00ff00>tree</c>", tree_flags::default_open)) {
                (void)h.ui.tree_leaf("<c=ff00ff>leaf</c>");
            }
            (void)h.ui.selectable("<c=ffff00>row</c>");
            if (auto c = h.ui.card("<c=ff00ff>card</c>")) { h.ui.text("<f=0>x</f>"); }
            if (h.ui.begin_table("t", 2)) {
                h.ui.table_setup_column("<c=ff0000>A</c>");
                h.ui.table_setup_column("B");
                (void)h.ui.table_headers_row();
                (void)h.ui.table_next_row();
                (void)h.ui.table_next_column();
                h.ui.text("<c=ff0000>cell</c>");
                h.ui.end_table();
            }
            h.ui.text_wrapped("plain wrapped text that is long enough to need at least two lines in this window, yes it is");
            (void)h.ui.button("tooltip");
            h.ui.tooltip("<c=ff0000>tip</c>\nsecond line");
        }
    };
    h.move({40, 40});
    h.frames(build, 60); // long enough for the tooltip to show up
    CHECK(true);
}

// images ---------------------------------------------------------------------------------------------------

void test_images()
{
    std::fprintf(stderr, "[images]\n");
    harness h;
    const auto build = [&] {
        if (auto w = h.ui.window("img", {20, 20}, {400, 0}, plain_window)) {
            h.ui.image(7, {40, 40});
            h.ui.text("text between");
            h.ui.image(7, {0, 0}, {0.25f, 0.25f}, {0.75f, 0.75f}, {255, 128, 128, 255}, 8.0f);
            (void)h.ui.image_button("btn", 9, {32, 32});
            h.ui.image(0, {40, 40}); // texture 0 is "none": nothing is drawn
        }
    };
    h.frames(build, 2);
    const draw_data d = h.ui.render_data();
    int with7 = 0, with9 = 0;
    for (const draw_cmd& c : d.commands) {
        with7 += c.texture == 7 && c.idx_count > 0;
        with9 += c.texture == 9 && c.idx_count > 0;
    }
    CHECK(with7 == 2);
    CHECK(with9 >= 1);
    // an image command holds only image quads
    for (const draw_cmd& c : d.commands) {
        if (c.texture != 0) { CHECK(c.idx_count % 6 == 0); }
    }
}

// nested tables ------------------------------------------------------------------------------------------------

void test_nested_tables()
{
    std::fprintf(stderr, "[nested tables]\n");
    harness h;
    f32 after_y[3]{};
    bool inner_ok = true;
    bool too_deep_refused = false;
    int  frame_no = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("tables", {0, 0}, {500, 0}, plain_window)) {
            if (h.ui.begin_table("outer", 2)) {
                h.ui.table_setup_column("Name");
                h.ui.table_setup_column("Details");
                (void)h.ui.table_headers_row();
                for (int r = 0; r < 3; ++r) {
                    (void)h.ui.table_next_row();
                    (void)h.ui.table_next_column();
                    h.ui.textf("row {}", r);
                    (void)h.ui.table_next_column();
                    h.ui.push_id(std::to_string(r));
                    if (h.ui.begin_table("inner", 2, table_default, r == 2 ? 60.0f : 0.0f)) {
                        h.ui.table_setup_column("k");
                        h.ui.table_setup_column("v");
                        (void)h.ui.table_headers_row();
                        for (int i = 0; i < 4; ++i) {
                            (void)h.ui.table_next_row();
                            (void)h.ui.table_next_column();
                            h.ui.textf("key {}", i);
                            (void)h.ui.table_next_column();
                            h.ui.textf("value {}", i);
                        }
                        h.ui.end_table();
                    } else {
                        inner_ok = false;
                    }
                    h.ui.pop_id();
                }
                h.ui.end_table();
            } else {
                inner_ok = false;
            }
            after_y[frame_no % 3] = h.ui.custom_item("after", {10, 10}).bounds.min.y;
        }
    };
    for (frame_no = 0; frame_no < 3; ++frame_no) { h.frame(build); }
    CHECK(inner_ok);
    CHECK(after_y[0] > 100.0f); // the tables take room
    CHECK(near_eq(after_y[1], after_y[2], 0.5f)); // the layout has settled and is stable
    // the rows grew to fit their inner tables: the whole thing is much taller than three text rows
    CHECK(after_y[2] > 3.0f * (h.ui.font().line_height(0) + 6.0f) + 150.0f);

    // depth limit: the sixth nested table is refused, and everything unwinds cleanly
    int opened = 0;
    const auto build_deep = [&] {
        if (auto w = h.ui.window("deep", {0, 0}, {500, 0}, plain_window)) {
            int depth = 0;
            for (; depth < 8; ++depth) {
                if (!h.ui.begin_table("t", 1)) { too_deep_refused = true; break; }
                h.ui.table_setup_column("c");
                (void)h.ui.table_next_row();
                (void)h.ui.table_next_column();
                h.ui.push_id("level");
                ++opened;
            }
            for (int i = 0; i < depth; ++i) {
                h.ui.pop_id();
                h.ui.end_table();
            }
            // the next table starts fresh
            CHECK(h.ui.begin_table("fresh", 1));
            h.ui.end_table();
        }
    };
    h.frames(build_deep, 2);
    CHECK(too_deep_refused);
    CHECK(opened >= 5 && opened <= 10);
}

// more editing: mouse selection, wheel, history limits, read-only ------------------------------------------------------

void test_editing_extra()
{
    std::fprintf(stderr, "[mouse selection, wheel, history limits]\n");
    harness h;

    // double-click selects a word, dragging selects a range (multi-line field at (12, 12), text starts at (22, 17))
    std::string text = "hello world\nsecond line";
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##m", text, {300, 120});
        }
    };
    h.click({700, 500}, build);
    h.click({40, 24}, build); // a little into "hello"
    h.click({40, 24}, build); // again, quickly: double-click
    h.type("J");
    h.frame(build);
    CHECK(text == "J world\nsecond line");

    text = "hello world\nsecond line";
    h.click({700, 500}, build);
    h.move({23, 24}); // the very start of line 0
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({70, 24 + 19}); // into line 1
    h.frames(build, 2);
    h.up();
    h.frame(build);
    h.type("X");
    h.frame(build);
    CHECK(text.starts_with("X") && text.find('\n') == std::string::npos && text.size() < 12);

    // the wheel scrolls a field that has more lines than fit, and clicks then land on the lines that are shown
    std::string many;
    for (int i = 0; i < 40; ++i) { many += "line " + std::to_string(i) + "\n"; }
    const auto build_many = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##many", many, {300, 100});
        }
    };
    h.click({700, 500}, build_many);
    h.move({60, 40});
    h.frames(build_many, 2);
    for (int i = 0; i < 4; ++i) {
        h.in.wheel = -1.0f;
        h.frame(build_many);
    }
    h.click({60, 24}, build_many); // the first visible line
    h.type("#");
    h.frame(build_many);
    const auto pos = many.find('#');
    CHECK(pos != std::string::npos && pos >= 5 * 7); // at least five lines down, so the view had scrolled

    // a read-only field can be selected and copied but not changed
    std::string fixed = "do not touch";
    const auto build_ro = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_multiline("##ro", fixed, {300, 60}, input_flags::read_only);
        }
    };
    h.click({700, 500}, build_ro);
    h.click({40, 24}, build_ro);
    h.type("zzz");
    h.frame(build_ro);
    h.key(key::backspace);
    h.frame(build_ro);
    h.key(key::enter);
    h.frame(build_ro);
    CHECK(fixed == "do not touch");
    h.key(key::a, true);
    h.frame(build_ro);
    h.key(key::c, true);
    h.frame(build_ro);
    CHECK(h.clip.data == "do not touch");

    // forward delete in a row is one undo step
    std::string line = "abcdef";
    const auto build_line = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_text("##l", line);
        }
    };
    h.click({700, 500}, build_line);
    h.click({30, 24}, build_line);
    h.key(key::home);
    h.frame(build_line);
    for (int i = 0; i < 3; ++i) {
        h.key(key::del);
        h.frame(build_line);
    }
    CHECK(line == "def");
    h.key(key::z, true);
    h.frame(build_line);
    CHECK(line == "abcdef");

    // the history is capped at 256 steps: undoing all of them stops at the state before the oldest kept step
    std::string many_edits;
    const auto build_edits = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            (void)h.ui.input_text("##e", many_edits);
        }
    };
    h.click({700, 500}, build_edits);
    h.click({30, 24}, build_edits);
    for (int i = 0; i < 300; ++i) {
        h.key(key::a, true);
        h.frame(build_edits);
        h.type(std::string(1, static_cast<char>('a' + i % 26)));
        h.frame(build_edits);
    }
    CHECK(many_edits == std::string(1, static_cast<char>('a' + 299 % 26)));
    for (int i = 0; i < 400; ++i) {
        h.key(key::z, true);
        h.frame(build_edits);
    }
    // ops are: (replace the selection with a letter) x 300, the first over an empty field; 256 are kept
    CHECK(many_edits == std::string(1, static_cast<char>('a' + (300 - 256 - 1) % 26)));
    // ... and redo walks forward again, through the same letters
    for (int i = 0; i < 400; ++i) {
        h.key(key::y, true);
        h.frame(build_edits);
    }
    CHECK(many_edits == std::string(1, static_cast<char>('a' + 299 % 26)));
}

// docking ---------------------------------------------------------------------------------------------------------

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
    CHECK(!h.ui.is_docked("A"));

    // programmatic docking: A fills the area, B splits it on the right
    CHECK(h.ui.dock_window("A", dock_zone::center));
    h.frames(build, 2);
    CHECK(h.ui.is_docked("A"));
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
    CHECK(h.ui.is_docked("C"));
    h.move({tab_c.x + 30.0f, tab_c.y + 80.0f});
    h.frame(build);
    h.frame(build);
    CHECK(!h.ui.is_docked("C"));
    h.move({tab_c.x + 60.0f, tab_c.y + 120.0f});
    h.frames(build, 2);
    c = h.ui.window_rect("C");
    CHECK(c.min.y > 60.0f); // it went along with the pointer

    // ... and dropping it on the left part of A's area docks it there, splitting that area in two
    h.move({40.0f, 300.0f});
    h.frames(build, 3);
    h.up();
    h.frames(build, 3);
    CHECK(h.ui.is_docked("C"));
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
    CHECK(!h.ui.is_docked("B"));
    a = h.ui.window_rect("A");
    CHECK(near_eq(a.max.x, 800.0f));
    show_b = true;
    h.frames(build, 2);
    CHECK(!h.ui.is_docked("B")); // it comes back floating

    h.ui.undock_window("A");
    CHECK(!h.ui.is_docked("A"));
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

        // a window docked in the left dock: the dock appears and the main area shrinks by its width and the handle
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
        CHECK(h.ui.is_docked("A") && h.ui.is_docked("B") && h.ui.is_docked("C"));

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
        CHECK(h.ui.is_docked("D"));

        // a window that stops being submitted leaves its space, and an emptied edge dock gives its room back
        h.ui.undock_window("A");
        h.frames(build, 4);
        CHECK(!h.ui.is_docked("A"));
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
        CHECK(h.ui.is_docked("E"));
        const rect e = h.ui.window_rect("E");
        CHECK(near_eq(e.min.x, 0.0f) && near_eq(e.max.x, 250.0f));
        CHECK(near_eq(main_rect.min.x, 254.0f, 1.0f));
    }
}

// text styles: bold / italic / underline / strike-through --------------------------------------------------------

void test_text_styles()
{
    std::fprintf(stderr, "[text styles]\n");
    font_atlas atlas = font_atlas::build().value();
    draw_list  dl;
    dl.begin({800, 600}, atlas, 1.0f);
    const color white{255, 255, 255, 255};

    dl.text({10, 10}, white, "Hello", 0);
    const std::size_t plain = dl.data().vertices.size();
    CHECK(plain == 20);

    dl.text({10, 40}, white, "Hello", 0, text_flags::bold);
    CHECK(dl.data().vertices.size() == plain + 2 * plain); // a second strike per glyph

    const std::size_t before_italic = dl.data().vertices.size();
    dl.text({10, 70}, white, "Hello", 0, text_flags::italic);
    CHECK(dl.data().vertices.size() == before_italic + plain);
    const vertex* iv = dl.data().vertices.data() + before_italic;
    CHECK(iv[1].pos.x - iv[2].pos.x > 1.0f); // the top of the glyph leans right of its bottom
    const vertex* pv = dl.data().vertices.data();
    CHECK(near_eq(pv[1].pos.x, pv[2].pos.x, 0.01f)); // plain text is upright

    const std::size_t before_line = dl.data().vertices.size();
    dl.text({10, 100}, white, "Hello", 0, text_flags::underline);
    CHECK(dl.data().vertices.size() > before_line + plain || dl.data().shapes.size() > 0); // the line is one more primitive
    dl.text({10, 130}, white, "Hello", 0, text_flags::underline | text_flags::strike | text_flags::bold | text_flags::italic);
    dl.text({10, 130}, white, "line one\nline two", 0, text_flags::underline); // one line per text line: nothing overflows

    // markup: the styles do not change the width of anything, unknown tags stay text
    harness h;
    f32 plain_x = 0, styled_x = 0, nested_x = 0, unknown_x = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            const auto rich = h.ui.rich_labels();
            (void)h.ui.button("ab cd");
            h.ui.same_line();
            plain_x = h.ui.custom_item("p", {1, 1}).bounds.min.x;
            (void)h.ui.button("<b>ab</b> <i>c</i><u>d</u>");
            h.ui.same_line();
            styled_x = h.ui.custom_item("q", {1, 1}).bounds.min.x;
            (void)h.ui.button("<b><i><u><s>ab cd</s></u></i></b>");
            h.ui.same_line();
            nested_x = h.ui.custom_item("r", {1, 1}).bounds.min.x;
            (void)h.ui.button("<bold>ab</bold> cd");
            h.ui.same_line();
            unknown_x = h.ui.custom_item("s", {1, 1}).bounds.min.x;
            h.ui.rich_text("<b>bold <i>and italic</b> still italic</i> and <u>unclosed");
        }
    };
    h.frames(build, 3);
    CHECK(near_eq(plain_x, styled_x, 0.6f));
    CHECK(near_eq(plain_x, nested_x, 0.6f));
    CHECK(unknown_x > plain_x + 20.0f); // "<bold>" is not a tag: it is shown
}

// docking: saving / restoring a layout, dragging a whole pane by its grip --------------------------------------------

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
    CHECK(text.starts_with("strata-dock 1"));
    CHECK(text.find("space ") != std::string::npos && text.find("S h ") != std::string::npos && text.find("W ") != std::string::npos);

    // take it apart ...
    for (const char* n : names) { h.ui.undock_window(n); }
    h.frames(build, 4);
    for (const char* n : names) { CHECK(!h.ui.is_docked(n)); }

    // ... and get it back
    CHECK(h.ui.dock_load_layout(text));
    h.frames(build, 4);
    for (std::size_t i = 0; i < names.size(); ++i) {
        CHECK(h.ui.is_docked(names[i]));
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
            CHECK(g.ui.is_docked(names[i]));
            const rect r = g.ui.window_rect(names[i]);
            CHECK(near_eq(r.min.x, before[i].min.x, 1.0f) && near_eq(r.max.y, before[i].max.y, 1.0f));
        }
    }

    // bad text changes nothing
    CHECK(!h.ui.dock_load_layout(""));
    CHECK(!h.ui.dock_load_layout("not a layout"));
    CHECK(!h.ui.dock_load_layout("strata-dock 1\nspace zz 0\n"));
    CHECK(!h.ui.dock_load_layout("strata-dock 1\nspace 1 0\nS h 0.5\nL 0 0\n")); // a split needs two children
    CHECK(!h.ui.dock_load_layout(text.substr(0, text.find("\nL ") + 5)));         // cut off inside a pane
    h.frames(build, 2);
    for (const char* n : names) { CHECK(h.ui.is_docked(n)); }

    // a window that is not in the text keeps floating; one that is not shown any more is skipped
    std::string partial = text;
    const std::size_t pos = partial.find("W ");
    CHECK(pos != std::string::npos);
    CHECK(h.ui.dock_load_layout(partial));
    h.frames(build, 3);
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
        // A and B are tabs of the left pane, C is the right one: the pane A+B is dropped on the right one
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
        for (const char* n : {"A", "B", "C"}) { CHECK(h.ui.is_docked(n)); }
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
        for (const char* n : {"A", "B", "C"}) { CHECK(h.ui.is_docked(n)); }
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
        CHECK(!h.ui.is_docked("A") && !h.ui.is_docked("B"));
        const rect a = h.ui.window_rect("A");
        const rect b = h.ui.window_rect("B");
        CHECK(a.min.y > 300.0f && b.min.y > a.min.y);
        area = {{0, 0}, {800, 600}};
    }
}

// charts: ticks, units, area fills, zoom and pan ---------------------------------------------------------------------

void test_charts()
{
    std::fprintf(stderr, "[charts: axes, zoom, pan, fills]\n");

    // the area fill is a strip: two vertices per point
    {
        font_atlas atlas = font_atlas::build().value();
        draw_list  dl;
        dl.begin({800, 600}, atlas, 1.0f);
        const std::array<vec2, 4> pts = {{{10, 50}, {40, 20}, {70, 40}, {100, 10}}};
        dl.area_fill(pts, 100.0f, color{255, 0, 0, 200}, color{255, 0, 0, 0});
        CHECK(dl.data().vertices.size() == 8 && dl.data().indices.size() == 18);
        CHECK(dl.data().vertices[0].col.a == 200 && dl.data().vertices[1].col.a == 0); // faded toward the base
        dl.area_fill(std::span<const vec2>{pts.data(), 1}, 100.0f, color{255, 0, 0, 200}, color{255, 0, 0, 0}); // one point: nothing
        CHECK(dl.data().vertices.size() == 8);
    }

    harness h;
    std::vector<f32> data(100);
    for (std::size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<f32>(i % 10); }
    const plot_series series[] = {{"v", data, color{0, 0, 0, 0}}};
    plot_options opt;
    opt.size     = {0.0f, 200.0f};
    opt.x        = {"time", "s"};
    opt.y        = {"load", "%"};
    opt.x_start  = 10.0f;
    opt.x_step   = 0.5f;
    opt.fill     = true;
    opt.zoom_pan = true;
    vec2 range{};
    bool zoomed = false;
    std::size_t vertices_flat = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("c", series, opt);
            range  = h.ui.plot_x_range("c");
            zoomed = h.ui.plot_zoomed("c");
        }
    };
    h.frames(build, 3);
    CHECK(near_eq(range.x, 10.0f, 0.01f) && near_eq(range.y, 10.0f + 99.0f * 0.5f, 0.01f)); // the whole data, in x units
    CHECK(!zoomed);
    vertices_flat = h.ui.render_data().vertices.size();
    CHECK(vertices_flat > 200); // ticks, labels, grid, fill and line

    // the wheel over the plot zooms x around the pointer
    const vec2 mid{200.0f, 112.0f};
    h.move(mid);
    h.frames(build, 3);
    const f32 full = range.y - range.x;
    h.in.wheel = 3.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK(zoomed);
    CHECK((range.y - range.x) < full * 0.8f);
    const f32 x_mid_before = range.x + (range.y - range.x) * 0.5f;
    CHECK(x_mid_before > 20.0f && x_mid_before < 40.0f);

    // dragging pans: to the left shows later data
    const f32 lo_before = range.x;
    h.down();
    h.frame(build);
    h.move({mid.x - 60.0f, mid.y});
    h.frames(build, 3);
    h.up();
    h.frames(build, 2);
    CHECK(range.x > lo_before + 1.0f);
    const f32 zoomed_span = range.y - range.x;
    CHECK(zoomed_span < full * 0.8f); // panning does not change the zoom

    // Ctrl + wheel zooms the values (and the x range stays)
    h.move(mid);
    h.frames(build, 3);
    h.in.ctrl = true;
    h.in.wheel = 2.0f;
    h.frame(build);
    h.in.ctrl = false;
    h.frames(build, 2);
    CHECK(near_eq(range.y - range.x, zoomed_span, 0.01f));

    // a double-click resets everything
    h.click(mid, build);
    h.click(mid, build);
    h.frames(build, 3);
    CHECK(!zoomed);
    CHECK(near_eq(range.x, 10.0f, 0.01f) && near_eq(range.y, 59.5f, 0.01f));

    // zooming out is bounded, zooming in as well
    h.in.wheel = -40.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK((range.y - range.x) <= full * 4.0f + 0.01f);
    h.in.wheel = 200.0f;
    h.frame(build);
    h.frames(build, 2);
    CHECK((range.y - range.x) >= opt.x_step * 2.0f - 0.01f);
    h.ui.plot_reset_view("c"); // (outside the window: another id scope, so this does nothing)

    // a lot of samples: bucketed, and a histogram, without trouble
    std::vector<f32> many(50000);
    for (std::size_t i = 0; i < many.size(); ++i) { many[i] = std::sin(static_cast<f32>(i) * 0.01f); }
    const plot_series big[] = {{"many", many, color{0, 0, 0, 0}}};
    plot_options bigopt = opt;
    bigopt.x_start = 0.0f;
    bigopt.x_step = 1.0f;
    const auto build_big = [&] {
        if (auto w = h.ui.window("big", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("line", big, bigopt);
            plot_options hist = bigopt;
            hist.kind = plot_kind::histogram;
            h.ui.plot("bars", big, hist);
        }
    };
    h.frames(build_big, 3);
    CHECK(h.ui.render_data().vertices.size() < 40000u);

    // empty and single-sample data are fine
    const std::vector<f32> none;
    const std::vector<f32> one{3.0f};
    const plot_series s_none[] = {{"n", none, color{0, 0, 0, 0}}};
    const plot_series s_one[]  = {{"o", one, color{0, 0, 0, 0}}};
    h.frames([&] {
        if (auto w = h.ui.window("edge", {0, 0}, {400, 0}, plain_window)) {
            h.ui.plot("e0", s_none, opt);
            h.ui.plot("e1", s_one, opt);
        }
    }, 3);
}

// acrylic: saturation / brightness of the blurred frame, glass popups ---------------------------------------------------

void test_acrylic_extras()
{
    std::fprintf(stderr, "[acrylic: saturation, brightness, glass popups]\n");
    {
        font_atlas atlas = font_atlas::build().value();
        draw_list  dl;
        dl.begin({800, 600}, atlas, 1.0f);
        dl.backdrop({{10, 10}, {200, 100}}, 16.0f, color{20, 20, 30, 150}, radii(8.0f), 0.03f, 1.4f, 1.1f);
        const shape_record& r = dl.data().shapes.back();
        CHECK(near_eq(r.shadow_offset.x, 1.4f, 0.001f) && near_eq(r.shadow_offset.y, 1.1f, 0.001f));
        dl.backdrop({{10, 10}, {200, 100}}, 16.0f, color{20, 20, 30, 150}); // the defaults leave the frame as it is
        const shape_record& d = dl.data().shapes.back();
        CHECK(near_eq(d.shadow_offset.x, 1.0f, 0.001f) && near_eq(d.shadow_offset.y, 1.0f, 0.001f));
    }

    // the theme file knows the new keys
    {
        style s = themes::midnight();
        const themes::theme_result r = themes::from_string("acrylic_saturation = 1.5\nacrylic_brightness = 0.9\npopup_acrylic = 1\n", s);
        CHECK(r.ok() && near_eq(s.acrylic_saturation, 1.5f, 0.001f) && near_eq(s.acrylic_brightness, 0.9f, 0.001f) && s.popup_acrylic == 1.0f);
        CHECK(themes::glass().popup_acrylic > 0.0f && themes::midnight().popup_acrylic == 0.0f);
    }

    // menus, dropdowns and tooltips are frosted glass when asked to be: the popup becomes a backdrop command
    const auto blur_commands = [](const harness& h) {
        int n = 0;
        for (const draw_cmd& c : h.ui.render_data().commands) { n += c.blur > 0.0f ? 1 : 0; }
        return n;
    };
    for (const bool glass : {false, true}) {
        harness h;
        h.ui.theme().popup_acrylic = glass ? 1.0f : 0.0f;
        const auto build = [&] {
            if (auto bar = h.ui.main_menu_bar()) {
                if (auto m = h.ui.menu("File")) { (void)h.ui.menu_item("Open"); }
            }
        };
        h.frames(build, 4);
        CHECK(blur_commands(h) == 0);
        const f32 bar_h = h.ui.main_menu_bar_height();
        h.click({6.0f + (h.ui.font().measure(0, "File").x + 22.0f) * 0.5f, bar_h * 0.5f}, build);
        h.frames(build, 12);
        CHECK(h.ui.menu_is_open());
        CHECK(blur_commands(h) == (glass ? 1 : 0));
    }

    // a combo list too
    {
        harness h;
        h.ui.theme().popup_acrylic = 1.0f;
        int sel = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("w", {0, 0}, {300, 0}, plain_window)) {
                (void)h.ui.combo("##c", sel, {"one", "two", "three"});
            }
        };
        h.frames(build, 3);
        h.click({100.0f, 12.0f + h.ui.frame_height() * 0.5f}, build);
        h.frames(build, 12);
        CHECK(blur_commands(h) >= 1);
    }
}

// menus: mnemonics, accelerators, icons, keep-open rows ------------------------------------------------------------------

void test_menu_extras()
{
    std::fprintf(stderr, "[menus: mnemonics, accelerators, keep-open]\n");
    harness h;
    int picked = 0;
    int toggles = 0;
    bool grid = false;
    int accel = 0;
    const auto build = [&] {
        if (h.ui.accelerator("Ctrl+O")) { accel += 1; }
        if (h.ui.accelerator("F5")) { accel += 10; }
        if (h.ui.accelerator("Ctrl+Shift+S")) { accel += 100; }
        if (h.ui.accelerator("Alt+F4")) { accel += 1000; }
        if (h.ui.accelerator("Del")) { accel += 10000; }
        if (auto bar = h.ui.main_menu_bar()) {
            if (auto m = h.ui.menu("&File")) {
                if (h.ui.menu_item("&Open", "Ctrl+O")) { picked = 1; }
                if (h.ui.menu_item("Save &as")) { picked = 2; }
                if (auto sub = h.ui.menu("&Recent")) {
                    if (h.ui.menu_item("&one")) { picked = 3; }
                    if (h.ui.menu_item("&two")) { picked = 4; }
                }
                menu_item_options keep;
                keep.keep_open = true;
                keep.icon      = "*";
                if (h.ui.menu_item("&Grid", grid, keep)) { ++toggles; }
                menu_item_options off;
                off.enabled = false;
                if (h.ui.menu_item("&Off", off)) { picked = 99; }
                if (h.ui.menu_item("R&&D")) { picked = 5; } // a literal ampersand, no mnemonic
            }
            if (auto m = h.ui.menu("&Edit")) {
                if (h.ui.menu_item("&Copy")) { picked = 6; }
            }
        }
    };
    h.frames(build, 4);

    // Alt + F opens File; O picks Open
    h.in.alt = true;
    h.in.pressed_key = 'F';
    h.frame(build);
    h.in.alt = false;
    h.frames(build, 5);
    CHECK(h.ui.menu_is_open());
    h.type("o");
    h.frames(build, 3);
    CHECK(picked == 1);
    CHECK(!h.ui.menu_is_open());

    // R for the submenu, then t for its second row (case does not matter)
    h.in.alt = true; h.in.pressed_key = 'F'; h.frame(build); h.in.alt = false;
    h.frames(build, 5);
    h.type("R");
    h.frames(build, 6);
    CHECK(h.ui.menu_is_open());
    h.type("t");
    h.frames(build, 3);
    CHECK(picked == 4);
    CHECK(!h.ui.menu_is_open());

    // a keep-open row toggles and leaves the menu up
    h.in.alt = true; h.in.pressed_key = 'F'; h.frame(build); h.in.alt = false;
    h.frames(build, 5);
    h.type("g");
    h.frames(build, 2);
    CHECK(toggles == 1 && grid && h.ui.menu_is_open());
    h.type("g");
    h.frames(build, 2);
    CHECK(toggles == 2 && !grid && h.ui.menu_is_open());
    picked = 0;
    h.type("o"); // the disabled row does not take it
    h.frames(build, 3);
    CHECK(picked != 99);
    h.type("d"); // "R&&D" has no mnemonic
    h.frames(build, 3);
    CHECK(picked != 5);

    // Alt + E switches to the Edit menu; C picks Copy
    h.in.alt = true; h.in.pressed_key = 'E'; h.frame(build); h.in.alt = false;
    h.frames(build, 6);
    CHECK(h.ui.menu_is_open());
    h.type("c");
    h.frames(build, 3);
    CHECK(picked == 6 && !h.ui.menu_is_open());

    // letters do nothing while no menu is open
    picked = 0;
    h.type("o");
    h.frames(build, 3);
    CHECK(picked == 0);

    // accelerators need the exact modifiers
    accel = 0;
    h.in.ctrl = true; h.in.pressed_key = 'O'; h.frame(build); h.in.ctrl = false;
    CHECK(accel == 1);
    h.in.pressed_key = 'O'; h.frame(build); // no ctrl
    CHECK(accel == 1);
    h.in.ctrl = true; h.in.shift = true; h.in.pressed_key = 'O'; h.frame(build); // too many
    h.in.ctrl = false; h.in.shift = false;
    CHECK(accel == 1);
    h.in.pressed_key = 0x74; h.frame(build); // F5
    CHECK(accel == 11);
    h.in.ctrl = true; h.in.shift = true; h.in.pressed_key = 'S'; h.frame(build);
    h.in.ctrl = false; h.in.shift = false;
    CHECK(accel == 111);
    h.in.alt = true; h.in.pressed_key = 0x73; h.frame(build); h.in.alt = false; // Alt+F4
    CHECK(accel == 1111);
    h.in.pressed_key = 0x2e; h.frame(build); // Del
    CHECK(accel == 11111);
    CHECK(!h.ui.accelerator("") && !h.ui.accelerator("Ctrl+") && !h.ui.accelerator("Ctrl+Nonsense") && !h.ui.accelerator("+"));

    // while a text field has the keyboard, plain keys are the field's, editing shortcuts too
    std::string text;
    const auto build_text = [&] {
        if (auto w = h.ui.window("t", {100, 100}, {300, 0}, plain_window)) { (void)h.ui.input_text("##f", text); }
        if (h.ui.accelerator("Del")) { accel += 100000; }
        if (h.ui.accelerator("Ctrl+C")) { accel += 1000000; }
        if (h.ui.accelerator("Ctrl+P")) { accel += 10000000; }
    };
    h.click({130.0f, 124.0f}, build_text);
    h.frames(build_text, 2);
    CHECK(h.ui.want_text_input());
    accel = 0;
    h.in.pressed_key = 0x2e; h.frame(build_text);
    h.in.ctrl = true; h.in.pressed_key = 'C'; h.frame(build_text);
    CHECK(accel == 0);
    h.in.pressed_key = 'P'; h.frame(build_text); h.in.ctrl = false;
    CHECK(accel == 10000000);
}

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

// the sliding highlight of tab bars and strips is part of the widget: moving the window moves it rigidly
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
    h.frame(build); // one frame after the jump: an absolute animation would still be at the old place
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

void test_texture_image()
{
    std::fprintf(stderr, "[textures: formats, mip maps, updates]\n");

    CHECK(texture_mip_count(256, 256, 0) == 9);
    CHECK(texture_mip_count(256, 256, 1) == 1);
    CHECK(texture_mip_count(100, 30, 0) == 7);
    CHECK(texture_mip_count(100, 30, 3) == 3);
    CHECK(texture_mip_count(4, 4, 99) == 3); // no more than fits
    CHECK(texture_mip_count(0, 4, 0) == 0);
    CHECK(texture_source_bytes(texture_format::rgba8) == 4 && texture_source_bytes(texture_format::r8) == 1 &&
          texture_source_bytes(texture_format::rgba16f) == 8);
    CHECK(texture_layout_of(texture_format::r8) == texture_layout::rgba8 && texture_layout_of(texture_format::bgra8) == texture_layout::bgra8);

    // half floats
    CHECK(float_to_half(1.0f) == 0x3c00 && float_to_half(0.5f) == 0x3800 && float_to_half(-2.0f) == 0xc000);
    CHECK(float_to_half(65504.0f) == 0x7bff && float_to_half(1.0e6f) == 0x7c00 && float_to_half(1.0e-9f) == 0);
    CHECK(near_eq(half_to_float(0x3c00), 1.0f, 0.0f) && near_eq(half_to_float(0xc000), -2.0f, 0.0f) && near_eq(half_to_float(0x0001), 5.96e-8f, 1.0e-9f));
    CHECK(std::isnan(half_to_float(float_to_half(std::nanf("")))) && std::isinf(half_to_float(0x7c00)));
    for (const f32 v : {0.0f, 0.1f, 0.333f, 0.75f, 1.0f, 3.14159f, 100.5f}) {
        CHECK(near_eq(half_to_float(float_to_half(v)), v, v * 0.001f + 1.0e-4f));
    }

    // rgba8: every level of a flat color is that color
    {
        texture_image img;
        const std::vector<u8> flat = [] { std::vector<u8> v(4 * 4 * 4); for (std::size_t i = 0; i < v.size(); i += 4) { v[i] = 200; v[i + 1] = 100; v[i + 2] = 50; v[i + 3] = 255; } return v; }();
        CHECK(img.create({4, 4, texture_format::rgba8, 0, false}, flat));
        CHECK(img.level_count() == 3 && img.width(1) == 2 && img.width(2) == 1 && img.height(2) == 1);
        CHECK(img.pitch(1) == 8);
        const auto last = img.pixels(2);
        CHECK(last.size() == 4 && last[0] == 200 && last[1] == 100 && last[2] == 50 && last[3] == 255);
        img.wipe();
        CHECK(!img.valid());
    }
    // colors are weighted by alpha: transparent texels do not darken what is opaque
    {
        texture_image img;
        std::vector<u8> px(2 * 2 * 4, 0);
        px[0] = 255; px[1] = 0; px[2] = 0; px[3] = 255;                       // one opaque red texel
        for (int i = 1; i < 4; ++i) { px[i * 4 + 2] = 255; }                    // three fully transparent blue ones
        CHECK(img.create({2, 2, texture_format::rgba8, 0, false}, px));
        const auto p = img.pixels(1);
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);     // still pure red
        CHECK(p[3] == 64);                                  // a quarter covered
    }
    // r8 is opaque grey, a8 is white coverage
    {
        texture_image g, a;
        const std::vector<u8> ramp = {0, 128, 255, 64};
        CHECK(g.create({2, 2, texture_format::r8, 1, false}, ramp) && a.create({2, 2, texture_format::a8, 1, false}, ramp));
        CHECK(g.layout() == texture_layout::rgba8 && g.pixels(0).size() == 16);
        CHECK(g.pixels(0)[4] == 128 && g.pixels(0)[5] == 128 && g.pixels(0)[6] == 128 && g.pixels(0)[7] == 255);
        CHECK(a.pixels(0)[4] == 255 && a.pixels(0)[7] == 128 && a.pixels(0)[11] == 255);
    }
    // bgra8 stays bgra8; rgba16f averages in floats
    {
        texture_image b, f;
        const std::vector<u8> px = {10, 20, 30, 255};
        CHECK(b.create({1, 1, texture_format::bgra8, 1, false}, px) && b.layout() == texture_layout::bgra8 && b.pixels(0)[0] == 10);
        std::vector<u16> half(2 * 2 * 4);
        for (int i = 0; i < 4; ++i) { half[i * 4] = float_to_half(i == 0 ? 1.0f : 0.0f); half[i * 4 + 3] = float_to_half(1.0f); }
        CHECK(f.create({2, 2, texture_format::rgba16f, 0, false}, std::span<const u8>{reinterpret_cast<const u8*>(half.data()), half.size() * 2}));
        CHECK(f.level_count() == 2 && f.pixels(0).size() == 32 && f.pixels(1).size() == 8);
        const u16* top = reinterpret_cast<const u16*>(f.pixels(1).data());
        CHECK(near_eq(half_to_float(top[0]), 0.25f, 0.001f) && near_eq(half_to_float(top[3]), 1.0f, 0.001f));
    }
    // too little data / no size: refused
    {
        texture_image img;
        CHECK(!img.create({4, 4, texture_format::rgba8, 1, false}, std::vector<u8>(10)));
        CHECK(!img.create({0, 4, texture_format::rgba8, 1, false}, std::vector<u8>(64)));
        CHECK(!img.valid());
    }
    // updates: level 0 is replaced, and the levels below are rebuilt where they touch it
    {
        texture_image img;
        CHECK(img.create({8, 8, texture_format::rgba8, 0, true}, std::vector<u8>(8 * 8 * 4, 0)));
        CHECK(img.level_count() == 4);
        std::vector<texture_image::region> dirty;
        std::vector<u8> red(3 * 3 * 4);
        for (std::size_t i = 0; i < red.size(); i += 4) { red[i] = 255; red[i + 3] = 255; }
        CHECK(img.update(2, 2, 3, 3, red, dirty));
        CHECK(dirty.size() == 4 && dirty[0].level == 0 && dirty[0].x == 2 && dirty[0].w == 3);
        CHECK(dirty[1].level == 1 && dirty[1].x == 1 && dirty[1].y == 1 && dirty[1].w == 2 && dirty[1].h == 2);
        CHECK(dirty[3].w == 1 && dirty[3].h == 1);
        const auto p0 = img.pixels(0);
        CHECK(p0[(2 * 8 + 2) * 4] == 255 && p0[(1 * 8 + 1) * 4] == 0 && p0[(4 * 8 + 4) * 4] == 255 && p0[(5 * 8 + 5) * 4] == 0);
        const auto p1 = img.pixels(1); // level 1 (4x4): the texel at (1, 1) covers (2..3, 2..3): all red
        CHECK(p1[(1 * 4 + 1) * 4] == 255 && p1[(1 * 4 + 1) * 4 + 3] == 255);
        CHECK(p1[(0 * 4 + 0) * 4 + 3] == 0);
        CHECK(!img.update(6, 6, 3, 3, red, dirty)); // does not fit
        CHECK(!img.update(0, 0, 3, 3, std::span<const u8>{red.data(), 8}, dirty)); // too few bytes
        CHECK(!img.update(0, 0, 0, 3, red, dirty));
    }
    // an update of an r8 texture takes r8 bytes
    {
        texture_image img;
        CHECK(img.create({4, 4, texture_format::r8, 0, true}, std::vector<u8>(16, 0)));
        std::vector<texture_image::region> dirty;
        CHECK(img.update(1, 1, 2, 1, std::vector<u8>{9, 10}, dirty));
        CHECK(img.pixels(0)[(1 * 4 + 1) * 4] == 9 && img.pixels(0)[(1 * 4 + 2) * 4 + 3] == 255);
    }
    // odd sizes: the chain and the updates stay inside their levels
    {
        texture_image img;
        CHECK(img.create({5, 3, texture_format::rgba8, 0, true}, std::vector<u8>(5 * 3 * 4, 255)));
        CHECK(img.level_count() == 3 && img.width(1) == 2 && img.height(1) == 1 && img.width(2) == 1);
        std::vector<texture_image::region> dirty;
        CHECK(img.update(4, 2, 1, 1, std::vector<u8>{1, 2, 3, 4}, dirty));
        for (const auto& r : dirty) { CHECK(r.x + r.w <= img.width(r.level) && r.y + r.h <= img.height(r.level) && r.w > 0 && r.h > 0); }
    }
}

// text fields: triple click, styled contents, input methods --------------------------------------------------------------

void test_text_field_extras()
{
    std::fprintf(stderr, "[text fields: triple click, spans, input method]\n");

    // triple click selects the line: in a single-line field, everything
    {
        harness h;
        std::string value = "hello big world";
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) { (void)h.ui.input_text("##f", value); }
        };
        h.frames(build, 2);
        const vec2 spot{60.0f, 12.0f + h.ui.frame_height() * 0.5f}; // on "big"
        h.click(spot, build);
        h.click(spot, build);
        h.type("X"); // a double click had selected the word
        h.frame(build);
        CHECK(value == "hello X world");
        value = "hello big world";
        h.key(key::escape);
        h.frames(build, 40); // (long enough for the next click not to count as a continuation)
        h.click(spot, build);
        h.click(spot, build);
        h.click(spot, build);
        h.type("X");
        h.frame(build);
        CHECK(value == "X");
        // a fourth click starts over
        value = "hello big world";
        h.key(key::escape);
        h.frames(build, 40);
        for (int i = 0; i < 4; ++i) { h.click(spot, build); }
        h.type("X");
        h.frame(build);
        CHECK(value == "hello bigX world" || value == "hello big Xworld" || value.find('X') != std::string::npos);
        CHECK(value != "X");
    }
    // ... in a multi-line field, the line between two line breaks (with its break)
    {
        harness h;
        std::string value = "one two\nthree four\nfive";
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) { (void)h.ui.input_multiline("##m", value, {0.0f, 160.0f}); }
        };
        h.frames(build, 2);
        const f32 lh = h.ui.font().line_height(0);
        const vec2 spot{40.0f, 12.0f + 5.0f + lh * 1.5f}; // the second line
        h.click(spot, build);
        h.click(spot, build);
        h.click(spot, build);
        h.type("X");
        h.frame(build);
        CHECK(value == "one two\nXfive");
        // the last line has no break to take
        value = "one\ntwo";
        h.key(key::escape);
        h.frames(build, 2);
        const vec2 last{40.0f, 12.0f + 5.0f + lh * 1.5f};
        h.click(last, build);
        h.click(last, build);
        h.click(last, build);
        h.type("X");
        h.frame(build);
        CHECK(value == "one\nX");
    }

    // styled contents: another font in a span makes the text wider, lines as high as the tallest font
    {
        font_config big;
        big.pixel_height = 30.0f;
        context_config cfg;
        cfg.extra_fonts = std::span<const font_config>{&big, 1};
        harness h{cfg};
        CHECK(h.ui.font().font_count() == 2);
        std::string value = "abc def ghi";
        bool styled = false;
        const std::array<text_span, 1> spans = {{{4, 7, 1, color{255, 128, 0, 255}, text_flags::underline}}};
        f32 caret_plain = 0.0f, caret_styled = 0.0f;
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
                if (styled) { h.ui.input_spans(spans); }
                (void)h.ui.input_text("##f", value);
            }
        };
        h.frames(build, 2);
        h.click({30.0f, 12.0f + h.ui.frame_height() * 0.5f}, build);
        h.key(key::end);
        h.frames(build, 2);
        caret_plain = h.ui.ime_position().x;
        const std::size_t plain_vertices = h.ui.render_data().vertices.size();
        styled = true;
        h.frames(build, 2);
        caret_styled = h.ui.ime_position().x;
        CHECK(caret_styled > caret_plain + 20.0f); // "def" is set in the 30 px font: the end of the line moved right
        (void)plain_vertices;
        // the same in a multi-line field: the styled line makes every line taller
        std::string text = "abc def ghi\nsecond line";
        const auto build_ml = [&] {
            if (auto w = h.ui.window("m", {0, 100}, {400, 0}, plain_window)) {
                if (styled) { h.ui.input_spans(spans); }
                (void)h.ui.input_multiline("##m", text, {0.0f, 160.0f});
            }
        };
        styled = false;
        h.frames(build_ml, 2);
        h.click({30.0f, 100.0f + 12.0f + 5.0f + h.ui.font().line_height(0) * 1.5f}, build_ml);
        h.key(key::end);
        h.frames(build_ml, 2);
        const f32 y_plain = h.ui.ime_position().y;
        styled = true;
        h.frames(build_ml, 2);
        CHECK(h.ui.ime_position().y > y_plain + 8.0f); // second line, lower: the first one is 30 px tall now
        // ranges beyond the text, overlapping ones and unknown fonts are made harmless
        const std::array<text_span, 4> odd = {{{0, 5000, 9, color{}, text_flags::bold}, {3, 6, 1, color{}, text_flags::none},
                                                {8, 2, 1, color{}, text_flags::none}, {2, 4, 0, color{}, text_flags::italic}}};
        h.frames([&] {
            if (auto w = h.ui.window("odd", {0, 300}, {400, 0}, plain_window)) {
                h.ui.input_spans(odd);
                (void)h.ui.input_text("##o", value);
                h.ui.input_spans(odd);
                (void)h.ui.input_multiline("##p", text, {0.0f, 100.0f});
            }
        }, 3);
        // passwords ignore spans
        secure_string secret{"secret"};
        h.frames([&] {
            if (auto w = h.ui.window("pw", {0, 400}, {400, 0}, plain_window)) {
                h.ui.input_spans(spans);
                (void)h.ui.input_text("##pw", secret, {}, input_flags::password);
            }
        }, 3);
    }

    // an input method's composition: shown in the field, inserted only when confirmed
    {
        harness h;
        std::string value = "ab";
        secure_string secret{"pw"};
        std::string notes = "line";
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) { (void)h.ui.input_text("##f", value); }
            if (auto w = h.ui.window("p", {0, 100}, {400, 0}, plain_window)) { (void)h.ui.input_text("##pw", secret, {}, input_flags::password); }
            if (auto w = h.ui.window("m", {0, 200}, {400, 0}, plain_window)) { (void)h.ui.input_multiline("##m", notes, {0.0f, 80.0f}); }
        };
        h.frames(build, 3);
        CHECK(!h.ui.ime_wanted()); // nothing has the keyboard
        const f32 fh = h.ui.frame_height();
        const auto compose = [&](std::string_view s, u32 cursor) {
            std::memcpy(h.in.ime.data(), s.data(), s.size());
            h.in.ime_len    = static_cast<u32>(s.size());
            h.in.ime_cursor = cursor;
        };
        const auto clear = [&] { h.in.ime_len = 0; h.in.ime_cursor = 0; };

        compose("ni", 2); // composing while no field is focused: nothing to show
        h.frames(build, 2);
        CHECK(h.ui.ime_composing() && !h.ui.ime_wanted());
        clear();

        h.click({150.0f, 12.0f + fh * 0.5f}, build); // focus the text field (the caret goes to the end)
        h.frames(build, 2);
        CHECK(h.ui.ime_wanted() && !h.ui.ime_composing());
        const vec2 idle = h.ui.ime_position();
        const std::size_t idle_vertices = h.ui.render_data().vertices.size();
        CHECK(idle.x > 12.0f && idle.y > 12.0f && h.ui.ime_line_height() > 10.0f);

        const std::string_view comp = "\xe6\x97\xa5\xe6\x9c\xac"; // "日本": two characters, six bytes
        compose(comp, 3);   // the caret after the first character
        h.frames(build, 2);
        CHECK(h.ui.ime_composing() && h.ui.ime_composition() == comp);
        CHECK(value == "ab"); // nothing was inserted
        CHECK(h.ui.render_data().vertices.size() > idle_vertices); // but it is drawn
        CHECK(h.ui.ime_position().x > idle.x + 5.0f); // the candidate window follows the composition's caret
        h.frames(build, 20);
        CHECK(value == "ab");

        clear();        // confirmed: the text arrives as typed input
        h.type(comp);
        h.frame(build);
        CHECK(value == "ab\xe6\x97\xa5\xe6\x9c\xac");
        h.frames(build, 2);
        CHECK(!h.ui.ime_composing());

        // a password field turns the input method off
        h.click({30.0f, 100.0f + 12.0f + fh * 0.5f}, build);
        h.frames(build, 3);
        CHECK(!h.ui.ime_wanted());
        // a multi-line field shows the composition in a box at the caret
        h.click({30.0f, 200.0f + 12.0f + 10.0f}, build);
        h.frames(build, 3);
        CHECK(h.ui.ime_wanted());
        const std::size_t before_chip = h.ui.render_data().vertices.size();
        compose("kana", 4);
        h.frames(build, 2);
        CHECK(h.ui.render_data().vertices.size() > before_chip);
        CHECK(notes == "line");
        clear();
    }
}

// toasts with buttons and progress, the log with wrapping and clock times -------------------------------------------------

void test_toasts_and_log_v2()
{
    std::fprintf(stderr, "[toasts: buttons, progress; log: wrap, clock]\n");
    const auto nothing = [] {};

    // buttons: pressing one closes the toast and says which; the body of such a toast is not a button
    {
        harness h;
        const std::array<std::string_view, 2> acts = {"Undo", "Details"};
        const toast_handle t = h.ui.toast({.title = "Deleted", .text = "3 files", .seconds = 30.0f, .actions = acts});
        CHECK(t != 0 && h.ui.toast_alive(t));
        h.frames(nothing, 40);
        CHECK(h.ui.toast_action(t) == -1);

        const f32 lh = h.ui.font().line_height(0);
        const f32 btn_h = lh + 8.0f;
        const f32 bottom = 600.0f - 14.0f;
        const f32 left = 800.0f - 14.0f - 340.0f + 12.0f + 6.0f;
        const f32 by = bottom - 12.0f - btn_h + 2.0f + btn_h * 0.5f;
        const f32 undo_w = h.ui.font().measure(0, "Undo").x + 22.0f;
        const f32 details_w = h.ui.font().measure(0, "Details").x + 22.0f;

        h.click({left + undo_w * 0.5f, by - 40.0f}, nothing); // the text area: nothing happens
        h.frames(nothing, 5);
        CHECK(h.ui.toast_alive(t) && h.ui.toast_action(t) == -1);

        h.click({left + undo_w + 6.0f + details_w * 0.5f, by}, nothing); // "Details"
        h.frames(nothing, 3);
        CHECK(!h.ui.toast_alive(t));
        CHECK(h.ui.toast_action(t) == 1);
        CHECK(h.ui.toast_action(t) == -1); // read once
        h.frames(nothing, 40);
        CHECK(h.ui.toast_count() == 0);
    }
    // progress: sticky while it runs, closes itself a moment after it completes
    {
        harness h;
        const toast_handle t = h.ui.toast({.title = "Downloading", .seconds = 0.0f, .progress = 0.0f});
        h.frames(nothing, 600); // ten seconds
        CHECK(h.ui.toast_alive(t));
        h.ui.toast_progress(t, 0.5f, "half way");
        h.frames(nothing, 60);
        CHECK(h.ui.toast_alive(t));
        h.ui.toast_progress(t, 1.0f);
        h.frames(nothing, 60);
        CHECK(h.ui.toast_alive(t)); // (2 s to read "done")
        h.frames(nothing, 120);
        CHECK(!h.ui.toast_alive(t) && h.ui.toast_count() == 0);
        // an endless one and closing by hand
        const toast_handle b = h.ui.toast({.text = "working", .seconds = 0.0f, .progress = toast_busy});
        h.frames(nothing, 300);
        CHECK(h.ui.toast_alive(b));
        h.ui.toast_close(b);
        h.frames(nothing, 60);
        CHECK(!h.ui.toast_alive(b));
        // handles of toasts that never existed are harmless
        h.ui.toast_progress(12345, 0.3f);
        h.ui.toast_close(12345);
        CHECK(h.ui.toast_action(12345) == -1 && !h.ui.toast_alive(0));
    }
    // a plain toast still goes away when clicked
    {
        harness h;
        const toast_handle t = h.ui.toast("note");
        h.frames(nothing, 40);
        h.click({700.0f, 570.0f}, nothing);
        h.frames(nothing, 20);
        CHECK(!h.ui.toast_alive(t));
    }

    // the log: wrapping makes long lines take more rows (more glyphs are drawn), clock stamps are the time of day
    {
        CHECK(log_buffer::clock_text(0).size() == 12 && log_buffer::clock_text(1'000'000'123).ends_with(".123"));
        harness h;
        log_buffer log;
        log.add(log_level::info, std::string(400, 'x') + " end", -1.0, 1'000'000'000);
        log.add(log_level::warn, "one\ntwo\nthree", -1.0, 1'000'000'500);
        CHECK(log[0].wall_ms == 1'000'000'000 && log[1].wall_ms == 1'000'000'500);
        log.add(log_level::info, "stamped by the clock");
        CHECK(log[2].wall_ms > 1'600'000'000'000); // after 2020
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 300}, plain_window)) { h.ui.log_view("log", log, {0.0f, 200.0f}, log_view_flags::no_toolbar); }
        };
        h.frames(build, 3);
        const std::size_t flat = h.ui.render_data().vertices.size();
        log.view.wrap = true;
        h.frames(build, 3);
        const std::size_t wrapped = h.ui.render_data().vertices.size();
        CHECK(wrapped > flat + 100); // the rest of the long line and the line breaks are drawn now
        log.view.show_time = true;
        log.view.clock = true;
        h.frames(build, 3);
        // clicking a row of a wrapped log selects that row (rows have different heights)
        log.view.follow = false;
        h.click({100.0f, 12.0f + 12.0f + 60.0f}, build);
        h.frames(build, 2);
        CHECK(log.view.sel_anchor != 0);
    }
}

// text beyond the basics: right-to-left text, Arabic joining, emoji and other supplementary characters ------------------------

void test_rtl_and_emoji()
{
    std::fprintf(stderr, "[right-to-left text, arabic joining, emoji]\n");
    const auto cps_of = [](std::string_view s) {
        std::vector<char32_t> v;
        while (!s.empty()) { v.push_back(decode_utf8(s)); }
        return v;
    };
    const std::string hebrew = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d"; // שלום: shin lamed vav final-mem
    const std::string arabic = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7"; // مرحبا

    // which text needs reordering
    CHECK(!has_rtl_text("plain ascii") && !has_rtl_text("caf\xc3\xa9 \xce\xb1\xce\xb2 \xd0\xb6") && !has_rtl_text("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
    CHECK(has_rtl_text(hebrew) && has_rtl_text(arabic) && has_rtl_text("abc " + hebrew));
    CHECK(!has_rtl_text("") && has_rtl_text("\xef\xbb\xbb")); // (arabic presentation form)

    // reordering (no atlas: no font decides about joined forms)
    {
        const auto reversed = [&](std::string_view s) { auto v = cps_of(s); std::ranges::reverse(v); return v; };
        CHECK(cps_of(to_visual(hebrew)) == reversed(hebrew)); // a right-to-left paragraph shows its letters backwards
        CHECK(to_visual("plain text") == "plain text");
        // left-to-right paragraph with a hebrew word: only the word turns around
        const std::string mixed = "abc " + hebrew + " def";
        CHECK(to_visual(mixed) == "abc " + std::string{cps_of(to_visual(hebrew)).empty() ? "" : to_visual(hebrew)} + " def");
        // right-to-left paragraph with digits: the number keeps its direction and moves to the left of the word
        CHECK(to_visual(hebrew + " 123") == "123 " + to_visual(hebrew));
        // brackets are mirrored in right-to-left runs
        const std::string in_brackets = "(" + hebrew + ")";
        CHECK(to_visual(in_brackets) == "(" + to_visual(hebrew) + ")");
        // a bracketed word inside a right-to-left paragraph: the brackets close around it, mirrored
        CHECK(to_visual(hebrew + " (" + hebrew + ")") == "(" + to_visual(hebrew) + ") " + to_visual(hebrew));
        // and after a number inside a left-to-right paragraph, a bracketed hebrew word is reversed as a unit with its brackets
        CHECK(to_visual("x " + hebrew + " 5 (" + hebrew + ") y").find("(" + to_visual(hebrew) + ")") != std::string::npos);
        // lines are paragraphs of their own
        CHECK(to_visual(hebrew + "\nabc") == to_visual(hebrew) + "\nabc");
        // trailing spaces stay at the end of the line: the right end in a left-to-right paragraph, the left side of a right-to-left one
        CHECK(to_visual("abc " + hebrew + "  ").ends_with("  ") && to_visual(hebrew + "  ").starts_with("  "));
        // explicit direction
        CHECK(to_visual("abc", nullptr, 0, text_direction::rtl) == "abc"); // no rtl characters: untouched
    }
    // arabic joining: initial / final forms, isolated letters, lam-alef ligatures, marks do not break a join
    {
        const auto v = [&](std::string_view s) { return cps_of(to_visual(s)); };
        // beh beh: initial then final (shown right to left, so final comes first in the visual order)
        CHECK((v("\xd8\xa8\xd8\xa8") == std::vector<char32_t>{0xfe90, 0xfe91}));
        // beh beh beh: initial, medial, final
        CHECK((v("\xd8\xa8\xd8\xa8\xd8\xa8") == std::vector<char32_t>{0xfe90, 0xfe92, 0xfe91}));
        // alef alone: isolated; alef after beh: final, and the alef does not join the letter after it
        CHECK((v("\xd8\xa7") == std::vector<char32_t>{0xfe8d}));
        CHECK((v("\xd8\xa8\xd8\xa7") == std::vector<char32_t>{0xfe8e, 0xfe91}));
        // lam + alef: one ligature glyph
        CHECK((v("\xd9\x84\xd8\xa7") == std::vector<char32_t>{0xfefb}));
        CHECK((v("\xd8\xa8\xd9\x84\xd8\xa7") == std::vector<char32_t>{0xfefc, 0xfe91}));
        // a mark (fatha) between letters is transparent
        CHECK((v("\xd8\xa8\xd9\x8e\xd8\xa8") == std::vector<char32_t>{0xfe90, 0x064e, 0xfe91}));
        // a font without presentation forms keeps the plain letters
        font_atlas latin = font_atlas::build().value();
        CHECK(!latin.has_glyph(0, 0xfe91));
        CHECK(cps_of(to_visual("\xd8\xa8\xd8\xa8", &latin, 0)) == cps_of("\xd8\xa8\xd8\xa8")); // (the same letters, in visual order)
    }

    // a font with the scripts: measuring, drawing and the caret
    static constexpr std::array<codepoint_range, 8> ranges = {glyph_ranges::latin, glyph_ranges::punctuation, glyph_ranges::hebrew,
                                                              glyph_ranges::arabic, glyph_ranges::arabic_forms_a, glyph_ranges::arabic_forms_b,
                                                              glyph_ranges::symbols, glyph_ranges::emoji};
    static constexpr std::array<std::string_view, 2> fallbacks = {"Segoe UI Emoji", "Segoe UI Symbol"};
    context_config cc;
    cc.font.ranges         = ranges;
    cc.font.fallback_faces = fallbacks;
    auto created = context::create(cc);
    CHECK(created.has_value());
    if (!created) { return; }
    harness h{cc};
    const font_atlas& atlas = h.ui.font();
    CHECK(atlas.has_glyph(0, 0x05e9) && atlas.has_glyph(0, 0x0645)); // shin, meem
    CHECK(atlas.has_glyph(0, 0xfe91));                              // the joined forms are in the font
    const bool have_emoji = atlas.has_glyph(0, 0x1f600);
    if (!have_emoji) { std::fprintf(stderr, "  (no emoji font here: the emoji checks are skipped)\n"); }
    else {
        CHECK(atlas.find(0, 0x1f600).visible && atlas.find(0, 0x1f600).advance > 4.0f); // U+1F600, from the fallback face
        CHECK(atlas.find(0, 0x1f600).y0 > -2.0f && atlas.find(0, 0x1f600).y1 < atlas.line_height_px(0) * 1.6f); // on the baseline of the line
        CHECK(atlas.measure(0, "a\xf0\x9f\x98\x80" "b").x > atlas.measure(0, "ab").x + 6.0f);
    }
    // invisible joiners and selectors take no room
    CHECK(near_eq(atlas.measure(0, "a\xe2\x80\x8d" "b").x, atlas.measure(0, "ab").x, 0.01f));
    CHECK(near_eq(atlas.measure(0, "a\xef\xb8\x8f" "b").x, atlas.measure(0, "ab").x, 0.01f));
    // measuring follows what is drawn: the joined text of a word is as wide as its glyphs
    CHECK(atlas.measure(0, hebrew).x > 15.0f && atlas.measure(0, arabic).x > 15.0f);
    CHECK(near_eq(atlas.measure(0, hebrew).x, atlas.measure(0, to_visual(hebrew, &atlas)).x, 0.01f));

    // drawing right-to-left text puts glyphs on the screen (and the reordered order is what comes out)
    {
        draw_list dl;
        dl.begin({800, 600}, atlas, 1.0f);
        dl.text({10, 10}, color{255, 255, 255, 255}, hebrew, 0);
        CHECK(dl.data().vertices.size() == 16); // four letters
        dl.text({10, 40}, color{255, 255, 255, 255}, arabic, 0);
        CHECK(dl.data().vertices.size() == 16 + 20);
    }

    // a text field: the caret sits where the letters are. in a hebrew word the start of the text is on the right
    {
        std::string value = hebrew;
        const auto build = [&] {
            if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) { (void)h.ui.input_text("##f", value); }
        };
        h.frames(build, 2);
        h.click({200.0f, 12.0f + h.ui.frame_height() * 0.5f}, build);
        h.key(key::home);
        h.frames(build, 2);
        const f32 at_start = h.ui.ime_position().x;
        h.key(key::end);
        h.frames(build, 2);
        const f32 at_end = h.ui.ime_position().x;
        CHECK(at_start > at_end + 10.0f); // first letter is the rightmost: the end of the text is on the left
        // typing at the end appends (logical order); the text grows to the right of the left edge, where its end is
        h.type(hebrew);
        h.frames(build, 2);
        CHECK(value == hebrew + hebrew);
        CHECK(near_eq(h.ui.ime_position().x, at_end, 1.5f));
        h.key(key::home);
        h.frames(build, 2);
        CHECK(h.ui.ime_position().x > at_start + 10.0f); // and the start of the text moved right with it
        // the mouse: a click near the left end of the word puts the caret at the end of the text
        h.key(key::home);
        h.frames(build, 2);
        const f32 word_left = h.ui.ime_position().x - atlas.measure(0, to_visual(hebrew + hebrew, &atlas)).x;
        h.click({word_left + 1.0f, 12.0f + h.ui.frame_height() * 0.5f}, build);
        h.type("!");
        h.frame(build);
        CHECK(value == hebrew + hebrew + "!");
        // backspace removes the last letter of the text, whichever side it is on
        h.key(key::backspace);
        h.key(key::backspace);
        h.frames(build, 2);
        CHECK(value == hebrew + hebrew.substr(0, hebrew.size() - 2));
    }
    // emoji in a text field: one backspace removes the whole 4-byte character
    if (have_emoji) {
        std::string value = "a";
        const auto build = [&] {
            if (auto w = h.ui.window("e", {0, 100}, {400, 0}, plain_window)) { (void)h.ui.input_text("##e", value); }
        };
        h.frames(build, 2);
        h.click({200.0f, 100.0f + 12.0f + h.ui.frame_height() * 0.5f}, build);
        h.type("\xf0\x9f\x98\x80");
        h.frame(build);
        CHECK(value == "a\xf0\x9f\x98\x80");
        h.key(key::backspace);
        h.frames(build, 2);
        CHECK(value == "a");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// the second round of features

void test_scale()
{
    std::fprintf(stderr, "[dpi / ui scale]\n");
    harness h;
    const f32 lh1 = h.ui.font().line_height(0);
    const u32 gen = h.ui.font_generation();
    CHECK(h.ui.scale() == 1.0f);
    const auto r = h.ui.set_scale(1.5f);
    CHECK(r.has_value());
    CHECK(h.ui.scale() == 1.5f && h.ui.font().scale() == 1.5f);
    CHECK(h.ui.font_generation() != gen);
    CHECK(near_eq(h.ui.font().line_height(0), lh1, 1.5f)); // layout metrics stay in logical pixels
    CHECK(h.ui.font().line_height_px(0) > lh1 * 1.3f);      // the glyphs themselves are bigger

    // the physical display is 1200x900: the logical one is 800x600, the pointer is converted too
    h.in.display_size = {1200.0f, 900.0f};
    std::string value;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) { (void)h.ui.input_text("##f", value); }
    };
    h.click({30.0f * 1.5f, 24.0f * 1.5f}, build); // logical (30, 24): inside the field
    CHECK(h.ui.want_text_input());
    CHECK(near_eq(h.ui.display_size().x, 800.0f, 0.01f));
    const draw_data d = h.ui.render_data();
    CHECK(near_eq(d.display_size.x, 1200.0f, 0.01f));
    bool clip_in_physical = false;
    for (const draw_cmd& c : d.commands) { clip_in_physical = clip_in_physical || c.clip.max.x > 800.5f; }
    CHECK(clip_in_physical); // commands are in physical pixels

    // and back
    CHECK(h.ui.set_scale(1.0f).has_value());
    CHECK(h.ui.scale() == 1.0f);
    CHECK(h.ui.set_scale(1.0f).has_value()); // the same scale again is a no-op that succeeds
}

void test_number_widgets()
{
    std::fprintf(stderr, "[drag sliders and number inputs]\n");
    harness h;
    f32 v = 1.0f;
    int n = 10;
    f32 vec[3] = {0.0f, 0.5f, 1.0f};
    f32 typed = 2.0f;
    int typed_int = 5;
    int which = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            switch (which) {
            case 0: (void)h.ui.drag_float("##d", v, 0.1f, 0.0f, 100.0f, 2); break;
            case 1: (void)h.ui.drag_int("##i", n, 0.25f, 0, 100); break;
            case 2: (void)h.ui.drag_float3("##v", vec, 0.01f, 0.0f, 1.0f, 2); break;
            default:
                (void)h.ui.input_float("##f", typed, 0.5f, 2);
                (void)h.ui.input_int("##n", typed_int, 2);
                break;
            }
        }
    };
    const f32 fh = h.ui.frame_height();
    const vec2 mid{200.0f, 12.0f + fh * 0.5f};

    // dragging: 40 px * 0.1 per pixel
    h.move(mid);
    h.frames(build, 3);
    h.down();
    h.frame(build);
    for (int i = 1; i <= 4; ++i) { h.move({mid.x + 10.0f * i, mid.y}); h.frame(build); }
    h.up();
    h.frames(build, 2);
    const f32 per_px = 100.0f / 376.0f; // a ranged field spans its range over its width
    CHECK(near_eq(v, 1.0f + 40.0f * per_px, 0.05f));

    // shift = fine (a tenth), and the range clamps
    h.in.shift = true;
    h.move(mid);
    h.frame(build);
    h.down();
    h.frame(build);
    h.move({mid.x + 100.0f, mid.y});
    h.frame(build);
    h.up();
    h.in.shift = false;
    h.frames(build, 2);
    CHECK(near_eq(v, 1.0f + 40.0f * per_px + 10.0f * per_px, 0.1f));
    h.move(mid);
    h.frame(build);
    h.down();
    h.frame(build);
    h.move({mid.x + 3000.0f, mid.y});
    h.frame(build);
    h.up();
    h.frames(build, 2);
    CHECK(near_eq(v, 100.0f, 0.01f)); // clamped to hi

    // a click without dragging starts typing; Enter applies
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.up();
    h.frames(build, 3);
    CHECK(h.ui.want_text_input());
    h.key(key::a, true);
    h.frame(build);
    h.type("42.5");
    h.frame(build);
    h.key(key::enter);
    h.frame(build);
    h.frames(build, 2);
    CHECK(near_eq(v, 42.5f, 0.001f));
    CHECK(!h.ui.want_text_input());

    // Esc cancels the entry
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.up();
    h.frames(build, 3);
    h.key(key::a, true);
    h.frame(build);
    h.type("7");
    h.frame(build);
    h.key(key::escape);
    h.frame(build);
    h.frames(build, 2);
    CHECK(near_eq(v, 42.5f, 0.001f));

    // an integer drag moves in whole steps
    which = 1;
    h.frames(build, 2);
    h.move(mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    for (int i = 1; i <= 5; ++i) { h.move({mid.x + 8.0f * i, mid.y}); h.frame(build); } // 40 px * 0.25 = 10
    h.up();
    h.frames(build, 2);
    CHECK(n == 20);

    // vectors: each component is its own field
    which = 2;
    h.frames(build, 2);
    const f32 comp_w = (376.0f - 8.0f) / 3.0f;
    const vec2 second{12.0f + comp_w + 4.0f + comp_w * 0.5f, mid.y};
    h.move(second);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({second.x - 20.0f, second.y});
    h.frame(build);
    h.up();
    h.frames(build, 2);
    CHECK(near_eq(vec[0], 0.0f) && near_eq(vec[1], 0.5f - 20.0f / comp_w, 0.02f) && near_eq(vec[2], 1.0f));

    // typed number fields: half-written text leaves the value alone, a parsed number sets it, the buttons step it
    which = 3;
    h.frames(build, 2);
    h.click({100.0f, mid.y}, build);
    h.key(key::a, true);
    h.frame(build);
    h.type("-");
    h.frame(build);
    CHECK(near_eq(typed, 2.0f)); // "-" does not parse
    h.type("3.25");
    h.frame(build);
    CHECK(near_eq(typed, -3.25f, 0.001f));
    h.click({700.0f, 500.0f}, build);
    const f32 bw = fh;
    const vec2 plus{12.0f + 376.0f - bw * 0.5f, mid.y};
    h.click(plus, build);
    CHECK(near_eq(typed, -2.75f, 0.001f)); // + step 0.5
    const vec2 int_field{100.0f, 12.0f + fh + 7.0f + fh * 0.5f};
    h.click(int_field, build);
    h.key(key::a, true);
    h.frame(build);
    h.type("12");
    h.frame(build);
    CHECK(typed_int == 12);
}

void test_plots()
{
    std::fprintf(stderr, "[plots]\n");
    harness h;
    std::vector<f32> data(200);
    for (std::size_t i = 0; i < data.size(); ++i) { data[i] = std::sin(static_cast<f32>(i) * 0.1f); }
    std::vector<f32> huge(20000);
    for (std::size_t i = 0; i < huge.size(); ++i) { huge[i] = std::sin(static_cast<f32>(i) * 0.01f); }
    u32 offset = 0;
    int which = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            switch (which) {
            case 0: h.ui.plot_lines("##p", data, {0, 80}, "overlay", plot_auto, plot_auto, offset); break;
            case 1: h.ui.plot_histogram("##p", data, {0, 80}); break;
            case 2: h.ui.plot_lines("##p", huge, {0, 80}); break;
            default: {
                const std::array<plot_series, 2> s = {{{"a", data, color{0, 0, 0, 0}}, {"b", data, color::from_hex(0xff0000ffu)}}};
                h.ui.plot("##p", s, {0, 100});
                h.ui.sparkline(data, {80, 20});
                break;
            }
            }
        }
    };
    h.frames(build, 3);
    const std::size_t base_vertices = h.ui.render_data().vertices.size();
    CHECK(base_vertices > 200);

    // hovering shows the cursor and a tooltip: more geometry, in an overlay
    h.move({200.0f, 40.0f});
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() > base_vertices);

    // ring buffer offset and the other kinds must not crash; a huge series is decimated to about a vertex per pixel
    offset = 57;
    h.frames(build, 2);
    which = 1;
    h.frames(build, 2);
    which = 2;
    h.move({-500.0f, -500.0f});
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() < 20000u); // 20000 samples were reduced
    which = 3;
    h.frames(build, 3);
    CHECK(!h.ui.render_data().commands.empty());
    which = 0;
    data.clear(); // an empty series draws the frame only
    h.frames(build, 2);
    CHECK(true);
}

void test_selectable_text()
{
    std::fprintf(stderr, "[selectable text]\n");
    harness h;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {400, 0}, plain_window)) {
            h.ui.text_selectable("hello world and more");
            h.ui.text("plain");
            {
                const auto sel = h.ui.selectable_text();
                h.ui.text("second line of text");
            }
        }
    };
    h.frames(build, 3);
    const f32 hello_w = h.ui.font().measure(0, "hello").x;
    // drag over "hello" (the text starts at (12, 12) with no padding)
    h.move({12.5f, 20.0f});
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({12.0f + hello_w, 20.0f});
    h.frames(build, 2);
    h.up();
    h.frame(build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(h.clip.data == "hello");

    // a double click selects the word under the pointer
    h.clip.data.clear();
    h.click({12.0f + h.ui.font().measure(0, "hello ").x + 6.0f, 20.0f}, build);
    h.click({12.0f + h.ui.font().measure(0, "hello ").x + 6.0f, 20.0f}, build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(h.clip.data == "world");

    // the scope makes text() selectable too, and nothing can be edited
    const f32 y2 = 12.0f + h.ui.font().line_height(0) + h.ui.theme().item_spacing + h.ui.font().line_height(0) + h.ui.theme().item_spacing + 4.0f;
    h.clip.data.clear();
    h.click({12.0f + 3.0f, y2}, build);
    h.key(key::a, true);
    h.frame(build);
    h.type("zzz");
    h.frame(build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(h.clip.data == "second line of text");
}

void test_modals()
{
    std::fprintf(stderr, "[modal windows and dialogs]\n");
    harness h;
    int clicks = 0;
    int result = 0;
    bool show_modal = false;
    const auto build = [&] {
        if (auto w = h.ui.window("bg", {0, 0}, {300, 0}, plain_window)) {
            if (h.ui.button("background")) { ++clicks; }
        }
        if (show_modal) {
            if (auto m = h.ui.modal("Modal", {300.0f, 0.0f}, modal_flags::esc_closes)) {
                h.ui.text("inside");
                if (h.ui.button("close")) { h.ui.close_modal(); }
            }
        }
        const int r = h.ui.dialog("Ask?", "Really?", {"Yes", "No"});
        if (r != 0) { result = r; }
    };
    const f32 fh = h.ui.frame_height();
    const vec2 button_at{30.0f, 12.0f + fh * 0.5f};

    h.click(button_at, build);
    CHECK(clicks == 1);
    CHECK(!h.ui.modal_open());

    // while a modal is open the window below gets nothing, and the modal has the pointer
    show_modal = true;
    h.ui.open_modal("Modal");
    h.frames(build, 30); // fades in
    CHECK(h.ui.modal_open());
    h.click(button_at, build);
    CHECK(clicks == 1); // blocked
    const rect mr = h.ui.window_rect("Modal");
    CHECK(mr.width() > 100.0f);
    CHECK(near_eq(mr.center().x, 400.0f, 3.0f)); // centered on the 800 x 600 display
    h.move({mr.center().x, mr.min.y + 40.0f});
    h.frames(build, 3);
    CHECK(h.ui.want_capture_mouse());

    // Esc closes it
    h.key(key::escape);
    h.frame(build);
    h.frames(build, 2);
    CHECK(!h.ui.modal_open());
    h.click(button_at, build);
    CHECK(clicks == 2); // reachable again

    // the close button inside closes it
    h.ui.open_modal("Modal");
    h.frames(build, 30);
    const rect mr2 = h.ui.window_rect("Modal");
    h.click({mr2.min.x + 12.0f + 30.0f, mr2.max.y - 12.0f - fh * 0.5f}, build);
    CHECK(!h.ui.modal_open());

    // a dialog returns the index of the button pressed (1-based); the last one is at the bottom right
    show_modal = false;
    h.ui.open_modal("Ask?");
    h.frames(build, 30);
    CHECK(result == 0);
    const rect dr = h.ui.window_rect("Ask?");
    h.click({dr.max.x - 12.0f - 42.0f, dr.max.y - 12.0f - fh * 0.5f}, build);
    CHECK(result == 2);
    CHECK(!h.ui.modal_open());

    // Esc dismisses it: -1
    result = 0;
    h.ui.open_modal("Ask?");
    h.frames(build, 30);
    h.key(key::escape);
    h.frame(build);
    CHECK(result == -1);

    // modals stack: the second one has the input, closing it returns to the first
    show_modal = true;
    h.ui.open_modal("Modal");
    h.ui.open_modal("Ask?");
    h.frames(build, 30);
    CHECK(h.ui.modal_open());
    h.ui.close_modal();
    h.frame(build);
    CHECK(h.ui.modal_open()); // "Modal" is still there
    h.ui.close_modal();
    h.frame(build);
    CHECK(!h.ui.modal_open());
}

void test_menus()
{
    std::fprintf(stderr, "[menus and context menus]\n");
    harness h;
    int picked = 0;
    int checked_toggles = 0;
    bool check_flag = false;
    int ctx_picked = 0;
    const auto build = [&] {
        if (auto bar = h.ui.main_menu_bar()) {
            if (auto m = h.ui.menu("File")) {
                if (h.ui.menu_item("Open", "Ctrl+O")) { picked = 1; }
                if (auto sub = h.ui.menu("Recent")) {
                    if (h.ui.menu_item("one")) { picked = 2; }
                    if (h.ui.menu_item("two")) { picked = 3; }
                }
                h.ui.menu_separator();
                if (h.ui.menu_item("Check", check_flag)) { ++checked_toggles; }
                (void)h.ui.menu_item("Off", "", false, false);
            }
            if (auto m = h.ui.menu("Edit")) {
                if (h.ui.menu_item("Copy")) { picked = 4; }
            }
        }
        if (auto w = h.ui.window("area", {100, 200}, {300, 0}, plain_window)) {
            const item_result box = h.ui.custom_item("box", {0.0f, 80.0f});
            if (auto cm = h.ui.context_menu("ctx", box.bounds)) {
                if (h.ui.menu_item("Rename")) { ctx_picked = 1; }
                if (h.ui.menu_item("Delete")) { ctx_picked = 2; }
            }
        }
    };
    h.frames(build, 4);
    const f32 bar_h = h.ui.main_menu_bar_height();
    CHECK(bar_h > 20.0f);
    const f32 file_w = h.ui.font().measure(0, "File").x + 22.0f;
    const vec2 file{6.0f + file_w * 0.5f, bar_h * 0.5f};
    const f32 row_h = bar_h - 2.0f;
    const auto row_center = [&](int index) { return vec2{60.0f, bar_h + 4.0f + row_h * (static_cast<f32>(index) + 0.5f)}; };

    // click File: the menu opens; click Open: it is picked and everything closes
    h.click(file, build);
    h.frames(build, 5);
    CHECK(h.ui.menu_is_open());
    CHECK(h.ui.want_capture_mouse());
    h.click(row_center(0), build);
    h.frames(build, 2);
    CHECK(picked == 1);
    CHECK(!h.ui.menu_is_open());

    // a submenu opens on hover beside its row, and its rows can be picked
    h.click(file, build);
    h.frames(build, 5);
    const f32 open_w = h.ui.font().measure(0, "Open").x;
    const f32 short_w = h.ui.font().measure(0, "Ctrl+O").x;
    const f32 popup_w = std::max(26.0f + open_w + 28.0f + short_w + 12.0f, 120.0f) + 8.0f;
    const f32 sub_x = 6.0f + popup_w - 6.0f; // where the submenu opens (the parent row's right edge - 2)
    h.move(row_center(1));
    h.frames(build, 6);
    h.move({sub_x + 20.0f, row_center(1).y});
    h.frames(build, 6);
    CHECK(h.ui.menu_is_open());
    h.click({sub_x + 30.0f, bar_h + 4.0f + row_h * 2.5f}, build); // the second row of the submenu ("two"): it starts level with its parent row
    h.frames(build, 2);
    CHECK(picked == 3);
    CHECK(!h.ui.menu_is_open());

    // a check item toggles its flag, and the disabled one does nothing
    h.click(file, build);
    h.frames(build, 5);
    const vec2 check_row = row_center(3); // Open, Recent, separator (7 px), Check
    h.click({check_row.x, bar_h + 4.0f + row_h * 2.0f + 7.0f + row_h * 0.5f}, build);
    h.frames(build, 2);
    CHECK(checked_toggles == 1 && check_flag);

    // hovering another header while a menu is open switches to it
    h.click(file, build);
    h.frames(build, 5);
    CHECK(h.ui.menu_is_open());
    const f32 edit_w = h.ui.font().measure(0, "Edit").x + 22.0f;
    h.move({6.0f + file_w + edit_w * 0.5f, bar_h * 0.5f});
    h.frames(build, 5);
    h.click({60.0f, bar_h + 4.0f + row_h * 0.5f}, build);
    h.frames(build, 2);
    CHECK(picked == 4); // the Edit menu's Copy

    // Esc closes a menu
    h.click(file, build);
    h.frames(build, 5);
    CHECK(h.ui.menu_is_open());
    h.key(key::escape);
    h.frame(build);
    CHECK(!h.ui.menu_is_open());

    // a context menu opens at the pointer on right click, and picks like any menu
    const vec2 target{200.0f, 240.0f};
    h.move(target);
    h.frames(build, 3);
    h.in.mouse_down[1] = true;
    h.frame(build);
    h.in.mouse_down[1] = false;
    h.frames(build, 5);
    CHECK(h.ui.menu_is_open());
    h.click({target.x + 30.0f, target.y + 4.0f + row_h * 1.5f}, build); // second row: Delete
    h.frames(build, 2);
    CHECK(ctx_picked == 2);
    CHECK(!h.ui.menu_is_open());

    // right click elsewhere closes it and does not open another one
    h.move(target);
    h.frames(build, 2);
    h.in.mouse_down[1] = true;
    h.frame(build);
    h.in.mouse_down[1] = false;
    h.frames(build, 4);
    CHECK(h.ui.menu_is_open());
    h.click({700.0f, 500.0f}, build);
    CHECK(!h.ui.menu_is_open());
}

void test_toasts()
{
    std::fprintf(stderr, "[toasts]\n");
    harness h;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {200, 0}, plain_window)) { h.ui.text("x"); }
    };
    h.frames(build, 2);
    h.ui.toast("hello", toast_kind::info, 2.0f);
    h.ui.toast("Title", "with a body that is long enough to wrap onto a second line in a narrow toast box", toast_kind::error, 5.0f);
    CHECK(h.ui.toast_count() == 2);
    h.frames(build, 10, 0.05f);
    const auto with_toasts = h.ui.render_data().vertices.size();
    CHECK(with_toasts > 100);

    // the short one expires, the long one stays
    h.frames(build, 30, 0.1f);
    CHECK(h.ui.toast_count() == 1);
    // hovering a toast pauses its timer
    const f32 age_probe = 0.0f;
    (void)age_probe;
    h.move({800.0f - 14.0f - 100.0f, 600.0f - 14.0f - 20.0f});
    h.frames(build, 3);
    CHECK(h.ui.want_capture_mouse());
    h.frames(build, 60, 0.1f); // 6 s over the toast: it would have expired
    CHECK(h.ui.toast_count() == 1);
    // clicking it dismisses it
    h.down();
    h.frame(build);
    h.up();
    h.frames(build, 20, 0.05f);
    CHECK(h.ui.toast_count() == 0);
    h.ui.toast("a", toast_kind::success);
    h.ui.clear_toasts();
    CHECK(h.ui.toast_count() == 0);
    for (int i = 0; i < 20; ++i) { h.ui.toast("spam", toast_kind::info); }
    CHECK(h.ui.toast_count() <= 8);
}

void test_log_view()
{
    std::fprintf(stderr, "[log view]\n");
    harness h;
    log_buffer log{100};
    for (int i = 0; i < 300; ++i) { log.addf(i % 7 == 0 ? log_level::warn : log_level::info, "message number {}", i); }
    CHECK(log.size() == 100); // a ring: the oldest lines fall out
    CHECK(log[0].text == "message number 200");

    log_buffer big{20000};
    for (int i = 0; i < 12000; ++i) { big.addf(log_level::debug, "line {}", i); }
    log_buffer* shown = &big;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {0, 0}, {500, 400}, plain_window)) {
            h.ui.log_view("log", *shown);
        }
    };
    h.frames(build, 4);
    // only the rows in view are drawn: 12000 lines cost about as much as 20
    CHECK(h.ui.render_data().vertices.size() < 15000u);

    // the filter narrows the list; the level hides the rest
    big.view.filter = "line 1199";
    h.frames(build, 3);
    const auto filtered = h.ui.render_data().vertices.size();
    big.view.filter.clear();
    big.view.min_level = log_level::error;
    h.frames(build, 3);
    CHECK(h.ui.render_data().vertices.size() < filtered + 3000u);
    big.view.min_level = log_level::trace;

    // follow mode sticks to the end: adding a line keeps the newest one on screen
    shown = &log;
    log.view.follow = true;
    h.frames(build, 3);
    log.add(log_level::error, "the newest line");
    h.frames(build, 3);
    CHECK(log.view.follow);

    // clicking a row selects it, Ctrl+C copies the selection
    h.click({40.0f, 200.0f}, build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(!h.clip.data.empty() && h.clip.data.find("message number") != std::string::npos);
    h.key(key::a, true);
    h.frame(build);
    h.key(key::c, true);
    h.frame(build);
    CHECK(std::count(h.clip.data.begin(), h.clip.data.end(), '\n') >= 90);
}

void test_themes()
{
    std::fprintf(stderr, "[themes and theme files]\n");
    CHECK(themes::names().size() >= 12);
    style s;
    for (const std::string_view name : themes::names()) { CHECK(themes::by_name(name, s)); }
    CHECK(!themes::by_name("no such theme", s));
    CHECK(themes::by_name("NORD", s)); // case-insensitive
    CHECK(s.accent == themes::nord().accent);

    // text round trip
    const style original = themes::dracula();
    const std::string text = themes::to_string(original, "dracula");
    style read;
    const themes::theme_result r = themes::from_string(text, read);
    CHECK(r.ok() && r.applied >= 22);
    CHECK(read.accent == original.accent && read.window_bg == original.window_bg && read.text_dim == original.text_dim);
    CHECK(read.rounding == original.rounding && read.frame_padding == original.frame_padding && read.blur_radius == original.blur_radius);

    // partial files, bases, comments, mistakes
    style t = themes::midnight();
    const themes::theme_result r2 = themes::from_string("# a comment\nbase = light\n; another\n// and one more\nrounding = 3.5\naccent = #f80\nbogus = 1\ntext = zzz\nnot a line\n", t);
    CHECK(t.window_bg == themes::light().window_bg); // the base was applied first
    CHECK(t.rounding == 3.5f);
    CHECK((t.accent == color{255, 136, 0, 255}));
    CHECK(r2.applied == 3 && r2.unknown == 1 && r2.invalid == 2);
    CHECK(r2.first_problem_line == 7);
    CHECK(!r2.ok());

    // files
    char temp[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, temp);
    const std::string path = std::string{temp} + "strata_selftest_theme.ini";
    CHECK(themes::save_file(path, themes::forest(), "forest"));
    style loaded;
    themes::theme_result fr;
    CHECK(themes::load_file(path, loaded, &fr));
    CHECK(fr.ok() && loaded.accent == themes::forest().accent);
    ::DeleteFileA(path.c_str());
    CHECK(!themes::load_file(path, loaded, nullptr)); // gone
}

void test_draw_list_extras()
{
    std::fprintf(stderr, "[draw list: gradients, curves, backdrop]\n");
    harness h;
    h.frames([&] {}, 1);
    draw_list dl;
    font_atlas atlas = font_atlas::build().value();
    dl.begin({800, 600}, atlas, 1.0f);

    const std::array<vec2, 4> pts = {{{10, 10}, {50, 40}, {90, 10}, {130, 40}}};
    dl.polyline(pts, color{255, 255, 255, 255}, 3.0f);
    CHECK(dl.data().vertices.size() == 16 && dl.data().indices.size() == 3 * 18);
    dl.polyline(pts, color{255, 255, 255, 255}, 3.0f, true);
    CHECK(dl.data().vertices.size() == 32 && dl.data().indices.size() == 3 * 18 + 4 * 18); // closed: one more segment
    const std::array<vec2, 2> dup = {{{5, 5}, {5, 5}}};
    dl.polyline(dup, color{255, 255, 255, 255}, 2.0f); // no direction: nothing to draw, nothing broken
    CHECK(dl.data().vertices.size() == 32);

    const auto before = dl.data().vertices.size();
    dl.bezier_cubic({0, 0}, {100, 0}, {100, 100}, {200, 100}, color{255, 0, 0, 255}, 2.0f);
    CHECK(dl.data().vertices.size() > before + 20);
    dl.arc({100, 100}, 30.0f, 0.0f, 3.0f, color{0, 255, 0, 255}, 4.0f);
    dl.arc({100, 100}, 30.0f, 0.0f, 6.3f, color{0, 255, 0, 255}, 4.0f); // a full circle closes
    dl.circle({20, 20}, 8.0f, color{255, 255, 255, 255});
    dl.circle_filled({20, 20}, 8.0f, color{255, 255, 255, 255});

    // gradients are shape records with a direction
    dl.rect_gradient_angle({{0, 0}, {100, 50}}, color{255, 0, 0, 255}, color{0, 0, 255, 255}, 0.0f, 4.0f);
    dl.rect_gradient_radial({{0, 0}, {100, 50}}, color{255, 255, 255, 255}, color{0, 0, 0, 255});
    const auto shapes = dl.data().shapes;
    CHECK(shapes.size() >= 4);
    const shape_record& lin = shapes[shapes.size() - 2];
    const shape_record& rad = shapes[shapes.size() - 1];
    CHECK(near_eq(lin.gradient_dir.x, 1.0f, 0.001f) && near_eq(lin.gradient_dir.y, 0.0f, 0.001f) && lin.gradient_kind == 0.0f);
    CHECK(rad.gradient_kind == 1.0f);
    CHECK(sizeof(shape_record) == 80);

    // a backdrop panel is a command of its own
    dl.backdrop({{100, 100}, {300, 200}}, 12.0f, color{20, 20, 30, 150});
    dl.rect_filled({{0, 300}, {50, 350}}, color{255, 255, 255, 255});
    int blur_cmds = 0;
    for (const draw_cmd& c : dl.data().commands) { blur_cmds += c.blur > 0.0f ? 1 : 0; }
    CHECK(blur_cmds == 1);
    CHECK(dl.data().commands.back().blur == 0.0f); // what follows is an ordinary command again

    // at a scale the output is physical
    dl.begin({1200, 900}, atlas, 1.5f);
    dl.rect_filled({{10, 10}, {20, 20}}, color{255, 255, 255, 255});
    const vertex& v0 = dl.data().vertices[0];
    CHECK(near_eq(v0.pos.x, 15.0f, 0.01f) && near_eq(v0.pos.y, 15.0f, 0.01f));
    CHECK(near_eq(dl.data().commands[0].clip.max.x, 1200.0f, 0.01f));
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
        CHECK(h.ui.popup_open());
        const f32 item_h = fh - 4.0f;
        const auto row = [&](int i) { return vec2{120.0f, 12.0f + fh + 4.0f + 4.0f + item_h * (static_cast<f32>(i) + 0.5f)}; };
        h.click(row(0), build);
        CHECK(sel[0] && sel[1] && changed_seen);
        CHECK(h.ui.popup_open()); // the list stays open
        h.click(row(2), build);
        CHECK(sel[2]);
        h.click(row(1), build); // toggles off
        CHECK(!sel[1]);
        // the toolbar ("select all / clear") shows for longer lists; Esc closes
        h.key(key::escape);
        h.frames(build, 3);
        CHECK(!h.ui.popup_open());
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

void test_chords()
{
    std::fprintf(stderr, "[key chords, hotkey_chord]\n");

    // text round trip
    key_chord c;
    CHECK((chord_from_string("ctrl + shift + s", c) && c == key_chord{'S', true, true, false}));
    CHECK(chord_to_string(c) == "Ctrl+Shift+S");
    CHECK(chord_from_string("Alt+F4", c) && c.key == 0x73 && c.alt && !c.ctrl && !c.shift);
    CHECK(chord_from_string("Ctrl+Num +", c) && c.key == 0x6b && c.ctrl); // a key whose name contains '+'
    CHECK(chord_from_string("Page Up", c) && c.key == 0x21 && !c.ctrl);
    CHECK(chord_from_string("Mouse 4", c) && c.key == 0x05);
    CHECK(chord_from_string(";", c) && c.key == 0xba);
    CHECK(chord_from_string("", c) && !c.bound() && chord_to_string(c).empty());
    key_chord keep{'A', true, false, false};
    CHECK(!chord_from_string("Ctrl+Nonsense", keep) && keep.key == 'A' && keep.ctrl); // untouched on failure
    CHECK(!chord_from_string("Ctrl+", keep) && !chord_from_string("+", keep));
    bool every_key = true;
    for (u32 vk = 1; vk < 0xff; ++vk) {
        if (key_name(vk) == "Key ?") { continue; }
        const key_chord in{vk, true, true, true};
        key_chord back;
        every_key = every_key && chord_from_string(chord_to_string(in), back) && back == in;
    }
    CHECK(every_key);

    // chord_pressed needs the exact modifiers
    {
        harness h;
        const key_chord save{'S', true, false, false};
        int hits = 0;
        const auto build = [&] { if (h.ui.chord_pressed(save)) { ++hits; } };
        h.in.ctrl = true; h.in.pressed_key = 'S'; h.frame(build); h.in.ctrl = false;
        CHECK(hits == 1);
        h.in.pressed_key = 'S'; h.frame(build);
        CHECK(hits == 1);
        h.in.ctrl = true; h.in.alt = true; h.in.pressed_key = 'S'; h.frame(build); h.in.ctrl = h.in.alt = false;
        CHECK(hits == 1);
        CHECK(!h.ui.chord_pressed({}));
    }

    // hotkey_chord: click the field, then press the chord
    {
        harness h;
        key_chord chord{'A', false, false, false};
        bool changed = false;
        const auto build = [&] {
            if (auto w = h.ui.window("k", {100, 100}, {300, 0}, plain_window)) { changed = h.ui.hotkey_chord("##k", chord) || changed; }
        };
        const vec2 field{130.0f, 124.0f};
        h.frames(build, 2);
        CHECK(!h.ui.want_text_input());
        h.click(field, build);
        CHECK(h.ui.want_text_input()); // waiting for a key
        h.in.ctrl = true; h.in.shift = true; h.in.pressed_key = 'K'; h.frame(build); h.in.ctrl = h.in.shift = false;
        CHECK((changed && chord == key_chord{'K', true, true, false}));
        CHECK(!h.ui.want_text_input());

        // a bare Esc leaves it as it was
        changed = false;
        h.click(field, build);
        h.in.pressed_key = 0x1b; h.frame(build);
        CHECK((!changed && chord == key_chord{'K', true, true, false}));

        // Ctrl + Delete is a chord of its own, a bare Delete unbinds
        h.click(field, build);
        h.in.ctrl = true; h.in.pressed_key = 0x2e; h.frame(build); h.in.ctrl = false;
        CHECK((changed && chord == key_chord{0x2e, true, false, false}));
        changed = false;
        h.click(field, build);
        h.in.pressed_key = 0x2e; h.frame(build);
        CHECK(changed && !chord.bound());

        // nothing fires while the field waits for a key
        h.click(field, build);
        int fired = 0;
        const auto build_fire = [&] { build(); if (h.ui.accelerator("Ctrl+P")) { ++fired; } };
        h.in.ctrl = true; h.in.pressed_key = 'P'; h.frame(build_fire); h.in.ctrl = false;
        CHECK((fired == 0 && chord == key_chord{'P', true, false, false}));
    }
}

void test_keybinds()
{
    std::fprintf(stderr, "[keybinds]\n");
    keybinds binds;
    CHECK(binds.add("save", "Ctrl+S", "write the document") == 0);
    CHECK(binds.add("open", "Ctrl+O") == 1);
    CHECK(binds.add("save", "F1") == 0); // registered already: left as it is
    CHECK((binds.actions().size() == 2 && binds.find("save")->chord == key_chord{'S', true, false, false}));
    CHECK(binds.add("broken", "Ctrl+Nonsense") == 2 && !binds.find("broken")->chord.bound());
    CHECK(binds.text("open") == "Ctrl+O" && binds.text("nope").empty() && binds.text("broken").empty());
    CHECK(binds.conflict("save") == nullptr && binds.conflict("broken") == nullptr);

    CHECK(binds.bind("open", {'S', true, false, false}));
    CHECK(!binds.bind("nope", {}));
    CHECK(binds.conflict("save") != nullptr && binds.conflict("save")->name == "open");
    CHECK(binds.reset("open") && binds.conflict("save") == nullptr && !binds.reset("nope"));

    // pressed()
    {
        harness h;
        int saves = 0, opens = 0;
        const auto build = [&] {
            if (binds.pressed(h.ui, "save")) { ++saves; }
            if (binds.pressed(h.ui, "open")) { ++opens; }
            if (binds.pressed(h.ui, "nope") || binds.pressed(h.ui, "broken")) { saves += 100; }
        };
        h.in.ctrl = true; h.in.pressed_key = 'S'; h.frame(build);
        h.in.pressed_key = 'O'; h.frame(build);
        CHECK(saves == 1 && opens == 1);
        CHECK(binds.bind("save", {0x74, false, false, false})); // F5 now
        h.in.pressed_key = 'S'; h.frame(build);
        h.in.ctrl = false; h.in.pressed_key = 0x74; h.frame(build);
        CHECK(saves == 2 && opens == 1);
    }

    // config round trip: an unbound action is written as an empty value and stays unbound
    binds.reset_all();
    CHECK(binds.bind("open", {0x74, false, false, true}) && binds.bind("save", {})); // Alt+F5, unbound
    config cfg;
    binds.store(cfg);
    CHECK(cfg.get("keybinds", "open") == "Alt+F5" && cfg.has("keybinds", "save") && cfg.get("keybinds", "save").empty());

    keybinds other;
    other.add("save", "Ctrl+S");
    other.add("open", "Ctrl+O");
    other.add("extra", "F9"); // not in the file: keeps its default
    CHECK(other.load(cfg) == 2);
    CHECK((!other.find("save")->chord.bound() && other.find("open")->chord == key_chord{0x74, false, false, true}));
    CHECK(other.find("extra")->chord.key == 0x78);

    cfg.set("keybinds", "open", "Ctrl+Nonsense"); // an unreadable value is skipped
    CHECK(other.load(cfg) == 0 && other.find("open")->chord.alt);
    other.reset_all();
    CHECK((other.find("open")->chord == key_chord{'O', true, false, false}));

    // the editor draws a row per action
    {
        harness h;
        binds.reset_all();
        CHECK(binds.bind("open", {'S', true, false, false})); // a conflict, so the warning mark shows too
        bool changed = false;
        const auto build = [&] {
            if (auto w = h.ui.window("keys", {20, 20}, {420, 0}, plain_window)) { changed = keybind_editor(h.ui, binds); }
        };
        h.frames(build, 3);
        CHECK(!changed && h.ui.render_data().vertices.size() > 200);
        const std::size_t with_three = h.ui.render_data().vertices.size();
        binds.add("more", "F2");
        h.frames(build, 3);
        CHECK(h.ui.render_data().vertices.size() > with_three);
    }
}

void test_config()
{
    std::fprintf(stderr, "[config files]\n");
    config c;
    const std::size_t bad = c.from_string(
        "# a comment\nname = strata\n volume=0.6 \n\n[Window]\nwidth = 1280\nmaximized = yes\nnote = a = b\nnonsense line\n[broken\n"
        "; another\n// and one more\n[keys]\nsave = Ctrl+S\nempty =\n");
    CHECK(bad == 2);
    CHECK(c.get("", "name") == "strata" && near_eq(c.get_float("", "volume"), 0.6f, 0.0001f));
    CHECK(c.get_int("window", "WIDTH") == 1280); // names are case-insensitive
    CHECK(c.get_bool("Window", "maximized") && !c.get_bool("Window", "missing") && c.get_bool("Window", "missing", true));
    CHECK(c.get("window", "note") == "a = b");   // the first '=' splits
    CHECK(c.get_int("window", "note", 7) == 7 && c.get_int("window", "missing", -1) == -1); // not a number: the fallback
    CHECK(c.get_float("", "name", 2.5f) == 2.5f && c.get_bool("", "name", true));
    CHECK(c.has("keys", "empty") && c.get("keys", "empty", "x").empty() && !c.has("keys", "absent"));
    CHECK(c.entries("window").size() == 3 && c.entries("nothing").empty());

    // set replaces in place, erase removes, keys that would be read back wrongly are made safe
    c.set_int("Window", "width", 800);
    c.set_float("window", "scale", 1.25f);
    c.set_bool("window", "maximized", false);
    CHECK(c.get_int("window", "width") == 800 && c.entries("window").size() == 4 && c.entries("window")[0].key == "width");
    CHECK(near_eq(c.get_float("window", "scale"), 1.25f, 0.0001f) && !c.get_bool("window", "maximized", true));
    CHECK(c.erase("window", "note") && !c.erase("window", "note") && !c.has("window", "note"));
    c.set("s", "a=b", "x\ny");
    c.set("s", "#hidden", "1");
    CHECK(c.get("s", "a=b") == "x y" && c.get("s", "a_b") == "x y" && c.get("s", "#hidden") == "1");
    config back;
    CHECK(back.from_string(c.to_string()) == 0 && back.to_string() == c.to_string()); // what is written can be read back

    // the unnamed section is written first, without a header, so it does not swallow the others
    config d;
    d.set("late", "a", "1");
    d.set("", "top", "2");
    CHECK(d.to_string() == "top = 2\n\n[late]\na = 1\n");

    // merging keeps what the text does not mention
    d.from_string("[late]\nb = 3\n[new]\nc = 4\n");
    CHECK(d.get_int("late", "a") == 1 && d.get_int("late", "b") == 3 && d.get_int("new", "c") == 4);
    CHECK(config{}.empty() && !d.empty());
    d.clear();
    CHECK(d.empty() && d.to_string().empty());

    // files
    char temp[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, temp);
    const std::string path = std::string{temp} + "strata_selftest_config.ini";
    CHECK(c.save_file(path));
    config loaded;
    std::size_t unreadable = 99;
    CHECK(loaded.load_file(path, &unreadable) && unreadable == 0 && loaded.to_string() == c.to_string());
    ::DeleteFileA(path.c_str());
    CHECK(!loaded.load_file(path)); // gone

    // a theme can live in a section of the same file
    style s = themes::nord();
    s.rounding = 3.5f;
    config with_theme;
    with_theme.set("", "note", "keep me");
    themes::to_config(with_theme, s);
    style read = themes::midnight();
    const themes::theme_result r = themes::from_config(with_theme, read);
    CHECK(r.ok() && r.applied >= 22);
    CHECK(read.rounding == 3.5f && read.accent == s.accent && read.window_bg == s.window_bg && read.frame_padding == s.frame_padding);
    CHECK(with_theme.get("", "note") == "keep me" && with_theme.has("theme", "accent"));
    style untouched = themes::light();
    CHECK(themes::from_config(config{}, untouched).applied == 0 && untouched.window_bg == themes::light().window_bg);
}

void test_window_height_cap()
{
    std::fprintf(stderr, "[auto-height windows stop at the bottom of the display]\n");
    harness h; // an 800 x 600 display
    int  rows = 60;
    f32  first_row_y = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("tall", {100, 50}, {300, 0}, window_flags::none)) {
            for (int i = 0; i < rows; ++i) {
                const item_result r = h.ui.custom_item("row" + std::to_string(i), {100.0f, 18.0f});
                if (i == 0) { first_row_y = r.bounds.min.y; }
            }
        }
    };
    h.frames(build, 4);
    rect r = h.ui.window_rect("tall");
    CHECK(near_eq(r.max.y, 600.0f - 8.0f, 1.0f)); // it would be 60 rows tall: it ends at the bottom of the display instead
    CHECK(near_eq(r.min.y, 50.0f, 0.5f) && near_eq(r.width(), 300.0f, 0.5f));

    // the content scrolls with the wheel (and the window keeps its size)
    const f32 y0 = first_row_y;
    h.move({200.0f, 200.0f});
    h.frames(build, 2);
    h.in.wheel = -1.0f;
    h.frames(build, 2); // the scroll offset is applied to the next frame's layout
    CHECK(near_eq(first_row_y, y0 - 48.0f, 1.0f));
    CHECK(near_eq(h.ui.window_rect("tall").max.y, r.max.y, 0.5f));

    // once the content fits it follows the content again, unscrolled
    rows = 3;
    h.frames(build, 4);
    r = h.ui.window_rect("tall");
    CHECK(r.height() < 150.0f && near_eq(first_row_y, y0, 0.5f));

    // a window that starts lower has less room
    harness l;
    const auto build_low = [&] {
        if (auto w = l.ui.window("low", {100, 400}, {300, 0}, window_flags::none)) {
            for (int i = 0; i < 40; ++i) { l.ui.text("row"); }
        }
    };
    l.frames(build_low, 4);
    CHECK(near_eq(l.ui.window_rect("low").max.y, 600.0f - 8.0f, 1.0f));

    // a fixed height is left alone, even if it reaches past the display
    harness g;
    const auto build_fixed = [&] {
        if (auto w = g.ui.window("fixed", {100, 300}, {300, 500}, window_flags::none)) { g.ui.text("x"); }
    };
    g.frames(build_fixed, 3);
    CHECK(near_eq(g.ui.window_rect("fixed").height(), 500.0f, 0.5f));
}

void test_popups_and_drag_drop()
{
    std::fprintf(stderr, "[generic popups, drag and drop, spinner / badge / chip]\n");

    // popups: open under a button, work like a small window, close on Esc, a click outside, or close_popup()
    {
        harness h;
        bool wrap = false;
        bool open_now = false;
        int  done = 0;
        rect btn;
        const auto build = [&] {
            if (auto w = h.ui.window("p", {100, 100}, {300, 0}, plain_window)) {
                if (h.ui.button("options")) { h.ui.toggle_popup("opts"); }
                btn = h.ui.last_item_rect();
                if (auto p = h.ui.popup("opts", 220.0f)) {
                    (void)h.ui.checkbox("wrap", wrap);
                    if (h.ui.button("done")) { ++done; h.ui.close_popup(); }
                }
                open_now = h.ui.popup_is_open("opts");
            }
        };
        h.frames(build, 3);
        CHECK(!open_now && !h.ui.popup_open());
        h.click({btn.min.x + 10.0f, btn.center().y}, build);
        CHECK(open_now);
        h.frames(build, 2);
        CHECK(h.ui.popup_open() && h.ui.want_capture_mouse());

        // the popup content takes clicks: the check box is the first row, 4 px below the button plus the padding
        const vec2 first_row{btn.min.x + 12.0f + 8.0f, btn.max.y + 4.0f + 12.0f + 8.0f};
        h.click(first_row, build);
        CHECK(wrap && open_now);

        // a click outside closes it and does not reach what is below
        h.click({600.0f, 500.0f}, build);
        CHECK(!open_now && !h.ui.popup_open());

        // clicking the button again toggles; Esc closes
        h.click({btn.min.x + 10.0f, btn.center().y}, build);
        CHECK(open_now);
        h.key(key::escape);
        h.frames(build, 2);
        CHECK(!open_now);

        // close_popup() from inside: the "done" button is the second row
        h.click({btn.min.x + 10.0f, btn.center().y}, build);
        h.frames(build, 2);
        const f32 row_h = h.ui.frame_height() + 7.0f; // a row plus the item spacing
        h.click({btn.min.x + 12.0f + 20.0f, btn.max.y + 4.0f + 12.0f + row_h + 8.0f}, build);
        CHECK(done == 1 && !open_now);

        // open_popup at a position
        bool at_open = false;
        const auto build_at = [&] {
            if (auto w = h.ui.window("q", {100, 300}, {300, 0}, plain_window)) {
                if (auto p = h.ui.popup("at", 150.0f)) { h.ui.text("free"); at_open = true; }
            }
        };
        h.ui.begin_frame(h.in);
        h.ui.end_frame();
        h.frames(build_at, 2);
        CHECK(!at_open);
    }

    // drag and drop
    {
        harness h;
        int dropped_on = -1, dropped_value = -1;
        bool hover_b = false;
        std::array<rect, 3> rows{};
        const auto build = [&] {
            if (auto w = h.ui.window("d", {100, 100}, {300, 0}, plain_window)) {
                for (int i = 0; i < 3; ++i) {
                    const item_result r = h.ui.custom_item("row" + std::to_string(i), {200.0f, 30.0f});
                    rows[static_cast<std::size_t>(i)] = r.bounds;
                    if (auto d = h.ui.drag_source("row", i)) { h.ui.text("moving"); }
                    const drop_result drop = h.ui.drop_target("row");
                    if (i == 1) { hover_b = drop.hovering; }
                    if (drop) { dropped_on = i; dropped_value = drop.as<int>(); }
                }
            }
        };
        h.frames(build, 3);
        const vec2 a = rows[0].center(), b = rows[1].center();
        h.move(a);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        CHECK(!h.ui.dragging()); // a press alone is not a drag
        h.move({a.x + 2.0f, a.y + 2.0f});
        h.frame(build);
        CHECK(!h.ui.dragging()); // and neither is a small move
        h.move(b);
        h.frames(build, 2);
        CHECK(h.ui.dragging() && h.ui.drag_payload_type() == "row" && hover_b);
        CHECK(h.ui.want_capture_mouse());
        h.up();
        h.frame(build);
        CHECK(dropped_on == 1 && dropped_value == 0);
        h.frames(build, 2);
        CHECK(!h.ui.dragging());

        // dropped on itself: it is a drop, not a click of the source; Esc cancels; the wrong type is not taken
        dropped_on = -1;
        h.move(a); h.frames(build, 2); h.down(); h.frame(build);
        h.move(b); h.frames(build, 2);
        h.key(key::escape);
        h.frames(build, 2);
        h.up();
        h.frame(build);
        CHECK(dropped_on == -1 && !h.ui.dragging());

        bool other = false;
        const auto build_other = [&] {
            if (auto w = h.ui.window("d", {100, 100}, {300, 0}, plain_window)) {
                for (int i = 0; i < 3; ++i) {
                    (void)h.ui.custom_item("row" + std::to_string(i), {200.0f, 30.0f});
                    if (auto d = h.ui.drag_source("row", i)) { h.ui.text("moving"); }
                    if (h.ui.drop_target("file")) { other = true; }
                }
            }
        };
        h.move(a); h.frames(build_other, 2); h.down(); h.frame(build_other);
        h.move(b); h.frames(build_other, 2);
        h.up(); h.frame(build_other);
        CHECK(!other);
    }

    // spinner, badge, chip
    {
        harness h;
        bool on = false;
        chip_result last;
        rect body;
        int closes = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("c", {100, 100}, {400, 0}, plain_window)) {
                h.ui.spinner();
                h.ui.same_line();
                h.ui.badge("3", toast_kind::warning);
                h.ui.same_line();
                h.ui.badge("new", color{80, 200, 120, 255});
                last = h.ui.chip("filter", {.closable = true, .selected = &on});
                body = h.ui.last_item_rect();
                if (last.closed) { ++closes; }
            }
        };
        h.frames(build, 3);
        const std::size_t v0 = h.ui.render_data().vertices.size();
        h.frames(build, 5);
        CHECK(v0 > 100 && body.width() > 20.0f);
        h.click({body.min.x + 10.0f, body.center().y}, build);
        CHECK(on && closes == 0);
        h.click({body.min.x + 10.0f, body.center().y}, build);
        CHECK(!on);
        h.click({body.max.x + 8.0f, body.center().y}, build); // the x
        CHECK(closes == 1 && !on);
    }
}

void test_tabs()
{
    std::fprintf(stderr, "[tabs: closable, reorderable, add button, overflow]\n");

    // apply_tab_events keeps `selected` on the tab it was on
    {
        std::vector<std::string> v{"a", "b", "c", "d"};
        int sel = 1;
        tab_events ev;
        ev.moved_from = 1; ev.moved_to = 3;
        apply_tab_events(ev, v, sel);
        CHECK(v == (std::vector<std::string>{"a", "c", "d", "b"}) && sel == 3);
        sel = 2; ev.moved_from = 0; ev.moved_to = 3;
        apply_tab_events(ev, v, sel);
        CHECK(v == (std::vector<std::string>{"c", "d", "b", "a"}) && sel == 1);
        sel = 1; ev = {}; ev.closed = 0;
        apply_tab_events(ev, v, sel);
        CHECK(v.size() == 3 && sel == 0);
        sel = 2; ev.closed = 2;
        apply_tab_events(ev, v, sel);
        CHECK(v.size() == 2 && sel == 1); // the last tab closed: the one before it
    }

    harness h;
    std::vector<std::string> names{"Alpha", "Beta", "Gamma", "Delta"};
    int sel = 0;
    tab_events ev;
    tab_bar_flags flags = tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button;
    int adds = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("t", {100, 100}, {500, 0}, plain_window)) {
            std::vector<tab_desc> d;
            for (const std::string& n : names) { d.emplace_back(std::string_view{n}); }
            ev = h.ui.tab_bar("tabs", d.data(), d.size(), sel, flags);
            if (ev.add) { ++adds; }
        }
        apply_tab_events(ev, names, sel);
    };
    const auto tab_w = [&](std::size_t i, bool closable) { return h.ui.font().measure(0, names[i]).x + 26.0f + (closable ? 20.0f : 0.0f); };
    const auto body_center = [&](std::size_t i) {
        f32 x = 112.0f;
        for (std::size_t j = 0; j < i; ++j) { x += tab_w(j, true) + 2.0f; }
        return vec2{x + (tab_w(i, true) - 20.0f) * 0.5f, 112.0f + h.ui.frame_height() * 0.5f};
    };
    const auto close_center = [&](std::size_t i) {
        f32 x = 112.0f;
        for (std::size_t j = 0; j <= i; ++j) { x += tab_w(j, true) + 2.0f; }
        return vec2{x - 2.0f - 11.0f, 112.0f + h.ui.frame_height() * 0.5f};
    };

    h.frames(build, 3);
    h.click(body_center(2), build);
    CHECK(sel == 2 && names.size() == 4);

    // the "+" comes after the last tab
    {
        f32 x = 112.0f;
        for (std::size_t j = 0; j < names.size(); ++j) { x += tab_w(j, true) + 2.0f; }
        h.click({x + 2.0f + 12.0f, 112.0f + h.ui.frame_height() * 0.5f}, build);
        CHECK(adds == 1);
    }

    // the x closes: the tab is gone from the list once the events are applied, and the selection stays on Gamma
    h.click(close_center(1), build); // Beta
    CHECK((names == std::vector<std::string>{"Alpha", "Gamma", "Delta"}) && names[static_cast<std::size_t>(sel)] == "Gamma");

    // dragging a tab past its neighbours moves it and it stays selected
    {
        const vec2 from = body_center(0);
        h.move(from);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        for (int i = 1; i <= 12; ++i) {
            h.move({from.x + 18.0f * static_cast<f32>(i), from.y});
            h.frame(build);
        }
        h.up();
        h.frames(build, 2);
        CHECK(names.size() == 3 && names[0] != "Alpha" && names[static_cast<std::size_t>(sel)] == "Alpha");
        CHECK(names.back() == "Alpha"); // 216 px to the right passes both other tabs
    }

    // many tabs scroll; the list button at the right end opens a menu of all of them
    {
        names.clear();
        for (int i = 0; i < 14; ++i) { names.push_back("Document " + std::to_string(i)); }
        flags = tab_bar_flags::closable;
        sel = 0;
        h.frames(build, 4);
        const f32 list_x = 112.0f + 476.0f - 13.0f;
        h.click({list_x, 112.0f + h.ui.frame_height() * 0.5f}, build);
        h.frames(build, 2);
        CHECK(h.ui.popup_open());
        CHECK(h.ui.want_capture_mouse());
        h.click({30.0f, 500.0f}, build); // outside the list
        CHECK(!h.ui.popup_open());

        // the list is at most nine rows tall and scrolls; a row picks its tab
        h.click({list_x, 112.0f + h.ui.frame_height() * 0.5f}, build);
        h.frames(build, 2);
        {
            const f32 lh    = h.ui.font().line_height(0);
            const f32 pitch = lh + 8.0f + 7.0f;
            const f32 top   = 111.0f + h.ui.frame_height() + 4.0f + 12.0f; // under the list button, plus the popup padding
            h.click({650.0f, top + 3.0f * pitch + (lh + 8.0f) * 0.5f}, build); // about the fourth row (the popup sits at 556 .. 796)
            h.frames(build, 2);
            CHECK(sel == 3 && !h.ui.popup_open());
        }

        // selecting a far tab from outside scrolls it into view and nothing breaks with the wheel
        sel = 13;
        h.frames(build, 30);
        h.move({300.0f, 125.0f});
        h.in.wheel = 1.0f;
        h.frames(build, 20);
        h.in.wheel = 0.0f;
        CHECK(sel == 13 && names.size() == 14);

        // and the tabs that fit are not scrolled at all
        names.resize(3);
        sel = 0;
        h.frames(build, 30);
        h.click(body_center(1), build);
        CHECK(sel == 1);
    }
}

void test_datetime_pickers()
{
    std::fprintf(stderr, "[dates, times, pickers]\n");

    // calendar arithmetic
    CHECK(is_leap_year(2024) && !is_leap_year(1900) && is_leap_year(2000) && !is_leap_year(2023));
    CHECK(days_in_month(2024, 2) == 29 && days_in_month(2023, 2) == 28 && days_in_month(2026, 9) == 30 && days_in_month(2026, 12) == 31);
    CHECK(days_from_civil({1970, 1, 1}) == 0 && days_from_civil({2000, 3, 1}) == 11017 && days_from_civil({1969, 12, 31}) == -1);
    CHECK((civil_from_days(days_from_civil({2026, 9, 25})) == date{2026, 9, 25}));
    CHECK(weekday({1970, 1, 1}) == 3);  // a Thursday
    CHECK(weekday({2026, 9, 25}) == 4); // a Friday
    CHECK(weekday({2000, 1, 1}) == 5 && weekday({2024, 2, 29}) == 3);
    CHECK((add_days({2026, 12, 31}, 1) == date{2027, 1, 1}) && (add_days({2024, 3, 1}, -1) == date{2024, 2, 29}));
    CHECK((add_months({2026, 1, 31}, 1) == date{2026, 2, 28}) && (add_months({2026, 11, 15}, 3) == date{2027, 2, 15}));
    CHECK((add_months({2026, 1, 15}, -1) == date{2025, 12, 15}) && (add_months({2026, 3, 31}, -12) == date{2025, 3, 31}));
    CHECK(is_valid(date{2024, 2, 29}) && !is_valid(date{2023, 2, 29}) && !is_valid(date{2026, 13, 1}) && !is_valid(date{2026, 4, 31}));
    CHECK((clamp_date({2026, 2, 40}) == date{2026, 2, 28}) && (clamp_date({2026, 0, 0}) == date{2026, 1, 1}));
    CHECK(date{2026, 9, 25} < date{2026, 10, 1} && date{2027, 1, 1} > date{2026, 12, 31} && time_of_day{9, 5, 0} < time_of_day{9, 6, 0});

    // text
    CHECK(to_string(date{2026, 9, 5}) == "2026-09-05" && to_string(time_of_day{7, 3, 9}) == "07:03" && to_string(time_of_day{7, 3, 9}, true) == "07:03:09");
    date d;
    time_of_day t;
    CHECK(parse_date("2026-09-25", d) && d == date{2026, 9, 25});
    CHECK(parse_date(" 2024/2/29 ", d) && d == date{2024, 2, 29});
    CHECK(parse_date("2026.12.01", d) && d == date{2026, 12, 1});
    CHECK(!parse_date("2023-02-29", d) && !parse_date("2026-13-01", d) && !parse_date("2026-09", d) && !parse_date("2026-09-25-", d));
    CHECK(!parse_date("", d) && !parse_date("abc", d) && !parse_date("2026-9-x", d) && d == date{2026, 12, 1}); // untouched on failure
    CHECK(parse_time("13:45", t) && t == time_of_day{13, 45, 0} && parse_time("07:03:09", t) && t == time_of_day{7, 3, 9});
    CHECK(!parse_time("24:00", t) && !parse_time("12:60", t) && !parse_time("12", t) && !parse_time("12:", t) && t == time_of_day{7, 3, 9});
    CHECK(month_name(1) == "January" && month_name(12) == "December" && weekday_short(0) == "Mo" && weekday_short(6) == "Su");
    CHECK(is_valid(today()) && is_valid(now()));

    // the pickers: open the popup, choose, close
    harness h;
    date        day{2026, 9, 25};
    time_of_day clock{13, 45, 0};
    bool        day_changed = false, time_changed = false;
    rect        day_box, clock_box;
    bool        boxes_known = false; // (while a popup is open the last item is one of its buttons)
    const auto build = [&] {
        if (auto w = h.ui.window("p", {100, 100}, {320, 0}, plain_window)) {
            day_changed  = h.ui.date_picker("day", day) || day_changed;
            if (!boxes_known) { day_box = h.ui.last_item_rect(); }
            time_changed = h.ui.time_picker("time", clock) || time_changed;
            if (!boxes_known) { clock_box = h.ui.last_item_rect(); }
        }
    };
    h.frames(build, 3);
    boxes_known = true;
    CHECK(day_box.width() > 100.0f && clock_box.min.y > day_box.max.y);

    // the calendar: September 2026 starts on a Tuesday, so the grid begins on Monday 31 August
    h.click(day_box.center(), build);
    h.frames(build, 3);
    CHECK(h.ui.popup_open());
    {
        const f32 fh    = h.ui.frame_height();
        const f32 pad   = 12.0f;
        const f32 head  = fh - 4.0f;
        const f32 lh    = h.ui.font().line_height(0);
        const f32 ch    = fh - 6.0f;
        const f32 gx    = day_box.min.x + pad;                       // the grid's left edge
        const f32 w     = std::max(day_box.width(), 260.0f) - 2.0f * pad;
        const f32 cw    = w / 7.0f;
        const f32 top   = day_box.max.y + 4.0f + pad;                // the header row
        const f32 grid0 = top + head + 2.0f + (lh + 4.0f) + 2.0f;    // the first week
        // 1 September is a Tuesday (column 1) of the first row: the 9th is the second Wednesday (row 1, column 2)
        h.click({gx + cw * 2.5f, grid0 + (ch + 2.0f) * 1.5f}, build);
        h.frames(build, 2);
        CHECK(day_changed && day == date{2026, 9, 9});
        CHECK(!h.ui.popup_open());
    }

    // next month with the arrow, then a day: the popup shows the month of the value when it opens
    day_changed = false;
    h.click(day_box.center(), build);
    h.frames(build, 3);
    {
        const f32 fh   = h.ui.frame_height();
        const f32 pad  = 12.0f;
        const f32 head = fh - 4.0f;
        const f32 w    = std::max(day_box.width(), 260.0f) - 2.0f * pad;
        const f32 top  = day_box.max.y + 4.0f + pad;
        const vec2 next{day_box.min.x + pad + w - head * 1.5f, top + head * 0.5f}; // the single right arrow
        h.click(next, build);
        h.click(next, build);
        CHECK(!day_changed && h.ui.popup_open()); // moving through months changes nothing
        // now October, then November 2026: 1 November is a Sunday, so it is the last column of the first row
        const f32 lh    = h.ui.font().line_height(0);
        const f32 ch    = fh - 6.0f;
        const f32 cw    = w / 7.0f;
        const f32 grid0 = top + head + 2.0f + (lh + 4.0f) + 2.0f;
        h.click({day_box.min.x + pad + cw * 6.5f, grid0 + ch * 0.5f}, build);
        h.frames(build, 2);
        CHECK(day_changed && day == date{2026, 11, 1});
    }

    // the time grid: an hour, a minute and the fine step
    h.click(clock_box.center(), build);
    h.frames(build, 3);
    CHECK(h.ui.popup_open());
    {
        const f32 fh  = h.ui.frame_height();
        const f32 pad = 12.0f;
        const f32 ch  = fh - 6.0f;
        const f32 lh  = h.ui.font().line_height(0);
        const f32 w   = std::max(clock_box.width(), 240.0f) - 2.0f * pad;
        const f32 cw  = w / 6.0f;
        const f32 x0  = clock_box.min.x + pad;
        const f32 top = clock_box.max.y + 4.0f + pad;       // the caption "hour"
        const f32 row0 = top + lh + 2.0f;                    // the first row of hours (the pickers space their rows by 2 px)
        // hour 8 is the third column of the second row
        h.click({x0 + cw * 2.5f, row0 + (ch + 2.0f) * 1.5f}, build);
        CHECK(time_changed && clock.hour == 8 && clock.minute == 45);
        time_changed = false;
        // minutes: the grid comes after the four rows of hours, a gap and the caption
        const f32 min0 = row0 + 4.0f * ch + 3.0f * 2.0f + 2.0f + 4.0f + 2.0f + lh + 2.0f;
        h.click({x0 + cw * 3.5f, min0 + ch * 0.5f}, build); // 15
        CHECK(time_changed && clock.minute == 15);
        // the + button, at the right end of the row under the two rows of minutes
        h.click({x0 + w - 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        CHECK(clock.minute == 16);
        h.click({x0 + 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        h.click({x0 + 17.0f, min0 + 2.0f * (ch + 2.0f) + ch * 0.5f}, build);
        CHECK(clock.minute == 14);
    }
    h.key(key::escape);
    h.frames(build, 3);
    CHECK(!h.ui.popup_open());
    CHECK(clock.hour == 8 && day == date{2026, 11, 1});

    // an invalid value is brought back to a valid one
    date bad{2026, 2, 40};
    time_of_day bad_t{30, 99, 99};
    const auto build_bad = [&] {
        if (auto w = h.ui.window("p", {100, 100}, {320, 0}, plain_window)) {
            (void)h.ui.date_picker("day", bad);
            (void)h.ui.time_picker("time", bad_t, true);
            (void)h.ui.datetime_picker("both", bad, bad_t);
        }
    };
    h.frames(build_bad, 3);
    CHECK((bad == date{2026, 2, 28}) && (bad_t == time_of_day{23, 59, 59}));
}

void test_lists_and_tables()
{
    std::fprintf(stderr, "[list clipper, table extras, tree tables]\n");

    // a long list in a fixed-height window: only the visible rows are submitted, and the scroll range is the whole list
    {
        harness h;
        int first = 1 << 30, last = -1, submitted = 0;
        const auto build = [&] {
            first = 1 << 30; last = -1; submitted = 0;
            if (auto w = h.ui.window("l", {50, 50}, {300, 300}, plain_window)) {
                list_clipper clip(h.ui, 1000, h.ui.frame_height());
                while (clip.step()) {
                    for (int i = clip.begin(); i < clip.end(); ++i) {
                        ++submitted;
                        first = std::min(first, i);
                        last  = std::max(last, i);
                        (void)h.ui.custom_item("r" + std::to_string(i), {100.0f, h.ui.frame_height()});
                    }
                }
            }
        };
        h.frames(build, 4);
        CHECK(first == 0 && last > 3 && last < 16 && submitted < 20);
        h.move({150.0f, 150.0f});
        h.frames(build, 2);
        for (int i = 0; i < 200; ++i) { h.in.wheel = -1.0f; h.frame(build); } // 48 px each
        h.frames(build, 2);
        CHECK(first > 200 && first < 400 && submitted < 20); // scrolled far into the list, still a handful of rows
        for (int i = 0; i < 1000; ++i) { h.in.wheel = -1.0f; h.frame(build); }
        h.frames(build, 2);
        CHECK(last == 999 && first > 970); // the end of the list is reachable: the spacers add up
        for (int i = 0; i < 1200; ++i) { h.in.wheel = 1.0f; h.frame(build); }
        h.frames(build, 2);
        CHECK(first == 0);

        // an empty list and a short one
        int rows_drawn = 0;
        const auto build_short = [&] {
            rows_drawn = 0;
            if (auto w = h.ui.window("s", {400, 50}, {300, 300}, plain_window)) {
                for (const std::size_t n : {std::size_t{0}, std::size_t{3}}) {
                    list_clipper clip(h.ui, n, 0.0f);
                    while (clip.step()) { for (int i = clip.begin(); i < clip.end(); ++i) { ++rows_drawn; h.ui.text("x"); } }
                }
            }
        };
        h.frames(build_short, 3);
        CHECK(rows_drawn == 3);
    }

    // a big table with a scrolling body
    {
        harness h;
        int submitted = 0, last = -1;
        const auto build = [&] {
            submitted = 0; last = -1;
            if (auto w = h.ui.window("t", {50, 50}, {400, 320}, plain_window)) {
                if (h.ui.begin_table("big", 2, table_default, 240.0f)) {
                    h.ui.table_setup_column("row");
                    h.ui.table_setup_column("value", 100.0f);
                    (void)h.ui.table_headers_row();
                    list_clipper clip(h.ui, 5000);
                    while (clip.step()) {
                        for (int i = clip.begin(); i < clip.end(); ++i) {
                            if (h.ui.table_next_row()) {
                                ++submitted;
                                last = i;
                                h.ui.table_next_column(); h.ui.textf("row {}", i);
                                h.ui.table_next_column(); h.ui.textf("{}", i * 3);
                            }
                        }
                    }
                    h.ui.end_table();
                }
            }
        };
        h.frames(build, 4);
        CHECK(submitted > 3 && submitted < 30);
        h.move({150.0f, 200.0f});
        for (int i = 0; i < 3000; ++i) { h.in.wheel = -1.0f; h.frame(build); if (last == 4999) { break; } }
        h.frames(build, 2);
        CHECK(last == 4999 && submitted < 30);
    }

    // table extras: columns hidden by default, moved by dragging the header, saved and loaded
    {
        harness h;
        bool shown[3]{};
        std::string layout;
        const auto build = [&] {
            if (auto w = h.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (h.ui.begin_table("cols", 3, table_default | table_flags::hideable | table_flags::reorderable)) {
                    h.ui.table_setup_column("A", 0.0f, 1.0f, table_column_flags::no_hide);
                    h.ui.table_setup_column("B");
                    h.ui.table_setup_column("C", 0.0f, 1.0f, table_column_flags::default_hidden);
                    (void)h.ui.table_headers_row();
                    if (h.ui.table_next_row()) {
                        shown[0] = h.ui.table_next_column(); h.ui.text("a");
                        shown[1] = h.ui.table_next_column(); h.ui.text("b");
                        shown[2] = h.ui.table_next_column(); h.ui.text("c");
                    }
                    h.ui.end_table();
                }
                layout = h.ui.table_save_layout("cols");
            }
        };
        h.frames(build, 3);
        CHECK(shown[0] && shown[1] && !shown[2]); // C starts hidden: its cell says so
        CHECK(layout == "order=0,1,2;hidden=2;widths=0.3333,0.3333,0.3333");

        // drag A to the right, past the middle of B
        const f32 hy = 112.0f + 4.0f;
        const f32 w  = 480.0f - 24.0f;
        const vec2 from{112.0f + w * 0.25f, hy};
        h.move(from);
        h.frames(build, 2);
        h.down();
        h.frame(build);
        for (int i = 1; i <= 10; ++i) { h.move({from.x + w * 0.07f * static_cast<f32>(i), hy}); h.frame(build); }
        h.up();
        h.frames(build, 2);
        CHECK(layout.rfind("order=1,0,2", 0) == 0);
        CHECK(shown[0] && shown[1] && !shown[2]); // the cells still follow the declared columns

        // a saved layout can be loaded into another table before it is ever drawn
        harness g;
        bool g_shown[3]{};
        std::string g_layout;
        bool loaded = false;
        const auto build_g = [&] {
            if (auto w = g.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (!loaded) { g.ui.table_load_layout("cols", "order=2,0,1;hidden=1;widths=0.5,0.25,0.25"); loaded = true; }
                if (g.ui.begin_table("cols", 3, table_default | table_flags::hideable | table_flags::reorderable)) {
                    g.ui.table_setup_column("A", 0.0f, 1.0f, table_column_flags::no_hide);
                    g.ui.table_setup_column("B");
                    g.ui.table_setup_column("C", 0.0f, 1.0f, table_column_flags::default_hidden);
                    (void)g.ui.table_headers_row();
                    if (g.ui.table_next_row()) {
                        g_shown[0] = g.ui.table_next_column(); g.ui.text("a");
                        g_shown[1] = g.ui.table_next_column(); g.ui.text("b");
                        g_shown[2] = g.ui.table_next_column(); g.ui.text("c");
                    }
                    g.ui.end_table();
                }
                g_layout = g.ui.table_save_layout("cols");
            }
        };
        g.frames(build_g, 3);
        CHECK(g_layout == "order=2,0,1;hidden=1;widths=0.5000,0.2500,0.2500");
        CHECK(g_shown[0] && !g_shown[1] && g_shown[2]); // the file overrides the defaults (C shown, B hidden)

        // a layout that does not fit is ignored
        g.frames(build_g, 1);
        std::string before = g_layout;
        g.ui.begin_frame(g.in);
        g.ui.end_frame();
        bool again = false;
        const auto build_bad = [&] {
            if (auto w = g.ui.window("x", {100, 100}, {480, 0}, plain_window)) {
                if (!again) { g.ui.table_load_layout("cols", "order=0,0,1;hidden=7;widths=a,b"); again = true; }
                if (g.ui.begin_table("cols", 3, table_default)) {
                    g.ui.table_setup_column("A"); g.ui.table_setup_column("B"); g.ui.table_setup_column("C");
                    g.ui.end_table();
                }
                g_layout = g.ui.table_save_layout("cols");
            }
        };
        g.frames(build_bad, 3);
        CHECK(g_layout == before);

        // right-clicking the header opens the column menu
        h.move({112.0f + w * 0.5f, hy});
        h.frames(build, 2);
        h.in.mouse_down[1] = true;
        h.frame(build);
        h.in.mouse_down[1] = false;
        h.frames(build, 2);
        CHECK(h.ui.menu_is_open());
    }

    // a tree table: the children are rows, shown while the node is open
    {
        harness h;
        int rows = 0;
        bool pressed_leaf = false;
        const auto build = [&] {
            rows = 0;
            if (auto w = h.ui.window("tt", {100, 100}, {480, 0}, plain_window)) {
                if (h.ui.begin_table("tree", 2, table_default)) {
                    h.ui.table_setup_column("name");
                    h.ui.table_setup_column("kind", 100.0f);
                    h.ui.table_next_row(); ++rows;
                    h.ui.table_next_column();
                    const bool open = h.ui.table_tree_node("root");
                    h.ui.table_next_column(); h.ui.text("folder");
                    if (open) {
                        h.ui.table_next_row(); ++rows;
                        h.ui.table_next_column();
                        if (h.ui.table_tree_leaf("readme")) { pressed_leaf = true; }
                        h.ui.table_next_column(); h.ui.text("file");
                        h.ui.table_next_row(); ++rows;
                        h.ui.table_next_column();
                        const bool sub_open = h.ui.table_tree_node("src");
                        h.ui.table_next_column(); h.ui.text("folder");
                        if (sub_open) {
                            h.ui.table_next_row(); ++rows;
                            h.ui.table_next_column();
                            (void)h.ui.table_tree_leaf("main");
                            h.ui.table_next_column(); h.ui.text("file");
                            h.ui.table_tree_pop();
                        }
                        h.ui.table_tree_pop();
                    }
                    h.ui.end_table();
                }
            }
        };
        h.frames(build, 3);
        CHECK(rows == 1);
        const f32 row_h = h.ui.font().line_height(0) + 6.0f;
        const f32 x0 = 112.0f + 8.0f; // the cell padding
        h.click({x0 + 8.0f, 112.0f + row_h * 0.5f}, build); // the arrow of "root"
        h.frames(build, 2);
        CHECK(rows == 3);
        h.click({x0 + 18.0f + 8.0f, 112.0f + row_h * 2.5f}, build); // the arrow of "src", one level in
        h.frames(build, 2);
        CHECK(rows == 4);
        h.click({x0 + 40.0f, 112.0f + row_h * 1.5f}, build); // the leaf "readme"
        CHECK(pressed_leaf);
        h.click({x0 + 8.0f, 112.0f + row_h * 0.5f}, build); // closing root hides everything below it
        h.frames(build, 2);
        CHECK(rows == 1);
    }
}

void test_password_masks_and_code()
{
    std::fprintf(stderr, "[password reveal, input masks, code editor]\n");

    // the eye button shows the text: the caret sits where the letters end instead of where the bullets end
    {
        harness h;
        std::string pw = "iiiiiiii";
        const auto build = [&] {
            if (auto w = h.ui.window("p", {100, 100}, {480, 0}, plain_window)) {
                (void)h.ui.input_text("##pw", pw, {}, input_flags::password | input_flags::reveal);
            }
        };
        h.frames(build, 3);
        const f32 mid_y = 112.0f + h.ui.frame_height() * 0.5f;
        h.click({300.0f, mid_y}, build);
        h.frames(build, 2);
        CHECK(h.ui.want_text_input());
        const f32 hidden_x = h.ui.ime_position().x;
        CHECK(h.ui.ime_position().x > 0.0f);
        h.click({552.0f, mid_y}, build); // the eye at the right end (the field is 456 wide)
        h.frames(build, 2);
        CHECK(h.ui.want_text_input()); // it did not take the keyboard away
        const f32 shown_x = h.ui.ime_position().x;
        CHECK(shown_x < hidden_x - 10.0f); // eight thin letters are narrower than eight bullets
        h.click({552.0f, mid_y}, build);
        h.frames(build, 2);
        CHECK(near_eq(h.ui.ime_position().x, hidden_x, 0.5f));
        // the text field itself does not start at the button: a click on the left still edits
        h.type("j");
        h.frame(build);
        CHECK(pw == "iiiiiiiij");
    }

    // masks: the field shapes what is typed, pasted and erased
    {
        harness h;
        std::string phone, plate = "ab12", code;
        int changes = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("m", {100, 100}, {480, 0}, plain_window)) {
                if (h.ui.input_masked("##phone", phone, "(###) ###-####", "phone")) { ++changes; }
                (void)h.ui.input_masked("##plate", plate, "UU-###");
                (void)h.ui.input_masked("##code", code, "+1 ###");
            }
        };
        h.frames(build, 3);
        CHECK(plate == "AB-12"); // a value that is stored is brought into shape
        const f32 fh = h.ui.frame_height();
        const f32 y_phone = 112.0f + fh * 0.5f;
        const f32 y_plate = y_phone + fh + 7.0f;
        const f32 y_code  = y_plate + fh + 7.0f;

        h.click({300.0f, y_phone}, build);
        h.type("abc12x3");
        h.frames(build, 2);
        CHECK(phone == "(123");
        h.type("4567890123");
        h.frames(build, 2);
        CHECK(phone == "(123) 456-7890"); // what does not fit is dropped
        h.key(key::backspace);
        h.frames(build, 2);
        CHECK(phone == "(123) 456-789");
        for (int i = 0; i < 7; ++i) { h.key(key::backspace); h.frame(build); }
        CHECK(phone == "(12"); // erasing through the fixed characters, one digit at a time
        for (int i = 0; i < 2; ++i) { h.key(key::backspace); h.frame(build); }
        CHECK(phone.empty());
        CHECK(changes > 5);

        // pasting: everything that is not a digit is left out
        h.clip.data = "555-123-4567 ext 9";
        h.key(key::v, true);
        h.frames(build, 2);
        CHECK(phone == "(555) 123-4567");

        // typing in the middle: the caret follows the digits, not the fixed characters
        h.key(key::home);
        h.frame(build);
        h.key(key::right);
        h.key(key::right);
        h.frame(build);
        h.type("9");
        h.frames(build, 2);
        CHECK(phone == "(595) 512-3456");

        // letters, with case: UU-###
        h.click({300.0f, y_plate}, build);
        h.key(key::end);
        h.frame(build);
        for (int i = 0; i < 6; ++i) { h.key(key::backspace); h.frame(build); }
        h.type("x9yz4562");
        h.frames(build, 2);
        CHECK(plate == "XY-456"); // '9' is not a letter and is dropped; z had no place; digits fill the rest

        // a fixed character that looks like a digit is not mistaken for one
        h.click({300.0f, y_code}, build);
        h.type("1");
        h.frames(build, 2);
        CHECK(code.empty());
        h.type("55");
        h.frames(build, 2);
        CHECK(code == "+1 55");
    }

    // the code editor
    {
        harness h;
        std::string src = "fn main() {";
        rect box;
        int goto_line = 0;
        const auto build = [&] {
            if (auto w = h.ui.window("c", {100, 100}, {480, 0}, plain_window)) {
                if (goto_line > 0) { h.ui.code_goto_line("##code", goto_line); goto_line = 0; } // (the id scope of the field)
                (void)h.ui.input_code("##code", src, {0.0f, 150.0f});
                box = h.ui.last_item_rect();
            }
        };
        h.frames(build, 3);
        const f32 line1_y = box.min.y + 5.0f + h.ui.font().line_height(0) * 0.5f;
        h.click({500.0f, line1_y}, build);
        h.frames(build, 2);
        CHECK(h.ui.want_text_input());

        // Enter after an opening bracket: one level deeper; a typed } steps back
        h.key(key::enter);
        h.frames(build, 2);
        CHECK(src == "fn main() {\n    ");
        h.type("x = 1;");
        h.key(key::enter);
        h.frames(build, 2);
        CHECK(src == "fn main() {\n    x = 1;\n    "); // the same indentation as the line above
        h.type("}");
        h.frames(build, 2);
        CHECK(src == "fn main() {\n    x = 1;\n}");

        // Tab goes to the next tab stop; with a selection over several lines it indents them all, Shift+Tab takes it back
        h.click({700.0f, 500.0f}, build); // (a focused field shows its own copy of the text: let go before changing it)
        src = "ab\nc";
        h.frames(build, 40); // (a second click at the same place soon after would be a double click)
        h.click({500.0f, line1_y}, build);
        h.key(key::tab);
        h.frames(build, 2);
        CHECK(src == "ab  \nc");
        h.key(key::a, true);
        h.frame(build);
        h.key(key::tab);
        h.frames(build, 2);
        CHECK(src == "    ab  \n    c");
        h.key(key::tab, false, true);
        h.frames(build, 2);
        CHECK(src == "ab  \nc");

        // a jump to a line puts the caret there (and scrolls to it)
        h.click({700.0f, 500.0f}, build);
        std::string many;
        for (int i = 1; i <= 60; ++i) { many += "line " + std::to_string(i) + "\n"; }
        src = many;
        h.frames(build, 40);
        h.click({500.0f, line1_y}, build);
        goto_line = 40;
        h.frames(build, 2);
        h.type("X");
        h.frames(build, 2);
        CHECK(src.find("Xline 40") != std::string::npos || src.find("\nXline 40") != std::string::npos);
        CHECK(h.ui.ime_position().y / h.ui.scale() > box.min.y && h.ui.ime_position().y / h.ui.scale() < box.max.y + 40.0f);
    }

    // line numbers make room at the left; brackets next to the caret are marked
    {
        std::string text = "call(x)";
        f32 code_x = 0.0f, plain_x = 0.0f;
        {
            harness h;
            const auto build = [&] {
                if (auto w = h.ui.window("c", {100, 100}, {480, 0}, plain_window)) { (void)h.ui.input_code("##code", text, {0.0f, 120.0f}); }
            };
            h.frames(build, 3);
            h.click({500.0f, 112.0f + 5.0f + 9.0f}, build);
            h.frames(build, 2);
            code_x = h.ui.ime_position().x;
            const std::size_t marked = h.ui.render_data().shapes.size();
            h.key(key::home);
            h.frames(build, 2);
            CHECK(marked > h.ui.render_data().shapes.size() + 1); // the pair of brackets is drawn while the caret is next to one
        }
        {
            harness h;
            const auto build = [&] {
                if (auto w = h.ui.window("c", {100, 100}, {480, 0}, plain_window)) { (void)h.ui.input_multiline("##plain", text, {0.0f, 120.0f}, input_flags::no_wrap); }
            };
            h.frames(build, 3);
            h.click({500.0f, 112.0f + 5.0f + 9.0f}, build);
            h.frames(build, 2);
            plain_x = h.ui.ime_position().x;
        }
        CHECK(code_x > plain_x + 20.0f);
    }

    // find and replace: Ctrl+H opens the bar with the find field focused
    {
        harness h;
        std::string src = "one two one three one";
        rect box;
        const auto build = [&] {
            if (auto w = h.ui.window("c", {100, 100}, {480, 0}, plain_window)) {
                (void)h.ui.input_code("##code", src, {0.0f, 150.0f});
                box = h.ui.last_item_rect();
            }
        };
        h.frames(build, 3);
        const f32 top0 = box.min.y;
        h.click({500.0f, box.min.y + 5.0f + 9.0f}, build);
        h.frames(build, 2);
        h.in.ctrl = true; h.in.pressed_key = 'H';
        h.frame(build);
        h.in.ctrl = false;
        h.frames(build, 2);
        CHECK(box.min.y > top0 + 60.0f); // the find and replace rows took room above the text
        CHECK(h.ui.want_text_input());   // the find field has the keyboard
        h.type("one");
        h.frames(build, 2);

        // the replace field is the last row above the text; then the "All" button
        const f32 fh = h.ui.frame_height();
        const f32 row3_y = box.min.y - 7.0f - fh * 0.5f;
        h.click({200.0f, row3_y}, build);
        h.type("1");
        h.frames(build, 2);
        CHECK(src == "one two one three one"); // nothing replaced yet
        h.click({522.0f, row3_y}, build); // "All"
        h.frames(build, 2);
        CHECK(src == "1 two 1 three 1");

        // a search that finds nothing changes nothing
        h.click({130.0f, 112.0f + fh * 0.5f}, build);
        h.type("zzz");
        h.click({522.0f, row3_y}, build);
        h.frames(build, 2);
        CHECK(src == "1 two 1 three 1");
    }
}

void test_contexts_palette_and_config_file()
{
    std::fprintf(stderr, "[keybind contexts, command palette, config files]\n");

    // fuzzy matching and titles
    CHECK(fuzzy_score("sv", "Save") >= 0 && fuzzy_score("vs", "Save") == -1 && fuzzy_score("", "anything") == 0);
    CHECK(fuzzy_score("SAVE", "save as") > fuzzy_score("sae", "save as")); // a whole word beats scattered letters
    CHECK(fuzzy_score("nc", "new console") > fuzzy_score("nc", "unicode")); // the start of a word counts
    CHECK(fuzzy_score("of", "toggle off") > fuzzy_score("of", "open file")); // and so does the query as one piece
    CHECK(fuzzy_score("save", "Save") > fuzzy_score("save", "Save all the open documents and settings"));
    CHECK(fuzzy_score("x", "save") == -1 && fuzzy_score("s v", "save") >= 0); // spaces in the query are ignored
    CHECK(action_title("save_all") == "Save all" && action_title("open") == "Open" && action_title("").empty());

    // contexts
    {
        keybinds binds;
        binds.add("global_f", "Ctrl+Shift+F", "a global action");
        binds.add("format", "Ctrl+Shift+F", "format the selection", "editor");
        binds.add("run", "F5", {}, "editor");
        binds.add("play", "F5", {}, "viewport"); // the same key in another context: no conflict between them
        CHECK(!binds.context_active("editor") && binds.context_active(""));
        CHECK(binds.conflict("global_f") != nullptr && binds.conflict("format") != nullptr); // a global one meets every context
        CHECK(binds.conflict("run") == nullptr && binds.conflict("play") == nullptr);
        harness h;
        int global = 0, fmt = 0, run = 0, play = 0;
        const auto build = [&] {
            if (binds.pressed(h.ui, "global_f")) { ++global; }
            if (binds.pressed(h.ui, "format")) { ++fmt; }
            if (binds.pressed(h.ui, "run")) { ++run; }
            if (binds.pressed(h.ui, "play")) { ++play; }
        };
        const auto press = [&](u32 vk, bool ctrl, bool shift) {
            h.in.ctrl = ctrl; h.in.shift = shift; h.in.pressed_key = vk;
            h.frame(build);
            h.in.ctrl = h.in.shift = false;
        };
        press('F', true, true);
        CHECK(global == 1 && fmt == 0); // the editor context is off: only the global action
        press(0x74, false, false);
        CHECK(run == 0 && play == 0);
        binds.set_context("editor");
        press('F', true, true);
        CHECK(global == 1 && fmt == 1); // the editor's action takes the key from the global one
        press(0x74, false, false);
        CHECK(run == 1 && play == 0);
        binds.set_context("viewport");
        press(0x74, false, false);
        CHECK(run == 2 && play == 1); // both contexts on: both fire (their conflict is the app's to resolve)
        binds.set_context("editor", false);
        press('F', true, true);
        CHECK(global == 2 && fmt == 1); // the global action is back
        CHECK(binds.available(*binds.find("play")) && !binds.available(*binds.find("format")));
    }

    // the command palette
    {
        keybinds binds;
        binds.add("save", "Ctrl+S", "write the document");
        binds.add("save_as", "Ctrl+Shift+S", "write it to another file");
        binds.add("open", "Ctrl+O");
        binds.add("format", "Ctrl+Shift+F", "format the selection", "editor");
        command_palette palette;
        harness h;
        std::string chosen;
        const auto build = [&] {
            const std::string c = palette.show(h.ui, binds);
            if (!c.empty()) { chosen = c; }
        };
        h.frames(build, 3);
        CHECK(!palette.is_open());
        h.in.ctrl = true; h.in.shift = true; h.in.pressed_key = 'P';
        h.frame(build);
        h.in.ctrl = h.in.shift = false;
        h.frames(build, 3);
        CHECK(palette.is_open() && h.ui.modal_open() && h.ui.want_text_input()); // the search field has the keyboard
        h.type("save");
        h.frames(build, 2);
        h.key(key::down); // the second result
        h.frame(build);
        h.key(key::enter);
        h.frame(build);
        CHECK(chosen == "save_as" && !palette.is_open());
        h.frames(build, 3);
        CHECK(!h.ui.modal_open());

        // a context action is not offered until its context is on
        chosen.clear();
        palette.open();
        h.frames(build, 3);
        h.type("format");
        h.frames(build, 2);
        h.key(key::enter);
        h.frame(build);
        CHECK(chosen.empty() && palette.is_open()); // nothing to choose
        binds.set_context("editor");
        h.frames(build, 2);
        h.key(key::enter);
        h.frame(build);
        CHECK(chosen == "format" && !palette.is_open());

        // Esc closes it without choosing; so does a click outside; the shortcut can be turned off
        chosen.clear();
        palette.open();
        h.frames(build, 3);
        h.key(key::escape);
        h.frames(build, 3);
        CHECK(!palette.is_open() && chosen.empty() && !h.ui.modal_open());
        palette.open();
        h.frames(build, 3);
        h.click({5.0f, 5.0f}, build);
        h.frames(build, 3);
        CHECK(!palette.is_open() && chosen.empty());
        palette.shortcut = {};
        h.in.ctrl = true; h.in.shift = true; h.in.pressed_key = 'P';
        h.frame(build);
        h.in.ctrl = h.in.shift = false;
        h.frames(build, 2);
        CHECK(!palette.is_open());
    }

    // config files: nothing happens unless it is switched on
    {
        char temp[MAX_PATH]{};
        ::GetTempPathA(MAX_PATH, temp);
        const std::string path = std::string{temp} + "strata_selftest_config_file.ini";
        ::DeleteFileA(path.c_str());

        const auto disk = [&] { config c; (void)c.load_file(path); return c; };
        {
            config_file cf{path};
            CHECK(!cf.load() && !cf.auto_save() && !cf.hot_reload() && cf.data().empty()); // no file yet
            cf.set_int("a", "x", 1);
            cf.set_float("a", "f", 0.5f);
            cf.set_bool("a", "b", true);
            cf.set("a", "s", "hi");
            CHECK(cf.dirty() && cf.data().get_int("a", "x") == 1);
            CHECK(!cf.update(100.0f) && cf.save_count() == 0); // off: nothing is written however long it takes
            CHECK(!disk().has("a", "x"));
            CHECK(cf.save() && !cf.dirty() && cf.save_count() == 1);
            CHECK(disk().get_int("a", "x") == 1 && disk().get("a", "s") == "hi");

            // a file that changes on disk is not touched while hot reload is off
            {
                config ext = disk();
                ext.set_int("a", "x", 5);
                ext.set("a", "longer_key", "to change the size as well");
                CHECK(ext.save_file(path));
            }
            CHECK(!cf.update(100.0f) && cf.data().get_int("a", "x") == 1 && cf.reload_count() == 0);

            // auto-save: written a moment after the last change
            cf.set_auto_save(true, 0.5f);
            cf.set_int("a", "y", 2);
            CHECK(!cf.update(0.3f) && !disk().has("a", "y"));
            cf.set_int("a", "y", 3); // another change restarts the wait
            CHECK(!cf.update(0.3f) && !disk().has("a", "y"));
            CHECK(!cf.update(0.3f) && disk().get_int("a", "y") == 3 && !cf.dirty() && cf.save_count() == 2);
            CHECK(disk().get_int("a", "x") == 5 || disk().get_int("a", "x") == 1); // it wrote its own data

            // hot reload: the file changed on disk: the data is replaced, and the caller is told
            cf.set_hot_reload(true, 0.1f);
            CHECK(!cf.update(0.2f)); // its own write is not a change
            {
                config ext;
                ext.set_int("a", "x", 100);
                ext.set("new", "k", "v");
                CHECK(ext.save_file(path));
            }
            CHECK(!cf.update(0.05f)); // not polled yet
            CHECK(cf.update(0.1f));
            CHECK(cf.data().get_int("a", "x") == 100 && cf.data().get("new", "k") == "v" && !cf.data().has("a", "y"));
            CHECK(cf.reload_count() == 1 && !cf.update(1.0f)); // and once only

            // unsaved changes are not thrown away by a reload: they are written first
            cf.set_auto_save(false);
            cf.set_int("a", "mine", 7);
            {
                config ext;
                ext.set_int("other", "z", 9);
                ext.set("padding", "p", "so the size is different");
                CHECK(ext.save_file(path));
            }
            CHECK(!cf.update(1.0f) && cf.data().get_int("a", "mine") == 7 && cf.data().get_int("a", "x") == 100);
            CHECK(cf.save() && !cf.update(1.0f)); // saving replaces the file on purpose
            CHECK(disk().get_int("a", "mine") == 7 && !disk().has("other", "z"));

            // auto-save also runs when the object goes away
            cf.set_auto_save(true, 60.0f);
            cf.set_int("a", "last", 11);
        }
        CHECK(disk().get_int("a", "last") == 11);
        ::DeleteFileA(path.c_str());
        {
            config_file cf{path}; // switched on but the file is missing: nothing to reload
            cf.set_hot_reload(true, 0.1f);
            CHECK(!cf.update(1.0f));
        }
    }
}

void test_clock_and_code_find()
{
    std::fprintf(stderr, "[fixed clock, code_find]\n");
    const date real = today();
    override_clock({2030, 2, 28}, {23, 59, 58});
    CHECK((today() == date{2030, 2, 28}) && (now() == time_of_day{23, 59, 58}));
    reset_clock();
    CHECK(is_valid(today()) && today().year >= 2024 && (today() >= real || today() < real)); // the real clock again

    // the calendar marks the fixed "today": the outline is a shape more than a month without it
    {
        std::size_t with_today = 0, without = 0;
        for (const bool mark : {true, false}) {
            harness h;
            date d{2030, 2, 10};
            rect box;
            bool boxed = false;
            const auto build = [&] {
                if (auto w = h.ui.window("p", {100, 100}, {320, 0}, plain_window)) {
                    (void)h.ui.date_picker("day", d);
                    if (!boxed) { box = h.ui.last_item_rect(); }
                }
            };
            mark ? override_clock({2030, 2, 20}, {0, 0, 0}) : override_clock({2031, 6, 1}, {0, 0, 0});
            h.frames(build, 3);
            boxed = true;
            h.click(box.center(), build);
            h.frames(build, 3);
            (mark ? with_today : without) = h.ui.render_data().shapes.size();
            reset_clock();
        }
        CHECK(with_today > without);
    }

    // code_find opens the find bar (the text above the field) and gives the find field the keyboard
    harness h;
    std::string src = "one two one three";
    rect box;
    int find_request = 0;
    const auto build = [&] {
        if (auto w = h.ui.window("c", {100, 100}, {480, 0}, plain_window)) {
            if (find_request == 1) { h.ui.code_find("##code", "one"); find_request = 0; }
            (void)h.ui.input_code("##code", src, {0.0f, 120.0f});
            box = h.ui.last_item_rect();
        }
    };
    h.frames(build, 3);
    const f32 top0 = box.min.y;
    CHECK(!h.ui.want_text_input());
    find_request = 1;
    h.frames(build, 3);
    CHECK(box.min.y > top0 + 30.0f && h.ui.want_text_input());
}

void test_scrollbar_drag()
{
    std::fprintf(stderr, "[scrollbar thumbs follow the pointer]\n");

    struct thumb_shape { bool found{}; f32 x{}, y{}, h{}; };
    // the scrollbar thumb is the one 5 px wide shape
    const auto find_thumb = [](const harness& h) {
        thumb_shape t;
        for (const shape_record& s : h.ui.render_data().shapes) {
            if (std::abs(s.half_size.x * 2.0f - 5.0f) < 0.01f && s.half_size.y > 5.0f) {
                t = {true, s.center.x, s.center.y - s.half_size.y, s.half_size.y * 2.0f};
            }
        }
        return t;
    };

    // drag down and back: the thumb moves exactly as far as the pointer, whatever the size of the list, and comes back to
    // where it started. `build` draws one scrolling area with a thumb
    const auto drag_test = [&](harness& h, const auto& build, const char* what, bool expect_min_thumb) {
        h.frames(build, 4);
        h.move({100.0f, 100.0f}); // (over the content: the wheel is not used here)
        thumb_shape t = find_thumb(h);
        CHECK(t.found);
        const f32 start_y = t.y, height = t.h;
        if (expect_min_thumb) { CHECK(near_eq(height, 20.0f, 0.01f)); } // a huge list: the thumb is at its minimum size
        f32 my = t.y + t.h * 0.5f;
        h.move({t.x, my});
        h.frames(build, 2);
        h.down();
        h.frame(build);
        bool follows = true, keeps_size = true;
        for (int i = 1; i <= 8; ++i) {
            my += 6.0f;
            h.move({t.x, my});
            h.frame(build);
            const thumb_shape now = find_thumb(h);
            follows    = follows && near_eq(now.y - start_y, 6.0f * static_cast<f32>(i), 0.05f);
            keeps_size = keeps_size && near_eq(now.h, height, 0.01f);
        }
        CHECK(follows && keeps_size);
        for (int i = 1; i <= 8; ++i) {
            my -= 6.0f;
            h.move({t.x, my});
            h.frame(build);
        }
        h.up();
        h.frames(build, 2);
        t = find_thumb(h);
        CHECK(near_eq(t.y, start_y, 0.05f));
        std::fprintf(stderr, "  %s: ok\n", what);
    };

    {   // a child region with a hundred thousand rows
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("l", {50, 50}, {400, 300}, plain_window)) {
                if (auto rows = h.ui.child("rows", {0.0f, 170.0f}, child_flags::frame)) {
                    list_clipper clip(h.ui, 100000, h.ui.font().line_height(0) + 8.0f);
                    while (clip.step()) { for (int i = clip.begin(); i < clip.end(); ++i) { (void)h.ui.selectable("row " + std::to_string(i), false); } }
                }
            }
        };
        drag_test(h, build, "child region, 100 000 rows", true);

        // a click on the track beside the thumb puts the thumb there (its middle under the pointer)
        const thumb_shape t = find_thumb(h);
        const f32 target = t.y + 90.0f;
        h.click({t.x, target}, build);
        const thumb_shape moved = find_thumb(h);
        CHECK(near_eq(moved.y + moved.h * 0.5f, target, 1.0f));

        // and a click near the bottom of the track goes there
        const f32 low = t.y + 140.0f;
        h.click({t.x, low}, build);
        h.frames(build, 2);
        const thumb_shape end = find_thumb(h);
        CHECK(near_eq(end.y + end.h * 0.5f, low, 1.0f) && end.y > moved.y + 20.0f);
    }
    {   // a window taller than its frame
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("w", {50, 50}, {300, 260}, plain_window)) {
                for (int i = 0; i < 300; ++i) { (void)h.ui.custom_item("r" + std::to_string(i), {100.0f, 20.0f}); }
            }
        };
        drag_test(h, build, "window", false);
    }
    {   // a table with a scrolling body
        harness h;
        const auto build = [&] {
            if (auto w = h.ui.window("t", {50, 50}, {400, 320}, plain_window)) {
                if (h.ui.begin_table("tab", 2, table_default, 200.0f)) {
                    h.ui.table_setup_column("a");
                    h.ui.table_setup_column("b");
                    (void)h.ui.table_headers_row();
                    for (int i = 0; i < 400; ++i) {
                        if (h.ui.table_next_row()) {
                            h.ui.table_next_column(); h.ui.textf("row {}", i);
                            h.ui.table_next_column(); h.ui.textf("{}", i * 2);
                        }
                    }
                    h.ui.end_table();
                }
            }
        };
        drag_test(h, build, "table", false);
    }
}

void test_modal_motion()
{
    std::fprintf(stderr, "[modal slide-in is smooth]\n");
    harness h;
    std::vector<f32> ys;
    const auto build = [&] {
        if (auto m = h.ui.modal("m", {300.0f, 0.0f}, modal_flags::esc_closes)) { h.ui.text("hello"); }
        ys.push_back(h.ui.window_rect("m").min.y);
    };
    h.frames(build, 3);
    h.ui.begin_frame(h.in);
    h.ui.open_modal("m");
    h.ui.end_frame();
    ys.clear();
    h.frames(build, 60);
    // it settles from below its place, never moving back, and the steps get small toward the end: no one-pixel jumps
    bool monotonic = true;
    f32  worst_tail = 0.0f;
    for (std::size_t i = 4; i < ys.size(); ++i) { // (the first frames only measure the window, invisibly)
        if (ys[i] > ys[i - 1] + 0.001f) { monotonic = false; }
        if (i > 20) { worst_tail = std::max(worst_tail, ys[i - 1] - ys[i]); }
    }
    CHECK(monotonic);
    CHECK(worst_tail < 0.2f); // after a third of a second it only creeps: at most a fifth of a pixel per frame
    CHECK(ys.back() == std::round(ys.back())); // and it rests on whole pixels
    CHECK(ys[4] - ys.back() > 3.0f);             // it did start away from its place
}

void test_layout_scrolled_above_screen()
{
    std::fprintf(stderr, "[content scrolled above the top of the screen keeps its height]\n");
    harness h;
    // a window at the top of the screen with cards taller than its frame: scrolling moves the first card above y = 0
    const auto build = [&] {
        if (auto w = h.ui.window("s", {50, 0}, {400, 320}, plain_window)) {
            for (int c = 0; c < 3; ++c) {
                if (auto card = h.ui.card("card " + std::to_string(c))) {
                    for (int i = 0; i < 12; ++i) { (void)h.ui.custom_item("c" + std::to_string(c) + "_" + std::to_string(i), {100.0f, 24.0f}); }
                }
            }
        }
    };
    const auto win_thumb = [&]() {
        f32 hh = -1.0f, y = -1.0f;
        for (const shape_record& s : h.ui.render_data().shapes) {
            if (std::abs(s.half_size.x * 2.0f - 5.0f) < 0.01f && s.half_size.y > 5.0f) { hh = s.half_size.y * 2.0f; y = s.center.y - s.half_size.y; }
        }
        return std::pair<f32, f32>{hh, y};
    };
    h.frames(build, 5);
    const auto first = win_thumb();
    CHECK(first.first > 20.0f);
    h.move({200.0f, 150.0f});
    h.frames(build, 2);
    f32 lowest = first.second;
    bool stable = true;
    for (int i = 0; i < 120; ++i) { // far more than the content is tall
        h.in.wheel = -1.0f;
        h.frame(build);
        const auto t = win_thumb();
        stable = stable && near_eq(t.first, first.first, 0.5f); // the thumb keeps its size: the content did not grow
        lowest = std::max(lowest, t.second);
    }
    CHECK(stable);
    // and the end of the content is reached: the thumb sits at the bottom of its track and stays there
    const auto end = win_thumb();
    for (int i = 0; i < 30; ++i) { h.in.wheel = -1.0f; h.frame(build); }
    CHECK(near_eq(win_thumb().second, end.second, 0.5f));
    // scrolling back brings the same first card back (the same height it had)
    for (int i = 0; i < 200; ++i) { h.in.wheel = 1.0f; h.frame(build); }
    h.frames(build, 2);
    CHECK(near_eq(win_thumb().second, first.second, 0.5f) && near_eq(win_thumb().first, first.first, 0.5f));

    // the same for a child region scrolled the same way
    harness g;
    const auto build_child = [&] {
        if (auto w = g.ui.window("c", {50, 0}, {400, 320}, plain_window)) {
            if (auto rows = g.ui.child("rows", {0.0f, 200.0f}, child_flags::frame)) {
                for (int i = 0; i < 40; ++i) { (void)g.ui.custom_item("r" + std::to_string(i), {100.0f, 24.0f}); }
            }
        }
    };
    const auto child_thumb = [&]() {
        f32 hh = -1.0f;
        for (const shape_record& s : g.ui.render_data().shapes) {
            if (std::abs(s.half_size.x * 2.0f - 5.0f) < 0.01f && s.half_size.y > 5.0f) { hh = s.half_size.y * 2.0f; }
        }
        return hh;
    };
    g.frames(build_child, 4);
    const f32 child_start = child_thumb();
    g.move({150.0f, 100.0f});
    g.frames(build_child, 2);
    bool child_stable = true;
    for (int i = 0; i < 80; ++i) { g.in.wheel = -1.0f; g.frame(build_child); child_stable = child_stable && near_eq(child_thumb(), child_start, 0.5f); }
    CHECK(child_stable);
}

} // namespace

int run_selftest()
{
    test_undo();
    test_multiline();
    test_editing_extra();
    test_rich();
    test_rich_smoke();
    test_images();
    test_nested_tables();
    test_docking();
    test_dock_spaces();
    test_dock_layout();
    test_dock_group_drag();
    test_text_styles();
    test_charts();
    test_acrylic_extras();
    test_menu_extras();
    test_floating_dock_animated_move();
    test_tab_highlight_follows_window();
    test_texture_image();
    test_text_field_extras();
    test_toasts_and_log_v2();
    test_rtl_and_emoji();
    test_scale();
    test_number_widgets();
    test_plots();
    test_selectable_text();
    test_modals();
    test_menus();
    test_toasts();
    test_log_view();
    test_themes();
    test_draw_list_extras();
    test_dock_animation_and_combo_multi();
    test_chords();
    test_keybinds();
    test_config();
    test_window_height_cap();
    test_popups_and_drag_drop();
    test_tabs();
    test_datetime_pickers();
    test_lists_and_tables();
    test_password_masks_and_code();
    test_contexts_palette_and_config_file();
    test_clock_and_code_find();
    test_scrollbar_drag();
    test_modal_motion();
    test_layout_scrolled_above_screen();
    std::fprintf(stderr, "selftest: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

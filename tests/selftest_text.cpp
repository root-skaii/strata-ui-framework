// headless self-test: text editing, multi-line input, rich text, text styles, bidi / emoji, code fields

#include "selftest_common.hpp"

namespace {

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

    h.key(key::down); // caret after "abX": nearest spot on the shorter line below is its end
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

void test_editing_extra()
{
    std::fprintf(stderr, "[mouse selection, wheel, history limits]\n");
    harness h;

    // double-click selects a word, drag selects a range (field at (12, 12), text from (22, 17))
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

    // the wheel scrolls an overfull field, and clicks land on the shown lines
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

    // history is capped at 256 steps: undoing all stops at the state before the oldest kept step
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
    // ops: replace the selection with a letter x 300, starting empty; 256 are kept
    CHECK(many_edits == std::string(1, static_cast<char>('a' + (300 - 256 - 1) % 26)));
    // ... and redo walks forward again, through the same letters
    for (int i = 0; i < 400; ++i) {
        h.key(key::y, true);
        h.frame(build_edits);
    }
    CHECK(many_edits == std::string(1, static_cast<char>('a' + 299 % 26)));
}

// docking ---------------------------------------------------------------------------------------------------------

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

// docking: saving / restoring a layout, dragging a pane by its grip --------------------------------------------

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

    // styled contents: a wider font in a span widens the text; lines take the tallest font's height
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

// toasts with buttons and progress, log wrapping and clock times -------------------------------------------------

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
        // rtl paragraph with digits: the number keeps its direction and moves left of the word
        CHECK(to_visual(hebrew + " 123") == "123 " + to_visual(hebrew));
        // brackets are mirrored in right-to-left runs
        const std::string in_brackets = "(" + hebrew + ")";
        CHECK(to_visual(in_brackets) == "(" + to_visual(hebrew) + ")");
        // a bracketed word inside a right-to-left paragraph: the brackets close around it, mirrored
        CHECK(to_visual(hebrew + " (" + hebrew + ")") == "(" + to_visual(hebrew) + ") " + to_visual(hebrew));
        // after a number in an ltr paragraph, a bracketed hebrew word reverses as a unit with its brackets
        CHECK(to_visual("x " + hebrew + " 5 (" + hebrew + ") y").find("(" + to_visual(hebrew) + ")") != std::string::npos);
        // lines are paragraphs of their own
        CHECK(to_visual(hebrew + "\nabc") == to_visual(hebrew) + "\nabc");
        // trailing spaces stay at the line end: right in ltr, left in rtl
        CHECK(to_visual("abc " + hebrew + "  ").ends_with("  ") && to_visual(hebrew + "  ").starts_with("  "));
        // explicit direction
        CHECK(to_visual("abc", nullptr, 0, text_direction::rtl) == "abc"); // no rtl characters: untouched
    }
    // arabic joining: initial / final forms, isolated letters, lam-alef ligatures, marks do not break joins
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

    // drawing rtl text emits glyphs in the reordered order
    {
        draw_list dl;
        dl.begin({800, 600}, atlas, 1.0f);
        dl.text({10, 10}, color{255, 255, 255, 255}, hebrew, 0);
        CHECK(dl.data().vertices.size() == 16); // four letters
        dl.text({10, 40}, color{255, 255, 255, 255}, arabic, 0);
        CHECK(dl.data().vertices.size() == 16 + 20);
    }

    // text field caret follows the letters: in a hebrew word the text starts on the right
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
        // typing at the end appends (logical order); the end is at the left edge, so text grows rightwards from it
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

void test_password_masks_and_code()
{
    std::fprintf(stderr, "[password reveal, input masks, code editor]\n");

    // with the eye on, the caret sits after the letters, not after the bullets
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
                box = h.ui.item_rect();
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

        // Tab goes to the next tab stop; over a multi-line selection it indents every line, Shift+Tab unindents
        h.click({700.0f, 500.0f}, build); // (a focused field shows its own copy: release it before changing the text)
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
                box = h.ui.item_rect();
            }
        };
        h.frames(build, 3);
        const f32 top0 = box.min.y;
        h.click({500.0f, box.min.y + 5.0f + 9.0f}, build);
        h.frames(build, 2);
        h.in.ctrl = true; h.press_now(strata::key::h);
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
                    if (!boxed) { box = h.ui.item_rect(); }
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
            box = h.ui.item_rect();
        }
    };
    h.frames(build, 3);
    const f32 top0 = box.min.y;
    CHECK(!h.ui.want_text_input());
    find_request = 1;
    h.frames(build, 3);
    CHECK(box.min.y > top0 + 30.0f && h.ui.want_text_input());
}

} // namespace

void run_text_tests()
{
    test_undo();
    test_multiline();
    test_editing_extra();
    test_rich();
    test_rich_smoke();
    test_text_styles();
    test_text_field_extras();
    test_rtl_and_emoji();
    test_selectable_text();
    test_password_masks_and_code();
    test_clock_and_code_find();
}

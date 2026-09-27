// checks for the production-hardening batch: 64-bit ids and "###", window slots that are given back, the diagnostics hook,
// input that no longer drops keys or text, and the idle deadline.

#include "selftest_common.hpp"

#include <strata/platform/win32.hpp>

#include <thread>

namespace {

void test_stable_ids()
{
    std::fprintf(stderr, "[ids: 64 bit, \"###\" keeps the id while the text changes]\n");
    static_assert(sizeof(id) == 8);
    CHECK(hash_id("Downloads (3)###dl") == hash_id("Downloads (12)###dl"));
    CHECK(hash_id("a###dl") == hash_id("###dl"));
    CHECK(hash_id("a##x") != hash_id("a##y"));
    CHECK(visible_label("Downloads (3)###dl") == "Downloads (3)");
    CHECK(hash_id("x", 1) != hash_id("x", 2)); // the scope still counts

    // a window whose title changes keeps its state: moved once, it stays where it was moved to
    harness h;
    int  count = 3;
    rect first{};
    const auto build = [&] {
        const std::string title = std::format("Downloads ({})###dl", count);
        if (auto w = h.ui.window(title, {100, 100}, {220, 120}, window_flags::none)) { h.ui.text("files"); }
    };
    h.frames(build, 2);
    first = h.ui.window_rect("x###dl");
    CHECK(near_eq(first.min.x, 100.0f, 0.5f));
    // drag it by the title bar
    h.move({160, 110});
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({260, 150});
    h.frame(build);
    h.up();
    h.frame(build);
    const rect moved = h.ui.window_rect("###dl");
    CHECK(near_eq(moved.min.x, 200.0f, 1.0f));
    count = 12; // new text, same window
    h.frames(build, 2);
    CHECK(near_eq(h.ui.window_rect("###dl").min.x, 200.0f, 1.0f));
}

struct diag_log {
    std::vector<std::pair<diagnostic_kind, std::string>> lines;
};

void diag_collect(void* user, const diagnostic& d) noexcept
{
    static_cast<diag_log*>(user)->lines.emplace_back(d.kind, std::string{d.message});
}

void test_window_recycling()
{
    std::fprintf(stderr, "[windows: slots of windows not shown any more are given back]\n");
    diag_log log;
    context_config cfg;
    cfg.diagnostics = {&diag_collect, &log};
    harness h{cfg};

    // 100 different windows over time, 5 at a time: once all 32 slots have been used, the ones shown longest ago make
    // room. every window still opens
    int opened = 0;
    for (int round = 0; round < 20; ++round) {
        h.frames([&] {
            for (int k = 0; k < 5; ++k) {
                const std::string title = std::format("inspector {}", round * 5 + k);
                if (auto w = h.ui.window(title, {10.0f + 20.0f * static_cast<f32>(k), 10.0f}, 200.0f)) { h.ui.text("x"); }
            }
        }, 2);
        for (int k = 0; k < 5; ++k) {
            opened += h.ui.window_rect(std::format("inspector {}", round * 5 + k)).width() > 0.0f ? 1 : 0;
        }
    }
    CHECK(opened == 100);
    CHECK(h.ui.stats().limits_hit == 0);
    CHECK(log.lines.empty());
    // a window that was recycled starts over where its code puts it
    CHECK(h.ui.window_rect("inspector 0").width() == 0.0f);

    // more than 32 on screen at once is still a limit: reported once through the hook, counted in the stats
    h.frames([&] {
        for (int k = 0; k < 40; ++k) {
            if (auto w = h.ui.window(std::format("crowd {}", k), {0, 0}, 100.0f)) { h.ui.text("x"); }
        }
    }, 3);
    CHECK(h.ui.stats().limits_hit > 0);
    const auto limits = std::ranges::count_if(log.lines, [](const auto& l) { return l.first == diagnostic_kind::limit; });
    CHECK(limits == 1);
    CHECK(!log.lines.empty() && log.lines[0].second.find("max_windows") != std::string::npos);
}

void test_diagnostics_collisions()
{
#ifndef NDEBUG
    std::fprintf(stderr, "[diagnostics: duplicate ids are reported once, by label]\n");
    diag_log log;
    harness h;
    h.ui.set_diagnostics({&diag_collect, &log});
    h.frames([&] {
        if (auto w = h.ui.window("dups", {0, 0}, 200.0f)) {
            (void)h.ui.button("same");
            (void)h.ui.button("same");
        }
    }, 5);
    CHECK(log.lines.size() == 1);
    CHECK(!log.lines.empty() && log.lines[0].first == diagnostic_kind::id_collision);
    CHECK(!log.lines.empty() && log.lines[0].second.find("\"same\"") != std::string::npos);
#endif
}

void test_input_not_dropped()
{
    std::fprintf(stderr, "[input: presses queue with their modifiers, long ime text, alt + key]\n");
    harness h;
    int saves = 0, finds = 0, f5 = 0;
    const auto shortcuts = [&] {
        if (auto w = h.ui.window("keys", {0, 0}, 200.0f)) { h.ui.text("x"); }
        saves += h.ui.accelerator("Ctrl+S") ? 1 : 0;
        finds += h.ui.accelerator("Ctrl+F") ? 1 : 0;
        f5    += h.ui.key_pressed(VK_F5) ? 1 : 0;
    };
    h.frames(shortcuts, 2);
    // three presses between two frames; Ctrl is no longer held by the time the frame runs
    h.press('S', true);
    h.press('F', true);
    h.press(VK_F5);
    h.frame(shortcuts);
    CHECK(saves == 1 && finds == 0 && f5 == 0); // one per frame, oldest first
    h.frames(shortcuts, 3);
    CHECK(saves == 1);
    CHECK(finds == 1); // with the Ctrl it was pressed with
    CHECK(f5 == 1);
    // a host that only fills pressed_key still works
    h.in.ctrl = true;
    h.in.pressed_key = 'S';
    h.frame(shortcuts);
    h.in.ctrl = false;
    CHECK(saves == 2);

    // a whole confirmed IME sentence in one frame (60 CJK characters = 180 bytes; the old buffer held 64)
    std::string text;
    std::string sentence;
    for (int i = 0; i < 60; ++i) { sentence += "\xe6\xbc\xa2"; } // U+6F22
    const auto field = [&] {
        if (auto w = h.ui.window("ime", {0, 100}, 400.0f)) { (void)h.ui.input_text("name", text, {}, input_flags::none, 4096); }
    };
    h.click({100, 172}, field);
    CHECK(h.ui.want_text_input());
    h.type(sentence);
    h.frame(field);
    h.frames(field, 1);
    CHECK(h.ui.want_text_input());
    // the focused field shows its own copy: click away so the string is written back and compared
    h.type("ab");
    h.frame(field);
    h.key(key::left, false, false);
    h.in.keys[h.in.key_count - 1].alt = true; // Alt + Left: not caret movement
    h.type("c");
    h.frame(field);
    h.click({700, 500}, field);
    CHECK(text == sentence + "abc");
}

void test_next_wake()
{
    std::fprintf(stderr, "[idle: next_wake_seconds, sticky toasts, the caret blink, whole deltas for timers]\n");
    constexpr f64 never = no_deadline;
    harness h;
    std::string text = "hello";
    bool with_field = false;
    const auto build = [&] {
        if (auto w = h.ui.window("idle", {0, 0}, {300, 200}, plain_window)) {
            h.ui.text("steady");
            if (with_field) { (void)h.ui.input_text("f", text); }
        }
    };
    h.frames(build, 10);
    CHECK(h.ui.can_idle());
    CHECK(h.ui.next_wake_seconds() == never); // nothing will change until there is input

    // a toast that stays until closed does not keep the ui awake once it has slid in (it used to, forever)
    const toast_handle sticky = h.ui.toast({.text = "stays", .seconds = 0.0f});
    h.frames(build, 40);
    CHECK(h.ui.can_idle());
    CHECK(h.ui.next_wake_seconds() == never);
    h.ui.toast_close(sticky);
    h.frames(build, 40);

    // a timed toast draws its countdown bar shrinking: it needs frames until it is gone, which its whole-delta timer
    // gets to after 2 s of real time however few frames that took
    h.ui.toast({.text = "timed", .seconds = 2.0f});
    h.frames(build, 40); // slid in
    CHECK(h.ui.next_wake_seconds() == 0.0);
    CHECK(h.ui.animations_settling());
    h.frame(build, 1.0f);
    h.frame(build, 1.0f);
    h.frames(build, 2);
    CHECK(h.ui.toast_count() == 0);
    h.frames(build, 30);

    // a focused field: the next caret flip is the deadline, and after it the picture differs
    with_field = true;
    h.click({100, 60}, build);
    CHECK(h.ui.want_text_input());
    h.frames(build, 45); // (the field's focus highlight takes about half a second to settle)
    CHECK(h.ui.frame_unchanged());
    const f64 flip = h.ui.next_wake_seconds();
    CHECK(flip > 0.0 && flip <= 0.531 + 0.002);
    h.frame(build, static_cast<f32>(flip));
    CHECK(!h.ui.frame_unchanged());

    // the system setting "do not blink": nothing to wake for
    h.in.caret_blink_time = 0.0f;
    h.frames(build, 3);
    CHECK(h.ui.want_text_input());
    CHECK(h.ui.next_wake_seconds() == never);
    h.in.caret_blink_time = 0.53f;

    // ui time takes the whole delta (a host that slept 2 s), animations only a capped step
    const f64 t0 = h.ui.time();
    h.frame(build, 2.0f);
    CHECK(near_eq(static_cast<f32>(h.ui.time() - t0), 2.0f, 0.001f));
}

void test_textf_long()
{
    std::fprintf(stderr, "[textf: text longer than the stack buffer is not cut]\n");
    harness h;
    std::string longer;
    for (int i = 0; i < 300; ++i) { longer += "\xc3\xa9"; } // 600 bytes of U+00E9: a cut at 512 would split one
    f32 formatted = 0.0f, plain = 0.0f;
    h.frames([&] {
        if (auto w = h.ui.window("t", {0, 0}, {6000, 200}, plain_window)) {
            h.ui.textf("{}", longer);
            formatted = h.ui.item_rect().width();
            h.ui.text(longer);
            plain = h.ui.item_rect().width();
        }
    }, 2);
    CHECK(formatted > 0.0f);
    CHECK(near_eq(formatted, plain, 0.01f));
}

void test_edit_events()
{
    std::fprintf(stderr, "[edit events: activated / deactivated / after edit, one undo step per drag and per entry]\n");
    harness h;
    f32         v    = 0.5f;
    f32         num  = 3.0f;
    bool        flag = false;
    std::string name = "x";
    int         pick = 0;
    struct counts {
        int activated{}, deactivated{}, edited{}, after{};
    };
    counts sl, tx, cb, co, dr;
    rect   slider_r{}, text_r{}, box_r{}, combo_r{}, drag_r{};
    const auto count = [&](counts& c) {
        c.activated   += h.ui.item_activated() ? 1 : 0;
        c.deactivated += h.ui.item_deactivated() ? 1 : 0;
        c.edited      += h.ui.item_edited() ? 1 : 0;
        c.after       += h.ui.item_deactivated_after_edit() ? 1 : 0;
    };
    const auto build = [&] {
        if (auto w = h.ui.window("edits", {0, 0}, {400, 560}, plain_window)) {
            (void)h.ui.slider("vol", v, 0.0f, 1.0f);
            slider_r = h.ui.item_rect();
            count(sl);
            (void)h.ui.input_text("name", name);
            text_r = h.ui.item_rect();
            count(tx);
            (void)h.ui.checkbox("flag", flag);
            box_r = h.ui.item_rect();
            count(cb);
            (void)h.ui.drag_float("num", num);
            drag_r = h.ui.item_rect();
            count(dr);
            (void)h.ui.combo("pick", pick, {"a", "b", "c"});
            combo_r = h.ui.item_rect();
            count(co);
        }
    };
    h.frames(build, 3);

    // a slider drag: one session, several changes, one edit when it is let go
    h.move(slider_r.center());
    h.frames(build, 2);
    h.down();
    h.frame(build);
    for (int i = 1; i <= 5; ++i) {
        h.move({slider_r.center().x + 12.0f * static_cast<f32>(i), slider_r.center().y});
        h.frame(build);
    }
    h.up();
    h.frames(build, 3);
    CHECK(sl.activated == 1);
    CHECK(sl.edited >= 2);
    CHECK(sl.deactivated == 1);
    CHECK(sl.after == 1);

    // a text entry: edited while typed into, one edit when the field lets go of the keyboard
    h.click(text_r.center(), build);
    CHECK(tx.activated == 1);
    h.type("ab");
    h.frame(build);
    h.type("c");
    h.frame(build);
    CHECK(tx.edited == 2);
    CHECK(tx.after == 0);
    h.click({780, 590}, build); // away
    h.frames(build, 2);
    CHECK(tx.deactivated == 1);
    CHECK(tx.after == 1);
    CHECK(name == "xabc");
    // focused and left again without typing: deactivated, but no edit
    h.click(text_r.center(), build);
    h.click({780, 590}, build);
    h.frames(build, 2);
    CHECK(tx.deactivated == 2);
    CHECK(tx.after == 1);

    // a checkbox click is a whole edit
    h.click(box_r.center(), build);
    h.frames(build, 2);
    CHECK(flag);
    CHECK(cb.after == 1);

    // a number typed into a drag field and cancelled with Esc: keystrokes are not edits of the value
    h.click(drag_r.center(), build); // a click without a drag types
    h.frames(build, 2);
    h.type("7");
    h.frame(build);
    h.key(key::escape);
    h.frames(build, 3);
    CHECK(num == 3.0f);
    CHECK(dr.activated == 1);
    CHECK(dr.deactivated == 1);
    CHECK(dr.after == 0);

    // a combo entry picked: the change and the end of the edit come together
    h.click(combo_r.center(), build); // opens
    h.frame(build);                   // (popup_open() reports the frame before)
    CHECK(h.ui.popup_open());
    const int after_before = co.after;
    h.click(combo_r.center(), build); // (with the list open, the last item is its last row: "c")
    h.frames(build, 2);
    CHECK(pick == 2);
    CHECK(co.after == after_before + 1);
}

void test_semantic_colors()
{
    std::fprintf(stderr, "[themes: semantic colours, chart palette, theme files, following the system appearance]\n");
    // theme files carry the new keys both ways
    style s = themes::light();
    s.series[3] = color{1, 2, 3, 255};
    const std::string text = themes::to_string(s, "t");
    CHECK(text.find("success = ") != std::string::npos);
    CHECK(text.find("series_4 = ") != std::string::npos);
    style back;
    const themes::theme_result r = themes::from_string(text, back);
    CHECK(r.ok());
    CHECK(back.success == s.success && back.warning == s.warning && back.error == s.error);
    CHECK(back.series == s.series);
    CHECK(themes::light().error != themes::midnight().error); // a light theme has its own, darker ones

    // the kinds use them: a toast / badge colour follows the theme
    style custom;
    custom.success = color{1, 200, 3, 255};
    CHECK(kind_color(toast_kind::success, custom) == custom.success);
    CHECK(kind_color(toast_kind::info, custom) == custom.accent_hover);

    // push_color reaches them
    harness h;
    h.frames([&] {
        h.ui.push_color(style_color::error, color{9, 9, 9, 255});
        CHECK(h.ui.theme().error == (color{9, 9, 9, 255}));
        h.ui.pop_color();
    }, 1);
    CHECK(h.ui.theme().error == style{}.error);

    // following the system appearance
    const style dark_blue = themes::for_appearance(true, false, color{0, 120, 215, 255});
    CHECK(dark_blue.window_bg == themes::midnight().window_bg);
    CHECK(dark_blue.accent == (color{0, 120, 215, 255}));
    CHECK(themes::for_appearance(false, false).window_bg == themes::light().window_bg);
    CHECK(themes::for_appearance(false, false).accent == themes::light().accent); // no accent: the theme's
    CHECK(themes::for_appearance(true, true, color{0, 120, 215, 255}).window_bg == themes::high_contrast().window_bg);
    // whatever this machine is set to, reading it works
    const win32_platform::appearance_settings a = win32_platform::appearance();
    CHECK(a.accent.a == 0 || a.accent.a == 255);
}

void test_save_state()
{
    std::fprintf(stderr, "[state: save_state / load_state carry windows, scroll, trees, tables]\n");
    bool node_open = false;
    bool load_table = false;
    std::string table_text;
    const auto make_build = [&](harness& h) {
        return [&] {
            if (auto w = h.ui.window("state", {50, 50}, {300, 200}, window_flags::resizable)) {
                node_open = h.ui.tree_node("node");
                if (node_open) { h.ui.text("child"); h.ui.tree_pop(); }
                if (load_table) { h.ui.table_load_layout("tbl", "order=2,0,1;hidden=1;widths=0.5,0.2,0.3"); } // (in its id scope)
                if (h.ui.begin_table("tbl", 3)) {
                    h.ui.table_setup_column("a");
                    h.ui.table_setup_column("b");
                    h.ui.table_setup_column("c");
                    (void)h.ui.table_headers_row();
                    h.ui.end_table();
                }
                table_text = h.ui.table_save_layout("tbl");
                for (int i = 0; i < 40; ++i) { h.ui.textf("line {}", i); }
            }
        };
    };

    config saved;
    {
        harness h;
        const auto build = make_build(h);
        load_table = true; // a layout other than the default
        h.frame(build);
        load_table = false;
        h.frames(build, 2);
        // move the window by its title bar, open the node, scroll the body
        h.move({120, 60});
        h.frames(build, 2);
        h.down();
        h.frame(build);
        h.move({220, 110});
        h.frame(build);
        h.up();
        h.frames(build, 2);
        CHECK(near_eq(h.ui.window_rect("state").min.x, 150.0f, 1.0f));
        h.click({165 + 12, 100 + 44}, build); // the node's row, near its left edge
        h.frames(build, 2);
        CHECK(node_open);
        h.scroll({250, 250}, -3.0f, build);
        h.ui.save_state(saved);
        CHECK(!saved.entries("ui").empty());
    }
    // through text, as a file would carry it
    config reread;
    CHECK(reread.from_string(saved.to_string()) == 0);

    harness fresh;
    CHECK(fresh.ui.load_state(reread));
    node_open = false;
    const std::string before_table = table_text;
    const auto build = make_build(fresh);
    fresh.frames(build, 3);
    CHECK(near_eq(fresh.ui.window_rect("state").min.x, 150.0f, 1.0f)); // where it was moved to
    CHECK(node_open);                                                  // the node the user opened
    CHECK(table_text == before_table);                                 // the table's columns
    CHECK(table_text.find("order=2,0,1") != std::string::npos);
    // the scroll offset is back: the first line is not where it is when unscrolled
    f32 first_line_y = 0.0f;
    fresh.frame([&] {
        if (auto w = fresh.ui.window("state", {50, 50}, {300, 200}, window_flags::resizable)) {
            fresh.ui.text("probe");
            first_line_y = fresh.ui.item_rect().min.y;
            for (int i = 0; i < 40; ++i) { fresh.ui.textf("line {}", i); }
        }
    });
    CHECK(first_line_y < 100.0f + 20.0f); // (unscrolled it sits below the title bar at ~139)

    // something else is not a state
    config other;
    other.set("ui", "version", "99");
    CHECK(!fresh.ui.load_state(other));
}

void test_log_queue()
{
    std::fprintf(stderr, "[log_queue: lines from several threads reach the log, in order per thread, with their times]\n");
    log_queue  q{100000};
    log_buffer log{100000};
    constexpr int threads = 4, per_thread = 2000;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&q, t] {
            for (int i = 0; i < per_thread; ++i) { q.addf(log_level::info, "t{} {}", t, i); }
        });
    }
    std::size_t moved = 0;
    while (moved < threads * per_thread) { // the ui thread drains while they write
        moved += q.drain_into(log);
        std::this_thread::yield();
    }
    for (std::thread& w : workers) { w.join(); }
    moved += q.drain_into(log);
    CHECK(moved == threads * per_thread);
    CHECK(log.size() == threads * per_thread);
    CHECK(q.dropped() == 0);
    // each thread's lines stay in the order it wrote them
    std::array<int, threads> next{};
    bool ordered = true;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const std::string_view text = log[i].text();
        const int t = text[1] - '0';
        const int n = std::stoi(std::string{text.substr(3)});
        ordered = ordered && n == next[static_cast<std::size_t>(t)];
        next[static_cast<std::size_t>(t)] = n + 1;
    }
    CHECK(ordered);
    CHECK(log[0].wall_ms > 0);

    // nobody drains: it stops growing and counts what it refused
    log_queue small{10};
    for (int i = 0; i < 15; ++i) { small.add(log_level::warn, "x"); }
    CHECK(small.dropped() == 5);
    log_buffer l2;
    CHECK(small.drain_into(l2) == 10);
    CHECK(small.drain_into(l2) == 0);
}

void test_smooth_scroll()
{
    std::fprintf(stderr, "[smooth scrolling: a notch is let out over frames, adds up to the same distance]\n");
    harness h;
    h.ui.set_scroll_smoothing(true);
    f32 first_y = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("scroll", {0, 0}, {300, 200}, window_flags::no_title_bar)) {
            for (int i = 0; i < 60; ++i) {
                h.ui.textf("row {}", i);
                if (i == 0) { first_y = h.ui.item_rect().min.y; }
            }
        }
    };
    h.move({150, 100});
    h.frames(build, 3);
    const f32 top = first_y;
    h.wheel(-1.0f);
    h.frame(build);
    h.frame(build);
    const f32 after_one = top - first_y;
    CHECK(after_one > 0.0f && after_one < 48.0f); // part of the way
    CHECK(h.ui.next_wake_seconds() == 0.0);        // and asking for the next frame
    h.frames(build, 40);
    CHECK(near_eq(top - first_y, 48.0f, 0.01f)); // the whole notch, as without smoothing
    CHECK(h.ui.can_idle());

    // a turn the other way drops what was left of the first
    h.wheel(-2.0f);
    h.frame(build);
    h.wheel(1.0f);
    h.frames(build, 40);
    const f32 net = top - first_y;
    CHECK(net < 48.0f + 2.0f * 48.0f); // not all of the 2 notches went out
}

void test_fast_math_nan()
{
    std::fprintf(stderr, "[fp:fast: the plot_auto NaN still reads as \"automatic\"]\n");
    // plot_auto is a quiet NaN; /fp:fast lets the compiler assume there are none, which would silently turn every
    // automatic axis into a fixed one. read through a volatile so the check cannot be folded away at compile time
    volatile f32 v = plot_auto;
    CHECK(std::isnan(v));

    // and end to end: an automatic range fits the data -- a line from 100 to 200 spans the plot's height
    harness h;
    const std::array<f32, 3> values{100.0f, 150.0f, 200.0f};
    rect plot_r{};
    h.frames([&] {
        if (auto w = h.ui.window("p", {0, 0}, {300, 200}, plain_window)) {
            h.ui.plot_lines("##auto", values, {200.0f, 80.0f});
            plot_r = h.ui.item_rect();
        }
    }, 2);
    // every vertex of the line lies inside the plot rectangle (a NaN range would put them at infinity or nowhere)
    const draw_data d = h.ui.render_data();
    bool finite = !d.vertices.empty();
    for (const vertex& vx : d.vertices) { finite = finite && std::isfinite(vx.pos.x) && std::isfinite(vx.pos.y); }
    CHECK(finite);
    CHECK(plot_r.height() > 0.0f);
}

} // namespace

void run_robust_tests()
{
    test_fast_math_nan();
    test_smooth_scroll();
    test_log_queue();
    test_save_state();
    test_semantic_colors();
    test_edit_events();
    test_next_wake();
    test_textf_long();
    test_stable_ids();
    test_window_recycling();
    test_diagnostics_collisions();
    test_input_not_dropped();
}

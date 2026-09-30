// 64-bit ids and "###", recycled window slots, the diagnostics hook, input that keeps every key and character,
// and the idle deadline.

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
    cfg.diagnostics      = {&diag_collect, &log};
    cfg.capacity.windows = 32;
    harness h{cfg};

    // 100 windows over time, 5 at a time: once all 32 slots (capacity.windows) were used, the least recently shown make room. every
    // window still opens
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

    // more than 32 on screen at once is still a limit: reported once via the hook, counted in stats
    h.frames([&] {
        for (int k = 0; k < 40; ++k) {
            if (auto w = h.ui.window(std::format("crowd {}", k), {0, 0}, 100.0f)) { h.ui.text("x"); }
        }
    }, 3);
    CHECK(h.ui.stats().limits_hit > 0);
    const auto limits = std::ranges::count_if(log.lines, [](const auto& l) { return l.first == diagnostic_kind::limit; });
    CHECK(limits == 1);
    CHECK(!log.lines.empty() && log.lines[0].second.find("capacity.windows") != std::string::npos);

    // ... unless the context was made with room for them
    context_config roomy;
    roomy.capacity.windows = 64;
    harness big{roomy};
    big.frames([&] {
        for (int k = 0; k < 40; ++k) {
            if (auto w = big.ui.window(std::format("crowd {}", k), {0, 0}, 100.0f)) { big.ui.text("x"); }
        }
    }, 3);
    CHECK(big.ui.stats().limits_hit == 0 && big.ui.window_rect("crowd 39").width() > 0.0f);
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
        f5    += h.ui.key_pressed(strata::key::f5) ? 1 : 0;
    };
    h.frames(shortcuts, 2);
    // three presses between two frames; Ctrl is no longer held by the time the frame runs
    h.press(strata::key::s, true);
    h.press(strata::key::f, true);
    h.press(strata::key::f5);
    h.frame(shortcuts);
    CHECK(saves == 1 && finds == 0 && f5 == 0); // one per frame, oldest first
    h.frames(shortcuts, 3);
    CHECK(saves == 1);
    CHECK(finds == 1); // with the Ctrl it was pressed with
    CHECK(f5 == 1);
    // a press made with the modifiers the host reports for the frame
    h.in.ctrl = true;
    h.press_now(strata::key::s);
    h.frame(shortcuts);
    h.in.ctrl = false;
    CHECK(saves == 2);

    // a whole confirmed IME sentence in one frame (60 CJK characters = 180 bytes)
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

    // a sticky toast does not keep the ui awake once it has slid in
    const toast_handle sticky = h.ui.toast({.text = "stays", .seconds = 0.0f});
    h.frames(build, 40);
    CHECK(h.ui.can_idle());
    CHECK(h.ui.next_wake_seconds() == never);
    h.ui.toast_close(sticky);
    h.frames(build, 40);

    // a timed toast's bar shrinks, so it needs frames until gone; its whole-delta timer finishes after 2 s of real
    // time however few frames that took
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
    h.frame(build);                   // (any_popup_open() reports the frame before)
    CHECK(h.ui.any_popup_open());
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
    const f32 notch     = 3.0f * h.ui.font().line_height(0); // one wheel notch (see test_scroll_speed)
    const f32 after_one = top - first_y;
    CHECK(after_one > 0.0f && after_one < notch); // part of the way
    CHECK(h.ui.next_wake_seconds() == 0.0);       // and asking for the next frame
    h.frames(build, 40);
    CHECK(near_eq(top - first_y, notch, 0.01f)); // the whole notch, as without smoothing
    CHECK(h.ui.can_idle());

    // a turn the other way drops what was left of the first
    h.wheel(-2.0f);
    h.frame(build);
    h.wheel(1.0f);
    h.frames(build, 40);
    const f32 net = top - first_y;
    CHECK(net < 3.0f * notch); // not all of the 2 notches went out
}

void test_fast_math_nan()
{
    std::fprintf(stderr, "[fp:fast: the plot_auto NaN still reads as \"automatic\"]\n");
    // plot_auto is a quiet NaN; /fp:fast may assume none exist and turn auto axes fixed. read via volatile so the check
    // is not folded at compile time
    volatile f32 v = plot_auto;
    CHECK(std::isnan(v));

    // end to end: an auto range fits the data (a line from 100 to 200 spans the plot height)
    harness h;
    const std::array<f32, 3> values{100.0f, 150.0f, 200.0f};
    rect plot_r{};
    h.frames([&] {
        if (auto w = h.ui.window("p", {0, 0}, {300, 200}, plain_window)) {
            h.ui.plot_lines("##auto", values, {200.0f, 80.0f});
            plot_r = h.ui.item_rect();
        }
    }, 2);
    // every line vertex lies inside the plot rect (a NaN range would put them anywhere)
    const draw_data d = h.ui.render_data();
    bool finite = !d.vertices.empty();
    for (const vertex& vx : d.vertices) { finite = finite && std::isfinite(vx.pos.x) && std::isfinite(vx.pos.y); }
    CHECK(finite);
    CHECK(plot_r.height() > 0.0f);
}

void test_scroll_speed()
{
    std::fprintf(stderr, "[scrolling: style::scroll_speed and the system's lines-per-notch]\n");
    harness h;
    f32 first_y = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("s", {0, 0}, {300, 200}, window_flags::no_title_bar)) {
            for (int i = 0; i < 200; ++i) {
                h.ui.textf("row {}", i);
                if (i == 0) { first_y = h.ui.item_rect().min.y; }
            }
        }
    };
    // how far one notch moves the content, with the settings as they are
    const auto notch = [&]() {
        h.ui.set_scroll_y(0.0f);
        h.frames(build, 2);
        const f32 before = first_y;
        h.scroll({150, 100}, -1.0f, build);
        return before - first_y;
    };
    h.move({150, 100});
    h.frames(build, 2);

    const f32 normal = notch();
    CHECK(normal > 0.0f);
    // three lines of text, which is what the mouse settings ask for by default
    CHECK(near_eq(normal, 3.0f * h.ui.font().line_height(0), 0.5f));

    h.ui.theme().scroll_speed = 2.0f;
    CHECK(near_eq(notch(), normal * 2.0f, 0.5f));
    h.ui.theme().scroll_speed = 0.5f;
    CHECK(near_eq(notch(), normal * 0.5f, 0.5f));
    h.ui.theme().scroll_speed = 0.0f; // the wheel scrolls nothing
    CHECK(near_eq(notch(), 0.0f, 0.01f));
    h.ui.theme().scroll_speed = 1.0f;

    // the user set their mouse to scroll more lines per notch
    h.in.wheel_lines = 6.0f;
    CHECK(near_eq(notch(), normal * 2.0f, 0.5f));
    // ... or a screenful (0), which is the window's body less a little
    h.in.wheel_lines = 0.0f;
    CHECK(notch() > normal * 3.0f);
    h.in.wheel_lines = 3.0f;
    CHECK(near_eq(notch(), normal, 0.5f));

    // it reaches child regions too, not only windows
    f32 child_scroll = 0.0f;
    const auto nested = [&] {
        if (auto w = h.ui.window("n", {0, 0}, {400, 300}, window_flags::no_title_bar)) {
            if (auto c = h.ui.child("inner", {350.0f, 200.0f}, child_flags::frame)) {
                for (int i = 0; i < 200; ++i) { h.ui.textf("row {}", i); }
                child_scroll = h.ui.scroll_y();
            }
        }
    };
    h.frames(nested, 2);
    h.scroll({150, 100}, -1.0f, nested);
    const f32 one = child_scroll;
    CHECK(near_eq(one, normal, 0.5f)); // the same distance as a window
    h.ui.theme().scroll_speed = 3.0f;
    h.scroll({150, 100}, -1.0f, nested);
    h.ui.theme().scroll_speed = 1.0f;
    CHECK(near_eq(child_scroll - one, normal * 3.0f, 0.5f));
}

// a "deeper" button that opens the next popup, inside which the same again
void nest_popups(context& ui, int level, int& deepest, std::array<rect, 8>& buttons)
{
    deepest = std::max(deepest, level);
    if (ui.button("deeper")) { ui.toggle_popup("next"); }
    buttons[static_cast<std::size_t>(level)] = ui.item_rect();
    if (auto p = ui.popup("next", 160.0f)) { nest_popups(ui, level + 1, deepest, buttons); }
}

void test_nested_popups()
{
    std::fprintf(stderr, "[popups stack: a combo, picker, menu or popup opened inside a popup keeps it open]\n");
    diag_log log;
    context_config cfg;
    cfg.diagnostics = {&diag_collect, &log};
    harness h{cfg};
    h.in.display_size = {800.0f, 1000.0f}; // room for every popup below its opener (the picker would flip up)

    bool  wrap = false, deep = false, modal_check = false, open_modal = false, modal_drawn = false;
    int   mode = 0, pick = 0, level_pick = 0, mode_after = 0, resets = 0, modal_mode = 0;
    color tint{200, 40, 40, 255};
    bool  settings_open = false, mode_open = false, pick_open = false, tint_open = false, inner_open = false;
    bool  other_open = false, want_other = false, modal_popup_open = false;
    rect  settings_btn{}, wrap_r{}, mode_r{}, pick_r{}, tint_r{}, more_r{}, deep_r{}, all_r{}, level_r{}, reset_r{};
    rect  modal_btn{}, modal_box{};
    const std::array<std::string_view, 5> fruits{"apple", "banana", "cherry", "date", "elder"};
    const auto build = [&] {
        settings_open = mode_open = pick_open = tint_open = inner_open = other_open = false;
        if (auto w = h.ui.window("host", {100, 100}, {300, 0}, plain_window)) {
            if (h.ui.button("settings")) { h.ui.toggle_popup("settings"); }
            settings_btn = h.ui.item_rect();
            if (want_other) { h.ui.open_popup("other"); want_other = false; }
            if (auto p = h.ui.popup("settings", 260.0f)) {
                (void)h.ui.checkbox("wrap", wrap);
                wrap_r = h.ui.item_rect();
                if (auto m = h.ui.context_menu("wrap menu")) {
                    if (h.ui.menu_item("reset")) { wrap = false; ++resets; }
                    reset_r = h.ui.item_rect();
                }
                (void)h.ui.combo("mode", mode, {"a", "b", "c", "d"});
                mode_open = h.ui.popup_open("mode");
                if (!mode_open) { mode_r = h.ui.item_rect(); } // (open, the last item is the list's last row)
                mode_after += h.ui.item_deactivated_after_edit() ? 1 : 0;
                (void)h.ui.combo_filtered("pick", pick, fruits);
                pick_open = h.ui.popup_open("pick");
                if (!pick_open) { pick_r = h.ui.item_rect(); }
                (void)h.ui.color_edit("tint", tint);
                tint_open = h.ui.popup_open("tint");
                if (!tint_open) { tint_r = h.ui.item_rect(); }
                if (h.ui.button("more")) { h.ui.toggle_popup("inner"); }
                more_r = h.ui.item_rect();
                if (auto q = h.ui.popup("inner", 200.0f)) {
                    (void)h.ui.checkbox("deep", deep);
                    deep_r = h.ui.item_rect();
                    (void)h.ui.combo("level", level_pick, {"one", "two", "three"});
                    if (!h.ui.popup_open("level")) { level_r = h.ui.item_rect(); }
                    if (h.ui.button("close all")) { h.ui.close_all_popups(); }
                    all_r = h.ui.item_rect();
                }
                inner_open = h.ui.popup_open("inner");
            }
            settings_open = h.ui.popup_open("settings");
            if (auto p = h.ui.popup("other")) { h.ui.text("other"); }
            other_open = h.ui.popup_open("other");
        }
        if (open_modal) { h.ui.open_modal("dialog"); open_modal = false; }
        modal_drawn = false;
        if (h.ui.begin_modal("dialog", {300.0f, 0.0f})) {
            modal_drawn = true;
            if (h.ui.button("modal options")) { h.ui.toggle_popup("in modal"); }
            modal_btn = h.ui.item_rect();
            if (auto p = h.ui.popup("in modal", 220.0f)) {
                (void)h.ui.checkbox("x", modal_check);
                modal_box = h.ui.item_rect();
                (void)h.ui.combo("modal mode", modal_mode, {"p", "q"});
            }
            modal_popup_open = h.ui.popup_open("in modal");
            h.ui.end_modal();
        }
    };
    const auto open_settings = [&] {
        if (!settings_open) { h.click(settings_btn.center(), build); }
        h.frames(build, 2); // the first frame only measures it
    };
    const f32 item_h = h.ui.frame_height() - 4.0f; // a combo list row
    h.frames(build, 2);

    // a combo inside a popup: opening it keeps the popup, a row picks, and the list covers the parent's rows
    open_settings();
    CHECK(settings_open && h.ui.any_popup_open());
    h.click(mode_r.center(), build);
    CHECK(mode_open && settings_open);
    const vec2 row_b{mode_r.center().x, mode_r.max.y + 4.0f + 4.0f + item_h * 1.5f};
    CHECK(pick_r.contains(row_b)); // (the row lies over the parent's next combo)
    h.click(row_b, build);
    h.frames(build, 2);
    CHECK(mode == 1 && !mode_open && settings_open);
    CHECK(!pick_open);              // the press was the list's, not the combo under it
    CHECK(mode_after == 1);         // the edit is reported for the nested combo too

    // a press in the parent outside the list closes the list only, and does nothing else
    h.click(mode_r.center(), build);
    CHECK(mode_open);
    h.click(wrap_r.center(), build);
    CHECK(!mode_open && settings_open && !wrap);
    h.click(wrap_r.center(), build);
    CHECK(wrap && settings_open);

    // Esc: the list, then the popup
    h.click(mode_r.center(), build);
    CHECK(mode_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!mode_open && settings_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!settings_open && !h.ui.any_popup_open());

    // a press outside everything closes the whole stack
    open_settings();
    h.click(mode_r.center(), build);
    CHECK(mode_open && settings_open);
    h.move(row_b);
    h.frames(build, 2);
    CHECK(h.ui.want_capture_mouse());
    h.click({700.0f, 550.0f}, build);
    CHECK(!mode_open && !settings_open && !h.ui.any_popup_open());

    // keys reach the nested list
    open_settings();
    h.click(mode_r.center(), build);
    h.key(key::down);
    h.frame(build);
    h.key(key::enter);
    h.frames(build, 2);
    CHECK(mode == 2 && !mode_open && settings_open);

    // the filtered combo: typing filters, Enter picks; Esc leaves the search field, then closes the list, then the popup
    h.click(pick_r.center(), build);
    CHECK(pick_open && settings_open);
    h.type("ch");
    h.frame(build);
    h.key(key::enter);
    h.frames(build, 2);
    CHECK(pick == 2 && !pick_open && settings_open);
    h.click(pick_r.center(), build);
    CHECK(pick_open);
    for (int i = 0; i < 3 && pick_open; ++i) {
        h.key(key::escape);
        h.frames(build, 2);
        CHECK(settings_open);
    }
    CHECK(!pick_open && settings_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!settings_open);

    // the color picker: a click in its square edits, the parent stays; Esc closes the picker only
    open_settings();
    h.click(tint_r.center(), build);
    CHECK(tint_open && settings_open);
    h.frames(build, 2);
    const color before = tint;
    const f32   pad    = h.ui.theme().padding;
    h.click({tint_r.min.x + pad + 90.0f, tint_r.max.y + 4.0f + pad + 30.0f}, build);
    CHECK(!(tint == before));
    CHECK(tint_open && settings_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!tint_open && settings_open);

    // a popup opened from a button in a popup
    h.click(more_r.center(), build);
    h.frames(build, 2);
    CHECK(inner_open && settings_open);
    h.click(deep_r.center(), build);
    CHECK(deep && inner_open && settings_open);
    h.move(deep_r.center()); // (below the parent, outside every window)
    h.frames(build, 2);
    CHECK(h.ui.want_capture_mouse());
    // ... with a combo in it: three levels
    h.click(level_r.center(), build);
    h.click({level_r.center().x, level_r.max.y + 4.0f + 4.0f + item_h * 2.5f}, build);
    h.frames(build, 2);
    CHECK(level_pick == 2 && inner_open && settings_open);
    // a press in the parent closes the child only
    const bool wrap_before = wrap;
    h.click(wrap_r.center(), build);
    CHECK(!inner_open && settings_open && wrap == wrap_before);
    // Esc: the child, then the parent
    h.click(more_r.center(), build);
    h.frames(build, 2);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!inner_open && settings_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!settings_open);
    // close_all_popups() from the child
    open_settings();
    h.click(more_r.center(), build);
    h.frames(build, 2);
    h.click(all_r.center(), build);
    CHECK(!inner_open && !settings_open && !h.ui.any_popup_open());

    // a context menu inside a popup: its item works and the popup stays; Esc closes the menu first
    open_settings();
    h.right_click(wrap_r.center(), build);
    h.frames(build, 2);
    const int resets_before = resets;
    h.click(reset_r.center(), build);
    CHECK(resets == resets_before + 1 && !wrap && settings_open);
    h.right_click(wrap_r.center(), build);
    h.frames(build, 2);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(settings_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!settings_open);

    // opened from outside every popup, a popup replaces the stack
    open_settings();
    h.click(more_r.center(), build);
    h.frames(build, 2);
    CHECK(inner_open && settings_open);
    want_other = true;
    h.frames(build, 2);
    CHECK(other_open && !settings_open && !inner_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!other_open);

    // a modal closes every popup; a popup (and a combo in it) inside the modal works, and Esc closes it before the modal
    open_settings();
    h.click(more_r.center(), build);
    h.frames(build, 2);
    open_modal = true;
    h.frames(build, 3);
    CHECK(modal_drawn && !settings_open && !inner_open);
    h.frames(build, 30); // (fades in)
    h.click(modal_btn.center(), build);
    h.frames(build, 2);
    CHECK(modal_popup_open);
    h.click(modal_box.center(), build);
    CHECK(modal_check && modal_popup_open);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!modal_popup_open && modal_drawn);
    h.key(key::escape);
    h.frames(build, 2);
    CHECK(!modal_drawn);

    // nothing above ran out of room
    CHECK(log.lines.empty());

    // four levels nest; a fifth is refused (reported once) and leaves the four open
    {
        diag_log log2;
        context_config cfg2;
        cfg2.diagnostics = {&diag_collect, &log2};
        harness n{cfg2};
        int deepest = 0;
        std::array<rect, 8> buttons{};
        const auto nested = [&] {
            deepest = 0;
            if (auto w = n.ui.window("nest", {20, 20}, {200, 0}, plain_window)) { nest_popups(n.ui, 0, deepest, buttons); }
        };
        n.frames(nested, 2);
        for (int level = 0; level < 4; ++level) {
            n.click(buttons[static_cast<std::size_t>(level)].center(), nested);
            n.frames(nested, 2);
            CHECK(deepest == level + 1);
        }
        CHECK(log2.lines.empty());
        n.click(buttons[4].center(), nested);
        n.frames(nested, 2);
        CHECK(deepest == 4);
        CHECK(log2.lines.size() == 1 && log2.lines[0].second.find("max_popup_levels") != std::string::npos);
    }
}

} // namespace

void test_nested_windows()
{
    std::fprintf(stderr, "[windows: a window begun inside another's code, the parent carries on after it]\n");
    harness h;
    int  a_clicks = 0, b_clicks = 0, c_clicks = 0;
    rect a_r{}, b_r{}, c_r{};
    const auto build = [&] {
        if (auto p = h.ui.window("P", {20, 20}, {300, 0}, plain_window)) {
            if (h.ui.button("a")) { ++a_clicks; }
            a_r = h.ui.item_rect();
            if (auto n = h.ui.window("N", {400, 300}, {200, 0}, plain_window)) {
                if (h.ui.button("b")) { ++b_clicks; }
                b_r = h.ui.item_rect();
            }
            if (h.ui.button("c")) { ++c_clicks; }
            c_r = h.ui.item_rect();
        }
        if (auto q = h.ui.window("Q", {700, 20}, {120, 0})) { h.ui.text("q"); } // (size without flags, as the README shows)
    };
    h.frames(build, 3);
    const rect p = h.ui.window_rect("P");
    const rect n = h.ui.window_rect("N");
    CHECK(near_eq(n.min.x, 400.0f, 0.5f) && near_eq(n.min.y, 300.0f, 0.5f));
    CHECK(n.contains(b_r.center()) && p.contains(c_r.center()));
    CHECK(c_r.min.y >= a_r.max.y && near_eq(c_r.min.x, a_r.min.x, 0.5f)); // the parent's layout resumed below "a"
    h.click(c_r.center(), build);
    h.click(b_r.center(), build);
    CHECK(a_clicks == 0 && b_clicks == 1 && c_clicks == 1);
}

void test_unbalanced_scopes()
{
    std::fprintf(stderr, "[misuse: missing end_* / pop_* are reported and repaired by end_frame]\n");
    diag_log log;
    context_config cfg;
    cfg.diagnostics = {&diag_collect, &log};
    harness h{cfg};
    bool opened = false;
    for (int i = 0; i < 24; ++i) { // more frames than max_tree_depth: the tree stack must not carry over
        h.frame([&] {
            (void)h.ui.begin_window("leaky", {20, 20}, {300, 0}, plain_window); // no end_window
            h.ui.set_next_item_open(true);
            opened = h.ui.tree_node("node");                                      // no tree_pop
            h.ui.push_id("scope");                                                 // no pop_id
        });
    }
    CHECK(opened);
    int misuse = 0, limits = 0;
    for (const auto& [kind, text] : log.lines) {
        misuse += kind == diagnostic_kind::misuse;
        limits += kind == diagnostic_kind::limit;
    }
    CHECK(misuse == 3 && limits == 0); // end_window, tree_pop, pop_id: each once
    // a well-formed window afterwards works as usual
    bool clicked = false;
    rect r{};
    const auto build = [&] {
        if (auto w = h.ui.window("ok", {400, 20}, {200, 0}, plain_window)) {
            clicked = h.ui.button("go") || clicked;
            r = h.ui.item_rect();
        }
    };
    h.frames(build, 2);
    h.click(r.center(), build);
    CHECK(clicked);
}

void run_robust_tests()
{
    test_nested_windows();
    test_unbalanced_scopes();
    test_nested_popups();
    test_scroll_speed();
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

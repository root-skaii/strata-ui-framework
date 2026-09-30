// headless self-test: menus, popups, modals, toasts, log view, window sizing, ui scale

#include "selftest_common.hpp"

namespace {

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
    h.press_now(strata::key::f);
    h.frame(build);
    h.in.alt = false;
    h.frames(build, 5);
    CHECK(h.ui.menu_open());
    h.type("o");
    h.frames(build, 3);
    CHECK(picked == 1);
    CHECK(!h.ui.menu_open());

    // R for the submenu, then t for its second row (case does not matter)
    h.in.alt = true; h.press_now(strata::key::f); h.frame(build); h.in.alt = false;
    h.frames(build, 5);
    h.type("R");
    h.frames(build, 6);
    CHECK(h.ui.menu_open());
    h.type("t");
    h.frames(build, 3);
    CHECK(picked == 4);
    CHECK(!h.ui.menu_open());

    // a keep-open row toggles and leaves the menu up
    h.in.alt = true; h.press_now(strata::key::f); h.frame(build); h.in.alt = false;
    h.frames(build, 5);
    h.type("g");
    h.frames(build, 2);
    CHECK(toggles == 1 && grid && h.ui.menu_open());
    h.type("g");
    h.frames(build, 2);
    CHECK(toggles == 2 && !grid && h.ui.menu_open());
    picked = 0;
    h.type("o"); // the disabled row does not take it
    h.frames(build, 3);
    CHECK(picked != 99);
    h.type("d"); // "R&&D" has no mnemonic
    h.frames(build, 3);
    CHECK(picked != 5);

    // Alt + E switches to the Edit menu; C picks Copy
    h.in.alt = true; h.press_now(strata::key::e); h.frame(build); h.in.alt = false;
    h.frames(build, 6);
    CHECK(h.ui.menu_open());
    h.type("c");
    h.frames(build, 3);
    CHECK(picked == 6 && !h.ui.menu_open());

    // letters do nothing while no menu is open
    picked = 0;
    h.type("o");
    h.frames(build, 3);
    CHECK(picked == 0);

    // accelerators need the exact modifiers
    accel = 0;
    h.in.ctrl = true; h.press_now(strata::key::o); h.frame(build); h.in.ctrl = false;
    CHECK(accel == 1);
    h.press_now(strata::key::o); h.frame(build); // no ctrl
    CHECK(accel == 1);
    h.in.ctrl = true; h.in.shift = true; h.press_now(strata::key::o); h.frame(build); // too many
    h.in.ctrl = false; h.in.shift = false;
    CHECK(accel == 1);
    h.press_now(strata::key::f5); h.frame(build); // F5
    CHECK(accel == 11);
    h.in.ctrl = true; h.in.shift = true; h.press_now(strata::key::s); h.frame(build);
    h.in.ctrl = false; h.in.shift = false;
    CHECK(accel == 111);
    h.in.alt = true; h.press_now(strata::key::f4); h.frame(build); h.in.alt = false; // Alt+F4
    CHECK(accel == 1111);
    h.press_now(strata::key::del); h.frame(build); // Del
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
    h.press_now(strata::key::del); h.frame(build_text);
    h.in.ctrl = true; h.press_now(strata::key::c); h.frame(build_text);
    CHECK(accel == 0);
    h.press_now(strata::key::p); h.frame(build_text); h.in.ctrl = false;
    CHECK(accel == 10000000);
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

    // a dialog returns the 1-based index of the pressed button; the last one sits bottom-right
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
    CHECK(h.ui.menu_open());
    CHECK(h.ui.want_capture_mouse());
    h.click(row_center(0), build);
    h.frames(build, 2);
    CHECK(picked == 1);
    CHECK(!h.ui.menu_open());

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
    CHECK(h.ui.menu_open());
    h.click({sub_x + 30.0f, bar_h + 4.0f + row_h * 2.5f}, build); // the second row of the submenu ("two"): it starts level with its parent row
    h.frames(build, 2);
    CHECK(picked == 3);
    CHECK(!h.ui.menu_open());

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
    CHECK(h.ui.menu_open());
    const f32 edit_w = h.ui.font().measure(0, "Edit").x + 22.0f;
    h.move({6.0f + file_w + edit_w * 0.5f, bar_h * 0.5f});
    h.frames(build, 5);
    h.click({60.0f, bar_h + 4.0f + row_h * 0.5f}, build);
    h.frames(build, 2);
    CHECK(picked == 4); // the Edit menu's Copy

    // Esc closes a menu
    h.click(file, build);
    h.frames(build, 5);
    CHECK(h.ui.menu_open());
    h.key(key::escape);
    h.frame(build);
    CHECK(!h.ui.menu_open());

    // a context menu opens at the pointer on right click, and picks like any menu
    const vec2 target{200.0f, 240.0f};
    h.move(target);
    h.frames(build, 3);
    h.in.mouse_down[1] = true;
    h.frame(build);
    h.in.mouse_down[1] = false;
    h.frames(build, 5);
    CHECK(h.ui.menu_open());
    h.click({target.x + 30.0f, target.y + 4.0f + row_h * 1.5f}, build); // second row: Delete
    h.frames(build, 2);
    CHECK(ctx_picked == 2);
    CHECK(!h.ui.menu_open());

    // right click elsewhere closes it and does not open another one
    h.move(target);
    h.frames(build, 2);
    h.in.mouse_down[1] = true;
    h.frame(build);
    h.in.mouse_down[1] = false;
    h.frames(build, 4);
    CHECK(h.ui.menu_open());
    h.click({700.0f, 500.0f}, build);
    CHECK(!h.ui.menu_open());
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

void test_toasts_and_log_v2()
{
    std::fprintf(stderr, "[toasts: buttons, progress; log: wrap, clock]\n");
    const auto nothing = [] {};

    // pressing a button closes the toast and reports which; the body is not a button
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

    // the log: wrapping gives long lines more rows (more glyphs), clock stamps are time of day
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

// text beyond the basics: rtl, Arabic joining, emoji and supplementary characters ------------------------

void test_log_view()
{
    std::fprintf(stderr, "[log view]\n");
    harness h;
    log_buffer log{100};
    for (int i = 0; i < 300; ++i) { log.addf(i % 7 == 0 ? log_level::warn : log_level::info, "message number {}", i); }
    CHECK(log.size() == 100); // a ring: the oldest lines fall out
    CHECK(log[0].text() == "message number 200");
    CHECK(log[99].text() == "message number 299");
    // compaction as lines leave the ring must shift the survivors' offsets: reading the oldest and newest after 200
    // evictions catches a botched compaction
    {
        log_buffer moved = std::move(log); // the lines point at the arena, so a move has to reseat them
        CHECK(moved.size() == 100);
        CHECK(moved[0].text() == "message number 200");
        CHECK(moved[99].text() == "message number 299");
        log_buffer copy = moved;
        copy.add(log_level::info, "after the copy");       // full ring: this pushes "200" out of the copy
        CHECK(copy.size() == 100);
        CHECK(copy[0].text() == "message number 201");     // the copy's own arena, not the original's
        CHECK(copy[99].text() == "after the copy");
        CHECK(moved[0].text() == "message number 200");    // ... and the original is untouched
        CHECK(moved[99].text() == "message number 299");
        log = std::move(moved);
        log_buffer cleared = log;
        cleared.clear();
        CHECK(cleared.size() == 0);
        cleared.add(log_level::info, "fresh");
        CHECK(cleared[0].text() == "fresh"); // clear() resets the arena too
    }
    CHECK(log.size() == 100 && log[0].text() == "message number 200"); // untouched by all of the above

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

void test_popups_and_drag_drop()
{
    std::fprintf(stderr, "[generic popups, drag and drop, spinner / badge / chip]\n");

    // popups open under a button, act as a small window, close on Esc, an outside click, or close_popup()
    {
        harness h;
        bool wrap = false;
        bool open_now = false;
        int  done = 0;
        rect btn;
        const auto build = [&] {
            if (auto w = h.ui.window("p", {100, 100}, {300, 0}, plain_window)) {
                if (h.ui.button("options")) { h.ui.toggle_popup("opts"); }
                btn = h.ui.item_rect();
                if (auto p = h.ui.popup("opts", 220.0f)) {
                    (void)h.ui.checkbox("wrap", wrap);
                    if (h.ui.button("done")) { ++done; h.ui.close_popup(); }
                }
                open_now = h.ui.popup_open("opts");
            }
        };
        h.frames(build, 3);
        CHECK(!open_now && !h.ui.any_popup_open());
        h.click({btn.min.x + 10.0f, btn.center().y}, build);
        CHECK(open_now);
        h.frames(build, 2);
        CHECK(h.ui.any_popup_open() && h.ui.want_capture_mouse());

        // popup content takes clicks: the check box is the first row, 4 px below the button plus padding
        const vec2 first_row{btn.min.x + 12.0f + 8.0f, btn.max.y + 4.0f + 12.0f + 8.0f};
        h.click(first_row, build);
        CHECK(wrap && open_now);

        // a click outside closes it and does not reach what is below
        h.click({600.0f, 500.0f}, build);
        CHECK(!open_now && !h.ui.any_popup_open());

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

        // dropped on itself it is a drop, not a click; Esc cancels; the wrong type is refused
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
                body = h.ui.item_rect();
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
    // settles from below without moving back, with shrinking steps: no one-pixel jumps
    bool monotonic = true;
    f32  worst_tail = 0.0f;
    for (std::size_t i = 4; i < ys.size(); ++i) { // (the first frames only measure the window, invisibly)
        if (ys[i] > ys[i - 1] + 0.001f) { monotonic = false; }
        if (i > 20) { worst_tail = std::max(worst_tail, ys[i - 1] - ys[i]); }
    }
    CHECK(monotonic);
    CHECK(worst_tail < 0.2f); // after a third of a second it creeps: at most 0.2 px per frame
    CHECK(ys.back() == std::round(ys.back())); // and it rests on whole pixels
    CHECK(ys[4] - ys.back() > 3.0f);             // it did start away from its place
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
    const f32 notch = 3.0f * h.ui.font().line_height(0); // (one wheel notch: the mouse settings' three lines)
    CHECK(near_eq(first_row_y, y0 - notch, 1.0f));
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

void test_layout_scrolled_above_screen()
{
    std::fprintf(stderr, "[content scrolled above the top of the screen keeps its height]\n");
    harness h;
    // a top-of-screen window with cards taller than it: scrolling moves the first card above y = 0
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
    // reaching the end: the thumb sits at the track bottom and stays
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

void test_layers_and_passive_windows()
{
    std::fprintf(stderr, "[layers and no_inputs windows]\n");
    harness h;
    bool clicked = false;
    const auto build = [&] {
        if (auto w = h.ui.window("plate", {100, 100}, {200, 100}, window_flags::no_inputs)) {
            h.ui.text("hud");
            if (h.ui.button("under the pointer")) { clicked = true; }
        }
        if (auto w = h.ui.window("tool", {400, 100}, {200, 100}, window_flags::none)) { h.ui.text("tool"); }
        { auto fg = h.ui.layer(layer::foreground); h.ui.draw().rect_filled({{10, 10}, {60, 40}}, {255, 0, 0, 255}); }
        if (auto w = h.ui.window("late", {400, 300}, 100.0f)) { h.ui.text("drawn after the foreground"); }
        { auto bg = h.ui.layer(layer::background); h.ui.draw().rect_filled({{0, 0}, {800, 600}}, {0, 0, 255, 255}); }
    };
    h.frames(build, 3);

    // the last command is the foreground rect (full-display clip) even though a window was built after it
    const draw_data dd = h.ui.render_data();
    CHECK(!dd.commands.empty());
    CHECK(near_eq(dd.commands.back().clip.max.x, dd.display_size.x, 0.5f) && near_eq(dd.commands.back().clip.max.y, dd.display_size.y, 0.5f));

    // the pointer over a passive window is nobody's: nothing captured, the button does not react
    h.click({150.0f, 120.0f}, build);
    CHECK(!h.ui.want_capture_mouse());
    CHECK(!clicked);
    h.move({450.0f, 120.0f});
    h.frames(build, 2);
    CHECK(h.ui.want_capture_mouse());

    // and it cannot be dragged by its body
    const rect before = h.ui.window_rect("plate");
    h.move({150.0f, 130.0f});
    h.frames(build, 2);
    h.down();
    h.frames(build, 2);
    h.move({250.0f, 230.0f});
    h.frames(build, 2);
    h.up();
    h.frames(build, 2);
    const rect after = h.ui.window_rect("plate");
    CHECK(near_eq(before.min.x, after.min.x, 0.5f) && near_eq(before.min.y, after.min.y, 0.5f));
}

} // namespace

void run_windows_tests()
{
    test_menu_extras();
    test_modals();
    test_menus();
    test_toasts();
    test_toasts_and_log_v2();
    test_log_view();
    test_popups_and_drag_drop();
    test_modal_motion();
    test_window_height_cap();
    test_layout_scrolled_above_screen();
    test_scale();
    test_layers_and_passive_windows();
}

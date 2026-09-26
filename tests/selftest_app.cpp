// checks for the second round of app-shaped gaps: disabled items, raw keys and mouse buttons, window focus, text
// focus, plain text as an item, row accessories, multi-selection, the self-contained confirmation, tab identity,
// the filtered combo and the geometry an app should not have to re-derive

#include "selftest_common.hpp"

#include <windows.h>

namespace {

void test_disabled()
{
    harness h;
    int  clicks = 0;
    bool hovered = false;
    bool enabled_inside = true;
    rect box{};
    bool off = true;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            auto d = h.ui.disabled_if(off);
            enabled_inside = h.ui.item_enabled();
            if (h.ui.button("go")) { ++clicks; }
            box     = h.ui.item_rect();
            hovered = h.ui.item_hovered();
        }
    };
    h.frames(build, 2);
    CHECK(!enabled_inside);

    h.click(box.center(), build);
    CHECK(clicks == 0);      // a disabled button cannot be pressed ...
    CHECK(hovered);          // ... but it is still the thing under the pointer, so it can say why

    off = false;
    h.click(box.center(), build);
    CHECK(clicks == 1);
    CHECK(h.ui.item_enabled());

    // an enabled scope inside a disabled one stays disabled, and the pair stays balanced
    harness h2;
    int inner = 0;
    rect inner_box{};
    const auto nested = [&] {
        if (auto w = h2.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            h2.ui.begin_disabled(true);
            h2.ui.begin_disabled(false);
            if (h2.ui.button("inner")) { ++inner; }
            inner_box = h2.ui.item_rect();
            h2.ui.end_disabled();
            h2.ui.end_disabled();
            CHECK(h2.ui.item_enabled());
        }
    };
    h2.frames(nested, 2);
    h2.click(inner_box.center(), nested);
    CHECK(inner == 0);
}

void test_raw_keys_and_buttons()
{
    harness h;
    int del = 0, f2 = 0, dup = 0;
    bool held = false, middle = false, right_down = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (h.ui.key_pressed(VK_DELETE))  { ++del; }
            if (h.ui.key_pressed(VK_F2))      { ++f2; }
            if (h.ui.key_pressed('D', true))  { ++dup; }
            held       = h.ui.key_down(VK_SHIFT);
            middle     = h.ui.mouse_clicked(2);
            right_down = h.ui.mouse_down(1);
        }
    };
    h.frames(build, 2);
    CHECK(del == 0 && f2 == 0 && dup == 0);

    h.in.pressed_key = VK_DELETE;
    h.frame(build);
    CHECK(del == 1);

    h.in.pressed_key = VK_F2;
    h.frame(build);
    CHECK(f2 == 1);

    h.in.pressed_key = 'D';            // without Ctrl it is not the shortcut
    h.frame(build);
    CHECK(dup == 0);
    h.in.pressed_key = 'D';
    h.in.ctrl = true;
    h.frame(build);
    h.in.ctrl = false;
    CHECK(dup == 1);

    h.in.set_held(VK_SHIFT, true);
    h.frame(build);
    CHECK(held);
    h.in.set_held(VK_SHIFT, false);
    h.frame(build);
    CHECK(!held);

    h.in.mouse_down[1] = true;
    h.in.mouse_down[2] = true;
    h.frame(build);
    CHECK(right_down && middle);
    h.frame(build);
    CHECK(!middle);  // `clicked` is the edge only
    h.in.mouse_down[1] = false;
    h.in.mouse_down[2] = false;

    // a text field with the keyboard swallows the plain keys, so Delete does not destroy while a name is typed
    std::string name = "x";
    rect field{};
    const auto with_field = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (h.ui.key_pressed(VK_DELETE)) { ++del; }
            (void)h.ui.input_text("n", name);
            field = h.ui.item_rect();
        }
    };
    h.frames(with_field, 2);
    h.click(field.center(), with_field);
    CHECK(h.ui.want_text_input());
    const int before = del;
    h.in.pressed_key = VK_DELETE;
    h.frame(with_field);
    CHECK(del == before);
}

void test_window_focus()
{
    harness h;
    bool a_focused = false, b_focused = false;
    rect a_rect{}, b_rect{};
    const auto build = [&] {
        if (auto w = h.ui.window("A", {0.0f, 0.0f}, {200.0f, 150.0f}, plain_window)) {
            a_focused = h.ui.window_focused();
            (void)h.ui.custom_item("body", {0.0f, 80.0f});
            a_rect = h.ui.item_rect();
        }
        if (auto w = h.ui.window("B", {300.0f, 0.0f}, {200.0f, 150.0f}, plain_window)) {
            b_focused = h.ui.window_focused();
            (void)h.ui.custom_item("body", {0.0f, 80.0f});
            b_rect = h.ui.item_rect();
        }
    };
    h.frames(build, 3);

    h.click(a_rect.center(), build);
    h.frames(build, 2);
    CHECK(a_focused && !b_focused);
    CHECK(h.ui.is_window_focused("A") && !h.ui.is_window_focused("B"));

    h.click(b_rect.center(), build);
    h.frames(build, 2);
    CHECK(b_focused && !a_focused);
    CHECK(h.ui.is_window_focused("B"));
}

void test_text_focus()
{
    harness h;
    std::string value = "abc";
    bool want = false;
    bool focused_here = false;
    bool by_label = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (want) { h.ui.request_text_focus("n"); } // asked every frame: it must not fight the caret
            (void)h.ui.input_text("n", value);
            focused_here = h.ui.item_focused();
            by_label     = h.ui.field_focused("n"); // asked in the scope the field was submitted in
        }
    };
    h.frames(build, 2);
    CHECK(!h.ui.want_text_input());

    want = true;
    h.frames(build, 2);
    CHECK(h.ui.want_text_input());
    CHECK(by_label);
    CHECK(h.ui.focused_field() != 0);
    CHECK(focused_here);

    // the grant selects what was there, so the first keystroke replaces it -- and then typing carries on, which is
    // what the repeated request must not undo (it would re-select and every keystroke would replace the last)
    h.type("Z");
    h.frame(build);
    h.type("Y");
    h.frame(build);
    CHECK(value == "ZY");
}

void test_text_is_an_item()
{
    harness h;
    bool hovered = false;
    rect text_rect{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            (void)h.ui.button("before");
            h.ui.text("a plain label");
            text_rect = h.ui.item_rect();
            hovered   = h.ui.item_hovered();
        }
    };
    h.frames(build, 2);
    CHECK(text_rect.width() > 10.0f && text_rect.height() > 4.0f);

    h.move(text_rect.center());
    h.frames(build, 2);
    CHECK(hovered); // it used to answer for the button before it

    h.move({-500.0f, -500.0f});
    h.frames(build, 2);
    CHECK(!hovered);
}

void test_row_elide_and_truncated()
{
    harness h;
    bool cut_long = false, cut_short = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {160.0f, 300.0f}, plain_window)) {
            (void)h.ui.selectable("a name far too long to fit inside this narrow window", false);
            cut_long = h.ui.item_truncated();
            (void)h.ui.selectable("short", false);
            cut_short = h.ui.item_truncated();
        }
    };
    h.frames(build, 2);
    CHECK(cut_long);
    CHECK(!cut_short);

    // text_ellipsis says so too
    bool cut_text = false;
    h.frames([&] {
        if (auto w = h.ui.window("w2", {0.0f, 0.0f}, {160.0f, 300.0f}, plain_window)) {
            h.ui.text_ellipsis("another string that is far wider than the window it is drawn in");
            cut_text = h.ui.item_truncated();
        }
    }, 2);
    CHECK(cut_text);
}

void test_row_accessories()
{
    harness h;
    int  removed = 0;
    int  row_hits = 0;
    bool flag = false;
    rect row{}, trash{}, box{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            h.ui.set_next_item_gutter(56.0f);
            if (h.ui.selectable("row", false)) { ++row_hits; }
            row = h.ui.item_rect();
            if (h.ui.row_accessory_button(0, "x")) { ++removed; }
            trash = h.ui.item_rect();
            (void)h.ui.row_accessory_checkbox("vis", flag);
            box = h.ui.item_rect();
        }
    };
    h.frames(build, 2);

    // they sit inside the row, right to left, and do not overlap each other
    CHECK(trash.max.x <= row.max.x + 0.5f);
    CHECK(box.max.x <= trash.min.x + 0.5f);
    CHECK(trash.min.y >= row.min.y - 0.5f && trash.max.y <= row.max.y + 0.5f);

    // a press on one goes to it, not to the row underneath
    h.click(trash.center(), build);
    CHECK(removed == 1);
    CHECK(row_hits == 0);

    h.click(box.center(), build);
    CHECK(flag);
    CHECK(row_hits == 0);

    // the row itself still works where there is no accessory
    h.click({row.min.x + 10.0f, row.center().y}, build);
    CHECK(row_hits == 1);
}

void test_selection()
{
    harness h;
    selection_state sel;
    const auto click_on = [&](int index, bool ctrl, bool shift) {
        h.in.ctrl  = ctrl;
        h.in.shift = shift;
        h.frame([&] {
            if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
                (void)h.ui.selection_click(sel, index);
            }
        });
        h.in.ctrl  = false;
        h.in.shift = false;
    };

    click_on(3, false, false);
    CHECK(sel.size() == 1 && sel.contains(3) && sel.anchor() == 3);

    click_on(5, true, false);                       // ctrl adds
    CHECK(sel.size() == 2 && sel.contains(3) && sel.contains(5));

    click_on(5, true, false);                       // ... and toggles off again
    CHECK(sel.size() == 1 && !sel.contains(5));

    click_on(1, false, false);
    click_on(4, false, true);                       // shift takes the range from the anchor
    CHECK(sel.size() == 4 && sel.contains(1) && sel.contains(2) && sel.contains(3) && sel.contains(4));
    CHECK(sel.anchor() == 1);                       // and the range keeps growing from where it started
    click_on(2, false, true);
    CHECK(sel.size() == 2 && sel.contains(1) && sel.contains(2));

    click_on(9, false, false);
    CHECK(sel.size() == 1 && sel.contains(9));
    sel.clamp_to(5);
    CHECK(sel.empty());
}

void test_confirm()
{
    harness h;
    int  answered = 0;
    u64  about    = 0;
    bool never    = false;
    bool ask      = false;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (ask) { h.ui.ask_confirm("del", "Delete it?", 42); ask = false; }
        }
        const int r = h.ui.confirm("del", {"Delete", "Cancel"}, {.remember = &never});
        if (r != 0) {
            answered = r;
            about    = h.ui.confirm_data();
        }
    };
    h.frames(build, 2);
    CHECK(answered == 0 && !h.ui.modal_open());

    ask = true;
    h.frames(build, 3);
    CHECK(h.ui.modal_open());   // it opened itself, with no "is it open" flag in the caller

    h.key(key::escape);
    h.frames(build, 3);
    CHECK(answered == -1);
    CHECK(!h.ui.modal_open());

    // with "don't ask again" set it never opens and answers straight away
    never    = true;
    answered = 0;
    ask      = true;
    h.frames(build, 2);
    CHECK(answered == 1);
    CHECK(about == 42);
    CHECK(!h.ui.modal_open());
}

void test_tab_identity()
{
    harness h;
    int selected = 1;
    std::string second = "notes.txt";
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {400.0f, 300.0f}, plain_window)) {
            const std::array<tab_desc, 3> tabs = {
                tab_desc{"main.cpp", {}, "doc0"},
                tab_desc{std::string_view{second}, {}, "doc1"},
                tab_desc{"README", {}, "doc2"},
            };
            (void)h.ui.tab_bar("docs", tabs.data(), tabs.size(), selected);
        }
    };
    h.frames(build, 3);
    CHECK(selected == 1);

    // the caption gains a dirty marker: with an explicit id the tab keeps its place and its state
    second = "notes.txt *";
    h.frames(build, 3);
    CHECK(selected == 1);
}

void test_geometry_and_clipboard()
{
    harness h;
    rect inner{};
    f32  width_inside = 0.0f;
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (auto c = h.ui.child("list", {0.0f, 120.0f})) {
                inner        = h.ui.content_rect();
                width_inside = h.ui.content_width();
                for (int i = 0; i < 40; ++i) { (void)h.ui.selectable("row", {reinterpret_cast<const char*>(&i), sizeof(i)}, false); }
            }
        }
    };
    h.frames(build, 3);
    CHECK(near_eq(inner.height(), 120.0f - 2.0f * h.ui.theme().padding * 0.7f, 2.0f));
    CHECK(context::scrollbar_width() > 0.0f);
    // the list overflows, so the scrollbar's room has already come off the content width
    CHECK(width_inside <= inner.width() - context::scrollbar_width() + 0.5f);

    CHECK(h.ui.copy_text("hello from the app"));
    std::string back;
    CHECK(h.ui.paste_text(back) && back == "hello from the app");
}

void test_tree_arrow_hit()
{
    harness h;
    bool arrow = false;
    bool pressed = false;
    rect row{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (h.ui.tree_node("node", tree_flags::arrow_only)) { h.ui.tree_pop(); }
            row     = h.ui.item_rect();
            pressed = h.ui.item_pressed();
            if (pressed) { arrow = h.ui.item_arrow_hit(); }
        }
    };
    h.frames(build, 2);

    h.click({row.min.x + 6.0f, row.center().y}, build);
    CHECK(pressed && arrow);

    h.click({row.max.x - 20.0f, row.center().y}, build);
    CHECK(pressed && !arrow);
}

void test_combo_filtered()
{
    harness h;
    const std::array<std::string_view, 5> items = {"alpha", "beta", "gamma", "delta", "epsilon"};
    int current = 0;
    int changes = 0;
    rect field{};
    const auto build = [&] {
        if (auto w = h.ui.window("w", {0.0f, 0.0f}, {300.0f, 300.0f}, plain_window)) {
            if (h.ui.combo_filtered("pick", current, items.data(), items.size())) { ++changes; }
            field = h.ui.item_rect();
        }
    };
    h.frames(build, 2);
    CHECK(!h.ui.popup_open());

    h.click(field.center(), build);
    h.frames(build, 2);
    CHECK(h.ui.popup_open());          // the popup is up with the keyboard in the search field
    CHECK(h.ui.want_text_input());

    h.type("del");                     // narrows to "delta"
    h.frames(build, 2);
    h.key(key::enter);
    h.frames(build, 3); // the popup was drawn on the frame the key arrived; popup_open() lags one more
    CHECK(current == 3);
    CHECK(changes == 1);
    CHECK(!h.ui.popup_open());
}

} // namespace

void run_app_tests()
{
    test_disabled();
    test_raw_keys_and_buttons();
    test_window_focus();
    test_text_focus();
    test_text_is_an_item();
    test_row_elide_and_truncated();
    test_row_accessories();
    test_selection();
    test_confirm();
    test_tab_identity();
    test_geometry_and_clipboard();
    test_tree_arrow_hit();
    test_combo_filtered();
}

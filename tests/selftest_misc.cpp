// headless self-test: acrylic, themes, draw list, key chords and binds, config files

#include "selftest_common.hpp"

namespace {

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
    // closed: one more segment, and one more point's worth of vertices -- the strip is walked as segs + 1 positions
    // (the last repeating the first) so that it can be split across draw commands, which costs the 4 vertices of the
    // repeat instead of indexing back to position 0. same segments, same pixels.
    dl.polyline(pts, color{255, 255, 255, 255}, 3.0f, true);
    CHECK(dl.data().vertices.size() == 16 + 20 && dl.data().indices.size() == 3 * 18 + 4 * 18);
    const std::array<vec2, 2> dup = {{{5, 5}, {5, 5}}};
    dl.polyline(dup, color{255, 255, 255, 255}, 2.0f); // no direction: nothing to draw, nothing broken
    CHECK(dl.data().vertices.size() == 16 + 20);

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

void test_key_sequences()
{
    std::fprintf(stderr, "[key sequences: Ctrl+K, Ctrl+S - style multi-key chords]\n");

    // text round trip
    key_sequence seq;
    CHECK((sequence_from_string("Ctrl+K, Ctrl+S", seq) && seq.count == 2));
    CHECK((seq.steps[0] == key_chord{'K', true, false, false} && seq.steps[1] == key_chord{'S', true, false, false}));
    CHECK(sequence_to_string(seq) == "Ctrl+K, Ctrl+S");
    CHECK((sequence_from_string("F5", seq) && seq.count == 1 && seq.steps[0] == key_chord{0x74, false, false, false}));
    CHECK(sequence_to_string(seq) == "F5");
    CHECK((sequence_from_string("", seq) && !seq.bound() && sequence_to_string(seq).empty()));
    CHECK(sequence_from_string("Ctrl+K,   Ctrl+O  ,Alt+F4", seq) && seq.count == 3); // extra spaces around the commas
    key_sequence keep = seq;
    CHECK(!sequence_from_string("Ctrl+K, Ctrl+O, Alt+F4, Ctrl+X", seq) && seq == keep); // too many steps: untouched
    CHECK(!sequence_from_string("Ctrl+K, Nonsense", seq) && seq == keep);              // a bad step: untouched
    CHECK(!sequence_from_string("Ctrl+K, ", seq));                                     // a trailing comma with nothing after it
    // a plain key_chord is a one-step sequence
    const key_chord single{'A', true, false, false};
    CHECK((key_sequence{single}.count == 1 && key_sequence{single}.steps[0] == single));
    CHECK(key_sequence{}.count == 0 && !key_sequence{}.bound());

    // sequence_pressed: needs every step, each within key_sequence_timeout of the one before
    {
        harness h;
        key_sequence save;
        CHECK(sequence_from_string("Ctrl+K, Ctrl+S", save));
        int hits = 0;
        const auto build = [&] { if (h.ui.sequence_pressed(save)) { ++hits; } };

        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build); // step 1
        CHECK(hits == 0);
        h.in.pressed_key = 'S'; h.frame(build); // step 2, right away
        h.in.ctrl = false;
        CHECK(hits == 1);

        // a wrong second key breaks the chord: the correct one right after does not fire on its own
        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build);
        h.in.pressed_key = 'X'; h.frame(build); // not Ctrl+S: breaks it
        h.in.pressed_key = 'S'; h.frame(build);
        h.in.ctrl = false;
        CHECK(hits == 1);
        // ... but starts a fresh attempt that does complete
        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build);
        h.in.pressed_key = 'S'; h.frame(build);
        h.in.ctrl = false;
        CHECK(hits == 2);

        // waiting too long between the steps also breaks it
        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build);
        h.in.ctrl = false;
        h.frames(build, 16, 0.1f); // 1.6 s of nothing: past key_sequence_timeout
        h.in.ctrl = true; h.in.pressed_key = 'S'; h.frame(build);
        h.in.ctrl = false;
        CHECK(hits == 2);

        // two sequences sharing a prefix: the one whose second step is pressed is the one that fires
        key_sequence open;
        CHECK(sequence_from_string("Ctrl+K, Ctrl+O", open));
        int open_hits = 0;
        const auto build2 = [&] {
            if (h.ui.sequence_pressed(save)) { ++hits; }
            if (h.ui.sequence_pressed(open)) { ++open_hits; }
        };
        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build2);
        h.in.pressed_key = 'O'; h.frame(build2);
        h.in.ctrl = false;
        CHECK(hits == 2 && open_hits == 1);

        // a one-step sequence behaves exactly like chord_pressed
        key_sequence one{key_chord{'Q', true, false, false}};
        int q = 0;
        const auto build3 = [&] { if (h.ui.sequence_pressed(one)) { ++q; } };
        h.in.ctrl = true; h.in.pressed_key = 'Q'; h.frame(build3); h.in.ctrl = false;
        CHECK(q == 1);
        CHECK(!h.ui.sequence_pressed({})); // unbound
    }

    // hotkey_sequence: click the field, then press one chord after another. each step commits right away (like
    // hotkey_chord for a single step), but the field keeps listening a little longer in case of an extension
    {
        harness h;
        key_sequence chord;
        bool changed = false;
        const auto build = [&] {
            if (auto w = h.ui.window("k", {100, 100}, {300, 0}, plain_window)) { changed = h.ui.hotkey_sequence("##k", chord) || changed; }
        };
        const vec2 field{130.0f, 124.0f};
        h.frames(build, 2);
        h.click(field, build);
        CHECK(h.ui.want_text_input());

        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build); h.in.ctrl = false;
        CHECK((changed && chord == key_sequence{key_chord{'K', true, false, false}} && h.ui.want_text_input()));

        changed = false;
        h.in.ctrl = true; h.in.pressed_key = 'S'; h.frame(build); h.in.ctrl = false;
        key_sequence expect;
        CHECK(sequence_from_string("Ctrl+K, Ctrl+S", expect));
        CHECK((changed && chord == expect && h.ui.want_text_input())); // still listening: room for a third step

        changed = false;
        h.frames(build, 16, 0.1f); // 1.6 s of nothing: past key_sequence_timeout, stops listening
        CHECK((!changed && !h.ui.want_text_input() && chord == expect)); // what was captured already stands

        // a bare Esc as the very first key leaves it as it was
        changed = false;
        h.click(field, build);
        h.in.pressed_key = 0x1b; h.frame(build);
        CHECK((!changed && chord == expect && !h.ui.want_text_input()));

        // a bare Backspace / Delete as the very first key unbinds
        changed = false;
        h.click(field, build);
        h.in.pressed_key = 0x08; h.frame(build);
        CHECK((changed && !chord.bound() && !h.ui.want_text_input()));

        // reaching key_sequence::max_steps commits right away and stops listening, no pause needed
        changed = false;
        h.click(field, build);
        h.in.ctrl = true;
        h.in.pressed_key = 'A'; h.frame(build);
        h.in.pressed_key = 'B'; h.frame(build);
        h.in.pressed_key = 'C'; h.frame(build); // the third step: max_steps reached
        h.in.ctrl = false;
        CHECK((changed && !h.ui.want_text_input() && chord.count == key_sequence::max_steps));
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

    CHECK(binds.bind("open", key_chord{'S', true, false, false}));
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
        CHECK(binds.bind("save", key_chord{0x74, false, false, false})); // F5 now
        h.in.pressed_key = 'S'; h.frame(build);
        h.in.ctrl = false; h.in.pressed_key = 0x74; h.frame(build);
        CHECK(saves == 2 && opens == 1);
    }

    // config round trip: an unbound action is written as an empty value and stays unbound
    binds.reset_all();
    CHECK(binds.bind("open", key_chord{0x74, false, false, true}) && binds.bind("save", {})); // Alt+F5, unbound
    config cfg;
    binds.store(cfg);
    CHECK(cfg.get("keybinds", "open") == "Alt+F5" && cfg.has("keybinds", "save") && cfg.get("keybinds", "save").empty());

    keybinds other;
    other.add("save", "Ctrl+S");
    other.add("open", "Ctrl+O");
    other.add("extra", "F9"); // not in the file: keeps its default
    CHECK(other.load(cfg) == 2);
    CHECK((!other.find("save")->chord.bound() && other.find("open")->chord == key_chord{0x74, false, false, true}));
    CHECK(other.find("extra")->chord.steps[0].key == 0x78);

    cfg.set("keybinds", "open", "Ctrl+Nonsense"); // an unreadable value is skipped
    CHECK(other.load(cfg) == 0 && other.find("open")->chord.steps[0].alt);
    other.reset_all();
    CHECK((other.find("open")->chord == key_chord{'O', true, false, false}));

    // a multi-key chord as an action's default, and firing it, round trip through pressed() and a config file just
    // like a single-key one
    {
        keybinds seq_binds;
        CHECK(seq_binds.add("quick_open", "Ctrl+K, Ctrl+O") == 0);
        CHECK(seq_binds.text("quick_open") == "Ctrl+K, Ctrl+O");
        CHECK(seq_binds.find("quick_open")->chord.count == 2);

        harness h;
        int hits = 0;
        const auto build = [&] { if (seq_binds.pressed(h.ui, "quick_open")) { ++hits; } };
        h.in.ctrl = true; h.in.pressed_key = 'K'; h.frame(build);
        h.in.pressed_key = 'O'; h.frame(build);
        h.in.ctrl = false;
        CHECK(hits == 1);

        config seq_cfg;
        seq_binds.store(seq_cfg);
        CHECK(seq_cfg.get("keybinds", "quick_open") == "Ctrl+K, Ctrl+O");
        keybinds reloaded;
        reloaded.add("quick_open"); // unbound until load()
        CHECK(reloaded.load(seq_cfg) == 1);
        CHECK(reloaded.find("quick_open")->chord == seq_binds.find("quick_open")->chord);
    }

    // the editor draws a row per action
    {
        harness h;
        binds.reset_all();
        CHECK(binds.bind("open", key_chord{'S', true, false, false})); // a conflict, so the warning mark shows too
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

} // namespace

void run_misc_tests()
{
    test_acrylic_extras();
    test_themes();
    test_draw_list_extras();
    test_chords();
    test_key_sequences();
    test_keybinds();
    test_config();
    test_contexts_palette_and_config_file();
}

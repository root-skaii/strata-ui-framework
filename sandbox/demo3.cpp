#include "demo3.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <tuple>

using namespace strata;

demo3_state::demo3_state()
{
    for (int i = 1; i <= 14; ++i) { many.push_back(std::format("Document {}", i)); }
    code =
        "// a small program: the field colors it from text_span ranges\n"
        "#include <cstdio>\n"
        "\n"
        "struct point { float x, y; };\n"
        "\n"
        "static float length(const point& p)\n"
        "{\n"
        "    return std::sqrt(p.x * p.x + p.y * p.y);\n"
        "}\n"
        "\n"
        "int main()\n"
        "{\n"
        "    const point points[] = {{3.0f, 4.0f}, {1.5f, 2.0f}};\n"
        "    for (const point& p : points) {\n"
        "        if (length(p) > 2.0f) {\n"
        "            std::printf(\"far: %.2f\\n\", length(p));\n"
        "        } else {\n"
        "            std::printf(\"near\\n\");\n"
        "        }\n"
        "    }\n"
        "    return 0;\n"
        "}\n";
}

namespace {

// tabs, popups and status widgets ---------------------------------------------------------------

void tabs_window(context& ui, demo3_state& s)
{
    if (auto w = ui.window("tabs, popups and status", {40, 40}, {620.0f, 0.0f}, window_flags::none)) {
        if (auto card = ui.card("Tabs you can close, move and add")) {
            if (s.docs.empty()) {
                if (ui.button("+ new tab")) { s.docs.push_back("untitled " + std::to_string(s.doc_counter++)); s.doc = 0; }
                ui.text_dim("every tab is closed");
            } else {
                std::vector<tab_desc> tabs;
                for (const std::string& d : s.docs) { tabs.emplace_back(std::string_view{d}); }
                const tab_events ev = ui.tab_bar("docs", tabs.data(), tabs.size(), s.doc,
                                                 tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button);
                if (ev.add) {
                    s.docs.push_back("untitled " + std::to_string(s.doc_counter++));
                    s.doc = static_cast<int>(s.docs.size()) - 1;
                }
                apply_tab_events(ev, s.docs, s.doc);
                if (!s.docs.empty()) { ui.textf("editing {}   (drag a tab sideways to move it)", s.docs[static_cast<std::size_t>(s.doc)]); }
            }
        }

        if (auto card = ui.card("Overflow: more tabs than room")) {
            if (auto bar = ui.child("overflow", {400.0f, 40.0f}, child_flags::no_padding | child_flags::no_scrollbar)) {
                std::vector<tab_desc> tabs;
                for (const std::string& d : s.many) { tabs.emplace_back(std::string_view{d}); }
                if (!tabs.empty()) {
                    const tab_events ev = ui.tab_bar("many", tabs.data(), tabs.size(), s.many_selected,
                                                     tab_bar_flags::closable | tab_bar_flags::reorderable);
                    apply_tab_events(ev, s.many, s.many_selected);
                }
            }
            ui.text_dim("wheel over the bar, or the arrow at its right end for a list of all of them");
        }

        if (auto card = ui.card("Popups")) {
            if (ui.button("options...")) { ui.toggle_popup("options"); }
            if (auto p = ui.popup("options", 260.0f)) {
                ui.checkbox("wrap lines", s.wrap);
                ui.slider("zoom", s.zoom, 0.5f, 2.0f);
                if (ui.button("done")) { ui.close_popup(); }
            }
            ui.same_line();
            if (ui.button("open at the pointer")) { ui.open_popup("here", ui.mouse_pos()); }
            if (auto p = ui.popup("here", 210.0f)) {
                ui.text("opened where you clicked");
                ui.text_dim("Esc or a click outside closes it");
            }
            ui.same_line();
            ui.text_dim(std::format("wrap {}, zoom {:.2f}", s.wrap ? "on" : "off", s.zoom));
        }

        if (auto card = ui.card("Status: spinner, badges and chips")) {
            ui.spinner();
            ui.same_line();
            ui.text("syncing 3 of 12 files...");
            ui.badge("new");
            ui.same_line();
            ui.badge("3", toast_kind::warning);
            ui.same_line();
            ui.badge("beta", color{178, 120, 255, 255});
            ui.same_line();
            ui.badge("failed", toast_kind::error);
            ui.same_line();
            ui.badge("ok", toast_kind::success);
            ui.spacing(4.0f);
            int remove = -1;
            for (std::size_t i = 0; i < s.tags.size() && i < s.tag_on.size(); ++i) {
                if (i > 0) { ui.same_line(); }
                const chip_result r = ui.chip(s.tags[i], {.closable = true, .selected = &s.tag_on[i]});
                if (r.closed) { remove = static_cast<int>(i); }
            }
            if (remove >= 0) {
                s.tags.erase(s.tags.begin() + remove);
                for (std::size_t i = static_cast<std::size_t>(remove); i + 1 < s.tag_on.size(); ++i) { s.tag_on[i] = s.tag_on[i + 1]; }
            }
            if (s.tags.size() < s.tag_on.size()) {
                ui.same_line();
                if (ui.chip("+ add tag").clicked) { s.tags.push_back("tag " + std::to_string(s.tag_counter++)); }
            }
        }
    }
}

// drag and drop, date and time pickers ----------------------------------------------------------

void dnd_window(context& ui, demo3_state& s)
{
    if (auto w = ui.window("drag and drop, dates and times", {40, 40}, {640.0f, 0.0f}, window_flags::none)) {
        if (auto card = ui.card("Reorder the list, drop a task on \"done\"")) {
            int move_from = -1, move_to = -1, finish = -1;
            for (std::size_t i = 0; i < s.todo.size(); ++i) {
                const int idx = static_cast<int>(i);
                if (ui.selectable(s.todo[i], s.selected_task == idx)) { s.selected_task = idx; }
                if (auto d = ui.drag_source("task", idx)) { ui.text_colored(ui.theme().accent_hover, s.todo[i]); }
                const rect row = ui.last_item_rect();
                const drop_result drop = ui.drop_target("task", drop_flags::no_highlight);
                if (drop.hovering) { // an insert marker above or below the row, by the half the pointer is in
                    const f32 y = drop.local.y < 0.5f ? row.min.y - 1.0f : row.max.y + 1.0f;
                    ui.draw().rect_filled({{row.min.x, y - 1.0f}, {row.max.x, y + 1.0f}}, ui.theme().accent);
                }
                if (drop) {
                    move_from = drop.as<int>();
                    move_to   = idx + (drop.local.y < 0.5f ? 0 : 1);
                    s.last_drop = std::format("moved \"{}\"", s.todo[static_cast<std::size_t>(move_from)]);
                }
            }
            if (move_from >= 0 && move_from != move_to && move_from + 1 != move_to) {
                std::string task = s.todo[static_cast<std::size_t>(move_from)];
                s.todo.erase(s.todo.begin() + move_from);
                s.todo.insert(s.todo.begin() + (move_to > move_from ? move_to - 1 : move_to), std::move(task));
            }
            if (s.todo.empty()) { ui.text_dim("nothing left to do"); }

            ui.spacing(4.0f);
            const item_result bin = ui.custom_item("done bin", {0.0f, 52.0f});
            const drop_result got = ui.drop_target("task");
            draw_list& dl = ui.draw();
            shape_style box;
            box.radius       = radii(ui.theme().rounding);
            box.fill_top     = ui.theme().widget_bg.scaled_alpha(0.5f);
            box.fill_bottom  = box.fill_top;
            box.border       = ui.theme().widget_border;
            box.border_width = 1.0f;
            dl.shape(bin.bounds, box);
            const std::string title = std::format("done ({})", s.done.size());
            dl.text({bin.bounds.min.x + 12.0f, bin.bounds.min.y + 6.0f}, ui.theme().text, title, ui.current_font());
            std::string finished;
            for (std::size_t i = 0; i < s.done.size() && i < 4; ++i) { finished += (i ? ", " : "") + s.done[i]; }
            dl.text({bin.bounds.min.x + 12.0f, bin.bounds.min.y + 28.0f}, ui.theme().text_dim, finished, ui.current_font());
            if (got) { finish = got.as<int>(); }
            if (finish >= 0 && finish < static_cast<int>(s.todo.size())) {
                s.done.push_back(s.todo[static_cast<std::size_t>(finish)]);
                s.todo.erase(s.todo.begin() + finish);
                s.selected_task = -1;
            }
            if (ui.button("reset")) {
                for (std::string& d : s.done) { s.todo.push_back(std::move(d)); }
                s.done.clear();
            }
            ui.same_line();
            ui.text_dim("last drop: " + s.last_drop);
        }

        if (auto card = ui.card("Drag a color onto the canvas")) {
            for (std::size_t i = 0; i < s.palette.size(); ++i) {
                if (i > 0) { ui.same_line(); }
                const item_result sw = ui.custom_item(std::format("swatch {}", i), {36.0f, 36.0f});
                ui.draw().rect_filled(sw.bounds, s.palette[i], 8.0f);
                if (auto d = ui.drag_source("color", s.palette[i])) {
                    const item_result dot = ui.custom_item("preview", {36.0f, 20.0f});
                    ui.draw().rect_filled(dot.bounds, s.palette[i], 5.0f);
                }
            }
            ui.same_line();
            const item_result canvas = ui.custom_item("canvas", {160.0f, 36.0f});
            ui.draw().rect_filled(canvas.bounds, s.canvas, 8.0f);
            if (const drop_result d = ui.drop_target("color")) { s.canvas = d.as<color>(); }
            ui.same_line();
            ui.text_dim("the canvas takes the color");
        }

        if (auto card = ui.card("Date and time pickers")) {
            (void)ui.date_picker("date", s.day);
            (void)ui.time_picker("time", s.clock);
            (void)ui.datetime_picker("date and time (with seconds)", s.day2, s.clock2, true);
            ui.textf("{}  {}   {}, {}", to_string(s.day), to_string(s.clock), weekday_short(weekday(s.day)),
                     is_leap_year(s.day.year) ? "a leap year" : "a common year");
        }
    }
}

// long lists and tables --------------------------------------------------------------------------

void lists_window(context& ui, demo3_state& s)
{
    if (auto w = ui.window("long lists and tables", {40, 30}, {720.0f, 660.0f}, window_flags::resizable)) {
        if (auto card = ui.card("100 000 rows, only the visible ones are submitted")) {
            if (auto rows = ui.child("bigrows", {0.0f, 170.0f}, child_flags::frame)) {
                s.list_rows_drawn = 0;
                list_clipper clip(ui, 100000, ui.font().line_height(ui.current_font()) + 8.0f);
                while (clip.step()) {
                    for (int i = clip.begin(); i < clip.end(); ++i) {
                        ++s.list_rows_drawn;
                        if (ui.selectable(std::format("row {}", i), s.list_selected == i)) { s.list_selected = i; }
                    }
                }
            }
            ui.textf("{} of 100000 rows submitted this frame, selected: {}", s.list_rows_drawn, s.list_selected);
        }

        if (auto card = ui.card("Columns you can hide (right-click the header) and move (drag it)")) {
            static constexpr std::array<std::string_view, 8> names = {"main.cpp", "context.hpp", "README.md", "CMakeLists.txt",
                                                                       "font.cpp", "theme.ini", "icon.png", "notes.txt"};
            static constexpr std::array<std::string_view, 8> kinds = {"C++", "header", "docs", "build", "C++", "config", "image", "text"};
            if (ui.begin_table("files", 5, table_default | table_flags::hideable | table_flags::reorderable)) {
                ui.table_setup_column("Name", 0.0f, 1.4f, table_column_flags::no_hide | table_column_flags::no_reorder);
                ui.table_setup_column("Kind", 80.0f);
                ui.table_setup_column("Size", 80.0f);
                ui.table_setup_column("Modified", 110.0f);
                ui.table_setup_column("Path", 0.0f, 1.0f, table_column_flags::default_hidden);
                (void)ui.table_headers_row();
                for (std::size_t i = 0; i < names.size(); ++i) {
                    if (ui.table_next_row()) {
                        ui.table_next_column();
                        if (ui.selectable(names[i], s.file_selected == static_cast<int>(i))) { s.file_selected = static_cast<int>(i); }
                        ui.table_next_column(); ui.text(kinds[i]);
                        ui.table_next_column(); ui.textf("{} KB", 3 + static_cast<int>(i) * 17 % 40);
                        ui.table_next_column(); ui.textf("2026-09-{:02}", 1 + static_cast<int>(i) * 3);
                        ui.table_next_column(); ui.textf("src/{}", names[i]);
                    }
                }
                ui.end_table();
            }
            if (ui.button("save layout")) { s.table_layout = ui.table_save_layout("files"); }
            ui.same_line();
            if (ui.button("restore layout")) { ui.table_load_layout("files", s.table_layout); }
            ui.same_line();
            ui.text_dim(s.table_layout.empty() ? "(the layout is text: keep it in a config file)" : s.table_layout);
        }

        if (auto card = ui.card("A tree table")) {
            if (ui.begin_table("tree", 3, table_default)) {
                ui.table_setup_column("Name");
                ui.table_setup_column("Kind", 90.0f);
                ui.table_setup_column("Size", 80.0f);
                (void)ui.table_headers_row();
                const auto cells = [&](std::string_view kind, std::string_view size) {
                    ui.table_next_column(); ui.text_dim(kind);
                    ui.table_next_column(); ui.text_dim(size);
                };
                ui.table_next_row();
                ui.table_next_column();
                if (ui.table_tree_node("project", tree_flags::default_open)) {
                    cells("folder", "");
                    ui.table_next_row(); ui.table_next_column();
                    if (ui.table_tree_node("src", tree_flags::default_open)) {
                        cells("folder", "");
                        for (const auto& [name, kind, size] : {std::tuple{"main.cpp", "C++", "12 KB"}, std::tuple{"context.hpp", "header", "48 KB"},
                                                              std::tuple{"font.cpp", "C++", "22 KB"}}) {
                            ui.table_next_row(); ui.table_next_column();
                            (void)ui.table_tree_leaf(name);
                            cells(kind, size);
                        }
                        ui.table_tree_pop();
                    } else {
                        cells("folder", "");
                    }
                    ui.table_next_row(); ui.table_next_column();
                    if (ui.table_tree_node("docs")) {
                        cells("folder", "");
                        ui.table_next_row(); ui.table_next_column();
                        (void)ui.table_tree_leaf("README.md");
                        cells("docs", "31 KB");
                        ui.table_tree_pop();
                    } else {
                        cells("folder", "");
                    }
                    ui.table_next_row(); ui.table_next_column();
                    (void)ui.table_tree_leaf("CMakeLists.txt");
                    cells("build", "2 KB");
                    ui.table_tree_pop();
                } else {
                    cells("folder", "");
                }
                ui.end_table();
            }
        }

        if (auto card = ui.card("A table of 50 000 rows")) {
            if (ui.begin_table("bigtable", 3, table_default, 160.0f)) {
                ui.table_setup_column("row");
                ui.table_setup_column("square", 110.0f);
                ui.table_setup_column("even", 70.0f);
                (void)ui.table_headers_row();
                s.big_rows_drawn = 0;
                list_clipper clip(ui, 50000);
                while (clip.step()) {
                    for (int i = clip.begin(); i < clip.end(); ++i) {
                        if (ui.table_next_row()) {
                            ++s.big_rows_drawn;
                            ui.table_next_column(); ui.textf("row {}", i);
                            ui.table_next_column(); ui.textf("{}", static_cast<long long>(i) * i);
                            ui.table_next_column(); ui.text(i % 2 == 0 ? "yes" : "no");
                        }
                    }
                }
                ui.end_table();
            }
            ui.textf("{} rows submitted this frame", s.big_rows_drawn);
        }
    }
}

// code editor, passwords and masks -----------------------------------------------------------------

// colors the text for input_spans: comments, strings, numbers, keywords and function names
void highlight(std::string_view t, std::vector<text_span>& out, font_id font)
{
    static constexpr std::array<std::string_view, 21> keywords = {"if", "else", "for", "while", "return", "int", "float", "const", "auto", "void",
                                                                    "struct", "class", "static", "true", "false", "nullptr", "bool", "using",
                                                                    "namespace", "include", "char"};
    const auto add = [&](std::size_t a, std::size_t b, color c) { out.push_back({static_cast<u32>(a), static_cast<u32>(b), font, c, text_flags::none}); };
    const color c_comment{106, 116, 138, 255}, c_string{152, 195, 121, 255}, c_number{209, 154, 102, 255}, c_keyword{198, 120, 221, 255},
                c_function{97, 175, 239, 255}, c_preproc{224, 108, 117, 255};
    out.clear();
    std::size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            std::size_t e = t.find('\n', i);
            e = e == std::string_view::npos ? t.size() : e;
            add(i, e, c_comment);
            i = e;
        } else if (c == '#' && (i == 0 || t[i - 1] == '\n')) {
            std::size_t e = t.find('\n', i);
            e = e == std::string_view::npos ? t.size() : e;
            add(i, e, c_preproc);
            i = e;
        } else if (c == '"') {
            std::size_t e = i + 1;
            while (e < t.size() && t[e] != '"' && t[e] != '\n') { e += t[e] == '\\' ? 2 : 1; }
            e = std::min(e + 1, t.size());
            add(i, e, c_string);
            i = e;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t e = i;
            while (e < t.size() && (std::isalnum(static_cast<unsigned char>(t[e])) || t[e] == '.')) { ++e; }
            add(i, e, c_number);
            i = e;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t e = i;
            while (e < t.size() && (std::isalnum(static_cast<unsigned char>(t[e])) || t[e] == '_')) { ++e; }
            const std::string_view word = t.substr(i, e - i);
            if (std::find(keywords.begin(), keywords.end(), word) != keywords.end()) {
                add(i, e, c_keyword);
            } else if (e < t.size() && t[e] == '(') {
                add(i, e, c_function);
            }
            i = e;
        } else {
            ++i;
        }
    }
}

void editor_window(context& ui, demo3_state& s)
{
    if (auto w = ui.window("code, passwords and masks", {40, 30}, {700.0f, 0.0f}, window_flags::none)) {
        if (auto card = ui.card("A code editor: line numbers, brackets, find and replace, auto indent")) {
            if (ui.button("go to line 12")) { ui.code_goto_line("##code", 12); }
            ui.same_line();
            if (ui.button("find...")) { ui.code_find("##code"); }
            ui.same_line();
            if (ui.button("find and replace...")) { ui.code_find("##code", {}, true); }
            ui.same_line();
            ui.text_dim("Ctrl+F / Ctrl+H in the field, Tab / Shift+Tab indent lines");
            if (s.scene == "editor" && s.frame == 3) { ui.code_find("##code", "std", true); } // (the screenshot shows the find bar)
            const font_id code_font = static_cast<font_id>(s.mono_font >= 0 ? s.mono_font : 0);
            const auto mono = ui.with_font(code_font);
            highlight(s.code, s.spans, code_font);
            ui.input_spans(s.spans);
            (void)ui.input_code("##code", s.code, {0.0f, 250.0f});
        }

        if (auto card = ui.card("Passwords with an eye button")) {
            (void)ui.input_text("password", s.password, {}, input_flags::password | input_flags::reveal);
            (void)ui.input_text("PIN (no button)", s.pin, {}, input_flags::password);
        }

        if (auto card = ui.card("Input masks: what is typed is shaped as it goes")) {
            (void)ui.input_masked("phone number", s.phone, "(###) ###-####", "(555) 123-4567");
            (void)ui.input_masked("license plate (letters are made upper case)", s.plate, "UU-###");
            (void)ui.input_masked("date", s.stamp, "####-##-##", "yyyy-mm-dd");
            (void)ui.input_masked("hex color", s.hex_color, "\\#XXXXXX");
            ui.text_dim(std::format("stored: {} | {} | {} | {}", s.phone, s.plate, s.stamp, s.hex_color));
        }
    }
}

} // namespace

void demo3_show(context& ui, demo3_state& s)
{
    if (s.show_tabs)   { tabs_window(ui, s); }
    if (s.show_dnd)    { dnd_window(ui, s); }
    if (s.show_lists)  { lists_window(ui, s); }
    if (s.show_editor) { editor_window(ui, s); }
}

void demo3_update(context& ui, demo3_state& s)
{
    (void)ui;
    ++s.frame;
    if (s.deterministic && s.frame == 1) { override_clock({2026, 9, 25}, {13, 45, 0}); } // the pickers mark "today"
}

void demo3_script(const demo3_state& s, int frame, vec2& pos, bool* down)
{
    const auto click = [&](vec2 at, int first_frame, int button) {
        if (frame == first_frame - 3) { pos = at; }
        if (frame == first_frame)     { down[button] = true; }
        if (frame == first_frame + 1) { down[button] = false; }
    };
    if (s.scene == "tabs") {        // the options popup
        click({100.0f, 389.0f}, 8, 0);
    } else if (s.scene == "dnd") {  // the calendar of the date field
        click({360.0f, 558.0f}, 8, 0);
    } else if (s.scene == "lists") { // the menu that hides columns
        click({420.0f, 389.0f}, 8, 1);
    }
}

# Widgets & Layout Guide

Back to [README](../README.md).

## Contents
- [Basic controls & text inputs](#basic-controls--text-inputs)
- [Colors, trees & tables](#colors-trees--tables)
- [Long lists, rows & accessories](#long-lists-rows--accessories)
- [Docking, multi-viewport & windows](#docking-multi-viewport--windows)
- [Tabs, popups, modals, menus & toasts](#tabs-popups-modals-menus--toasts)
- [Sidebar layout, cards, child regions & hotkeys](#sidebar-layout-cards-child-regions--hotkeys)
- [Flex wrap & layout helpers](#flex-wrap--layout-helpers)
- [Creative & inspector widgets](#creative--inspector-widgets)
- [Charts & heatmaps](#charts--heatmaps)
- [Code editor, passwords & input masks](#code-editor-passwords--input-masks)
- [Rich text & selectable log view](#rich-text--selectable-log-view)
- [Drag and drop, date & time pickers](#drag-and-drop-date--time-pickers)
- [Node editor & undo stack](#node-editor--undo-stack)

---

## Basic controls & text inputs

```cpp
ui.button("save");                       ui.icon_button(icon_font, strata::icons::save, "Save");
ui.checkbox("enabled", flag);            ui.toggle("dark mode", flag);
ui.radio_button("option a", mode == 0);  ui.segmented("mode", {"Low", "Med", "High"}, mode);
ui.slider("speed", value, 0.0f, 4.0f);   ui.range_slider("bounds", lo, hi, 0.0f, 100.0f);
ui.knob("gain", gain, 0.0f, 1.0f, 0.5f); ui.splitter("split", pane_w, 120.0f, 480.0f, true);
ui.progress_bar(0.4f);                   ui.separator();  ui.spacing();  ui.same_line();

std::string name;
ui.input_text("name", name, "hint text");                           // std::string, grows as needed
ui.input_text("pin", pin, "secret", strata::input_flags::password); // masked, copy/cut disabled
ui.input_text("code", buf, sizeof buf);                             // fixed char buffer
if (ui.input_submitted()) { /* enter was pressed */ }

std::string notes;
ui.input_multiline("notes", notes, {0, 160});                       // multi-line, wraps, scrolls, undo / redo

ui.drag_float("gain", gain, 0.02f, 0.0f, 4.0f, 2, " x");            // Shift 0.1x, Alt 10x, click to type
ui.drag_float3("position", pos, 0.01f);                             // 2..4 components with colored ticks
ui.input_float("ratio", ratio, 0.1f, 3);  ui.input_int("items", items, 1);

int mode = 0;
ui.combo("mode", mode, {"fast", "balanced", "quality"});
bool layers[4] = {true, false, true, false};
ui.combo_multi("layers", layers, {"terrain", "water", "trees", "roads"}, "no layers");
```

- **Range slider & knob:** `range_slider` drags low/high handles without crossing; `knob` drags vertically (`Shift` fine, `Alt` coarse, double-click resets to default).
- **Text inputs:** UTF-8 aware, selection/editing keys, `Ctrl+Z`/`Y` undo/redo, IME composition support.
- **Keyboard navigation:** `Tab` / `Shift+Tab` cycles focus across content widgets in the focused window with an accent focus ring (`Space` / `Enter` activates, `Esc` or click clears; toggle with `ui.set_keyboard_nav(bool)`).
- **Disabled scopes:** `begin_disabled(cond)` / `ui.disabled_if(cond)` grays and blocks input while preserving hover tooltips.

## Colors, trees & tables

```cpp
ui.color_edit("accent", accent, strata::color_flags::no_alpha);
ui.color_picker("tint", tint);

if (auto scene = ui.tree("Scene", strata::tree_flags::default_open)) {
    if (ui.tree_leaf("Camera", selected == "Camera")) { selected = "Camera"; }
    if (auto lights = ui.tree("Lights")) { /* ... */ }
}

if (ui.begin_table("files", 3, strata::table_default, /*scroll height*/ 240.0f)) {
    ui.table_setup_column("Name", 0.0f, 1.5f);  // stretch, weight 1.5
    ui.table_setup_column("Size", 80.0f);       // fixed 80 px
    ui.table_setup_column("Type");
    int clicked = ui.table_headers_row(sort_column, ascending);
    for (auto& row : rows) {
        if (!ui.table_next_row()) { continue; } // off-screen rows skip cells
        ui.table_next_column();  ui.selectable(row.name, row.selected);
        ui.table_next_column();  ui.textf("{} KB", row.size);
        ui.table_next_column();  ui.text_ellipsis(row.type);
    }
    ui.end_table();
}
```

- **Tables:** resizable, sortable, hideable (`table_flags::hideable`), reorderable (`table_flags::reorderable`), horizontal scroll (`table_flags::scroll_x`), nested up to 5 levels, layout persistence via `table_save_layout()` / `table_load_layout()`, and tree-table rows (`table_tree_node` / `table_tree_leaf` / `table_tree_pop`).
- **Row identity without strings:** `ui.push_id(&object)` or `ui.push_id(index)` disambiguates repeated labels.

## Long lists, rows & accessories

```cpp
strata::list_clipper clip(ui, rows.size(), ui.frame_height());
while (clip.step()) for (int i = clip.begin(); i < clip.end(); ++i) { draw_row(rows[i]); }

// Trailing row controls without overlapping ellipsized text
ui.set_next_item_gutter(56.0f);
if (ui.selectable(object.name, id, selected)) { ui.selection_click(sel, index); }
if (ui.item_truncated()) { ui.tooltip(object.name); }
if (ui.row_accessory_button(icon_font, icons::trash)) { destroy(); }
ui.row_accessory_checkbox("vis", object.visible);
```

- **Overlap & gutters:** `right_gutter(w)`, `same_line_right(w)`, `allow_item_overlap()`, and `item_claimed()` support custom composite rows.
- **Multi-selection:** `selection_state` + `ui.selection_click(sel, i)` implements click, `Ctrl+click`, and `Shift+click` range selection.
- **List keyboard cursor:** `nav_begin()` / `nav_end()` enables Up/Down/Home/End/PageUp/PageDown and Enter activation.

## Docking, multi-viewport & windows

```cpp
strata::rect client{{0, 0}, ui.display_size()};
client = ui.dock_edge("explorer",  strata::dock_side::left,   260, client);
client = ui.dock_edge("inspector", strata::dock_side::right,  320, client);
client = ui.dock_edge("console",   strata::dock_side::bottom, 200, client);
ui.dock_area(client);
ui.floating_dock("Tools", {330, 80}, {360, 330});

if (auto w = ui.window("inspector", {40, 40}, {300, 400},
                       strata::window_flags::dockable | strata::window_flags::resizable)) { /* ... */ }

ui.dock_window("inspector", strata::dock_zone::right, "viewport", 0.3f);
std::string layout = ui.dock_save_layout();
ui.dock_load_layout(layout);
```

- **Multi-viewport (tear-off OS windows):** enable `context_config::multi_viewport` (or `ui.set_multi_viewport(true)`). Floating windows dragged past `display_size` (or flagged `window_flags::detached`) split out of `render_data()` into `ui.viewports()`, each rendered via `ui.viewport_render_data(vp.window_id)`. `strata::app` hosts secondary Win32 windows automatically when `app_config::multi_viewport = true`.
- **Window modes:** `resizable`, `no_title_bar`, `drag_by_body`, `acrylic`, `horizontal` (sideways scroll), `no_inputs` (HUD), `click_through`, `placed`, and nested windows.
- **Draw layers:** `ui.layer(strata::layer::foreground)` / `layer::background` draw above or behind all windows.

## Tabs, popups, modals, menus & toasts

```cpp
auto ev = ui.tab_bar("docs", tabs.data(), tabs.size(), current,
                     tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button);
apply_tab_events(ev, docs, current);

if (ui.button("options")) { ui.toggle_popup("opts"); }
if (auto p = ui.popup("opts", 260)) { ui.checkbox("wrap", wrap); if (ui.button("done")) { ui.close_popup(); } }

if (auto bar = ui.main_menu_bar()) {
    if (auto m = ui.menu("&File")) {
        if (ui.menu_item("&Open...", "Ctrl+O")) { /* ... */ }
        if (auto r = ui.menu("Open &recent")) { ui.menu_item("a.strata"); }
        ui.menu_separator();  ui.menu_item("Autosave", autosave);
    }
}
if (ui.accelerator("Ctrl+O")) { /* fires with all menus closed */ }
if (auto m = ui.context_menu("row", row_rect)) { ui.menu_item("Rename"); }

switch (ui.dialog("Delete?", "This cannot be undone.", {"Delete", "Cancel"})) { case 1: /* Delete */ break; }
ui.toast("Saved", "profile.json written.", strata::toast_kind::success);
```

- **Status widgets:** `spinner()`, `badge(text, kind_or_color)`, `chip(label, {.closable = true, .selected = &on})`, and `ask_confirm` / `confirm` with optional "don't ask again" checkbox.

## Sidebar layout, cards, child regions & hotkeys

```cpp
ui.tab_strip("tabs", {{"General", icons::settings}, {"Display", icons::view}, "About"}, page, icons_font, 150.0f);
ui.same_line();
if (auto child = ui.child("page")) {
    auto fade = ui.page_transition("fade", page);
    if (auto card = ui.card("Graphics", icons::view, icons_font)) {
        ui.toggle("V-sync", vsync);  ui.tooltip("Wait for display refresh");
    }
    if (auto card = ui.card("Keys")) { ui.hotkey("Screenshot", key); }
}
```

## Flex wrap & layout helpers

```cpp
// Multi-column fixed / weighted row
if (auto r = ui.row({strata::col_fixed(90.0f), strata::col_weight(1.0f), strata::col_fixed(70.0f)})) {
    ui.button("Left");  ui.row_next();  ui.input_text("##q", query);  ui.row_next();  ui.button("Go");
}

// Auto-wrapping horizontal flow container (toolbars, tag clouds, card grids)
ui.begin_flex({.gap_x = 6.0f, .gap_y = 6.0f});
for (auto& tag : tags) { ui.chip(tag.name, {.selected = &tag.active}); }
ui.end_flex();
```

## Creative & inspector widgets

```cpp
// Timeline / sequencer: playhead scrub, wheel zoom, Alt/middle-drag pan, clip move/trim, draggable keyframes
if (ui.begin_timeline("Sequence", tl_state, {0.0f, 180.0f})) {
    auto res = ui.timeline_track("Camera", clips, keys);
    ui.end_timeline();
}

// Cubic Bezier curve editor: drag points & tangent handles, Shift breaks tangents, double-click adds, right-click removes
ui.curve_editor("Ease", curve_points, {0.0f, 160.0f});
float y = strata::eval_curve(curve_points, t);

// Two-column property inspector grid with modified indicator dot, clickable reset, and mixed-value marker
if (ui.begin_property_grid("inspector", 0.38f)) {
    if (ui.property_row("Exposure", {.modified = val != 1.0f, .mixed = is_mixed}, [&] {
            ui.slider("##exp", val, 0.0f, 8.0f);
        })) {
        val = 1.0f;
    }
    ui.end_property_grid();
}
```

## Charts & heatmaps

```cpp
strata::plot_ref_line refs[] = {{.value = 16.67f, .vertical = false, .col = {255, 90, 90, 200}, .label = "60 fps"}};
strata::plot_options o;
o.kind = strata::plot_kind::lines; // or histogram, scatter, stacked_bars
o.x = {"time", "s"};  o.y = {"frame", "ms", strata::plot_auto, strata::plot_auto, /*log_scale*/ false};
o.fill = true;  o.zoom_pan = true;  o.interactive_legend = true;  o.ref_lines = refs;
ui.plot("Frame time", std::array{strata::plot_series{"cpu", cpu}, strata::plot_series{"gpu", gpu}}, o);

ui.plot_heatmap("Matrix", grid_values, rows, cols, {0.0f, 180.0f});
```

- Clicking a legend entry toggles that series' visibility and refits auto-Y ranges.
- `plot_kind::scatter` reads optional `plot_series::x_values` for irregular XY data.
- `plot_lines`, `plot_histogram`, and `sparkline` cover quick single-series plots.

## Code editor, passwords & input masks

```cpp
ui.input_spans(syntax_spans);
ui.input_code("##code", source, {0, 260});                      // line numbers, bracket match, auto-indent, Ctrl+/, Ctrl+F/H
ui.code_goto_line("##code", 120);   ui.code_find("##code", "TODO", false);

ui.input_text("password", pw, {}, strata::input_flags::password | strata::input_flags::reveal);
ui.input_masked("phone", phone, "(###) ###-####");
```

- `Ctrl+/` toggles `// ` comments across the caret line or multi-line selection (with undo).

## Rich text & selectable log view

```cpp
ui.rich_text("normal <f=2>heading</f> <c=ff8800>orange</c> <b>bold</b> <a=https://example.com>link</a>");
if (auto href = ui.rich_link_clicked(); !href.empty()) { open_url(href); }

{ auto rich = ui.rich_labels(); ui.button("<f=2>Save</f> <c=8a91a6>Ctrl+S</c>"); }
{ auto sel = ui.selectable_text(); ui.text("drag to select and Ctrl+C"); }

strata::log_buffer log{5000};
log.addf(strata::log_level::warn, "low disk: {} MB", mb);
ui.log_view("console", log);
```

## Drag and drop, date & time pickers

```cpp
ui.selectable(task.name);
if (auto d = ui.drag_source("task", index)) { ui.text(task.name); }
if (const strata::drop_result drop = ui.drop_target("task")) { move_task(drop.as<int>(), pos); }

strata::date d{2026, 9, 25};  strata::time_of_day t{13, 45, 0};
ui.date_picker("date", d);  ui.time_picker("time", t, true);  ui.datetime_picker("both", d, t);
```

## Node editor & undo stack

```cpp
strata::node_editor graph;
if (graph.begin(ui, "graph", {0, 400})) {
    for (node& n : nodes) {
        graph.begin_node(n.id, n.title, n.pos);
        graph.input_pin(n.in_pin, "in");
        graph.output_pin(n.out_pin, "out");
        graph.end_node();
    }
    for (link& l : links) { graph.link(l.from, l.to); }
    graph.end();
}
if (strata::node_link made; graph.link_created(made)) { links.push_back(made); }
```

- Built-in box-select, pan/zoom, grid snap, minimap, `F` to frame selection, `context_requested`, and `save_view`/`load_view`. Pair with `strata::undo_stack` (`strata/undo.hpp`) for grouped undo/redo history.

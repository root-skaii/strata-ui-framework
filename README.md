# strata

Immediate-mode UI framework for Direct3D 11 and 12. C++ latest (`/std:c++latest`), CMake 4.3+, Windows x64, MSVC.

![Docked windows with a tree, nested tables, images and rich text](docs/screenshots/features.png)

```
strata/    the library  (strata::strata, static)
sandbox/   test application, same UI on d3d11 or d3d12
cmake/     options, compile flags, hlsl embedding, package config
```

## Screenshots

All rendered by the sandbox (`strata_sandbox --scene NAME --shot FILE`) on Direct3D 11.

| | |
|---|---|
| ![Docking](docs/screenshots/docking.png)<br>**Docking** -- a window dragged over a pane shows drop guides | ![Acrylic](docs/screenshots/acrylic.png)<br>**Acrylic** -- gradients, antialiased curves, frosted glass (`glass` theme) |
| ![Charts](docs/screenshots/charts.png)<br>**Charts** -- ticks, units, area fills, hover readout, zoom / pan | ![Code editor](docs/screenshots/editor.png)<br>**Code editor** -- highlighting, find / replace, passwords, input masks |
| ![Command palette](docs/screenshots/palette.png)<br>**Command palette** -- rebindable keys, multi-key chords, fuzzy search | ![Pickers](docs/screenshots/pickers.png)<br>**Drag and drop, date picker** |
| ![Menus](docs/screenshots/menus.png)<br>**Menus** -- mnemonics, accelerators, icons, submenus | ![App scene](docs/screenshots/app.png)<br>**App-shaped UI** -- row accessories, filtered combo, corner brackets |

## Build

| Where | How |
|---|---|
| VS Code | open the folder with the *CMake Tools* extension, pick a preset (`x64-debug` / `x64-release`), build; F5 launches `sandbox (dx11)` / `sandbox (dx12)` |
| Visual Studio | *Open Folder* on this directory (Ninja presets), **or** generate a solution: `cmake --preset vs2022` then open `build/vs2022/strata.sln` |
| Terminal | `build_vs.cmd x64-release` (loads the VS x64 environment, configures, builds) |

*Open Folder* needs CMake >= 4.3 (Visual Studio's bundled copy may be older) -- use the `vs2022` preset to generate a
solution instead if so.

Options (`-D`): `STRATA_BUILD_DX11`, `STRATA_BUILD_DX12`, `STRATA_BUILD_SANDBOX`, `STRATA_STATIC_CRT` (/MT),
`STRATA_FAST_MATH` (/fp:fast), `STRATA_AVX2` (/arch:AVX2), `STRATA_WERROR`, `STRATA_INSTALL`, `STRATA_BUILD_TESTS`.

`ctest --test-dir build/x64-release` runs the headless self-test and compares sandbox screenshots against
`tests/golden/*.png` (machine-specific -- other fonts need their own; regenerate with the `strata_update_goldens`
target after an intended visual change and review the image diff).

`STRATA_AVX2` is off by default so binaries run on any x64 CPU. Shaders compile at build time (`fxc.exe`, Windows
SDK) and are embedded as bytecode -- no shader files or `d3dcompiler` DLL at runtime.

## Sandbox

```
strata_sandbox.exe [options]        (strata_sandbox.exe --help lists everything)

  --dx11 | --dx12          graphics backend (default dx11)
  --vsync | --novsync      present interval
  --fps N                  frame cap, 0 = unlimited (default: display refresh; some drivers ignore vsync)
  --width N --height N     client size (default 1280x720)
  --frames N               exit after N frames (smoke test)
  --theme N|NAME           a built-in theme by index or name (midnight, light, ocean, rose, dracula, nord, solarized_dark,
                           solarized_light, high_contrast, forest, amber, glass);  --theme-file FILE applies a theme file on top
  --scale F                ui scale (default: the monitor's dpi scale; --width / --height are logical pixels)
  --scene NAME             only one scene: default, features, visuals, inputs, multiselect, charts, textures, scripts, textlog, menus, context, modal, toasts, config, palette, tabs, dnd, lists, editor, icons, bigtree, rows, app, ...
  --selftest               headless checks of the ui logic (text editing, docking, menus, modals, ...), exit code 0 = passed
  --shot FILE              render fixed 1/60 s steps, save the last frame as a png and exit
  --golden FILE            the same, but compare with the png FILE; exit code 0 = match (--update-golden rewrites it)
  --log FILE               stderr (D3D debug layer messages in Debug builds, frame report) to a file
  --metrics                strata's own inspector windows: frame cost, geometry, culling, idle state, live draw commands
  --idle                   skip rendering and presenting while the ui reports nothing changed

  --font FILE|NAME --size PX                   primary font (id 0)
  --mono FILE|NAME --mono-size PX              second font (id 1, default Consolas 13)
  --heading FILE|NAME --heading-size PX        third font (id 2, bold, default Segoe UI 22)
  --icons FILE|NAME --icon-size PX             icon font, --no-icons
  --no-extra-fonts   --cjk   --no-kern
```

Esc closes the focused window; clicking a window raises it. "show feature windows (docking)" docks a set of example
windows into the side/bottom docks and a floating "Tools" panel -- drag them around freely.

## Using the library

```cpp
auto ui = strata::context::create().value();          // builds the font atlas
strata::win32_platform platform;  platform.attach(hwnd);   // forward WM_* to platform.handle_message()

strata::d3d11_renderer renderer;                       // or d3d12_renderer
renderer.create(device, device_context, ui.font());

// every frame
ui.begin_frame(platform.new_frame());
if (auto w = ui.window("debug", {24, 24}, 300)) {
    ui.textf("{:.1f} fps", fps);
    ui.checkbox("enabled", enabled);
    ui.slider("speed", speed, 0.0f, 4.0f);
    if (ui.button("reset")) { /* ... */ }
}
ui.end_frame();
// bind your render target, then:
renderer.render(ui.render_data());
```

`d3d11_renderer::render` saves and restores every pipeline stage it touches. `d3d12_renderer::render` records into
your open command list and expects the usual frames-in-flight fence discipline.

## Design notes

- **No heap traffic in steady state.** Vertex / index / command arrays are `vmem_array`s: reserved address space
  committed in 64 KiB chunks, never moved, cleared per frame.
- **Logical pixels.** Layout is in logical pixels; the draw list scales on output, so renderers see only physical
  pixels (see *DPI and UI scale*).
- **16-byte vertex** (`float2` pos, `unorm16x2` uv, `rgba8`), one atlas, one shader pair; draws merge per clip rect.
- **Nothing changed, nothing sent.** `end_frame` hashes the geometry; `ui.can_idle()` is true when the frame is
  identical and no animation is moving (see *Idling*).
- **Analytic rounded shapes.** One quad per shape; the pixel shader evaluates an SDF with per-corner radii, gradient,
  inner border and soft shadow. Plain axis-aligned rects take a 4-vertex fast path.
- **No exceptions, no RTTI**, static CRT, `/GL` + `/LTCG` in release, windows.h kept out of public headers.

## Widgets

```cpp
ui.button("save");                       ui.icon_button(icon_font, strata::icons::save, "Save");
ui.checkbox("enabled", flag);            ui.toggle("dark mode", flag);
ui.slider("speed", value, 0.0f, 4.0f);   // caption + value above, thin track, round knob (float or integer)
ui.progress_bar(0.4f);                   ui.separator();  ui.spacing();  ui.same_line();

std::string name;
ui.input_text("name", name, "hint text");                              // std::string, grows as needed
ui.input_text("pin", pin, "secret", strata::input_flags::password);    // masked, copy/cut disabled
ui.input_text("code", buf, sizeof buf);                                // fixed char buffer
if (ui.input_submitted()) { /* enter was pressed */ }

std::string notes;
ui.input_multiline("notes", notes, {0, 160});                          // several lines, wraps, scrolls, undo / redo

int mode = 0;
ui.combo("mode", mode, {"fast", "balanced", "quality"});               // returns true on change

int tab = 0;
ui.tab_bar("tabs", {{"General", icons::settings}, "Advanced", "About"}, tab, icon_font);
switch (tab) { /* draw the content of the selected tab */ }
```

- **Text input:** UTF-8 aware, standard selection / editing keys, Ctrl+Z/Y undo-redo. Wire `win32_platform` for keys
  and clipboard.
- **Multi-line input:** the same, plus word wrap and scrolling; `input_flags::no_wrap` / `read_only` are available.
- **Combo box:** drawn above every window, flips up near the bottom, scrolls past 8 rows.
- **Icons:** bake an icon font's `glyph_ranges::private_use` (Segoe MDL2 Assets / Segoe Fluent Icons ship with
  Windows) and use `strata/icons.hpp`, or any code point via `strata::glyph_string{0xe80f}`.

## Color, trees, tables

```cpp
// color picker: swatch that opens a popup, or the picker inline
ui.color_edit("accent", accent, strata::color_flags::no_alpha);
ui.color_picker("tint", tint);

// trees and selectable lists; open state persists per node
if (auto scene = ui.tree("Scene", strata::tree_flags::default_open)) {
    if (ui.tree_leaf("Camera", selected == "Camera")) { selected = "Camera"; }
    if (auto lights = ui.tree("Lights")) { /* ... */ }
}                                            // or tree_node(...) + tree_pop()

// tables: fixed / stretch columns, sticky sortable header, resizable, striped, optional scrolling body
if (ui.begin_table("files", 3, strata::table_default, /*scroll height*/ 240.0f)) {
    ui.table_setup_column("Name", 0.0f, 1.5f);   // stretch, weight 1.5
    ui.table_setup_column("Size", 80.0f);        // fixed 80 px
    ui.table_setup_column("Type");
    int clicked = ui.table_headers_row(sort_column, ascending);   // column clicked this frame, or -1
    for (auto& row : rows) {
        if (!ui.table_next_row()) { continue; }  // false for rows scrolled out of view: skip their cells
        ui.table_next_column();  ui.selectable(row.name, row.selected);
        ui.table_next_column();  ui.textf("{} KB", row.size);
        ui.table_next_column();  ui.text_ellipsis(row.type);
    }
    ui.end_table();
}
```

- **Color picker:** SV square, hue bar, alpha bar, preview and a hex field; `color_edit` shows it in a popup.
- **Trees:** guide lines, animated arrow, `selected` / `default_open`; `selectable` is the same row without children.
- **Off-screen rows cost nothing** -- rows outside the clip rect skip hit-testing and drawing, so deep trees and long
  lists (`--scene bigtree`) stay cheap without any opt-in.
- **Row identity without strings:** pass the identity separately (a pointer, `u64`, `int`, or raw bytes) when labels
  would collide, e.g. `ui.push_id(&object)`.
- **Nested tables:** up to five levels; give repeated per-row tables their own id with `push_id`.

## Images and docking

```cpp
// images: the renderer owns the texture (rgba8, straight alpha); the ui only draws it
strata::texture_id tex = renderer.create_texture(w, h, pixels);        // d3d11_renderer / d3d12_renderer, 0 on failure
ui.image(tex, {96, 96});                                               // size.x == 0: layout width, size.y == 0: square
ui.image(tex, {96, 96}, {0.25f, 0.25f}, {0.75f, 0.75f}, tint, /*rounding*/ 12.0f);   // crop, tint, round corners
if (ui.image_button("open", tex, {64, 64})) { /* clicked */ }
renderer.destroy_texture(tex);                                         // d3d12: after the gpu is done with it

// docking: regions that windows can be dropped into (every frame, before the windows)
strata::rect client{{0, 0}, ui.display_size()};
client = ui.dock_edge("explorer",  strata::dock_side::left,   260, client);   // side panels: take room only while used
client = ui.dock_edge("inspector", strata::dock_side::right,  320, client);
client = ui.dock_edge("console",   strata::dock_side::bottom, 200, client);
ui.dock_area(client);                                                      // the main space: what is left
ui.floating_dock("Tools", {330, 80}, {360, 330});                          // a movable panel that windows dock into
if (auto w = ui.window("inspector", {40, 40}, {300, 400}, strata::window_flags::dockable | strata::window_flags::resizable)) { /* ... */ }
ui.dock_window("inspector", strata::dock_zone::right, "viewport", 0.3f);    // optional: a default layout

std::string layout = ui.dock_save_layout();      // the whole arrangement as text: keep it in a file / settings blob ...
ui.dock_load_layout(layout);                     // ... and bring it back (before or after the windows were shown)
```

- **Images:** the renderer owns the texture. Formats: `rgba8`, `bgra8`, `r8`, `a8`, `rgba16f`. Mip maps and partial
  `update_texture` are supported; `strata::texture_image` (`strata/texture.hpp`) is the cpu side for custom renderers.
- **Docking:** drag a `window_flags::dockable` window over a dock area -- its centre adds a tab, its edges split the
  pane. `dock_window` / `undock_window` / `is_docked` do it from code. Drag splitters to resize, drag a tab to float it.
- **Saving a layout:** `dock_save_layout()` / `dock_load_layout()` round-trip the whole arrangement (splits, ratios,
  tabs, window positions) as text, matching windows by name.

## DPI and UI scale

```cpp
ui.set_scale(platform.dpi_scale());        // 1.0 = 96 dpi, 1.5 = 144 dpi ...  (rebuilds the font atlas)
renderer.update_atlas(ui.font());          // d3d11 / d3d12: upload it
ui.release_font_pixels();
// on WM_DPICHANGED: the same three lines with the new platform.dpi_scale()
```

Everything you specify stays in logical pixels; the draw list scales on output, so renderers only see physical
pixels. A rescale costs one atlas rebuild. The dpi scale is a good default but a poor *setting* -- see
*UI scale at runtime* for changing it independently.

## Drawing: gradients, curves, acrylic

```cpp
dl.rect_gradient_angle(r, from, to, /*degrees*/ 45.0f, /*rounding*/ 10.0f);   dl.rect_gradient_radial(r, centre, edge, 10.0f);
dl.polyline(points, color, /*thickness*/ 2.0f, /*closed*/ false);             dl.bezier_cubic(p0, p1, p2, p3, color, 3.0f);
dl.arc(centre, radius, a0, a1, color, 8.0f);     dl.circle(c, r, color, 1.0f);   dl.circle_filled(c, r, color);
dl.backdrop(rect, /*blur*/ 18.0f, /*tint*/ color, radii(10.0f));                   // frosted glass
ui.window("glass", pos, size, strata::window_flags::acrylic);                        // or child_flags::acrylic
```

- **Gradients and curves:** angled or radial gradients, mitred polylines with AA, beziers and arcs that tessellate to
  them -- all one quad or one strip.
- **Acrylic / blur:** `window_flags::acrylic` / `child_flags::acrylic` frost the background behind a window or child.
  D3D12 needs the extra `d3d12_target` argument to `renderer.render(...)` for a live blur (otherwise panels are flat
  tints). Theme `glass` is built for it.

## Themes and theme files

```cpp
ui.theme() = strata::themes::nord();                 // 12 built in: midnight light ocean rose dracula nord solarized_dark
strata::themes::by_name("forest", ui.theme());       // solarized_light high_contrast forest amber glass; themes::names()
strata::themes::save_file("my.theme", ui.theme(), "my theme");
strata::themes::theme_result r;  strata::themes::load_file("my.theme", ui.theme(), &r);   // r.applied / unknown / invalid / first_problem_line
```

Theme files are `key = value` text (colors as `#rgb` / `#rgba` / `#rrggbb` / `#rrggbbaa`), optionally starting from a
built-in theme with `base = nord`. The sandbox has save / load buttons and `--theme` / `--theme-file`.

## Key bindings and config files

```cpp
strata::keybinds binds;
binds.add("save", "Ctrl+S", "write the document");      // name, default chord, tooltip
binds.add("quick_open", "Ctrl+P");
binds.add("goto_symbol", "Ctrl+K, Ctrl+O");              // a short sequence: Ctrl+K, then Ctrl+O

if (binds.pressed(ui, "save")) { save(); }               // true on the frame the chord is pressed
(void)ui.menu_item("Save", binds.text("save"));          // "Ctrl+S" as the shortcut hint
strata::keybind_editor(ui, binds);                       // a table: click a field, press the new chord

strata::config cfg;                                      // ini-like: [sections] and key = value lines
binds.store(cfg);                                        // [keybinds]  save = Ctrl+S
strata::themes::to_config(cfg, ui.theme());             // [theme]     accent = #5b8dff ...
cfg.set_float("app", "volume", 0.6f);
cfg.save_file("settings.ini");
// later: register the actions, then
cfg.load_file("settings.ini");
binds.load(cfg);  strata::themes::from_config(cfg, ui.theme());  volume = cfg.get_float("app", "volume", 1.0f);
```

- **Chords:** `key_chord{key, ctrl, shift, alt}`, a virtual-key code (or side mouse button) with exact modifiers;
  `accelerator()` syntax like `"Ctrl+Shift+S"`.
- **Multi-key chords:** a `key_sequence` chains up to 3 chords, e.g. `"Ctrl+K, Ctrl+S"`.
- **config:** ini-like `[section]` / `key = value`, case-insensitive, with typed getters that take a fallback.
- **Contexts:** `binds.add(..., "editor")` only fires while `binds.set_context("editor", has_focus)` is on that frame.
- **Command palette:** `strata::command_palette` -- a fuzzy-search modal over registered actions.
- **config_file:** binds a `config` to a file, with optional `set_auto_save` / `set_hot_reload`.
- `--scene config` / `--scene palette` show all of the above.

## Tabs, popups and status widgets

```cpp
std::vector<std::string> docs{"main.cpp", "notes.txt"};   int current = 0;
std::vector<tab_desc> tabs;  for (auto& d : docs) tabs.emplace_back(std::string_view{d});
auto ev = ui.tab_bar("docs", tabs.data(), tabs.size(), current,
                     tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button);
if (ev.add) { docs.push_back("untitled"); current = int(docs.size()) - 1; }
apply_tab_events(ev, docs, current);        // removes a closed tab, applies a drag, keeps `current` on its tab

if (ui.button("options")) { ui.toggle_popup("opts"); }          // a popup: any widgets, closes on Esc / a click outside
if (auto p = ui.popup("opts", 260)) { ui.checkbox("wrap", wrap); if (ui.button("done")) { ui.close_popup(); } }
ui.open_popup("menu", ui.mouse_pos());                           // or at a position

ui.spinner();  ui.same_line();  ui.text("syncing...");
ui.badge("3", toast_kind::warning);   ui.badge("beta", color{178, 120, 255, 255});
auto r = ui.chip("filter", {.closable = true, .selected = &on});    // r.clicked / r.closed
```

- **Tabs:** `tab_bar` reports `changed` / `closed` / `moved_from` / `moved_to` / `add`; `apply_tab_events` updates
  your list for you.
- **Popups:** `open_popup` / `toggle_popup` / `popup` / `close_popup`; they stack up to 4 levels (a popup opened from
  inside a popup opens on top of it).
- **Status widgets:** `spinner`, `badge`, `chip`.
- `--scene tabs` shows it all.

## Drag and drop, date and time pickers

```cpp
ui.selectable(task.name);
if (auto d = ui.drag_source("task", index)) { ui.text(task.name); }      // the preview that follows the pointer
...
ui.selectable(other.name);
const drop_result drop = ui.drop_target("task", drop_flags::no_highlight);
if (drop.hovering) { /* draw an insert marker: drop.local.y < 0.5 is the upper half */ }
if (drop)          { move_task(drop.as<int>(), position); }                 // the payload

strata::date d{2026, 9, 25};  strata::time_of_day t{13, 45, 0};
ui.date_picker("date", d);  ui.time_picker("time", t, /*seconds*/ true);  ui.datetime_picker("both", d, t);
```

- **Drag and drop:** the payload is a small typed copy of your data; a target accepts one type. Esc cancels a drag.
- **Pickers:** a month calendar or hour / minute (/ second) grids; `datetime.hpp` has the date-math helpers
  (`add_days`, `parse_date`, ...).
- `--scene dnd` shows both.

## Long lists and tables

```cpp
strata::list_clipper clip(ui, rows.size(), ui.frame_height());          // rows of one height, in a scrolling area
while (clip.step()) for (int i = clip.begin(); i < clip.end(); ++i) { draw_row(rows[i]); }

ui.begin_table("files", 5, table_default | table_flags::hideable | table_flags::reorderable);
ui.table_setup_column("Name", 0, 1, table_column_flags::no_hide | table_column_flags::no_reorder);
ui.table_setup_column("Path", 0, 1, table_column_flags::default_hidden);      // ... the other columns
// right-click the header: a menu of columns; drag a header: move the column
std::string saved = ui.table_save_layout("files");                            // order, widths, hidden: text for a config file
ui.table_load_layout("files", saved);                                         // may be called before the table is first shown

// a tree table: expandable nodes in a cell, the children are the rows that follow
ui.table_next_row(); ui.table_next_column();
if (ui.table_tree_node("src", tree_flags::default_open)) {
    ui.table_next_row(); ui.table_next_column(); (void)ui.table_tree_leaf("main.cpp");
    ui.table_tree_pop();
}
```

- **list_clipper** submits only visible rows (uniform height), so even 100k-row lists are cheap -- works in windows,
  `child` regions and tables with a fixed height.
- **Table columns** can be hideable / reorderable, with a save / load layout round-trip.
- **Tree tables:** `table_tree_node` / `table_tree_leaf` / `table_tree_pop` for expandable rows inside a table.
- **Keyboard navigation** is opt-in: `nav_begin()` / `nav_end()` gives Up / Down / Home / End / PageUp / PageDown and
  Enter-to-click over a region.
- `--scene lists` shows the tables, `--scene bigtree` a deep tree with a live `ui.stats()` panel.

## Rows: overlapping items, right-aligned controls, diagnostics

```cpp
// a full-width header row with its own buttons on the right
{
    auto g = ui.right_gutter(76.0f);              // everything below is laid out 76 px narrower ...
    row = ui.custom_item("row", {0.0f, 30.0f});   // ... so the row ends where its buttons start
    ui.label_clipped(pos, row.bounds.width() - 16.0f, ui.theme().text, component.name);  // "..." at the edge
}
ui.allow_item_overlap();                          // what follows may take the press from the row
ui.same_line_right(76.0f);  ui.toggle("##on", component.enabled);
ui.same_line_right(24.0f);  if (ui.icon_button(icons_font, icons::trash)) { remove(); }
if (row.pressed && !ui.item_claimed()) { select(); }
if (ui.item_clicked(strata::mouse_button::right)) { ui.open_popup("row menu"); }
```

- **`same_line_right(width)`** / **`right_gutter(w)`** lay out right-aligned controls on a row without them
  overlapping the row's own (ellipsized) label.
- **`allow_item_overlap()`** lets a later widget -- a trailing button, say -- take the press over the row beneath it;
  `item_claimed()` reports whether it did.
- **Row accessories** (`set_next_item_gutter` + `row_accessory_button` / `_checkbox` / `_toggle`) do the common
  "row with trailing controls" case for you:

```cpp
ui.set_next_item_gutter(56.0f);
if (ui.selectable(object.name, id, selected)) { select(); }
if (ui.item_truncated()) { ui.tooltip(object.name); }          // it was cut: show the whole thing
if (ui.row_accessory_button(icon_font, icons::trash)) { destroy(); }
ui.row_accessory_checkbox("vis", object.visible);
```

- **`ui.stats()`** reports the last frame's cost: items, vertices, indices, draw calls, and cache hits -- useful for
  diagnosing slow frames.
- `--scene rows` shows overlap, gutters and keyboard lists; `--scene app` shows accessories and a filtered combo.

## Shortcuts, focus, disabled items, selections

```cpp
// a shortcut that only fires while the user is looking at this panel, and never while a name is being typed
if (ui.window_focused()) {
    if (ui.key_pressed(VK_DELETE))      { ui.ask_confirm("destroy", "Destroy the selection?", sel.size()); }
    if (ui.key_pressed(VK_F2))          { ui.request_text_focus("name"); }
    if (ui.key_pressed('D', true))      { duplicate(); }        // Ctrl+D
}
{   auto d = ui.disabled_if(sel.empty());                        // greyed, dead, still explains itself
    if (ui.button("Destroy selected")) { ... }
    if (sel.empty()) { ui.tooltip("select something first"); }
}
if (ui.selectable(rows[i].name, id, sel.contains(i))) { ui.selection_click(sel, i); }   // ctrl / shift rules
switch (ui.confirm("destroy", {"Destroy", "Cancel"}, {.remember = &never_ask, .danger = 1})) { case 1: ...; }
```

- **Keys and mouse:** `key_pressed` / `key_down` over virtual-key codes, `mouse_clicked` / `mouse_down` /
  `item_clicked(mouse_button)`; all quiet while a text or hotkey field has the keyboard.
- **Disabled items:** `begin_disabled(cond)` / `ui.disabled_if(cond)` fades and disables a scope while
  `item_hovered()` still works, so an explanatory tooltip can still show.
- **Selections:** `selection_state` + `ui.selection_click(sel, index)` gives click/Ctrl/Shift selection for free.
- **Confirmations:** `ask_confirm(id, message)` opens one, `confirm(id, buttons, options)` draws it and returns the
  button pressed (-1 Esc, 0 while open); `options.remember` wires a "don't ask again" flag.

## Code editor, passwords and input masks

```cpp
ui.input_spans(spans);                                          // colors from your highlighter, as for any text field
ui.input_code("##code", source, {0, 260});                      // line numbers, current line, brackets, auto indent, find / replace
ui.code_goto_line("##code", 120);   ui.code_find("##code", "TODO", /*replace bar*/ false);

ui.input_text("password", pw, {}, input_flags::password | input_flags::reveal);   // an eye button shows the text
ui.input_masked("phone", phone, "(###) ###-####");                 // "(555) 123-4567" is built as you type
ui.input_masked("plate", plate, "UU-###");                         // "AB-123": letters are made upper case
```

- **input_code:** a non-wrapping multi-line field with line numbers, current-line highlight, bracket matching,
  auto-indent and find / replace (Ctrl+F / Ctrl+H).
- **Masks:** `#` digit, `A` letter, `U` / `L` letter forced upper / lower, `X` letter or digit, `?` anything, `\`
  escapes, everything else is literal.
- `--scene editor` shows it all, with the find / replace bar open.

## Number inputs, multi-select, plots

```cpp
ui.drag_float("gain", gain, /*speed*/ 0.02f, /*lo*/ 0.0f, /*hi*/ 4.0f, /*decimals*/ 2, " x");   ui.drag_int("count", n, 0.25f, 0, 200);
ui.drag_float3("position", pos, 0.01f);                       // 2..4 components under one caption, each a colored tick
ui.input_float("ratio", ratio, /*step*/ 0.1f, 3);             ui.input_int("items", items, 1);    // typed, with - / + buttons
bool layers[4] = {true, false, true, false};
ui.combo_multi("layers", layers, {"terrain", "water", "trees", "roads"}, "no layers");

ui.plot_lines("signal", samples, {0, 90}, "rolling", 0.0f, 1.0f, /*ring offset*/ head);      ui.plot_histogram("bins", bins);
ui.plot("two series", std::array{plot_series{"a", a, {}}, plot_series{"b", b, color{...}}});    ui.sparkline(samples, {100, 20});

strata::plot_options o;                        // a full chart
o.x = {"time", "s"};  o.y = {"load", "%", 0.0f, 100.0f};     // axis title, unit, optional fixed range
o.x_start = 0.0f;  o.x_step = 0.5f;            // sample i is at x_start + i * x_step
o.fill = true;  o.zoom_pan = true;             // area under the lines; wheel / drag / double-click
ui.plot("load", std::array{plot_series{"cpu", cpu, {}}, plot_series{"gpu", gpu, {}}}, o);
```

- **Drag fields:** drag sideways to change the value (Shift 0.1x, Alt 10x), click without moving to type instead.
- **Multi-select dropdown:** shows "a, b" or "all (n)", with select-all / clear for longer lists.
- **Plots:** `plot_lines` / `plot_histogram` / `sparkline` for quick charts; `plot(label, series, plot_options)` for a
  full chart with nice-ticked axes, a legend, fills, and optional zoom / pan.

## Selectable text and the log view

```cpp
ui.text_selectable("this can be selected and copied");       // wraps at the layout width; drag, double-click a word, Ctrl+C
{ auto sel = ui.selectable_text();  ui.text("so can this");  ui.text_dim("and this"); }   // text() inside the scope is selectable

strata::log_buffer log{5000};                                // a ring of lines
log.addf(strata::log_level::warn, "low disk: {} MB", mb);    log.add(strata::log_level::error, "boom");
ui.log_view("console", log);                                 // toolbar + lines, fills a fixed-height window
```

- **Selectable text** is a frameless read-only multi-line field sharing the normal selection and clipboard code.
- **Log view:** filter by substring and minimum level, follow-scroll, a time column, copy / clear. Only visible rows
  draw, so even a 12000-line log is cheap.

## Modals, menus, toasts

```cpp
if (ui.button("delete...")) ui.open_modal("Delete?");
switch (ui.dialog("Delete?", "This cannot be undone.", {"Delete", "Cancel"})) { case 1: /* Delete */ break; case -1: /* dismissed */ break; }
if (auto m = ui.modal("Settings", {440, 0})) { /* widgets */  if (ui.button("close")) ui.close_modal(); }   // modals stack

if (auto bar = ui.main_menu_bar()) {
    if (auto m = ui.menu("&File")) {                                               // "&F": Alt+F opens it
        if (ui.menu_item("&Open...", "Ctrl+O")) { /* ... */ }                      // O picks it while the menu is open
        if (auto r = ui.menu("Open &recent")) { ui.menu_item("a.strata"); }        // submenus nest (4 levels)
        ui.menu_separator();  ui.menu_item("Autosave", autosave);                  // a check item
    }
}
if (ui.accelerator("Ctrl+O")) { /* works with every menu closed: the same strings the rows show */ }
if (auto m = ui.context_menu("row", row_rect)) { ui.menu_item("Rename"); ui.menu_item("Delete", "Del"); }  // right-click
ui.toast("Saved", "profile.json was written.", strata::toast_kind::success);       // info / success / warning / error
```

- **Modals:** `open_modal` / `modal` / `dialog` -- centred, dimmed, stacked; `dialog()` returns the 1-based button
  pressed (-1 dismissed, 0 while open).
- **Menus:** `main_menu_bar` / `menu` / `menu_item`; `&` marks a mnemonic, `accelerator("Ctrl+Shift+S")` fires with
  every menu closed.
- **Toasts:** `ui.toast(...)` stacks notifications in a corner (max 8), with optional buttons and progress.

## Menus: sidebar layout, child regions, cards, tooltips, key binding

```cpp
constexpr auto flags = strata::window_flags::no_title_bar | strata::window_flags::drag_by_body | strata::window_flags::resizable;
if (auto w = ui.window("settings", {260, 90}, {740, 480}, flags)) {
    ui.text("Settings");  ui.same_line(ui.content_width() - 36);  if (ui.icon_button(icons_font, icons::cancel)) close();
    ui.separator();

    ui.tab_strip("tabs", {{"General", icons::settings}, {"Display", icons::view}, "About"}, page, icons_font, 150.0f);
    ui.same_line();
    if (auto child = ui.child("page")) {                       // takes the space right of the strip, down to the window bottom
        auto fade = ui.page_transition("fade", page);          // fades + slides in whenever `page` changes
        if (auto card = ui.card("Graphics", icons::view, icons_font)) {
            ui.combo("Quality", quality, {"Low", "High"});
            ui.toggle("V-sync", vsync);   ui.tooltip("Wait for the display refresh");
        }
        if (auto card = ui.card("Keys")) { ui.hotkey("Screenshot", key); }
    }
}
```

`strata_sandbox --menu` runs this example (five placeholder pages).

- **Child regions:** `begin_child` / `ui.child(id, size, flags)` -- clipped, scrollable, own scrollbar; nest up to 4
  deep.
- **Cards:** `ui.card("Title", icon, icon_font)` -- a titled group box.
- **Tab strip:** `ui.tab_strip(...)` -- a vertical sidebar; `tab_strip_flags::icons_only` makes an icon rail.
- **Hotkeys:** `ui.hotkey("Label", key_code)` captures a key or side mouse button by clicking then pressing it.
- **Transitions:** `ui.page_transition(key, page)` fades and slides content in when `page` changes.

## Rich text

```cpp
ui.rich_text("normal <f=2>heading font</f> <c=ff8800>orange</c> <f=1><c=5b8dff80>translucent mono</c></f> 1 << 2");
ui.rich_textf("<f={}>{}</f> saved", heading_font, name);
ui.rich_text_wrapped("a paragraph that <c=ff6b8a>breaks</c> at word boundaries\nand at new lines");
ui.text_wrapped("plain text, wrapped");

// rich labels: the captions of ordinary widgets read the same markup
{
    auto rich = ui.rich_labels();
    ui.button("<f=2>Save</f> <c=8a91a6>Ctrl+S</c>");
    ui.checkbox("enabled <f=1>(mono)</f>", flag);
    ui.combo("<c=8a91a6>colour</c>", pick, {"<c=ff6b8a>rose</c>", "<c=19c2b4>teal</c>"});
}
```

Tags: `<f=N>` font id, `<c=rrggbb[aa]>` color, `<b>` `<i>` `<u>` `<s>` (combine freely), each closed by `</f>`,
`</c>`, `</b>` ...; `<<` is a literal `<`. Tags nest.

**Links.** `<a=href>text</a>` draws underlined, shows the hand cursor and reports the href on click -- strata never
opens anything itself:

```cpp
ui.rich_text("see <a=https://example.com>the manual</a>, or <a=cmd:reset>reset</a>");
if (auto href = ui.rich_link_clicked(); !href.empty()) { open(href); }   // an event, not a state
```

**Styled field contents.** `ui.input_spans(spans)` before the next `input_text` / `input_multiline` gives byte
ranges (`text_span{start, end, font, color, style}`) their own font, color and style -- for syntax highlighting or
markup previews.

**IME.** Compositions (Chinese, Japanese, Korean, ...) show at the caret until confirmed. Wire `win32_platform`'s
`input_state::ime*` fields and call `platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height())`
after `end_frame()`.

## Theming, custom drawing, overrides

```cpp
ui.theme() = strata::themes::light();        // midnight (default), light, ocean, rose; apply between frames

// per-widget overrides: scoped, or push/pop
{
    auto danger = ui.style_overrides({
        strata::override_color(strata::style_color::accent, red),
        strata::override_var(strata::style_var::rounding, 16.0f),
    });
    ui.button("delete");
}

// custom widgets: lay out + hit-test a slot, then draw into it
auto item = ui.custom_item("meter", {0, 46});             // {0, h} = full width
float glow = ui.animate("meter", item.hovered ? 1 : 0);   // smoothed per-key value
strata::shape_style s;
s.radius = strata::radii(8);  s.fill_top = a;  s.fill_bottom = b;
s.border = c;  s.border_width = 1;  s.shadow = d;  s.shadow_blur = 12;  s.shadow_offset = {0, 4};
ui.draw().shape(item.bounds, s);                           // also: rect_filled, rect_outline, line, text, ...
```

`style` covers colors, rounding, padding, spacing, borders, shadow, gradient and animation speed; `style_overrides`
changes them for a scope. `custom_item` + `draw()` let you build custom widgets with the same primitives strata uses
internally.

## Windows

Windows drag, collapse and stack; pressing one raises it. Input goes to the topmost window under the pointer.
`ui.window(..., strata::window_flags::resizable)` resizes from the right/bottom edges and the corner grip;
`height == 0` follows the content until the user drags, then scrolls.

`ui.cursor()` reports the pointer shape to apply (`arrow`, `text`, `hand`, `not_allowed`, `resize_*`);
`platform.set_cursor(ui.cursor())` after `end_frame()` applies it via `win32_platform`.

## Idling

An untouched UI draws the same thing every frame. `end_frame` hashes the geometry to tell you when nothing changed:

```cpp
ui.end_frame();
if (ui.can_idle()) {
    // nothing to draw that is not already on the screen
} else {
    renderer.render(ui.render_data());
    present();
}
```

- `can_idle()` -- geometry unchanged and no animation settling (a toast counting down, a pending tooltip).
- `next_wake_seconds()` -- how long a sleeping host may wait for input; `0` = run now, `no_deadline` = wait for input.
- `invalidate()` forces the next frame to count as changed (texture replaced, theme edited, ...).

Overlays can't skip drawing (the game redrew the target), but the renderers still skip the upload themselves via a
content hash. The sandbox's `--idle` flag demonstrates and reports this.

## Diagnostics

```cpp
ui.debug_metrics_window(show_metrics);    // frame cost, geometry, culling, idle state, and what went wrong
ui.debug_draw_list_window(show_commands); // the live commands: clip, index count, base vertex, texture
```

`ui.stats()` gives the same numbers as a struct, plus overflow counters for fixed-size tables (`draw_overflow`,
`clip_overflows`, `alpha_overflows`) and, in debug builds, `id_collisions` when two widgets hash to the same id.

## Footprint

Static-linked, no heap traffic in steady state. Linking a full-featured probe (window, text, button, checkbox,
slider, text field, rich text, a table) against an empty release binary adds **~580 KiB** (~420 KiB is strata's own
code, the rest mostly `std::format` float tables); each D3D backend adds ~35 KiB more.

A typical UI (a couple of windows, a table) holds well under 1 MiB of committed memory and emits a few thousand
vertices/indices per frame (~0.1 ms CPU); steady state allocates nothing.

No disk, registry, thread or hook use -- everything is statically linked, and each backend delay-loads only its own
D3D DLL. Typed text (including passwords, via `secure_string`) is zeroed on focus loss and context destruction.

## Fonts, Unicode, kerning

```cpp
strata::context_config cfg;
cfg.font.file         = "C:/Windows/Fonts/consola.ttf";   // or cfg.font.data = embedded bytes, or cfg.font.face = "Segoe UI"
cfg.font.pixel_height = 15;
cfg.font.ranges       = my_ranges;                         // glyph_ranges::latin, greek, cyrillic, cjk_unified, ...
auto ui = strata::context::create(cfg).value();
```

- **Multiple fonts:** `cfg.extra_fonts` adds fonts 1, 2, ... into one shared atlas; `ui.push_font(id)` / `pop_font()`
  switches. Titles use font 0.
- **Unicode:** full UTF-8; you choose the baked blocks (`font_config::ranges`). Missing glyphs draw `?`. CJK needs a
  font with those glyphs (`--font C:\Windows\Fonts\msyh.ttc --cjk`).
- **Fallback faces and emoji:** `font_config::fallback_faces` fills missing glyphs at bake time;
  `glyph_ranges::emoji` / `symbols` / `math_alphanumeric` are available.
- **Right-to-left text:** bake `glyph_ranges::hebrew` / `arabic` / `arabic_forms_a` / `_b` and bidi reordering plus
  Arabic shaping happen automatically in every widget. Not done: explicit embeddings, right-aligned rtl paragraphs,
  Indic / Southeast Asian shaping (see TODO).
- **Icon fonts:** `strata/icons.hpp` names the code points shared by Segoe MDL2 Assets and Segoe Fluent Icons;
  `--scene icons` shows what a loaded font actually has.

## UI scale at runtime

```cpp
strata::overlay::options opt;
opt.ui_scale      = 0.0f;   // 0 = the monitor's dpi scale, which is only where it starts
opt.scale_hotkeys = true;   // Ctrl + Plus / Minus step it by 10 %, Ctrl + 0 goes back
...
strata::overlay::set_ui_scale_percent(125);          // from anywhere, at any time
ui.textf("ui scale {} %", ui.scale_percent());
```

The DPI scale is a good default, not a setting -- overlays are often wanted smaller or larger. `set_ui_scale_percent`
works from any thread and applies on the next frame. Each rebuild costs tens of ms per font, so drive it from a
stepper rather than a dragged slider.

## In-game overlay (direct3d 11 and 12)

`overlay/` is a dll loaded into a direct3d program (modding tools, inspectors) that draws a strata ui over every
frame and takes keyboard and mouse while open.

```cpp
strata::overlay::options opt;
opt.ui = [](strata::context& ui) { if (auto w = ui.window("my tool", {40, 40}, {360, 0})) { ui.text("hello"); } };
strata::overlay::install(opt);       // from a thread of your own, not from DllMain; F1 (opt.toggle_key) shows / hides it
```

- **The hook:** a dummy swap chain yields `IDXGISwapChain`'s shared vtable, so `Present` / `Present1` /
  `ResizeBuffers` can be replaced on the game's own swap chain with no code patched (`uninstall()` restores them).
- **Input:** the game window is subclassed and feeds `win32_platform`; while open the game can optionally get no
  keyboard / mouse (`options.block_game_input`).
- **Pieces:** `strata_overlay` (static lib for your dll), `strata_overlay_demo` (sample dll), `strata_overlay_inject
  <pid|exe> <dll>` (LoadLibrary injector), `strata_overlay_host` (a stand-in game used by ctest).
- **Direct3D 12:** the queue seen calling `ExecuteCommandLists` is hooked the same way; the overlay submits its own
  command list before Present.
- **Limits:** Vulkan / OpenGL need their own hook; not intended for online games with anti-cheat.

## VR and XR overlays

`STRATA_BUILD_OPENXR` / `STRATA_BUILD_OPENVR` put the same 2d ui on a quad placed in the tracking volume, raycast
by the controllers instead of a mouse. Not yet verified on real hardware.

```cpp
strata::vr_panel panel;
panel.width_meters = 0.6f;
panel.head_locked  = true; // rides along with the hmd; see vr_panel for a panel that stays put in the room instead
overlay.create("my.app", "My App", panel);
// per frame: overlay.update_controllers(); input = vr_pointer_input(overlay.controllers(), input); ... overlay.submit_texture(tex);
```

- **OpenXR** (`xr_session`): a composition quad layer in the session's LOCAL space; `sandbox --xr`.
- **OpenVR** (`vr_overlay`): a SteamVR `IVROverlay` quad. Aiming uses the Input System's per-model tip pose (a
  generated action manifest), falling back to the raw device pose if that fails to bind.
- `sandbox --openvr`, and `overlay/demo/demo_dll.cpp` mirrors the in-game overlay into a SteamVR panel.

## Hosting, state, HDR and diagnostics

**`strata::app`** (target `strata::app`, `strata/app.hpp`) is a ready-made host: per-monitor-dpi window, flip-model
d3d11 swap chain, resizing, idling, device loss recovery, vetoable close and a saved layout file. strata itself never
creates a window or device.

```cpp
auto app = strata::app::create({.title = "tool", .follow_system_theme = true, .state_file = "tool.ini"});
return app->run([&](strata::app&, strata::context& ui) { if (auto w = ui.window("hello", {40, 40}, 300.0f)) { ui.text("hi"); } });
```

- **State:** `ui.save_state(config)` / `ui.load_state(config)` persist dock layout, windows, table columns, open
  tree nodes and scroll offsets.
- **HDR / srgb targets:** `renderer.set_output({output_space::scrgb | hdr10 | srgb_view, paper_white_nits})`; the
  overlay detects it from the swap chain automatically.
- **Lost device:** `renderer.device_lost()`, then recreate the device, `ui.rebuild_font_atlas()`, `renderer.create()`
  and textures.
- **Tests:** `strata_render_test` (hostile pipeline state, output encodings, device recovery), `strata_app_test`, and
  an `x64-asan` preset with `strata_fuzz` (libFuzzer over config, theme, dock layout, rich text, chords).

## TODO

**Text**
- [ ] OpenType shaping for Indic / Southeast Asian scripts (Devanagari, Thai, Tamil, Khmer ...), color emoji, explicit bidi embeddings,
      right-aligned right-to-left paragraphs

**Input and windows**
- [ ] Keyboard navigation of widgets and menus (Tab / arrows; today only text fields, combos and hotkeys take the keyboard)
- [ ] Multi-viewport (windows outside the application window)
- [ ] Horizontal scrolling for windows and tables themselves (today only a `child_flags::horizontal` child scrolls sideways)

## Ideas

Would fit, not promised.

**Widgets**
- Radio buttons and a list box
- Range slider (two handles), an angle knob, a slider with a log / nonlinear scale
- A splitter widget: a draggable divider between two regions inside one window (docking has its own)
- Typing into the date field, week numbers, date ranges, a setting for the first day of the week
- Tabs that tear off into windows or move between two tab bars
- Multi-select with Shift / Ctrl in lists, trees and tables; rows that can be dragged to reorder in a table

**Editor**
- Multiple carets, code folding, a minimap, an autocomplete popup (the popup API is there for it), comment toggling per language

**Input**
- Files dropped on the window from the OS (`WM_DROPFILES`) as `input_state::dropped_files`
- Gamepad input, together with keyboard navigation

**Platform and rendering**
- A Vulkan or OpenGL backend (the in-game overlay's README lists the missing one as a limit)
- Screen reader support (UI Automation)
- Images from the clipboard (paste a picture into `image()`)

## License

This project is licensed under the [MIT License](LICENSE).

## Acknowledgments

Portions of this project's documentation and code were developed with assistance from Claude (Anthropic). Design decisions, architecture and direction are my own.

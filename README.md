# strata

Immediate-mode UI framework for Direct3D 11 and 12. C++ latest (`/std:c++latest`), CMake 4.3+, Windows x64, MSVC.

```
strata/    the library  (strata::strata, static)
sandbox/   test application, same UI on d3d11 or d3d12
cmake/     options, compile flags, hlsl embedding, package config
```

## Build

| Where | How |
|---|---|
| VS Code | open the folder with the *CMake Tools* extension, pick a preset (`x64-debug` / `x64-release`), build; F5 launches `sandbox (dx11)` / `sandbox (dx12)` |
| Visual Studio | *Open Folder* on this directory (Ninja presets), **or** generate a solution: `cmake --preset vs2022` then open `build/vs2022/strata.sln` |
| Terminal | `build_vs.cmd x64-release` (loads the VS x64 environment, configures, builds) |

Visual Studio's *Open Folder* uses its bundled CMake; `cmake_minimum_required(4.3)` needs a VS
that ships CMake >= 4.3, otherwise generate the solution with the `vs2022` preset from a
CMake 4.3 command line.

Options (`-D`): `STRATA_BUILD_DX11`, `STRATA_BUILD_DX12`, `STRATA_BUILD_SANDBOX`, `STRATA_STATIC_CRT` (/MT),
`STRATA_FAST_MATH` (/fp:fast), `STRATA_AVX2` (/arch:AVX2), `STRATA_WERROR`, `STRATA_INSTALL`, `STRATA_BUILD_TESTS`.

Tests: `ctest --test-dir build/x64-release` runs the headless self-test and compares screenshots of the sandbox scenes with
`tests/golden/*.png` (`-L gpu` selects only the screenshots, they need a desktop session and a gpu). After a change that
is meant to look different, `cmake --build build/x64-release --target strata_update_goldens` rewrites the goldens (review the
diff of the images!). A golden is what one machine drew: another machine with other fonts needs its own.

Requirements: `/arch:AVX2` binaries fault with an illegal instruction on CPUs without AVX2 - configure with
`-DSTRATA_AVX2=OFF` for older machines. `fxc.exe` (Windows SDK) compiles the shaders at build time and
embeds the bytecode, so no shader files or `d3dcompiler` DLL are needed at runtime.

## Sandbox

```
strata_sandbox.exe [options]        (strata_sandbox.exe --help lists everything)

  --dx11 | --dx12          graphics backend (default dx11)
  --vsync | --novsync      present interval
  --fps N                  frame cap, 0 = unlimited (default: display refresh; some drivers ignore vsync)
  --width N --height N     client size (default 1280x720)
  --frames N               exit after N frames (smoke test)
  --stress N               buttons in the stress window (default 60)
  --theme N|NAME           a built-in theme by index or name (midnight, light, ocean, rose, dracula, nord, solarized_dark,
                           solarized_light, high_contrast, forest, amber, glass);  --theme-file FILE applies a theme file on top
  --scale F                ui scale (default: the monitor's dpi scale; --width / --height are logical pixels)
  --scene NAME             only one scene: default, features, visuals, inputs, multiselect, charts, textures, scripts, textlog, menus, context, modal, toasts, ...
  --menu                   only the sidebar settings-menu example
  --features               only the docked feature windows: multi-line input, rich text, images, nested tables
  --selftest               headless checks of the ui logic (text editing, docking, menus, modals, ...), exit code 0 = passed
  --shot FILE              render --shot-frames (24) fixed 1/60 s steps, save the last frame as a png (read back from the gpu) and exit
  --golden FILE            the same, but compare with the png FILE; exit code 0 = match (--update-golden rewrites it)
  --log FILE               stderr (D3D debug layer messages in Debug builds, frame report) to a file

  --font FILE | --face NAME --size PX          primary font (id 0)
  --mono FILE|NAME --mono-size PX              second font (id 1, default Consolas 13)
  --heading FILE|NAME --heading-size PX        third font (id 2, bold, default Segoe UI 22)
  --icons FILE|NAME --icon-size PX             icon font (id after the extras; default Segoe MDL2 Assets,
                                               then Segoe Fluent Icons, then none), --no-icons
  --no-extra-fonts   --cjk   --no-kern
```
Esc closes the window. Click a window to raise it; the "overlap A / overlap B" windows show the stacking.
The "show feature windows (docking)" checkbox docks the feature windows into left / right / bottom edge docks, the client area
and a floating "Tools" dock (drag any of them around: to a side of the app, into the floating dock, or out again).

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

`d3d11_renderer::render` saves and restores every pipeline stage it touches. `d3d12_renderer::render`
records into your open command list and expects the usual frames-in-flight fence discipline.

## Design notes

- **No heap traffic in steady state.** Vertex / index / command arrays are `vmem_array`s: reserved address
  space committed in 64 KiB chunks, never moved, cleared per frame.
- **Logical pixels.** The ui lays out in logical pixels; the draw list multiplies by the ui scale on the way out, so the
  renderers only ever see physical pixels (see *DPI and UI scale*).
- **16-byte vertex** (`float2` pos, `unorm16x2` uv, `rgba8`), one atlas, one shader pair, draws merge per clip rect
  (an image takes a command of its own, since a command draws with one texture).
- **Rounded shapes are analytic.** One quad per shape (an 80-byte record); the pixel shader evaluates a signed distance field with per-corner radii, an angled or radial gradient, inner border and soft drop shadow. Axis-aligned plain rects take a 4-vertex fast path.
- **Animated widgets** (hover / press / toggle) via a small fixed hash table keyed by widget id.
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

- **Text input:** single-line, UTF-8 aware. Click / drag to place the caret and select, double-click selects a word, triple-click a line,
  arrows, Home / End, Ctrl+arrows (by word), Shift extends the selection, Backspace / Delete, Ctrl+A / C / X / V,
  **Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo**. The host feeds `input_state::keys` and `typed`; `win32_platform` does that
  from `WM_KEYDOWN` / `WM_CHAR`, and `ui.set_clipboard(platform.clipboard())` enables copy / paste.
  `ui.want_text_input()` is true while a field has focus (the sandbox uses it so Esc doesn't close the window while typing).
- **Undo / redo:** every change of the focused field is recorded (up to 256 steps / 1 MiB). Typing, Backspace and Delete in
  a row (within a second) are one step; a paste, cut or replaced selection is a step of its own. The history lives only
  while the field has focus, is kept in zeroed-on-free memory like the edit buffer, and password fields keep none.
- **Multi-line input:** `ui.input_multiline(label, text, size, flags, hint, max_bytes)` for `std::string` or `secure_string`.
  Enter starts a line (Ctrl+Enter is `input_submitted()`), Up / Down keep their column, Page Up / Down, Home / End (by line),
  Ctrl+Home / End (by text), Tab inserts four spaces, click / drag / double-click / triple-click select (a triple click takes the
  line between two line breaks, with its break), the wheel and a scrollbar scroll.
  Lines wrap at word boundaries (a word wider than the field breaks between characters) unless `input_flags::no_wrap`,
  which scrolls sideways instead. `input_flags::read_only` gives a selectable, copyable view. Pasted text keeps its line
  breaks (`\r\n` and `\r` become `\n`) and tabs become spaces. It shares the caret, selection, clipboard and undo code of the
  single-line field. The lines are recomputed only when the text, width or font changes.
- **Combo box:** the popup is drawn in an overlay layer above every window, flips upward near the bottom edge, scrolls
  with the wheel when it has more than 8 rows, supports Up / Down / Enter / Esc, and a click outside only closes it.
- **Tabs:** a header row with a sliding highlight; you choose what to draw for the selected index.
- **Icons:** bake `glyph_ranges::private_use` from an icon font (Segoe MDL2 Assets / Segoe Fluent Icons ship with
  Windows; `font_config::exact_face` makes a missing font an error instead of a silent substitute) and use
  `strata/icons.hpp` for named code points, or any code point as `strata::glyph_string{0xe80f}`.

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

- **Color picker:** saturation / value square, hue bar, alpha bar over a checkerboard, live preview and a hex field that
  accepts `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`. It keeps its own HSV while you drag, so grays and black do not lose
  the hue. `color_edit` shows it in a popup (flips upward near the screen edge, closes on Esc or a click outside).
- **Popups:** `begin_popup_at` / `end_popup` are the general mechanism behind the combo list and the color popup:
  drawn above every window with the normal layout redirected into them, so any widget works inside.
- **Trees:** guide lines, animated arrow, `arrow_only` (arrow toggles, the row is read with `ui.item_pressed()`),
  `selected`, `default_open`. `selectable` is the same row without children.
- **Tables:** widths are stored as fractions of the table width, so they survive window resizing and drag-resizing
  keeps the total constant. Each cell is clipped to its column. With a height, the body scrolls (wheel or scrollbar)
  under a fixed header; `table_next_row()` returns false for rows outside the view so big tables stay cheap.
  Rows use the previous row's height for their background (uniform rows look exact, mixed heights are drawn
  slightly off for one row).
- **Nested tables:** `begin_table` inside a cell of another table (up to five levels deep) works like any other; the outer
  row grows to fit it. Tables repeated in rows need distinct ids so each keeps its own column widths and scroll position:
  `ui.push_id(row_key); if (ui.begin_table("items", 2)) {...} ui.pop_id();`. The mouse wheel goes to the innermost table
  (or child region, or window) under the pointer that can still scroll.

## Images and docking

```cpp
// images: the renderer owns the texture (rgba8, straight alpha); the ui only draws it
strata::texture_id tex = renderer.create_texture(w, h, pixels);        // d3d11_renderer / d3d12_renderer, 0 on failure
ui.image(tex, {96, 96});                                               // size.x == 0: layout width, size.y == 0: square
ui.image(tex, {96, 96}, {0.25f, 0.25f}, {0.75f, 0.75f}, tint, /*rounding*/ 12.0f);   // crop, tint, round corners
if (ui.image_button("open", tex, {64, 64})) { /* clicked */ }
ui.draw().image(rect, tex, uv0, uv1, tint, strata::radii(8));          // or straight into the draw list
renderer.destroy_texture(tex);                                         // d3d12: after the gpu is done with it

// other formats, mip maps, updates
strata::texture_desc d{w, h, strata::texture_format::bgra8, /*mip_levels: 0 = the whole chain*/ 0, /*updatable*/ true};
strata::texture_id dyn = renderer.create_texture(d, pixels);           // rgba8 bgra8 r8 a8 rgba16f
renderer.update_texture(dyn, x, y, rw, rh, new_pixels);                // a rectangle, in the texture's format; the mips follow

// docking: regions that windows can be dropped into (every frame, before the windows)
strata::rect client{{0, 0}, ui.display_size()};
client = ui.dock_edge("explorer",  strata::dock_side::left,   260, client);   // side panels: take room only while used
client = ui.dock_edge("inspector", strata::dock_side::right,  320, client);
client = ui.dock_edge("console",   strata::dock_side::bottom, 200, client);
ui.dock_area(client);                                                      // the main space: what is left
ui.floating_dock("Tools", {330, 80}, {360, 330});                          // a movable panel that windows dock into
if (auto w = ui.window("inspector", {40, 40}, {300, 400}, strata::window_flags::dockable | strata::window_flags::resizable)) { /* ... */ }
ui.dock_window("inspector", strata::dock_zone::right, "viewport", 0.3f);    // optional: a default layout
ui.dock_window("log", strata::dock_zone::center, {}, 0.5f, "console");     // ... or straight into a space

std::string layout = ui.dock_save_layout();      // the whole arrangement as text: keep it in a file / settings blob ...
ui.dock_load_layout(layout);                     // ... and bring it back (before or after the windows were shown)
```

- **Images:** one textured quad per image, corners rounded analytically by the pixel shader (the same signed-distance
  test as the shapes), so a rounded, tinted, cropped image costs no more than a plain one. The renderer hands out ids;
  `d3d11_renderer` keeps a list of shader resource views, `d3d12_renderer` a slot in its descriptor heap (255 textures,
  `create_texture` uploads on the queue given to `create()` and waits). A command that refers to a destroyed texture draws
  nothing.
- **Texture formats:** `texture_format::rgba8`, `bgra8` (decoder / GDI order), `r8` (a grey level, shown as opaque grey), `a8` (coverage:
  white with that alpha, so `tint` colors it: masks, icons) and `rgba16f` (half floats, the displayed range is 0..1). `r8` / `a8` are
  expanded to rgba8 on the cpu (one code path in both renderers); the others upload as they are.
- **Mip maps:** `texture_desc::mip_levels` = 1 (none), 0 (the whole chain) or n. The levels are made on the cpu with a 2x2 box filter
  that weights colors by alpha (transparent texels do not darken the edge of a shape), and the pixel shader picks the level from how
  much the quad shrinks the image (trilinear), so a picture drawn far below its size stays smooth instead of shimmering. Images
  that are drawn at their size or larger read level 0 as before.
- **Texture updates:** a texture created `updatable` keeps a cpu copy (wiped when the texture is destroyed; without the flag the
  copy is wiped right after the upload). `update_texture(id, x, y, w, h, pixels)` replaces a rectangle and rebuilds only the parts
  of the mip levels it touches: d3d11 `UpdateSubresource`, d3d12 a blocking copy on the queue (which runs after the frames already
  submitted, so the ones in flight are undisturbed). `strata::texture_image` (public, `strata/texture.hpp`) is the cpu side -
  conversion, mip chain, dirty regions - for hosts with a renderer of their own.
- **Docking:** a window with `window_flags::dockable` that is dragged by its title bar over the dock area shows where it would
  land: the centre of a pane adds a **tab**, its edges **split** the pane, the border of the area splits the whole tree.
  Let go and it fills that pane (tab bar instead of a title bar, no move / resize of its own, scrolls when its content
  does not fit). Drag the splitters to resize panes, drag a tab away to float the window again. A window that is not
  submitted for a frame leaves the dock and its pane collapses into its sibling. Docked windows sit below floating ones,
  the tab bars and splitters count for `want_capture_mouse()`. `dock_window(title, zone, target, size)` /
  `undock_window` / `is_docked` / `window_rect` do the same from code (e.g. for a default layout). Up to 32 panes; without a space a docked window just floats where it last was.
- **Dock spaces:** every dock area is a *space* with its own tree of panes, up to 8 at once, so docks are not all over the app:
  `dock_area(rect)` is the main one, `dock_area("name", rect)` adds more, `dock_edge(name, side, size, region)` is a panel
  along the left / right / top / bottom of a region that returns what is left, so several chain around the client area
  (an empty edge dock takes no room and shows a thin drop strip along the side while a dockable window is dragged; the
  user resizes an occupied one by its handle), and `floating_dock(title, pos, size)` is a window that is itself a space:
  windows dock into it, move, stack and collapse with it. `dock_window(title, zone, target, size, space)` docks from code.
- **Whole panes:** what is left of a tab bar right of its tabs is a grip (six dots). Drag it and the whole pane, every tab of it,
  travels: a ghost of the tab bar follows the pointer, the same drop previews show where it lands (a pane cannot be dropped on
  itself), and on release the tabs join / split the target together, keeping their order and the selected one. Let go over no dock
  and the windows float again, fanned out from the pointer.
- **Saving a layout:** `dock_save_layout()` returns plain text (`strata-dock 1`, then per space its splits, ratios, tabs, selected
  tab and edge-dock size, and per window its position, size and collapsed state). `dock_load_layout(text)` replaces the current
  arrangement: windows and spaces are matched by name, so what is not shown any more is skipped and what is not in the text keeps
  floating; text that is not a layout (or does not fit in 32 panes) returns false and changes nothing. Save it whenever you like,
  e.g. at exit, and load it at start up.

## DPI and UI scale

```cpp
ui.set_scale(platform.dpi_scale());        // 1.0 = 96 dpi, 1.5 = 144 dpi ...  (rebuilds the font atlas)
renderer.update_atlas(ui.font());          // d3d11 / d3d12: upload it
ui.release_font_pixels();
// on WM_DPICHANGED: the same three lines with the new platform.dpi_scale()
```
Everything - padding, rounding, icons, table columns, your `custom_item`s - stays in logical pixels; `input_state::mouse_pos` and
`display_size` arrive in physical ones and are converted, draw commands leave in physical ones, and glyphs are rasterised at
`scale` times their configured size, so text stays sharp instead of being stretched. `font_atlas` has both flavours of metrics
(`measure` / `line_height` in logical, `*_px` in physical pixels). Scaling costs one atlas rebuild (0.3 s for the sandbox's four fonts);
font `ranges` / `data` given to `create()` must outlive the context if you rescale. `context::scale()`, `font_generation()`.
The sandbox uses the monitor's dpi at start, follows `WM_DPICHANGED`, and has an "ui scale" combo.

## Drawing: gradients, curves, acrylic

```cpp
dl.rect_gradient_angle(r, from, to, /*degrees*/ 45.0f, /*rounding*/ 10.0f);   dl.rect_gradient_radial(r, centre, edge, 10.0f);
dl.polyline(points, color, /*thickness*/ 2.0f, /*closed*/ false);             dl.bezier_cubic(p0, p1, p2, p3, color, 3.0f);
dl.arc(centre, radius, a0, a1, color, 8.0f);     dl.circle(c, r, color, 1.0f);   dl.circle_filled(c, r, color);
dl.backdrop(rect, /*blur*/ 18.0f, /*tint*/ color, radii(10.0f));                   // frosted glass
ui.window("glass", pos, size, strata::window_flags::acrylic);                        // or child_flags::acrylic
```
- **Gradients:** a `shape_style` has `gradient_dir` (any direction; `gradient_direction(degrees)`) and `radial`; `fill_top` is where it
  starts, `fill_bottom` where it ends. Still one quad per shape.
- **Lines and curves:** `polyline` builds a strip with mitred joins and a one-pixel antialiasing fringe (butt ends); curves and arcs
  are tessellated to it (segment count from the length unless given). `polygon_filled` fills a convex polygon.
- **Acrylic / blur:** a backdrop command copies the render target so far, boxes it down (1/2 or 1/4 size), blurs it with a
  separable gaussian (two to six passes for big radii) and draws the panel from that - rounded, tinted (`style.window_bg` alpha *
  `acrylic_alpha`) and with a fine grain (`acrylic_noise`). One blur serves consecutive panels; a new one is made when something
  was drawn in between, so panels stack correctly. `style.blur_radius` is the radius. d3d11 reads the bound render target itself;
  d3d12 needs `renderer.render(data, list, frame, &d3d12_target{resource, rtv_handle_ptr})`, without it (or with a multisampled
  target) backdrop panels degrade to flat tints. Theme `glass` is made for it.
- **Saturation / brightness:** `style.acrylic_saturation` (0 grey, 1 as it is, > 1 vivid) and `acrylic_brightness` re-grade the blurred
  frame before the tint goes over it (`draw_list::backdrop(..., noise, saturation, brightness)`).
- **Glass popups:** `style.popup_acrylic` (0 opaque, 1 glass) makes menus, dropdowns, tooltips and toasts frosted panels: shadow, then
  the backdrop with the tint blended toward `acrylic_alpha`, then the border. A popup that is still fading in is drawn opaque. All three
  are keys of the theme file (theme `glass` turns them on).

## Themes and theme files

```cpp
ui.theme() = strata::themes::nord();                 // 12 built in: midnight light ocean rose dracula nord solarized_dark
strata::themes::by_name("forest", ui.theme());       // solarized_light high_contrast forest amber glass; themes::names()
strata::themes::save_file("my.theme", ui.theme(), "my theme");
strata::themes::theme_result r;  strata::themes::load_file("my.theme", ui.theme(), &r);   // r.applied / unknown / invalid / first_problem_line
```
A theme file is plain text, `key = value` per line, `#`, `;` or `//` start a comment line, colors are `#rgb`, `#rgba`, `#rrggbb` or
`#rrggbbaa`, and a `base = nord` line (first) starts from a built-in theme so a file can override just a few keys. `to_string` /
`from_string` do the same in memory. Keys: every `style` member (`padding`, `rounding`, `accent`, `window_bg`, `blur_radius`, ...).
The sandbox has a "theme file" field with save / load buttons and takes `--theme` / `--theme-file`.

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
- **Drag fields:** drag sideways to change the value (Shift = a tenth of the speed, Alt = ten times); a plain click (the pointer stays within
  5 px) turns the box into a text field with the number selected, so you can just type (Enter applies, Esc cancels, leaving applies); `lo < hi` clamps and makes the field a ruler: its width spans the range and the fill follows the pointer 1:1 (`speed` only matters without a range).
  Integer drags move in whole steps and carry the remainder.
- **Number fields:** `input_float` / `input_int` reparse on every change, so half-written text ("-", "1e") leaves the value alone.
- **Multi-select dropdown:** the field shows what is chosen ("a, b", "all (5)" or the placeholder); the popup has a check box per row,
  stays open while you click, has "select all" / "clear" above longer lists and works with Up / Down / Enter.
- **Plots:** min / max on the left, a grid, a hover cursor with a tooltip listing every series' value; series of any length (more samples
  than pixels are reduced to each pixel's extremes); `values` may be a ring buffer via `offset`. `plot_auto` lo / hi fit the data.
- **Charts** (`plot(label, series, plot_options)`): "nice" tick marks (steps of 1, 2 or 5 times a power of ten, as many as fit) with labels,
  units (`plot_axis::unit`, added to every label and the hover tooltip), grid lines, axis titles and a legend; `fill` shades the area
  under line series and fades it toward the axis (`draw_list::area_fill`); a histogram has bars with real widths (`x_step`). With
  `zoom_pan` the wheel zooms x around the pointer, Ctrl + wheel zooms the values, dragging pans (the values too once they were zoomed),
  a double-click resets; the view is kept per chart between frames (`plot_zoomed`, `plot_x_range`, `plot_reset_view`), it always
  overlaps the data and is limited to four times the data width. The value range follows the samples in view unless fixed or zoomed.

## Selectable text and the log view

```cpp
ui.text_selectable("this can be selected and copied");       // wraps at the layout width; drag, double-click a word, Ctrl+C
{ auto sel = ui.selectable_text();  ui.text("so can this");  ui.text_dim("and this"); }   // text() inside the scope is selectable

strata::log_buffer log{5000};                                // a ring of lines
log.addf(strata::log_level::warn, "low disk: {} MB", mb);    log.add(strata::log_level::error, "boom");             // lines are stamped with ui.time() when first drawn (shown by the "time" checkbox)
ui.log_view("console", log);                                 // toolbar + lines, fills a fixed-height window
```
- **Selectable text** is the read-only multi-line field without its frame (`input_flags::read_only | no_frame | auto_height`), so it shares
  the selection, caret-less highlighting, word selection and clipboard code.
- **Log view:** filter (case-insensitive substring), minimum level, follow (sticks to the newest line; scrolling up lets go, reaching the
  bottom takes it back), optional time column, copy, clear. Only the visible rows are drawn (12000 lines cost like 20). Click / Shift-click /
  drag select rows, Ctrl+A / Ctrl+C copy them. The view's state lives in `log.view`, so a buffer is one view.
  **`wrap`** (`log.view.wrap`, a checkbox in the toolbar) lets long lines and lines with line breaks take as many rows as they need: the
  height of each line is measured once per line and width, the rows are laid out from a table (binary search for the first visible row), so
  a wrapped log of thousands of lines still draws only what is in view. **Clock times:** every line is stamped with the system clock when
  it is added (`log.add(level, text, ui_time, wall_ms)` to replay lines with the times they had); `show_time` + `clock` shows
  `HH:MM:SS.mmm` local time (`log_buffer::clock_text(wall_ms)`), otherwise the seconds of ui time as before.

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
        strata::menu_item_options o;  o.icon = strata::icons::save;  o.icon_font = icon_font;  o.shortcut = "Ctrl+S";
        o.keep_open = true;                                                        // a click does not close the menus
        if (ui.menu_item("&Save", o)) { /* ... */ }
    }
}
if (ui.accelerator("Ctrl+O")) { /* works with every menu closed: the same strings the rows show */ }
if (auto m = ui.context_menu("row", row_rect)) { ui.menu_item("Rename"); ui.menu_item("Delete", "Del"); }  // right-click
ui.toast("Saved", "profile.json was written.", strata::toast_kind::success);       // info / success / warning / error
```
- **Modals:** centered, fading in over a dimmed area (`style.modal_dim`) that covers every window and the menu bar; the topmost one has the
  input, everything below gets no hover or clicks (`want_capture_mouse()` stays true). `modal_flags::esc_closes` / `backdrop_closes`
  (Esc is ignored while something inside has the keyboard). `dialog()` is the ready-made message box; it returns the 1-based button pressed, -1
  when dismissed, 0 while open, and closes itself.
- **Menus:** popups in the overlay layer (a menu measures itself in its first, invisible frame). A press outside closes the whole chain, Esc
  too; a plain row closes an open submenu beside it; with a bar menu open the other headers open on hover. `menu_item(label, shortcut,
  selected, enabled)` and a `bool&` overload for check items; a click closes all menus. The main menu bar is a window of its own above the
  others (`main_menu_bar_height()`), `begin_popup_menu` / `open_popup_menu` are the same popups without the right click.
  `menu_item(label, menu_item_options)` (also with a `bool&`) has everything a row can have: `shortcut` text, an `icon` glyph (any
  font, e.g. the icon font; a checked row puts it on an accent chip) or an `image` texture in the icon column, `selected`, `enabled` and
  `keep_open` (the click returns true but the menus stay: toggles, tool options).
- **Mnemonics and accelerators:** a `&` in a label marks its mnemonic (`"&Open"`, `"&&"` is a literal `&`; underlined in popups, and in the
  bar while Alt is held). While a menu is open, pressing the letter (or digit) activates that row of the deepest open popup, or opens the
  submenu; Alt + the letter opens a menu of the bar (the win32 platform swallows the system beep of Alt + letter). `accelerator("Ctrl+Shift+S")`
  is true on the frame the combination is pressed (exact modifiers; names: letters, digits, F1-F24, Enter, Esc, Space, Tab, Backspace, Del,
  Ins, Home, End, PgUp, PgDn, arrows); with a text field focused only Ctrl / Alt combinations count, and not its own Ctrl+A/C/V/X/Z/Y.
- **Toasts:** stacked newest-first in a corner (`set_toast_corner`), sliding and fading; hovering one pauses its timer, clicking dismisses
  it; at most 8 at a time. `ui.toast(toast_options)` returns a `toast_handle` and adds **buttons** and **progress**:
  `.actions = {"Undo", "Details"}` (up to three; pressing one closes the toast and `ui.toast_action(handle)` says which, once) and
  `.progress` (0..1: a bar that fills, `toast_busy`: an endless one; `.seconds = 0` keeps it until closed).
  `toast_progress(handle, fraction, text)` feeds it (1.0 completes it: it closes 2 s later), `toast_close`, `toast_alive`. A toast with
  buttons or progress is not dismissed by a click on its body (a small x shows while it is hovered); a plain one still is.

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
`strata_sandbox --menu` shows exactly this as a working example (five pages; the pages are placeholders).

- **Window flags:** `no_title_bar`, `no_collapse`, `no_move`, `no_background` (content only, no fill / border /
  shadow), `drag_by_body` (dragging any empty spot moves the window), `resizable`, `dockable`, `acrylic` (frosted glass).
- **Child regions:** `begin_child` / `end_child` or the scoped `ui.child(id, size, flags)`: a clipped, scrollable area with its
  own scrollbar (wheel goes to the innermost scroller under the pointer). `size.x == 0` is the rest of the line (right of
  the previous item after `same_line()`), `size.y == 0` fills down to the bottom of a fixed-height window.
  `child_flags::frame` draws a background, `no_padding`, `no_scrollbar`. Children nest (up to 4 deep).
- **Cards:** `ui.card("Title", icon, icon_font)`: a titled group box that grows with its content (the height comes from
  the previous frame, so the very first frame shows only the header).
- **Tab strip:** `ui.tab_strip(id, tabs, count | {..}, selected, icon_font, width, flags, height)`, a vertical sidebar with a sliding
  highlight; `tab_strip_flags::icons_only` makes a narrow icon rail whose labels appear as tooltips.
- **Tooltips:** `ui.tooltip("text")` after any widget; shows after the pointer rests on it. `ui.item_hovered()`.
- **Hotkeys:** `ui.hotkey("Label", key_code)`: click, press a key or a side mouse button; Esc cancels, Backspace / Delete
  unbinds. `key_code` is a virtual-key code (`strata::key_name(code)` gives its name). `input_state::pressed_key` carries the
  key; `win32_platform` fills it.
- **Docking animation:** `ui.set_dock_animation(true)` makes panes slide to their new place when a window docks, undocks or a pane closes
  (a new pane grows out of the edge it was dropped at); dragging a splitter always follows the pointer. Off by default.
- **Alpha and transitions:** `ui.push_alpha(a)` / `pop_alpha()` scale everything drawn (nearly invisible content is inert);
  `ui.page_transition(key, page, slide)` fades and slides the content that follows when `page` changes.
- **Layout helper:** `ui.same_line(x)` continues on the same line at an x offset from the left of the content area
  (right-align a close button with `content_width() - width`).

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
`<f=N>...</f>` switches to font id N, `<c=rrggbb>` / `<c=rrggbbaa>` ... `</c>` changes the color, `<b>` bold, `<i>` italic, `<u>` underline
and `<s>` strike-through (each closed by `</b>` ...; they combine: `<b><i>both</i></b>`), `<<` is a literal `<`. Tags nest.
The styles are synthesized, so they work with any font and change no advance (layout is the same as for plain text): bold is a
second strike a pixel to the right, italic slants the glyph quads, the lines follow the text. They are also a `text_flags` argument of
`draw_list::text(pos, color, text, font, text_flags::bold | text_flags::underline)`. For a real bold face pick another font id.
The runs of a line share one baseline and the line is as high as its tallest run; `\n` starts a new line and, with
`rich_text_wrapped` / `text_wrapped`, lines also break before a word that does not fit (a longer word breaks between
characters). Icons work the same way: `<f=3>` + an icon code point.
While `rich_labels()` is alive the labels of buttons, checkboxes, toggles, field captions, tab bars and strips, tree rows,
selectables, combo items, card titles, table headers and tooltips (and `text()`) are markup; the widgets size themselves
to the mixed-font text. Widget ids still come from the raw string, table headers and `text_ellipsis` are clipped instead
of cut. The *contents* of text fields are plain text, but can be styled: see below.

**Styled contents of text fields.** `ui.input_spans(spans)` before the next `input_text` / `input_multiline` gives ranges of the
text (`text_span{start, end, font, color, style}`, byte offsets) their own font, color and bold / italic / underline / strike:
syntax highlighting, a live markup preview, a mono font for code. Lines are as high as the tallest font in them and runs share a
baseline; wrapping, caret, selection and mouse hits measure the mixed runs. The field stays plain text (undo, copy and paste see only
the text): recompute the spans from it when the field reports a change; ranges are clamped, overlaps resolved (the first wins) and
bad font ids fall back, so stale spans for a frame are harmless. Password fields ignore them.

**Input methods (IME).** While the user composes text (Chinese, Japanese, Korean ...), the composition is shown at the caret of the
focused field - inline and underlined in a single-line field, in a small box at the caret in a multi-line one - and only inserted
when confirmed. `input_state::ime` / `ime_len` / `ime_cursor` carry the composition (`win32_platform` fills them from
`WM_IME_COMPOSITION`); what the user confirms arrives as `typed`. Call `platform.set_ime(ui.ime_wanted(), ui.ime_position(),
ui.ime_line_height())` after `end_frame()`: the IME is on only while a text field has the keyboard (hotkeys keep working under a CJK
layout; password fields turn it off) and the candidate window sits at the caret. The window procedure returns 0 for messages where
`win32_platform::swallows(msg)` is true (the IME's own composition window is suppressed; the ui draws it).

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

`style` covers colors, rounding, padding, spacing, border width, shadow, gradient strength and animation
speed; `style_color` / `style_var` name what `push_color` / `push_var` / `style_overrides` can change.

## Windows

Windows are draggable, collapsible, and stack: pressing anywhere on a window raises it above the others (this
frame, without re-recording anything: the draw commands of each window are contiguous and get reordered at
`end_frame`). Input goes to the topmost window under the pointer, and the unfocused windows' titles dim.
`ui.want_capture_mouse()` tells a host application when the pointer belongs to the UI.

**Resizing:** `ui.window("title", pos, {width, height}, strata::window_flags::resizable)`. The right edge, the bottom edge
and the bottom-right corner are drag handles (marked by a grip in the corner); the size is remembered, minimum 150 px wide.
`height == 0` means "follow the content" until the user drags a vertical edge, after which the height is fixed and the
content scrolls (wheel or scrollbar; tables and combo lists inside get the wheel first). Windows can also be created
with a fixed height and no resizing. The plain `ui.window("title", pos, width)` form is unchanged.

**Pointer shape:** `ui.cursor()` reports what the pointer should look like (arrow, text I-beam, resize arrows). With
`win32_platform`: `platform.set_cursor(ui.cursor())` after `end_frame()` and, in your window procedure,
`case WM_SETCURSOR: if (LOWORD(lparam) == HTCLIENT && platform.apply_cursor()) return TRUE;`.

## Footprint

What the library leaves behind, measured on the running sandbox (handle / module / file / registry checks, and a scan of
the process memory for text typed into a field):

- **Disk / registry / threads / hooks:** none. Statically linked, no third-party DLLs; only system DLLs load, and each backend
  delay-loads its own D3D DLL (`d3d11.dll` only in DX11 mode, `d3d12.dll` only in DX12 mode).
- **Typed text:** the edit buffer is a `secure_string` (its memory is zeroed whenever it is freed or reallocated) and is wiped as soon as
  the field loses focus, when focus moves to another field, and when the context is destroyed. Typed-character buffers (platform
  and context), pasted text and copies inside the temporary `input_state` (`begin_frame(platform.new_frame())`) are zeroed after use.
  The undo history of a field holds typed text too: it is kept in the same zeroed-on-free memory and dropped with the
  edit buffer. Verified: after a field loses focus the only copy left in process memory is the application's own variable. For secrets use
  `input_text(label, secure_string&, ...)` or the fixed-buffer overload; a plain `std::string` may leave partial copies when it grows.
- **Geometry:** the vertex / index / shape arrays are zeroed before their pages go back to the OS; the D3D upload buffers are
  zeroed on `destroy()` (best effort on D3D11: the driver may keep older renamed copies; on D3D12 idle the GPU first).
- **Fonts:** the CPU bitmap is freed with `ui.release_font_pixels()` (zeroed first); font file bytes and glyph scratch buffers are
  zeroed; a font from a file / memory is registered with GDI only while the atlas is built. No font blob is embedded.
- **Not covered:** GPU / driver memory (the atlas texture stays until released), the application's own strings, the swap file, and
  copies the OS clipboard holds after a copy.


## Fonts, Unicode, kerning

```cpp
strata::context_config cfg;
cfg.font.file         = "C:/Windows/Fonts/consola.ttf";   // or cfg.font.data = embedded bytes, or cfg.font.face = "Segoe UI"
cfg.font.pixel_height = 15;
cfg.font.ranges       = my_ranges;                         // glyph_ranges::latin, greek, cyrillic, cjk_unified, ...
auto ui = strata::context::create(cfg).value();
```

- **Multiple fonts:** `cfg.extra_fonts` adds fonts 1, 2, ...; all of them are packed into one shared atlas texture, so
  the UI stays a single pipeline state. `ui.push_font(id)` / `pop_font()` or the scoped
  `auto f = ui.with_font(1);` switch the font for everything submitted after it: text and widget sizes both follow
  it. Window titles always use font 0.
- **Custom fonts:** `.ttf` / `.otf` / `.ttc` from a file or from memory. The font is installed privately for the
  process while the atlas is built and removed again right after; nothing is left registered.
- **Unicode:** UTF-8 is decoded fully. You choose which blocks are baked (`font_config::ranges`, any plane: emoji and other
  supplementary characters work, their glyphs are found through the font's cmap). The
  atlas grows from 256x256 up to `max_atlas_size` (4096 default). Code points the font lacks or that were not baked
  draw `?`. CJK ideographs are ~21k glyphs and need a font that has them (`--font C:\Windows\Fonts\msyh.ttc --cjk`
  in the sandbox; ~2.7 s to build in Debug).
- **Fallback faces and emoji:** `font_config::fallback_faces = {"Segoe UI Emoji", "Segoe UI Symbol"}` gives a text font the glyphs it
  lacks: while baking, every requested code point the main face does not have is taken from the first fallback that has it, at the same
  size and on the same baseline (so it costs nothing at run time). `glyph_ranges::emoji` (~1500 glyphs), `symbols`, `math_alphanumeric`.
  Emoji are single-color (the color layers of the font are not used). Zero-width joiners, variation selectors, direction marks and
  skin-tone modifiers take no room, so `\U0001F44D\U0001F3FD` shows a thumb, not a thumb and a box.
- **Right-to-left text:** `glyph_ranges::hebrew`, `arabic`, `arabic_forms_a` / `arabic_forms_b` (the joined letter shapes). Whenever a
  string has a right-to-left character, `draw_list::text` and `font_atlas::measure` reorder it (the Unicode bidi algorithm for one
  paragraph per line: weak / neutral / number rules, bracket pairs (N0), mirrored brackets, trailing spaces) and join Arabic letters
  (initial / medial / final / isolated forms, lam-alef ligatures, marks stay transparent; a font without the presentation forms keeps
  the plain letters), so labels, buttons, tooltips, rich text, the log - every widget - show Hebrew and Arabic correctly, with no
  cost for text that has none (a byte scan rejects it). The text fields keep the text in logical order and use `bidi_layout` for the caret
  and the mouse: Home / End, clicks and selections follow the reordered letters (a selection across directions is drawn as one rectangle
  between its ends). `strata/bidi.hpp` (`has_rtl_text`, `to_visual`, `bidi_layout`) is public. Not done: explicit embeddings /
  isolates (U+202A-202E, U+2066-2069 are ignored), text alignment to the right edge for right-to-left paragraphs, and OpenType shaping
  for Indic and Southeast Asian scripts (Devanagari, Thai, Tamil ... need GSUB / GPOS: a shaping engine, see the TODO).
- **Kerning:** the font's `kern` table is applied to measuring and drawing, in integer pixels like the advances.
  Fonts that only kern through OpenType GPOS get no kerning (`font_atlas::kerning_pair_count()` is 0 then).

## In-game overlay (direct3d 11 and 12)

`overlay/` turns strata into an in-game overlay: a dll that is loaded into a direct3d 11 program (a game, for modding tools and
inspectors) and draws a strata ui over every frame, with the game's keyboard and mouse taken while it is open.

```cpp
strata::overlay::options opt;
opt.ui = [](strata::context& ui) { if (auto w = ui.window("my tool", {40, 40}, {360, 0})) { ui.text("hello"); } };
strata::overlay::install(opt);       // from a thread of your own, not from DllMain; F1 (opt.toggle_key) shows / hides it
```
- **The hook:** a dummy swap chain gives the address of `IDXGISwapChain`'s vtable (all swap chains of that implementation share it);
  `Present`, `Present1` and `ResizeBuffers` are replaced in it, no code is patched, and `uninstall()` puts the slots back. The first swap
  chain that presents with a real window is the game's: the ui goes into its back buffer just before Present (the render target bindings
  are restored, `d3d11_renderer` restores the rest), a target view is made per buffer size and released on `ResizeBuffers`. While hidden
  nothing is drawn.
- **Input:** the game's window is subclassed; messages go to `win32_platform`; while the overlay is open the game does not see the
  keyboard or mouse (`options.block_game_input`, or only what the ui wants when false), raw input is drained, the cursor is shown and
  unclipped every frame and set from `ui.cursor()`. Threads: the window thread feeds messages, the render thread reads the frame (a mutex
  guards the platform), `ui` runs on the game's render thread.
- **Pieces:** `strata_overlay` (static library, link it into your dll), `strata_overlay_demo` (the sample dll: tabs, a log other threads write
  to through the C api `strata_overlay_log(level, utf8)`, a theme switcher, `strata_overlay_show / _eject / _frames`), `strata_overlay_inject`
  (a LoadLibrary injector: `strata_overlay_inject <pid | exe> <dll>`) and `strata_overlay_host` (a stand-in game: flip-model swap chain,
  loads the dll like an injector, sends F1 to its window and checks that frames are drawn while open and only then; ctest runs it and writes
  a png of the frame). `STRATA_OVERLAY_SHOW=1` starts open, `STRATA_OVERLAY_CAPTURE=file.png` writes the back buffer once, `STRATA_OVERLAY_LOG=file`
  logs what the hook does.
- **Direct3D 12:** the same swap chain class is hooked, and a dummy queue gives the vtable of `ID3D12CommandQueue`, where
  `ExecuteCommandLists` is replaced: the direct queue that was seen executing command lists is the game's. Per frame the overlay records its
  own command list (transition of the current back buffer to render target, `d3d12_renderer::render`, transition back), runs it on that
  queue right before Present and signals a fence, so a frame slot's allocator and upload buffers are reused only after the gpu is done
  (waits are per back buffer index, normally free). `ResizeBuffers` waits for the overlay's work and rebuilds the render target views. The
  first frames after injecting wait until a queue has been seen. `strata_overlay_host --d3d12` tests it.
- **Limits:** Vulkan / OpenGL games need a hook of their own (the ui and the renderers are the same). A d3d12 game with several direct queues
  is served by the one that ran last. A game that recenters or locks the cursor every frame for mouse-look fights for it (in Unity set
  `Cursor.lockState = None` while the overlay is open). The overlay does not need the game's cooperation but the game's own anti-cheat
  may not like an injected dll: do not use it in online games.

## TODO

The known limitations, as a work list. `[x]` = done, `[ ]` = still open.

**1. Text**
- [x] Supplementary planes (emoji, math alphanumerics ...) through the font's cmap, and fallback faces for missing glyphs
- [x] Right-to-left text: bidi reordering, Arabic joining, caret and mouse in the text fields (Hebrew, Arabic, Persian, Urdu ...)
- [ ] OpenType shaping for Indic / Southeast Asian scripts (Devanagari, Thai, Tamil, Khmer ...), color emoji, explicit bidi embeddings,
      right-aligned right-to-left paragraphs

**2. Widgets**
- [x] Multi-line text input (`input_multiline`: wrapping, scrolling, selection, line-wise navigation)
- [x] Images (`image`, `image_button`, `draw_list::image`, `create_texture` on both renderers)
- [x] Docking (dock area, tabs, splits, splitters, drag-out, drop preview, `dock_window`)
- [x] Edge docks (left / right / top / bottom of the app), floating docks and several dock spaces at once
- [x] Undo / redo in text fields (single- and multi-line)
- [x] Nested tables (up to five levels)
- [x] Images: mip maps, texture updates, formats other than rgba8 (`texture_desc`, `update_texture`, `texture_image`)
- [x] Docking: saving / restoring a layout (`dock_save_layout` / `dock_load_layout`), dragging a whole tab group by its grip
- [x] Text fields: mixed-font *contents* (`input_spans`), IME composition (win32), triple-click to select a line

**3. Rich text**
- [x] Inline font mixing beyond one line: `rich_text` takes newlines, `rich_text_wrapped` / `text_wrapped` wrap, and
      `rich_labels()` lets every other widget draw markup captions.
- [x] Bold / italic / underline / strike-through tags (`<b>`, `<i>`, `<u>`, `<s>`; synthesized, `text_flags` in the draw list)

**4. Second round** (all done)
- [x] DPI and UI scale (`set_scale`, per-monitor DPI in the win32 platform, atlas rebuild, `update_atlas` in both renderers)
- [x] Anti-aliased lines, polylines, curves, circles and arcs in the draw list
- [x] Gradients (angled / radial) and blur / acrylic backgrounds (real backdrop blur in both renderers)
- [x] Additional themes (12) and a theme file (save / load / by name)
- [x] Drag sliders and number inputs (`drag_float`, `drag_int`, vectors, `input_float`, `input_int`)
- [x] Multi-select dropdown (`combo_multi`)
- [x] Plots and graphs (lines, histograms, sparklines, multi-series)
- [x] Text selection in plain `text()` (`text_selectable`, selectable-text scope)
- [x] Modal windows and dialogs
- [x] Context menus and a main menu bar
- [x] Toasts / notifications
- [x] Console / log view
- [x] Animated transitions for docking (optional, off by default)
- [x] More self-test coverage (540 checks; the overlay has its own smoke test) and a GPU screenshot regression test (`--shot`, golden images, ctest)

**5. Ideas** (the open ones first)
- [ ] Keyboard navigation of widgets and menus (Tab / arrows; today only text fields, combos and hotkeys take the keyboard)
- [x] Menus: mnemonics (`&Open`, Alt + letter), accelerators (`accelerator("Ctrl+O")`), icons in rows, keep-open items
- [x] Toasts: action buttons, progress toasts; log view: per-line wrapping, timestamps from the clock
- [x] Plots: axes with ticks and units, zoom / pan, area fills (`plot_options`)
- [x] Acrylic: saturation / brightness boost, blur of popups and tooltips
- [ ] Multi-viewport (windows outside the application window)

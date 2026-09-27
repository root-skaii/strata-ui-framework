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

Requirements: `STRATA_AVX2` is off by default, so the binaries run on any x64 CPU. Turned on, `/arch:AVX2` binaries
fault with an illegal instruction on CPUs without AVX2 (pre-2013 Intel / pre-2015 AMD) - only for a program that controls
the machines it runs on, never for an overlay dll. `fxc.exe` (Windows SDK) compiles the shaders at build time and
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
  --scene NAME             only one scene: default, features, visuals, inputs, multiselect, charts, textures, scripts, textlog, menus, context, modal, toasts, config, palette, tabs, dnd, lists, editor, icons, bigtree, rows, app, ...
  --icon-page HEX          with --scene icons: a raw page of 256 code points with their hex values, for picking one
  --menu                   only the sidebar settings-menu example
  --features               only the docked feature windows: multi-line input, rich text, images, nested tables
  --selftest               headless checks of the ui logic (text editing, docking, menus, modals, ...), exit code 0 = passed
  --shot FILE              render --shot-frames (24) fixed 1/60 s steps, save the last frame as a png (read back from the gpu) and exit
  --golden FILE            the same, but compare with the png FILE; exit code 0 = match (--update-golden rewrites it)
  --log FILE               stderr (D3D debug layer messages in Debug builds, frame report) to a file
  --metrics                strata's own inspector windows: frame cost, geometry, culling, idle state, and in red
                           anything that went wrong quietly; plus the live draw commands, one row each
  --idle                   skip rendering and presenting while the ui reports nothing changed; the frame report
                           then says how many frames were skipped (try `--scene icons --idle --frames 300`)

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
- **16-bit indices**, relative to each command's `vtx_offset` (`BaseVertexLocation`), which halves index bandwidth.
  A command therefore spans at most 65536 vertices; one that would reach past that is split, exactly as a clip or
  texture change splits it. Text, polylines and area fills emit in chunks so no single primitive can exceed it.
- **Nothing changed, nothing sent.** `end_frame` hashes the geometry; `ui.can_idle()` is true when this frame is
  byte-identical to the last one and no animation is still moving, and the renderers skip the buffer upload on their
  own when the hash says they already hold it (see *Idling*).
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
- **Rows outside the view cost nothing.** `selectable`, `tree_leaf`, `tree_node`, `table_tree_*` and `custom_item`
  place themselves in the layout and then stop if their rectangle does not meet the clip rectangle: no hit test, no
  animation slot, no measuring, no geometry. The layout advance, the open / closed state and the `tree_pop` nesting
  are the same either way, so the scrollbar range and everything below are unchanged and nothing has to opt in. This
  is what makes a deep tree affordable when `list_clipper` cannot be used, because the rows are not all one height:
  `--scene bigtree` is 5 704 nodes over four levels, all expanded, at 0.4 ms of UI time a frame with 22 rows drawn.
- **Telling rows apart without building strings.** Ids come from the label, so rows that repeat a name used to need a
  `name + "##" + path` per row per frame. The extra-id overloads take the identity separately -- it is hashed after
  the label and never shown -- and `push_id` also accepts a pointer, a `u64` or an `int`:

```cpp
ui.selectable(node.name, {reinterpret_cast<const char*>(&node.id), sizeof(node.id)}, node.id == selected);
ui.tree_node(ns.name, ns.full_path, tree_flags::default_open);
ui.push_id(&object);  /* rows of this object */  ui.pop_id();
```
- **Opening and closing:** `set_next_item_open(bool)` (and `set_next_item_open_recursive`) overrides the stored state
  of the next node; Ctrl or Shift held while a node's arrow is clicked applies it to the whole subtree;
  `open_all_tree_nodes()` / `close_all_tree_nodes()` do the same for everything in the current id scope, and keep
  applying for a few frames so a tree unfolds all the way instead of one level per frame.
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
- **Docking comfort:** while a window is dragged over a pane, a cross of **drop guides** shows in its middle (centre = tab, the
  four arrows = split that side) and one guide per border of a big space (splits the whole tree); the guide under the pointer
  lights up together with the preview, and the rest of the pane still works by position. Dropping on a pane's **tab bar** joins
  its tabs at the place under the pointer (a marker shows where). Drag a tab sideways along its bar to **reorder** the tabs, pull it
  down or up to tear it off. **Double-click** a tab to float its window, a splitter or an edge-dock handle to reset it (equal
  halves / the size it was given). Hold **Shift** while dragging a window (or a whole pane) to move it without docking;
  **Esc** cancels the docking of the drag in progress.
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
- **Saving a layout:** `dock_save_layout()` returns plain text (`strata-dock 2`, then per space its splits, ratios, tabs, selected
  tab and edge-dock size, and per window its position, size and collapsed state). `dock_load_layout(text)` replaces the current
  arrangement: windows and spaces are matched by name, so what is not shown any more is skipped and what is not in the text keeps
  floating; text that is not a layout (or does not fit in 32 panes) returns false and changes nothing -- that includes a
  `strata-dock 1` layout, saved before ids were 64-bit, whose window keys no longer match anything. Save it whenever you like,
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
The sandbox uses the monitor's dpi at start, follows `WM_DPICHANGED`, and has an "ui scale" combo. The dpi scale is
the right default but a poor setting: see *UI scale at runtime* below for changing it while the ui is up, which the
overlay does for you (atlas re-upload included).

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
- **Chords:** `key_chord{key, ctrl, shift, alt}` is a virtual-key code (or `input_state::pressed_key` side mouse button) with the exact
  modifiers. `chord_to_string` / `chord_from_string` use the `accelerator()` syntax (`"Ctrl+Shift+S"`, `"Alt+F4"`, `"Page Up"`,
  `"Mouse 4"`); `ui.chord_pressed(chord)` is what `accelerator()` uses. Plain keys are ignored while a text field has the keyboard,
  and nothing fires while a hotkey field waits for a key.
- **Multi-key chords:** a `key_sequence` is up to `key_sequence::max_steps` (3) chords pressed one after another ("Ctrl+K, Ctrl+S"),
  each within `key_sequence_timeout` (1.5 s) of the one before; a plain `key_chord` converts to a one-step sequence, so this is a
  drop-in everywhere a chord was accepted (`keybinds::action::chord` is a `key_sequence`). `sequence_to_string` / `sequence_from_string`
  join / split the steps with `", "`. `ui.sequence_pressed(seq)` is true on the frame the last step lands; a key that does not
  continue any sequence sharing the prefix so far leaves it pending for one more frame (so two sequences sharing a prefix, like
  "Ctrl+K, Ctrl+S" and "Ctrl+K, Ctrl+O", both get a fair look at the next key) before it is dropped. `ui.hotkey_sequence("Label", seq)`
  is the rebinding field: the first key commits immediately, pressing another within the timeout extends it (each extension commits
  too); a bare Esc as the very first key leaves it as it was, a bare Backspace / Delete as the very first key unbinds it (with a
  modifier, or once a step is already captured, they are just steps of the chord).
- **keybinds:** `bind` / `reset` / `reset_all`, `conflict(name)` (another action on the same chord), `find`, `actions()`. `keybind_editor`
  shows a reset button where a binding differs from its default and a mark where two actions collide; unbinding is Backspace / Delete.
- **config:** case-insensitive sections and keys, order kept, values are one line of text; typed getters take a fallback for a missing or
  malformed value. Keys before the first header are the section `""`. `from_string` merges into what is there and returns the number of
  lines it could not read; `load_file` / `save_file` take utf-8 paths. Unbound actions are stored as an empty value, so a saved
  "unbound" survives a load (an action missing from the file keeps its default).
- **Contexts:** `binds.add("format", "Ctrl+Shift+F", "format the selection", "editor")` makes an action that only works (and only shows in
  the palette) while `binds.set_context("editor", editor_has_focus)` is on; call it every frame with what is true right now. Actions of
  different contexts can share a chord without conflicting, and while a context action is on it takes its key from a global action
  with the same chord.
- **Command palette:** `strata::command_palette palette; if (auto cmd = palette.show(ui, binds); !cmd.empty()) run(cmd);` is a modal search
  box over every available action with its shortcut: Ctrl+Shift+P opens it (`palette.shortcut`, or `palette.open()`), typing filters
  by `fuzzy_score`, Up / Down / Enter or a click chooses, Esc closes. Call `show()` once per frame outside any window.
- **config_file (auto-save and hot reload, both optional):** `strata::config_file settings{"settings.ini"}; settings.load();` ties a config to a
  file. Nothing else happens until you switch it on: `set_auto_save(true)` writes the file a moment after the last change (and when
  the object goes away), `set_hot_reload(true)` reads it again when it changes on disk. Call `if (settings.update(dt)) apply(settings.data());`
  once per frame (true = the data was replaced from the file). Use its `set_*` setters (or `touch()` after changing `data()`) so
  auto-save notices. A file that changed on disk while there are unsaved changes is not read until they are written, so hot reload
  never throws your changes away.
- The sandbox's "key bindings and config file" window (`--scene config`, `--scene palette`) is the whole thing: rebind, contexts, palette,
  save / load, the auto-save and hot-reload switches, and a live preview of the file; F2 / F3 / F6 / F7 are its default actions.

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
- **Tabs:** `tab_bar` reports `changed`, `closed`, `moved_from` / `moved_to` and `add`; changing your own list is up to you (`apply_tab_events` does
  it). Tabs are told apart by their label. Any number of tabs: when they do not fit they scroll (mouse wheel over the bar) and an arrow at
  the right end opens a list of all of them. The plain `tab_bar(id, tabs, count, selected)` looks and behaves as before.
- **Popups:** `open_popup` / `toggle_popup` / `popup` (or `begin_popup` / `end_popup`) / `close_popup` / `popup_is_open`. The panel opens under the last widget
  (`last_item_rect()`) or at a position, its height follows the content, and it is drawn above all windows. One popup is open at a time
  (menus, dropdowns and this share the slot), so no popup inside a popup.
- **Status widgets:** `spinner` (an arc for work of unknown length), `badge` (a non-interactive pill; `kind_color(kind, theme)` gives the color),
  `chip` (a removable / toggleable tag; `chip_options` has `closable`, `selected`, `tint`, `icon`).
- `--scene tabs` shows all of it, with the options popup open.

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
- **Drag and drop:** the payload is a small copy of your data (an index, an id, a color) tagged with a type name; a target only takes the type it asks
  for. Dragging starts after a few pixels of movement, Esc cancels, and the release that ends a drag is not a click of the source. A
  target outlines itself while a matching payload is over it (`drop_flags::no_highlight` turns that off). `begin_drag_source` /
  `set_drag_payload` / `end_drag_source` are the unscoped form; `dragging()` and `drag_payload_type()` tell what is going on.
- **Pickers:** a field that opens a month calendar (arrows for months and years, today marked, a Today button) or grids of hours / minutes
  (and seconds) with - / + for the minutes in between. `datetime.hpp` has `date`, `time_of_day` (comparable), `days_in_month`,
  `weekday`, `add_days`, `add_months`, `parse_date` / `parse_time`, `to_string`, and `override_clock(...)` to fix `today()` / `now()` for
  tests and screenshots.
- `--scene dnd` shows both, with the calendar open.

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
- **list_clipper** submits only the rows that can be seen and reserves the space of the others, so the scrollbar and layout are those of the whole
  list (100 000 rows cost a handful). It works in a fixed-height window, a `child`, and a table with a `height` (there it uses the table's row
  height; `table_skip_rows(n)` skips rows by hand). Rows must have one height.
- **Table columns:** `table_next_column()` is false for a hidden column (the code keeps filling cells in the order it declared them),
  `table_headers_row` returns the clicked column by its declared index. `table_column_flags`: `default_hidden`, `no_hide`, `no_reorder`.
- **Tree tables:** `table_tree_node` / `table_tree_leaf` / `table_tree_pop` draw an arrow, indent by depth and keep their open state by label.
- **Auto-height windows:** a window that follows its content never grows past the bottom of the display: it scrolls instead.
- **Rows of mixed height** do not need a clipper at all: they are culled one by one (see *Color, trees, tables*).
  `ui.skip_item(h)` / `ui.skip_items(n, h)` reserve the space of a run the caller culled itself.
- **Scrolling** is now controllable from the code, for the innermost region being built (a `child`, otherwise the
  window): `scroll_y()`, `scroll_max_y()`, `set_scroll_y()`, `scroll_to_top()` / `scroll_to_bottom()`, and
  `ensure_item_visible()` / `scroll_to_item()` for the row that was just submitted -- which is how "reveal the
  selection" works. The new offset shows on the next frame, so it is called every frame the selection holds.
- **Sideways scrolling** is opt-in per child region:
  `ui.begin_child("pane", size, strata::child_flags::horizontal)`. Content wider than the region then scrolls
  instead of being cut off, with a bar along the bottom, the tilt wheel (`WM_MOUSEHWHEEL`, which `win32_platform`
  now forwards as `input_state::wheel_x`) and Shift + wheel. `scroll_x()`, `scroll_max_x()` and `set_scroll_x()`
  are the counterparts of the vertical three. Full-width widgets still size themselves to the *visible* width, so
  what overflows is whatever asked to be wide: an image, a long unwrapped line, a table given fixed column widths.
  Windows and tables have no horizontal scrolling of their own -- they lay out to the width they are given -- so
  put one inside a horizontal child when it needs to be wider than its pane.
- **Keyboard navigation** is opt-in per list: between `nav_begin()` and `nav_end()` (or `auto n = ui.navigation("id");`)
  Up / Down move a cursor over the rows, Home / End jump, PageUp / PageDown move by ten, Left closes a node or steps
  out to its parent, Right opens it or steps in, and Enter reports the row exactly like a click. Clicking a row moves
  the cursor to it; the row under the cursor reports `item_focused()` and draws a focus ring; the view follows it. The
  scope only takes the keys while no text field has them, which is what `nav_active()` says.
- `--scene lists` shows the tables, `--scene bigtree` the deep tree with a live `ui.stats()` panel.

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

- **`same_line_right(width)`** places the next item so it ends at the right edge of the content area -- which already
  accounts for the padding and for a scrollbar that is showing, so a right-aligned control does not move when either
  changes. **`push_right_gutter(w)` / `pop_right_gutter()`** (or the scoped `ui.right_gutter(w)`) take `w` off the
  width everything until the pop is laid out in, while `same_line_right` still reaches the real edge: the full-width
  item submitted first ends at the gutter instead of running under whatever is drawn in it, and its label is
  ellipsized there. `ui.label_clipped(pos, max_width, color, text)` is `text_ellipsis` for a custom item that paints
  its own row.
- **Overlapping items.** By default the item submitted *first* claims the press, which is wrong when a button is drawn
  on top of a row. `allow_item_overlap()` after an item lets a later overlapping one take the press instead (exactly,
  in the same frame) and drops its hover highlight while the pointer is over that later item (one frame late -- the
  item on top has not been submitted yet when the one below draws itself). `item_claimed()` says whether that
  happened. Nothing changes for code that does not call it.
- **The last item** can be asked about without submitting an invisible one over it: `item_rect()`, `item_hovered()`,
  `item_clicked(mouse_button)` (right and middle are reported on the press, left on the release, like the widget's own
  return value) and `item_double_clicked()`.
- **`draw().corner_brackets(rect, color)`** marks a rectangle by its four corners only -- the viewport outline an
  object picker draws over what is under the cursor, which reads on top of a busy scene without boxing it in.
- **`ui.stats()`** reports what the frame that just ended cost: items submitted and how many of those were culled,
  vertices, indices, draw calls, label measurements and how many the cache answered, animation-table occupancy, and
  the time in `begin_frame` / `end_frame`. It is what tells you whether a stall is strata or your own data walk.
- **Tooltip delay** is `style::tooltip_delay_s` (0.4 s by default, `style_var::tooltip_delay` to push it), so tooltips
  do not pop on every row a pointer crosses while a tree scrolls past.
- **Text fields** take `input_flags::clear_button` (a small x while the field has text) beside the existing
  `select_all_on_focus`.
- **Row accessories** are the ready-made version of all that, for the common case -- a small control at the right
  end of a row. Tell the row how much to reserve with `set_next_item_gutter(w)` before submitting it, then call
  `row_accessory_button` / `row_accessory_checkbox` / `row_accessory_toggle` after it. strata places them right to
  left inside the row, clips them to it, keeps them clear of the scrollbar, takes the press away from the row and
  elides the row's own label at the gutter -- no hit boxes, hover halos, clip intersections or gradient fades in the
  caller:

```cpp
ui.set_next_item_gutter(56.0f);
if (ui.selectable(object.name, id, selected)) { select(); }
if (ui.item_truncated()) { ui.tooltip(object.name); }          // it was cut: show the whole thing
if (ui.row_accessory_button(icon_font, icons::trash)) { destroy(); }
ui.row_accessory_checkbox("vis", object.visible);
```
- **Row labels are elided** to the room the row actually has (its width, minus the indent, minus the gutter) instead
  of running out from under whatever is drawn on top. `item_truncated()` says whether that happened, for a row or
  for `text_ellipsis()`, so a caller does not have to re-measure to decide whether a tooltip is worth showing.
- **Plain text is an item.** `text()`, `text_dim()`, `textf()` and `text_colored()` register their rectangle, so
  `item_hovered()`, `item_rect()`, `tooltip()` and `context_menu()` after them are about the text and not about
  whatever widget came before it. They still take no press, so nothing about clicking changes.
- `--scene rows` shows the overlapping header rows, the gutter and the keyboard list; `--scene app` shows the
  accessories, the selection, the disabled buttons, the filtered combo and the runtime scale together.

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

- **Raw keys.** `key_pressed(vk, ctrl, shift, alt)` is the edge, `key_down(vk)` the level, over windows virtual-key
  codes -- so Delete, F2, F5 and the rest need no `GetAsyncKeyState` and no held/not-held bit of your own. Both stay
  quiet while a text field or a hotkey field has the keyboard. `key_down` needs `input_state::keys_held`, which
  `win32_platform` fills; a host that cannot always answers false, while `key_pressed` works either way.
  `accelerator("Del")` has always worked outside a menu too, and follows the same focus rule.
- **Raw mouse buttons:** `mouse_down(b)`, `mouse_clicked(b)`, `mouse_released(b)`, 0 left / 1 right / 2 middle. Ask
  `item_clicked(mouse_button)` instead when you mean "on the thing I just submitted".
- **Which window has the keyboard.** `window_focused()` is about the window being submitted right now and
  `is_window_focused(title)` about any of them, so Delete in one panel does not act on another's selection. It
  follows the last window that was pressed in, docked panels included (which never restack, so "topmost" could not
  answer this).
- **Text focus** is idempotent: `request_text_focus(label)` does nothing when that field already has the keyboard,
  so it can be called every frame with no "did I ask already" flag and without taking the caret back on every
  keystroke. `focused_field()` and `field_focused(label)` say which field is live -- no more inferring it from
  hover plus `want_text_input()`.
- **Disabled items:** `begin_disabled(cond)` / `end_disabled()`, or the scoped `ui.disabled_if(cond)`. Everything
  inside is faded and inert -- no hover highlight, no press, no keyboard -- but still reports `item_hovered()`, so
  the tooltip that explains why it is disabled works. They nest, and an enabled scope inside a disabled one stays
  disabled.
- **Selections:** `selection_state` holds the indices, `ui.selection_click(sel, index)` applies the rules a list is
  expected to have -- plain click selects one, Ctrl toggles, Shift takes the range from the anchor, and the anchor
  stays put so dragging the range keeps working. `clamp_to(count)` drops what a shrinking list left behind.
- **Confirmations** own their state: `ask_confirm(id, message, user_data)` opens one and remembers what it was about
  (`confirm_data()`), `confirm(id, buttons, options)` draws it and returns the button (1..n), -1 for Esc, 0 while
  nothing is being asked. `confirm_options::remember` points at a "don't ask again" flag: the dialog renders the
  checkbox, stores the answer there, and while the flag is set it never opens and answers `remembered` straight
  away -- so the caller needs no special case for it, and no member-variable pair per dialog. `danger` draws one
  button in the warning colour. The id is global, like a modal's title.
- **Clipboard:** `ui.copy_text(string_view)` and `ui.paste_text(std::string&)` go through the hooks the text fields
  already use, instead of `GlobalAlloc` / `OpenClipboard` in the app.
- **Geometry you should not have to re-derive:** `context::scrollbar_width()`, `content_rect()` (the visible
  rectangle of the innermost scrolling region, in logical screen coordinates) and `item_arrow_hit()` (the press on
  the last tree row landed on its arrow, not its label).
- **Long dropdowns:** `combo_filtered()` opens with the keyboard in a search box, narrows as you type
  (case-insensitive substring), submits only the rows in view, and takes Up / Down / PageUp / PageDown / Enter /
  Esc. A few hundred entries are what it is for.
- **Tabs** can keep their identity apart from their caption: `tab_desc{label, icon, id}`. A tab whose text gains a
  dirty dot, a pin marker or a count keeps its place, its selection and its drag state only if it keeps its `id`.
  Middle-clicking a closable tab closes it.

## Code editor, passwords and input masks

```cpp
ui.input_spans(spans);                                          // colors from your highlighter, as for any text field
ui.input_code("##code", source, {0, 260});                      // line numbers, current line, brackets, auto indent, find / replace
ui.code_goto_line("##code", 120);   ui.code_find("##code", "TODO", /*replace bar*/ false);

ui.input_text("password", pw, {}, input_flags::password | input_flags::reveal);   // an eye button shows the text
ui.input_masked("phone", phone, "(###) ###-####");                 // "(555) 123-4567" is built as you type
ui.input_masked("plate", plate, "UU-###");                         // "AB-123": letters are made upper case
```
- **input_code** is a multi-line field without wrapping. `code_flags` (all on in `code_default`): `line_numbers`, `highlight_line`, `bracket_match`
  (the bracket next to the caret and its partner are boxed), `auto_indent` (Enter keeps the indentation and adds a level after `{ ( [`, a typed `}` steps
  back), `find_replace` (Ctrl+F / Ctrl+H open a bar above the text that marks every match: Enter / Shift+Enter or the arrows step through them,
  Replace and All change the text). Tab goes to the next tab stop, and indents every line of a selection (Shift+Tab unindents).
  `code_goto_line` and `code_find` are called in the id scope of the field.
- **Masks:** `#` a digit, `A` a letter, `U` / `L` a letter turned to upper / lower case, `X` a letter or digit, `?` any character, `\` makes the next
  character literal, everything else is literal. `value` always holds the formatted text; typing, deleting, pasting and moving the caret keep its
  shape (the undo history is off for masked fields).
- `--scene editor` shows the editor (with the find and replace bar open), passwords and masks.

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
  The text of every line lives in one arena rather than a `std::string` per line -- a program logs at whatever rate
  it produces events, and a string each meant an allocation each. `line.text()` is a `std::string_view` into that
  arena, valid until the next `add()` / `clear()`; the arena is compacted as lines fall out of the ring and settles
  at about the size of the lines the ring holds, after which logging allocates nothing.

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
  key; `win32_platform` fills it. `ui.hotkey_chord("Label", chord)` does the same for a key with modifiers (see below).
  `ui.hotkey_sequence("Label", seq)` captures a short sequence of chords pressed one after another (a `key_sequence`, see
  "Key bindings and config files"): each step commits right away, and the field keeps listening for `key_sequence_timeout`
  longer in case another key extends it into a longer chord.
- **Docking animation:** `ui.set_dock_animation(true)` makes panes slide to their new place when a window docks, undocks or a pane closes
  (a new pane grows out of the edge it was dropped at); dragging a splitter always follows the pointer. On by default; `set_dock_animation(false)` snaps panes into place.
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

**Links.** `<a=href>text</a>` draws `text` underlined in the accent color (a `<c=>` inside the link keeps its own
color), shows the hand cursor over it, and reports the href for the frame it was clicked in:

```cpp
ui.rich_text("see <a=https://example.com>the manual</a>, or <a=cmd:reset>reset</a>");
if (auto href = ui.rich_link_clicked(); !href.empty()) { open(href); }   // an event, not a state
ui.rich_link_hovered();   // the href under the pointer right now, for a status bar
```
The href is whatever you put there -- a url, a file, a command name; strata does not open anything itself, because
what a link should do is the application's business. Links in *widget captions* (under `rich_labels()`) are drawn
but not clickable: the widget owns the click.
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
content scrolls (wheel or scrollbar; tables and combo lists inside get the wheel first). A "follow the content" window
never grows past the bottom of the display: when the content is taller it stops there and scrolls, so nothing is
unreachable. Windows can also be created with a fixed height and no resizing. The plain `ui.window("title", pos, width)`
form is unchanged.

**Pointer shape:** `ui.cursor()` reports what the pointer should look like: `arrow`, `text` (I-beam), `hand` (over a
rich-text link), `not_allowed` (over a disabled item, so "this does nothing" is distinguishable from "this is
broken"), and the resize arrows `resize_ew`, `resize_ns`, `resize_nwse`, `resize_nesw`. With `win32_platform`:
`platform.set_cursor(ui.cursor())` after `end_frame()` and, in your window procedure,
`case WM_SETCURSOR: if (LOWORD(lparam) == HTCLIENT && platform.apply_cursor()) return TRUE;`.

## Idling

A UI nobody is touching draws the same thing every frame. `end_frame` hashes the vertices, indices, commands and
shapes, so it can say whether this frame differs from the last one at all:

```cpp
ui.end_frame();
if (ui.can_idle()) {
    // nothing to draw that is not already on the screen
} else {
    renderer.render(ui.render_data());
    present();
}
```

- `frame_unchanged()` -- the geometry is byte-identical to the previous frame.
- `animations_settling()` -- something will look different next frame even if nobody touches anything: an animation
  still short of its target, a toast counting down, the pause before a tooltip appears. Without this the hash alone
  would idle a fading animation one frame short and freeze a toast on the screen forever.
- `can_idle()` is both: unchanged and not settling. A toast that stays until it is closed does not count once it has
  slid in (it used to keep the UI awake forever).
- `next_wake_seconds()` -- for a host that sleeps instead of spinning: how long it may wait for input before the next
  frame, because nothing time-driven changes the UI before then (the caret blink, a tooltip's delay). `0` means run the
  next frame now, `no_deadline` means wait for input. Pass the real elapsed time -- sleep included -- as
  `input_state::delta_time`: timers take all of it, animations at most 0.1 s of it.

```cpp
ui.end_frame();
if (!ui.frame_unchanged()) { renderer.render(ui.render_data()); present(); }
const f64 wait = ui.next_wake_seconds();
MsgWaitForMultipleObjects(0, nullptr, FALSE, wait == strata::no_deadline ? INFINITE : DWORD(wait * 1000), QS_ALLINPUT);
```
- `invalidate()` forces the next frame to count as changed (a texture was replaced, the host rebuilt its back
  buffers, the theme was edited between frames).

**An overlay cannot skip drawing** -- the game cleared the target and redrew its own frame, so the UI has to go back
on top. What it can skip is the upload, and the renderers do that themselves: `render_data()` carries a
`content_hash`, each renderer remembers what its buffers hold (per frame slot on D3D12, where frames are in flight)
and skips the `Map` + `memcpy` of the vertex, index and shape buffers when they already hold this frame. For a
static panel that is most of what rendering costs on the CPU side.

`d3d11_renderer::set_state_restore(false)` turns off the save-and-restore of the eighteen pipeline stages `render()`
touches -- about forty driver calls per frame spent putting back state nobody will read. An in-game overlay needs it
on (the default); an application that owns its device does not, and then has to set what it needs before whatever it
draws next.

In the sandbox, `--idle` does the skip, sleeps until the next deadline and reports it: `--scene icons --idle --frames 300`
idles 299 of 300 frames. `strata::app` (below) idles this way by itself.
The busier scenes idle none of them, because a progress bar, a spinner or an fps readout really does change the
geometry every frame.

## Diagnostics

```cpp
ui.debug_metrics_window(show_metrics);    // frame cost, geometry, culling, idle state, and what went wrong
ui.debug_draw_list_window(show_commands); // the live commands: clip, index count, base vertex, texture
```

`ui.stats()` is the same numbers as a struct: items submitted and how many the clip rectangle culled, vertices /
indices / draw calls, the measurement cache hit rate, animation table occupancy, and `begin_frame_ms` /
`end_frame_ms`. Four of its fields are things that used to fail silently and now do not:

- `draw_overflow` -- a vertex / index / command / shape reservation ran out and geometry was dropped. Raise
  `draw_list_limits`.
- `clip_overflows` / `alpha_overflows` -- nesting deeper than the clip (32) or alpha (16) stack holds. The push is
  counted instead of stored and the matching pop skips it, so the levels that did fit stay correct; the innermost
  ones are simply not clipped or faded. (Before, the push was applied without being saved, and every later pop
  restored the wrong level -- the rest of the frame was clipped one level too shallow.)
- `id_collisions` -- **debug builds only.** Two widgets whose labels hash to the same id in the same scope share
  their hover, press and focus state: the second one steals the first one's click, and nothing on screen looks
  wrong. `id_collision()` and `id_collision_label()` name the first one of the frame, which is almost always a
  repeated label. Give one a `"label##suffix"`, or wrap them in `push_id()`. Release builds do not check.

## Footprint

### Binary size

What strata adds to a program, measured by linking a probe that uses it (window, text, button, checkbox, slider,
text field, rich text, a table) against an empty program with the same flags: x64 release, `/O2 /GL /LTCG /MT`,
`/OPT:REF /OPT:ICF`, no exceptions, no RTTI. Subtracting the empty program removes the static CRT baseline
(105 KiB) but *not* the parts of the CRT that strata itself drags in, which is the honest way round -- those bytes
are in your binary because strata is.

| what | adds | running total |
|---|---:|---:|
| empty program, static CRT | -- | 106 KiB |
| \+ strata core (context, widgets, draw list, fonts, bidi, themes) | 583 KiB | 689 KiB |
| \+ `d3d11_renderer` | 37 KiB | 725 KiB |
| \+ `d3d12_renderer` | 36 KiB | 761 KiB |
| \+ the two diagnostic windows | 17 KiB | 778 KiB |

Roughly 420 KiB of that is strata's own code; the rest is the CRT it pulls in, most of it the `charconv` /
`std::format` float tables (~110 KiB) that any use of `textf` or a number field reaches. By translation unit, the
largest are `context.cpp` (93 KiB), `context_text.cpp` (39 KiB), `font.cpp` (26 KiB), `context_dock.cpp` (25 KiB),
`context_data.cpp` (tables, 23 KiB) and `bidi.cpp` (19 KiB). `/OPT:REF` drops what a program does not call, so a UI
that never opens a code editor, a plot or a date picker does not pay for them -- the diagnostic windows in the
table above are only 17 KiB *because they are called*.

The shipped artifacts, for comparison: `strata_overlay_demo.dll` 820 KiB (strata + both renderers + the overlay
hook + a demo UI), `strata_sandbox.exe` 1.9 MiB (all of it, plus a large demo application and the self-tests).
`strata.lib` itself is ~111 MiB on disk, which is `/GL` intermediate code, not machine code -- nothing of that size
reaches a binary.

### Memory

| object | size |
|---|---:|
| `context` | 26,528 bytes |
| `draw_list` | 1,080 bytes |
| `font_atlas` | 72 bytes (plus the pixels below) |
| `input_state` | 456 bytes |
| `style` | 116 bytes |
| `vertex` | 16 bytes |
| index | 2 bytes |
| `draw_cmd` | 36 bytes |
| `shape_record` | 80 bytes |

**Address space** the draw list reserves up front, committed 64 KiB at a time as it is used and never moved (the
defaults in `draw_list_limits`; all four are configurable):

| array | capacity | reserved |
|---|---:|---:|
| vertices | 1,048,576 | 16 MiB |
| indices | 4,194,304 | 8 MiB |
| commands | 16,384 | 576 KiB |
| shapes | 131,072 | 10 MiB |

About 34 MiB of reservation, of which a real frame touches a fraction of one percent. Reservation is not memory: it
costs address space (of 128 TiB) and no pages until written.

**Actually committed.** A context with one 14 px font, after 240 frames of two windows holding 60 rows and a 40-row
four-column table: **816 KiB** of private working set, of which 256 KiB is the CPU-side atlas bitmap
(`release_font_pixels()` gives that back once every renderer has its copy). The atlas is 8 bits per pixel and sized
to the glyphs you bake: 512x512 for the default Latin / Greek / Cyrillic set with one font, 2048x1024 (2 MiB) for
the sandbox's four fonts with icons, up to `max_atlas_size` (4096, so 16 MiB) for CJK.

**Per frame**, that same UI produced 2,452 vertices, 3,702 indices, 134 commands and 21 shapes -- 38 KiB + 7 KiB +
2 KiB to upload. The sandbox's busiest scene runs about 6,600 vertices / 10,500 indices / 31 draw calls, roughly
124 KiB per frame, and the UI costs ~0.13 ms of CPU to build. Steady state allocates nothing: the geometry arrays,
the animation table, the measurement cache, the rich-text runs, the bidi scratch and the log's text arena are all
grown once and reused.

### Traces

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
- **Icon fonts and which Windows ships what.** `strata/icons.hpp` names the code points of the two Microsoft icon
  fonts, which share them: **Segoe MDL2 Assets** (`segmdl2.ttf`, Windows 10, and still present on Windows 11) and
  **Segoe Fluent Icons** (`SegoeIcons.ttf`, Windows 11 only). Bake `glyph_ranges::private_use` for the icon font and
  select it with `ui.with_font(icon_font)` or the `icon_*` widgets. A code point the font does not have draws the
  fallback glyph silently, so **`--scene icons`** draws every `icons::` constant with its name and marks in red the
  ones the loaded font is missing -- run it once against the font you ship with. `--scene icons --icon-page E700`
  shows a raw page of 256 code points with their hex values, for picking a new one.
  One caveat: `icons::eye_off` (U+ED1A, "Hide") is in Segoe Fluent Icons and in current Segoe MDL2 Assets but not in
  the `segmdl2.ttf` that shipped with Windows 10 up to at least build 19045; use `icons::view` for the hidden state
  if you have to support those. There is no "clear filter" glyph in either font -- draw `filter` and `cancel`
  together. Round two added, all checked the same way: `clear` E894, `select_all` E8B3, `rename` E8AC (what F2
  does), `document` E8A5, `layers` F156, `star_filled` E735, and `expand` E740 / `collapse` E73F for making a view
  bigger or smaller (`expand_all` / `collapse_all` are the tree ones). Note `import` is E8B5 and `export` EDE1 --
  E896 / E898 are Segoe's Download and Upload arrows, which are a different thing.

## UI scale at runtime

```cpp
strata::overlay::options opt;
opt.ui_scale      = 0.0f;   // 0 = the monitor's dpi scale, which is only where it starts
opt.scale_hotkeys = true;   // Ctrl + Plus / Minus step it by 10 %, Ctrl + 0 goes back
...
strata::overlay::set_ui_scale_percent(125);          // from anywhere, at any time
ui.textf("ui scale {} %", ui.scale_percent());
```

The dpi scale is the right *default* and a poor *setting*: an overlay is often wanted a little smaller or larger
than the desktop. `context::set_scale` has always been able to change it, but it rebuilds the font atlas, and the
host then has to hand the new atlas to its renderer (`renderer.update_atlas(ui.font())`, then
`ui.release_font_pixels()`) -- a step that is easy to miss, and text draws from a stale texture when it is missed.
The overlay now does that itself: `set_ui_scale()` may be called from any thread, and the change is applied at the
start of the next frame, on the render thread, atlas re-upload included. Everything the ui draws -- text, widgets,
padding, window sizes -- scales together; the game's own window is not touched.

A rebuild costs tens of milliseconds per baked font, so drive it from a stepper or apply a slider when it is let go,
not on every tick. `ui.scale_percent()` / `ui.set_scale_percent(n)` are the same thing in the units a setting shows.

## In-game overlay (direct3d 11 and 12)

`overlay/` turns strata into an in-game overlay: a dll that is loaded into a direct3d 11 program (a game, for modding tools and
inspectors) and draws a strata ui over every frame, with the game's keyboard and mouse taken while it is open.

```cpp
strata::overlay::options opt;
opt.ui = [](strata::context& ui) { if (auto w = ui.window("my tool", {40, 40}, {360, 0})) { ui.text("hello"); } };
strata::overlay::install(opt);       // from a thread of your own, not from DllMain; F1 (opt.toggle_key) shows / hides it
```
- **The hook:** a dummy swap chain gives the address of `IDXGISwapChain`'s vtable (all swap chains of that implementation share it);
  `Present`, `Present1` and `ResizeBuffers` are replaced in it, no code is patched, and `uninstall()` puts the slots back. The dummy is
  made with the api the game has already loaded -- direct3d 11, or direct3d 12 when only `d3d12.dll` is in the process -- so the
  overlay never loads the other api (and its driver) into the game; `strata_overlay_host` checks that. When the game replaces
  its swap chain (a resolution or display-mode change), the overlay follows to the new one and keeps its UI; it holds no view
  of the game's buffers between frames, so the game can always make the new one. The first swap
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
- **Direct3D 12:** the same swap chain class is hooked. When the game's swap chain first presents, a queue made on the game's own
  device gives the vtable of `ID3D12CommandQueue`, where `ExecuteCommandLists` is replaced: the direct queue that was seen executing
  command lists is the game's. Per frame the overlay records its
  own command list (transition of the current back buffer to render target, `d3d12_renderer::render`, transition back), runs it on that
  queue right before Present and signals a fence, so a frame slot's allocator and upload buffers are reused only after the gpu is done
  (waits are per back buffer index, normally free). `ResizeBuffers` waits for the overlay's work and rebuilds the render target views. The
  first frames after injecting wait until a queue has been seen. `strata_overlay_host --d3d12` tests it.
- **Limits:** Vulkan / OpenGL games need a hook of their own (the ui and the renderers are the same). A d3d12 game with several direct queues
  is served by the one that ran last. A game that recenters or locks the cursor every frame for mouse-look fights for it (in Unity set
  `Cursor.lockState = None` while the overlay is open). The overlay does not need the game's cooperation but the game's own anti-cheat
  may not like an injected dll: do not use it in online games.

## Hosting, state, HDR and diagnostics

**`strata::app`** (target `strata::app`, `strata/app.hpp`) is a ready-made host: a per-monitor-dpi window, a flip-model
direct3d 11 swap chain with a frame latency object, DPI changes, resizing, sleeping while nothing changes, a lost device
(`on_device_reset` is where textures are made again), a close request that can be refused (`on_close_request`),
following the system's dark / light mode and accent (`follow_system_theme`), and the user's arrangement kept in a file
(`state_file`). strata itself still never creates a window or a device, so it can live inside someone else's (an overlay).

```cpp
auto app = strata::app::create({.title = "tool", .follow_system_theme = true, .state_file = "tool.ini"});
return app->run([&](strata::app&, strata::context& ui) { if (auto w = ui.window("hello", {40, 40}, 300.0f)) { ui.text("hi"); } });
```

- **State:** `ui.save_state(config)` / `ui.load_state(config)` put the dock layout, window places / sizes / collapsed,
  table columns, open tree nodes and scroll offsets into one config section; loading works before the UI is first shown.
- **Ids:** 64 bit. `"Downloads (3)###dl"` shows the text before `###` and keys the item by what follows, so a title that
  changes keeps its window. Window slots of windows no longer shown are given back when all 32 are in use.
- **Edits:** `item_activated()`, `item_deactivated()`, `item_edited()`, `item_deactivated_after_edit()` after any value
  widget -- one undo step per drag or text entry instead of one per frame.
- **Input:** `input_state::presses` queues every key press between two frames with the modifiers it had (they are handled
  one per frame, none is lost); typed text holds 1 KiB per frame (a whole IME sentence); key events carry `alt`; caret
  blink and double-click time come from the system settings (`win32_platform` fills them).
- **Smooth scrolling** is on (`set_scroll_smoothing(false)` for the jump).
- **Themes** have `success`, `warning`, `error` and a six-colour chart palette `series`, in theme files too;
  `win32_platform::appearance()` + `themes::for_appearance()` follow the system; `style::text_contrast` (0 = off, per
  theme) thickens light text on dark backgrounds.
- **HDR / srgb targets:** `renderer.set_output({output_space::scrgb | hdr10 | srgb_view, paper_white_nits})` encodes the
  UI for an FP16 scRGB, a 10-bit HDR10 or a `*_SRGB` target. The overlay works it out from the game's swap chain
  (`options::output` overrides it).
- **Lost device:** `renderer.device_lost()`; recover with a new device, `ui.rebuild_font_atlas()`, `renderer.create()`,
  new textures.
- **Worker threads** log through `strata::log_queue` (`add` anywhere, `drain_into(log)` on the UI thread).
- **Diagnostics:** `context_config::diagnostics` (or `set_diagnostics`) receives each distinct problem once -- a fixed
  table that ran out, a duplicate id (debug builds), a draw list overflow; `frame_stats::limits_hit` counts them.
- **D3D12 textures** are staged and copied at the start of the next `render()` (no CPU wait), and `destroy_texture()` may
  be called any time.
- **Tests:** `strata_render_test` (renderers on a real device: hostile pipeline state, output encodings, text contrast,
  device recovery), `strata_app_test`, overlay runs on FP16 swap chains and across a replaced swap chain. The `x64-asan`
  preset builds everything with AddressSanitizer plus `strata_fuzz` (libFuzzer over config, theme, dock layout, saved
  state, rich text, key chords and text editing; ctest runs it briefly from `tests/fuzz/corpus`).

## TODO

The known limitations, as a work list.

**Text**
- [ ] OpenType shaping for Indic / Southeast Asian scripts (Devanagari, Thai, Tamil, Khmer ...), color emoji, explicit bidi embeddings,
      right-aligned right-to-left paragraphs

**Input and windows**
- [ ] Keyboard navigation of widgets and menus (Tab / arrows; today only text fields, combos and hotkeys take the keyboard)
- [ ] Multi-viewport (windows outside the application window)
- [ ] Horizontal scrolling for windows and tables themselves (today only a `child_flags::horizontal` child scrolls
      sideways, which is enough to put a wide table in but does not give the table its own bar)

## Ideas

Things that would fit, not promised. Not on the TODO list.

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

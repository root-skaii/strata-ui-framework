# Architecture, Styling, Fonts & Diagnostics

Back to [README](../README.md).

## Contents
- [Design principles](#design-principles)
- [Custom drawing, gradients & acrylic blur](#custom-drawing-gradients--acrylic-blur)
- [DPI, SDF fonts, Unicode & SVG icons](#dpi-sdf-fonts-unicode--svg-icons)
- [Themes & live hot-reload](#themes--live-hot-reload)
- [Key bindings, command palette & config](#key-bindings-command-palette--config)
- [Idling & diagnostics](#idling--diagnostics)
- [Accessibility & footprint](#accessibility--footprint)

---

## Design principles

- **Zero heap traffic in steady state:** vertex, index, command, and shape arrays use `vmem_array` (reserved virtual address space committed in 64 KiB pages, never relocated, cleared per frame). Windows, tables, children, and cards are sized once at `context::create()` via `context_config::capacity`.
- **Analytic SDF shapes:** one quad per rounded panel evaluates per-corner radii, linear/radial gradient, inner border, and soft drop shadow in the fragment shader; plain axis-aligned rectangles take a 4-vertex fast path.
- **16-byte vertex layout:** `float2 pos`, `unorm16x2 uv` (bit 15 marks shape quads), `rgba8 col`. Draws merge automatically per clip rectangle.
- **Logical coordinates:** all layout runs in logical pixels; `draw_list` scales on emission so renderers only see physical pixels.
- **Lean C++ footprint:** compiled without exceptions or RTTI, static library only, `<windows.h>` kept out of public headers, and sensitive text inputs wiped via `secure_string` on focus loss and destruction.

## Custom drawing, gradients & acrylic blur

```cpp
auto item = ui.custom_item("meter", {0, 46});
float glow = ui.animate("meter", item.hovered ? 1.0f : 0.0f);

auto& dl = ui.draw();
dl.rect_gradient_angle(item.bounds, col_a, col_b, 45.0f, 10.0f);
dl.polyline(points, col, 2.0f, false);
dl.bezier_cubic(p0, p1, p2, p3, col, 3.0f);
dl.arc(center, radius, a0, a1, col, 6.0f);
dl.backdrop(rect, 18.0f, tint, strata::radii(10.0f)); // frosted glass panel
```

- `window_flags::acrylic` / `child_flags::acrylic` frost the scene behind translucent panels (works automatically on D3D11/OpenGL/software, and via `d3d12_target` / `vulkan_target::image` on D3D12/Vulkan).
- Scoped overrides (`ui.style_overrides({...})`) or `push_color` / `push_var` tweak colors and metrics for a subtree.

## DPI, SDF fonts, Unicode & SVG icons

```cpp
strata::context_config cfg;
cfg.font.face         = "Segoe UI";   // or cfg.font.file = "path.ttf", or cfg.font.data = embedded_bytes
cfg.font.pixel_height = 15.0f;
cfg.font.sdf          = true;         // optional single-channel SDF atlas for sharp zoomed text
cfg.font.sdf_spread   = 4;
cfg.font.ranges       = strata::glyph_ranges::default_set;
auto ui = strata::context::create(cfg).value();
```

- **DPI scaling:** `ui.set_scale(platform.dpi_scale())` rebuilds the atlas; call `renderer.update_atlas(ui.font())` and `ui.release_font_pixels()`.
- **SDF glyph baking:** `font_config::sdf = true` bakes an exact 2D Euclidean Distance Transform into the existing R8 atlas (`0.5` = glyph edge), rendered via screen-space derivatives (`fwidth`) on GPU backends and smoothstep on software.
- **On-demand dynamic glyphs:** `context_config::dynamic_glyphs = true` bakes unseen code points lazily and reports dirty sub-rectangles (`sync_font_atlas`).
- **Unicode & RTL:** full UTF-8, selectable glyph ranges (Latin, Greek, Cyrillic, CJK, Hangul, Hebrew, Arabic, Emoji), fallback faces, automatic bidi reordering and Arabic shaping, plus `ui_strings` for UI localization.
- **Cross-platform SVG vector icons (`strata/svg.hpp`):** parses and anti-aliased-rasterizes SVG `d="..."` paths at any DPI (`svg_rasterize_alpha`, `svg_rasterize_rgba`, `draw_svg_path`) and ships 22 built-in 24×24 icons (`strata::svg_icons::*`) with no OS icon-font dependency.

## Themes & live hot-reload

```cpp
ui.theme() = strata::themes::nord(); // 12 built-in: midnight, light, ocean, rose, dracula, nord,
                                     // solarized_dark, solarized_light, high_contrast, forest, amber, glass
strata::themes::save_file("my.theme", ui.theme(), "custom");

// Hot-reload theme file changes in the frame loop (polling mtime, zero background threads)
strata::themes::theme_watcher watcher{"my.theme", 0.25f};
if (watcher.poll(ui.theme(), dt)) { ui.invalidate(); }
```

- Theme files use `key = value` text with `#rgb` / `#rgba` / `#rrggbb` / `#rrggbbaa` colors and optional `base = <name>`.

## Key bindings, command palette & config

```cpp
strata::keybinds binds;
binds.add("save", "Ctrl+S", "Write the document");
binds.add("goto_symbol", "Ctrl+K, Ctrl+O"); // multi-key chord sequence (up to 3 chords)

if (binds.pressed(ui, "save")) { save(); }
strata::keybind_editor(ui, binds);

strata::config cfg;
binds.store(cfg);
strata::themes::to_config(cfg, ui.theme());
ui.save_state(cfg);  // dock layout, window rects, table columns, tree nodes, scroll offsets
cfg.save_file("settings.ini");
```

- `strata::command_palette` provides a fuzzy-search modal over registered actions; `config_file` supports auto-save and hot-reload.

## Idling & diagnostics

```cpp
ui.end_frame();
if (!ui.can_idle()) {
    renderer.render(ui.render_data());
    present();
}
// sleep up to ui.next_wake_seconds() while idle
```

- Built-in debug tools: `ui.debug_metrics_window(open)`, `ui.debug_draw_list_window(open)`, and `ui.debug_inspector_window(open)` (live widget picker, ID/override tree, and WCAG contrast checker).
- `ui.stats()` exposes per-frame vertices, indices, draw calls, culled items, cache hits, and limit/overflow counters. Unbalanced `begin_*`/`end_*` calls are reported via `diagnostics_hook` and repaired automatically at `end_frame()`.

## Accessibility & footprint

- **System appearance & motion:** `themes::for_appearance(dark, high_contrast, accent, reduced_motion)` adapts to OS dark/light mode, high-contrast mode, accent color, and `style::reduce_motion` (disables hover, toggle, scroll, and dock animations). `themes::check_contrast(theme)` flags pairs below WCAG 4.5:1 / 3:1.
- **Keyboard focus:** `Tab` / `Shift+Tab` cycles interactive widgets in the focused window (`Space` / `Enter` activates, `Esc` clears).
- **Binary size:** statically linking a full UI probe adds ~580 KiB in release (~35–50 KiB per GPU backend). Typical UIs use well under 1 MiB committed memory and ~0.1 ms CPU per frame.

# Changelog

All notable changes to `strata` are documented here.

## [0.1.0]

Initial public preview release.

### Core & Widgets
- Zero-allocation steady-state immediate-mode context backed by reserved virtual memory (`vmem_array`) and frame hashing (`can_idle()` / `next_wake_seconds()`).
- Analytic SDF shape shader with per-corner radii, linear/radial gradients, soft shadows, antialiased vector paths, and live acrylic backdrop blur (`themes::glass()`).
- Portable TrueType rasterizer with optional SDF glyph baking (`font_config::sdf`), UTF-8/RTL/IME input, and built-in SVG path icons (`strata/svg.hpp`).
- Pro-tool widgets: docking and multi-viewport tear-off OS windows, syntax-highlighted code editor (`Ctrl+/`, find/replace), node graph editor, multi-series charts and 2D heatmaps, timeline sequencer, Bezier curve editor, property inspector grid, rich text, command palette, and 100k-row virtualized tables (`list_clipper`).

### Backends & Platforms
- **Tested GPU renderers:** Direct3D 11, Direct3D 12, OpenGL 3.3+ Core, Vulkan 1.0+, and WebGPU (Dawn / Emscripten WASM).
- **Compile-tested GPU renderer:** Metal (`metal_renderer`, verified in macOS CI).
- **Headless renderers:** CPU software rasterizer (`software_renderer` with partial damage-rect redraw) and `null_renderer` validator.
- **Platform adapters:** Win32 (`win32_platform`), header-only GLFW (`glfw_platform`) and SDL3 (`sdl3_platform`), plus ready-made `strata::app` host.

### Overlays
- **In-game overlays (`strata::overlay`):** DXGI vtable hook (D3D11/12), OpenGL IAT hook (`SwapBuffers`), official Vulkan explicit layer (`VK_LAYER_strata_overlay`), runtime DPI/percentage scaling, HDR/scRGB detection, and safe `done` / `busy` / `pinned` teardown.
- **Experimental (off by default, not hardware-tested):** OpenXR (`STRATA_BUILD_OPENXR`) and OpenVR (`STRATA_BUILD_OPENVR`).

### Packaging, Examples & Verification
- Lowered CMake minimum requirement to `3.28` (`FetchContent`, `find_package(strata CONFIG)`, and `packaging/vcpkg/` overlay port).
- Standalone starter templates in `examples/minimal_app/` and `examples/minimal_overlay/`.
- Shared desktop + WebGPU/WASM modular showcase (`showcase/` + `web/`), headless benchmark (`strata_bench`), fuzz targets, and golden screenshot suite.

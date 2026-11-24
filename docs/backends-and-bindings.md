# Backends, Platforms, Bindings & Sandbox

Back to [README](../README.md).

## Contents
- [Graphics backends](#graphics-backends)
- [Platform adapters & file drop](#platform-adapters--file-drop)
- [Ready-made host (`strata::app`)](#ready-made-host-strataapp)
- [Headless rendering & replay](#headless-rendering--replay)
- [C interface & Rust bindings](#c-interface--rust-bindings)
- [Packaging (`FetchContent` & vcpkg)](#packaging-fetchcontent--vcpkg)
- [Sandbox CLI reference](#sandbox-cli-reference)

---

## Graphics backends

Every renderer satisfies the `strata::renderer_backend` concept (`strata/backend/backend.hpp`) with zero virtual calls in the frame loop. Unused graphics API DLLs are delay-loaded or resolved dynamically on demand.

| Backend | Header | Class | Render Signature | Backdrop Blur |
|---|---|---|---|---|
| Direct3D 11 | `strata/backend/d3d11.hpp` | `d3d11_renderer` | `render(data)` | Automatic (bound RTV) |
| Direct3D 12 | `strata/backend/d3d12.hpp` | `d3d12_renderer` | `render(data, cmd_list, slot, target)` | Via `d3d12_target` |
| OpenGL 3.3+ | `strata/backend/opengl.hpp` | `gl_renderer` | `render(data)` | Automatic (bound FBO) |
| Vulkan 1.0+ | `strata/backend/vulkan.hpp` | `vulkan_renderer` | `render(data, cmd_buf, slot, target)` | Via `vulkan_target::image` |
| WebGPU | `strata/backend/webgpu.hpp` | `wgpu_renderer` | `render(data, pass)` / `render(data, encoder, view)` | Flat tint / pass encoder |
| Metal | `strata/backend/metal.hpp` | `metal_renderer` | `render(data, render_encoder)` | Flat tint / encoder |
| CPU Software | `strata/backend/software.hpp` | `software_renderer` | `render(data)` / `render(data, damage)` | Built-in |
| Null | `strata/backend/null.hpp` | `null_renderer` | `render(data)` | - |

- **State restoration:** `d3d11_renderer` and `gl_renderer` save and restore modified pipeline state by default (safe for overlays); dedicated hosts can disable this via `set_state_restore(false)`.
- **HDR & color spaces:** `renderer.set_output({output_space::srgb | srgb_view | scrgb | hdr10, paper_white_nits})`.
- **Textures:** `create_texture` (`rgba8`, `bgra8`, `r8`, `a8`, `rgba16f`, mipmaps, `updatable`), `update_texture`, `register_texture` (wraps host GPU textures without taking ownership), and `destroy_texture`.
- **Device recovery:** check `renderer.device_lost()`, recreate the graphics device, then call `ui.rebuild_font_atlas()`, `renderer.create(...)`, and recreate host textures.

## Platform adapters & file drop

Every adapter satisfies `strata::platform_backend` and plugs directly into `strata::run_frame(ui, platform, build)`.

- **Win32** (`strata/platform/win32.hpp`): message pump hook, per-monitor DPI, IME positioning, cursor shapes, clipboard, system appearance change notifications, and OS file drag-and-drop (`platform.enable_file_drop(true)` + `platform.dropped_files()`).
- **GLFW** (`strata/platform/glfw.hpp`, header-only): include `<GLFW/glfw3.h>` first; supports keys, UTF-8 chars, mouse, scroll, HiDPI, standard cursors, clipboard, and drop callbacks (`on_drop` / `dropped_files()`).
- **SDL3** (`strata/platform/sdl3.hpp`, header-only): include `<SDL3/SDL.h>` first; processes `SDL_Event` directly including IME (`SDL_StartTextInput` / `SDL_SetTextInputArea`), HiDPI, cursors, clipboard, and `SDL_EVENT_DROP_FILE`.

## Ready-made host (`strata::app`)

Target `strata::app` (`strata/app.hpp`) creates a Win32 window, graphics device/swapchain, message loop, per-monitor DPI handling, idling, device-loss recovery, system theme tracking, saved layout persistence, and optional `multi_viewport` tear-off OS windows.

```cpp
auto app = strata::app::create({
    .title               = "tool",
    .backend             = strata::app_backend::auto_select, // or d3d11, opengl, vulkan
    .follow_system_theme = true,
    .multi_viewport      = true,
    .state_file          = "tool.ini",
});
if (!app) { return 1; }
return app->run([&](strata::app&, strata::context& ui) {
    if (auto w = ui.window("hello", {40, 40}, 300.0f)) { ui.text("hi"); }
});
```

## Headless rendering & replay

- **CPU Software Renderer:** `software_renderer` rasterizes `draw_data` without a GPU or window. For partial updates (blinking carets, single hovered buttons), pass a damage rectangle to skip unaffected regions:
  ```cpp
  sr.clear(bg, damage_rect);
  sr.render(ui.render_data(), damage_rect);
  ```
- **Deterministic input replay:** `input_recorder` / `input_replayer` (`strata/input_record.hpp`) record per-frame `input_state` to a file for reproducible bug reports and automated tests (`strata_sandbox --record FILE` / `--replay FILE`).

## C interface & Rust bindings

`strata/strata_c.h` exposes an exception-free C99 API with opaque handles and layout-matched `strata_draw_data` covering contexts, widgets (`range_slider`, `knob`, tables, tabs, menus, popups, docking, plots, node editor, debug windows), `strata_software_*`, and `strata_gl_*`.

```c
strata_context* ui = strata_context_create(NULL);
strata_begin_frame(ui, &input);
if (strata_begin_window(ui, "hello", 20, 20, 300, 160, 0)) {
    if (strata_button(ui, "ok")) { /* ... */ }
}
strata_end_window(ui);
strata_end_frame(ui);
```

Official Rust bindings live in [`bindings/rust/`](../bindings/rust/README.md) (`strata-sys` static CMake build + safe `strata` wrapper crate):

```powershell
$env:STRATA_LIB_DIR = (Resolve-Path "build\x64-dev\lib").Path
$env:RUSTFLAGS = "-C target-feature=+crt-static"
cargo test --manifest-path bindings\rust\Cargo.toml
```

## Packaging (`FetchContent` & vcpkg)

**CMake `FetchContent`** (`STRATA_MINIMAL=ON` disables sandbox, overlay, tests, and optional GPU backends):

```cmake
include(FetchContent)
FetchContent_Declare(strata GIT_REPOSITORY https://github.com/<org>/strata.git GIT_TAG main)
set(STRATA_MINIMAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(strata)
target_link_libraries(my_app PRIVATE strata::strata)
```

**vcpkg overlay port:** [`packaging/vcpkg/`](../packaging/vcpkg/vcpkg.json) supports `opengl`, `vulkan`, and `webgpu` feature flags.

## Sandbox CLI reference

```text
strata_sandbox.exe [options]        (--help lists all flags)

  --dx11 | --dx12 | --gl | --vk     graphics backend (default dx11; aliases --opengl, --vulkan)
  --software                        CPU rasterizer (screenshots work without a GPU)
  --xr | --openvr                   standalone VR loop (requires STRATA_BUILD_OPENXR / OPENVR)
  --vsync | --novsync | --fps N     present interval and frame cap
  --width N --height N --scale F    logical window size and UI scale
  --theme N|NAME | --theme-file F   built-in theme or custom .theme file
  --scene NAME                      default, features, visuals, inputs, multiselect, charts,
                                    textures, scripts, textlog, menus, context, modal, toasts,
                                    config, palette, tabs, dnd, lists, editor, icons, bigtree,
                                    rows, nodes, app
  --menu | --features | --metrics   preset layouts and debug windows
  --selftest                        run headless UI test suite (exit 0 = passed)
  --shot FILE                       render fixed 1/60 s steps, save PNG, and exit
  --golden FILE [--update-golden]   compare screenshot against PNG golden
  --diff A.png --against B.png      compare two PNGs (--diff-out OUT.png writes amplified diff)
  --record FILE | --replay FILE     record or replay per-frame input
  --idle | --frames N | --log FILE  idle skipping, fixed frame count, stderr log redirection
```

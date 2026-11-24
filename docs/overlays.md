# In-Game & VR/XR Overlays

Back to [README](../README.md).

## Contents
- [Direct3D 11, Direct3D 12 & OpenGL overlay](#direct3d-11-direct3d-12--opengl-overlay)
- [Vulkan explicit overlay layer](#vulkan-explicit-overlay-layer)
- [Runtime UI scaling](#runtime-ui-scaling)
- [VR & XR overlays (OpenXR / OpenVR)](#vr--xr-overlays-openxr--openvr)

---

## Direct3D 11, Direct3D 12 & OpenGL overlay

`overlay/` provides a static library (`strata_overlay`) for building DLLs that render a `strata` UI over Direct3D 11, Direct3D 12, or OpenGL applications (inspectors, profilers, modding tools).

```cpp
strata::overlay::options opt;
opt.backend = strata::overlay::backend_kind::auto_detect; // or dxgi / opengl
opt.ui = [](strata::context& ui) {
    if (auto w = ui.window("Inspector", {40, 40}, {360, 0})) {
        ui.text("hello");
    }
};
strata::overlay::install(opt); // call from a worker thread, never from DllMain; F1 toggles visibility
```

- **DXGI hook (D3D11 / D3D12):** probes a temporary swap chain to read `IDXGISwapChain`'s shared vtable and patches `Present`, `Present1`, `ResizeBuffers`, `ResizeBuffers1`, and `SetColorSpace1` without modifying executable code bytes. For D3D12, `ID3D12CommandQueue::ExecuteCommandLists` is hooked the same way to locate the host's direct queue.
- **OpenGL hook:** when `backend == backend_kind::opengl` (or `auto_detect` finds `opengl32.dll` loaded without D3D11/D3D12), IAT slots for `gdi32.dll!SwapBuffers` and `opengl32.dll!wglSwapBuffers` are patched without code modification (`opengl32.dll` is delay-loaded so Direct3D hosts never pull it in).
- **Safe teardown (`overlay::uninstall()`):** returns `done` (all hooks restored and resources freed; safe to unload DLL), `busy` (hooked call still in flight; retry later), or `pinned` (another hook or `WndProc` chained over ours; keep DLL loaded as pass-through).
- **Input & HUD:** the host window is subclassed to feed `win32_platform`. `options::block_game_input` controls whether input is swallowed while open; `options::hud` (or `overlay::host` / `overlay::module` in `module.hpp`) renders every frame regardless of toggle state without capturing input.
- **HDR detection:** swap chain color spaces (FP16 scRGB, 10-bit HDR10, sRGB) are detected automatically and can be overridden via `options::output`.
- **Overlay targets:** `strata_overlay` (static lib), `strata_overlay_demo` (sample DLL), `strata_overlay_inject <pid|exe> <dll>` (LoadLibrary injector), and `strata_overlay_host` (stand-in host used by `ctest`).

## Vulkan explicit overlay layer

When built with `STRATA_BUILD_VULKAN=ON`, `overlay/vulkan/` builds an official Vulkan loader layer (`strata_vk_layer.dll` + `VK_LAYER_strata_overlay.json` in `bin/`).

- Intercepts `vkCreateWin32SurfaceKHR`, `vkCreateSwapchainKHR`, and `vkQueuePresentKHR` through the standard Vulkan loader dispatch chain (`vkNegotiateLoaderLayerInterfaceVersion`) with zero binary patching.
- Activate on any Vulkan application by pointing the loader at `bin/`:
  ```powershell
  $env:VK_LAYER_PATH      = (Resolve-Path "build\x64-release\bin").Path
  $env:VK_INSTANCE_LAYERS = "VK_LAYER_strata_overlay"
  ```
- Press `F1` to toggle the overlay window.

## Runtime UI scaling

```cpp
strata::overlay::options opt;
opt.ui_scale      = 0.0f;  // 0 = start at monitor DPI scale
opt.scale_hotkeys = true;  // Ctrl + Plus / Minus step 10 %, Ctrl + 0 resets
// ...
strata::overlay::set_ui_scale_percent(125); // thread-safe; rebuilds atlas on render thread next frame
```

## VR & XR overlays (OpenXR / OpenVR)

`STRATA_BUILD_OPENXR=ON` / `STRATA_BUILD_OPENVR=ON` (requires `STRATA_BUILD_DX11=ON`) render the same 2D UI onto a quad in the VR tracking volume with controller raycast input.

```cpp
strata::vr_panel panel;
panel.width_meters = 0.6f;
panel.head_locked  = true; // follows HMD; false anchors in room space
overlay.create("my.app", "My App", panel);
// per frame: overlay.update_controllers(); input = vr_pointer_input(overlay.controllers(), input); ...
```

- **OpenXR (`xr_session`):** composition quad layer in LOCAL space (`strata_sandbox --xr`).
- **OpenVR (`vr_overlay`):** SteamVR `IVROverlay` quad (`strata_sandbox --openvr`, mirrored in `overlay/demo/demo_dll.cpp`).

> **Note:** in-game overlays are designed for local tooling, profiling, and modding; do not use in online games with anti-cheat.

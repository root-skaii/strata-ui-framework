# strata

Immediate-mode UI framework for Direct3D 11/12. C++latest (`/std:c++latest`), CMake 4.3+, MSVC, Windows x64.

```
strata/    the library  (strata::strata, static)
sandbox/   test app, same UI on d3d11 or d3d12
overlay/   in-game overlay for external d3d11/12 apps (hook, demo dll, test host, injector)
cmake/     options, compile flags, hlsl embedding, package config
tests/     selftest sources, golden screenshots, fuzz targets (tests/fuzz/corpus)
```

`README.md` is ~600 lines, organized by heading (Build, Sandbox, Widgets, Theming, Fonts, Footprint, Overlay, ...).
Grep for the heading you need instead of reading the whole file. It's written for a human skimming on GitHub, not as
an exhaustive spec: each section is a short intro + one code example, then at most a handful of one-line bullets for
what isn't obvious from the code. When adding to it, match that density — don't let it regrow into a reference manual
(exact pixel/byte counts, exhaustive flag enumerations, internal implementation notes, edge-case-by-edge-case bullets
belong in a code comment or nowhere, not the README). If a change needs more explanation than that, put the detail in
a code comment near the thing it explains instead of expanding the README section.

## Build

- PowerShell: `cmd /c ".\build_vs.cmd x64-release"` (loads the VS x64 env, configures, builds).
  Bash's `cmd //c` cannot find `build_vs.cmd` — use PowerShell, or `cmd /c` from it.
- Presets: `x64-debug`, `x64-release`, `vs2022` (generates a VS solution), `x64-asan` (ASan + `strata_fuzz`;
  its exes need the VS env on PATH — run through a `cmd` that calls `vcvars64` first, like `build_vs.cmd` does).
- Run the GUI exe (`strata_sandbox.exe`) through `cmd /c`, or PowerShell `Start-Process -Wait -PassThru`, so the
  shell actually waits for it.
- Built exes land at `build/<preset>/bin/` (e.g. `build/x64-release/bin/strata_sandbox.exe`), except the `vs2022`
  preset which puts them at `build/vs2022/bin/Release/`. See the [run skill](.claude/skills/run/SKILL.md) for the
  common invocations (screenshot a scene, headless selftest, etc.) without re-deriving them.

## Test

- `ctest --test-dir build/x64-release` — 60 tests: `strata_sandbox --selftest` (~1350 checks, headless; add
  `--log out.txt` to see output), 52 golden screenshot comparisons (dx11+dx12), `strata_render_test` (real D3D11
  device offscreen: hostile GS, output encodings, text contrast, device recovery, registered textures),
  `strata_app_test`, `strata_overlay_modules` (headless), overlay host tests (incl. `--fp16`).
- Goldens are gitignored and machine-specific (system fonts: Segoe UI, Consolas). Regenerate with
  `cmake --build ... --target strata_update_goldens` (or `strata_update_golden_<name>_<dx11|dx12>` for one) and
  **review the diff** before trusting it — failed comparisons leave `<name>.diff.png` in
  `build/x64-release/test_artifacts`.
- `strata_sandbox --dx11|--dx12 --scene NAME --shot out.png` renders fixed-step frames and saves a PNG (view with
  the Read tool). Scenes: default features visuals inputs multiselect textlog menus context modal dialog toasts
  config palette tabs dnd lists editor dockdrag. `--scale F`, `--theme NAME`, `--rescale F`. `--shot` defaults to
  1280x720 but golden scenes pass their own `--height` (see `sandbox/CMakeLists.txt` `strata_shot(...)`); popups
  flip above/below depending on the room there, so match the golden's size when finding click coordinates.
- New selftests go in `tests/selftest_{text,dock,widgets,windows,misc,robust,app,rows}.cpp` (+ `selftest_common.hpp` for
  `CHECK`/harness, `selftest.cpp` for the runner) — add the function to that file's `run_<area>_tests()` too.
  The harness turns dock animation and scroll smoothing off. `CHECK(...)` is variadic; braced initialisers are fine.

## Architecture

- `context` is a pimpl: private state lives in `struct context::impl` (`strata/src/context_impl.hpp`), accessed as
  `m_->name_` from `src/context*.cpp`. Former inline accessors moved to `src/context_access.cpp`. New private state
  goes in `impl`, not the public header. Docking state is `src/dock_state.hpp` (`m_->dock_`).
- Fixed-size tables report overflow via `report_limit()` → diagnostics hook (or stderr), once per limit, counted in
  `frame_stats::limits_hit`.
- Multi-key chords (`key_sequence`, e.g. `Ctrl+K, Ctrl+S`): `keybinds::action::chord` is a `key_sequence`
  (a `key_chord` converts implicitly). Matching state is deferred one frame (cleared in `begin_frame`) so two
  sequences sharing a prefix both get a look at the next key — don't collapse that to an immediate clear.
- Keys: one `strata::key` enum, numbered like Win32 virtual keys (win32_platform casts). `input_state::keys` is the
  only press list; `begin_frame` feeds both the one-press-per-frame shortcut queue and the editing-key list text
  fields read from it.
- Sub-widget ids (a window's resize edge, a tab's close box) use `part_id(part::x, owner)` (`src/core/part_id.hpp`),
  not `hash_id("##salt", ...)`. `##` literals that remain are real hidden labels, or persisted (`##dockspace`).
- Widget code lives one family per file: `context_window.cpp`, `context_input.cpp` (single-line fields),
  `context_text.cpp` (editing engine, multi-line), `context_table.cpp`, `context_tree.cpp`, `context_color.cpp`,
  `context_child.cpp`, `context_hotkey.cpp`, ... `context.cpp` keeps the frame lifecycle, ids, layout, basic widgets.
- Words a widget draws itself come from `ui_strings` (`m_->strings_`), never a literal.

## Overlay

- `overlay/` is a separate consumer of the library: `strata_overlay` (static lib: hook + input + drawing, link
  into your own dll), `strata_overlay_demo` (sample dll, F1 shows a docked UI in a game), `strata_overlay_host`
  (stand-in "game" that loads the dll and checks it works, `/DELAYLOAD`s both d3d11 and d3d12 so it only pulls in
  the one the loaded overlay actually uses — like a real game would), `strata_overlay_inject` (LoadLibrary injector).
- The overlay host test flashes FP16 windows on screen — that's expected, not a bug (host clear color is
  linearised; an SDR monitor tone-maps it to paper white). Don't "fix" the brightness.

## Environment quirks

- Bash-tool heredocs mangle backslash escapes (`\n` arrives as a real newline, some abort with "unexpected EOF").
  For multi-line script edits, write the file with the Write tool and run it; use the Edit tool for single edits.
- All source is LF now (`.gitattributes`), so the Edit tool works cleanly.

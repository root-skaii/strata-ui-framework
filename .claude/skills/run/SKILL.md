---
name: run
description: Launch and drive the strata sandbox to see a change working — headless selftest, a scene screenshot, the full golden test suite, or an interactive run. Project-specific: covers this repo's build/exe layout so the generic run skill doesn't have to rediscover it.
---

# Running strata

Build first (PowerShell — bash `cmd //c` can't find `build_vs.cmd`):
```
cmd /c ".\build_vs.cmd x64-release"
```

Exe lands at `build/<preset>/bin/strata_sandbox.exe` (`x64-release`, `x64-debug`, `x64-asan`), or
`build/vs2022/bin/Release/strata_sandbox.exe` for the VS-generated solution.

## Fastest ways to check a change

**Headless, no GPU window needed — prefer this first:**
```
strata_sandbox.exe --selftest --log out.txt
```
Read the tail of `out.txt` (it's silent on stdout without `--log`). ~850 checks.

**See a UI change without running interactively:**
```
strata_sandbox.exe --dx11 --scene NAME --shot out.png --width W --height H
```
Then Read `out.png`. Scenes: `default features visuals inputs multiselect textlog menus context modal dialog
toasts config palette tabs dnd lists editor dockdrag` (no golden: `dockdrag`, a docked tab mid-drag).
Match the golden's `--height` for a scene with popups (see `sandbox/CMakeLists.txt`'s `strata_shot(...)` calls,
e.g. `tabs` = 900) — popups flip above/below depending on the room there, so a mismatched size throws off any
click coordinates copied from a golden's script.

**Full suite, including screenshot-diff regressions:**
```
ctest --test-dir build/x64-release          # all 59
ctest --test-dir build/x64-release -L gpu   # just the rendering/golden ones
```
A failed golden comparison leaves `<name>.diff.png` in `build/x64-release/test_artifacts` — Read it before
trusting a rewrite.

**Interactive** (rare — needs a real desktop session): run the exe through `cmd /c`, or PowerShell
`Start-Process -Wait -PassThru`, so the shell waits for the GUI process instead of returning immediately.

## Full flag list

`--dx11`/`--dx12`, `--scale`, `--theme`, `--rescale`, `--width`/`--height`, `--frames`, `--vsync`, `--fps`,
`--menu`, `--metrics`, `--idle`, `--font`/`--face`/`--size`, and more — the parser in `sandbox/main.cpp` is the
source of truth; check there before guessing an unfamiliar flag.

# Rust bindings for strata

Workspace crates:
- `strata-sys`: raw FFI bindings to `strata/include/strata/strata_c.h`, linked statically against `strata`.
- `strata`: safe Rust wrapper (`Context`, `Ui`, `SoftwareRenderer`, `GlRenderer`).

## Build and test

When `strata` is already built with CMake (e.g. `build/x64-dev/lib/strata.lib`), point `STRATA_LIB_DIR` at the directory containing `strata.lib` / `libstrata.a`. On MSVC, `strata` builds with the static CRT (`/MT`), so pass `-C target-feature=+crt-static` (or set it in `.cargo/config.toml`):

```powershell
$env:STRATA_LIB_DIR = (Resolve-Path "..\..\build\x64-dev\lib").Path
$env:RUSTFLAGS = "-C target-feature=+crt-static"
cargo test
cargo run --example software_hello
```

Without `STRATA_LIB_DIR`, `strata-sys/build.rs` invokes CMake automatically to build the static `strata` library into `OUT_DIR`.

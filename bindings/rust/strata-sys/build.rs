use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-env-changed=STRATA_LIB_DIR");
    println!("cargo:rerun-if-env-changed=STRATA_BUILD_OPENGL");
    println!("cargo:rerun-if-env-changed=STRATA_CMAKE_GENERATOR");

    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let repo_root = manifest_dir
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .expect("strata-sys must live under bindings/rust/strata-sys");

    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join("strata/include/strata/strata_c.h").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        repo_root.join("strata/src/c_api.cpp").display()
    );

    if let Ok(lib_dir) = env::var("STRATA_LIB_DIR") {
        println!("cargo:rustc-link-search=native={lib_dir}");
    } else {
        let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
        let build_dir = out_dir.join("cmake-build");
        let profile = env::var("PROFILE").unwrap_or_else(|_| "release".into());
        let cmake_build_type = if profile == "debug" { "Debug" } else { "Release" };
        let build_gl = env::var("STRATA_BUILD_OPENGL").unwrap_or_else(|_| "ON".into());

        let mut cfg = Command::new("cmake");
        cfg.arg("-S")
            .arg(repo_root)
            .arg("-B")
            .arg(&build_dir)
            .arg(format!("-DCMAKE_BUILD_TYPE={cmake_build_type}"))
            .arg("-DSTRATA_BUILD_SANDBOX=OFF")
            .arg("-DSTRATA_BUILD_TESTS=OFF")
            .arg("-DSTRATA_BUILD_OVERLAY=OFF")
            .arg("-DSTRATA_BUILD_DX11=OFF")
            .arg("-DSTRATA_BUILD_DX12=OFF")
            .arg("-DSTRATA_BUILD_VULKAN=OFF")
            .arg(format!("-DSTRATA_BUILD_OPENGL={build_gl}"))
            .arg("-DSTRATA_LTO=OFF")
            .arg("-DSTRATA_INSTALL=OFF");

        if let Ok(gen) = env::var("STRATA_CMAKE_GENERATOR") {
            cfg.arg("-G").arg(gen);
        }

        let status = cfg.status().expect("failed to invoke cmake");
        assert!(status.success(), "cmake configure failed");

        let status = Command::new("cmake")
            .arg("--build")
            .arg(&build_dir)
            .arg("--target")
            .arg("strata")
            .arg("--config")
            .arg(cmake_build_type)
            .status()
            .expect("failed to invoke cmake --build");
        assert!(status.success(), "cmake build failed");

        // Multi-config generators (Visual Studio) place the archive under strata/<Config>;
        // single-config generators (Ninja, Makefiles) place it directly under strata/.
        let candidates = [
            build_dir.join("strata").join(cmake_build_type),
            build_dir.join("strata"),
            build_dir.join("lib").join(cmake_build_type),
            build_dir.join("lib"),
        ];
        for dir in &candidates {
            if dir.exists() {
                println!("cargo:rustc-link-search=native={}", dir.display());
            }
        }
    }

    println!("cargo:rustc-link-lib=static=strata");

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap_or_default();

    if target_os == "windows" {
        for lib in ["gdi32", "user32", "advapi32", "imm32", "shell32"] {
            println!("cargo:rustc-link-lib={lib}");
        }
        if target_env == "gnu" {
            println!("cargo:rustc-link-lib=stdc++");
        }
    } else if target_os == "macos" {
        println!("cargo:rustc-link-lib=c++");
    } else {
        println!("cargo:rustc-link-lib=stdc++");
    }
}

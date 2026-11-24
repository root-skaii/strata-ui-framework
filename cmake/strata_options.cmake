# project-wide options, output layout and the shared `strata_options` compile
# interface. every strata target links it PRIVATE (via BUILD_INTERFACE) so the
# flags never leak into consumers of the installed package.

include_guard(GLOBAL)

set(_strata_win_default OFF)
if(WIN32)
    set(_strata_win_default ON)
endif()
set(_strata_sandbox_default OFF)
if(WIN32 AND PROJECT_IS_TOP_LEVEL)
    set(_strata_sandbox_default ON)
endif()

find_package(Vulkan QUIET)
set(_strata_vulkan_default OFF)
if(Vulkan_FOUND)
    set(_strata_vulkan_default ON)
endif()
set(_strata_apple_default OFF)
if(APPLE)
    set(_strata_apple_default ON)
endif()
set(_strata_webgpu_default OFF)
if(EMSCRIPTEN)
    set(_strata_webgpu_default ON)
endif()

# Convenience umbrella for FetchContent / minimal embeds (software + null only, no sandbox/tests/overlay)
option(STRATA_MINIMAL "Disable optional backends, sandbox, overlay, and tests" OFF)

# -- Backends ------------------------------------------------------------------
option(STRATA_BUILD_DX11    "Build the Direct3D 11 backend (Windows)"          ${_strata_win_default})
option(STRATA_BUILD_DX12    "Build the Direct3D 12 backend (Windows)"          ${_strata_win_default})
option(STRATA_BUILD_OPENGL  "Build the OpenGL 3.3+ Core backend"               ON)
option(STRATA_BUILD_VULKAN  "Build the Vulkan backend (needs Vulkan SDK)"      ${_strata_vulkan_default})
option(STRATA_BUILD_WEBGPU  "Build the WebGPU backend (Dawn / wgpu / WASM)"    ${_strata_webgpu_default})
option(STRATA_BUILD_METAL   "Build the Apple Metal backend (macOS / iOS)"      ${_strata_apple_default})
# VR quad overlays (fetches Khronos OpenXR / Valve OpenVR on first enable; requires STRATA_BUILD_DX11)
option(STRATA_BUILD_OPENXR  "Build the OpenXR VR quad backend (Windows)"       OFF)
option(STRATA_BUILD_OPENVR  "Build the OpenVR (SteamVR) backend (Windows)"     OFF)

# -- Components ----------------------------------------------------------------
set(_strata_overlay_default OFF)
if(WIN32 AND STRATA_BUILD_DX11 AND STRATA_BUILD_DX12)
    set(_strata_overlay_default ON)
endif()
option(STRATA_BUILD_SANDBOX  "Build the sandbox test application (Windows)"     ${_strata_sandbox_default})
option(STRATA_BUILD_WEB_DEMO "Build the browser WebGPU sandbox (Emscripten)"    ${_strata_webgpu_default})
option(STRATA_BUILD_OVERLAY  "Build the D3D11/D3D12 in-game overlay targets"    ${_strata_overlay_default})
option(STRATA_BUILD_TESTS   "Register headless and screenshot tests with CTest" ${PROJECT_IS_TOP_LEVEL})
option(STRATA_BUILD_FUZZERS "Build the libFuzzer target strata_fuzz"           OFF)
option(STRATA_INSTALL       "Generate CMake install and package config rules"  ${PROJECT_IS_TOP_LEVEL})

if(STRATA_MINIMAL)
    set(STRATA_BUILD_DX11    OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_DX12    OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_OPENGL  OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_VULKAN  OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_WEBGPU  OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_METAL   OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_SANDBOX OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_OVERLAY OFF CACHE BOOL "" FORCE)
    set(STRATA_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(STRATA_INSTALL       OFF CACHE BOOL "" FORCE)
endif()

# -- Build & diagnostics -------------------------------------------------------
option(STRATA_STATIC_CRT    "Link the static MSVC runtime (/MT)"               ON)
option(STRATA_LTO           "Enable LTO (/GL+/LTCG or ThinLTO) outside Debug"  ON)
option(STRATA_FAST_MATH     "Compile with fast floating-point (/fp:fast)"      ON)
# /arch:AVX2 faults on pre-2013 Intel / pre-2015 AMD CPUs; off by default for broad portability
option(STRATA_AVX2          "Compile with AVX2 instructions (/arch:AVX2)"      OFF)
option(STRATA_WERROR        "Treat compiler warnings as errors"                OFF)
option(STRATA_PCH           "Precompile standard and core library headers"     ON)
# OFF strips diagnostic hooks and message text while keeping frame_stats counters
option(STRATA_DIAGNOSTICS   "Enable runtime misuse/limit diagnostics"          ON)
option(STRATA_ASAN          "Build with AddressSanitizer (/fsanitize=address)" OFF)

if(NOT WIN32 AND (STRATA_BUILD_DX11 OR STRATA_BUILD_DX12 OR STRATA_BUILD_OPENXR OR STRATA_BUILD_OPENVR OR STRATA_BUILD_OVERLAY))
    message(FATAL_ERROR "Direct3D / VR / overlay targets require Windows")
endif()

if(STRATA_BUILD_OPENXR AND NOT STRATA_BUILD_DX11)
    message(FATAL_ERROR "STRATA_BUILD_OPENXR requires STRATA_BUILD_DX11 (openxr interop uses XR_KHR_D3D11_enable)")
endif()

if(STRATA_BUILD_OPENVR AND NOT STRATA_BUILD_DX11)
    message(FATAL_ERROR "STRATA_BUILD_OPENVR requires STRATA_BUILD_DX11 (IVROverlay::SetOverlayTexture targets d3d11)")
endif()

if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # /GL + /LTCG for MSVC outside Debug. clang-cl uses explicit ThinLTO below
    # because CMake's IPO switch emits full -flto instead of -flto=thin.
    if(STRATA_LTO AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON)
    endif()

    if(MSVC)
        # strip default /EHsc and /Ob1|/Ob2 to avoid D9025 alongside our overrides
        string(REGEX REPLACE "/EH[a-z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        foreach(_cfg RELEASE RELWITHDEBINFO)
            string(REGEX REPLACE "/Ob[0-3]" "/Ob3" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
        endforeach()

        if(STRATA_STATIC_CRT)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    endif()
endif()

add_library(strata_options INTERFACE)

# CMake 4.3 has no cxx_std_26 mapping for MSVC yet; /std:c++latest is passed explicitly below.
# Skipped for clang-cl so CMake does not inject -clang:-std=c++23 ahead of /std:c++latest.
if(NOT (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    target_compile_features(strata_options INTERFACE cxx_std_23)
endif()
set_target_properties(strata_options PROPERTIES CXX_EXTENSIONS OFF)

if(WIN32)
    target_compile_definitions(strata_options INTERFACE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
        # the framework is exception-free; keeps the STL from pulling in unwind tables
        _HAS_EXCEPTIONS=0
    )
endif()

if(STRATA_PCH)
    target_precompile_headers(strata_options INTERFACE
        <algorithm> <array> <chrono> <cmath> <compare> <concepts> <cstddef> <cstdint> <cstdio> <cstring> <ctime>
        <deque> <expected> <format> <functional> <initializer_list> <limits> <memory> <mutex> <new> <ranges> <span>
        <string> <string_view> <type_traits> <utility> <vector>
    )
endif()

if(MSVC)
    target_compile_options(strata_options INTERFACE
        /std:c++latest /W4 /permissive- /utf-8
        /Zc:__cplusplus /Zc:inline
        /GR- /EHs-c-
        $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /Gw>
    )
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl is conforming by default and warns these MSVC-only switches as unused
        target_compile_options(strata_options INTERFACE /Zc:preprocessor /Zc:throwingNew)
    else()
        # C++20 designated initializers and C Vulkan/Win32 structs intentionally zero-init omitted fields
        target_compile_options(strata_options INTERFACE -Wno-missing-field-initializers)
    endif()
    target_link_options(strata_options INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
    )

    if(STRATA_LTO AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT STRATA_ASAN)
        target_compile_options(strata_options INTERFACE $<$<NOT:$<CONFIG:Debug>>:-flto=thin>)
        target_link_options(strata_options INTERFACE $<$<NOT:$<CONFIG:Debug>>:-flto=thin>)
    endif()

    if(STRATA_FAST_MATH)
        target_compile_options(strata_options INTERFACE /fp:fast)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # /fp:fast enables -ffinite-math-only in clang-cl, which folds std::isnan/isinf/isfinite
            # to constants and breaks plot_auto (NaN sentinel) and float validation
            target_compile_options(strata_options INTERFACE /clang:-fno-finite-math-only)
        endif()
    endif()
    if(STRATA_AVX2)
        target_compile_options(strata_options INTERFACE /arch:AVX2)
    endif()
    if(STRATA_WERROR)
        target_compile_options(strata_options INTERFACE /WX)
    endif()
    if(STRATA_ASAN)
        target_compile_options(strata_options INTERFACE /fsanitize=address /Zi)
        target_link_options(strata_options INTERFACE /DEBUG /INCREMENTAL:NO)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(strata_options INTERFACE
        -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti
        $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections -fdata-sections>
    )
    if(STRATA_FAST_MATH)
        target_compile_options(strata_options INTERFACE -ffast-math -fno-finite-math-only)
    endif()
    if(STRATA_AVX2)
        target_compile_options(strata_options INTERFACE -mavx2)
    endif()
    if(STRATA_WERROR)
        target_compile_options(strata_options INTERFACE -Werror)
    endif()
    if(STRATA_ASAN)
        target_compile_options(strata_options INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(strata_options INTERFACE -fsanitize=address)
    endif()
endif()

if(STRATA_ASAN AND PROJECT_IS_TOP_LEVEL)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        OFF)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)
endif()

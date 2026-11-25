cmake_minimum_required(VERSION 3.28)

project(strata
    VERSION 0.1.0
    DESCRIPTION "immediate-mode ui framework"
    LANGUAGES CXX
)

if(NOT EMSCRIPTEN AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "strata only supports 64-bit builds (or Emscripten WebAssembly)")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# no c++20 modules here; scanning only adds a build step and an @modmap
# response file to every compile command, which clangd cannot expand.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(strata_options)
if(WIN32)
    include(strata_openxr)
    include(strata_openvr)
endif()

if(STRATA_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(strata)

if((WIN32 AND STRATA_BUILD_SANDBOX) OR (EMSCRIPTEN AND STRATA_BUILD_WEB_DEMO))
    add_subdirectory(showcase)
endif()

if(WIN32 AND STRATA_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

if(EMSCRIPTEN AND STRATA_BUILD_WEB_DEMO)
    add_subdirectory(web)
endif()

if(STRATA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

set(_strata_overlay_built OFF)
if(WIN32 AND STRATA_BUILD_OVERLAY)
    if(STRATA_BUILD_DX11 AND STRATA_BUILD_DX12)
        add_subdirectory(overlay)
        set(_strata_overlay_built ON)
    else()
        message(WARNING "strata: skipping overlay (STRATA_BUILD_OVERLAY requires both STRATA_BUILD_DX11 and STRATA_BUILD_DX12)")
    endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
    set(_strata_backends "software;null")
    if(STRATA_BUILD_DX11)
        list(APPEND _strata_backends "d3d11")
    endif()
    if(STRATA_BUILD_DX12)
        list(APPEND _strata_backends "d3d12")
    endif()
    if(STRATA_BUILD_OPENGL)
        list(APPEND _strata_backends "opengl")
    endif()
    if(STRATA_BUILD_VULKAN)
        list(APPEND _strata_backends "vulkan")
    endif()
    if(STRATA_BUILD_WEBGPU)
        list(APPEND _strata_backends "webgpu")
    endif()
    if(STRATA_BUILD_METAL)
        list(APPEND _strata_backends "metal")
    endif()
    if(STRATA_BUILD_OPENXR)
        list(APPEND _strata_backends "openxr")
    endif()
    if(STRATA_BUILD_OPENVR)
        list(APPEND _strata_backends "openvr")
    endif()
    string(JOIN ", " _strata_backends_str ${_strata_backends})

    message(STATUS "strata ${PROJECT_VERSION} (${CMAKE_CXX_COMPILER_ID}, ${CMAKE_BUILD_TYPE})")
    message(STATUS "  backends:    ${_strata_backends_str}")
    message(STATUS "  components:  sandbox=${STRATA_BUILD_SANDBOX} overlay=${_strata_overlay_built} tests=${STRATA_BUILD_TESTS} fuzzers=${STRATA_BUILD_FUZZERS}")
    message(STATUS "  build flags: static_crt=${STRATA_STATIC_CRT} lto=${STRATA_LTO} pch=${STRATA_PCH} diagnostics=${STRATA_DIAGNOSTICS} asan=${STRATA_ASAN}")
endif()

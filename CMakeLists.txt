cmake_minimum_required(VERSION 4.3)

project(strata
    VERSION 0.1.0
    DESCRIPTION "immediate-mode ui framework for direct3d 11 / 12"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "strata targets windows / direct3d only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "strata only supports x64 builds")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# no c++20 modules here; scanning only adds a build step and an @modmap
# response file to every compile command, which clangd can't expand.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(strata_options)

if(STRATA_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(strata)

if(STRATA_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

if(STRATA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(STRATA_BUILD_OVERLAY AND STRATA_BUILD_DX11 AND STRATA_BUILD_DX12)
    add_subdirectory(overlay)
endif()

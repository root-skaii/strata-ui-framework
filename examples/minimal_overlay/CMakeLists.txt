cmake_minimum_required(VERSION 3.28)
project(strata_minimal_overlay LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    strata
    GIT_REPOSITORY https://github.com/root-skaii/strata-ui-framework.git
    GIT_TAG        main
)
set(STRATA_MINIMAL       ON  CACHE BOOL "" FORCE)
set(STRATA_STATIC_CRT    OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_DX11    ON  CACHE BOOL "" FORCE)
set(STRATA_BUILD_DX12    ON CACHE BOOL "" FORCE)
set(STRATA_BUILD_OPENGL  ON CACHE BOOL "" FORCE)
set(STRATA_BUILD_OVERLAY ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(strata)

add_library(minimal_overlay SHARED overlay_dll.cpp)
target_link_libraries(minimal_overlay PRIVATE strata::overlay user32.lib)

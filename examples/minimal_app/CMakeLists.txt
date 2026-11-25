cmake_minimum_required(VERSION 3.28)
project(strata_minimal_app LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    strata
    GIT_REPOSITORY https://github.com/root-skaii/strata-ui-framework.git
    GIT_TAG        main
)
set(STRATA_MINIMAL    ON  CACHE BOOL "" FORCE)
set(STRATA_STATIC_CRT OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_DX11 ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(strata)

add_executable(minimal_app main.cpp)
target_link_libraries(minimal_app PRIVATE strata::app)

# openxr (vr) support: strata's only external dependency, fetched only when STRATA_BUILD_OPENXR is on.
# provides the `openxr_loader` target (khronos loader, headers are PUBLIC on it) for strata/CMakeLists.txt to link.

include_guard(GLOBAL)

if(NOT STRATA_BUILD_OPENXR)
    return()
endif()

include(FetchContent)

# keep the fetched build to just the loader -- no sample layers, no test suite. DYNAMIC_LOADER defaults to OFF on
# windows (a plain openxr_loader.lib archive of object code), which src/loader/CMakeLists.txt then links against
# the *dynamic* CRT, assuming a static-lib consumer normally uses /MD -- this project builds with /MT
# (STRATA_STATIC_CRT), so that default mismatches and the final .exe fails with LNK2038 ("RuntimeLibrary ...
# doesn't match") plus a pile of unresolved __imp_ symbols. forcing DYNAMIC_LOADER ON instead makes it a real
# openxr_loader.dll + import lib, which that same file links against the *static* CRT (self-contained, matching
# /MT) -- an import lib also has no RuntimeLibrary metadata to conflict with in the first place. the .dll lands
# next to the .exe automatically (both use CMAKE_RUNTIME_OUTPUT_DIRECTORY, set globally before this file runs).
set(BUILD_LOADER            ON  CACHE BOOL "" FORCE)
set(BUILD_TESTS             OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS        OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER          ON  CACHE BOOL "" FORCE)

FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.42
    GIT_SHALLOW    TRUE
)

# strata_options.cmake strips /EH flags from CMAKE_CXX_FLAGS project-wide (the library itself is exception-free);
# the loader is someone else's code and expects the normal default, so it gets it back just for this subdirectory.
set(_strata_openxr_saved_flags "${CMAKE_CXX_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS " /EHsc")
FetchContent_MakeAvailable(openxr_sdk)
set(CMAKE_CXX_FLAGS "${_strata_openxr_saved_flags}")
unset(_strata_openxr_saved_flags)

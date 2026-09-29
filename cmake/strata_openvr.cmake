# openvr (steamvr) support: fetched only when STRATA_BUILD_OPENVR is on. unlike openxr's loader, openvr_api is a
# closed-source binary Valve ships prebuilt (headers/openvr.h + lib/win64/openvr_api.lib + bin/win64/openvr_api.dll)
# -- there is nothing to build, so this only downloads the tree and wraps the prebuilt files in an IMPORTED target;
# it deliberately does not add_subdirectory() the fetched CMakeLists.txt (which builds Valve's OpenGL/D3D11 sample
# apps and their own dependencies, none of which strata wants).

include_guard(GLOBAL)

if(NOT STRATA_BUILD_OPENVR)
    return()
endif()

include(FetchContent)

FetchContent_Declare(openvr_sdk
    GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
    GIT_TAG        v2.5.1
    GIT_SHALLOW    TRUE
)

# FetchContent_Populate(name) (no SOURCE_DIR/etc.) is deprecated in favour of FetchContent_MakeAvailable, but that
# alternative add_subdirectory()s the fetched tree whenever it has a top-level CMakeLists.txt -- exactly what this
# file exists to avoid. CMP0169 OLD keeps the deprecated form callable (cmake 4 errors on it otherwise); scoped to
# just this call so it doesn't loosen policy for anything else.
cmake_policy(PUSH)
cmake_policy(SET CMP0169 OLD)
FetchContent_GetProperties(openvr_sdk)
if(NOT openvr_sdk_POPULATED)
    FetchContent_Populate(openvr_sdk)
endif()
cmake_policy(POP)

add_library(openvr_api SHARED IMPORTED GLOBAL)
set_target_properties(openvr_api PROPERTIES
    IMPORTED_LOCATION             "${openvr_sdk_SOURCE_DIR}/bin/win64/openvr_api.dll"
    IMPORTED_IMPLIB                "${openvr_sdk_SOURCE_DIR}/lib/win64/openvr_api.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${openvr_sdk_SOURCE_DIR}/headers"
)

# openvr_api.dll is a prebuilt file elsewhere in the fetched tree, not something ninja produces into
# CMAKE_RUNTIME_OUTPUT_DIRECTORY the way openxr_loader.dll is -- callers that need to run (strata_sandbox,
# strata_overlay_host) must copy it next to their own output explicitly with this.
function(strata_openvr_copy_dll target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:openvr_api>" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "copying openvr_api.dll next to ${target}")
endfunction()

# project-wide options, output layout and the shared `strata_options` compile
# interface. every strata target links it PRIVATE (via BUILD_INTERFACE) so the
# flags never leak into consumers of the installed package.

include_guard(GLOBAL)

option(STRATA_BUILD_SANDBOX "build the sandbox test application"   ${PROJECT_IS_TOP_LEVEL})
option(STRATA_BUILD_DX11    "build the direct3d 11 backend"        ON)
option(STRATA_BUILD_OVERLAY "build the in-game overlay (needs direct3d 11 and 12): dll, test host, injector" ON)
option(STRATA_BUILD_DX12    "build the direct3d 12 backend"        ON)
option(STRATA_INSTALL       "generate install / package rules"     ${PROJECT_IS_TOP_LEVEL})
option(STRATA_STATIC_CRT    "link the static msvc runtime (/MT)"   ON)
option(STRATA_FAST_MATH     "compile with /fp:fast"                ON)
# /arch:AVX2 makes the binaries fault with an illegal instruction on cpus
# without AVX2 (pre-2013 intel / pre-2015 amd). see README "requirements".
option(STRATA_AVX2          "compile with /arch:AVX2"              ON)
option(STRATA_WERROR        "treat warnings as errors"             OFF)
# ctest: the headless self-test, and screenshot comparisons of the sandbox scenes against sandbox/golden/*.png
option(STRATA_BUILD_TESTS   "register the sandbox tests with ctest"  ${PROJECT_IS_TOP_LEVEL})

if(NOT STRATA_BUILD_DX11 AND NOT STRATA_BUILD_DX12)
    message(FATAL_ERROR "enable at least one of STRATA_BUILD_DX11 / STRATA_BUILD_DX12")
endif()

if(PROJECT_IS_TOP_LEVEL)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
    endif()

    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # /GL + /LTCG for everything that isn't a debug build
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL     ON)

    # cmake's default /EHsc and /Ob1|/Ob2 would trigger D9025 next to our overrides
    string(REGEX REPLACE "/EH[a-z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    foreach(_cfg RELEASE RELWITHDEBINFO)
        string(REGEX REPLACE "/Ob[0-3]" "/Ob3" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
    endforeach()

    if(STRATA_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(strata_options INTERFACE)

# cmake 4.3 has no cxx_std_26 mapping for msvc yet: cxx_std_23 selects
# /std:c++latest, which is also passed explicitly below so we always track the
# newest standard the installed compiler offers.
target_compile_features(strata_options INTERFACE cxx_std_23)
set_target_properties(strata_options PROPERTIES CXX_EXTENSIONS OFF)

target_compile_definitions(strata_options INTERFACE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    # the framework is exception free; keeps the stl from pulling in unwind tables
    _HAS_EXCEPTIONS=0
)

if(MSVC)
    target_compile_options(strata_options INTERFACE
        /std:c++latest /W4 /MP /permissive- /utf-8
        /Zc:__cplusplus /Zc:inline /Zc:preprocessor /Zc:throwingNew
        /GR- /EHs-c-
        # release: intrinsics, function-level + global-data comdats for /OPT:REF|ICF
        # (/O2 and /Ob3 come from CMAKE_CXX_FLAGS_RELEASE, see above)
        $<$<NOT:$<CONFIG:Debug>>:/Oi /Gy /Gw>
    )
    target_link_options(strata_options INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
    )

    if(STRATA_FAST_MATH)
        target_compile_options(strata_options INTERFACE /fp:fast)
    endif()
    if(STRATA_AVX2)
        target_compile_options(strata_options INTERFACE /arch:AVX2)
    endif()
    if(STRATA_WERROR)
        target_compile_options(strata_options INTERFACE /WX)
    endif()
endif()

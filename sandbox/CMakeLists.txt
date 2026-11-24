# strata_sandbox - windowed test application running the same UI across backends
#   strata_sandbox.exe [--dx11 | --dx12 | --gl | --vk | --software] [--novsync] [--frames N]

add_executable(strata_sandbox WIN32
    demo2.cpp
    demo2.hpp
    demo3.cpp
    demo3.hpp
    demo4.cpp
    demo4.hpp
    gfx_host.hpp
    gfx_software.cpp
    imgio.cpp
    imgio.hpp
    main.cpp
    ../tests/selftest.cpp
    ../tests/selftest.hpp
    ../tests/selftest_app.cpp
    ../tests/selftest_backends.cpp
    ../tests/selftest_common.hpp
    ../tests/selftest_dock.cpp
    ../tests/selftest_interactive.cpp
    ../tests/selftest_misc.cpp
    ../tests/selftest_new.cpp
    ../tests/selftest_robust.cpp
    ../tests/selftest_rows.cpp
    ../tests/selftest_text.cpp
    ../tests/selftest_tools.cpp
    ../tests/selftest_widgets.cpp
    ../tests/selftest_windows.cpp
)

target_include_directories(strata_sandbox PRIVATE
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/strata/src"
)

target_link_libraries(strata_sandbox PRIVATE
    $<BUILD_INTERFACE:strata_options>
    strata::strata
    user32.lib
    shell32.lib
)
if(STRATA_PCH)
    target_precompile_headers(strata_sandbox PRIVATE <strata/strata.hpp>)
endif()

if(STRATA_BUILD_DX11)
    target_sources(strata_sandbox PRIVATE gfx_d3d11.cpp)
    target_link_libraries(strata_sandbox PRIVATE d3d11.lib dxgi.lib)
endif()

if(STRATA_BUILD_DX12)
    target_sources(strata_sandbox PRIVATE gfx_d3d12.cpp)
    target_link_libraries(strata_sandbox PRIVATE d3d12.lib dxgi.lib)
endif()

if(STRATA_BUILD_OPENGL)
    target_sources(strata_sandbox PRIVATE gfx_opengl.cpp)
    target_link_libraries(strata_sandbox PRIVATE opengl32.lib)
endif()

if(STRATA_BUILD_VULKAN)
    target_sources(strata_sandbox PRIVATE gfx_vulkan.cpp)
    target_link_libraries(strata_sandbox PRIVATE Vulkan::Vulkan)
endif()

if(STRATA_BUILD_OPENXR)
    target_sources(strata_sandbox PRIVATE xr_main.cpp xr_main.hpp)
endif()

if(STRATA_BUILD_OPENVR)
    target_sources(strata_sandbox PRIVATE openvr_main.cpp openvr_main.hpp)
    target_link_libraries(strata_sandbox PRIVATE openvr_api)
    strata_openvr_copy_dll(strata_sandbox)
endif()

set_target_properties(strata_sandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

# default startup project for Visual Studio generators
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT strata_sandbox)

# tests ------------------------------------------------------------------------
#   ctest --test-dir build/x64-release -LE gpu   headless self-test + CPU software shots
#   ctest --test-dir build/x64-release           everything (includes GPU shots)
#   cmake --build build/x64-release --target strata_update_goldens   regenerate local goldens
# Goldens are local/gitignored and machine-specific (system fonts, GPU rounding).
# Mismatches write <name>.actual.png and <name>.diff.png into <build>/test_artifacts.
set(STRATA_GOLDEN_DIR       "${PROJECT_SOURCE_DIR}/tests/golden" CACHE PATH   "Directory for local golden screenshots")
set(STRATA_GOLDEN_TOLERANCE 6                                    CACHE STRING "Max per-channel difference (0..255) for a matching pixel")
set(STRATA_GOLDEN_ALLOWED   0.3                                  CACHE STRING "Max percentage of differing pixels allowed")

if(STRATA_BUILD_TESTS)
    file(MAKE_DIRECTORY "${STRATA_GOLDEN_DIR}")
    add_test(NAME strata_selftest COMMAND strata_sandbox --selftest)
    set_tests_properties(strata_selftest PROPERTIES TIMEOUT 120)

    add_custom_target(strata_update_goldens)

    # registers golden screenshot comparisons for each enabled D3D backend and the CPU software rasteriser
    function(strata_shot name)
        foreach(api IN ITEMS dx11 dx12 software)
            string(TOUPPER "${api}" api_upper)
            if(NOT api STREQUAL "software" AND NOT STRATA_BUILD_${api_upper})
                continue()
            endif()
            set(golden "${STRATA_GOLDEN_DIR}/${name}_${api}.png")
            add_test(NAME shot_${name}_${api}
                COMMAND strata_sandbox --${api} ${ARGN}
                    --golden "${golden}"
                    --golden-tolerance ${STRATA_GOLDEN_TOLERANCE}
                    --golden-allowed ${STRATA_GOLDEN_ALLOWED}
                    --artifacts "${CMAKE_BINARY_DIR}/test_artifacts"
            )
            set(label gpu)
            if(api STREQUAL "software")
                set(label software)
            endif()
            set_tests_properties(shot_${name}_${api} PROPERTIES LABELS "${label}" TIMEOUT 180)
            add_custom_target(strata_update_golden_${name}_${api}
                COMMAND strata_sandbox --${api} ${ARGN} --golden "${golden}" --update-golden
                COMMENT "golden ${name} (${api})"
            )
            add_dependencies(strata_update_goldens strata_update_golden_${name}_${api})
        endforeach()
    endfunction()

    strata_shot(default)
    strata_shot(features    --scene features)
    strata_shot(visuals     --scene visuals)
    strata_shot(visuals15   --scene visuals --scale 1.5)
    strata_shot(inputs      --scene inputs --height 1100)
    strata_shot(multiselect --scene multiselect --height 1100)
    strata_shot(charts      --scene charts --height 800)
    strata_shot(textures    --scene textures --height 800)
    strata_shot(scripts     --scene scripts)
    strata_shot(textlog     --scene textlog)
    strata_shot(menus       --scene menus)
    strata_shot(context     --scene context)
    strata_shot(modal       --scene modal --shot-frames 60)
    strata_shot(toasts      --scene toasts)
    strata_shot(config      --scene config --height 1000)
    strata_shot(palette     --scene palette --height 1000 --shot-frames 60)
    strata_shot(tabs        --scene tabs --height 900)
    strata_shot(dnd         --scene dnd --height 900)
    strata_shot(lists       --scene lists --height 900)
    strata_shot(editor      --scene editor --height 900)
    strata_shot(icons       --scene icons --width 1240 --height 1000)
    strata_shot(bigtree     --scene bigtree --width 1020 --height 900)
    strata_shot(rows        --scene rows --width 880 --height 620)
    strata_shot(app         --scene app --width 900 --height 640)
    strata_shot(nodes       --scene nodes --width 760 --height 520)
    strata_shot(dracula     --theme dracula)
    strata_shot(glass       --theme glass --scene visuals)
endif()

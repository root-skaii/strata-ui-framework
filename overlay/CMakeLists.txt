# strata overlay - in-game overlay for Direct3D 11 and 12 applications
#   strata_overlay          static library: hook, input, drawing (link into a host DLL)
#   strata_overlay_demo     sample DLL (strata_overlay_demo.dll): F1 toggles a docked UI
#   strata_overlay_host     stand-in host used by CTest to verify loading and rendering
#   strata_overlay_inject   minimal LoadLibrary injector

add_library(strata_overlay STATIC
    src/attach.cpp
    src/capture.cpp
    src/diagnostics.cpp
    src/frame.cpp
    src/hooks.cpp
    src/host.cpp
    src/output.cpp
    src/overlay.cpp
    src/render_d3d12.cpp
    src/window.cpp
)
add_library(strata::overlay ALIAS strata_overlay)
target_include_directories(strata_overlay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(strata_overlay
    PUBLIC  strata::strata
    PRIVATE $<BUILD_INTERFACE:strata_options> d3d11.lib dxgi.lib user32.lib
)

add_library(strata_overlay_demo SHARED demo/demo_dll.cpp)
target_link_libraries(strata_overlay_demo PRIVATE strata_overlay $<BUILD_INTERFACE:strata_options> user32.lib)
if(STRATA_BUILD_OPENVR)
    target_link_libraries(strata_overlay_demo PRIVATE openvr_api)
    strata_openvr_copy_dll(strata_overlay_demo)
endif()

add_executable(strata_overlay_host WIN32 tools/test_host.cpp)
target_link_libraries(strata_overlay_host PRIVATE
    $<BUILD_INTERFACE:strata_options>
    d3d11.lib
    d3d12.lib
    dxgi.lib
    user32.lib
    shell32.lib
    delayimp.lib
)
# load only the active API DLL per run so tests verify the overlay does not eagerly pull in the other
target_link_options(strata_overlay_host PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:d3d12.dll)

# headless module / lifecycle checks (no window, hook, or real input)
add_executable(strata_overlay_module_test tools/module_test.cpp)
target_link_libraries(strata_overlay_module_test PRIVATE strata_overlay $<BUILD_INTERFACE:strata_options>)

add_executable(strata_overlay_inject tools/injector.cpp)
target_link_libraries(strata_overlay_inject PRIVATE $<BUILD_INTERFACE:strata_options> user32.lib)

if(STRATA_BUILD_TESTS)
    add_test(NAME strata_overlay_modules COMMAND strata_overlay_module_test)

    add_test(NAME strata_overlay_smoke
        COMMAND strata_overlay_host --dll $<TARGET_FILE:strata_overlay_demo>)
    add_test(NAME strata_overlay_smoke_dx12
        COMMAND strata_overlay_host --d3d12 --dll $<TARGET_FILE:strata_overlay_demo>)
    add_test(NAME strata_overlay_smoke_fp16
        COMMAND strata_overlay_host --fp16 --dll $<TARGET_FILE:strata_overlay_demo>)
    add_test(NAME strata_overlay_smoke_dx12_fp16
        COMMAND strata_overlay_host --d3d12 --fp16 --dll $<TARGET_FILE:strata_overlay_demo>)

    set_tests_properties(strata_overlay_smoke_fp16 strata_overlay_smoke_dx12_fp16
        PROPERTIES TIMEOUT 60 LABELS gpu)
    set_tests_properties(strata_overlay_smoke PROPERTIES
        TIMEOUT 60
        LABELS gpu
        ENVIRONMENT "STRATA_OVERLAY_CAPTURE=${PROJECT_BINARY_DIR}/test_artifacts/overlay_smoke.png")
    set_tests_properties(strata_overlay_smoke_dx12 PROPERTIES
        TIMEOUT 60
        LABELS gpu
        ENVIRONMENT "STRATA_OVERLAY_CAPTURE=${PROJECT_BINARY_DIR}/test_artifacts/overlay_smoke_dx12.png")
endif()

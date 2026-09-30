# strata overlay - an in-game overlay for direct3d 11 and 12 applications
#   strata_overlay          static library: the hook, input, drawing (link it into your own dll)
#   strata_overlay_demo     the sample dll (strata_overlay_demo.dll): F1 shows a docked ui in the game
#   strata_overlay_host     a stand-in "game" that loads the dll and checks that it works
#   strata_overlay_inject   a small LoadLibrary injector

add_library(strata_overlay STATIC
    src/overlay.cpp src/host.cpp src/diagnostics.cpp src/capture.cpp src/render_d3d12.cpp src/window.cpp
    src/attach.cpp src/output.cpp src/frame.cpp src/hooks.cpp)
add_library(strata::overlay ALIAS strata_overlay)
target_include_directories(strata_overlay PUBLIC include)
target_link_libraries(strata_overlay PUBLIC strata::strata PRIVATE $<BUILD_INTERFACE:strata_options> d3d11.lib dxgi.lib user32.lib)

add_library(strata_overlay_demo SHARED demo/demo_dll.cpp)
target_link_libraries(strata_overlay_demo PRIVATE strata_overlay $<BUILD_INTERFACE:strata_options> user32.lib)
if(STRATA_BUILD_OPENVR)
    target_link_libraries(strata_overlay_demo PRIVATE openvr_api)
    strata_openvr_copy_dll(strata_overlay_demo)
endif()

add_executable(strata_overlay_host WIN32 tools/test_host.cpp)
target_link_libraries(strata_overlay_host PRIVATE $<BUILD_INTERFACE:strata_options> d3d11.lib d3d12.lib dxgi.lib user32.lib shell32.lib delayimp.lib)
# like a real game, a run only loads the api it uses; the host checks that the overlay does not bring in the other one
target_link_options(strata_overlay_host PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:d3d12.dll)

# headless: modules, modes, ordering, enable / remove (no window, no hook, no input)
add_executable(strata_overlay_module_test tools/module_test.cpp)
target_link_libraries(strata_overlay_module_test PRIVATE strata_overlay $<BUILD_INTERFACE:strata_options>)

add_executable(strata_overlay_inject tools/injector.cpp)
target_link_libraries(strata_overlay_inject PRIVATE $<BUILD_INTERFACE:strata_options> user32.lib)
target_compile_definitions(strata_overlay_inject PRIVATE UNICODE _UNICODE)

if(STRATA_BUILD_TESTS)
    add_test(NAME strata_overlay_modules COMMAND strata_overlay_module_test)
    # the host loads the dll the way an injector would, presses F1 through its message queue and checks that frames are
    # drawn while the overlay is open (and only then); the frame is captured as a png next to the other test artifacts
    add_test(NAME strata_overlay_smoke
             COMMAND strata_overlay_host --dll $<TARGET_FILE:strata_overlay_demo>)
    add_test(NAME strata_overlay_smoke_dx12
             COMMAND strata_overlay_host --d3d12 --dll $<TARGET_FILE:strata_overlay_demo>)
    # an FP16 (scRGB) swap chain: the overlay has to notice and encode the ui for it
    add_test(NAME strata_overlay_smoke_fp16
             COMMAND strata_overlay_host --fp16 --dll $<TARGET_FILE:strata_overlay_demo>)
    add_test(NAME strata_overlay_smoke_dx12_fp16
             COMMAND strata_overlay_host --d3d12 --fp16 --dll $<TARGET_FILE:strata_overlay_demo>)
    set_tests_properties(strata_overlay_smoke_fp16 strata_overlay_smoke_dx12_fp16 PROPERTIES TIMEOUT 60 LABELS gpu)
    set_tests_properties(strata_overlay_smoke PROPERTIES
        TIMEOUT 60
        LABELS gpu
        ENVIRONMENT "STRATA_OVERLAY_CAPTURE=${PROJECT_BINARY_DIR}/test_artifacts/overlay_smoke.png")
    set_tests_properties(strata_overlay_smoke_dx12 PROPERTIES
        TIMEOUT 60
        LABELS gpu
        ENVIRONMENT "STRATA_OVERLAY_CAPTURE=${PROJECT_BINARY_DIR}/test_artifacts/overlay_smoke_dx12.png")
endif()

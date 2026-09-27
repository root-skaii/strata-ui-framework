# strata_fuzz: libFuzzer over the parsers and text editing (the x64-asan preset builds it with AddressSanitizer). ctest
# gives it a short run from the seed corpus -- copied into the build folder, since libFuzzer adds what it finds to the
# corpus it is given; fuzz for longer by hand:  strata_fuzz build/x64-asan/tests/fuzz_corpus -max_len=4096
if(STRATA_BUILD_FUZZERS)
    add_executable(strata_fuzz fuzz/fuzz_main.cpp)
    target_link_libraries(strata_fuzz PRIVATE strata::strata $<BUILD_INTERFACE:strata_options>)
    target_compile_options(strata_fuzz PRIVATE /fsanitize=fuzzer)
    add_custom_command(TARGET strata_fuzz POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus" "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus")
    add_test(NAME strata_fuzz_smoke COMMAND strata_fuzz "${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus" -runs=20000 -max_len=2048)
    set_tests_properties(strata_fuzz_smoke PROPERTIES TIMEOUT 600)
endif()

# strata_render_test: the renderers against a real direct3d 11 device (hardware, or WARP), drawing offscreen
if(STRATA_BUILD_DX11)
    add_executable(strata_render_test render_test.cpp)
    target_link_libraries(strata_render_test PRIVATE strata::strata $<BUILD_INTERFACE:strata_options> d3d11.lib dxgi.lib)
    strata_embed_hlsl(strata_render_test SOURCE hostile.hlsl ENTRY gs_swallow PROFILE gs_5_0 SYMBOL g_test_gs_swallow)
    add_test(NAME strata_render_test COMMAND strata_render_test)
    set_tests_properties(strata_render_test PROPERTIES LABELS gpu TIMEOUT 60)

    # strata::app: a real window, device and message loop
    add_executable(strata_app_test app_test.cpp)
    target_link_libraries(strata_app_test PRIVATE strata::app $<BUILD_INTERFACE:strata_options> user32.lib)
    add_test(NAME strata_app_test COMMAND strata_app_test)
    set_tests_properties(strata_app_test PROPERTIES LABELS gpu TIMEOUT 60)
endif()

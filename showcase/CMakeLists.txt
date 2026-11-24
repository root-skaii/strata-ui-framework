# Platform- and renderer-agnostic modular showcase library shared by desktop (strata_sandbox) and browser (strata_web_demo).

add_library(strata_showcase STATIC
    app.cpp
    app.hpp
    module.hpp
    modules/mod_charts.cpp
    modules/mod_console.cpp
    modules/mod_data.cpp
    modules/mod_editor.cpp
    modules/mod_explorer.cpp
    modules/mod_inspector.cpp
    modules/mod_nodes.cpp
    modules/mod_overlays.cpp
    modules/mod_pickers.cpp
    modules/mod_sequencer.cpp
    modules/mod_typography.cpp
    modules/mod_visuals.cpp
    modules/registry.cpp
    modules/registry.hpp
)

target_include_directories(strata_showcase PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(strata_showcase PUBLIC
    strata::strata
)

target_link_libraries(strata_showcase PRIVATE
    $<BUILD_INTERFACE:strata_options>
)

if(STRATA_PCH)
    target_precompile_headers(strata_showcase PRIVATE <strata/strata.hpp>)
endif()

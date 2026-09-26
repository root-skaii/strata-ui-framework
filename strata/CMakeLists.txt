# strata - the framework library

include(strata_shaders)

add_library(strata STATIC
    src/bidi.cpp
    src/config.cpp
    src/context.cpp
    src/context_badge.cpp
    src/context_code.cpp
    src/context_combo.cpp
    src/context_data.cpp
    src/context_nav.cpp
    src/context_datetime.cpp
    src/context_dock.cpp
    src/context_extra.cpp
    src/context_image.cpp
    src/context_log.cpp
    src/context_menu.cpp
    src/context_modal.cpp
    src/context_number.cpp
    src/context_popup.cpp
    src/context_plot.cpp
    src/context_rich.cpp
    src/context_tabs.cpp
    src/context_text.cpp
    src/context_toast.cpp
    src/datetime.cpp
    src/draw_list.cpp
    src/font.cpp
    src/keybinds.cpp
    src/limits.cpp
    src/platform_win32.cpp
    src/texture.cpp
    src/themes.cpp
    src/vmem.cpp
)
add_library(strata::strata ALIAS strata)

target_sources(strata PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/strata/backend/d3d11.hpp
        include/strata/backend/d3d12.hpp
        include/strata/bidi.hpp
        include/strata/config.hpp
        include/strata/platform/win32.hpp
        include/strata/context.hpp
        include/strata/datetime.hpp
        include/strata/draw_list.hpp
        include/strata/font.hpp
        include/strata/icons.hpp
        include/strata/keybinds.hpp
        include/strata/strata.hpp
        include/strata/texture.hpp
        include/strata/themes.hpp
        include/strata/types.hpp
        include/strata/vmem.hpp
)
target_sources(strata PRIVATE src/text_util.hpp src/limits.hpp src/dock_state.hpp)

target_link_libraries(strata PRIVATE $<BUILD_INTERFACE:strata_options> imm32.lib)
target_compile_features(strata PUBLIC cxx_std_23)
set_target_properties(strata PROPERTIES CXX_EXTENSIONS OFF)

# windows.h stays out of the public headers, but consumers still get the
# knobs that keep their own includes lean.
target_compile_definitions(strata PUBLIC
    NOMINMAX
    _HAS_EXCEPTIONS=0
    $<$<BOOL:${STRATA_BUILD_DX11}>:STRATA_HAS_DX11=1>
    $<$<BOOL:${STRATA_BUILD_DX12}>:STRATA_HAS_DX12=1>
)

if(STRATA_BUILD_DX11 OR STRATA_BUILD_DX12)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY vs_main PROFILE vs_5_0 SYMBOL g_strata_ui_vs)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_main PROFILE ps_5_0 SYMBOL g_strata_ui_ps)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_image PROFILE ps_5_0 SYMBOL g_strata_ui_ps_image)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_backdrop PROFILE ps_5_0 SYMBOL g_strata_ui_ps_backdrop)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY vs_fullscreen PROFILE vs_5_0 SYMBOL g_strata_ui_vs_fullscreen)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_blur_down PROFILE ps_5_0 SYMBOL g_strata_ui_ps_blur_down)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_blur_gauss PROFILE ps_5_0 SYMBOL g_strata_ui_ps_blur_gauss)
endif()

if(STRATA_BUILD_DX11)
    target_sources(strata PRIVATE src/backend_d3d11.cpp)
    target_link_libraries(strata PRIVATE d3d11.lib dxgi.lib)
    # only load d3d11.dll when a d3d11 renderer is actually created
    target_link_options(strata INTERFACE /DELAYLOAD:d3d11.dll)
endif()

if(STRATA_BUILD_DX12)
    target_sources(strata PRIVATE src/backend_d3d12.cpp)
    target_link_libraries(strata PRIVATE d3d12.lib dxgi.lib)
    target_link_options(strata INTERFACE /DELAYLOAD:d3d12.dll)
endif()

if(STRATA_BUILD_DX11 OR STRATA_BUILD_DX12)
    target_link_libraries(strata INTERFACE delayimp.lib)
endif()

target_link_libraries(strata PRIVATE gdi32.lib user32.lib)

if(STRATA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS strata
        EXPORT strata_targets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(EXPORT strata_targets
        NAMESPACE strata::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/strata"
    )

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/cmake/strata-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/strata-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/strata"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/strata-config-version.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/strata-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/strata-config-version.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/strata"
    )
endif()

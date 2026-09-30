# strata - the framework library

include(strata_shaders)

add_library(strata STATIC
    src/bidi.cpp
    src/config.cpp
    src/context.cpp
    src/context_badge.cpp
    src/context_code.cpp
    src/context_combo.cpp
    src/context_color.cpp
    src/context_table.cpp
    src/context_tree.cpp
    src/context_nav.cpp
    src/core/animation.cpp
    src/core/id_stack.cpp
    src/core/style_stack.cpp
    src/context_datetime.cpp
    src/context_debug.cpp
    src/context_dock.cpp
    src/context_child.cpp
    src/context_hotkey.cpp
    src/context_input.cpp
    src/context_window.cpp
    src/context_image.cpp
    src/context_log.cpp
    src/context_menu.cpp
    src/context_modal.cpp
    src/context_number.cpp
    src/context_popup.cpp
    src/context_plot.cpp
    src/context_access.cpp
    src/context_rich.cpp
    src/context_state.cpp
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
        include/strata/backend/backend.hpp
        include/strata/backend/d3d11.hpp
        include/strata/backend/d3d12.hpp
        include/strata/backend/openxr.hpp
        include/strata/backend/openvr.hpp
        include/strata/backend/vr_types.hpp
        include/strata/bidi.hpp
        include/strata/config.hpp
        include/strata/platform/win32.hpp
        include/strata/context.hpp
        include/strata/datetime.hpp
        include/strata/draw_list.hpp
        include/strata/font.hpp
        include/strata/frame.hpp
        include/strata/icons.hpp
        include/strata/keybinds.hpp
        include/strata/log.hpp
        include/strata/log_queue.hpp
        include/strata/strata.hpp
        include/strata/texture.hpp
        include/strata/themes.hpp
        include/strata/types.hpp
        include/strata/vmem.hpp
)
target_sources(strata PRIVATE src/text_util.hpp src/limits.hpp src/chart_state.hpp src/child_card_state.hpp src/code_state.hpp src/color_picker_state.hpp src/combo_filter_state.hpp src/confirm_state.hpp src/date_picker_state.hpp src/dock_state.hpp src/drag_drop_state.hpp src/edit_state.hpp src/hotkey_state.hpp src/measure_cache_state.hpp src/menu_state.hpp src/modal_state.hpp src/nav_state.hpp src/popup_state.hpp src/rich_text_state.hpp src/row_accessory_state.hpp src/table_tree_state.hpp src/toast_state.hpp src/hash.hpp src/blur_plan.hpp src/context_impl.hpp src/core/animation.hpp src/core/id_stack.hpp src/core/input_frame.hpp src/core/layout_state.hpp src/core/style_stack.hpp src/core/window_registry.hpp src/core/part_id.hpp src/widget_util.hpp)

target_link_libraries(strata PRIVATE $<BUILD_INTERFACE:strata_options> imm32.lib)
# skipped for clang-cl: see the matching guard + comment in cmake/strata_options.cmake --
# this would otherwise inject -clang:-std=c++23 ahead of /std:c++latest and win.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_features(strata PUBLIC cxx_std_23)
endif()
set_target_properties(strata PROPERTIES CXX_EXTENSIONS OFF)

# windows.h stays out of the public headers, but consumers still get the
# knobs that keep their own includes lean.
target_compile_definitions(strata PUBLIC
    NOMINMAX
    _HAS_EXCEPTIONS=0
    $<$<BOOL:${STRATA_BUILD_DX11}>:STRATA_HAS_DX11=1>
    $<$<BOOL:${STRATA_BUILD_DX12}>:STRATA_HAS_DX12=1>
    $<$<BOOL:${STRATA_BUILD_OPENXR}>:STRATA_HAS_OPENXR=1>
    $<$<BOOL:${STRATA_BUILD_OPENVR}>:STRATA_HAS_OPENVR=1>
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

if(STRATA_BUILD_OPENXR)
    target_sources(strata PRIVATE src/backend_openxr.cpp)
    target_link_libraries(strata PRIVATE openxr_loader)
    # delay-loaded so detect_vr() (openvr.hpp) sees an *injected* strata linking this as still "not loaded" until
    # something actually calls into it -- an eager load would make the check true for every process this dll is
    # in, whether or not the host game itself uses openxr.
    target_link_options(strata INTERFACE /DELAYLOAD:openxr_loader.dll)
    target_link_libraries(strata INTERFACE delayimp.lib)
endif()

if(STRATA_BUILD_OPENVR)
    target_sources(strata PRIVATE src/backend_openvr.cpp)
    target_link_libraries(strata PRIVATE openvr_api)
    target_link_options(strata INTERFACE /DELAYLOAD:openvr_api.dll) # see the openxr_loader comment above
    target_link_libraries(strata INTERFACE delayimp.lib)
endif()

target_link_libraries(strata PRIVATE gdi32.lib user32.lib advapi32.lib)

# strata::app: a ready-made window + direct3d 11 host around the library (strata itself never creates either)
if(STRATA_BUILD_DX11)
    add_library(strata_app STATIC src/app_win32.cpp)
    add_library(strata::app ALIAS strata_app)
    target_sources(strata_app PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/strata/app.hpp)
    target_link_libraries(strata_app PUBLIC strata::strata PRIVATE $<BUILD_INTERFACE:strata_options> d3d11.lib dxgi.lib user32.lib)
endif()

if(STRATA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(strata_install_targets strata)
    if(TARGET strata_app)
        set_target_properties(strata_app PROPERTIES EXPORT_NAME app) # (strata::app for consumers of the package)
        list(APPEND strata_install_targets strata_app)
    endif()
    install(TARGETS ${strata_install_targets}
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

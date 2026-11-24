# strata - the framework library

if(STRATA_BUILD_DX11 OR STRATA_BUILD_DX12)
    include(strata_shaders)
endif()
if(STRATA_BUILD_VULKAN)
    find_package(Vulkan REQUIRED)
    include(strata_spirv)
endif()

add_library(strata STATIC
    src/backend_software.cpp
    src/bidi.cpp
    src/c_api.cpp
    src/config.cpp
    src/context.cpp
    src/context_access.cpp
    src/context_badge.cpp
    src/context_child.cpp
    src/context_code.cpp
    src/context_color.cpp
    src/context_combo.cpp
    src/context_datetime.cpp
    src/context_debug.cpp
    src/context_dock.cpp
    src/context_editor_widgets.cpp
    src/context_hotkey.cpp
    src/context_image.cpp
    src/context_input.cpp
    src/context_log.cpp
    src/context_menu.cpp
    src/context_modal.cpp
    src/context_nav.cpp
    src/context_number.cpp
    src/context_plot.cpp
    src/context_popup.cpp
    src/context_rich.cpp
    src/context_state.cpp
    src/context_table.cpp
    src/context_tabs.cpp
    src/context_text.cpp
    src/context_toast.cpp
    src/context_tree.cpp
    src/context_window.cpp
    src/core/animation.cpp
    src/core/id_stack.cpp
    src/core/style_stack.cpp
    src/datetime.cpp
    src/draw_list.cpp
    src/font.cpp
    src/font_ttf.cpp
    src/input_record.cpp
    src/keybinds.cpp
    src/limits.cpp
    src/node_editor.cpp
    src/svg.cpp
    src/texture.cpp
    src/themes.cpp
    src/themes_contrast.cpp
    src/themes_io.cpp
    src/undo.cpp
    src/vmem.cpp
)
if(WIN32)
    target_sources(strata PRIVATE src/platform_win32.cpp)
endif()
add_library(strata::strata ALIAS strata)

target_sources(strata PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/strata/backend/backend.hpp
        include/strata/backend/d3d11.hpp
        include/strata/backend/d3d12.hpp
        include/strata/backend/metal.hpp
        include/strata/backend/null.hpp
        include/strata/backend/opengl.hpp
        include/strata/backend/openvr.hpp
        include/strata/backend/openxr.hpp
        include/strata/backend/software.hpp
        include/strata/backend/vr_types.hpp
        include/strata/backend/vulkan.hpp
        include/strata/backend/webgpu.hpp
        include/strata/bidi.hpp
        include/strata/config.hpp
        include/strata/context.hpp
        include/strata/datetime.hpp
        include/strata/draw_list.hpp
        include/strata/font.hpp
        include/strata/frame.hpp
        include/strata/icons.hpp
        include/strata/input_record.hpp
        include/strata/keybinds.hpp
        include/strata/log.hpp
        include/strata/log_queue.hpp
        include/strata/node_editor.hpp
        include/strata/platform/glfw.hpp
        include/strata/platform/sdl3.hpp
        include/strata/platform/win32.hpp
        include/strata/strata.hpp
        include/strata/strata_c.h
        include/strata/svg.hpp
        include/strata/texture.hpp
        include/strata/themes.hpp
        include/strata/types.hpp
        include/strata/undo.hpp
        include/strata/vmem.hpp
)

target_sources(strata PRIVATE
    src/blur_plan.hpp
    src/chart_state.hpp
    src/child_card_state.hpp
    src/code_state.hpp
    src/color_picker_state.hpp
    src/combo_filter_state.hpp
    src/confirm_state.hpp
    src/context_helpers.hpp
    src/context_impl.hpp
    src/core/animation.hpp
    src/core/id_stack.hpp
    src/core/input_frame.hpp
    src/core/layout_state.hpp
    src/core/part_id.hpp
    src/core/style_stack.hpp
    src/core/window_registry.hpp
    src/date_picker_state.hpp
    src/dock_state.hpp
    src/drag_drop_state.hpp
    src/edit_state.hpp
    src/font_ttf.hpp
    src/hash.hpp
    src/hotkey_state.hpp
    src/inspector_state.hpp
    src/limits.hpp
    src/measure_cache_state.hpp
    src/menu_state.hpp
    src/modal_state.hpp
    src/nav_state.hpp
    src/popup_state.hpp
    src/rich_text_state.hpp
    src/row_accessory_state.hpp
    src/table_tree_state.hpp
    src/text_util.hpp
    src/toast_state.hpp
    src/widget_util.hpp
)

target_link_libraries(strata PRIVATE $<BUILD_INTERFACE:strata_options>)
if(NOT STRATA_DIAGNOSTICS)
    target_compile_definitions(strata PUBLIC STRATA_NO_DIAGNOSTICS=1)
endif()
if(STRATA_PCH)
    target_precompile_headers(strata PRIVATE <strata/context.hpp>) # nearly every src/ file includes it
endif()
# skipped for clang-cl: see the matching guard in cmake/strata_options.cmake
if(NOT (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    target_compile_features(strata PUBLIC cxx_std_23)
endif()
set_target_properties(strata PROPERTIES CXX_EXTENSIONS OFF)

# windows.h stays out of the public headers, but consumers still get the
# macros that keep their own includes lean.
if(WIN32)
    target_compile_definitions(strata PUBLIC NOMINMAX _HAS_EXCEPTIONS=0)
endif()
target_compile_definitions(strata PUBLIC
    $<$<BOOL:${STRATA_BUILD_DX11}>:STRATA_HAS_DX11=1>
    $<$<BOOL:${STRATA_BUILD_DX12}>:STRATA_HAS_DX12=1>
    $<$<BOOL:${STRATA_BUILD_OPENGL}>:STRATA_HAS_OPENGL=1>
    $<$<BOOL:${STRATA_BUILD_VULKAN}>:STRATA_HAS_VULKAN=1>
    $<$<BOOL:${STRATA_BUILD_WEBGPU}>:STRATA_HAS_WEBGPU=1>
    $<$<BOOL:${STRATA_BUILD_METAL}>:STRATA_HAS_METAL=1>
    $<$<BOOL:${STRATA_BUILD_OPENXR}>:STRATA_HAS_OPENXR=1>
    $<$<BOOL:${STRATA_BUILD_OPENVR}>:STRATA_HAS_OPENVR=1>
)

if(STRATA_BUILD_DX11 OR STRATA_BUILD_DX12)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY vs_main       PROFILE vs_5_0 SYMBOL g_strata_ui_vs)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_main       PROFILE ps_5_0 SYMBOL g_strata_ui_ps)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_image      PROFILE ps_5_0 SYMBOL g_strata_ui_ps_image)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_backdrop   PROFILE ps_5_0 SYMBOL g_strata_ui_ps_backdrop)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY vs_fullscreen PROFILE vs_5_0 SYMBOL g_strata_ui_vs_fullscreen)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_blur_down  PROFILE ps_5_0 SYMBOL g_strata_ui_ps_blur_down)
    strata_embed_hlsl(strata SOURCE shaders/ui.hlsl ENTRY ps_blur_gauss PROFILE ps_5_0 SYMBOL g_strata_ui_ps_blur_gauss)
endif()

if(STRATA_BUILD_DX11)
    target_sources(strata PRIVATE src/backend_d3d11.cpp)
    target_link_libraries(strata PRIVATE d3d11.lib dxgi.lib)
    if(MSVC)
        # load d3d11.dll only when a d3d11 renderer is actually created
        target_link_options(strata INTERFACE /DELAYLOAD:d3d11.dll)
        target_link_libraries(strata INTERFACE delayimp.lib)
    endif()
endif()

if(STRATA_BUILD_DX12)
    target_sources(strata PRIVATE src/backend_d3d12.cpp)
    target_link_libraries(strata PRIVATE d3d12.lib dxgi.lib)
    if(MSVC)
        target_link_options(strata INTERFACE /DELAYLOAD:d3d12.dll)
        target_link_libraries(strata INTERFACE delayimp.lib)
    endif()
endif()

if(STRATA_BUILD_OPENGL)
    target_sources(strata PRIVATE src/backend_opengl.cpp)
endif()

if(STRATA_BUILD_WEBGPU)
    target_sources(strata PRIVATE src/backend_webgpu.cpp)
    if(EMSCRIPTEN)
        target_compile_options(strata PUBLIC --use-port=emdawnwebgpu)
        target_link_options(strata PUBLIC --use-port=emdawnwebgpu)
    endif()
endif()

if(STRATA_BUILD_METAL)
    enable_language(OBJCXX)
    target_sources(strata PRIVATE src/backend_metal.mm)
    set_source_files_properties(src/backend_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(strata PRIVATE "-framework Metal" "-framework Foundation")
endif()

if(STRATA_BUILD_VULKAN)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE vert DEFINE STAGE_VS_MAIN       SYMBOL g_strata_vk_vs)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE frag DEFINE STAGE_PS_MAIN       SYMBOL g_strata_vk_ps)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE frag DEFINE STAGE_PS_IMAGE      SYMBOL g_strata_vk_ps_image)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE frag DEFINE STAGE_PS_BACKDROP   SYMBOL g_strata_vk_ps_backdrop)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE vert DEFINE STAGE_VS_FULLSCREEN SYMBOL g_strata_vk_vs_fullscreen)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE frag DEFINE STAGE_PS_BLUR_DOWN  SYMBOL g_strata_vk_ps_blur_down)
    strata_embed_spirv(strata SOURCE shaders/ui_vk.glsl STAGE frag DEFINE STAGE_PS_BLUR_GAUSS SYMBOL g_strata_vk_ps_blur_gauss)
    target_sources(strata PRIVATE src/backend_vulkan.cpp)
    target_link_libraries(strata PUBLIC Vulkan::Headers PRIVATE Vulkan::Vulkan)
    if(MSVC)
        target_link_options(strata INTERFACE /DELAYLOAD:vulkan-1.dll)
        target_link_libraries(strata INTERFACE delayimp.lib)
    endif()
endif()

if(STRATA_BUILD_OPENXR)
    target_sources(strata PRIVATE src/backend_openxr.cpp)
    target_link_libraries(strata PRIVATE openxr_loader)
    if(MSVC)
        # delay-loaded so injected DLLs do not load openxr_loader.dll unless OpenXR is used
        target_link_options(strata INTERFACE /DELAYLOAD:openxr_loader.dll)
        target_link_libraries(strata INTERFACE delayimp.lib)
    endif()
endif()

if(STRATA_BUILD_OPENVR)
    target_sources(strata PRIVATE src/backend_openvr.cpp)
    target_link_libraries(strata PRIVATE openvr_api)
    if(MSVC)
        target_link_options(strata INTERFACE /DELAYLOAD:openvr_api.dll)
        target_link_libraries(strata INTERFACE delayimp.lib)
    endif()
endif()

if(WIN32)
    target_link_libraries(strata PRIVATE gdi32.lib user32.lib advapi32.lib imm32.lib shell32.lib)
endif()

# strata::app: ready-made Win32 window + D3D11 / OpenGL / Vulkan host around the library
if(WIN32 AND (STRATA_BUILD_DX11 OR STRATA_BUILD_OPENGL OR STRATA_BUILD_VULKAN))
    add_library(strata_app STATIC src/app_win32.cpp)
    add_library(strata::app ALIAS strata_app)
    target_sources(strata_app PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/strata/app.hpp)
    target_link_libraries(strata_app PUBLIC strata::strata PRIVATE $<BUILD_INTERFACE:strata_options> gdi32.lib user32.lib)
    if(STRATA_BUILD_DX11)
        target_link_libraries(strata_app PRIVATE d3d11.lib dxgi.lib)
    endif()
    if(STRATA_BUILD_OPENGL)
        target_link_libraries(strata_app PRIVATE opengl32.lib)
    endif()
    if(STRATA_BUILD_VULKAN)
        target_link_libraries(strata_app PRIVATE Vulkan::Vulkan)
    endif()
endif()

if(STRATA_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(strata_install_targets strata)
    if(TARGET strata_app)
        set_target_properties(strata_app PROPERTIES EXPORT_NAME app)
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

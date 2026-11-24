# Browser WebGPU sandbox (Emscripten / WebAssembly)

set(STRATA_WEB_FONT "" CACHE FILEPATH "Proportional TTF font embedded into the browser demo")
set(STRATA_WEB_MONO_FONT "" CACHE FILEPATH "Monospace TTF font embedded into the browser demo")

if(NOT STRATA_WEB_FONT)
    foreach(_cand
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
        "/usr/share/fonts/TTF/DejaVuSans.ttf"
        "/System/Library/Fonts/Supplemental/Arial.ttf"
    )
        if(EXISTS "${_cand}")
            set(STRATA_WEB_FONT "${_cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT STRATA_WEB_MONO_FONT)
    foreach(_cand
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
        "/System/Library/Fonts/Supplemental/Courier New.ttf"
    )
        if(EXISTS "${_cand}")
            set(STRATA_WEB_MONO_FONT "${_cand}")
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${STRATA_WEB_FONT}")
    message(FATAL_ERROR "STRATA_WEB_FONT not found; install fonts-dejavu-core or pass -DSTRATA_WEB_FONT=/path/to/font.ttf")
endif()

add_executable(strata_web_demo main.cpp)
target_link_libraries(strata_web_demo PRIVATE strata::strata $<BUILD_INTERFACE:strata_options>)
set_target_properties(strata_web_demo PROPERTIES
    OUTPUT_NAME "index"
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
)

target_link_options(strata_web_demo PRIVATE
    --shell-file "${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sENVIRONMENT=web
    --embed-file "${STRATA_WEB_FONT}@/font.ttf"
)
if(EXISTS "${STRATA_WEB_MONO_FONT}")
    target_link_options(strata_web_demo PRIVATE --embed-file "${STRATA_WEB_MONO_FONT}@/mono.ttf")
endif()

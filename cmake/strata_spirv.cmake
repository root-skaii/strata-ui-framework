# compiles GLSL to SPIR-V at build time (via glslc or glslangValidator) and embeds the
# 32-bit word stream as a header, so the Vulkan backend needs no shader compiler at runtime.

include_guard(GLOBAL)

find_program(STRATA_GLSLC glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
)
if(NOT STRATA_GLSLC)
    find_program(STRATA_GLSLANG glslangValidator
        HINTS
            "$ENV{VULKAN_SDK}/Bin"
            "$ENV{VULKAN_SDK}/bin"
    )
endif()

if(NOT STRATA_GLSLC AND NOT STRATA_GLSLANG)
    message(FATAL_ERROR "glslc or glslangValidator not found - install the Vulkan SDK or set STRATA_GLSLC")
endif()

if(STRATA_GLSLC)
    message(STATUS "strata: using glslc at ${STRATA_GLSLC}")
else()
    message(STATUS "strata: using glslangValidator at ${STRATA_GLSLANG}")
endif()

set(_strata_embed_spirv_script "${CMAKE_CURRENT_LIST_DIR}/embed_spirv.cmake")

# strata_embed_spirv(<target> SOURCE <file.glsl> STAGE <vert|frag> DEFINE <macro> SYMBOL <c_name>)
#   generates <binary_dir>/generated/<SYMBOL>.h exposing `inline constexpr std::uint32_t <SYMBOL>[]`
function(strata_embed_spirv target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE;STAGE;DEFINE;SYMBOL" "")

    cmake_path(ABSOLUTE_PATH arg_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(spv_file "${out_dir}/${arg_SYMBOL}.spv")
    set(out_file "${out_dir}/${arg_SYMBOL}.h")

    if(STRATA_GLSLC)
        set(_glslc_stage "vertex")
        if(arg_STAGE STREQUAL "frag")
            set(_glslc_stage "fragment")
        endif()
        set(_compile_cmd "${STRATA_GLSLC}" "-fshader-stage=${_glslc_stage}" "-D${arg_DEFINE}=1" -O --target-env=vulkan1.0 -o "${spv_file}" "${arg_SOURCE}")
    else()
        set(_compile_cmd "${STRATA_GLSLANG}" -V -S "${arg_STAGE}" "-D${arg_DEFINE}=1" -o "${spv_file}" "${arg_SOURCE}")
    endif()

    add_custom_command(
        OUTPUT "${out_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND ${_compile_cmd}
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${spv_file}" "-DOUTPUT=${out_file}" "-DSYMBOL=${arg_SYMBOL}" -P "${_strata_embed_spirv_script}"
        DEPENDS "${arg_SOURCE}" "${_strata_embed_spirv_script}"
        COMMENT "spirv ${arg_STAGE} ${arg_DEFINE} -> ${arg_SYMBOL}.h"
        VERBATIM
    )

    target_sources(${target} PRIVATE "${out_file}")
    target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()

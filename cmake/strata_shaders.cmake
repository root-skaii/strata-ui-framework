# compiles hlsl at build time with fxc and embeds the bytecode as a c array, so
# the library never needs d3dcompiler_47.dll (or any file) at runtime.

include_guard(GLOBAL)

find_program(STRATA_FXC fxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkBinPath}/x64"
)

if(NOT STRATA_FXC)
    file(GLOB _strata_fxc_candidates
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe"
    )
    list(SORT _strata_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
    if(_strata_fxc_candidates)
        list(GET _strata_fxc_candidates 0 _strata_fxc_best)
        set(STRATA_FXC "${_strata_fxc_best}" CACHE FILEPATH "path to fxc.exe" FORCE)
    endif()
endif()

if(NOT STRATA_FXC)
    message(FATAL_ERROR "fxc.exe not found - install the windows 10/11 sdk or set STRATA_FXC")
endif()
message(STATUS "strata: using fxc at ${STRATA_FXC}")

# strata_embed_hlsl(<target> SOURCE <file.hlsl> ENTRY <fn> PROFILE <vs_5_0|ps_5_0|...> SYMBOL <c_name>)
#   generates <binary_dir>/generated/<SYMBOL>.h exposing `const BYTE <SYMBOL>[]`
#   (include <windows.h> first) and adds it plus its include dir to <target>.
function(strata_embed_hlsl target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "SOURCE;ENTRY;PROFILE;SYMBOL" "")

    cmake_path(ABSOLUTE_PATH arg_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(out_file "${out_dir}/${arg_SYMBOL}.h")

    add_custom_command(
        OUTPUT "${out_file}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND "${STRATA_FXC}" /nologo /T ${arg_PROFILE} /E ${arg_ENTRY} /O3 /WX
                /Qstrip_reflect /Qstrip_debug /Vn ${arg_SYMBOL} /Fh "${out_file}" "${arg_SOURCE}"
        DEPENDS "${arg_SOURCE}"
        COMMENT "fxc ${arg_PROFILE} ${arg_ENTRY} -> ${arg_SYMBOL}.h"
        VERBATIM
    )

    target_sources(${target} PRIVATE "${out_file}")
    target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()

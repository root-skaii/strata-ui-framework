# converts a compiled SPIR-V binary into a C++ header containing
# `inline constexpr std::uint32_t <SYMBOL>[] = { ... };`.
# invoked at build time via `cmake -DINPUT=... -DOUTPUT=... -DSYMBOL=... -P embed_spirv.cmake`.

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hex_len)
math(EXPR _hex_rem "${_hex_len} % 8")
if(_hex_len EQUAL 0 OR NOT _hex_rem EQUAL 0)
    message(FATAL_ERROR "invalid SPIR-V binary '${INPUT}' (hex length ${_hex_len})")
endif()

# little-endian 32-bit words (SPIR-V on x64): b0b1b2b3 -> 0xb3b2b1b0u
set(_b "([0-9a-fA-F][0-9a-fA-F])")
string(REGEX REPLACE "${_b}${_b}${_b}${_b}" "0x\\4\\3\\2\\1u," _words "${_hex}")
# wrap lines every 8 words
set(_w "0x[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F]u,")
string(REGEX REPLACE "(${_w}${_w}${_w}${_w}${_w}${_w}${_w}${_w})" "    \\1\n" _body "${_words}")

file(WRITE "${OUTPUT}"
"#pragma once\n"
"#include <cstdint>\n\n"
"inline constexpr std::uint32_t ${SYMBOL}[] = {\n"
"${_body}\n"
"};\n"
)

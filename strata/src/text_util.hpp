#pragma once

// internal (not installed): utf-8 helpers shared by the text-editing code. byte offsets always sit on code point boundaries.

#include "strata/types.hpp"

#include <array>
#include <string_view>

namespace strata::internal {

// finds the persistent state slot of a widget, or takes a free / stale one (slots untouched for two frames are stale)
template <class T, std::size_t N>
[[nodiscard]] T* state_for(std::array<T, N>& slots, id key, u64 frame) noexcept
{
    T* spare = nullptr;
    for (T& t : slots) {
        if (t.key == key) {
            t.last_frame = frame;
            return &t;
        }
        if (spare == nullptr && (t.key == 0 || t.last_frame + 2 < frame)) {
            spare = &t;
        }
    }
    T* slot = spare != nullptr ? spare : &slots[0];
    *slot            = {};
    slot->key        = key;
    slot->last_frame = frame;
    return slot;
}

} // namespace strata::internal

namespace strata::text {

[[nodiscard]] inline bool is_continuation(char c) noexcept { return (static_cast<u8>(c) & 0xc0) == 0x80; }

[[nodiscard]] inline std::size_t prev_boundary(std::string_view s, std::size_t i) noexcept
{
    if (i == 0) { return 0; }
    --i;
    while (i > 0 && is_continuation(s[i])) { --i; }
    return i;
}

[[nodiscard]] inline std::size_t next_boundary(std::string_view s, std::size_t i) noexcept
{
    if (i >= s.size()) { return s.size(); }
    ++i;
    while (i < s.size() && is_continuation(s[i])) { ++i; }
    return i;
}

[[nodiscard]] inline bool is_word_char(char c) noexcept
{
    const auto u = static_cast<u8>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

[[nodiscard]] inline std::size_t word_start(std::string_view s, std::size_t i) noexcept
{
    while (i > 0 && is_word_char(s[prev_boundary(s, i)])) { i = prev_boundary(s, i); }
    return i;
}

[[nodiscard]] inline std::size_t word_end(std::string_view s, std::size_t i) noexcept
{
    while (i < s.size() && is_word_char(s[i])) { i = next_boundary(s, i); }
    return i;
}

// ctrl+left: skips separators, then the word before the caret. newlines stop the jump so lines stay distinct
[[nodiscard]] inline std::size_t prev_word(std::string_view s, std::size_t i) noexcept
{
    while (i > 0 && !is_word_char(s[prev_boundary(s, i)]) && s[prev_boundary(s, i)] != '\n') { i = prev_boundary(s, i); }
    if (i > 0 && s[prev_boundary(s, i)] == '\n' ) { return prev_boundary(s, i); }
    return word_start(s, i);
}

[[nodiscard]] inline std::size_t next_word(std::string_view s, std::size_t i) noexcept
{
    if (i < s.size() && s[i] == '\n') { return i + 1; }
    i = word_end(s, i);
    while (i < s.size() && !is_word_char(s[i]) && s[i] != '\n') { i = next_boundary(s, i); }
    return i;
}

[[nodiscard]] inline std::size_t count_codepoints(std::string_view s) noexcept
{
    std::size_t n = 0;
    for (const char c : s) { n += is_continuation(c) ? 0 : 1; }
    return n;
}

} // namespace strata::text

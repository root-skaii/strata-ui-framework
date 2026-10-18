#pragma once

// internal: utf-8 helpers for text editing. byte offsets always sit on code point boundaries.

#include "strata/types.hpp"

#include <span>
#include <string_view>

namespace strata::internal {

// a widget's persistent slot, or a free / stale one (untouched for two frames). all in use: slot 0 is taken over and
// `full` is set, for the caller to report
template <class T>
[[nodiscard]] T* state_for(std::span<T> slots, id key, u64 frame, bool& full) noexcept
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
    full    = spare == nullptr;
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

// ctrl+left: skips separators, then the word before the caret; stops at newlines
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

// what a double click at `idx` selects: the word, else the run of spaces or of punctuation, else the line break
inline void word_bounds(std::string_view s, std::size_t idx, std::size_t& first, std::size_t& last) noexcept
{
    idx = std::min(idx, s.size());
    if (s.empty()) { first = last = 0; return; }
    const auto kind = [&](std::size_t i) { // 1 word, 2 blank, 3 line break, 4 punctuation
        const char c = s[i];
        return is_word_char(c) ? 1 : (c == ' ' || c == '\t') ? 2 : c == '\n' ? 3 : 4;
    };
    // the click lands between two characters: a word on either side wins, then whatever is to the right
    std::size_t at = idx;
    if (idx >= s.size() || kind(idx) != 1) {
        const std::size_t before = idx > 0 ? prev_boundary(s, idx) : 0;
        if (idx > 0 && kind(before) == 1) { at = before; }
        else if (idx >= s.size() || kind(idx) == 3) { at = idx > 0 ? before : 0; }
    }
    const int k = kind(at);
    first = at;
    while (first > 0 && kind(prev_boundary(s, first)) == k && k != 3) { first = prev_boundary(s, first); }
    last = next_boundary(s, at);
    while (k != 3 && last < s.size() && kind(last) == k) { last = next_boundary(s, last); }
}

// a drag that began with a double or triple click grows the selection by whole words or lines: [from, to) is what the
// press selected, `idx` the pointer; sets the new anchor and cursor
inline void extend_unit_selection(std::string_view s, u32 clicks, std::size_t from, std::size_t to, std::size_t idx,
                                  std::size_t& anchor, std::size_t& cursor) noexcept
{
    const auto lo = [&](std::size_t i) {
        if (clicks >= 3) { return i == 0 ? std::size_t{0} : (s.substr(0, i).rfind('\n') == std::string_view::npos ? 0 : s.substr(0, i).rfind('\n') + 1); }
        std::size_t a = 0, b = 0;
        word_bounds(s, i, a, b);
        return a;
    };
    const auto hi = [&](std::size_t i) {
        if (clicks >= 3) { const auto nl = s.find('\n', i); return nl == std::string_view::npos ? s.size() : nl + 1; }
        std::size_t a = 0, b = 0;
        word_bounds(s, i, a, b);
        return b;
    };
    if (idx >= to)        { anchor = from; cursor = std::max(to, hi(idx)); }
    else if (idx < from)  { anchor = to;   cursor = std::min(from, lo(idx)); }
    else                  { anchor = from; cursor = to; }
}

[[nodiscard]] inline std::size_t count_codepoints(std::string_view s) noexcept
{
    std::size_t n = 0;
    for (const char c : s) { n += is_continuation(c) ? 0 : 1; }
    return n;
}

} // namespace strata::text

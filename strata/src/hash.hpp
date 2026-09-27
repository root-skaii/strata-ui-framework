#pragma once

// internal (not installed): a fast 64-bit hash of a byte range, for "is this frame's geometry the same as the last one".
// it is XXH64: four independent lanes of multiply-rotate rounds over 32-byte stripes, so the cpu is never waiting on one
// long chain of multiplies. fnv-1a, which this replaces, costs about 4 cycles per byte (each byte waits for the previous
// multiply): a megabyte of vertices took a millisecond of end_frame.

#include "strata/types.hpp"

#include <bit>
#include <cstring>

namespace strata::internal {

namespace xxh {
inline constexpr u64 p1 = 11400714785074694791ull;
inline constexpr u64 p2 = 14029467366897019727ull;
inline constexpr u64 p3 = 1609587929392839161ull;
inline constexpr u64 p4 = 9650029242287828579ull;
inline constexpr u64 p5 = 2870177450012600261ull;

[[nodiscard]] inline u64 read64(const u8* p) noexcept
{
    u64 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
[[nodiscard]] inline u32 read32(const u8* p) noexcept
{
    u32 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
[[nodiscard]] inline u64 round(u64 acc, u64 input) noexcept
{
    acc += input * p2;
    acc = std::rotl(acc, 31);
    return acc * p1;
}
[[nodiscard]] inline u64 merge(u64 acc, u64 lane) noexcept
{
    acc ^= round(0, lane);
    return acc * p1 + p4;
}
} // namespace xxh

[[nodiscard]] inline u64 hash_bytes(const void* data, std::size_t size, u64 seed = 0) noexcept
{
    using namespace xxh;
    const u8* p   = static_cast<const u8*>(data);
    const u8* end = p + size;
    u64 h;
    if (size >= 32) {
        u64 v1 = seed + p1 + p2;
        u64 v2 = seed + p2;
        u64 v3 = seed;
        u64 v4 = seed - p1;
        const u8* limit = end - 32;
        do {
            v1 = round(v1, read64(p));
            v2 = round(v2, read64(p + 8));
            v3 = round(v3, read64(p + 16));
            v4 = round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = merge(h, v1);
        h = merge(h, v2);
        h = merge(h, v3);
        h = merge(h, v4);
    } else {
        h = seed + p5;
    }
    h += static_cast<u64>(size);
    for (; p + 8 <= end; p += 8) {
        h ^= round(0, read64(p));
        h = std::rotl(h, 27) * p1 + p4;
    }
    if (p + 4 <= end) {
        h ^= static_cast<u64>(read32(p)) * p1;
        h = std::rotl(h, 23) * p2 + p3;
        p += 4;
    }
    for (; p < end; ++p) {
        h ^= static_cast<u64>(*p) * p5;
        h = std::rotl(h, 11) * p1;
    }
    h ^= h >> 33;
    h *= p2;
    h ^= h >> 29;
    h *= p3;
    h ^= h >> 32;
    return h;
}

} // namespace strata::internal

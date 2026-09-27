#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace strata {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

struct vec2 {
    f32 x{};
    f32 y{};

    [[nodiscard]] friend constexpr bool operator==(vec2, vec2) noexcept = default;

    [[nodiscard]] friend constexpr vec2 operator+(vec2 a, vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    [[nodiscard]] friend constexpr vec2 operator-(vec2 a, vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    [[nodiscard]] friend constexpr vec2 operator*(vec2 a, f32 s) noexcept { return {a.x * s, a.y * s}; }
    [[nodiscard]] friend constexpr vec2 operator*(f32 s, vec2 a) noexcept { return a * s; }
    [[nodiscard]] friend constexpr vec2 operator-(vec2 a) noexcept { return {-a.x, -a.y}; }

    constexpr vec2& operator+=(vec2 o) noexcept { x += o.x; y += o.y; return *this; }
    constexpr vec2& operator-=(vec2 o) noexcept { x -= o.x; y -= o.y; return *this; }
};

[[nodiscard]] constexpr f32 dot(vec2 a, vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] inline vec2 round(vec2 v) noexcept { return {std::round(v.x), std::round(v.y)}; }

struct rect {
    vec2 min{};
    vec2 max{};

    [[nodiscard]] friend constexpr bool operator==(const rect&, const rect&) noexcept = default;

    [[nodiscard]] static constexpr rect from_size(vec2 pos, vec2 size) noexcept { return {pos, pos + size}; }

    [[nodiscard]] constexpr f32  width() const noexcept  { return max.x - min.x; }
    [[nodiscard]] constexpr f32  height() const noexcept { return max.y - min.y; }
    [[nodiscard]] constexpr vec2 size() const noexcept   { return max - min; }
    [[nodiscard]] constexpr vec2 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] constexpr bool empty() const noexcept  { return max.x <= min.x || max.y <= min.y; }

    [[nodiscard]] constexpr bool contains(vec2 p) const noexcept
    {
        return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y;
    }

    [[nodiscard]] constexpr bool overlaps(const rect& o) const noexcept
    {
        return o.min.x < max.x && o.max.x > min.x && o.min.y < max.y && o.max.y > min.y;
    }

    [[nodiscard]] constexpr rect expanded(f32 v) const noexcept
    {
        return {{min.x - v, min.y - v}, {max.x + v, max.y + v}};
    }

    [[nodiscard]] constexpr rect intersect(const rect& o) const noexcept
    {
        return {{std::max(min.x, o.min.x), std::max(min.y, o.min.y)},
                {std::min(max.x, o.max.x), std::min(max.y, o.max.y)}};
    }
};

// straight-alpha rgba8, same bytes as DXGI_FORMAT_R8G8B8A8_UNORM
struct color {
    u8 r{};
    u8 g{};
    u8 b{};
    u8 a{255};

    [[nodiscard]] friend constexpr bool operator==(color, color) noexcept = default;

    // 0xrrggbbaa
    [[nodiscard]] static constexpr color from_hex(u32 rgba) noexcept
    {
        return {static_cast<u8>(rgba >> 24), static_cast<u8>(rgba >> 16),
                static_cast<u8>(rgba >> 8), static_cast<u8>(rgba)};
    }

    [[nodiscard]] constexpr color scaled_alpha(f32 k) const noexcept
    {
        return {r, g, b, static_cast<u8>(static_cast<f32>(a) * std::clamp(k, 0.0f, 1.0f) + 0.5f)};
    }
};

[[nodiscard]] constexpr color lerp(color a, color b, f32 t) noexcept
{
    const auto mix = [t](u8 x, u8 y) {
        return static_cast<u8>(static_cast<f32>(x) + (static_cast<f32>(y) - static_cast<f32>(x)) * t + 0.5f);
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

namespace literals {
[[nodiscard]] consteval color operator""_rgba(unsigned long long hex)
{
    return color::from_hex(static_cast<u32>(hex));
}
} // namespace literals

// widget identity; 0 = none. 64 bits because 32 would give a big tree even odds of a collision past ~77k nodes
using id = u64;

// renderer-owned texture (create_texture); 0 = none / the font atlas
using texture_id = u32;

// fnv-1a 64 continuing from `seed` (the scope's id). "text###key" hashes only "###key", so a window titled
// "Downloads (3)###downloads" keeps its state when the count changes
[[nodiscard]] constexpr id hash_id(std::string_view s, id seed = 0) noexcept
{
    if (const auto at = s.find("###"); at != std::string_view::npos) {
        s.remove_prefix(at);
    }
    u64 h = seed != 0 ? seed : 0xcbf29ce484222325ull;
    for (const char c : s) {
        h = (h ^ static_cast<u8>(c)) * 0x100000001b3ull;
    }
    return h != 0 ? h : 1u;
}

// "label##hidden" shows "label", hashes all; "label###key" shows "label", hashes "###key"
[[nodiscard]] constexpr std::string_view visible_label(std::string_view s) noexcept
{
    const auto pos = s.find("##");
    return pos == std::string_view::npos ? s : s.substr(0, pos);
}

} // namespace strata

#pragma once

// texture descriptions and cpu-side image prep (format conversion, mip generation), used by the renderers and
// available to custom ones.

#include "strata/types.hpp"

#include <span>
#include <vector>

namespace strata {

// input pixel layout (rows tightly packed)
enum class texture_format : u8 {
    rgba8,   // 4 bytes per pixel, straight alpha
    bgra8,   // 4 bytes, blue first (GDI / most decoders), straight alpha
    r8,      // 1 byte: a grey level shown as opaque grey
    a8,      // 1 byte coverage, drawn white with that alpha (`tint` colours it)
    rgba16f, // 8 bytes, half floats (0..1 displayed, clamped), straight alpha
};

// gpu storage: r8 and a8 are expanded to rgba8 on the cpu
enum class texture_layout : u8 { rgba8, bgra8, rgba16f };

struct texture_desc {
    u32            width{};
    u32            height{};
    texture_format format{texture_format::rgba8};
    // 1: no mips; 0: full chain; n: n levels. alpha-weighted 2x2 box filter on the cpu; the shader picks the level
    u32            mip_levels{1};
    // keep a cpu copy for update_texture() (costs the memory again); otherwise wiped after upload
    bool           updatable{false};
};

[[nodiscard]] constexpr u32 texture_source_bytes(texture_format f) noexcept
{
    switch (f) {
    case texture_format::r8:
    case texture_format::a8:      return 1;
    case texture_format::rgba16f: return 8;
    default:                      return 4;
    }
}

[[nodiscard]] constexpr texture_layout texture_layout_of(texture_format f) noexcept
{
    switch (f) {
    case texture_format::bgra8:   return texture_layout::bgra8;
    case texture_format::rgba16f: return texture_layout::rgba16f;
    default:                      return texture_layout::rgba8;
    }
}

[[nodiscard]] constexpr u32 texture_layout_bytes(texture_layout l) noexcept
{
    return l == texture_layout::rgba16f ? 8u : 4u;
}

// level count for `requested` (0 = all) at this size
[[nodiscard]] u32 texture_mip_count(u32 width, u32 height, u32 requested) noexcept;

// texture pixels in gpu layout, all mip levels
class texture_image {
public:
    struct region { // a rectangle of one level that changed
        u32 level{};
        u32 x{}, y{}, w{}, h{};
    };

    // converts `pixels` (desc.format, tightly packed) and builds mips; false for an empty size or too few bytes
    [[nodiscard]] bool create(const texture_desc& desc, std::span<const u8> pixels);
    // replaces a rectangle of level 0 (texture format, tightly packed) and rebuilds the affected mips; `dirty` gets one
    // region per level, level 0 first. false if out of bounds
    [[nodiscard]] bool update(u32 x, u32 y, u32 w, u32 h, std::span<const u8> pixels, std::vector<region>& dirty);
    // zeroes and frees the pixels
    void wipe() noexcept;

    [[nodiscard]] bool           valid() const noexcept { return !levels_.empty(); }
    [[nodiscard]] texture_format format() const noexcept { return format_; }
    [[nodiscard]] texture_layout layout() const noexcept { return texture_layout_of(format_); }
    [[nodiscard]] u32            level_count() const noexcept { return static_cast<u32>(levels_.size()); }
    [[nodiscard]] u32            width(u32 level = 0) const noexcept { return levels_[level].w; }
    [[nodiscard]] u32            height(u32 level = 0) const noexcept { return levels_[level].h; }
    [[nodiscard]] u32            pitch(u32 level = 0) const noexcept { return levels_[level].w * texture_layout_bytes(layout()); }
    [[nodiscard]] std::span<const u8> pixels(u32 level = 0) const noexcept { return levels_[level].data; }

private:
    struct level {
        u32             w{};
        u32             h{};
        std::vector<u8> data;
    };
    void build_level(u32 k, u32 x0, u32 y0, u32 x1, u32 y1); // level k from k-1 over [x0, x1) x [y0, y1)

    texture_format     format_{};
    std::vector<level> levels_;
};

// half float <-> float (rgba16f)
[[nodiscard]] f32 half_to_float(u16 h) noexcept;
[[nodiscard]] u16 float_to_half(f32 f) noexcept;

} // namespace strata

#pragma once

// internal: zero-dependency TrueType (sfnt / ttc) outline parser and analytical scanline rasteriser.
// used directly on non-Windows platforms and when font_rasterizer::truetype is requested on Windows.

#include "strata/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace strata::internal {

using i8  = std::int8_t;
using i16 = std::int16_t;

struct ttf_bitmap {
    u16             w{};
    u16             h{};
    i32             origin_x{}; // left bearing in pixels from pen x
    i32             origin_y{}; // top bearing in pixels above baseline (+y up, matching GDI gmptGlyphOrigin.y)
    f32             advance{};  // horizontal advance in physical pixels
    std::vector<u8> pixels;     // w * h coverage (0..255)
};

struct ttf_kern_pair {
    u32 key{};    // left_cp << 16 | right_cp (BMP)
    f32 amount{}; // physical pixels
};

class ttf_font {
public:
    // parses a TTF/TTC buffer (copies or borrows nothing: caller keeps `bytes` alive via owned storage if needed).
    // `bytes` must stay valid for the lifetime of this ttf_font.
    [[nodiscard]] bool init(std::span<const u8> bytes, u32 face_index = 0) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] f32  scale_for_pixel_height(f32 pixel_height_px) const noexcept;
    [[nodiscard]] f32  ascent_px(f32 scale) const noexcept { return static_cast<f32>(ascent_) * scale; }
    [[nodiscard]] f32  descent_px(f32 scale) const noexcept { return static_cast<f32>(descent_) * scale; }
    [[nodiscard]] f32  line_height_px(f32 scale) const noexcept
    {
        return static_cast<f32>(ascent_ - descent_ + line_gap_) * scale;
    }

    // maps a unicode code point to a glyph index (0 = missing / .notdef)
    [[nodiscard]] u16 glyph_index(char32_t cp) const noexcept;

    // measures + rasterises `gid` at `scale`; returns false only on malformed glyph data
    [[nodiscard]] bool rasterise(u16 gid, f32 scale, ttf_bitmap& out, bool need_pixels = true) const;

    // extracts scaled BMP kerning pairs from the `kern` format-0 table (if present)
    void collect_kerning(f32 scale, std::vector<ttf_kern_pair>& out) const;

private:
    struct edge {
        f32 x0{}, y0{}, x1{}, y1{};
    };

    [[nodiscard]] bool glyph_offset(u16 gid, std::size_t& start, std::size_t& end) const noexcept;
    [[nodiscard]] bool load_outline(u16 gid, f32 m00, f32 m01, f32 m10, f32 m11, f32 tx, f32 ty,
                                    std::vector<edge>& edges, int depth) const;
    [[nodiscard]] f32  advance_units(u16 gid) const noexcept;

    std::span<const u8> data_;
    std::size_t         cmap_fmt4_{};
    std::size_t         cmap_fmt12_{};
    std::size_t         glyf_{};
    std::size_t         glyf_len_{};
    std::size_t         loca_{};
    std::size_t         loca_len_{};
    std::size_t         hmtx_{};
    std::size_t         hmtx_len_{};
    std::size_t         kern_{};
    std::size_t         kern_len_{};
    u16                 units_per_em_{};
    u16                 num_glyphs_{};
    u16                 num_hmetrics_{};
    i16                 ascent_{};
    i16                 descent_{};
    i16                 line_gap_{};
    bool                loca_long_{};
    bool                valid_{};
};

} // namespace strata::internal

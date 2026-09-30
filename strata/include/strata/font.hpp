#pragma once

#include "strata/types.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace strata {

struct glyph {
    // quad relative to the pen at the top of the line
    f32  x0{}, y0{}, x1{}, y1{};
    f32  advance{};
    // atlas coordinates in 1/32767 units (bit 15 = shape flag)
    u16  u0{}, v0{}, u1{}, v1{};
    bool visible{}; // has pixels (false for whitespace)
    bool present{}; // the font has this code point at all
};

enum class font_error : u8 {
    device_failed,
    font_unavailable, // face not found / gdi refused it
    file_unreadable,
    font_invalid,     // not a parsable ttf / otf / ttc
    atlas_full,       // glyph set does not fit max_atlas_size
    no_fonts,
};

struct codepoint_range {
    char32_t first{};
    char32_t last{};
};

// unicode blocks to bake, any plane. cjk / hangul need a font with them and grow the atlas a lot (cjk_unified ~21k).
namespace glyph_ranges {
inline constexpr codepoint_range latin{0x0020, 0x024f};          // ascii, latin-1, latin extended-a/b
inline constexpr codepoint_range greek{0x0370, 0x03ff};
inline constexpr codepoint_range cyrillic{0x0400, 0x052f};
inline constexpr codepoint_range punctuation{0x2000, 0x206f};
inline constexpr codepoint_range currency{0x20a0, 0x20cf};
inline constexpr codepoint_range arrows{0x2190, 0x21ff};
inline constexpr codepoint_range math_operators{0x2200, 0x22ff};
inline constexpr codepoint_range box_and_shapes{0x2500, 0x25ff};
inline constexpr codepoint_range cjk_symbols{0x3000, 0x303f};
inline constexpr codepoint_range hiragana{0x3040, 0x309f};
inline constexpr codepoint_range katakana{0x30a0, 0x30ff};
inline constexpr codepoint_range cjk_unified{0x4e00, 0x9fff};
inline constexpr codepoint_range hangul_syllables{0xac00, 0xd7af};
inline constexpr codepoint_range fullwidth_forms{0xff00, 0xffef};
inline constexpr codepoint_range private_use{0xe000, 0xf8ff};    // icon fonts live here
inline constexpr codepoint_range hebrew{0x0590, 0x05ff};
inline constexpr codepoint_range arabic{0x0600, 0x06ff};
inline constexpr codepoint_range arabic_supplement{0x0750, 0x077f};
inline constexpr codepoint_range arabic_forms_a{0xfb50, 0xfdff};  // presentation forms (the shaper's source)
inline constexpr codepoint_range arabic_forms_b{0xfe70, 0xfeff};
inline constexpr codepoint_range hebrew_forms{0xfb1d, 0xfb4f};
inline constexpr codepoint_range symbols{0x2600, 0x27bf};         // misc symbols and dingbats (BMP "emoji")
inline constexpr codepoint_range emoji{0x1f300, 0x1faff};         // pictographs, emoticons, transport, ... (~1500)
inline constexpr codepoint_range math_alphanumeric{0x1d400, 0x1d7ff};

inline constexpr std::array default_set{latin, greek, cyrillic, punctuation, currency};
} // namespace glyph_ranges

struct font_config {
    // installed font family; ignored when `file` or `data` is set.
    std::string_view face = "Segoe UI";
    // utf-8 path to a .ttf / .otf / .ttc, loaded privately for this process
    std::string_view file{};
    // or the bytes in memory; must outlive build()
    std::span<const u8> data{};

    f32  pixel_height = 14.0f; // logical pixels (baked at size * ui scale)
    bool bold         = false;
    // moves every glyph of this font down (+) or up (-) by this many logical pixels, to line a font up with another one
    // whose ink sits higher or lower in its line box (an icon font next to a pixel font)
    f32  y_offset     = 0.0f;
    bool kerning      = true; // apply kerning pairs (integer pixels)
    // fail with font_unavailable instead of letting gdi substitute a missing `face`
    bool exact_face   = false;
    // fallback faces for missing glyphs, in order: {"Segoe UI Emoji", "Segoe UI Symbol"}. same size and baseline;
    // single-color only
    std::span<const std::string_view> fallback_faces{};

    std::span<const codepoint_range> ranges = glyph_ranges::default_set;
};

// font index inside a font_atlas, in build() order
using font_id = u32;

// decodes one utf-8 code point and advances; malformed input yields U+FFFD
[[nodiscard]] constexpr char32_t decode_utf8(std::string_view& s) noexcept
{
    const auto b0 = static_cast<u8>(s.front());
    std::size_t len{};
    char32_t    cp{};
    if (b0 < 0x80)              { len = 1; cp = b0; }
    else if ((b0 >> 5) == 0x06) { len = 2; cp = b0 & 0x1fu; }
    else if ((b0 >> 4) == 0x0e) { len = 3; cp = b0 & 0x0fu; }
    else if ((b0 >> 3) == 0x1e) { len = 4; cp = b0 & 0x07u; }
    else { s.remove_prefix(1); return 0xfffd; }

    if (len > s.size()) { s.remove_prefix(s.size()); return 0xfffd; }
    for (std::size_t i = 1; i < len; ++i) {
        const auto b = static_cast<u8>(s[i]);
        if ((b & 0xc0) != 0x80) { s.remove_prefix(1); return 0xfffd; }
        cp = (cp << 6) | (b & 0x3fu);
    }
    s.remove_prefix(len);
    return cp;
}

// one 8-bit coverage texture for all fonts (one pipeline state), rasterised through gdi. missing code points draw
// '?', else U+FFFD, else the first existing glyph.
// measure, line_height, ascent, kerning and advance are logical; *_px and find() quads are physical (logical * scale()).
class font_atlas {
public:
    font_atlas() = default;
    ~font_atlas();
    font_atlas(const font_atlas&)            = default;
    font_atlas& operator=(const font_atlas&) = default;
    font_atlas(font_atlas&&) noexcept            = default;
    font_atlas& operator=(font_atlas&&) noexcept = default;

    // the atlas grows from 256x256 up to `max_atlas_size` (power of two, <= 16384).
    // `scale` multiplies every font's pixel height (dpi scaling)
    [[nodiscard]] static std::expected<font_atlas, font_error> build(std::span<const font_config> fonts,
                                                                      u32 max_atlas_size = 4096, f32 scale = 1.0f);
    [[nodiscard]] static std::expected<font_atlas, font_error> build(const font_config& font = {},
                                                                      u32 max_atlas_size = 4096, f32 scale = 1.0f)
    {
        return build(std::span<const font_config>{&font, 1}, max_atlas_size, scale);
    }

    // physical pixels per logical pixel baked for
    [[nodiscard]] f32 scale() const noexcept { return scale_; }

    [[nodiscard]] std::size_t font_count() const noexcept { return fonts_.size(); }

    [[nodiscard]] const glyph& find(font_id f, char32_t cp) const noexcept
    {
        const font_data& fd = fonts_[f];
        // hot path: the lowest range is normally latin
        if (!fd.ranges.empty()) {
            const range_entry& r0 = fd.ranges.front();
            if (cp >= r0.first && cp <= r0.last) {
                const glyph& g = fd.glyphs[r0.offset + (cp - r0.first)];
                return g.present ? g : fd.glyphs[fd.fallback];
            }
        }
        return lookup_slow(fd, cp);
    }
    [[nodiscard]] const glyph& find(char32_t cp) const noexcept { return find(0, cp); }
    // the font really has this glyph (find() falls back otherwise)
    [[nodiscard]] bool has_glyph(font_id f, char32_t cp) const noexcept;

    // pen adjustment (usually <= 0) between two code points; _px is physical
    [[nodiscard]] f32 kerning_px(font_id f, char32_t left, char32_t right) const noexcept
    {
        const font_data& fd = fonts_[f];
        if (fd.kern.empty() || left > 0xffff || right > 0xffff) {
            return 0.0f;
        }
        if (left < 128 && right < 128) { // common case: one table read
            const std::int8_t k = fd.kern_ascii[left * 128 + right];
            if (k != kern_ask) { return static_cast<f32>(k); }
        }
        return kerning_slow(fd, (static_cast<u32>(left) << 16) | static_cast<u32>(right));
    }
    [[nodiscard]] f32 kerning(font_id f, char32_t left, char32_t right) const noexcept { return kerning_px(f, left, right) * inv_scale_; }
    [[nodiscard]] f32 kerning(char32_t left, char32_t right) const noexcept { return kerning(0, left, right); }
    // pen advance after a code point, logical
    [[nodiscard]] f32 advance(font_id f, char32_t cp) const noexcept { return find(f, cp).advance * inv_scale_; }
    [[nodiscard]] std::size_t kerning_pair_count(font_id f = 0) const noexcept { return fonts_[f].kern.size(); }

    // widest line and total height (logical); _px is physical
    [[nodiscard]] vec2 measure_px(font_id f, std::string_view text) const noexcept;
    [[nodiscard]] vec2 measure(font_id f, std::string_view text) const noexcept { return measure_px(f, text) * inv_scale_; }
    [[nodiscard]] vec2 measure(std::string_view text) const noexcept { return measure(0, text); }

    [[nodiscard]] f32 line_height_px(font_id f = 0) const noexcept { return fonts_[f].line_height; }
    [[nodiscard]] f32 line_height(font_id f = 0) const noexcept { return fonts_[f].line_height * inv_scale_; }
    // top of line to baseline; aligns runs of different fonts
    [[nodiscard]] f32 ascent_px(font_id f = 0) const noexcept { return fonts_[f].ascent; }
    [[nodiscard]] f32 ascent(font_id f = 0) const noexcept { return fonts_[f].ascent * inv_scale_; }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] std::size_t glyph_count(font_id f) const noexcept { return fonts_[f].glyphs.size(); }
    [[nodiscard]] std::size_t glyph_count() const noexcept;
    [[nodiscard]] std::span<const u8> pixels() const noexcept { return pixels_; }

    // frees the cpu bitmap; call after all renderers exist (the gpu keeps a copy). later renderers need a rebuilt atlas.
    // also done on destruction.
    void discard_pixels() noexcept;

    // centre of a 2x2 white block for untextured geometry
    [[nodiscard]] std::array<u16, 2> white_uv() const noexcept { return white_uv_; }

private:
    struct range_entry {
        char32_t    first{};
        char32_t    last{};
        std::size_t offset{}; // index of `first` in the font's glyph table
    };

    struct kern_pair {
        u32 key{}; // left << 16 | right (utf-16 code units)
        f32 amount{};
    };

    struct font_data {
        std::vector<range_entry> ranges; // sorted, disjoint
        std::vector<glyph>       glyphs;
        std::vector<kern_pair>   kern;   // sorted by key
        // ascii kerning pairs [left * 128 + right] in whole pixels; kern_ask = look in `kern`.
        // filled when `kern` is not empty
        std::vector<std::int8_t> kern_ascii;
        std::size_t              fallback{};
        f32                      line_height{};
        f32                      ascent{};
    };

    static constexpr std::int8_t kern_ask = -128;

    [[nodiscard]] static const glyph& lookup_slow(const font_data& fd, char32_t cp) noexcept;
    [[nodiscard]] static f32          kerning_slow(const font_data& fd, u32 key) noexcept;

    std::vector<font_data> fonts_;
    std::vector<u8>        pixels_;
    std::array<u16, 2>     white_uv_{};
    u32                    width_{};
    u32                    height_{};
    f32                    scale_{1.0f};
    f32                    inv_scale_{1.0f};
};

} // namespace strata

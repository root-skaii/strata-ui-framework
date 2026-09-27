#pragma once

// right-to-left text: compact Unicode bidi (UAX #9, one paragraph per line) plus Arabic joining. draw_list::text and
// font_atlas::measure apply it to rtl strings; text fields use bidi_layout for caret and mouse.
// not supported: Indic / Southeast Asian shaping, explicit embeddings / isolates (U+202A..202E, U+2066..2069 are
// ignored), paired brackets (N0). a mixed-direction selection is one rectangle.

#include "strata/font.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace strata {

enum class text_direction : u8 {
    automatic, // first strong character decides
    ltr,
    rtl,
};

// the text has an rtl character (Hebrew, Arabic, Syriac ...). cheap: a byte scan rejects latin / cjk
[[nodiscard]] bool has_rtl_text(std::string_view utf8) noexcept;

// the text in visual order: letters joined, runs reordered per line, brackets mirrored. `atlas` / `font` tell
// whether joined forms exist (bake glyph_ranges::arabic_forms_b); nullptr assumes they do. ltr text is returned as is.
[[nodiscard]] std::string to_visual(std::string_view utf8, const font_atlas* atlas = nullptr, font_id font = 0,
                                    text_direction direction = text_direction::automatic);

// reusable working buffers for per-frame callers: one per call site makes the steady state allocation-free.
struct visual_scratch {
    std::vector<char32_t> cps;    // the line, decoded
    std::vector<char32_t> shaped; // presentation forms actually drawn
    std::vector<u32>      origin; // source logical code point of each (a ligature: its first)
    std::vector<u8>       span;   // how many logical code points it stands for
    std::vector<u32>      order;  // visual position -> index into `shaped`
    std::vector<u8>       level;  // embedding level of each shaped code point
};

// to_visual() into a caller-owned string (cleared first), reusing `scratch`.
void to_visual_into(std::string& out, visual_scratch& scratch, std::string_view utf8,
                    const font_atlas* atlas = nullptr, font_id font = 0,
                    text_direction direction = text_direction::automatic);

// one line laid out for editing: byte offsets <-> caret positions
class bidi_layout {
public:
    void build(const font_atlas& atlas, font_id font, std::string_view utf8, text_direction direction = text_direction::automatic);

    [[nodiscard]] bool  rtl() const noexcept { return rtl_; }          // the line has right-to-left characters
    [[nodiscard]] f32   width() const noexcept { return width_; }
    // caret x at a byte offset (logical; mid-character offsets snap to its edge)
    [[nodiscard]] f32   caret_x(std::size_t byte_offset) const noexcept;
    // byte offset with the caret nearest to x
    [[nodiscard]] std::size_t index_at(f32 x) const noexcept;

private:
    bool                rtl_{};
    f32                 width_{};
    std::size_t         size_{};
    std::vector<u32>    byte_of_;     // per logical code point: byte offset; +1 entry: text size
    std::vector<u32>    glyph_of_;    // per logical code point: its visual glyph
    std::vector<u8>     second_;      // per logical code point: 1 if second half of a ligature
    std::vector<f32>    left_;        // per visual glyph: left edge and width
    std::vector<f32>    advance_;
    std::vector<u8>     level_;       // per visual glyph: odd = right to left
    std::vector<u32>    visual_pos_;  // per shaped code point: visual position (inverse of scratch_.order)
    visual_scratch      scratch_;     // build() runs every frame while editing: keep buffers
};

} // namespace strata

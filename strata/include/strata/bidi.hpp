#pragma once

// right-to-left text: a compact Unicode bidi (UAX #9, one paragraph per line) plus Arabic joining. draw_list::text and
// font_atlas::measure apply it automatically to strings with right-to-left characters; text fields use bidi_layout to
// place the caret and mouse.
//
// not supported: OpenType shaping for Indic / Southeast Asian scripts, explicit embeddings / isolates (U+202A..202E,
// U+2066..2069 are ignored), paired brackets (N0). a selection spanning several directions is one rectangle.

#include "strata/font.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace strata {

enum class text_direction : u8 {
    automatic, // the first strong character decides (a Hebrew / Arabic letter: right-to-left, otherwise left-to-right)
    ltr,
    rtl,
};

// true if the text has a right-to-left character (Hebrew, Arabic, Syriac ...). cheap: latin / cjk is rejected by a byte scan
[[nodiscard]] bool has_rtl_text(std::string_view utf8) noexcept;

// the text as drawn left to right: letters joined, runs reordered per line, brackets mirrored. `atlas` / `font` decide
// whether a joined form exists (letters stay unjoined without presentation forms: bake glyph_ranges::arabic_forms_b);
// nullptr assumes it does. text without right-to-left characters is returned as is.
[[nodiscard]] std::string to_visual(std::string_view utf8, const font_atlas* atlas = nullptr, font_id font = 0,
                                    text_direction direction = text_direction::automatic);

// one line laid out for editing: maps byte offsets of the logical text to caret positions and back
class bidi_layout {
public:
    void build(const font_atlas& atlas, font_id font, std::string_view utf8, text_direction direction = text_direction::automatic);

    [[nodiscard]] bool  rtl() const noexcept { return rtl_; }          // the line has right-to-left characters
    [[nodiscard]] f32   width() const noexcept { return width_; }
    // x of the caret at a byte offset (logical order; offsets inside a character snap to its edge)
    [[nodiscard]] f32   caret_x(std::size_t byte_offset) const noexcept;
    // the byte offset whose caret is nearest to x
    [[nodiscard]] std::size_t index_at(f32 x) const noexcept;

private:
    bool                rtl_{};
    f32                 width_{};
    std::size_t         size_{};
    std::vector<u32>    byte_of_;     // per logical code point: its byte offset; one more entry: the text size
    std::vector<u32>    glyph_of_;    // per logical code point: the visual glyph that shows it
    std::vector<u8>     second_;      // per logical code point: 1 if it is the second half of a ligature
    std::vector<f32>    left_;        // per visual glyph: its left edge, and its width
    std::vector<f32>    advance_;
    std::vector<u8>     level_;       // per visual glyph: odd = right to left
};

} // namespace strata

#pragma once

#include "strata/types.hpp"

#include <array>
#include <string_view>

namespace strata {

// a single code point encoded as utf-8 at compile time, convertible to string_view.
// keep the object alive while a view of it is in use (the constants below all have static storage).
class glyph_string {
public:
    constexpr glyph_string(char32_t cp) noexcept
    {
        if (cp < 0x80) {
            buf_[0] = static_cast<char>(cp);
            len_    = 1;
        } else if (cp < 0x800) {
            buf_[0] = static_cast<char>(0xc0 | (cp >> 6));
            buf_[1] = static_cast<char>(0x80 | (cp & 0x3f));
            len_    = 2;
        } else if (cp < 0x10000) {
            buf_[0] = static_cast<char>(0xe0 | (cp >> 12));
            buf_[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            buf_[2] = static_cast<char>(0x80 | (cp & 0x3f));
            len_    = 3;
        } else {
            buf_[0] = static_cast<char>(0xf0 | (cp >> 18));
            buf_[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            buf_[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            buf_[3] = static_cast<char>(0x80 | (cp & 0x3f));
            len_    = 4;
        }
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept { return {buf_.data(), len_}; }

private:
    std::array<char, 4> buf_{};
    u8                  len_{};
};

// code points of "Segoe MDL2 Assets" (Windows 10) / "Segoe Fluent Icons" (Windows 11),
// which share them. bake glyph_ranges::private_use for an icon font and select it
// with ui.with_font(icon_font) or the icon_* widgets.
namespace icons {
inline constexpr glyph_string home{0xe80f};
inline constexpr glyph_string settings{0xe713};
inline constexpr glyph_string save{0xe74e};
inline constexpr glyph_string trash{0xe74d};
inline constexpr glyph_string search{0xe721};
inline constexpr glyph_string accept{0xe73e};
inline constexpr glyph_string cancel{0xe711};
inline constexpr glyph_string add{0xe710};
inline constexpr glyph_string edit{0xe70f};
inline constexpr glyph_string refresh{0xe72c};
inline constexpr glyph_string folder{0xe8b7};
inline constexpr glyph_string info{0xe946};
inline constexpr glyph_string warning{0xe7ba};
inline constexpr glyph_string play{0xe768};
inline constexpr glyph_string pause{0xe769};
inline constexpr glyph_string chevron_up{0xe70e};
inline constexpr glyph_string chevron_down{0xe70d};
inline constexpr glyph_string chevron_left{0xe76b};
inline constexpr glyph_string chevron_right{0xe76c};
inline constexpr glyph_string copy{0xe8c8};
inline constexpr glyph_string contact{0xe77b};
inline constexpr glyph_string mail{0xe715};
inline constexpr glyph_string calendar{0xe787};
inline constexpr glyph_string clock{0xe121};
inline constexpr glyph_string globe{0xe774};
inline constexpr glyph_string lock{0xe72e};
inline constexpr glyph_string unlock{0xe785};
inline constexpr glyph_string heart{0xeb51};
inline constexpr glyph_string color{0xe790};
inline constexpr glyph_string font{0xe8d2};
inline constexpr glyph_string view{0xe890};
} // namespace icons

} // namespace strata

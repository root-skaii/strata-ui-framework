#pragma once

#include "strata/types.hpp"

#include <array>
#include <string_view>

namespace strata {

// one code point as compile-time utf-8, convertible to string_view. keep it alive while a view is in use.
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

// "Segoe MDL2 Assets" (Windows 10) / "Segoe Fluent Icons" (Windows 11) code points. bake glyph_ranges::private_use
// and select the font with ui.with_font(icon_font) or the icon_* widgets. the sandbox's `--scene icons` shows every
// constant and flags the ones the loaded font lacks.
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

// tools: visibility, pinning, links, clipboard, filtering, sorting.
// no "clear filter" glyph exists: draw `filter` and `cancel` together.
inline constexpr glyph_string eye{0xe7b3};
// "Hide" is missing from the segmdl2.ttf of Windows 10 up to at least build 19045 (blank glyph there); use `view`
// for the hidden state to support those builds.
inline constexpr glyph_string eye_off{0xed1a};
inline constexpr glyph_string pin{0xe718};
inline constexpr glyph_string unpin{0xe77a};
inline constexpr glyph_string pin_off{0xe77a}; // the other name for unpin
inline constexpr glyph_string link{0xe71b};
inline constexpr glyph_string unlink{0xe8af};
inline constexpr glyph_string duplicate{0xe8c8};
inline constexpr glyph_string cut{0xe8c6};
inline constexpr glyph_string paste{0xe77f};
inline constexpr glyph_string filter{0xe71c};
inline constexpr glyph_string sort{0xe8cb};
inline constexpr glyph_string clear{0xe894};       // "clear field / filter" button
inline constexpr glyph_string select_all{0xe8b3};
inline constexpr glyph_string rename{0xe8ac};      // what F2 does
inline constexpr glyph_string document{0xe8a5};
inline constexpr glyph_string layers{0xf156};
inline constexpr glyph_string expand_all{0xe710};
inline constexpr glyph_string collapse_all{0xe738};
inline constexpr glyph_string more{0xe712};

// running: transport controls beyond play / pause, undo history
inline constexpr glyph_string stop{0xe71a};
inline constexpr glyph_string step{0xe893};
inline constexpr glyph_string next_frame{0xe893}; // the other name for step
inline constexpr glyph_string undo{0xe7a7};
inline constexpr glyph_string redo{0xe7a6};

// status
inline constexpr glyph_string star{0xe734};
inline constexpr glyph_string star_filled{0xe735};
inline constexpr glyph_string error{0xea39};
inline constexpr glyph_string success{0xe930};

// 3d scene: viewport, objects, gizmo modes
inline constexpr glyph_string camera{0xe722};
inline constexpr glyph_string cube{0xf158};
inline constexpr glyph_string light{0xea80};
inline constexpr glyph_string move{0xe7c9};
inline constexpr glyph_string rotate{0xe7ad};
inline constexpr glyph_string scale{0xe740};
// view size, not trees (see expand_all / collapse_all)
inline constexpr glyph_string expand{0xe740};      // alias of scale (both Segoe "FullScreen")
inline constexpr glyph_string collapse{0xe73f};
inline constexpr glyph_string target{0xe1d2};
inline constexpr glyph_string grid{0xe80a};
inline constexpr glyph_string zoom_in{0xe8a3};
inline constexpr glyph_string zoom_out{0xe71f};

// files and views
inline constexpr glyph_string import_{0xe8b5}; // `import` is keyword-like in some toolchains
inline constexpr glyph_string export_{0xede1};
inline constexpr glyph_string open_folder{0xe838};
inline constexpr glyph_string code{0xe943};
inline constexpr glyph_string script{0xe99a};
inline constexpr glyph_string list{0xea37};
inline constexpr glyph_string tree{0xe902};

// every constant with its name (icons scene, font checks)
struct named_icon {
    std::string_view name;
    char32_t         code;
};

inline constexpr auto all = std::to_array<named_icon>({
    {"home", 0xe80f}, {"settings", 0xe713}, {"save", 0xe74e}, {"trash", 0xe74d}, {"search", 0xe721},
    {"accept", 0xe73e}, {"cancel", 0xe711}, {"add", 0xe710}, {"edit", 0xe70f}, {"refresh", 0xe72c},
    {"folder", 0xe8b7}, {"info", 0xe946}, {"warning", 0xe7ba}, {"play", 0xe768}, {"pause", 0xe769},
    {"chevron_up", 0xe70e}, {"chevron_down", 0xe70d}, {"chevron_left", 0xe76b}, {"chevron_right", 0xe76c},
    {"copy", 0xe8c8}, {"contact", 0xe77b}, {"mail", 0xe715}, {"calendar", 0xe787}, {"clock", 0xe121},
    {"globe", 0xe774}, {"lock", 0xe72e}, {"unlock", 0xe785}, {"heart", 0xeb51}, {"color", 0xe790},
    {"font", 0xe8d2}, {"view", 0xe890},
    {"eye", 0xe7b3}, {"eye_off", 0xed1a}, {"pin", 0xe718}, {"unpin", 0xe77a}, {"pin_off", 0xe77a},
    {"link", 0xe71b}, {"unlink", 0xe8af}, {"duplicate", 0xe8c8}, {"cut", 0xe8c6}, {"paste", 0xe77f},
    {"filter", 0xe71c}, {"sort", 0xe8cb}, {"expand_all", 0xe710},
    {"collapse_all", 0xe738}, {"more", 0xe712},
    {"stop", 0xe71a}, {"step", 0xe893}, {"next_frame", 0xe893}, {"undo", 0xe7a7}, {"redo", 0xe7a6},
    {"star", 0xe734}, {"error", 0xea39}, {"success", 0xe930},
    {"camera", 0xe722}, {"cube", 0xf158}, {"light", 0xea80}, {"move", 0xe7c9}, {"rotate", 0xe7ad},
    {"scale", 0xe740}, {"target", 0xe1d2}, {"grid", 0xe80a}, {"zoom_in", 0xe8a3}, {"zoom_out", 0xe71f},
    {"import", 0xe8b5}, {"export", 0xede1}, {"open_folder", 0xe838}, {"code", 0xe943}, {"script", 0xe99a},
    {"list", 0xea37}, {"tree", 0xe902},
    {"clear", 0xe894}, {"select_all", 0xe8b3}, {"rename", 0xe8ac}, {"document", 0xe8a5}, {"layers", 0xf156},
    {"star_filled", 0xe735}, {"expand", 0xe740}, {"collapse", 0xe73f},
});
} // namespace icons

} // namespace strata

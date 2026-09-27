#pragma once

// minimal png reader / writer for screenshots and goldens (8-bit rgb / rgba, no interlacing), with its own
// deflate / inflate.

#include <strata/types.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace imgio {

// writes rgb (alpha dropped); false on failure. `path` is utf-8
[[nodiscard]] bool write_png(std::string_view path, std::span<const strata::u8> rgba, strata::u32 width, strata::u32 height);
// reads any 8-bit rgb / rgba png into rgba; false if the file is missing or not supported
[[nodiscard]] bool read_png(std::string_view path, std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height);

struct compare_result {
    bool         same_size{};
    strata::u64  pixels{};
    strata::u64  differing{};     // pixels with any channel further than `tolerance` from the reference
    strata::u32  max_difference{}; // largest channel difference seen
    [[nodiscard]] double fraction() const noexcept { return pixels != 0 ? static_cast<double>(differing) / static_cast<double>(pixels) : 0.0; }
};

// compares two rgba images channel by channel (alpha ignored)
[[nodiscard]] compare_result compare(std::span<const strata::u8> a, std::span<const strata::u8> b, strata::u32 width,
                                     strata::u32 height, strata::u32 tolerance);
// an image that shows where `a` and `b` differ (differences amplified, unchanged pixels dimmed)
[[nodiscard]] std::vector<strata::u8> diff_image(std::span<const strata::u8> a, std::span<const strata::u8> b, strata::u32 width,
                                                 strata::u32 height);

} // namespace imgio

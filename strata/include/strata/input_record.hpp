#pragma once

// records the input_state fed to context::begin_frame and plays it back: bug reports that reproduce, scripted
// tests without a mouse. with the software renderer a recording gives the same pixels on any machine with the same fonts.
//
//   strata::input_recorder rec;                 strata::input_replayer play;
//   rec.record(input);   // each frame           play.load_file("run.strata_input");
//   rec.save("run.strata_input");                while (play.next(input)) { ui.begin_frame(input); ... }
//
// the format is little-endian, versioned, one length-prefixed record per frame.

#include "strata/context.hpp"

#include <span>
#include <string_view>
#include <vector>

namespace strata {

class input_recorder {
public:
    input_recorder();

    void record(const input_state& in);
    void clear();

    [[nodiscard]] std::size_t        frames() const noexcept { return frames_; }
    // header and every recorded frame
    [[nodiscard]] std::span<const u8> data() const noexcept { return bytes_; }
    // writes data() to a file (utf-8 path); false on failure
    [[nodiscard]] bool save(std::string_view path) const;

private:
    std::vector<u8> bytes_;
    std::size_t     frames_{};
};

class input_replayer {
public:
    // false (nothing kept) for a wrong header, an unknown version or a truncated / malformed frame
    [[nodiscard]] bool load(std::span<const u8> bytes);
    [[nodiscard]] bool load_file(std::string_view path);

    // the next recorded frame into `out`. after the last one it returns false and leaves `out` as the final frame with
    // its one-shot parts (key presses, typed text, wheel) cleared, so the ui can settle
    [[nodiscard]] bool next(input_state& out) noexcept;
    void rewind() noexcept { position_ = 0; }

    [[nodiscard]] std::size_t frames() const noexcept { return offsets_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] bool        finished() const noexcept { return position_ >= offsets_.size(); }

private:
    std::vector<u8>          bytes_;
    std::vector<std::size_t> offsets_; // start of each frame's payload
    std::size_t              position_{};
    input_state              last_;
};

} // namespace strata

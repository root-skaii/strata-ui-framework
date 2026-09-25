#pragma once

#include "strata/types.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

enum class log_level : u8 { trace, debug, info, warn, error };

// the text of a console / log window: a ring of lines that context::log_view() draws with filtering, colors, a follow
// mode and row selection. one buffer is one view: the view's state (filter, level, follow) lives in `view`.
class log_buffer {
public:
    struct line {
        log_level   level{};
        f64         time{}; // seconds of ui time (context::time()) when the line was added; log_view stamps lines that had none
        u64         seq{};  // grows with every line, so a selection survives old lines falling out of the ring
        std::string text;
        i64         wall_ms{}; // the clock when it was added: milliseconds since 1970-01-01 (utc)
        f32         wrap_w{-1.0f}; // (log_view's cache: the wrap width the height below was measured for)
        f32         wrap_h{};
    };

    // what the view shows and how; edit these from code or let the toolbar do it
    struct view_state {
        std::string filter;                 // case-insensitive substring
        log_level   min_level{log_level::trace};
        bool        follow{true};           // stick to the newest line; scrolling up turns it off, reaching the bottom back on
        bool        show_time{};
        bool        clock{};                // show_time: the wall clock (HH:MM:SS.mmm, local time) instead of ui seconds
        bool        wrap{};                 // long lines (and lines with line breaks) take as many rows as they need
        u64         sel_anchor{};           // selected lines: [min, max] of these two seq numbers (0 = none)
        u64         sel_cursor{};
    };

    explicit log_buffer(std::size_t max_lines = 5000) : max_lines_{max_lines} {}

    // "HH:MM:SS.mmm" in local time for a line's wall_ms (what the view shows with `clock`)
    [[nodiscard]] static std::string clock_text(i64 wall_ms)
    {
        const std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &secs);
#else
        localtime_r(&secs, &tm);
#endif
        const int ms = static_cast<int>(((wall_ms % 1000) + 1000) % 1000);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
        return buf;
    }

    // `time` < 0 (the default): the log view stamps the line with the ui time the first time it draws it
    // `wall_ms` < 0 stamps the line with the system clock now; pass a value to replay lines with the times they had
    void add(log_level level, std::string_view text, f64 time = -1.0, i64 wall_ms = -1)
    {
        if (wall_ms < 0) {
            wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        lines_.push_back({level, time, next_seq_++, std::string{text}, wall_ms, -1.0f, 0.0f});
        while (lines_.size() > max_lines_) { lines_.pop_front(); }
        ++version_;
    }
    template <class... args>
    void addf(log_level level, std::format_string<args...> fmt, args&&... a)
    {
        add(level, std::format(fmt, std::forward<args>(a)...));
    }
    void clear()
    {
        lines_.clear();
        view.sel_anchor = view.sel_cursor = 0;
        ++version_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return lines_.size(); }
    [[nodiscard]] const line& operator[](std::size_t i) const { return lines_[i]; }
    [[nodiscard]] u64 version() const noexcept { return version_; } // changes when lines are added or removed
    [[nodiscard]] std::size_t max_lines() const noexcept { return max_lines_; }

    view_state view;

private:
    friend class context;

    std::deque<line> lines_;
    std::size_t      max_lines_;
    u64              next_seq_{1};
    u64              version_{};

    // indices of the lines the filter lets through, rebuilt when the buffer or the filter changes
    std::vector<u32> visible_;
    std::vector<f32> row_top_;      // wrapping: where each visible row starts (one more entry than rows)
    u64              visible_version_{~u64{0}};
    std::string      visible_filter_;
    log_level        visible_level_{};
    f32              last_scroll_{-1.0f};
    bool             focused_{};
};

} // namespace strata

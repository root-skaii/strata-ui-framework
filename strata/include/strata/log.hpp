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

// console / log text: a ring of lines that context::log_view() draws with filter, colours, follow mode and selection.
// one buffer = one view; view state lives in `view`.
class log_buffer {
public:
    // line text lives in one arena (no allocation per line). `text()` is valid until the next add() / clear().
    struct line {
        log_level   level{};
        f64         time{}; // ui time (context::time()) when added; log_view stamps unset lines
        u64         seq{};  // monotonic, so selections survive ring eviction
        i64         wall_ms{}; // wall clock when added: ms since 1970-01-01 utc
        f32         wrap_w{-1.0f}; // (log_view cache: wrap width `height` was measured for)
        f32         wrap_h{};

        [[nodiscard]] std::string_view text() const noexcept { return {arena->data() + off, len}; }

    private:
        friend class log_buffer;
        const std::vector<char>* arena{};
        std::size_t              off{};
        std::size_t              len{};
    };

    // view state; set from code or via the toolbar
    struct view_state {
        std::string filter;                 // case-insensitive substring
        log_level   min_level{log_level::trace};
        bool        follow{true};           // stick to the newest line; scrolling up / reaching the bottom toggles it
        bool        show_time{};
        bool        clock{};                // show_time as local wall clock (HH:MM:SS.mmm) instead of ui seconds
        bool        wrap{};                 // long / multi-line entries take several rows
        u64         sel_anchor{};           // selection: [min, max] of these seq numbers (0 = none)
        u64         sel_cursor{};
    };

    explicit log_buffer(std::size_t max_lines = 5000) : max_lines_{max_lines} {}

    // lines point into this buffer's arena, so copy / move must reseat them
    log_buffer(const log_buffer& o) : max_lines_{o.max_lines_} { assign_from(o); }
    log_buffer(log_buffer&& o) noexcept : max_lines_{o.max_lines_} { assign_from(std::move(o)); }
    log_buffer& operator=(const log_buffer& o)
    {
        if (this != &o) { assign_from(o); }
        return *this;
    }
    log_buffer& operator=(log_buffer&& o) noexcept
    {
        if (this != &o) { assign_from(std::move(o)); }
        return *this;
    }
    ~log_buffer() = default;

    // "HH:MM:SS.mmm" local time for a wall_ms
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

    // `time` < 0: stamped with ui time when first drawn. `wall_ms` < 0: stamped with the clock now; pass one to replay.
    void add(log_level level, std::string_view text, f64 time = -1.0, i64 wall_ms = -1)
    {
        if (wall_ms < 0) {
            wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        line l;
        l.level   = level;
        l.time    = time;
        l.seq     = next_seq_++;
        l.wall_ms = wall_ms;
        l.arena   = &arena_;
        l.off     = arena_.size();
        l.len     = text.size();
        arena_.insert(arena_.end(), text.begin(), text.end());
        lines_.push_back(l);
        while (lines_.size() > max_lines_) {
            dead_ += lines_.front().len;
            lines_.pop_front();
        }
        compact();
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
        arena_.clear();
        dead_ = 0;
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

    // the arena grows at the back; once dead bytes at the front exceed half, live bytes move down (amortised ~1 byte
    // moved per byte logged) and the arena settles near the ring's size.
    // shared by copy / move: take everything, then reseat every line's `arena` to *this*
    template <class Self>
    void assign_from(Self&& o)
    {
        lines_     = std::forward<Self>(o).lines_;
        arena_     = std::forward<Self>(o).arena_;
        view       = std::forward<Self>(o).view;
        dead_      = o.dead_;
        max_lines_ = o.max_lines_;
        next_seq_  = o.next_seq_;
        version_   = o.version_;
        for (line& l : lines_) { l.arena = &arena_; }
    }

    void compact()
    {
        if (dead_ * 2 < arena_.size() || dead_ == 0) {
            return;
        }
        arena_.erase(arena_.begin(), arena_.begin() + static_cast<std::ptrdiff_t>(dead_));
        for (line& l : lines_) { l.off -= dead_; }
        dead_ = 0;
    }

    std::deque<line>  lines_;
    std::vector<char> arena_;   // the text of every live line, back to back
    std::size_t       dead_{};  // dead bytes at the front
    std::size_t      max_lines_;
    u64              next_seq_{1};
    u64              version_{};

    // filtered line indices, rebuilt when the buffer or filter changes
    std::vector<u32> visible_;
    std::vector<f32> row_top_;      // wrapping: first row of each visible line (+1 entry)
    u64              visible_version_{~u64{0}};
    std::string      visible_filter_;
    log_level        visible_level_{};
    f32              last_scroll_{-1.0f};
    bool             focused_{};
};

} // namespace strata

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
    // the text of the lines lives in one arena, not in a std::string per line: a log is written to at whatever rate
    // the program produces events, and a string per line means an allocation per line plus the deque's own churn.
    // `text()` is a view into that arena, valid until the next add() / clear().
    struct line {
        log_level   level{};
        f64         time{}; // seconds of ui time (context::time()) when the line was added; log_view stamps lines that had none
        u64         seq{};  // grows with every line, so a selection survives old lines falling out of the ring
        i64         wall_ms{}; // the clock when it was added: milliseconds since 1970-01-01 (utc)
        f32         wrap_w{-1.0f}; // (log_view's cache: the wrap width the height below was measured for)
        f32         wrap_h{};

        [[nodiscard]] std::string_view text() const noexcept { return {arena->data() + off, len}; }

    private:
        friend class log_buffer;
        const std::vector<char>* arena{};
        std::size_t              off{};
        std::size_t              len{};
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

    // every line points at this buffer's arena for its text, so a copy or a move has to reseat them -- otherwise the
    // copy's lines would read the original's bytes, and a moved-from buffer's would dangle
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

    // the arena only ever grows at the back, so the bytes of lines that fell out of the ring pile up at the front.
    // once they are more than half of it, the live bytes move down to 0 and every offset shifts with them: one
    // memmove per half-arena of text, which amortises to a byte moved about once per byte logged, and the arena
    // settles at roughly the size of the lines the ring holds. no reallocation after that.
    // shared by the copy / move constructors and assignments: take everything over, then point every line's `arena`
    // at *this* buffer's arena
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
    std::size_t       dead_{};  // bytes at the front belonging to lines that are gone
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

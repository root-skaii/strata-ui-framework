#pragma once

// lines for a log_buffer written from any thread. a log_buffer (and the log_view drawing it) belongs to the ui thread; a
// program logs from workers, loaders and callbacks. those add() here, and the ui thread moves what has come in into its
// buffer once per frame:
//
//     strata::log_queue g_log_in;                         // shared by everyone
//     g_log_in.add(strata::log_level::info, "loaded 12 assets");        // any thread
//     g_log_in.addf(strata::log_level::warn, "{} missing", name);
//     ...
//     g_log_in.drain_into(log);                           // ui thread, each frame, before ui.log_view(..., log)
//
// each line keeps the wall-clock time it was added at, not the time it was drained. a queue that nobody drains stops
// growing at `max_pending` lines and counts what it dropped. the text of the pending lines lives in one arena, so in steady
// state neither side allocates per line.

#include "strata/log.hpp"

#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata {

class log_queue {
public:
    explicit log_queue(std::size_t max_pending = 10000) : max_pending_{max_pending} {}

    log_queue(const log_queue&)            = delete;
    log_queue& operator=(const log_queue&) = delete;

    void add(log_level level, std::string_view text)
    {
        const i64 wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const std::lock_guard lock{mutex_};
        if (in_.lines.size() >= max_pending_) {
            ++dropped_;
            return;
        }
        in_.lines.push_back({level, wall, in_.text.size(), text.size()});
        in_.text.append(text);
    }

    template <class... args>
    void addf(log_level level, std::format_string<args...> fmt, args&&... a)
    {
        add(level, std::format(fmt, std::forward<args>(a)...));
    }

    // moves everything added so far into `log` (ui thread); the number of lines moved. the lock is held only to swap
    // the pending lines out, not while the log takes them
    std::size_t drain_into(log_buffer& log)
    {
        {
            const std::lock_guard lock{mutex_};
            if (in_.lines.empty()) { return 0; }
            std::swap(in_, out_);
        }
        for (const pending& p : out_.lines) {
            log.add(p.level, std::string_view{out_.text}.substr(p.off, p.len), -1.0, p.wall_ms);
        }
        const std::size_t n = out_.lines.size();
        out_.lines.clear(); // (capacity kept: the next swap hands it back to the writers)
        out_.text.clear();
        return n;
    }

    // lines refused because max_pending were waiting (nobody drained the queue)
    [[nodiscard]] std::size_t dropped() const
    {
        const std::lock_guard lock{mutex_};
        return dropped_;
    }

private:
    struct pending {
        log_level   level{};
        i64         wall_ms{};
        std::size_t off{};
        std::size_t len{};
    };
    struct batch {
        std::vector<pending> lines;
        std::string          text;
    };

    mutable std::mutex mutex_;
    batch              in_;  // written by add()
    batch              out_; // read by drain_into()
    std::size_t        max_pending_;
    std::size_t        dropped_{};
};

} // namespace strata

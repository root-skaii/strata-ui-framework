// the console / log view

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>

namespace strata {

namespace {

[[nodiscard]] char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// case-insensitive (ascii) substring test; `needle` is already lower case
[[nodiscard]] bool contains_ci(std::string_view hay, std::string_view needle) noexcept
{
    if (needle.empty()) { return true; }
    if (needle.size() > hay.size()) { return false; }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lower(hay[i + k]) == needle[k]) { ++k; }
        if (k == needle.size()) { return true; }
    }
    return false;
}

[[nodiscard]] std::string_view level_tag(log_level l) noexcept
{
    switch (l) {
    case log_level::trace: return "TRC";
    case log_level::debug: return "DBG";
    case log_level::info:  return "INF";
    case log_level::warn:  return "WRN";
    default:               return "ERR";
    }
}

[[nodiscard]] color level_color(log_level l, const style& st) noexcept
{
    switch (l) {
    case log_level::trace: return st.text_dim.scaled_alpha(0.65f);
    case log_level::debug: return st.text_dim;
    case log_level::info:  return st.text;
    case log_level::warn:  return st.warning;
    default:               return st.error;
    }
}

} // namespace

void context::log_view(std::string_view id_label, log_buffer& log, vec2 size, log_view_flags flags)
{
    if (m_->cur_ == nullptr) {
        return;
    }
    push_id(id_label);
    log_buffer::view_state& v = log.view;
    const font_id f = current_font();
    for (std::size_t i = log.lines_.size(); i-- > 0 && log.lines_[i].time < 0.0;) { // lines added since the last look
        log.lines_[i].time = m_->time_;
    }

    // toolbar -------------------------------------------------------------------------------------------
    if (!has_flag(flags, log_view_flags::no_toolbar)) {
        set_next_item_width(std::max(m_->layout_.width * 0.22f, 90.0f));
        (void)input_text("##filter", v.filter, "filter...");
        same_line();
        static constexpr std::array<std::string_view, 5> level_names = {"trace", "debug", "info", "warn", "error"};
        int level = static_cast<int>(v.min_level);
        set_next_item_width(86.0f);
        if (combo("##level", level, level_names.data(), level_names.size())) {
            v.min_level = static_cast<log_level>(level);
        }
        same_line();
        (void)checkbox("follow", v.follow);
        same_line();
        (void)checkbox("time", v.show_time);
        tooltip(v.clock ? "the time of day when the line was added" : "seconds of ui time when the line was added");
        if (v.show_time) {
            same_line();
            (void)checkbox("clock", v.clock);
            tooltip("time of day instead of ui seconds");
        }
        same_line();
        (void)checkbox("wrap", v.wrap);
        tooltip("long lines take several rows");
        same_line();
        const bool copy_clicked = button("copy");
        same_line();
        if (button("clear")) {
            log.clear();
            v.follow = true;
        }
        if (copy_clicked && m_->clipboard_.set != nullptr) {
            std::string all;
            const bool any_selected = v.sel_anchor != 0;
            const u64 lo = std::min(v.sel_anchor, v.sel_cursor);
            const u64 hi = std::max(v.sel_anchor, v.sel_cursor);
            for (std::size_t i = 0; i < log.size(); ++i) {
                const log_buffer::line& l = log[i];
                if (any_selected && (l.seq < lo || l.seq > hi)) { continue; }
                if (l.level < v.min_level) { continue; }
                all += l.text();
                all += '\n';
            }
            m_->clipboard_.set(m_->clipboard_.user, all);
        }
    }

    // the lines that pass the filter --------------------------------------------------------------------------------
    if (log.visible_version_ != log.version_ || log.visible_filter_ != v.filter || log.visible_level_ != v.min_level) {
        std::string needle = v.filter;
        for (char& c : needle) { c = lower(c); }
        log.visible_.clear();
        for (std::size_t i = 0; i < log.lines_.size(); ++i) {
            const log_buffer::line& l = log.lines_[i];
            if (l.level >= v.min_level && contains_ci(l.text(), needle)) {
                log.visible_.push_back(static_cast<u32>(i));
            }
        }
        log.visible_version_ = log.version_;
        log.visible_filter_  = v.filter;
        log.visible_level_   = v.min_level;
    }
    const std::size_t count = log.visible_.size();

    // the body: a scrolling child; only visible rows are drawn ---------------------------------------------------------
    f32 body_h = size.y;
    if (body_h <= 0.0f) {
        body_h = m_->layout_.bound_bottom > 0.0f ? std::max(m_->layout_.bound_bottom - layout_next_y(), 60.0f) : 220.0f;
    }
    if (begin_child("##body", {size.x, body_h}, child_flags::frame)) {
        child_frame& cf = m_->child_stack_[m_->child_depth_ - 1];
        child_state& st = *cf.state;
        const f32 row_h  = m_->font_.line_height(f) + 3.0f;
        const f32 time_w = v.show_time ? m_->font_.measure(f, v.clock ? "00:00:00.000 " : "000.0 ").x + 4.0f : 0.0f;
        const f32 tag_w  = m_->font_.measure(f, "WRN ").x + 6.0f;

        // wrapping: rows are as tall as their text (measured once per line and width), laid out by a table
        const f32 wrap_w = std::max(cf.inner.width() - time_w - tag_w - 14.0f, 40.0f);
        const bool wrap  = v.wrap;
        if (wrap) {
            log.row_top_.resize(count + 1);
            f32 y = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                log_buffer::line& l = log.lines_[log.visible_[i]];
                if (l.wrap_w != wrap_w) {
                    l.wrap_h = l.text().empty() ? m_->font_.line_height(f) : rich_layout(l.text(), f, m_->style_.text, wrap_w, false).y;
                    l.wrap_w = wrap_w;
                }
                log.row_top_[i] = y;
                y += std::max(l.wrap_h, m_->font_.line_height(f)) + 3.0f;
            }
            log.row_top_[count] = y;
        }
        const f32 total  = wrap ? log.row_top_[count] : static_cast<f32>(count) * row_h;
        const f32 view_h = cf.inner.height();
        const auto row_at = [&](f32 fy) -> std::size_t {
            if (count == 0 || fy <= 0.0f) { return 0; }
            if (!wrap) { return std::min(count - 1, static_cast<std::size_t>(fy / row_h)); }
            const auto it = std::upper_bound(log.row_top_.begin(), log.row_top_.begin() + static_cast<std::ptrdiff_t>(count), fy);
            return std::min(count - 1, static_cast<std::size_t>(it - log.row_top_.begin()) - 1);
        };
        const f32 max_scroll = std::max(0.0f, total - view_h);

        // follow mode: sticks to the newest line; scrolling up releases, reaching the bottom re-engages
        if (v.follow && log.last_scroll_ >= 0.0f && st.scroll < log.last_scroll_ - 1.0f) {
            v.follow = false;
        } else if (!v.follow && max_scroll > 0.0f && st.scroll >= max_scroll - 1.0f) {
            v.follow = true;
        }
        if (v.follow) {
            st.scroll = max_scroll;
        }
        log.last_scroll_ = v.follow ? st.scroll : -1.0f;
        m_->layout_.origin.y = cf.inner.min.y - st.scroll;

        (void)layout_place({1.0f, std::max(total, 1.0f)}); // the scrollable height

        const bool overflowing = total > view_h;
        const rect hit = {cf.bounds.min, {cf.bounds.max.x - (overflowing ? 12.0f : 0.0f), cf.bounds.max.y}};
        const interaction in = interact(widget_id("##rows"), hit);
        if (in.held && count > 0) {
            const std::size_t idx = row_at(m_->input_.mouse_.y - m_->layout_.origin.y);
            const u64 seq = log.lines_[log.visible_[idx]].seq;
            if (m_->input_.mouse_pressed_) {
                v.sel_cursor = seq;
                if (!(m_->input_.mod_shift_ && v.sel_anchor != 0)) { v.sel_anchor = seq; }
                log.focused_ = true;
            } else {
                v.sel_cursor = seq; // dragging extends
            }
        } else if (m_->input_.mouse_pressed_ && !cf.bounds.contains(m_->input_.mouse_)) {
            log.focused_ = false;
        }
        if (log.focused_) { // Ctrl+A / Ctrl+C on the selected rows
            for (u32 i = 0; i < m_->input_.key_count_; ++i) {
                if (m_->input_.keys_[i].k == key::a && count > 0) {
                    v.sel_anchor = log.lines_[log.visible_.front()].seq;
                    v.sel_cursor = log.lines_[log.visible_.back()].seq;
                } else if (m_->input_.keys_[i].k == key::c && v.sel_anchor != 0 && m_->clipboard_.set != nullptr) {
                    const u64 lo = std::min(v.sel_anchor, v.sel_cursor);
                    const u64 hi = std::max(v.sel_anchor, v.sel_cursor);
                    std::string text;
                    for (const u32 vi : log.visible_) {
                        const log_buffer::line& l = log.lines_[vi];
                        if (l.seq >= lo && l.seq <= hi) { text += l.text(); text += '\n'; }
                    }
                    m_->clipboard_.set(m_->clipboard_.user, text);
                }
            }
        }

        const u64 sel_lo = std::min(v.sel_anchor, v.sel_cursor);
        const u64 sel_hi = std::max(v.sel_anchor, v.sel_cursor);
        const std::size_t first = count == 0 ? 0 : row_at(st.scroll);
        std::size_t last = first;
        if (wrap) {
            while (last < count && log.row_top_[last] < st.scroll + view_h) { ++last; }
            last = std::min(count, last + 1);
        } else {
            last = std::min(count, first + static_cast<std::size_t>(view_h / row_h) + 2);
        }
        for (std::size_t i = first; i < last; ++i) {
            const log_buffer::line& l = log.lines_[log.visible_[i]];
            const f32 top = wrap ? log.row_top_[i] : static_cast<f32>(i) * row_h;
            const f32 h   = wrap ? log.row_top_[i + 1] - top : row_h;
            const f32 y = m_->layout_.origin.y + top;
            const f32 x = cf.inner.min.x;
            if (v.sel_anchor != 0 && l.seq >= sel_lo && l.seq <= sel_hi) {
                m_->dl_.rect_filled({{cf.inner.min.x - 2.0f, y}, {cf.inner.max.x, y + h}}, m_->style_.accent.scaled_alpha(0.28f));
            } else if ((i & 1) != 0) {
                m_->dl_.rect_filled({{cf.inner.min.x - 2.0f, y}, {cf.inner.max.x, y + h}}, color{255, 255, 255, 6});
            }
            f32 tx = x;
            if (v.show_time) {
                if (v.clock) {
                    m_->dl_.text({tx, y + 1.5f}, m_->style_.text_dim.scaled_alpha(0.8f), log_buffer::clock_text(l.wall_ms), f);
                } else {
                    std::array<char, 16> buf;
                    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), l.time, std::chars_format::fixed, 1);
                    m_->dl_.text({tx, y + 1.5f}, m_->style_.text_dim.scaled_alpha(0.8f), {buf.data(), r.ptr}, f);
                }
                tx += time_w;
            }
            const color c = level_color(l.level, m_->style_);
            m_->dl_.text({tx, y + 1.5f}, c, level_tag(l.level), f);
            if (wrap) {
                rich_layout(l.text(), f, c, wrap_w, false);
                rich_draw({tx + tag_w, y + 1.5f});
            } else {
                m_->dl_.text({tx + tag_w, y + 1.5f}, c, l.text(), f);
            }
        }
        end_child();
    }
    pop_id();
}

} // namespace strata

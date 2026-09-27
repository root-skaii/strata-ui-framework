#include "strata/datetime.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <format>

namespace strata {

namespace {

struct clock_override {
    bool        on{};
    date        day{};
    time_of_day time{};
};
clock_override g_clock;

} // namespace

void override_clock(const date& d, const time_of_day& t) noexcept
{
    g_clock = {true, d, t};
}

void reset_clock() noexcept
{
    g_clock = {};
}

date today()
{
    if (g_clock.on) { return g_clock.day; }
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return {st.wYear, st.wMonth, st.wDay};
}

time_of_day now()
{
    if (g_clock.on) { return g_clock.time; }
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return {st.wHour, st.wMinute, st.wSecond};
}

std::string to_string(const date& d)
{
    return std::format("{:04}-{:02}-{:02}", d.year, d.month, d.day);
}

std::string to_string(const time_of_day& t, bool seconds)
{
    return seconds ? std::format("{:02}:{:02}:{:02}", t.hour, t.minute, t.second) : std::format("{:02}:{:02}", t.hour, t.minute);
}

namespace {

// the numbers in `text` separated by `seps`; false unless exactly `count`, all digits
[[nodiscard]] bool split_numbers(std::string_view text, std::string_view seps, std::array<i32, 3>& out, std::size_t count) noexcept
{
    std::size_t found = 0;
    while (!text.empty()) {
        if (found >= count) { return false; }
        std::size_t end = 0;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; }
        if (end == 0 || end > 4) { return false; }
        i32 v = 0;
        if (std::from_chars(text.data(), text.data() + end, v).ec != std::errc{}) { return false; }
        out[found++] = v;
        text.remove_prefix(end);
        if (text.empty()) { break; }
        if (seps.find(text.front()) == std::string_view::npos) { return false; }
        text.remove_prefix(1);
        if (text.empty()) { return false; } // a trailing separator
    }
    return found == count;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return s;
}

} // namespace

bool parse_date(std::string_view text, date& out) noexcept
{
    std::array<i32, 3> n{};
    if (!split_numbers(trim(text), "-/.", n, 3)) { return false; }
    const date d{n[0], n[1], n[2]};
    if (!is_valid(d)) { return false; }
    out = d;
    return true;
}

bool parse_time(std::string_view text, time_of_day& out) noexcept
{
    text = trim(text);
    std::array<i32, 3> n{};
    time_of_day        t;
    if (split_numbers(text, ":", n, 3)) {
        t = {n[0], n[1], n[2]};
    } else if (split_numbers(text, ":", n, 2)) {
        t = {n[0], n[1], 0};
    } else {
        return false;
    }
    if (!is_valid(t)) { return false; }
    out = t;
    return true;
}

std::string_view month_name(i32 month) noexcept
{
    static constexpr std::array<std::string_view, 12> names = {"January", "February", "March", "April", "May", "June", "July",
                                                                "August", "September", "October", "November", "December"};
    return month >= 1 && month <= 12 ? names[static_cast<std::size_t>(month - 1)] : std::string_view{"?"};
}

std::string_view weekday_short(i32 weekday) noexcept
{
    static constexpr std::array<std::string_view, 7> names = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    return weekday >= 0 && weekday < 7 ? names[static_cast<std::size_t>(weekday)] : std::string_view{"?"};
}

} // namespace strata

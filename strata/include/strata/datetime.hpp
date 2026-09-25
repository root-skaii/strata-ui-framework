#pragma once

// calendar dates and times of day for the date / time pickers: plain values, no time zones. dates are proleptic
// Gregorian; weekdays count from Monday.

#include "strata/types.hpp"

#include <compare>
#include <string>
#include <string_view>

namespace strata {

struct date {
    i32 year{1970};
    i32 month{1}; // 1 .. 12
    i32 day{1};   // 1 .. days_in_month

    friend constexpr auto operator<=>(const date&, const date&) noexcept = default;
};

struct time_of_day {
    i32 hour{};   // 0 .. 23
    i32 minute{}; // 0 .. 59
    i32 second{}; // 0 .. 59

    friend constexpr auto operator<=>(const time_of_day&, const time_of_day&) noexcept = default;
};

[[nodiscard]] constexpr bool is_leap_year(i32 year) noexcept
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr i32 days_in_month(i32 year, i32 month) noexcept
{
    constexpr i32 days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) { return 30; }
    return month == 2 && is_leap_year(year) ? 29 : days[month - 1];
}

[[nodiscard]] constexpr bool is_valid(const date& d) noexcept
{
    return d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= days_in_month(d.year, d.month);
}

[[nodiscard]] constexpr bool is_valid(const time_of_day& t) noexcept
{
    return t.hour >= 0 && t.hour < 24 && t.minute >= 0 && t.minute < 60 && t.second >= 0 && t.second < 60;
}

// days since 1970-01-01 (negative before it) and back; the algorithms of Howard Hinnant
[[nodiscard]] constexpr i64 days_from_civil(const date& d) noexcept
{
    const i64 y   = d.month <= 2 ? d.year - 1 : d.year;
    const i64 era = (y >= 0 ? y : y - 399) / 400;
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (d.month + (d.month > 2 ? -3 : 9)) + 2) / 5 + d.day - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

[[nodiscard]] constexpr date civil_from_days(i64 z) noexcept
{
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp  = (5 * doy + 2) / 153;
    const i32 d   = static_cast<i32>(doy - (153 * mp + 2) / 5 + 1);
    const i32 m   = static_cast<i32>(mp < 10 ? mp + 3 : mp - 9);
    return {static_cast<i32>(yoe + era * 400 + (m <= 2 ? 1 : 0)), m, d};
}

// 0 = Monday .. 6 = Sunday
[[nodiscard]] constexpr i32 weekday(const date& d) noexcept
{
    const i64 z = days_from_civil(d) + 3; // 1970-01-01 was a Thursday
    return static_cast<i32>(((z % 7) + 7) % 7);
}

[[nodiscard]] constexpr date add_days(const date& d, i64 days) noexcept
{
    return civil_from_days(days_from_civil(d) + days);
}

// the same day of another month; a day that does not exist there (31 Jan + 1 month) becomes the last day of it
[[nodiscard]] constexpr date add_months(const date& d, i32 months) noexcept
{
    const i64 index = static_cast<i64>(d.year) * 12 + (d.month - 1) + months;
    date      r{static_cast<i32>(index >= 0 ? index / 12 : (index - 11) / 12), 1, 1};
    r.month = static_cast<i32>(index - static_cast<i64>(r.year) * 12) + 1;
    const i32 last = days_in_month(r.year, r.month);
    r.day = d.day < last ? d.day : last;
    return r;
}

// the nearest valid date / time
[[nodiscard]] constexpr date clamp_date(date d) noexcept
{
    d.month = d.month < 1 ? 1 : d.month > 12 ? 12 : d.month;
    const i32 last = days_in_month(d.year, d.month);
    d.day = d.day < 1 ? 1 : d.day > last ? last : d.day;
    return d;
}

[[nodiscard]] constexpr time_of_day clamp_time(time_of_day t) noexcept
{
    t.hour   = t.hour < 0 ? 0 : t.hour > 23 ? 23 : t.hour;
    t.minute = t.minute < 0 ? 0 : t.minute > 59 ? 59 : t.minute;
    t.second = t.second < 0 ? 0 : t.second > 59 ? 59 : t.second;
    return t;
}

// the local clock
[[nodiscard]] date        today();
[[nodiscard]] time_of_day now();
// makes today() / now() return fixed values (for tests and reproducible screenshots) until reset_clock()
void override_clock(const date& d, const time_of_day& t) noexcept;
void reset_clock() noexcept;

// "2026-09-25" and "13:45" (or "13:45:30" with seconds)
[[nodiscard]] std::string to_string(const date& d);
[[nodiscard]] std::string to_string(const time_of_day& t, bool seconds = false);
// the inverse: "2026-09-25" (also with '/' or '.'), "13:45" or "13:45:30". false (and `out` untouched) for text that is
// not a real date / time
[[nodiscard]] bool parse_date(std::string_view text, date& out) noexcept;
[[nodiscard]] bool parse_time(std::string_view text, time_of_day& out) noexcept;

[[nodiscard]] std::string_view month_name(i32 month) noexcept;   // "January" .. "December"
[[nodiscard]] std::string_view weekday_short(i32 weekday) noexcept; // "Mo" .. "Su"

} // namespace strata

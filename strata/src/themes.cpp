#include "strata/themes.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <cstdio>

namespace strata::themes {

namespace {

struct named_theme {
    std::string_view name;
    style (*make)() noexcept;
};

constexpr std::array<named_theme, 12> registry = {{
    {"midnight", &midnight},
    {"light", &light},
    {"ocean", &ocean},
    {"rose", &rose},
    {"dracula", &dracula},
    {"nord", &nord},
    {"solarized_dark", &solarized_dark},
    {"solarized_light", &solarized_light},
    {"high_contrast", &high_contrast},
    {"forest", &forest},
    {"amber", &amber},
    {"glass", &glass},
}};

constexpr std::array<std::string_view, 12> registry_names = {
    "midnight", "light", "ocean", "rose", "dracula", "nord", "solarized_dark", "solarized_light",
    "high_contrast", "forest", "amber", "glass"};

struct number_key {
    std::string_view name;
    f32 style::*     member;
};
struct color_key {
    std::string_view name;
    color style::*   member;
};

constexpr std::array<number_key, 14> number_keys = {{
    {"padding", &style::padding},
    {"item_spacing", &style::item_spacing},
    {"rounding", &style::rounding},
    {"border_width", &style::border_width},
    {"shadow_blur", &style::shadow_blur},
    {"gradient", &style::gradient},
    {"anim_speed", &style::anim_speed},
    {"blur_radius", &style::blur_radius},
    {"acrylic_alpha", &style::acrylic_alpha},
    {"acrylic_noise", &style::acrylic_noise},
    {"acrylic_saturation", &style::acrylic_saturation},
    {"acrylic_brightness", &style::acrylic_brightness},
    {"popup_acrylic", &style::popup_acrylic},
    {"tooltip_delay_s", &style::tooltip_delay_s},
}};

constexpr std::array<color_key, 13> color_keys = {{
    {"window_bg", &style::window_bg},
    {"title_bg", &style::title_bg},
    {"border", &style::border},
    {"widget_bg", &style::widget_bg},
    {"widget_hover", &style::widget_hover},
    {"widget_active", &style::widget_active},
    {"widget_border", &style::widget_border},
    {"accent", &style::accent},
    {"accent_hover", &style::accent_hover},
    {"text", &style::text},
    {"text_dim", &style::text_dim},
    {"shadow", &style::shadow},
    {"modal_dim", &style::modal_dim},
}};

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
        if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
        if (x != y) { return false; }
    }
    return true;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) { s.remove_suffix(1); }
    return s;
}

[[nodiscard]] bool parse_color(std::string_view s, color& out) noexcept
{
    if (!s.empty() && s.front() == '#') { s.remove_prefix(1); }
    if (s.size() != 3 && s.size() != 4 && s.size() != 6 && s.size() != 8) { return false; }
    std::array<u8, 8> nib{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9')      { nib[i] = static_cast<u8>(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { nib[i] = static_cast<u8>(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { nib[i] = static_cast<u8>(ch - 'A' + 10); }
        else { return false; }
    }
    if (s.size() <= 4) {
        const auto dbl = [&](std::size_t i) { return static_cast<u8>(nib[i] * 17); };
        out = color{dbl(0), dbl(1), dbl(2), s.size() == 4 ? dbl(3) : u8{255}};
    } else {
        const auto byte = [&](std::size_t i) { return static_cast<u8>(nib[i] * 16 + nib[i + 1]); };
        out = color{byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : u8{255}};
    }
    return true;
}

[[nodiscard]] bool parse_number(std::string_view s, f32& out) noexcept
{
    if (s.empty()) { return false; }
    f32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) { return false; }
    out = v;
    return true;
}

void append_color(std::string& out, color c)
{
    constexpr char digits[] = "0123456789abcdef";
    out.push_back('#');
    for (const u8 v : {c.r, c.g, c.b, c.a}) {
        out.push_back(digits[v >> 4]);
        out.push_back(digits[v & 15]);
    }
}

void append_number(std::string& out, f32 v)
{
    std::array<char, 32> buf;
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    out.append(buf.data(), r.ptr);
}

[[nodiscard]] std::wstring widen(std::string_view s)
{
    std::wstring out(s.size(), L'\0');
    if (!s.empty()) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<std::size_t>(n > 0 ? n : 0));
    }
    return out;
}

} // namespace

std::span<const std::string_view> names() noexcept
{
    return registry_names;
}

bool by_name(std::string_view name, style& out) noexcept
{
    for (const named_theme& t : registry) {
        if (iequals(t.name, name)) {
            out = t.make();
            return true;
        }
    }
    return false;
}

std::string to_string(const style& s, std::string_view name)
{
    std::string out = "# strata theme";
    if (!name.empty()) {
        out += ": ";
        out += name;
    }
    out += "\n";
    for (const number_key& k : number_keys) {
        out += k.name;
        out += " = ";
        append_number(out, s.*k.member);
        out += "\n";
    }
    out += "frame_padding_x = ";
    append_number(out, s.frame_padding.x);
    out += "\nframe_padding_y = ";
    append_number(out, s.frame_padding.y);
    out += "\n";
    for (const color_key& k : color_keys) {
        out += k.name;
        out += " = ";
        append_color(out, s.*k.member);
        out += "\n";
    }
    return out;
}

theme_result from_string(std::string_view text, style& s)
{
    theme_result result;
    std::size_t line_no = 0;
    const auto problem = [&](std::size_t& counter) {
        ++counter;
        if (result.first_problem_line == 0) { result.first_problem_line = line_no; }
    };

    while (!text.empty()) {
        ++line_no;
        const std::size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (line.empty() || line.front() == '#' || line.front() == ';' || line.starts_with("//")) { continue; }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problem(result.invalid);
            continue;
        }
        const std::string_view key   = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));

        if (iequals(key, "base")) {
            style base;
            if (by_name(value, base)) { s = base; ++result.applied; } else { problem(result.invalid); }
            continue;
        }
        if (iequals(key, "frame_padding_x") || iequals(key, "frame_padding_y")) {
            f32 v{};
            if (!parse_number(value, v)) { problem(result.invalid); continue; }
            (iequals(key, "frame_padding_x") ? s.frame_padding.x : s.frame_padding.y) = v;
            ++result.applied;
            continue;
        }
        bool found = false;
        for (const number_key& k : number_keys) {
            if (iequals(key, k.name)) {
                f32 v{};
                if (parse_number(value, v)) { s.*k.member = v; ++result.applied; } else { problem(result.invalid); }
                found = true;
                break;
            }
        }
        if (found) { continue; }
        for (const color_key& k : color_keys) {
            if (iequals(key, k.name)) {
                color c;
                if (parse_color(value, c)) { s.*k.member = c; ++result.applied; } else { problem(result.invalid); }
                found = true;
                break;
            }
        }
        if (!found) { problem(result.unknown); }
    }
    return result;
}

void to_config(config& cfg, const style& s, std::string_view section)
{
    const std::string owned = to_string(s);
    std::string_view  text  = owned;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        const std::size_t eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) { continue; }
        cfg.set(section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
}

theme_result from_config(const config& cfg, style& s, std::string_view section)
{
    std::string text;
    for (const config::entry& e : cfg.entries(section)) {
        text += e.key;
        text += " = ";
        text += e.value;
        text += '\n';
    }
    return from_string(text, s);
}

bool save_file(std::string_view path, const style& s, std::string_view name)
{
    const std::wstring wide = widen(path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), L"wb") != 0 || f == nullptr) {
        return false;
    }
    const std::string text = to_string(s, name);
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

bool load_file(std::string_view path, style& s, theme_result* result)
{
    const std::wstring wide = widen(path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), L"rb") != 0 || f == nullptr) {
        return false;
    }
    std::string text;
    std::array<char, 4096> buf;
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) { break; }
        text.append(buf.data(), n);
        if (text.size() > (1u << 20)) { break; } // a theme is a few hundred bytes
    }
    std::fclose(f);
    // a byte order mark from a text editor
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xef && static_cast<u8>(text[1]) == 0xbb && static_cast<u8>(text[2]) == 0xbf) {
        text.erase(0, 3);
    }
    const theme_result r = from_string(text, s);
    if (result != nullptr) { *result = r; }
    return true;
}

} // namespace strata::themes

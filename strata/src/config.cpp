#include "strata/config.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <format>

namespace strata {

namespace {

constexpr std::size_t max_file_bytes = std::size_t{4} << 20;

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

[[nodiscard]] bool is_comment(std::string_view line) noexcept
{
    return line.front() == '#' || line.front() == ';' || line.starts_with("//");
}

[[nodiscard]] std::string clean_value(std::string_view v)
{
    std::string out{trim(v)};
    for (char& c : out) {
        if (c == '\n' || c == '\r') { c = ' '; }
    }
    return out;
}

// a key that would be read back as something else (a comment, a "key = value" split, a header) is made harmless
[[nodiscard]] std::string clean_key(std::string_view k)
{
    std::string out{trim(k)};
    for (char& c : out) {
        if (c == '=' || c == '\n' || c == '\r') { c = '_'; }
    }
    if (out.empty() || out.front() == '#' || out.front() == ';' || out.front() == '[' || out.starts_with("//")) {
        out.insert(out.begin(), '_');
    }
    return out;
}

[[nodiscard]] std::string clean_section(std::string_view s)
{
    std::string out{trim(s)};
    for (char& c : out) {
        if (c == '[' || c == ']' || c == '\n' || c == '\r') { c = '_'; }
    }
    return out;
}

[[nodiscard]] std::wstring widen(std::string_view s)
{
    if (s.empty()) { return {}; }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

const config::section_data* config::find_section(std::string_view name) const noexcept
{
    for (const section_data& s : sections_) {
        if (iequals(s.name, name)) { return &s; }
    }
    return nullptr;
}

const config::entry* config::find_entry(std::string_view section, std::string_view key) const noexcept
{
    const section_data* s = find_section(section);
    if (s == nullptr) { return nullptr; }
    for (const entry& e : s->items) {
        if (iequals(e.key, key)) { return &e; }
    }
    return nullptr;
}

void config::set(std::string_view section, std::string_view key, std::string_view value)
{
    const std::string name = clean_section(section);
    section_data*     s    = nullptr;
    for (section_data& d : sections_) {
        if (iequals(d.name, name)) { s = &d; break; }
    }
    if (s == nullptr) {
        sections_.push_back({name, {}});
        s = &sections_.back();
    }
    const std::string k = clean_key(key);
    for (entry& e : s->items) {
        if (iequals(e.key, k)) {
            e.value = clean_value(value);
            return;
        }
    }
    s->items.push_back({k, clean_value(value)});
}

void config::set_int(std::string_view section, std::string_view key, i32 value)
{
    set(section, key, std::to_string(value));
}

void config::set_float(std::string_view section, std::string_view key, f32 value)
{
    set(section, key, std::format("{}", value));
}

void config::set_bool(std::string_view section, std::string_view key, bool value)
{
    set(section, key, value ? "true" : "false");
}

std::string_view config::get(std::string_view section, std::string_view key, std::string_view fallback) const noexcept
{
    const entry* e = find_entry(section, clean_key(key));
    return e != nullptr ? std::string_view{e->value} : fallback;
}

i32 config::get_int(std::string_view section, std::string_view key, i32 fallback) const noexcept
{
    const entry* e = find_entry(section, clean_key(key));
    if (e == nullptr) { return fallback; }
    i32        v{};
    const auto r = std::from_chars(e->value.data(), e->value.data() + e->value.size(), v);
    return r.ec == std::errc{} && r.ptr == e->value.data() + e->value.size() ? v : fallback;
}

f32 config::get_float(std::string_view section, std::string_view key, f32 fallback) const noexcept
{
    const entry* e = find_entry(section, clean_key(key));
    if (e == nullptr) { return fallback; }
    f32        v{};
    const auto r = std::from_chars(e->value.data(), e->value.data() + e->value.size(), v);
    return r.ec == std::errc{} && r.ptr == e->value.data() + e->value.size() ? v : fallback;
}

bool config::get_bool(std::string_view section, std::string_view key, bool fallback) const noexcept
{
    const entry* e = find_entry(section, clean_key(key));
    if (e == nullptr) { return fallback; }
    for (const std::string_view yes : {"true", "yes", "on", "1"}) {
        if (iequals(e->value, yes)) { return true; }
    }
    for (const std::string_view no : {"false", "no", "off", "0"}) {
        if (iequals(e->value, no)) { return false; }
    }
    return fallback;
}

bool config::has(std::string_view section, std::string_view key) const noexcept
{
    return find_entry(section, clean_key(key)) != nullptr;
}

bool config::erase(std::string_view section, std::string_view key)
{
    const std::string k = clean_key(key);
    for (section_data& s : sections_) {
        if (!iequals(s.name, section)) { continue; }
        for (auto it = s.items.begin(); it != s.items.end(); ++it) {
            if (iequals(it->key, k)) {
                s.items.erase(it);
                return true;
            }
        }
    }
    return false;
}

bool config::empty() const noexcept
{
    return std::ranges::all_of(sections_, [](const section_data& s) { return s.items.empty(); });
}

std::span<const config::entry> config::entries(std::string_view section) const noexcept
{
    const section_data* s = find_section(section);
    return s != nullptr ? std::span<const entry>{s->items} : std::span<const entry>{};
}

std::string config::to_string() const
{
    std::string out;
    const auto  write = [&](const section_data& s) {
        if (s.items.empty()) { return; }
        if (!out.empty()) { out += '\n'; }
        if (!s.name.empty()) {
            out += '[';
            out += s.name;
            out += "]\n";
        }
        for (const entry& e : s.items) {
            out += e.key;
            out += " = ";
            out += e.value;
            out += '\n';
        }
    };
    for (const section_data& s : sections_) { // the unnamed section has no header, so it has to come first
        if (s.name.empty()) { write(s); }
    }
    for (const section_data& s : sections_) {
        if (!s.name.empty()) { write(s); }
    }
    return out;
}

std::size_t config::from_string(std::string_view text)
{
    std::size_t unreadable = 0;
    std::string section;
    while (!text.empty()) {
        const std::size_t nl   = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);

        if (line.empty() || is_comment(line)) { continue; }
        if (line.front() == '[') {
            if (line.back() != ']') { ++unreadable; continue; }
            section = clean_section(line.substr(1, line.size() - 2));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || trim(line.substr(0, eq)).empty()) {
            ++unreadable;
            continue;
        }
        set(section, line.substr(0, eq), line.substr(eq + 1));
    }
    return unreadable;
}

bool config::save_file(std::string_view path) const
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, widen(path).c_str(), L"wb") != 0 || f == nullptr) {
        return false;
    }
    const std::string text = to_string();
    const bool        ok   = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

bool config::load_file(std::string_view path, std::size_t* unreadable_lines)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, widen(path).c_str(), L"rb") != 0 || f == nullptr) {
        return false;
    }
    std::string          text;
    std::array<char, 4096> buf;
    while (text.size() < max_file_bytes) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) { break; }
        text.append(buf.data(), n);
    }
    std::fclose(f);
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xef && static_cast<u8>(text[1]) == 0xbb && static_cast<u8>(text[2]) == 0xbf) {
        text.erase(0, 3); // a byte order mark from a text editor
    }
    const std::size_t bad = from_string(text);
    if (unreadable_lines != nullptr) { *unreadable_lines = bad; }
    return true;
}


// config_file ------------------------------------------------------------------------------------

config_file::~config_file()
{
    if (auto_save_ && dirty_) { (void)save(); }
}

u64 config_file::stamp() const
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!::GetFileAttributesExW(widen(path_).c_str(), GetFileExInfoStandard, &info)) { return 0; }
    const u64 time = (static_cast<u64>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    const u64 size = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return time ^ (size * 0x9e3779b97f4a7c15ull) ^ 1ull; // (never 0 for a file that exists)
}

bool config_file::load()
{
    config fresh;
    if (!fresh.load_file(path_)) { return false; }
    data_        = std::move(fresh);
    dirty_       = false;
    since_change_ = 0.0f;
    known_stamp_ = stamp();
    return true;
}

bool config_file::save()
{
    if (!data_.save_file(path_)) { return false; }
    dirty_       = false;
    since_change_ = 0.0f;
    known_stamp_ = stamp(); // our own write is not a change to react to
    ++saves_;
    return true;
}

void config_file::touch() noexcept
{
    dirty_        = true;
    since_change_ = 0.0f;
}

void config_file::set(std::string_view section, std::string_view key, std::string_view value)
{
    data_.set(section, key, value);
    touch();
}

void config_file::set_int(std::string_view section, std::string_view key, i32 value)
{
    data_.set_int(section, key, value);
    touch();
}

void config_file::set_float(std::string_view section, std::string_view key, f32 value)
{
    data_.set_float(section, key, value);
    touch();
}

void config_file::set_bool(std::string_view section, std::string_view key, bool value)
{
    data_.set_bool(section, key, value);
    touch();
}

void config_file::set_auto_save(bool on, f32 delay_seconds) noexcept
{
    auto_save_  = on;
    save_delay_ = std::max(delay_seconds, 0.0f);
}

void config_file::set_hot_reload(bool on, f32 poll_seconds) noexcept
{
    hot_reload_ = on;
    poll_every_ = std::max(poll_seconds, 0.05f);
    if (on) { known_stamp_ = known_stamp_ != 0 ? known_stamp_ : stamp(); }
}

bool config_file::update(f32 dt)
{
    if (auto_save_ && dirty_) {
        since_change_ += dt;
        if (since_change_ >= save_delay_) { (void)save(); }
    }
    if (hot_reload_ && !dirty_) {
        since_poll_ += dt;
        if (since_poll_ >= poll_every_) {
            since_poll_ = 0.0f;
            const u64 now = stamp();
            if (now != 0 && now != known_stamp_ && load()) {
                ++reloads_;
                return true;
            }
        }
    }
    return false;
}

} // namespace strata

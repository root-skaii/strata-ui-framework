#pragma once

// a small ini-like settings store:
//
//     # comments start with '#', ';' or '//'
//     volume = 0.6                 (keys before the first header are in section "")
//     [keybinds]
//     save = Ctrl+S
//     [theme]
//     accent = #ff8800
//
// names are case-insensitive, order is kept, values are one line of text. keybinds and themes store / load into a
// section, so one file can hold everything.

#include "strata/types.hpp"

#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

class config {
public:
    struct entry {
        std::string key;
        std::string value;
    };

    // creates missing sections / keys. keys are sanitised ('=' or a leading comment character); newlines in values
    // become spaces
    void set(std::string_view section, std::string_view key, std::string_view value);
    void set_int(std::string_view section, std::string_view key, i32 value);
    void set_float(std::string_view section, std::string_view key, f32 value);
    void set_bool(std::string_view section, std::string_view key, bool value);

    // return the fallback when the key is missing or does not parse. get_bool: true / yes / on / 1, false / no / off / 0
    [[nodiscard]] std::string_view get(std::string_view section, std::string_view key, std::string_view fallback = {}) const noexcept;
    [[nodiscard]] i32  get_int(std::string_view section, std::string_view key, i32 fallback = 0) const noexcept;
    [[nodiscard]] f32  get_float(std::string_view section, std::string_view key, f32 fallback = 0.0f) const noexcept;
    [[nodiscard]] bool get_bool(std::string_view section, std::string_view key, bool fallback = false) const noexcept;

    [[nodiscard]] bool has(std::string_view section, std::string_view key) const noexcept;
    bool erase(std::string_view section, std::string_view key);
    void clear() noexcept { sections_.clear(); }
    [[nodiscard]] bool empty() const noexcept;

    // a section's keys in file order
    [[nodiscard]] std::span<const entry> entries(std::string_view section) const noexcept;

    [[nodiscard]] std::string to_string() const;
    // merges: set keys replace, the rest stay. returns the number of unreadable lines
    std::size_t from_string(std::string_view text);
    // utf-8 path; false if the file cannot be written / read
    [[nodiscard]] bool save_file(std::string_view path) const;
    [[nodiscard]] bool load_file(std::string_view path, std::size_t* unreadable_lines = nullptr);

private:
    struct section_data {
        std::string        name;
        std::vector<entry> items;
    };
    [[nodiscard]] const section_data* find_section(std::string_view name) const noexcept;
    [[nodiscard]] const entry*        find_entry(std::string_view section, std::string_view key) const noexcept;

    std::vector<section_data> sections_;
};

// a config bound to a file, with two opt-in conveniences:
//   auto-save   writes shortly after the last change (and on destruction)
//   hot reload  re-reads the file when it changes on disk
// call update() once per frame; with both off it does nothing.
//
//     strata::config_file settings{"settings.ini"};
//     settings.load();
//     settings.set_auto_save(true);
//     settings.set_hot_reload(true);
//     ...
//     if (settings.update(dt)) { apply(settings.data()); }              // changed on disk
//     settings.set_float("app", "volume", v);                            // marks dirty for auto-save
//
// changes through data() need touch(). a file changed on disk is not reloaded while unsaved changes exist.
class config_file {
public:
    explicit config_file(std::string path) : path_{std::move(path)} {}
    ~config_file();
    config_file(const config_file&)            = delete;
    config_file& operator=(const config_file&) = delete;

    [[nodiscard]] config&       data() noexcept { return data_; }
    [[nodiscard]] const config& data() const noexcept { return data_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // replaces the data with the file (false, unchanged, if unreadable)
    bool load();
    bool save();

    // config's setters, marking the data changed
    void set(std::string_view section, std::string_view key, std::string_view value);
    void set_int(std::string_view section, std::string_view key, i32 value);
    void set_float(std::string_view section, std::string_view key, f32 value);
    void set_bool(std::string_view section, std::string_view key, bool value);
    void touch() noexcept;
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    void set_auto_save(bool on, f32 delay_seconds = 1.0f) noexcept;
    void set_hot_reload(bool on, f32 poll_seconds = 0.5f) noexcept;
    [[nodiscard]] bool auto_save() const noexcept { return auto_save_; }
    [[nodiscard]] bool hot_reload() const noexcept { return hot_reload_; }

    // call once per frame. true when the data was reloaded from a changed file
    bool update(f32 dt);

    [[nodiscard]] u32 reload_count() const noexcept { return reloads_; }
    [[nodiscard]] u32 save_count() const noexcept { return saves_; }

private:
    [[nodiscard]] u64 stamp() const; // file mtime and size, 0 if missing

    std::string path_;
    config      data_;
    bool        dirty_{};
    bool        auto_save_{};
    bool        hot_reload_{};
    f32         save_delay_{1.0f};
    f32         poll_every_{0.5f};
    f32         since_change_{};
    f32         since_poll_{};
    u64         known_stamp_{};
    u32         reloads_{};
    u32         saves_{};
};

} // namespace strata

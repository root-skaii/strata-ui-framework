#pragma once

// a small settings store in an ini-like text format:
//
//     # comments start with '#', ';' or '//'
//     volume = 0.6                 (keys before the first header belong to the section "")
//     [keybinds]
//     save = Ctrl+S
//     [theme]
//     accent = #ff8800
//
// section and key names are case-insensitive, order is kept (a saved file diffs well), values are plain text on one line.
// keybinds::store / load and themes::store / load put their data into a section of a config, so one file can hold
// everything an application remembers.

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

    // creates the section / key when they do not exist. keys must not contain '=' and are made safe if they do (or
    // start with a comment character); line breaks in values become spaces
    void set(std::string_view section, std::string_view key, std::string_view value);
    void set_int(std::string_view section, std::string_view key, i32 value);
    void set_float(std::string_view section, std::string_view key, f32 value);
    void set_bool(std::string_view section, std::string_view key, bool value);

    // the fallback is returned when the key is missing (or, for the typed getters, is not a valid number / bool).
    // get_bool reads true / yes / on / 1 and false / no / off / 0
    [[nodiscard]] std::string_view get(std::string_view section, std::string_view key, std::string_view fallback = {}) const noexcept;
    [[nodiscard]] i32  get_int(std::string_view section, std::string_view key, i32 fallback = 0) const noexcept;
    [[nodiscard]] f32  get_float(std::string_view section, std::string_view key, f32 fallback = 0.0f) const noexcept;
    [[nodiscard]] bool get_bool(std::string_view section, std::string_view key, bool fallback = false) const noexcept;

    [[nodiscard]] bool has(std::string_view section, std::string_view key) const noexcept;
    bool erase(std::string_view section, std::string_view key);
    void clear() noexcept { sections_.clear(); }
    [[nodiscard]] bool empty() const noexcept;

    // the keys of one section in file order (empty if there is none)
    [[nodiscard]] std::span<const entry> entries(std::string_view section) const noexcept;

    [[nodiscard]] std::string to_string() const;
    // merges: what the text sets replaces what is there, the rest stays. returns the number of lines it could not read
    std::size_t from_string(std::string_view text);
    // utf-8 path. save returns false if the file cannot be written, load if it cannot be read
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

// a config that belongs to a file, with two conveniences that are OFF until you switch them on:
//   auto-save   the file is written a moment after the last change (and when the object goes away), so nothing has to
//               remember to call save()
//   hot reload  the file is read again when it changes on disk (edited by hand, by another tool), so a theme or a
//               keybinding can be tweaked while the program runs
// call update() once per frame; with both off it does nothing and the file is only read and written by load() / save().
//
//     strata::config_file settings{"settings.ini"};
//     settings.load();
//     settings.set_auto_save(true);
//     settings.set_hot_reload(true);
//     ...
//     if (settings.update(dt)) { apply(settings.data()); }              // the file changed on disk
//     settings.set_float("app", "volume", v);                            // marks it changed: auto-save writes it
//
// changes made through data() need a touch() to count. a file that changed on disk while there are unsaved changes is
// not read until they have been written (so hot reload never throws your changes away).
class config_file {
public:
    explicit config_file(std::string path) : path_{std::move(path)} {}
    ~config_file();
    config_file(const config_file&)            = delete;
    config_file& operator=(const config_file&) = delete;

    [[nodiscard]] config&       data() noexcept { return data_; }
    [[nodiscard]] const config& data() const noexcept { return data_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // replaces the data with the file's contents (false, and nothing changes, if it cannot be read)
    bool load();
    bool save();

    // the setters of config, and mark the data changed
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

    // call once per frame with the frame time. returns true when the data was replaced from a changed file: apply it
    bool update(f32 dt);

    [[nodiscard]] u32 reload_count() const noexcept { return reloads_; }
    [[nodiscard]] u32 save_count() const noexcept { return saves_; }

private:
    [[nodiscard]] u64 stamp() const; // the file's modification time and size, 0 if it does not exist

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

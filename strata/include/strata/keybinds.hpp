#pragma once

// named actions with rebindable chords: code asks "was `save` pressed?", the user picks the key.
//
//     strata::keybinds binds;
//     binds.add("save", "Ctrl+S", "write the document");
//     binds.add("format", "Ctrl+Shift+F", "format the selection", "editor");     // only while "editor" is on
//     binds.add("goto_symbol", "Ctrl+K, Ctrl+O", "jump to a symbol");            // a chord sequence
//     ...
//     binds.set_context("editor", editor_has_focus);
//     if (binds.pressed(ui, "save")) { save(); }
//     (void)ui.menu_item("Save", binds.text("save"));              // shortcut shown in the menu
//     strata::keybind_editor(ui, binds);                          // rebinding table
//
//     strata::command_palette palette;                             // Ctrl+Shift+P: search and run any action
//     if (const std::string cmd = palette.show(ui, binds); !cmd.empty()) { run(cmd); }
//
// binds.store(cfg) / binds.load(cfg) persist bindings (register actions first: only registered ones are loaded).

#include "strata/config.hpp"
#include "strata/context.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

class keybinds {
public:
    struct action {
        std::string  name;
        std::string  description; // tooltip in keybind_editor, shown in the command palette
        std::string  context;     // empty: always available
        key_sequence chord;
        key_sequence default_chord;
    };

    // registers an action with its default in sequence_to_string() syntax ("" = unbound). with a `context` it works
    // (and shows in the palette) only while that context is on. returns its index; existing actions are left as is
    u32 add(std::string_view name, std::string_view default_chord = {}, std::string_view description = {},
            std::string_view context = {});

    [[nodiscard]] std::span<const action> actions() const noexcept { return actions_; }
    [[nodiscard]] const action*           find(std::string_view name) const noexcept;

    // false if unknown
    bool bind(std::string_view name, const key_sequence& chord);
    bool reset(std::string_view name);
    void reset_all() noexcept;

    // contexts: named modes ("editor", "viewport") that enable groups of actions; set every frame. context-less actions
    // are always on, and an active context action takes its chord from a global one
    void set_context(std::string_view context, bool active = true);
    [[nodiscard]] bool context_active(std::string_view context) const noexcept; // "" is always on
    [[nodiscard]] bool available(const action& a) const noexcept { return context_active(a.context); }

    // another action on the same chord that can be active together, or nullptr (different contexts never conflict)
    [[nodiscard]] const action* conflict(std::string_view name) const noexcept;

    // true when the chord (or last sequence step) is pressed while the context is on
    [[nodiscard]] bool pressed(const context& ui, std::string_view name) const;
    // chord as text, empty if unbound or unknown
    [[nodiscard]] std::string text(std::string_view name) const;

    // one `name = Ctrl+S` line per action, unbound = empty value
    void store(config& cfg, std::string_view section = "keybinds") const;
    // rebinds registered actions found in the section; returns the count changed. unparsable values are skipped
    std::size_t load(const config& cfg, std::string_view section = "keybinds");

private:
    std::vector<action>      actions_;
    std::vector<std::string> active_;
};

// rebinding table: click a key field and press the chord (Backspace / Delete unbinds), reset buttons for changed
// bindings, warnings for shared chords. height > 0 scrolls. true when a binding changed
bool keybind_editor(context& ui, keybinds& binds, std::string_view id = "keybinds", f32 height = 0.0f);

// fuzzy score of `query` in `text`: -1 unless its letters appear in order (case-insensitive); higher is better
// (consecutive letters and word starts score). the palette's sort key
[[nodiscard]] int fuzzy_score(std::string_view query, std::string_view text) noexcept;

// "save_all" -> "Save all"
[[nodiscard]] std::string action_title(std::string_view name);

// searchable list of available actions with shortcuts: Up / Down, Enter or click picks, Esc closes.
// a modal: call show() once per frame outside any window
class command_palette {
public:
    // opening chord (empty: only open())
    key_chord shortcut{'P', true, true, false};
    // visible result count
    int       max_rows = 9;

    void open() noexcept;
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // the action chosen this frame, or empty. the palette closes when one is
    [[nodiscard]] std::string show(context& ui, const keybinds& binds);

private:
    bool        open_{};
    bool        just_opened_{};
    int         selected_{};
    std::string query_;
};

} // namespace strata

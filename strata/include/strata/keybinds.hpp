#pragma once

// named actions with rebindable key chords, so the code asks "was `save` pressed?" and the user decides which key that is:
//
//     strata::keybinds binds;
//     binds.add("save", "Ctrl+S", "write the document");
//     binds.add("format", "Ctrl+Shift+F", "format the selection", "editor");     // only while the "editor" context is on
//     ...
//     binds.set_context("editor", editor_has_focus);
//     if (binds.pressed(ui, "save")) { save(); }
//     (void)ui.menu_item("Save", binds.text("save"));              // the shortcut shown in the menu
//     strata::keybind_editor(ui, binds);                          // a table where the user rebinds them
//
//     strata::command_palette palette;                             // Ctrl+Shift+P: search every action and run one
//     if (const std::string cmd = palette.show(ui, binds); !cmd.empty()) { run(cmd); }
//
// binds.store(cfg) / binds.load(cfg) keep the bindings in a config file (register the actions first, then load: only
// registered actions are read).

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
        std::string name;
        std::string description; // shown as a tooltip by keybind_editor and beside the name in the command palette
        std::string context;     // empty: always available
        key_chord   chord;
        key_chord   default_chord;
    };

    // registers an action with its default chord in the chord_to_string() syntax ("Ctrl+S", "F5", "" = unbound).
    // an action with a `context` only works (and shows in the palette) while that context is on. returns its index; an
    // action that is already there is left as it is
    u32 add(std::string_view name, std::string_view default_chord = {}, std::string_view description = {},
            std::string_view context = {});

    [[nodiscard]] std::span<const action> actions() const noexcept { return actions_; }
    [[nodiscard]] const action*           find(std::string_view name) const noexcept;

    // false if there is no such action
    bool bind(std::string_view name, const key_chord& chord);
    bool reset(std::string_view name);
    void reset_all() noexcept;

    // contexts: named modes ("editor", "viewport", "dialog") that switch groups of actions on. call it every frame with
    // what is true right now. an action without a context is always on, and while a context action with the same chord
    // is on it takes the key from the global one
    void set_context(std::string_view context, bool active = true);
    [[nodiscard]] bool context_active(std::string_view context) const noexcept; // "" is always on
    [[nodiscard]] bool available(const action& a) const noexcept { return context_active(a.context); }

    // another action bound to the same chord that can be on at the same time (both would fire), or nullptr. actions of
    // different contexts do not conflict with each other
    [[nodiscard]] const action* conflict(std::string_view name) const noexcept;

    // true on the frame the action's chord is pressed (same rules as context::accelerator) while its context is on
    [[nodiscard]] bool pressed(const context& ui, std::string_view name) const;
    // the chord as text ("Ctrl+S"), empty if unbound or unknown
    [[nodiscard]] std::string text(std::string_view name) const;

    // one `name = Ctrl+S` line per action, an unbound action is written as an empty value
    void store(config& cfg, std::string_view section = "keybinds") const;
    // rebinds every registered action the section mentions; returns how many it changed. values that do not parse are skipped
    std::size_t load(const config& cfg, std::string_view section = "keybinds");

private:
    std::vector<action>      actions_;
    std::vector<std::string> active_;
};

// a table of the actions with a key field to rebind each (click it, press the new chord; Backspace / Delete unbinds),
// a reset button where the binding differs from its default and a warning mark where two actions share a chord.
// height > 0 makes the body scroll. returns true when a binding changed
bool keybind_editor(context& ui, keybinds& binds, std::string_view id = "keybinds", f32 height = 0.0f);

// how well `query` matches `text`: -1 when its letters do not appear in order (ignoring case), otherwise higher is
// better (consecutive letters and word starts count). the same measure the palette sorts by
[[nodiscard]] int fuzzy_score(std::string_view query, std::string_view text) noexcept;

// "save_all" -> "Save all"
[[nodiscard]] std::string action_title(std::string_view name);

// a search box over every available action, with its shortcut: type, Up / Down, Enter (or click) to choose, Esc closes.
// it is a modal, so call show() once per frame outside any window
class command_palette {
public:
    // the chord that opens it (empty chord: only open() does)
    key_chord shortcut{'P', true, true, false};
    // how many results show at once
    int       max_rows = 9;

    void open() noexcept;
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    // returns the name of the action that was chosen this frame, or an empty string. the palette closes when one is
    [[nodiscard]] std::string show(context& ui, const keybinds& binds);

private:
    bool        open_{};
    bool        just_opened_{};
    int         selected_{};
    std::string query_;
};

} // namespace strata

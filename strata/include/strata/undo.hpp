#pragma once

// a command history for tools: each edit is recorded with the code that reverts it and the code that repeats it.
// nothing here knows about the ui; node_editor reports moves, links and deletions, the application pushes them.
//
//   strata::undo_stack history;
//   history.push([=] { pos = before; }, [=] { pos = after; }, "move node");   // after applying the edit
//   if (ui.key_pressed(strata::key::z, true)) { history.undo(); }
//   if (ui.key_pressed(strata::key::y, true)) { history.redo(); }

#include "strata/types.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace strata {

class undo_stack {
public:
    explicit undo_stack(std::size_t limit = 256) : limit_{limit} {}

    // records an edit that has already been applied; drops the redo history. with a group open it joins the group
    void push(std::function<void()> undo, std::function<void()> redo, std::string_view name = {});

    // everything pushed between begin_group() and end_group() undoes and redoes as one step (groups nest)
    void begin_group(std::string_view name = {});
    void end_group();

    [[nodiscard]] bool can_undo() const noexcept { return !done_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !undone_.empty(); }
    // false when there is nothing to do (or a group is still open)
    bool undo();
    bool redo();

    // the name of the step undo() / redo() would take, for a menu item
    [[nodiscard]] std::string_view undo_name() const noexcept { return done_.empty() ? std::string_view{} : done_.back().name; }
    [[nodiscard]] std::string_view redo_name() const noexcept { return undone_.empty() ? std::string_view{} : undone_.back().name; }

    [[nodiscard]] std::size_t size() const noexcept { return done_.size(); }
    void clear() noexcept;

private:
    struct step {
        std::function<void()> undo;
        std::function<void()> redo;
        std::string           name;
    };
    struct group {
        std::string       name;
        std::vector<step> steps;
    };

    std::vector<step>  done_;
    std::vector<step>  undone_;
    std::vector<group> open_;
    std::size_t        limit_;
};

} // namespace strata

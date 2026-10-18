#include "strata/undo.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace strata {

void undo_stack::push(std::function<void()> undo, std::function<void()> redo, std::string_view name)
{
    step s{std::move(undo), std::move(redo), std::string{name}};
    if (!open_.empty()) {
        open_.back().steps.push_back(std::move(s));
        return;
    }
    undone_.clear();
    done_.push_back(std::move(s));
    if (limit_ > 0 && done_.size() > limit_) { done_.erase(done_.begin()); }
}

void undo_stack::begin_group(std::string_view name) { open_.push_back({std::string{name}, {}}); }

void undo_stack::end_group()
{
    if (open_.empty()) { return; }
    group g = std::move(open_.back());
    open_.pop_back();
    if (g.steps.empty()) { return; }
    if (g.steps.size() == 1 && g.name.empty()) {
        push(std::move(g.steps[0].undo), std::move(g.steps[0].redo), g.steps[0].name);
        return;
    }
    auto shared = std::make_shared<std::vector<step>>(std::move(g.steps));
    push([shared] { for (auto i = shared->rbegin(); i != shared->rend(); ++i) { if (i->undo) { i->undo(); } } },
         [shared] { for (const step& s : *shared) { if (s.redo) { s.redo(); } } }, g.name);
}

bool undo_stack::undo()
{
    if (done_.empty() || !open_.empty()) { return false; }
    step s = std::move(done_.back());
    done_.pop_back();
    if (s.undo) { s.undo(); }
    undone_.push_back(std::move(s));
    return true;
}

bool undo_stack::redo()
{
    if (undone_.empty() || !open_.empty()) { return false; }
    step s = std::move(undone_.back());
    undone_.pop_back();
    if (s.redo) { s.redo(); }
    done_.push_back(std::move(s));
    return true;
}

void undo_stack::clear() noexcept
{
    done_.clear();
    undone_.clear();
    open_.clear();
}

} // namespace strata

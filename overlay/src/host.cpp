#include <strata/overlay/module.hpp>

#include <algorithm>

namespace strata::overlay {

host::~host()
{
    if (installed_.load()) { uninstall(); }
    detach_all();
}

void host::enqueue(std::unique_ptr<module> m)
{
    if (m->mode() == module_mode::always) {
        has_always_.store(true);
        if (installed_.load()) { overlay::set_draw_hidden(true); }
    }
    const std::lock_guard lock{mutex_};
    pending_.push_back(std::move(m));
}

void host::remove(module& m)
{
    m.removed_.store(true);
    if (!installed_.load()) {
        merge_pending(nullptr);
        const std::lock_guard lock{mutex_};
        std::erase_if(modules_, [](const std::unique_ptr<module>& p) { return p->removed_.load(); });
    }
    // installed: the render thread detaches it at the start of its next frame
}

// render thread (or, with no context, before install / after uninstall)
void host::merge_pending(context* ui)
{
    std::vector<std::unique_ptr<module>> incoming;
    {
        const std::lock_guard lock{mutex_};
        incoming.swap(pending_);
    }
    for (auto& m : incoming) { modules_.push_back(std::move(m)); }

    for (auto& m : modules_) {
        if (m->removed_.load()) { continue; }
        if (!m->attached_ && ui != nullptr) {
            m->attached_ = true;
            m->on_attach(*ui);
        }
    }
    const auto gone = std::remove_if(modules_.begin(), modules_.end(), [](const std::unique_ptr<module>& p) {
        if (!p->removed_.load()) { return false; }
        if (p->attached_) { p->on_detach(); }
        return true;
    });
    modules_.erase(gone, modules_.end());
}

void host::refresh_draw_hidden()
{
    const bool any = std::any_of(modules_.begin(), modules_.end(), [](const std::unique_ptr<module>& m) { return m->mode() == module_mode::always; });
    has_always_.store(any);
    overlay::set_draw_hidden(any);
}

void host::frame(context& ui, bool open)
{
    const std::size_t before = modules_.size();
    merge_pending(&ui);
    if (modules_.size() != before) { refresh_draw_hidden(); }

    for (auto& m : modules_) {
        if (m->enabled() && (open || m->mode() == module_mode::always)) { m->on_frame(ui); }
    }
}

bool host::install(const options& base)
{
    if (installed_.load()) { return true; }
    options opt = base;
    opt.ui      = {};
    opt.hud     = [this](context& ui) { frame(ui, overlay::visible()); }; // (every drawn frame; the host decides which modules are due)
    opt.draw_hidden = has_always_.load();
    if (!overlay::install(opt)) { return false; }
    installed_.store(true);
    overlay::set_draw_hidden(has_always_.load()); // (a set hud implies it: only `always` modules want hidden frames)
    return true;
}

overlay::uninstall_result host::uninstall()
{
    if (!installed_.exchange(false)) { return overlay::uninstall_result::done; }
    const overlay::uninstall_result r = overlay::uninstall(); // waits for frames in flight
    if (r == overlay::uninstall_result::busy) { // a frame may still be inside a module: keep them, retry later
        installed_.store(true);
        return r;
    }
    detach_all();
    return r;
}

void host::detach_all()
{
    const std::lock_guard lock{mutex_};
    for (auto& m : modules_) {
        if (m->attached_) { m->on_detach(); }
    }
    modules_.clear();
    pending_.clear();
    has_always_.store(false);
}

} // namespace strata::overlay

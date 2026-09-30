#pragma once

// modules: how an overlay is put together. everything the user sees is a module with a lifecycle -- the menu, an fps
// counter, markers drawn over the game -- and the host runs them. there is no separate "hud" versus "ui": a module
// draws into the same context, and whether it takes input is a property of its windows (window_flags::no_inputs) and
// of its mode.
//
//   struct fps : strata::overlay::module {
//       strata::overlay::module_mode mode() const noexcept override { return strata::overlay::module_mode::always; }
//       void on_frame(strata::context& ui) override {
//           using strata::window_flags;
//           if (auto w = ui.window("fps", {10, 10}, 110, window_flags::no_inputs | window_flags::no_title_bar | window_flags::no_background)) {
//               ui.textf("{} frames", strata::overlay::frames());
//           }
//       }
//   };
//
//   strata::overlay::host overlay;
//   overlay.add<fps>();
//   overlay.add<my_menu>();
//   overlay.install({.toggle_key = 0x70});     // F1 opens / closes the overlay; `always` modules run regardless
//
// the hook under the host is per process (a swap chain's vtable exists once), so there is one installed host at a time.

#include <strata/overlay/overlay.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace strata::overlay {

enum class module_mode : unsigned char {
    when_open, // only while the overlay is open (a menu, a tool window): the ui has input
    always,    // on every frame the game presents, open or not. while closed the ui gets no input and the game keeps its
               // mouse and keyboard: build it from no_inputs windows or ui.layer(layer::foreground)
};

class module {
public:
    module() = default;
    virtual ~module() = default;

    module(const module&)            = delete;
    module& operator=(const module&) = delete;

    // once, on the render thread, before the module's first frame
    virtual void on_attach(context&) {}
    // every frame the module is due (see mode()), on the render thread, between begin_frame and end_frame
    virtual void on_frame(context& ui) = 0;
    // once, when the module is removed or the host uninstalls. the context may already be gone
    virtual void on_detach() {}

    [[nodiscard]] virtual std::string_view name() const noexcept { return {}; }
    [[nodiscard]] virtual module_mode      mode() const noexcept { return module_mode::when_open; }

    // a disabled module keeps its state but gets no frames. any thread
    void set_enabled(bool on) noexcept { enabled_.store(on); }
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(); }

private:
    friend class host;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> removed_{};
    bool              attached_{};
};

class host {
public:
    host() = default;
    ~host();

    host(const host&)            = delete;
    host& operator=(const host&) = delete;

    // constructs a module in place and returns it (the host owns it). any thread, before or after install(); the
    // module's first frame comes on the next drawn frame. the reference stays valid until remove() or uninstall()
    template <class M, class... Args>
        requires std::is_base_of_v<module, M>
    M& add(Args&&... args)
    {
        auto  owned = std::make_unique<M>(std::forward<Args>(args)...);
        M&    ref   = *owned;
        enqueue(std::move(owned));
        return ref;
    }

    // detaches and destroys the module on the render thread (immediately when not installed). any thread. the
    // reference is dangling afterwards
    void remove(module& m);

    // installs the hook and starts running modules. `base` supplies the rest of the overlay options (toggle key, scale,
    // hdr ...); its ui / hud callbacks are ignored. false: see last_error()
    bool install(const options& base = {});
    // removes the hook, detaches every module and destroys it
    void uninstall();
    [[nodiscard]] bool installed() const noexcept { return installed_.load(); }

    // one frame of modules on `ui`, between its begin_frame and end_frame: attaches new modules, drops removed ones,
    // runs the ones due (`always` ones, and `when_open` ones if `open`). what the hook calls each drawn frame; public so
    // a module set can be driven (and tested) without a game
    void frame(context& ui, bool open);

    // the overlay open / closed (what the toggle key flips)
    void show(bool visible) { overlay::show(visible); }
    [[nodiscard]] bool visible() const noexcept { return overlay::visible(); }

private:
    void enqueue(std::unique_ptr<module> m);
    void merge_pending(context* ui);
    void detach_all();
    void refresh_draw_hidden();

    std::mutex                           mutex_;     // guards pending_ (add / remove from any thread)
    std::vector<std::unique_ptr<module>> pending_;
    std::vector<std::unique_ptr<module>> modules_;   // render thread only
    std::atomic<bool>                    installed_{};
    std::atomic<bool>                    has_always_{};
};

} // namespace strata::overlay

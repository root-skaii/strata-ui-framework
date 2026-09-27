#pragma once

// a ready-made host: the window, the direct3d 11 device and swap chain, the message loop and everything an application
// otherwise writes around strata itself -- dpi changes, resizing, sleeping while nothing changes, a lost device, the
// system theme, remembering what the user arranged. link strata::app (it is a separate library: strata itself never
// creates a window or a device, so it can live inside someone else's, as in an overlay).
//
//     int main()
//     {
//         auto app = strata::app::create({.title = "tool", .state_file = "tool.ini"});
//         if (!app) { return 1; }
//         return app->run([&](strata::app&, strata::context& ui) {
//             if (auto w = ui.window("hello", {40, 40}, 300.0f)) { ui.text("hi"); }
//         });
//     }

#include "strata/backend/d3d11.hpp"
#include "strata/context.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace strata {

enum class app_error : u8 {
    window,     // the window could not be created
    device,     // no direct3d 11 device (not even WARP)
    swap_chain,
    fonts,      // the font atlas could not be built (see context_config::font)
    renderer,
};

struct app_config {
    std::string_view title = "strata";        // utf-8
    int              width = 1280;            // client size in logical pixels (the monitor's dpi scale is applied)
    int              height = 720;
    bool             vsync = true;            // off: as fast as it goes (with tearing where the display allows it)
    // sleep while nothing changes (context::next_wake_seconds): a tool that is not being touched uses no cpu or gpu
    bool             idle = true;
    // follow the system's dark / light mode, accent colour and high contrast (themes::for_appearance), also when they
    // change while the program runs. off: ui.theme is what it is
    bool             follow_system_theme = false;
    // utf-8 path: what the user arranged (windows, docking, tables, open tree nodes, scroll) is restored from it at start
    // and written back when the app ends (context::save_state). empty: nothing is remembered
    std::string_view state_file{};
    color            clear{14, 16, 22, 255};  // behind the ui
    context_config   ui{};                    // fonts, theme, limits, diagnostics
};

class app {
public:
    [[nodiscard]] static std::expected<app, app_error> create(const app_config& cfg = {});

    app(app&&) noexcept;
    app& operator=(app&&) noexcept;
    ~app();
    app(const app&)            = delete;
    app& operator=(const app&) = delete;

    // shows the window and runs until it closes (or quit()); `frame` builds the ui, between begin_frame and end_frame.
    // returns the exit code given to quit() (0 when the window was closed)
    int run(const std::function<void(app&, context&)>& frame);
    void quit(int exit_code = 0) noexcept;

    // the window's close button / Alt+F4 asks this first: false keeps the window open (a "save changes?" dialog can then
    // call quit() itself). unset: it closes
    std::function<bool(app&)> on_close_request;
    // the device was lost (or reset_device() was called) and a new one is in place: textures created before are gone --
    // make them again here. the ui, its fonts and the renderer are already back
    std::function<void(app&)> on_device_reset;

    [[nodiscard]] context&        ui() noexcept;
    [[nodiscard]] d3d11_renderer& renderer() noexcept;
    [[nodiscard]] ID3D11Device*        device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* device_context() const noexcept;
    [[nodiscard]] void*                window() const noexcept; // HWND
    // frames built so far, and the ones actually drawn (the rest found nothing changed)
    [[nodiscard]] u64 frames() const noexcept;
    [[nodiscard]] u64 frames_drawn() const noexcept;

    // replaces the device, swap chain and renderer with new ones, as after a lost device (on_device_reset follows). for a
    // switch of adapter, or to exercise the recovery path; false if a new device could not be made
    bool reset_device();

private:
    struct impl;
    explicit app(std::unique_ptr<impl> p) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

#pragma once

// a ready-made host: window, d3d11 device and swap chain, message loop, dpi changes, resizing, idling, device loss,
// system theme and saved layout. a separate library (strata itself never creates a window or device).
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
    fonts,      // font atlas could not be built
    renderer,
};

struct app_config {
    std::string_view title = "strata";        // utf-8
    int              width = 1280;            // client size, logical pixels
    int              height = 720;
    bool             vsync = true;            // off: uncapped (tearing where allowed)
    // sleep while nothing changes (context::next_wake_seconds): an idle tool uses no cpu or gpu
    bool             idle = true;
    // follow the system dark / light mode, accent and high contrast (themes::for_appearance), live. off: ui.theme as is
    bool             follow_system_theme = false;
    // utf-8 path for the saved layout (context::save_state), restored at start, written at exit. empty: none
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

    // shows the window and runs until closed or quit(); `frame` builds the ui between begin_frame and end_frame.
    // returns quit()'s code (0 when closed)
    int run(const std::function<void(app&, context&)>& frame);
    void quit(int exit_code = 0) noexcept;

    // asked on close / Alt+F4: false keeps the window open (e.g. a "save changes?" dialog that later calls quit())
    std::function<bool(app&)> on_close_request;
    // a new device replaced a lost one (or reset_device()): recreate your textures here; ui, fonts and renderer are back
    std::function<void(app&)> on_device_reset;

    [[nodiscard]] context&        ui() noexcept;
    [[nodiscard]] d3d11_renderer& renderer() noexcept;
    [[nodiscard]] ID3D11Device*        device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* device_context() const noexcept;
    [[nodiscard]] void*                window() const noexcept; // HWND
    // frames built, and frames actually drawn
    [[nodiscard]] u64 frames() const noexcept;
    [[nodiscard]] u64 frames_drawn() const noexcept;

    // replaces device, swap chain and renderer as after device loss (on_device_reset follows). false on failure
    bool reset_device();

private:
    struct impl;
    explicit app(std::unique_ptr<impl> p) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

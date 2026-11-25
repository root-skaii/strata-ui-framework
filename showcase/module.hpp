#pragma once

// Modular showcase contract. Adding a new feature demo requires:
//   1. Implementing a `demo_module` subclass in `showcase/modules/mod_<name>.cpp`
//   2. Registering its factory in `showcase/modules/registry.cpp`
//   3. Adding the `.cpp` file to `showcase/CMakeLists.txt`
// Every registered module automatically appears in both the desktop sandbox and the browser WebGPU demo.

#include <strata/strata.hpp>
#include <strata/svg.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace strata::showcase {

enum class module_category : u8 {
    studio,
    controls,
    data,
    authoring,
    visuals,
    system,
};

[[nodiscard]] constexpr std::string_view category_label(module_category c) noexcept
{
    switch (c) {
    case module_category::studio:    return "Studio";
    case module_category::controls:  return "Controls";
    case module_category::data:      return "Data & Plots";
    case module_category::authoring: return "Authoring";
    case module_category::visuals:   return "Visuals";
    case module_category::system:    return "System";
    }
    return "Other";
}

struct module_info {
    std::string_view id;
    std::string_view title;
    module_category  category{module_category::controls};
    std::string_view summary;
    vec2             default_size{480.0f, 400.0f};
};

// Shared state passed to every module each frame so panels interact like a real application.
struct demo_env {
    // Font slots (0 = primary; >0 when loaded by the host)
    font_id font_mono{0};
    font_id font_heading{0};
    font_id font_icons{0};

    [[nodiscard]] bool has_mono() const noexcept { return font_mono != 0; }
    [[nodiscard]] bool has_heading() const noexcept { return font_heading != 0; }
    [[nodiscard]] bool has_icons() const noexcept { return font_icons != 0; }

    // Host telemetry
    std::string_view backend_name{"Software"};
    bool             is_web{false};
    bool             deterministic{false};
    f32              time{0.0f};
    f32              fps{60.0f};
    f32              ui_ms{0.0f};
    u32              frame{0};

    // Playback & cross-panel state
    bool        animate{true};
    f32         speed{1.0f};
    std::string selected_entity{"Main Camera"};
    std::string selected_kind{"Camera"};
    color       accent_override{94, 139, 255, 255};
    bool        draw_backdrop{false};

    // Live material/camera preview state shared between Inspector and preview viewports
    struct preview_params {
        f32   exposure{1.35f};
        f32   roughness{0.32f};
        f32   metallic{0.85f};
        f32   fov_deg{55.0f};
        f32   bloom{1.15f};
        f32   gain{0.68f};
        color tint{94, 139, 255, 255};
        bool  shadows{true};
        int   quality{1};
        int   shader_preset{0};
    };
    preview_params preview{};

    // Shared services owned by showcase_app
    log_buffer* log{nullptr};
    keybinds*   binds{nullptr};

    // Requests consumed by showcase_app / host between frames
    bool  open_palette{false};
    int   requested_theme{-1};
    f32   requested_scale{0.0f};
    std::function<void(std::string_view)> switch_workspace;
};

class demo_module {
public:
    virtual ~demo_module() = default;

    [[nodiscard]] virtual module_info info() const = 0;

    // Called once per frame before drawing UI.
    virtual void update(demo_env& /*env*/, f32 /*dt*/) {}

    // Draws the module's UI inside the current window or gallery pane.
    virtual void draw(context& ui, demo_env& env) = 0;

    // Optional full-viewport backdrop drawn before windows (used by acrylic/glass showcases).
    virtual void draw_backdrop(context& /*ui*/, demo_env& /*env*/) {}
};

} // namespace strata::showcase

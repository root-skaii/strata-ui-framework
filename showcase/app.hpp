#pragma once

#include "showcase/module.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::showcase {

struct showcase_config {
    font_id          font_mono{0};
    font_id          font_heading{0};
    font_id          font_icons{0};
    std::string_view backend_name{"Software"};
    bool             is_web{false};
    bool             deterministic{false};
    std::string_view initial_scene{"studio"};
    int              initial_theme{0};
};

class showcase_app {
public:
    showcase_app();
    ~showcase_app() = default;

    void init(context& ui, const showcase_config& cfg);

    // Switches to a named workspace ("studio", "telemetry", "authoring", "visuals", "gallery", "productivity",
    // "palette") or focuses an individual module by its id (e.g. "charts", "nodes", "data").
    void set_scene(context& ui, std::string_view name);

    // Advances animations and emits one complete UI frame (menu bar, dockspace, modules, overlays).
    void tick(context& ui, f32 dt, f32 fps, f32 ui_ms);

    // Recommended clear color behind the UI for the active theme.
    [[nodiscard]] color clear_color(const context& ui) const noexcept;

    // Non-zero when the user requested a UI scale change from inside the demo.
    [[nodiscard]] f32 take_requested_scale() noexcept;

    [[nodiscard]] std::string_view active_workspace() const noexcept { return active_workspace_; }
    [[nodiscard]] static std::span<const std::string_view> workspace_names() noexcept;

private:
    void register_keybinds();
    void apply_theme(context& ui, int index);
    void apply_workspace_layout(context& ui);
    void draw_menu_bar(context& ui, f32& out_bar_h);
    void draw_gallery(context& ui, const rect& client);
    void run_action(context& ui, std::string_view action);
    [[nodiscard]] demo_module* find_module(std::string_view id) const noexcept;

    struct module_slot {
        std::unique_ptr<demo_module> mod;
        module_info                  info;
        bool                         visible{false};
    };

    demo_env                 env_;
    log_buffer               log_{512};
    keybinds                 binds_;
    command_palette          palette_;
    std::vector<module_slot> modules_;

    std::string active_workspace_{"studio"};
    std::string focused_module_;     // non-empty when viewing a single module via --scene <id>
    std::string gallery_selected_{"inspector"};
    std::string gallery_filter_;
    bool        gallery_mode_{false};
    bool        layout_dirty_{true};
    bool        open_palette_next_{false};
    int         theme_idx_{0};
    bool        show_metrics_{false};
    bool        show_inspector_{false};
    bool        show_draw_list_{false};
};

} // namespace strata::showcase

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
    std::string_view initial_scene{"tour"};
    int              initial_theme{-1}; // < 0 selects the default Obsidian theme
};

class showcase_app {
public:
    showcase_app();
    ~showcase_app() = default;

    void init(context& ui, const showcase_config& cfg);

    // Switches to a tour chapter ("tour"/"overview", "tour_controls", "tour_visuals", "tour_data",
    // "tour_authoring", "tour_workflow"), a docked workspace ("studio", "telemetry", "authoring",
    // "visuals", "productivity", "gallery", "palette"), or a single module id ("charts", "nodes", ...).
    void set_scene(context& ui, std::string_view name);

    // Advances animations and emits one complete UI frame.
    void tick(context& ui, f32 dt, f32 fps, f32 ui_ms);

    // Recommended clear color behind the UI for the active theme.
    [[nodiscard]] color clear_color(const context& ui) const noexcept;

    // Non-zero when the user requested a UI scale change from inside the demo.
    [[nodiscard]] f32 take_requested_scale() noexcept;

    [[nodiscard]] std::string_view active_workspace() const noexcept { return active_workspace_; }
    [[nodiscard]] static std::span<const std::string_view> workspace_names() noexcept;

private:
    enum class tour_page : u8 {
        overview,
        controls,
        visuals,
        data,
        authoring,
        studio,
        workflow,
        catalog,
    };

    void register_keybinds();
    void apply_theme(context& ui, int index);
    void apply_theme_by_name(context& ui, std::string_view name);
    void apply_workspace_layout(context& ui);
    void enter_tour_page(context& ui, tour_page page);

    void draw_tour_header(context& ui, f32 width, f32 height);
    void draw_tour_rail(context& ui, const rect& r);
    void draw_tour_footer(context& ui, const rect& r);
    void draw_tour_overview(context& ui, const rect& r);
    void draw_tour_controls(context& ui, const rect& r);
    void draw_tour_visuals(context& ui, const rect& r);
    void draw_tour_data(context& ui, const rect& r);
    void draw_tour_authoring(context& ui, const rect& r);
    void draw_tour_workflow(context& ui, const rect& r);
    void draw_material_preview(context& ui, const rect& bounds);

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

    std::string active_workspace_{"tour"};
    std::string focused_module_;     // non-empty when viewing a single module via --scene <id>
    std::string gallery_selected_{"inspector"};
    std::string gallery_filter_;
    bool        tour_mode_{true};
    tour_page   tour_page_{tour_page::overview};
    bool        gallery_mode_{false};
    bool        studio_viewport_visible_{true};
    bool        layout_dirty_{true};
    bool        open_palette_next_{false};
    int         theme_idx_{0};
    bool        show_metrics_{false};
    bool        show_inspector_{false};
    bool        show_draw_list_{false};

    // Interactive state for the Overview hero card and chapter sub-tabs
    f32         hero_radius_{14.0f};
    bool        hero_glass_{true};
    int         hero_swatch_{0};
    int         data_tab_{0};
    int         authoring_tab_{0};
};

} // namespace strata::showcase

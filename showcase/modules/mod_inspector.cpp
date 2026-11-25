#include "showcase/module.hpp"

#include <array>
#include <cmath>
#include <string>

namespace strata::showcase {
namespace {

struct inspector_module final : demo_module {
    f32   clamp_lo{18.0f};
    f32   clamp_hi{84.0f};
    f32   pan{0.50f};
    f32   pos[3]{0.0f, 1.85f, -4.20f};
    std::array<bool, 6> features{true, true, true, false, true, false};

    module_info info() const override
    {
        return {
            .id           = "inspector",
            .title        = "Inspector",
            .category     = module_category::controls,
            .summary      = "Property grid with modified-state reset, rotary knobs, range sliders, color picker, chips, and SVG icons",
            .default_size = {320.0f, 520.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        auto& p = env.preview;

        // Clean header reflecting the active entity without loud badge clutter
        ui.text(env.selected_entity);
        ui.same_line_right(76.0f);
        ui.text_dim(env.selected_kind);

        // Interactive vector icon toolbar
        static constexpr std::array<std::string_view, 6> k_icons = {
            svg_icons::play, svg_icons::pause, svg_icons::refresh,
            svg_icons::eye,  svg_icons::lock,  svg_icons::settings,
        };
        for (std::size_t i = 0; i < k_icons.size(); ++i) {
            if (i > 0) { ui.same_line(); }
            ui.push_id(static_cast<int>(i));
            const auto it = ui.custom_item("##tb_ic", {28.0f, 26.0f});
            const bool active_icon = (i == 0 && env.animate) || (i == 1 && !env.animate) || (i == 3 && p.shadows);
            if (active_icon) {
                ui.draw().rect_filled(it.bounds, color{ui.theme().accent.r, ui.theme().accent.g, ui.theme().accent.b, 32}, 5.0f);
            } else if (it.hovered) {
                ui.draw().rect_filled(it.bounds, ui.theme().widget_hover, 5.0f);
            }
            const rect cell{{it.bounds.min.x + 5.0f, it.bounds.min.y + 4.0f},
                            {it.bounds.max.x - 5.0f, it.bounds.max.y - 4.0f}};
            const color col = active_icon ? ui.theme().accent : (it.hovered ? ui.theme().text : ui.theme().text_dim);
            draw_svg_path(ui.draw(), cell, k_icons[i], svg_icons::viewbox, col, 1.65f);
            if (it.pressed) {
                if (i == 0) { env.animate = true; }
                else if (i == 1) { env.animate = false; }
                else if (i == 2) { p = demo_env::preview_params{}; }
                else if (i == 3) { p.shadows = !p.shadows; }
            }
            ui.pop_id();
        }

        ui.toggle("Live Simulation", env.animate);
        ui.same_line();
        ui.checkbox("Shadows", p.shadows);

        static constexpr std::string_view k_quality[] = {"Draft", "Balanced", "Studio"};
        ui.segmented("##quality", k_quality, p.quality);
        ui.spacing();

        ui.separator("Transform & Material");
        if (ui.begin_property_grid("insp_props", 0.38f)) {
            if (ui.property_row("Position", {.modified = std::abs(pos[1] - 1.85f) > 0.01f}, [&] {
                    ui.drag_float3("##pos", pos, 0.05f, -50.0f, 50.0f);
                })) { pos[0] = 0.0f; pos[1] = 1.85f; pos[2] = -4.20f; }

            if (ui.property_row("Exposure", {.modified = std::abs(p.exposure - 1.35f) > 0.01f}, [&] {
                    ui.drag_float("##exp", p.exposure, 0.02f, 0.2f, 4.0f);
                })) { p.exposure = 1.35f; }

            if (ui.property_row("Roughness", {.modified = std::abs(p.roughness - 0.32f) > 0.01f}, [&] {
                    ui.slider("##rough", p.roughness, 0.04f, 1.0f);
                })) { p.roughness = 0.32f; }

            if (ui.property_row("Metallic", {.modified = std::abs(p.metallic - 0.85f) > 0.01f, .mixed = false}, [&] {
                    ui.slider("##metal", p.metallic, 0.0f, 1.0f);
                })) { p.metallic = 0.85f; }

            if (ui.property_row("FOV (deg)", {.modified = std::abs(p.fov_deg - 55.0f) > 0.1f}, [&] {
                    ui.slider("##fov", p.fov_deg, 28.0f, 100.0f);
                })) { p.fov_deg = 55.0f; }

            if (ui.property_row("Tint", {}, [&] {
                    ui.color_edit("##tint", p.tint);
                })) { p.tint = color{94, 139, 255, 255}; }

            static constexpr std::string_view k_shaders[] = {
                "PBR Standard", "Clearcoat Glass", "Subsurface Skin", "Anisotropic Metal"
            };
            if (ui.property_row("Shader", {}, [&] {
                    ui.combo("##shader", p.shader_preset, k_shaders);
                })) { p.shader_preset = 0; }

            ui.end_property_grid();
        }

        ui.separator("Signal & Mix");
        ui.slider("Sim Speed", env.speed, 0.1f, 3.0f);
        ui.range_slider("LOD Range", clamp_lo, clamp_hi, 0.0f, 100.0f);

        ui.knob("Gain", p.gain, 0.0f, 1.0f, 0.68f);
        ui.same_line();
        ui.knob("Pan", pan, 0.0f, 1.0f, 0.50f);
        ui.same_line();
        ui.knob("Bloom", p.bloom, 0.2f, 2.5f, 1.15f);

        ui.separator("Pipeline Passes");
        static constexpr std::string_view k_tags[] = {"SDF", "TAA", "Bloom", "SSR", "CSM", "HDR"};
        ui.begin_flex({.gap_x = 6.0f, .gap_y = 6.0f});
        for (std::size_t i = 0; i < features.size(); ++i) {
            (void)ui.chip(k_tags[i], {.selected = &features[i]});
        }
        ui.end_flex();
    }
};

} // namespace

std::unique_ptr<demo_module> make_inspector_module()
{
    return std::make_unique<inspector_module>();
}

} // namespace strata::showcase

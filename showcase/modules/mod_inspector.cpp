#include "showcase/module.hpp"

#include <array>
#include <cmath>
#include <string>

namespace strata::showcase {
namespace {

struct inspector_module final : demo_module {
    int   quality_mode{1};
    int   shader_preset{0};
    f32   exposure{1.35f};
    f32   roughness{0.32f};
    f32   metallic{0.85f};
    f32   fov_deg{55.0f};
    f32   clamp_lo{18.0f};
    f32   clamp_hi{84.0f};
    f32   gain{0.68f};
    f32   pan{0.50f};
    f32   bloom_thresh{1.15f};
    f32   pos[3]{0.0f, 1.85f, -4.20f};
    color tint{91, 141, 255, 255};
    bool  cast_shadows{true};
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
        // Header reflecting the live selection from Scene Explorer
        if (env.has_heading()) {
            auto h = ui.with_font(env.font_heading);
            ui.text(env.selected_entity);
        } else {
            ui.text(env.selected_entity);
        }
        ui.same_line_right(76.0f);
        ui.badge(env.selected_kind, toast_kind::info);

        // Quick transport / action toolbar with built-in SVG vector icons
        const auto bar = ui.custom_item("insp_svg_bar", {0.0f, 28.0f});
        static constexpr std::array<std::string_view, 6> k_icons = {
            svg_icons::play, svg_icons::pause, svg_icons::refresh,
            svg_icons::eye,  svg_icons::lock,  svg_icons::settings,
        };
        for (std::size_t i = 0; i < k_icons.size(); ++i) {
            const f32 x = bar.bounds.min.x + static_cast<f32>(i) * 32.0f;
            const rect cell{{x, bar.bounds.min.y + 2.0f}, {x + 24.0f, bar.bounds.min.y + 26.0f}};
            const bool active_icon = (i == 0 && env.animate) || (i == 1 && !env.animate);
            const color col = active_icon ? ui.theme().accent : ui.theme().text_dim;
            draw_svg_path(ui.draw(), cell, k_icons[i], svg_icons::viewbox, col, 1.75f);
        }

        ui.toggle("Live Simulation", env.animate);
        ui.same_line();
        ui.checkbox("Shadows", cast_shadows);

        static constexpr std::string_view k_quality[] = {"Draft", "Balanced", "Studio"};
        ui.segmented("##quality", k_quality, quality_mode);
        ui.spacing();

        ui.separator("Transform & Material");
        if (ui.begin_property_grid("insp_props", 0.40f)) {
            if (ui.property_row("Position", {.modified = std::abs(pos[1] - 1.85f) > 0.01f}, [&] {
                    ui.drag_float3("##pos", pos, 0.05f, -50.0f, 50.0f);
                })) { pos[0] = 0.0f; pos[1] = 1.85f; pos[2] = -4.20f; }

            if (ui.property_row("Exposure", {.modified = std::abs(exposure - 1.0f) > 0.01f}, [&] {
                    ui.drag_float("##exp", exposure, 0.02f, 0.1f, 8.0f);
                })) { exposure = 1.0f; }

            if (ui.property_row("Roughness", {.modified = std::abs(roughness - 0.5f) > 0.01f}, [&] {
                    ui.slider("##rough", roughness, 0.0f, 1.0f);
                })) { roughness = 0.5f; }

            if (ui.property_row("Metallic", {.modified = std::abs(metallic - 0.5f) > 0.01f, .mixed = false}, [&] {
                    ui.slider("##metal", metallic, 0.0f, 1.0f);
                })) { metallic = 0.5f; }

            if (ui.property_row("FOV (deg)", {.modified = std::abs(fov_deg - 55.0f) > 0.1f}, [&] {
                    ui.slider("##fov", fov_deg, 24.0f, 110.0f);
                })) { fov_deg = 55.0f; }

            if (ui.property_row("Tint", {}, [&] {
                    ui.color_edit("##tint", tint);
                })) { tint = color{91, 141, 255, 255}; }

            static constexpr std::string_view k_shaders[] = {
                "PBR Standard", "Clearcoat Glass", "Subsurface Skin", "Anisotropic Metal"
            };
            if (ui.property_row("Shader", {}, [&] {
                    ui.combo("##shader", shader_preset, k_shaders);
                })) { shader_preset = 0; }

            ui.end_property_grid();
        }

        ui.separator("Signal & Mix");
        ui.slider("Sim Speed", env.speed, 0.1f, 3.0f);
        ui.range_slider("LOD Range", clamp_lo, clamp_hi, 0.0f, 100.0f);

        ui.knob("Gain", gain, 0.0f, 1.0f, 0.68f);
        ui.same_line();
        ui.knob("Pan", pan, 0.0f, 1.0f, 0.50f);
        ui.same_line();
        ui.knob("Bloom", bloom_thresh, 0.2f, 3.0f, 1.15f);

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

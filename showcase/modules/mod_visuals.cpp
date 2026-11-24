#include "showcase/module.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace strata::showcase {
namespace {

struct visuals_module final : demo_module {
    f32 gauge_val{0.72f};

    module_info info() const override
    {
        return {
            .id           = "visuals",
            .title        = "SDF, Gradients & Frosted Glass",
            .category     = module_category::visuals,
            .summary      = "Analytic SDF gradients, antialiased vector paths, live acrylic backdrop blur, and built-in SVG icons",
            .default_size = {580.0f, 520.0f},
        };
    }

    void draw_backdrop(context& ui, demo_env& env) override
    {
        draw_list& bg = ui.draw();
        const vec2 ds = ui.display_size();

        // Deep midnight gradient canvas
        bg.rect_gradient_angle({{0.0f, 0.0f}, ds},
                               color::from_hex(0x0b0f1eff), color::from_hex(0x16122cff), 35.0f);

        // Soft glowing radial light orbs behind the acrylic windows
        const f32 drift = env.deterministic ? 0.0f : std::sin(env.time * 0.35f) * 28.0f;
        bg.rect_gradient_radial({{ds.x * 0.04f + drift, ds.y * 0.10f}, {ds.x * 0.50f + drift, ds.y * 0.72f}},
                                color::from_hex(0x3a6ff866), color::from_hex(0x3a6ff800), 0.0f);
        bg.rect_gradient_radial({{ds.x * 0.48f - drift, ds.y * 0.02f}, {ds.x * 0.98f - drift, ds.y * 0.66f}},
                                color::from_hex(0xe8438862), color::from_hex(0xe8438800), 0.0f);
        bg.rect_gradient_radial({{ds.x * 0.25f, ds.y * 0.42f}, {ds.x * 0.78f, ds.y * 1.05f}},
                                color::from_hex(0x1cc8b452), color::from_hex(0x1cc8b400), 0.0f);

        // Subtle architectural grid
        const color grid_col{255, 255, 255, 13};
        for (f32 x = 0.0f; x < ds.x; x += 48.0f) {
            bg.line({x, 0.0f}, {x, ds.y}, grid_col, 1.0f);
        }
        for (f32 y = 0.0f; y < ds.y; y += 48.0f) {
            bg.line({0.0f, y}, {ds.x, y}, grid_col, 1.0f);
        }

        // Flowing antialiased cubic Bezier ribbons
        for (int i = 0; i < 4; ++i) {
            const f32 y0 = ds.y * (0.22f + static_cast<f32>(i) * 0.18f);
            const f32 dy = (i % 2 == 0 ? 1.0f : -1.0f) * 70.0f;
            bg.bezier_cubic({-20.0f, y0}, {ds.x * 0.33f, y0 + dy}, {ds.x * 0.66f, y0 - dy},
                            {ds.x + 20.0f, y0 + dy * 0.35f}, color{255, 255, 255, 28}, 1.5f);
        }
    }

    void draw(context& ui, demo_env& /*env*/) override
    {
        ui.text_dim("Analytic SDF Gradients (angled, linear, radial, 4-corner)");
        const auto swatch_area = ui.custom_item("grad_swatches", {0.0f, 64.0f});
        {
            draw_list& dl = ui.draw();
            const f32 gap = 10.0f;
            const f32 w   = (swatch_area.bounds.width() - gap * 3.0f) * 0.25f;
            const f32 y0  = swatch_area.bounds.min.y;
            const f32 y1  = swatch_area.bounds.max.y;
            const f32 x0  = swatch_area.bounds.min.x;

            dl.rect_gradient_angle({{x0, y0}, {x0 + w, y1}},
                                   color::from_hex(0x3b82f6ff), color::from_hex(0x14b8a6ff), 125.0f, 10.0f);
            dl.rect_gradient_angle({{x0 + (w + gap), y0}, {x0 + (w + gap) + w, y1}},
                                   color::from_hex(0x8b5cf6ff), color::from_hex(0xec4899ff), 100.0f, 10.0f);
            dl.rect_gradient_radial({{x0 + (w + gap) * 2.0f, y0}, {x0 + (w + gap) * 2.0f + w, y1}},
                                    color::from_hex(0xfde047ff), color::from_hex(0xf43f5eff), 10.0f);
            dl.rect_gradient({{x0 + (w + gap) * 3.0f, y0}, {x0 + (w + gap) * 3.0f + w, y1}},
                             color::from_hex(0xf97316ff), color::from_hex(0xef4444ff),
                             color::from_hex(0x22c55eff), color::from_hex(0x3b82f6ff));
        }

        ui.spacing();
        ui.text_dim("Antialiased Vector Primitives & Radial Gauge");
        const auto vec_area = ui.custom_item("vec_canvas", {0.0f, 110.0f});
        {
            draw_list& dl = ui.draw();
            const rect& r = vec_area.bounds;
            dl.rect_filled(r, color{12, 16, 28, 130}, 10.0f);
            dl.rect_outline(r, ui.theme().border, 10.0f, 1.0f);

            // Multi-weight polyline wave
            for (int row = 0; row < 3; ++row) {
                std::array<vec2, 9> pts{};
                const f32 by = r.min.y + 22.0f + static_cast<f32>(row) * 32.0f;
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    pts[i] = {r.min.x + 18.0f + static_cast<f32>(i) * 14.0f,
                              by + (i % 2 == 0 ? -9.0f : 9.0f)};
                }
                dl.polyline(pts, color::from_hex(0xf59e0bff), 1.25f + static_cast<f32>(row) * 1.4f, false);
            }

            // Smooth Bezier curve + circles
            const f32 mx = r.min.x + r.width() * 0.36f;
            dl.bezier_cubic({mx, r.max.y - 18.0f}, {mx + 48.0f, r.min.y + 8.0f},
                            {mx + 96.0f, r.max.y - 8.0f}, {mx + 148.0f, r.min.y + 20.0f},
                            color::from_hex(0x2dd4bfff), 2.5f);
            dl.circle({mx + 42.0f, r.max.y - 26.0f}, 11.0f, color::from_hex(0xf43f5eff), 2.2f);
            dl.circle_filled({mx + 74.0f, r.max.y - 26.0f}, 10.0f, color::from_hex(0x60a5faff));

            // Radial gauge
            const vec2 gc{r.max.x - 68.0f, r.center().y};
            const f32 start_a = std::numbers::pi_v<f32> * 0.75f;
            const f32 sweep   = std::numbers::pi_v<f32> * 1.50f;
            dl.arc(gc, 36.0f, start_a, start_a + sweep, color{255, 255, 255, 34}, 8.0f);
            dl.arc(gc, 36.0f, start_a, start_a + sweep * gauge_val, ui.theme().accent, 8.0f);
            dl.circle(gc, 20.0f, color{255, 255, 255, 42}, 1.0f);
            dl.circle_filled(gc, 4.0f, ui.theme().accent);
        }

        ui.slider("Gauge Value", gauge_val, 0.0f, 1.0f);
        ui.spacing();

        ui.separator("Live Acrylic Blur Controls");
        if (auto r = ui.row({col_weight(1.0f), col_weight(1.0f)}, 12.0f)) {
            ui.slider("Blur Radius", ui.theme().blur_radius, 0.0f, 36.0f);
            ui.row_next();
            ui.slider("Glass Opacity", ui.theme().acrylic_alpha, 0.15f, 1.0f);
        }

        if (auto c = ui.child("acrylic_surface_demo", {0.0f, 66.0f},
                              child_flags::frame | child_flags::acrylic | child_flags::no_scrollbar)) {
            ui.text("Frosted Acrylic Child Region");
            ui.text_dim("Evaluates live Gaussian backdrop blur, subtle film grain, and SDF rounded border in one pass.");
        }

        ui.separator("Zero-Dependency SVG Vector Icons (24x24)");
        const auto icon_box = ui.custom_item("svg_gallery", {0.0f, 44.0f});
        {
            static constexpr std::array<std::string_view, 14> k_all = {
                svg_icons::play,     svg_icons::pause,    svg_icons::stop,     svg_icons::refresh,
                svg_icons::search,   svg_icons::folder,   svg_icons::file,     svg_icons::settings,
                svg_icons::eye,      svg_icons::lock,     svg_icons::check,    svg_icons::plus,
                svg_icons::copy,     svg_icons::trash,
            };
            const f32 step = std::min(36.0f, icon_box.bounds.width() / static_cast<f32>(k_all.size()));
            for (std::size_t i = 0; i < k_all.size(); ++i) {
                const f32 x = icon_box.bounds.min.x + static_cast<f32>(i) * step;
                const rect tile{{x, icon_box.bounds.min.y + 4.0f}, {x + 28.0f, icon_box.bounds.min.y + 32.0f}};
                ui.draw().rect_filled(tile, ui.theme().widget_bg, 6.0f);
                const rect inner{{tile.min.x + 5.0f, tile.min.y + 5.0f}, {tile.max.x - 5.0f, tile.max.y - 5.0f}};
                draw_svg_path(ui.draw(), inner, k_all[i], svg_icons::viewbox,
                              i % 3 == 0 ? ui.theme().accent : ui.theme().text, 1.7f);
            }
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_visuals_module()
{
    return std::make_unique<visuals_module>();
}

} // namespace strata::showcase

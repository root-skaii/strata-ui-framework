#include "showcase/module.hpp"

#include <array>
#include <cmath>

namespace strata::showcase {
namespace {

struct charts_module final : demo_module {
    std::array<f32, 96> gpu_ms{};
    std::array<f32, 96> cpu_ms{};
    std::array<f32, 28> pass_bars{};
    std::array<f32, 48> heat_tiles{}; // 6x8 GPU tile occupancy
    bool                show_heatmap{true};

    charts_module()
    {
        populate(1.25f);
    }

    void populate(f32 t)
    {
        for (std::size_t i = 0; i < gpu_ms.size(); ++i) {
            const f32 x = static_cast<f32>(i) * 0.14f + t * 1.1f;
            gpu_ms[i] = 8.4f + 3.2f * std::sin(x) + 1.1f * std::cos(x * 2.7f);
            cpu_ms[i] = 5.2f + 1.9f * std::cos(x * 0.85f + 0.6f) + 0.6f * std::sin(x * 3.1f);
        }
        for (std::size_t i = 0; i < pass_bars.size(); ++i) {
            const f32 x = static_cast<f32>(i) * 0.42f + t * 0.7f;
            pass_bars[i] = 4.5f + 7.5f * (0.5f + 0.5f * std::sin(x) * std::cos(x * 0.6f));
        }
        for (u32 r = 0; r < 6; ++r) {
            for (u32 c = 0; c < 8; ++c) {
                const f32 dx = (static_cast<f32>(c) - 3.5f) * 0.45f;
                const f32 dy = (static_cast<f32>(r) - 2.5f) * 0.55f;
                const f32 radial = std::exp(-(dx * dx + dy * dy) * 0.45f);
                const f32 wave   = 0.5f + 0.5f * std::sin(t * 0.9f + static_cast<f32>(c) * 0.45f + static_cast<f32>(r) * 0.55f);
                heat_tiles[r * 8 + c] = std::clamp(0.25f + 0.45f * radial + 0.30f * wave, 0.0f, 1.0f);
            }
        }
    }

    module_info info() const override
    {
        return {
            .id           = "charts",
            .title        = "Telemetry",
            .category     = module_category::data,
            .summary      = "Multi-series interactive charts with budget ref lines, bar histograms, zoom/pan, and 2D heatmaps",
            .default_size = {620.0f, 440.0f},
        };
    }

    void update(demo_env& env, f32 /*dt*/) override
    {
        populate(env.deterministic ? 1.25f : (1.25f + env.time));
    }

    void draw(context& ui, demo_env& /*env*/) override
    {
        {
            const auto dot = ui.custom_item("##budget_dot", {12.0f, 18.0f});
            ui.draw().circle_filled(dot.bounds.center(), 4.0f, ui.theme().success);
        }
        ui.same_line();
        ui.text("60 Hz Budget");
        ui.same_line();
        ui.text_dim(std::format("·  GPU {:.1f} ms  ·  CPU {:.1f} ms  ·  0 heap allocs", gpu_ms.back(), cpu_ms.back()));
        ui.same_line_right(110.0f);
        ui.checkbox("Heatmap", show_heatmap);

        const plot_series series[2] = {
            {.name = "GPU Pass", .values = gpu_ms},
            {.name = "CPU Main", .values = cpu_ms},
        };
        const plot_ref_line refs[1] = {
            {.value = 16.6f, .vertical = false, .col = {240, 93, 108, 200}, .label = "16.6 ms"},
        };

        plot_options po;
        po.size               = {0.0f, show_heatmap ? 205.0f : 280.0f};
        po.x                  = {"frame", ""};
        po.y                  = {"time", "ms", 0.0f, 20.0f};
        po.fill               = true;
        po.zoom_pan           = true;
        po.interactive_legend = true;
        po.ref_lines          = refs;
        ui.plot("Frame Timing", series, po);

        if (show_heatmap) {
            if (auto r = ui.row({col_weight(1.1f), col_weight(1.0f)}, 14.0f)) {
                plot_options bo;
                bo.kind = plot_kind::histogram;
                bo.size = {0.0f, 155.0f};
                bo.x    = {"pass", ""};
                bo.y    = {"ms", "ms", 0.0f, 16.0f};
                const plot_series bar_s[1] = {{.name = "Pass", .values = pass_bars}};
                ui.plot("Render Pass Histogram", bar_s, bo);

                ui.row_next();
                ui.plot_heatmap("GPU Tile Occupancy (6x8)", heat_tiles, 6, 8, {0.0f, 155.0f}, 0.0f, 1.0f);
            }
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_charts_module()
{
    return std::make_unique<charts_module>();
}

} // namespace strata::showcase

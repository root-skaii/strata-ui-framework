#include "showcase/module.hpp"

#include <array>
#include <format>
#include <string>

namespace strata::showcase {
namespace {

struct data_module final : demo_module {
    static constexpr i32 k_total_rows = 100'000;
    i32 selected_row{2};
    i32 drawn_rows{0};

    module_info info() const override
    {
        return {
            .id           = "data",
            .title        = "Data Grid",
            .category     = module_category::data,
            .summary      = "100,000-row virtualized table using list_clip (O(visible) cost) with sortable columns",
            .default_size = {640.0f, 280.0f},
        };
    }

    void draw(context& ui, demo_env& /*env*/) override
    {
        {
            const auto dot = ui.custom_item("##rows_dot", {12.0f, 18.0f});
            ui.draw().circle_filled(dot.bounds.center(), 4.0f, ui.theme().success);
        }
        ui.same_line();
        ui.text("100,000 Rows");
        ui.same_line();
        ui.text_dim(std::format("·  {} visible rows built this frame  ·  Selected #{:06d}", drawn_rows, selected_row));

        static constexpr std::array<std::string_view, 5> k_passes = {
            "G-Buffer", "Shadow CSM", "Lighting", "Volumetrics", "UI SDF"
        };
        static constexpr std::array<std::string_view, 4> k_states = {
            "Optimal", "Cached", "Synced", "Warning"
        };

        drawn_rows = 0;
        if (auto t = ui.table("draw_profiler_grid", 5, table_default | table_flags::resizable, 280.0f)) {
            ui.table_setup_column("Call ID",   96.0f);
            ui.table_setup_column("Pass",      130.0f);
            ui.table_setup_column("Shader",    0.0f, 1.4f);
            ui.table_setup_column("GPU (ms)",  96.0f);
            ui.table_setup_column("Status",    106.0f);
            (void)ui.table_headers_row(0, true);

            list_clipper clip(ui, static_cast<std::size_t>(k_total_rows));
            while (clip.step()) {
                for (int row = clip.begin(); row < clip.end(); ++row) {
                    if (!ui.table_next_row()) { continue; }
                    ++drawn_rows;
                    ui.table_next_column();
                    const bool sel = (row == selected_row);
                    if (ui.selectable(std::format("#{:06d}", row), sel)) {
                        selected_row = row;
                    }
                    ui.table_next_column();
                    ui.text(k_passes[static_cast<std::size_t>(row) % k_passes.size()]);
                    ui.table_next_column();
                    ui.textf("pbr_surface_v{}.hlsl", (row % 12) + 1);
                    ui.table_next_column();
                    const f32 ms = 0.08f + static_cast<f32>((row * 37) % 140) * 0.01f;
                    ui.textf("{:.2f} ms", ms);
                    ui.table_next_column();
                    const std::size_t si = static_cast<std::size_t>(row) % k_states.size();
                    const color scol = (si == 3) ? ui.theme().warning : (si == 0 ? ui.theme().success : ui.theme().text_dim);
                    ui.text_colored(scol, k_states[si]);
                }
            }
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_data_module()
{
    return std::make_unique<data_module>();
}

} // namespace strata::showcase

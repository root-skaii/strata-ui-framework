#include "showcase/module.hpp"

#include <array>
#include <string>
#include <vector>

namespace strata::showcase {
namespace {

struct pickers_module final : demo_module {
    std::vector<std::string> in_progress{
        "SDF glyph supersampling",
        "Multi-viewport DPI scaling",
        "Vulkan swapchain resize",
        "Node graph auto-layout",
    };
    std::vector<std::string> ready_qa{
        "WebGPU frosted acrylic",
        "Zero-heap steady state",
    };
    std::string last_event{"Drag tasks between columns or drop a swatch"};

    std::array<color, 4> tokens{{
        color::from_hex(0x5b8dffff),
        color::from_hex(0x3ec694ff),
        color::from_hex(0xf0568fff),
        color::from_hex(0xf59e0bff),
    }};
    color preview_swatch{91, 141, 255, 255};

    date        release_day{2026, 11, 24};
    time_of_day release_time{14, 30, 0};

    module_info info() const override
    {
        return {
            .id           = "pickers",
            .title        = "Workflow & Pickers",
            .category     = module_category::controls,
            .summary      = "Kanban drag-and-drop lists, color token drop targets, and calendar date/time pickers",
            .default_size = {560.0f, 460.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        ui.text_dim(last_event);
        ui.spacing();

        int move_from_col = -1;
        int move_idx      = -1;
        int move_to_col   = -1;

        if (auto r = ui.row({col_weight(1.0f), col_weight(1.0f)}, 12.0f)) {
            if (auto c = ui.child("col_progress", {0.0f, 175.0f}, child_flags::frame)) {
                ui.badge("In Progress", toast_kind::info);
                ui.same_line();
                ui.text_dim(std::format("{}", in_progress.size()));
                ui.separator();
                for (int i = 0; i < static_cast<int>(in_progress.size()); ++i) {
                    const std::string& task = in_progress[static_cast<std::size_t>(i)];
                    (void)ui.selectable(task, false);
                    struct drag_task { int col; int idx; };
                    if (auto d = ui.drag_source("kanban_task", drag_task{0, i})) {
                        ui.text(task);
                    }
                }
            }
            if (auto drop = ui.drop_target("kanban_task")) {
                struct drag_task { int col; int idx; };
                const auto p = drop.as<drag_task>();
                if (p.col != 0) { move_from_col = p.col; move_idx = p.idx; move_to_col = 0; }
            }

            ui.row_next();

            if (auto c = ui.child("col_qa", {0.0f, 175.0f}, child_flags::frame)) {
                ui.badge("Ready for QA", toast_kind::success);
                ui.same_line();
                ui.text_dim(std::format("{}", ready_qa.size()));
                ui.separator();
                for (int i = 0; i < static_cast<int>(ready_qa.size()); ++i) {
                    const std::string& task = ready_qa[static_cast<std::size_t>(i)];
                    (void)ui.selectable(task, false);
                    struct drag_task { int col; int idx; };
                    if (auto d = ui.drag_source("kanban_task", drag_task{1, i})) {
                        ui.text(task);
                    }
                }
            }
            if (auto drop = ui.drop_target("kanban_task")) {
                struct drag_task { int col; int idx; };
                const auto p = drop.as<drag_task>();
                if (p.col != 1) { move_from_col = p.col; move_idx = p.idx; move_to_col = 1; }
            }
        }

        if (move_from_col >= 0 && move_idx >= 0) {
            auto& src = (move_from_col == 0) ? in_progress : ready_qa;
            auto& dst = (move_to_col == 0)   ? in_progress : ready_qa;
            if (move_idx < static_cast<int>(src.size())) {
                std::string moved = std::move(src[static_cast<std::size_t>(move_idx)]);
                src.erase(src.begin() + move_idx);
                last_event = std::format("Moved '{}' -> {}", moved, move_to_col == 0 ? "In Progress" : "Ready for QA");
                if (env.log != nullptr) { env.log->add(log_level::info, last_event); }
                dst.push_back(std::move(moved));
            }
        }

        ui.separator("Color Token Drag & Drop");
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (i > 0) { ui.same_line(); }
            ui.push_id(static_cast<int>(i));
            const auto item = ui.custom_item("swatch", {34.0f, 26.0f});
            ui.draw().rect_filled(item.bounds, tokens[i], 6.0f);
            ui.draw().rect_outline(item.bounds, ui.theme().border, 6.0f, 1.0f);
            if (auto d = ui.drag_source("color_token", tokens[i])) {
                ui.text("Drop on target");
            }
            ui.pop_id();
        }
        ui.same_line();
        const auto target = ui.custom_item("swatch_drop", {140.0f, 26.0f});
        ui.draw().rect_filled(target.bounds, preview_swatch, 6.0f);
        ui.draw().rect_outline(target.bounds, ui.theme().border, 6.0f, 1.0f);
        if (auto drop = ui.drop_target("color_token")) {
            preview_swatch = drop.as<color>();
            last_event     = "Updated material token swatch";
        }

        ui.separator("Release Schedule Pickers");
        if (auto r = ui.row({col_weight(1.0f), col_weight(1.0f)}, 12.0f)) {
            (void)ui.date_picker("Target Date", release_day);
            ui.row_next();
            (void)ui.time_picker("Deploy Window", release_time, true);
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_pickers_module()
{
    return std::make_unique<pickers_module>();
}

} // namespace strata::showcase

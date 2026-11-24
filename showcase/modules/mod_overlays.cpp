#include "showcase/module.hpp"

#include <string>
#include <vector>

namespace strata::showcase {
namespace {

struct overlays_module final : demo_module {
    std::vector<std::string> docs{"studio.cpp", "pbr_lit.hlsl", "theme.ini", "pipeline.json"};
    int  active_doc{0};
    int  doc_seq{1};
    int  backend_filter_idx{0};
    bool modal_open{false};
    bool modal_confirm_clean{true};
    std::string modal_branch{"release/v0.2"};

    module_info info() const override
    {
        return {
            .id           = "overlays",
            .title        = "Tabs & Overlays",
            .category     = module_category::system,
            .summary      = "Closable/reorderable document tabs, filtered combo, modal dialogs, toast notifications, and keybind editor",
            .default_size = {540.0f, 440.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        // Reorderable / closable document tab bar
        std::vector<tab_desc> descs;
        descs.reserve(docs.size());
        for (const auto& d : docs) { descs.push_back(tab_desc{std::string_view{d}}); }
        const tab_events ev = ui.tab_bar("open_docs", descs.data(), descs.size(), active_doc,
                                         tab_bar_flags::closable | tab_bar_flags::reorderable | tab_bar_flags::add_button);
        if (ev.closed >= 0 && docs.size() > 1 && ev.closed < static_cast<int>(docs.size())) {
            docs.erase(docs.begin() + ev.closed);
            if (active_doc >= static_cast<int>(docs.size())) {
                active_doc = static_cast<int>(docs.size()) - 1;
            }
        }
        if (ev.moved_from >= 0 && ev.moved_to >= 0 &&
            ev.moved_from < static_cast<int>(docs.size()) && ev.moved_to < static_cast<int>(docs.size())) {
            std::string moved = std::move(docs[static_cast<std::size_t>(ev.moved_from)]);
            docs.erase(docs.begin() + ev.moved_from);
            docs.insert(docs.begin() + ev.moved_to, std::move(moved));
            active_doc = ev.moved_to;
        }
        if (ev.add) {
            docs.push_back(std::format("untitled_{}.cpp", doc_seq++));
            active_doc = static_cast<int>(docs.size()) - 1;
        }

        if (!docs.empty()) {
            ui.text_dim(std::format("Active document: {} (drag tabs to reorder, click + to add)",
                                    docs[static_cast<std::size_t>(active_doc)]));
        }

        ui.separator("Filtered Combo, Modals & Toasts");
        static constexpr std::string_view k_targets[] = {
            "Direct3D 11 (Windows)", "Direct3D 12 (Windows)", "OpenGL 3.3 Core",
            "Vulkan 1.3 (SPIR-V)", "WebGPU (WASM / Dawn)", "Apple Metal", "CPU Software Rasterizer",
        };
        (void)ui.combo_filtered("Target Backend", backend_filter_idx, k_targets);

        if (ui.button("Open Release Modal")) { modal_open = true; }
        ui.same_line();
        if (ui.button("Success Toast")) {
            ui.toast("Published", "Artifact uploaded cleanly.", toast_kind::success);
        }
        ui.same_line();
        if (ui.button("Warning Toast")) {
            ui.toast("Shader Warning", "Unreferenced uniform 'u_fog_bias'.", toast_kind::warning);
        }
        ui.same_line();
        if (ui.button("Command Palette")) {
            env.open_palette = true;
        }

        if (modal_open) {
            if (auto m = ui.modal("Publish Build", {420.0f, 0.0f})) {
                ui.text("Configure release artifact before publishing:");
                ui.spacing();
                (void)ui.input_text("Branch", modal_branch);
                ui.checkbox("Clean intermediate cache before archive", modal_confirm_clean);
                ui.separator();
                if (ui.button("Publish")) {
                    ui.toast("Release Started", std::format("Building {}", modal_branch), toast_kind::info);
                    if (env.log != nullptr) { env.log->addf(log_level::info, "Published branch {}", modal_branch); }
                    modal_open = false;
                }
                ui.same_line();
                if (ui.button("Cancel")) {
                    modal_open = false;
                }
            } else {
                modal_open = false;
            }
        }

        ui.separator("Rebindable Shortcuts (click a binding to change)");
        if (env.binds != nullptr) {
            (void)keybind_editor(ui, *env.binds, "studio_keybinds", 165.0f);
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_overlays_module()
{
    return std::make_unique<overlays_module>();
}

} // namespace strata::showcase

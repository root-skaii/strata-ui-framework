#include "showcase/module.hpp"

#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace strata::showcase {
namespace {

struct scene_item {
    const char* name;
    const char* kind;
    const char* badge;
    toast_kind  badge_kind;
    bool        visible;
};

struct scene_group {
    const char*             name;
    bool                    default_open;
    std::vector<scene_item> items;
};

[[nodiscard]] bool matches_filter(std::string_view text, std::string_view filter) noexcept
{
    if (filter.empty()) { return true; }
    if (filter.size() > text.size()) { return false; }
    for (std::size_t i = 0; i + filter.size() <= text.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < filter.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(text[i + j])) !=
                std::tolower(static_cast<unsigned char>(filter[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) { return true; }
    }
    return false;
}

struct explorer_module final : demo_module {
    std::string              filter_;
    std::vector<scene_group> groups_;

    explorer_module()
    {
        groups_ = {
            {"Cameras", true, {
                {"Main Camera",  "Camera",   "HDR",  toast_kind::info,    true},
                {"Orbit Rig",    "Camera",   "DOF",  toast_kind::info,    true},
            }},
            {"Lighting", true, {
                {"Key Sun",      "Light",    "CSM",  toast_kind::warning, true},
                {"Sky Dome",     "IBL",      "EXR",  toast_kind::info,    true},
                {"Rim Fill",     "Light",    "Area", toast_kind::info,    true},
            }},
            {"Geometry", true, {
                {"Hero Mech",    "Mesh",     "LOD0", toast_kind::success, true},
                {"Studio Stage", "Mesh",     "Bake", toast_kind::success, true},
                {"Volumetric Fog","Volume",  "Ray",  toast_kind::warning, false},
            }},
            {"Materials", true, {
                {"Brushed Titanium", "PBR",  "Metal",toast_kind::info,    true},
                {"Frosted Acrylic",  "Glass","Blur", toast_kind::info,    true},
            }},
            {"Audio & FX", false, {
                {"Ambience Bus", "Audio",    "48k",  toast_kind::success, true},
                {"Bloom & Tone", "PostFX",   "ACES", toast_kind::info,    true},
            }},
        };
    }

    module_info info() const override
    {
        return {
            .id           = "explorer",
            .title        = "Scene Explorer",
            .category     = module_category::studio,
            .summary      = "Hierarchical scene outliner with live search, visibility toggles, and inspector sync",
            .default_size = {260.0f, 460.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        (void)ui.input_text("##scene_search", filter_, "Search scene...");
        ui.spacing();

        int visible_count = 0;
        int total_count   = 0;

        for (auto& grp : groups_) {
            bool any_match = filter_.empty();
            for (const auto& item : grp.items) {
                ++total_count;
                if (item.visible) { ++visible_count; }
                if (matches_filter(item.name, filter_) || matches_filter(item.kind, filter_)) {
                    any_match = true;
                }
            }
            if (!any_match) { continue; }

            const tree_flags gflags = (grp.default_open || !filter_.empty()) ? tree_flags::default_open : tree_flags::none;
            if (auto t = ui.tree(grp.name, gflags)) {
                for (auto& item : grp.items) {
                    if (!matches_filter(item.name, filter_) && !matches_filter(item.kind, filter_)) { continue; }
                    ui.push_id(item.name);
                    const bool selected = (env.selected_entity == item.name);
                    ui.set_next_item_gutter(58.0f);
                    if (ui.tree_leaf(item.name, selected)) {
                        env.selected_entity = item.name;
                        env.selected_kind   = item.kind;
                        if (env.log != nullptr) {
                            env.log->addf(log_level::debug, "Selected {} ({})", item.name, item.kind);
                        }
                    }
                    ui.same_line_right(52.0f);
                    ui.badge(item.badge, item.badge_kind);
                    ui.pop_id();
                }
            }
        }

        ui.separator();
        ui.text_dim(std::format("{} / {} active  |  {}", visible_count, total_count, env.selected_entity));
    }
};

} // namespace

std::unique_ptr<demo_module> make_explorer_module()
{
    return std::make_unique<explorer_module>();
}

} // namespace strata::showcase

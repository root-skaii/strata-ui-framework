#include "showcase/module.hpp"

#include <array>
#include <cmath>
#include <vector>

namespace strata::showcase {
namespace {

struct sequencer_module final : demo_module {
    timeline_state tl;
    std::array<timeline_clip, 2> cam_clips{{
        {6.0f,  44.0f, "Dolly Track A", {72, 148, 255, 255}},
        {48.0f, 94.0f, "Hero Orbit",    {62, 198, 148, 255}},
    }};
    std::array<timeline_clip, 2> light_clips{{
        {14.0f, 56.0f, "Key Sweep",     {240, 168, 72, 255}},
        {62.0f, 96.0f, "Rim Pulse",     {224, 96, 152, 255}},
    }};
    std::array<timeline_keyframe, 4> dof_keys{{{12.0f}, {38.0f}, {64.0f}, {86.0f}}};

    std::vector<curve_point> ease_curve{
        {.x = 0.0f,  .y = 0.06f, .in_tangent = {-0.10f, 0.0f},  .out_tangent = {0.28f, 0.02f}},
        {.x = 0.45f, .y = 0.78f, .in_tangent = {-0.18f, -0.10f},.out_tangent = {0.18f, 0.10f}},
        {.x = 1.0f,  .y = 0.94f, .in_tangent = {-0.26f, 0.0f},  .out_tangent = {0.10f, 0.0f}},
    };

    sequencer_module()
    {
        tl.time = 34.0f;
    }

    module_info info() const override
    {
        return {
            .id           = "sequencer",
            .title        = "Sequencer & Curves",
            .category     = module_category::authoring,
            .summary      = "Multi-track timeline sequencer with draggable clips/keyframes alongside a cubic Bezier curve editor",
            .default_size = {720.0f, 220.0f},
        };
    }

    void update(demo_env& env, f32 dt) override
    {
        if (!env.deterministic && env.animate) {
            tl.time += dt * 12.0f * env.speed;
            if (tl.time > 100.0f) { tl.time -= 100.0f; }
        } else if (env.deterministic) {
            tl.time = 34.0f;
        }
    }

    void draw(context& ui, demo_env& /*env*/) override
    {
        if (auto r = ui.row({col_weight(1.55f), col_weight(1.0f)}, 14.0f)) {
            if (ui.begin_timeline("Sequence", tl, {0.0f, 162.0f})) {
                (void)ui.timeline_track("Camera Rail", cam_clips);
                (void)ui.timeline_track("Lighting Rig", light_clips);
                (void)ui.timeline_track("Focus Keys", {}, dof_keys);
                ui.end_timeline();
            }
            ui.row_next();
            (void)ui.curve_editor("Ease Curve", ease_curve, {0.0f, 162.0f});
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_sequencer_module()
{
    return std::make_unique<sequencer_module>();
}

} // namespace strata::showcase

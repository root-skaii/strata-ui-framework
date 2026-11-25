// Minimal standalone desktop application using strata::app.

#include <strata/app.hpp>
#include <strata/themes.hpp>

int main()
{
    auto app = strata::app::create({
        .title               = "strata — minimal app",
        .width               = 960,
        .height              = 600,
        .follow_system_theme = true,
    });
    if (!app) { return 1; }

    bool  enabled = true;
    bool  glass   = false;
    float speed   = 1.0f;

    return app->run([&](strata::app& host, strata::context& ui) {
        if (auto w = ui.window("Inspector", {32, 32}, {340, 0})) {
            ui.textf("Drawn {} / {} frames", host.frames_drawn(), host.frames());
            ui.separator();
            ui.checkbox("Enabled", enabled);
            if (ui.checkbox("Frosted glass theme", glass)) {
                ui.theme() = glass ? strata::themes::glass() : strata::themes::midnight();
            }
            ui.slider("Speed", speed, 0.0f, 4.0f);
            if (ui.button("Reset")) {
                enabled = true;
                speed   = 1.0f;
            }
        }
    });
}

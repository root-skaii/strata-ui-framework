#include "showcase/module.hpp"

namespace strata::showcase {
namespace {

struct console_module final : demo_module {
    int counter_{1};

    module_info info() const override
    {
        return {
            .id           = "console",
            .title        = "Console",
            .category     = module_category::system,
            .summary      = "Filterable multi-level log stream with rich-text status banner",
            .default_size = {540.0f, 220.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        ui.rich_textf("<c=8a94ad>Pipeline:</c> <c=3ec694>Ready</c>  |  Backend <c=5b8dff>{}</c>  |  Selected <c=f4f6fb>{}</c>",
                      env.backend_name, env.selected_entity);
        ui.same_line_right(195.0f);
        if (ui.button("+ Info") && env.log != nullptr) {
            env.log->addf(log_level::info, "Checkpoint #{} committed (0 heap allocs)", counter_++);
        }
        ui.same_line();
        if (ui.button("+ Warn") && env.log != nullptr) {
            env.log->addf(log_level::warn, "Shader cache miss on pass #{} (recompiled)", counter_++);
        }
        ui.same_line();
        if (ui.button("Clear") && env.log != nullptr) {
            env.log->clear();
        }

        if (env.log != nullptr) {
            ui.log_view("studio_console", *env.log);
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_console_module()
{
    return std::make_unique<console_module>();
}

} // namespace strata::showcase

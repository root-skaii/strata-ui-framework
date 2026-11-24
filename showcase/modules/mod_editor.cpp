#include "showcase/module.hpp"

#include <string>

namespace strata::showcase {
namespace {

struct editor_module final : demo_module {
    std::string code =
        "// Strata Studio — Live UI & Shader Script (Ctrl+/ comment, Ctrl+F find)\n"
        "#include <strata/strata.hpp>\n"
        "\n"
        "void render_inspector(strata::context& ui, float& exposure, float& roughness) {\n"
        "    if (ui.begin_property_grid(\"material\", 0.40f)) {\n"
        "        if (ui.property_row(\"Exposure\", {.modified = exposure != 1.0f}, [&] {\n"
        "                ui.drag_float(\"##exp\", exposure, 0.02f, 0.1f, 8.0f);\n"
        "            })) {\n"
        "            exposure = 1.0f;\n"
        "        }\n"
        "        ui.property_row(\"Roughness\", {}, [&] {\n"
        "            ui.slider(\"##rough\", roughness, 0.0f, 1.0f);\n"
        "        });\n"
        "        ui.end_property_grid();\n"
        "    }\n"
        "}\n";

    std::string api_key{"sk-live-9482a7"};
    std::string build_date{"2026-11-24"};
    std::string hex_tint{"5B8DFF"};

    module_info info() const override
    {
        return {
            .id           = "editor",
            .title        = "Code Editor",
            .category     = module_category::authoring,
            .summary      = "Syntax-highlighted code editor with line numbers, Ctrl+/ comments, Ctrl+F find/replace, and masked inputs",
            .default_size = {520.0f, 440.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        ui.badge("C++23", toast_kind::info);
        ui.same_line();
        ui.text_dim("Ctrl+/ toggle comment  |  Ctrl+F find & replace  |  Tab indents");

        {
            const font_id mono = env.has_mono() ? env.font_mono : 0;
            auto fscope = ui.with_font(mono);
            (void)ui.input_code("##studio_code", code, {0.0f, 310.0f});
        }

        ui.separator("Credentials & Masked Inputs");
        if (auto r = ui.row({col_weight(1.0f), col_weight(1.0f), col_weight(0.9f)}, 10.0f)) {
            (void)ui.input_text("API Token", api_key, {}, input_flags::password | input_flags::reveal);
            ui.row_next();
            (void)ui.input_masked("Release", build_date, "####-##-##");
            ui.row_next();
            (void)ui.input_masked("Accent #", hex_tint, "XXXXXX");
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_editor_module()
{
    return std::make_unique<editor_module>();
}

} // namespace strata::showcase

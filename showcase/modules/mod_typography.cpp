#include "showcase/module.hpp"

#include <string>

namespace strata::showcase {
namespace {

struct typography_module final : demo_module {
    bool enabled_opt{true};
    bool amber_opt{true};
    int  theme_pick{0};

    module_info info() const override
    {
        return {
            .id           = "typography",
            .title        = "Typography & Rich Text",
            .category     = module_category::visuals,
            .summary      = "Inline rich-text markup, nested styles, mixed font baselines, rich widget labels, and UTF-8/RTL",
            .default_size = {440.0f, 520.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        if (env.has_heading()) {
            auto h = ui.with_font(env.font_heading);
            ui.text("Rich Text & Inline Markup");
        } else {
            ui.text("Rich Text & Inline Markup");
        }

        if (env.has_mono()) {
            ui.rich_textf(
                "Mix <c=5b8dff>accent</c>, <c=3ec694>emerald</c>, <c=f59e0b>amber</c>, and "
                "<f={}>monospace</f> runs on one baseline.\n"
                "Styles nest cleanly: <b>bold</b>, <i>italic</i>, <u>underline</u>, <s>strike</s>, and "
                "<b><i><u>combined</u></i></b>.",
                env.font_mono);
        } else {
            ui.rich_text(
                "Mix <c=5b8dff>accent</c>, <c=3ec694>emerald</c>, and <c=f59e0b>amber</c> on one baseline.\n"
                "Styles nest: <b>bold</b>, <i>italic</i>, <u>underline</u>, <s>strike</s>.");
        }

        ui.separator("Rich Widget Labels");
        {
            auto rich = ui.rich_labels();
            if (ui.button("<b>Deploy</b>  <c=9aa5bb>Ctrl+D</c>")) {
                ui.toast("Deploy Queued", "Rich-label action triggered.", toast_kind::info);
            }
            ui.same_line();
            {
                const auto danger = ui.style_overrides({
                    override_color(style_color::widget_bg, color::from_hex(0x5a1f2bff)),
                    override_color(style_color::widget_hover, color::from_hex(0x7a2a3aff)),
                    override_color(style_color::widget_border, color::from_hex(0x9a3a4eff)),
                    override_color(style_color::text, color::from_hex(0xfff2f4ff)),
                });
                (void)ui.button("<b>Purge</b>  <c=ffb3c1>Del</c>");
            }

            ui.checkbox("Enable <b>SDF</b> glyph baking", enabled_opt);
            ui.toggle("<c=f59e0b>HDR</c> tone mapping", amber_opt);

            static constexpr std::string_view k_styles[] = {
                "<c=5b8dff>Midnight Blue</c>",
                "<c=3ec694>Emerald Glass</c>",
                "<c=f0568f>Rose Accent</c>",
            };
            (void)ui.combo("Accent Style", theme_pick, k_styles);
        }

        ui.separator("Multilingual UTF-8 & Bidirectional (RTL)");
        ui.text("Latin & Polish:  zażółć gęślą jaźń · café · ñandú");
        ui.text("Cyrillic & Greek:  Привет мир · Γειά σου κόσμε");
        ui.text("Hebrew (RTL):  \u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd 2026");
        ui.text("Arabic (Joined):  \u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645");
        ui.text_dim("Kerning pairs:  AVATAR · To Wa Ty · LT · Yo. 1/2");
    }
};

} // namespace

std::unique_ptr<demo_module> make_typography_module()
{
    return std::make_unique<typography_module>();
}

} // namespace strata::showcase

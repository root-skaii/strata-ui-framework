#include "showcase/module.hpp"

#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace strata::showcase {
namespace {

[[nodiscard]] bool is_ident_start(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) != 0 || c == '_';
}

[[nodiscard]] bool is_ident_char(char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

[[nodiscard]] bool is_keyword(std::string_view w) noexcept
{
    static constexpr std::array<std::string_view, 18> k_kw = {
        "void", "if", "else", "for", "while", "return", "const", "constexpr", "auto",
        "bool", "float", "double", "int", "struct", "class", "namespace", "true", "false",
    };
    for (std::string_view k : k_kw) {
        if (w == k) { return true; }
    }
    return false;
}

[[nodiscard]] bool is_type_name(std::string_view w) noexcept
{
    static constexpr std::array<std::string_view, 8> k_types = {
        "strata", "context", "std", "string", "vec2", "color", "f32", "u32",
    };
    for (std::string_view t : k_types) {
        if (w == t) { return true; }
    }
    return false;
}

void highlight_cpp(std::string_view src, font_id mono, std::vector<text_span>& out)
{
    out.clear();
    const color col_comment = color::from_hex(0x6f9e6fff);
    const color col_preproc = color::from_hex(0xc586c0ff);
    const color col_string  = color::from_hex(0xe0af68ff);
    const color col_keyword = color::from_hex(0x7aa2f7ff);
    const color col_type    = color::from_hex(0x4ec9b0ff);
    const color col_number  = color::from_hex(0xb5cea8ff);
    const color col_func    = color::from_hex(0xdcdcaaff);

    const std::size_t n = src.size();
    std::size_t i = 0;
    while (i < n) {
        // Line comment
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            const std::size_t start = i;
            while (i < n && src[i] != '\n') { ++i; }
            out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_comment, text_flags::italic});
            continue;
        }
        // Preprocessor line (#include <...>)
        if (src[i] == '#') {
            const std::size_t start = i;
            ++i;
            while (i < n && is_ident_char(src[i])) { ++i; }
            out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_preproc, text_flags::none});
            while (i < n && src[i] == ' ') { ++i; }
            if (i < n && (src[i] == '<' || src[i] == '"')) {
                const char close = (src[i] == '<') ? '>' : '"';
                const std::size_t hs = i++;
                while (i < n && src[i] != close && src[i] != '\n') { ++i; }
                if (i < n && src[i] == close) { ++i; }
                out.push_back({static_cast<u32>(hs), static_cast<u32>(i), mono, col_string, text_flags::none});
            }
            continue;
        }
        // String literal
        if (src[i] == '"') {
            const std::size_t start = i++;
            while (i < n && src[i] != '"' && src[i] != '\n') {
                if (src[i] == '\\' && i + 1 < n) { ++i; }
                ++i;
            }
            if (i < n && src[i] == '"') { ++i; }
            out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_string, text_flags::none});
            continue;
        }
        // Numeric literal
        if (std::isdigit(static_cast<unsigned char>(src[i])) != 0) {
            const std::size_t start = i++;
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) != 0 || src[i] == '.')) { ++i; }
            out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_number, text_flags::none});
            continue;
        }
        // Identifier / keyword / type / function call
        if (is_ident_start(src[i])) {
            const std::size_t start = i++;
            while (i < n && is_ident_char(src[i])) { ++i; }
            const std::string_view word = src.substr(start, i - start);
            if (is_keyword(word)) {
                out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_keyword, text_flags::bold});
            } else if (is_type_name(word)) {
                out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_type, text_flags::none});
            } else if (i < n && src[i] == '(') {
                out.push_back({static_cast<u32>(start), static_cast<u32>(i), mono, col_func, text_flags::none});
            }
            continue;
        }
        ++i;
    }
}

struct editor_module final : demo_module {
    std::string code =
        "// Strata Studio - Live UI & Shader Script (Ctrl+/ comment, Ctrl+F find)\n"
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

    std::vector<text_span> spans;
    bool                   spans_dirty{true};

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
            if (spans_dirty) {
                highlight_cpp(code, mono, spans);
                spans_dirty = false;
            }
            auto fscope = ui.with_font(mono);
            ui.input_spans(spans);
            if (ui.input_code("##studio_code", code, {0.0f, 310.0f})) {
                spans_dirty = true;
            }
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

#include "demo2.hpp"

#include "gfx_host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>
#include <format>
#include <numbers>

using namespace strata;

void demo2_art(context& ui, demo2_state& s)
{
    draw_list& dl = ui.draw();
    const vec2 d  = ui.display_size();
    const f32  t  = s.time;

    dl.rect_gradient_angle({{0.0f, 0.0f}, d}, color::from_hex(0x0b1022ff), color::from_hex(0x1d1033ff), 60.0f);

    struct blob { f32 fx, fy, ph, r; u32 col; };
    static constexpr std::array<blob, 4> blobs = {{
        {0.18f, 0.30f, 0.0f, 260.0f, 0x3b73f0ff},
        {0.72f, 0.28f, 1.7f, 300.0f, 0xf0568fff},
        {0.48f, 0.74f, 3.1f, 280.0f, 0x19c2b4ff},
        {0.88f, 0.80f, 4.4f, 220.0f, 0xffb454ff},
    }};
    for (const blob& b : blobs) {
        const vec2  c{d.x * (b.fx + 0.04f * std::sin(t * 0.3f + b.ph)), d.y * (b.fy + 0.04f * std::cos(t * 0.25f + b.ph))};
        const color col = color::from_hex(b.col);
        dl.rect_gradient_radial({{c.x - b.r, c.y - b.r}, {c.x + b.r, c.y + b.r}}, col.scaled_alpha(0.80f), col.scaled_alpha(0.0f), b.r);
    }

    // crisp detail for the blur to eat: a grid, some curves and big letters
    const color faint = color{255, 255, 255, 26};
    for (f32 x = 0.0f; x < d.x; x += 48.0f) { dl.line({x, 0.0f}, {x, d.y}, faint, 1.0f); }
    for (f32 y = 0.0f; y < d.y; y += 48.0f) { dl.line({0.0f, y}, {d.x, y}, faint, 1.0f); }
    for (int i = 0; i < 6; ++i) {
        const f32 k = static_cast<f32>(i);
        dl.bezier_cubic({0.0f, d.y * (0.15f + 0.13f * k)}, {d.x * 0.3f, d.y * (0.0f + 0.15f * k) + 60.0f * std::sin(t * 0.4f + k)},
                        {d.x * 0.6f, d.y * (0.5f + 0.06f * k) - 60.0f * std::cos(t * 0.3f + k)}, {d.x, d.y * (0.25f + 0.1f * k)},
                        color{255, 255, 255, static_cast<u8>(60 + i * 12)}, 2.0f);
    }
    const font_id big = static_cast<font_id>(std::max(s.font_heading, 0));
    dl.text({d.x * 0.5f - 60.0f, d.y * 0.5f - 12.0f}, color{255, 255, 255, 200}, "S T R A T A", big);
    dl.text({d.x * 0.5f - 250.0f, d.y * 0.5f + 20.0f}, color{255, 255, 255, 140},
            "the quick brown fox jumps over the lazy dog 0123456789 - fine print behind frosted glass", 0);
}

void demo2_visuals(context& ui, demo2_state& s)
{
    constexpr window_flags flags = window_flags::acrylic | window_flags::resizable;
    auto w = ui.window("visuals", {40, 60}, {540, 0}, flags);
    if (!w) { return; }

    ui.text_dim("gradients: angled, radial, four corners");
    {
        constexpr vec2 size{118.0f, 62.0f};
        item_result a = ui.custom_item("g_lin0", size);
        ui.draw().rect_gradient_angle(a.bounds, color::from_hex(0x3b73f0ff), color::from_hex(0x19c2b4ff), 0.0f, 10.0f);
        ui.same_line();
        item_result b = ui.custom_item("g_lin45", size);
        ui.draw().rect_gradient_angle(b.bounds, color::from_hex(0xa78bfaff), color::from_hex(0xf0568fff), 45.0f, 10.0f);
        ui.same_line();
        item_result c = ui.custom_item("g_rad", size);
        ui.draw().rect_gradient_radial(c.bounds, color::from_hex(0xffe08aff), color::from_hex(0xf0568fff), 10.0f);
        ui.same_line();
        item_result d = ui.custom_item("g_corner", size);
        ui.draw().rect_gradient(d.bounds, color::from_hex(0xff5e5eff), color::from_hex(0xffe15eff), color::from_hex(0x5effa0ff),
                                color::from_hex(0x5eb1ffff));
    }
    ui.spacing();

    ui.text_dim("antialiased lines, curves, arcs and circles");
    {
        const item_result box = ui.custom_item("aa_box", {0.0f, 150.0f});
        draw_list& dl = ui.draw();
        const rect& r = box.bounds;
        shape_style bg;
        bg.radius      = radii(ui.theme().rounding * 0.8f);
        bg.fill_top    = color{0, 0, 0, 70};
        bg.fill_bottom = color{0, 0, 0, 70};
        bg.border      = ui.theme().border;
        bg.border_width = 1.0f;
        dl.shape(r, bg);

        for (int i = 0; i < 4; ++i) {
            const f32 y0 = r.min.y + 18.0f + static_cast<f32>(i) * 26.0f;
            std::array<vec2, 9> pts;
            for (std::size_t k = 0; k < pts.size(); ++k) {
                pts[k] = {r.min.x + 14.0f + static_cast<f32>(k) * 14.0f, y0 + ((k & 1) != 0 ? 14.0f : 0.0f)};
            }
            const f32 th = i == 0 ? 1.0f : (i == 1 ? 2.0f : (i == 2 ? 4.0f : 8.0f));
            dl.polyline(pts, color::from_hex(0xffb454ffu), th, false);
        }
        dl.bezier_cubic({r.min.x + 150.0f, r.max.y - 24.0f}, {r.min.x + 210.0f, r.min.y + 4.0f}, {r.min.x + 250.0f, r.max.y + 4.0f},
                        {r.min.x + 300.0f, r.min.y + 30.0f}, color::from_hex(0x19c2b4ffu), 3.0f);
        const vec2 gc{r.max.x - 92.0f, r.center().y + 8.0f};
        const f32  a0 = std::numbers::pi_v<f32> * 0.75f;
        const f32  sweep = std::numbers::pi_v<f32> * 1.5f;
        dl.arc(gc, 44.0f, a0, a0 + sweep, color{255, 255, 255, 50}, 9.0f);
        dl.arc(gc, 44.0f, a0, a0 + sweep * s.gauge, ui.theme().accent, 9.0f);
        dl.circle(gc, 28.0f, color{255, 255, 255, 90}, 1.0f);
        dl.circle_filled(gc, 4.0f, ui.theme().accent_hover);
        dl.circle(vec2{r.min.x + 200.0f, r.min.y + 118.0f}, 16.0f, color::from_hex(0xf0568fffu), 2.0f);
        dl.circle_filled(vec2{r.min.x + 236.0f, r.min.y + 118.0f}, 12.0f, color::from_hex(0x3b73f0ffu));
        std::array<vec2, 10> star;
        const vec2 sc{r.min.x + 296.0f, r.min.y + 108.0f};
        for (std::size_t k = 0; k < star.size(); ++k) {
            const f32 ang = std::numbers::pi_v<f32> * (-0.5f + static_cast<f32>(k) * 0.2f);
            const f32 rad = (k & 1) != 0 ? 10.0f : 24.0f;
            star[k] = sc + vec2{std::cos(ang), std::sin(ang)} * rad;
        }
        dl.polyline(star, color{255, 255, 255, 230}, 1.5f, true);
    }
    ui.slider("gauge", s.gauge, 0.0f, 1.0f);
    ui.spacing();

    ui.text_dim("acrylic: this window blurs what is behind it");
    ui.slider("blur radius", ui.theme().blur_radius, 0.0f, 48.0f);
    ui.slider("tint opacity", ui.theme().acrylic_alpha, 0.05f, 1.0f);
    if (auto panel = ui.child("acrylic_panel", {0.0f, 78.0f}, child_flags::acrylic | child_flags::frame)) {
        ui.text("an acrylic child region");
        ui.text_dim("blurred, tinted and noisy on purpose");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// drag / number inputs and plots

void demo2_inputs(context& ui, demo2_state& s)
{
    auto w = ui.window("inputs and plots", {40, 60}, {500, 0}, window_flags::resizable);
    if (!w) { return; }

    ui.text_dim("drag a number sideways (Shift = fine, Alt = coarse), click it to type");
    (void)ui.drag_float("gain", s.d_float, 0.02f, 0.0f, 4.0f, 2, " x");
    (void)ui.drag_int("count", s.d_int, 0.25f, 0, 200);
    (void)ui.drag_float3("position", s.d_pos, 0.01f, -2.0f, 2.0f, 2);
    (void)ui.drag_float2("resolution", s.d_size, 4.0f, 64.0f, 8192.0f, 0);
    ui.spacing();
    ui.text_dim("typed numbers (half-written text leaves the value alone)");
    (void)ui.input_float("ratio", s.in_float, 0.1f, 3);
    (void)ui.input_int("items", s.in_int, 1);
    ui.spacing();
    ui.text_dim("multi-select dropdowns");
    (void)ui.combo_multi("layers", s.layers, {"terrain", "water", "trees", "buildings", "roads", "labels"}, "no layers");
    (void)ui.combo_multi("channels", s.channels, {"red", "green", "blue"});
    ui.separator();

    ui.text_dim("styled contents: text_span ranges over plain text");
    {
        // *bold*, _italic_, `code` (mono font), and a # heading line: the markers stay in the text, the spans style what is between
        std::vector<text_span> spans;
        const std::string_view t = s.styled_text;
        const auto code_font = static_cast<font_id>(s.font_mono >= 0 ? s.font_mono : 0);
        const auto head_font = static_cast<font_id>(s.font_heading >= 0 ? s.font_heading : 0);
        std::size_t line = 0;
        while (line <= t.size()) {
            std::size_t end = t.find('\n', line);
            if (end == std::string_view::npos) { end = t.size(); }
            if (line < end && t[line] == '#') {
                spans.push_back({static_cast<u32>(line), static_cast<u32>(end), head_font, ui.theme().accent_hover, text_flags::none});
            } else {
                for (std::size_t i = line; i < end; ++i) {
                    const char m = t[i];
                    if (m != '*' && m != '_' && m != '`') { continue; }
                    const std::size_t close = t.find(m, i + 1);
                    if (close == std::string_view::npos || close >= end) { break; }
                    text_span sp{static_cast<u32>(i), static_cast<u32>(close + 1), 0, color{0, 0, 0, 0}, text_flags::none};
                    if (m == '*') { sp.style = text_flags::bold; sp.col = ui.theme().text; }
                    if (m == '_') { sp.style = text_flags::italic; sp.col = color{255, 180, 84, 255}; }
                    if (m == '`') { sp.font = code_font; sp.col = color{25, 194, 180, 255}; }
                    spans.push_back(sp);
                    i = close;
                }
            }
            line = end + 1;
        }
        ui.input_spans(spans);
        (void)ui.input_multiline("##styled", s.styled_text, {0.0f, 130.0f});
    }
    ui.separator();

    ui.text_dim("plots: rolling line, histogram, several series, sparklines");
    const std::span<const f32> hist{s.history};
    ui.plot_lines("signal", hist, {0.0f, 92.0f}, "rolling signal", 0.0f, 1.0f, s.history_head);
    ui.plot_histogram("histogram", s.bars, {0.0f, 70.0f}, {});

    static std::array<f32, 48> a{};
    static std::array<f32, 48> b{};
    for (std::size_t i = 0; i < a.size(); ++i) {
        const f32 x = static_cast<f32>(i) * 0.22f;
        a[i] = std::sin(x) * 0.8f;
        b[i] = std::cos(x * 0.7f + 1.0f) * 0.6f + 0.2f;
    }
    const color automatic{0, 0, 0, 0}; // alpha 0: one of the theme's series colors
    const std::array<plot_series, 2> series = {{{"sin", a, automatic}, {"cos", b, automatic}}};
    ui.plot("two series", series, {0.0f, 92.0f});

    ui.text("sparklines");
    ui.same_line();
    ui.sparkline(hist, {110.0f, 22.0f}, color::from_hex(0x19c2b4ffu), s.history_head);
    ui.same_line();
    ui.sparkline(a, {110.0f, 22.0f}, color::from_hex(0xf0568fffu));
    ui.checkbox("pause the signal", s.plot_paused);
}

// ---------------------------------------------------------------------------------------------------------------------
// charts: axes with ticks and units, area fills, wheel zoom and drag pan

void demo2_charts(context& ui, demo2_state& s)
{
    auto w = ui.window("charts", {60, 40}, {620, 0}, window_flags::resizable);
    if (!w) { return; }

    static std::array<f32, 240> cpu{};
    static std::array<f32, 240> gpu{};
    static std::array<f32, 32>  frames{};
    static bool made = false;
    if (!made) {
        made = true;
        for (std::size_t i = 0; i < cpu.size(); ++i) {
            const f32 t = static_cast<f32>(i) * 0.5f; // one sample every half second
            cpu[i] = 42.0f + 24.0f * std::sin(t * 0.21f) + 9.0f * std::sin(t * 1.3f);
            gpu[i] = 58.0f + 20.0f * std::sin(t * 0.13f + 1.0f) + 6.0f * std::sin(t * 2.1f);
        }
        for (std::size_t i = 0; i < frames.size(); ++i) { frames[i] = 6.0f + 9.0f * std::abs(std::sin(static_cast<f32>(i) * 0.47f)) + static_cast<f32>(i % 5); }
    }
    (void)s;

    ui.text_dim("wheel: zoom time   Ctrl+wheel: zoom value   drag: pan   double-click: reset");
    plot_options load;
    load.size       = {0.0f, 230.0f};
    load.x          = {"time", "s"};
    load.y          = {"load", "%", 0.0f, 100.0f};
    load.x_step     = 0.5f;
    load.fill       = true;
    load.zoom_pan   = true;
    const color automatic{0, 0, 0, 0}; // alpha 0: one of the theme's series colors
    const std::array<plot_series, 2> series = {{{"cpu", cpu, automatic}, {"gpu", gpu, automatic}}};
    ui.plot("load over time", series, load);

    ui.spacing();
    plot_options ft;
    ft.kind         = plot_kind::histogram;
    ft.size         = {0.0f, 150.0f};
    ft.x            = {"frame", ""};
    ft.y            = {"frame time", "ms"};
    ft.zoom_pan     = true;
    const std::array<plot_series, 1> bars = {{{"ms", frames, automatic}}};
    ui.plot("frame times", bars, ft);

    ui.spacing();
    plot_options area;
    area.size       = {0.0f, 120.0f};
    area.x          = {"", "s"};
    area.y          = {"gpu", "%"};
    area.x_step     = 0.5f;
    area.fill       = true;
    area.fill_alpha = 0.3f;
    const std::array<plot_series, 1> one = {{{"gpu", gpu, color::from_hex(0xf0568fffu)}}};
    ui.plot("gpu area", one, area);
}

// ---------------------------------------------------------------------------------------------------------------------
// right-to-left scripts, joined arabic letters, emoji

void demo2_scripts(context& ui, demo2_state& s)
{
    auto w = ui.window("scripts", {40, 40}, {520, 0}, window_flags::resizable);
    if (!w) { return; }

    ui.text_dim("hebrew and arabic: reordered, letters joined, in every widget");
    ui.text("\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd");
    ui.text("\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645");
    ui.text("Hello \u05e9\u05dc\u05d5\u05dd 123 (\u05e2\u05d5\u05dc\u05dd) world");
    ui.text("\u0644\u0627 \u0625\u0644\u0647 \u0625\u0644\u0627 \u0627\u0644\u0644\u0647");
    (void)ui.button("\u05e9\u05de\u05d5\u05e8 (Save)");
    ui.same_line();
    (void)ui.button("\u0625\u0644\u063a\u0627\u0621");
    ui.spacing();

    ui.text_dim("emoji and symbols (single-color glyphs from the fallback faces)");
    ui.text("\U0001F600 \U0001F680 \U0001F525 \U0001F4A1 \U0001F3AE \u2764 \u2605 \u2714 \u26A0");
    ui.text("skin tones and joiners take no room: \U0001F44D\U0001F3FD  \U0001F468\u200D\U0001F4BB  \u2764\uFE0F");
    ui.spacing();

    ui.text_dim("editing: the caret and the mouse follow the reordered letters");
    (void)ui.input_text("hebrew / arabic", s.rtl_line);
    (void)ui.input_multiline("mixed directions", s.mixed_text, {0.0f, 100.0f});
    ui.spacing();
    ui.text_dim("not supported: shaping of Indic and Southeast Asian scripts (Devanagari, Thai, Tamil ...)");
}

// ---------------------------------------------------------------------------------------------------------------------
// textures: mip maps, pixel formats, updates

namespace {

// a picture with fine detail and colored corners: shimmers when it is shrunk without mip maps
std::vector<u8> checker_pixels(u32 n)
{
    std::vector<u8> px(static_cast<std::size_t>(n) * n * 4);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const bool on = ((x / 2) + (y / 2)) % 2 == 0;
            u8* p = &px[(static_cast<std::size_t>(y) * n + x) * 4];
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(n);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(n);
            p[0] = static_cast<u8>(on ? 255.0f * (0.35f + 0.65f * fx) : 20.0f);
            p[1] = static_cast<u8>(on ? 255.0f * (0.35f + 0.65f * fy) : 20.0f);
            p[2] = static_cast<u8>(on ? 255.0f * (1.0f - 0.65f * fx) : 20.0f);
            p[3] = 255;
        }
    }
    return px;
}

// one smooth picture, produced in a channel order of choice
std::vector<u8> gradient_pixels(u32 n, bool bgra)
{
    std::vector<u8> px(static_cast<std::size_t>(n) * n * 4);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 fx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(n);
            const f32 fy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(n);
            const u8 r = static_cast<u8>(255.0f * fx);
            const u8 g = static_cast<u8>(255.0f * (1.0f - fy));
            const u8 b = static_cast<u8>(255.0f * fy * 0.8f);
            u8* p = &px[(static_cast<std::size_t>(y) * n + x) * 4];
            p[0] = bgra ? b : r;
            p[1] = g;
            p[2] = bgra ? r : b;
            p[3] = 255;
        }
    }
    return px;
}

std::vector<u8> mask_pixels(u32 n) // a soft ring, coverage only
{
    std::vector<u8> px(static_cast<std::size_t>(n) * n);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(n) - 0.5f;
            const f32 dy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(n) - 0.5f;
            const f32 r = std::sqrt(dx * dx + dy * dy);
            const f32 ring = 1.0f - std::clamp(std::abs(r - 0.36f) * static_cast<f32>(n) * 0.16f, 0.0f, 1.0f);
            const f32 dot = 1.0f - std::clamp((r - 0.10f) * static_cast<f32>(n) * 0.35f, 0.0f, 1.0f);
            px[static_cast<std::size_t>(y) * n + x] = static_cast<u8>(255.0f * std::max(ring, dot));
        }
    }
    return px;
}

std::vector<u8> half_pixels(u32 n) // a radial gradient in half floats
{
    std::vector<u8> px(static_cast<std::size_t>(n) * n * 8);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(n) - 0.5f;
            const f32 dy = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(n) - 0.5f;
            const f32 r = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) * 2.0f, 0.0f, 1.0f);
            const u16 v[4] = {float_to_half(r), float_to_half(r * r), float_to_half(0.2f + 0.8f * (1.0f - r)), float_to_half(1.0f)};
            std::memcpy(&px[(static_cast<std::size_t>(y) * n + x) * 8], v, sizeof(v));
        }
    }
    return px;
}

// the animated picture: plasma that depends on the frame counter (so screenshots stay reproducible)
void plasma_pixels(std::vector<u8>& px, u32 w, u32 h, u32 y0, f32 t)
{
    px.resize(static_cast<std::size_t>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const f32 fx = static_cast<f32>(x) * 0.11f;
            const f32 fy = static_cast<f32>(y + y0) * 0.11f;
            const f32 v = std::sin(fx + t) + std::sin(fy * 1.3f - t * 0.7f) + std::sin((fx + fy) * 0.7f + t * 0.5f);
            u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<u8>(127.5f + 127.5f * std::sin(v * 1.6f));
            p[1] = static_cast<u8>(127.5f + 127.5f * std::sin(v * 1.6f + 2.1f));
            p[2] = static_cast<u8>(127.5f + 127.5f * std::sin(v * 1.6f + 4.2f));
            p[3] = 255;
        }
    }
}

constexpr u32 dynamic_size = 96;

} // namespace

void demo2_textures_create(gfx_host& host, demo2_state& s)
{
    s.tex_checker      = host.create_texture(texture_desc{256, 256, texture_format::rgba8, 1, false}, checker_pixels(256));
    s.tex_checker_mips = host.create_texture(texture_desc{256, 256, texture_format::rgba8, 0, false}, checker_pixels(256));
    s.tex_mask         = host.create_texture(texture_desc{96, 96, texture_format::a8, 0, false}, mask_pixels(96));
    std::vector<u8> grey(128 * 16);
    for (u32 y = 0; y < 16; ++y) { for (u32 x = 0; x < 128; ++x) { grey[y * 128 + x] = static_cast<u8>(x * 2); } }
    s.tex_grey = host.create_texture(texture_desc{128, 16, texture_format::r8, 1, false}, grey);
    s.tex_rgba = host.create_texture(texture_desc{64, 64, texture_format::rgba8, 0, false}, gradient_pixels(64, false));
    s.tex_bgra = host.create_texture(texture_desc{64, 64, texture_format::bgra8, 0, false}, gradient_pixels(64, true));
    s.tex_half = host.create_texture(texture_desc{64, 64, texture_format::rgba16f, 0, false}, half_pixels(64));
    std::vector<u8> plasma;
    plasma_pixels(plasma, dynamic_size, dynamic_size, 0, 0.0f);
    s.tex_dynamic = host.create_texture(texture_desc{dynamic_size, dynamic_size, texture_format::rgba8, 0, true}, plasma);
}

// the picture is redrawn every frame in three strips, each an update_texture() of its own rectangle; the mip levels below
// them follow
void demo2_textures_update(gfx_host& host, demo2_state& s)
{
    if (s.tex_dynamic == 0 || !s.show_textures) { return; }
    const f32 t = static_cast<f32>(s.frame) * 0.06f;
    std::vector<u8> strip;
    for (u32 y0 = 0; y0 < dynamic_size; y0 += 32) {
        plasma_pixels(strip, dynamic_size, 32, y0, t);
        (void)host.update_texture(s.tex_dynamic, 0, y0, dynamic_size, 32, strip);
    }
}

void demo2_textures(context& ui, demo2_state& s)
{
    auto w = ui.window("textures", {40, 40}, {560, 0}, window_flags::resizable);
    if (!w) { return; }

    ui.text_dim("the same 256x256 checker shrunk to 40 px: without mip maps, with mip maps");
    ui.image(s.tex_checker, {40, 40});
    ui.same_line();
    ui.image(s.tex_checker_mips, {40, 40});
    ui.same_line();
    ui.image(s.tex_checker, {130, 130});
    ui.same_line();
    ui.image(s.tex_checker_mips, {130, 130});
    ui.spacing();

    ui.text_dim("a8: a coverage mask, tinted three ways   r8: a grey ramp");
    ui.image(s.tex_mask, {64, 64}, {0, 0}, {1, 1}, color{255, 170, 80, 255});
    ui.same_line();
    ui.image(s.tex_mask, {64, 64}, {0, 0}, {1, 1}, color{80, 200, 255, 255});
    ui.same_line();
    ui.image(s.tex_mask, {64, 64}, {0, 0}, {1, 1}, color{240, 100, 150, 255});
    ui.same_line();
    ui.image(s.tex_grey, {128, 24});
    ui.spacing();

    ui.text_dim("rgba8, bgra8 (the same picture in the other channel order), rgba16f");
    ui.image(s.tex_rgba, {96, 96});
    ui.same_line();
    ui.image(s.tex_bgra, {96, 96});
    ui.same_line();
    ui.image(s.tex_half, {96, 96});
    ui.spacing();

    ui.text_dim("updatable: redrawn in strips every frame, shown at three sizes");
    ui.image(s.tex_dynamic, {96, 96});
    ui.same_line();
    ui.image(s.tex_dynamic, {24, 24});
    ui.same_line();
    ui.image(s.tex_dynamic, {48, 48}, {0.25f, 0.25f}, {0.75f, 0.75f});
}

// ---------------------------------------------------------------------------------------------------------------------
// selectable text, the log, toasts

namespace {

void seed_log(demo2_state& s)
{
    static constexpr std::array<const char*, 12> messages = {
        "loading asset bundle 'core'", "shader cache: 148 entries, 3 stale", "device: adapter 0, feature level 11_0",
        "swap chain 1280x720, flip-discard", "font atlas 1024x1024, 9904 glyphs", "audio: no output device, using null",
        "texture 'rock_101.png' 512x512 rgba8", "mesh 'suzanne' 3968 tris, 1 material", "script 'init.lua' ran in 0.4 ms",
        "network: connection refused (127.0.0.1:7777)", "config: unknown key 'vsync_mode', ignored", "frame budget exceeded: 21.4 ms",
    };
    static constexpr std::array<log_level, 12> levels = {
        log_level::info, log_level::debug, log_level::info, log_level::info, log_level::trace, log_level::warn,
        log_level::debug, log_level::info, log_level::debug, log_level::error, log_level::warn, log_level::warn,
    };
    for (int i = 0; i < 72; ++i) {
        const std::size_t k = static_cast<std::size_t>(i) % messages.size();
        s.log.add(levels[k], std::format("{}  [{}]", messages[k], i)); // stamped with the ui time when first drawn
    }
    s.log.add(log_level::warn, "a long line: the window is narrower than this sentence, so with 'wrap' on it takes as many rows as it needs, "
                               "and the row height follows (hard line breaks work as well)");
    s.log.add(log_level::error, "stack trace:\n  at render() frame.cpp:214\n  at run() main.cpp:88\n  at main() main.cpp:301");
    s.log.view.wrap = true;
    s.log_counter = 74;
    s.log_seeded  = true;
}

} // namespace

void demo2_textlog(context& ui, demo2_state& s)
{
    if (!s.log_seeded) { seed_log(s); }

    if (auto w = ui.window("text, log and toasts", {560, 60}, {600, 560}, window_flags::resizable)) {
        ui.text_dim("selectable text: drag to select, double-click a word, Ctrl+C copies");
        {
            const auto sel = ui.selectable_text();
            ui.text("This paragraph is ordinary text() inside a selectable_text() scope, so it can be selected with the mouse and "
                    "copied, and it wraps at the width of the window like text_wrapped().");
            ui.text_dim("dim text works too - select across the two paragraphs.");
        }
        ui.separator();

        ui.text_dim("toasts");
        if (ui.button("info")) { ui.toast("A short note about something that happened.", toast_kind::info); }
        ui.same_line();
        if (ui.button("success")) { ui.toast("Saved", "profile.json was written (2.4 KB).", toast_kind::success); }
        ui.same_line();
        if (ui.button("warning")) { ui.toast("Low disk space", "Only 1.2 GB is left on this drive.", toast_kind::warning, 5.0f); }
        ui.same_line();
        if (ui.button("error")) { ui.toast("Connection lost", "Could not reach the server; retrying in 5 s.", toast_kind::error, 6.0f); }
        if (ui.button("with buttons")) {
            static constexpr std::array<std::string_view, 2> actions = {"Undo", "Details"};
            s.t_actions = ui.toast({.title = "Deleted", .text = "3 files were moved to the recycle bin.", .kind = toast_kind::warning,
                                    .seconds = 10.0f, .actions = actions});
        }
        ui.same_line();
        if (ui.button("progress")) {
            s.t_progress_v = 0.0f;
            s.t_progress   = ui.toast({.title = "Downloading", .text = "update-1.4.2.zip", .seconds = 0.0f, .progress = 0.0f});
        }
        ui.same_line();
        if (ui.button("busy")) {
            (void)ui.toast({.title = "Working", .text = "compiling shaders...", .seconds = 4.0f, .progress = toast_busy});
        }
        ui.separator();

        ui.text_dim("log view (only the visible rows are drawn)");
        if (ui.button("add 5 lines")) {
            for (int i = 0; i < 5; ++i) {
                s.log.addf(i % 4 == 3 ? log_level::warn : log_level::info, "generated line {}", ++s.log_counter);
            }
        }
        ui.same_line();
        if (ui.button("add 5000")) {
            for (int i = 0; i < 5000; ++i) { s.log.addf(log_level::debug, "bulk line {}", ++s.log_counter); }
        }
        ui.same_line();
        ui.checkbox("keep adding", s.auto_log);
        ui.log_view("log", s.log);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// menus, modals, context menus

void demo2_menus(context& ui, demo2_state& s)
{
    const auto act = [&](std::string_view what) { s.last_action = std::string{what}; };

    const bool have_icons = s.font_icons >= 0;
    const auto row = [&](std::string_view shortcut, std::string_view glyph = {}) {
        menu_item_options o;
        o.shortcut = shortcut;
        if (have_icons) {
            o.icon      = glyph;
            o.icon_font = static_cast<font_id>(s.font_icons);
        }
        return o;
    };

    // the accelerators work with every menu closed: the same strings the rows show
    if (ui.accelerator("Ctrl+N")) { act("file > new (Ctrl+N)"); }
    if (ui.accelerator("Ctrl+O")) { act("file > open (Ctrl+O)"); }
    if (ui.accelerator("Ctrl+S")) { act("file > save (Ctrl+S)"); }
    if (ui.accelerator("Ctrl+Shift+S")) { act("file > save as (Ctrl+Shift+S)"); }
    if (ui.accelerator("F5")) { act("view > refresh (F5)"); }

    // "&Open": the underlined letter is the mnemonic (Alt+F opens File; then O picks Open)
    if (auto bar = ui.main_menu_bar()) {
        if (auto m = ui.menu("&File")) {
            if (ui.menu_item("&New", row("Ctrl+N", icons::add))) { act("file > new"); }
            if (ui.menu_item("&Open...", row("Ctrl+O", icons::folder))) { act("file > open"); }
            if (auto recent = ui.menu("Open &recent")) {
                if (ui.menu_item("project_&a.strata")) { act("recent > project_a"); }
                if (ui.menu_item("project_&b.strata")) { act("recent > project_b"); }
                if (auto more = ui.menu("&More")) {
                    if (ui.menu_item("&old_project.strata")) { act("recent > more > old"); }
                }
            }
            if (ui.menu_item("&Save", row("Ctrl+S", icons::save))) { act("file > save"); }
            ui.menu_separator();
            (void)ui.menu_item("A&utosave", s.autosave, row(""));
            ui.menu_separator();
            if (ui.menu_item("&Quit", row("Alt+F4", icons::cancel))) { act("file > quit"); }
        }
        if (auto m = ui.menu("&Edit")) {
            if (ui.menu_item("&Undo", row("Ctrl+Z", icons::refresh))) { act("edit > undo"); }
            menu_item_options redo = row("Ctrl+Y");
            redo.enabled = false;
            if (ui.menu_item("&Redo", redo)) { act("edit > redo"); } // disabled
            ui.menu_separator();
            if (ui.menu_item("Cu&t", row("Ctrl+X"))) { act("edit > cut"); }
            if (ui.menu_item("&Copy", row("Ctrl+C", icons::copy))) { act("edit > copy"); }
            if (ui.menu_item("&Paste", row("Ctrl+V"))) { act("edit > paste"); }
        }
        if (auto m = ui.menu("&View")) {
            // toggles that keep the menu open, so several can be flipped in one go
            menu_item_options keep = row("G", icons::view);
            keep.keep_open = true;
            (void)ui.menu_item("Show &grid", s.show_grid, keep);
            keep.shortcut = {};
            keep.icon     = have_icons ? std::string_view{icons::info} : std::string_view{};
            (void)ui.menu_item("Show &statistics", s.show_stats, keep);
            if (auto zoom = ui.menu("&Zoom")) {
                if (ui.menu_item("&50%")) { act("view > zoom 50"); }
                if (ui.menu_item("&100%")) { act("view > zoom 100"); }
                if (ui.menu_item("&200%")) { act("view > zoom 200"); }
            }
            if (ui.menu_item("Re&fresh", row("F5", icons::refresh))) { act("view > refresh"); }
        }
        if (auto m = ui.menu("&Help")) {
            if (ui.menu_item("&About strata", row("", icons::info))) { ui.open_modal("About"); }
        }
    }

    if (auto w = ui.window("menus, modals and toasts", {40, 90}, {470, 0}, window_flags::resizable)) {
        ui.textf("last menu action: {}", s.last_action);
        ui.textf("grid {}   statistics {}   autosave {}", s.show_grid ? "on" : "off", s.show_stats ? "on" : "off", s.autosave ? "on" : "off");
        ui.separator();

        ui.text_dim("context menu: right-click the box");
        const item_result box = ui.custom_item("ctx_area", {0.0f, 60.0f});
        s.context_target = box.bounds.center();
        {
            shape_style bg;
            bg.radius       = radii(ui.theme().rounding * 0.8f);
            bg.fill_top     = ui.theme().widget_bg;
            bg.fill_bottom  = ui.theme().widget_bg;
            bg.border       = box.hovered ? ui.theme().accent : ui.theme().widget_border;
            bg.border_width = 1.0f;
            ui.draw().shape(box.bounds, bg);
            ui.draw().text({box.bounds.min.x + 14.0f, box.bounds.center().y - 9.0f}, ui.theme().text, "right-click me", 0);
        }
        if (auto m = ui.context_menu("box_menu", box.bounds)) {
            if (ui.menu_item("Rename")) { act("context > rename"); }
            if (ui.menu_item("Duplicate", "Ctrl+D")) { act("context > duplicate"); }
            if (auto more = ui.menu("Send to")) {
                if (ui.menu_item("Desktop")) { act("context > send > desktop"); }
                if (ui.menu_item("Mail")) { act("context > send > mail"); }
            }
            ui.menu_separator();
            if (ui.menu_item("Delete", "Del")) { act("context > delete"); }
        }
        ui.separator();

        ui.text_dim("modal windows and dialogs");
        if (ui.button("delete file...")) { ui.open_modal("Delete file?"); }
        ui.same_line();
        if (ui.button("settings...")) { ui.open_modal("Settings"); }
        ui.same_line();
        if (ui.button("about")) { ui.open_modal("About"); }
        ui.textf("dialog result: {}", s.dialog_status);
        ui.separator();

        ui.text_dim("toasts");
        if (ui.button("toast")) { ui.toast("Hello", "A toast from the menus window.", toast_kind::success); }
    }

    switch (ui.dialog("Delete file?", "This will permanently delete profile.json. This cannot be undone.", {"Delete", "Cancel"})) {
    case 1:  s.dialog_status = "Delete"; ui.toast("Deleted", "profile.json is gone.", toast_kind::warning); break;
    case 2:  s.dialog_status = "Cancel"; break;
    case -1: s.dialog_status = "dismissed"; break;
    default: break;
    }

    if (auto m = ui.modal("Settings", {440.0f, 0.0f}, modal_flags::esc_closes)) {
        ui.text_dim("a modal window with ordinary widgets in it");
        (void)ui.input_text("name", s.name_edit, "name");
        ui.slider("volume", s.modal_volume, 0.0f, 1.0f);
        ui.checkbox("enable sounds", s.modal_flag);
        ui.spacing();
        if (ui.button("reset...")) { ui.open_modal("Reset settings?"); } // a modal above a modal
        ui.same_line();
        if (ui.button("close")) { ui.close_modal(); }
    }
    if (ui.dialog("Reset settings?", "All settings return to their defaults.", {"Reset", "Keep"}) == 1) {
        s.modal_volume = 0.5f;
        s.modal_flag   = true;
        ui.toast("Settings reset", toast_kind::info);
    }
    if (auto m = ui.modal("About", {380.0f, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes)) {
        ui.text("strata");
        ui.text_dim("immediate-mode ui for direct3d 11 / 12");
        ui.spacing();
        if (ui.button("ok")) { ui.close_modal(); }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// key bindings and a config file

demo2_state::demo2_state()
    : file{(std::filesystem::temp_directory_path() / "strata_sandbox_config.ini").string()}
{
    binds.add("save_config", "F6", "write the settings, key bindings and theme to the config file");
    binds.add("load_config", "F7", "read them back");
    binds.add("mute", "F3", "toggle the volume");
    binds.add("greet", "F2", "show a toast with the name");
    binds.add("toggle_sidebar", "Ctrl+B", "show or hide the sidebar");
    binds.add("format_document", "Alt+Shift+F", "format the whole document", "editor");
    binds.add("run_script", "F5", "run the script", "editor");
    binds.add("reset_camera", "F5", "back to the default view", "viewport"); // the same key, another context
}

namespace {

// everything the demo remembers, in one config: a section per kind of data
void fill_config(const context& ui, const demo2_state& s, config& cfg)
{
    cfg.set("app", "name", s.user_name);
    cfg.set_float("app", "volume", s.volume);
    cfg.set_bool("app", "muted", s.muted);
    s.binds.store(cfg);
    themes::to_config(cfg, ui.theme());
}

void apply_config(context& ui, demo2_state& s, const config& cfg)
{
    s.user_name = std::string{cfg.get("app", "name", s.user_name)};
    s.volume    = cfg.get_float("app", "volume", s.volume);
    s.muted     = cfg.get_bool("app", "muted", s.muted);
    (void)s.binds.load(cfg);
    (void)themes::from_config(cfg, ui.theme());
}

// the settings changed: what the file would hold now (auto-save, when it is on, writes it a moment later)
void settings_changed(const context& ui, demo2_state& s)
{
    fill_config(ui, s, s.file.data());
    s.file.touch();
}

void do_action(context& ui, demo2_state& s, std::string_view name)
{
    s.last_bind_action = std::string{name};
    if (name == "save_config")          { s.config_request = 1; }
    else if (name == "load_config")     { s.config_request = 2; }
    else if (name == "mute")            { s.muted = !s.muted; settings_changed(ui, s); }
    else if (name == "greet")           { ui.toast("Hello", s.user_name, toast_kind::info, 2.5f); }
    else if (name == "toggle_sidebar")  { s.sidebar = !s.sidebar; }
    else if (name == "run_script")      { ui.toast("Run", "the script started", toast_kind::success, 2.0f); }
    else if (name == "format_document") { ui.toast("Format", "the document was formatted", toast_kind::info, 2.0f); }
    else if (name == "reset_camera")    { ui.toast("Camera", "the view was reset", toast_kind::info, 2.0f); }
}

void config_actions(context& ui, demo2_state& s)
{
    const f32 dt = static_cast<f32>(ui.time() - s.last_time);
    s.last_time  = ui.time();

    s.binds.set_context("editor", s.ctx_editor);
    s.binds.set_context("viewport", s.ctx_viewport);
    for (const keybinds::action& a : s.binds.actions()) {
        if (s.binds.pressed(ui, a.name)) { do_action(ui, s, a.name); }
    }
    const std::string picked = s.palette.show(ui, s.binds); // Ctrl+Shift+P
    if (!picked.empty()) { do_action(ui, s, picked); }

    if (s.config_request == 1) {
        fill_config(ui, s, s.file.data());
        s.config_status = s.file.save() ? "saved to strata_sandbox_config.ini" : "could not write the file";
    } else if (s.config_request == 2) {
        if (!s.file.load()) {
            s.config_status = "nothing to load yet: save first";
        } else {
            apply_config(ui, s, s.file.data());
            s.config_status = "loaded";
        }
    }
    s.config_request = 0;

    // the two optional conveniences, when they are switched on
    if (s.file.update(dt)) {
        apply_config(ui, s, s.file.data());
        s.config_status = "reloaded: the file changed on disk";
    }
}

} // namespace

void demo2_config(context& ui, demo2_state& s)
{
    if (auto w = ui.window("keybinds and config", {40, 30}, {560.0f, 0.0f}, window_flags::none)) {
        if (auto card = ui.card("Key bindings")) {
            ui.text_wrapped_colored(ui.theme().text_dim, "Click a key field and press the new key (with Ctrl / Shift / Alt if you like). Backspace unbinds, Esc cancels.");
            ui.spacing(4.0f);
            if (keybind_editor(ui, s.binds)) { settings_changed(ui, s); }
            ui.spacing(4.0f);
            ui.text_dim(std::format("last action: {}", s.last_bind_action));
            ui.same_line();
            if (ui.button("reset all")) { s.binds.reset_all(); settings_changed(ui, s); }
        }

        if (auto card = ui.card("Contexts and the command palette")) {
            ui.checkbox("editor context is on", s.ctx_editor);
            ui.same_line();
            ui.checkbox("viewport context is on", s.ctx_viewport);
            ui.text_dim("F5 runs the script in the editor and resets the camera in the viewport");
            if (ui.button("open the command palette")) { s.palette.open(); }
            ui.same_line();
            ui.text_dim("or press Ctrl+Shift+P");
        }

        if (auto card = ui.card("Settings")) {
            bool changed = ui.input_text("name", s.user_name);
            changed = ui.slider("volume", s.volume, 0.0f, 1.0f) || changed;
            changed = ui.checkbox("muted", s.muted) || changed;
            if (changed) { settings_changed(ui, s); }
        }

        if (auto card = ui.card("Config file")) {
            if (ui.button("save")) { s.config_request = 1; }
            ui.same_line();
            if (ui.button("load")) { s.config_request = 2; }
            ui.same_line();
            ui.text_dim(s.config_status);

            bool auto_save = s.file.auto_save();
            if (ui.checkbox("auto-save (a second after a change)", auto_save)) { s.file.set_auto_save(auto_save, 1.0f); }
            bool hot_reload = s.file.hot_reload();
            if (ui.checkbox("hot reload (edit the file in a text editor)", hot_reload)) { s.file.set_hot_reload(hot_reload, 0.5f); }
            ui.text_dim(std::format("{}, saved {}x, reloaded {}x", s.file.dirty() ? "unsaved changes" : "nothing unsaved", s.file.save_count(), s.file.reload_count()));

            config now; // what saving would write right now
            fill_config(ui, s, now);
            const std::string preview = now.to_string();
            if (preview != s.config_text) { s.config_text = preview; }
            const auto mono = ui.with_font(static_cast<font_id>(s.font_mono >= 0 ? s.font_mono : 0));
            (void)ui.input_multiline("##preview", s.config_text, {0.0f, 130.0f}, input_flags::read_only | input_flags::no_wrap);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------

void demo2_more(context& ui, demo2_state& s)
{
    s.d3.mono_font = s.font_mono;
    s.d3.scene     = s.scene;
    demo3_show(ui, s.d3);
}

void demo2_update(context& ui, demo2_state& s, float dt)
{
    ++s.frame;
    (void)dt;
    s.d3.scene         = s.scene;
    s.d3.deterministic = s.deterministic;
    demo3_update(ui, s.d3);
    config_actions(ui, s);
    if (s.scene == "config" && s.frame == 1) { // a customised binding: the reset button and the conflict mark show up
        (void)s.binds.bind("greet", {0x75, false, false, false});
        s.ctx_editor = true;
    }
    if (s.scene == "palette" && s.frame == 4) { s.palette.open(); }

    // the rolling data: a signal that is a function of the demo time, and a small histogram
    if (s.history.empty()) { s.history.assign(160, 0.5f); }
    if (s.bars.empty()) {
        for (int i = 0; i < 24; ++i) { s.bars.push_back(0.35f + 0.6f * std::abs(std::sin(static_cast<f32>(i) * 0.55f)) * (1.0f - static_cast<f32>(i) / 40.0f)); }
    }
    if (!s.plot_paused) {
        const f32 t = s.time;
        s.history[s.history_head] = 0.5f + 0.38f * std::sin(t * 2.6f) + 0.10f * std::sin(t * 9.0f);
        s.history_head = (s.history_head + 1) % static_cast<unsigned>(s.history.size());
    }
    if (const int a = ui.toast_action(s.t_actions); a >= 0) {
        s.log.addf(log_level::info, "toast button pressed: {}", a == 0 ? "Undo" : "Details");
    }
    if (ui.toast_alive(s.t_progress)) { // (a toast that is being closed by hand simply stops being fed)
        s.t_progress_v = std::min(s.t_progress_v + 0.006f, 1.0f);
        ui.toast_progress(s.t_progress, s.t_progress_v, std::format("update-1.4.2.zip  {}%", static_cast<int>(s.t_progress_v * 100.0f)));
    }
    if (s.auto_log && s.log_seeded && s.frame % 20 == 0) {
        s.log.addf(log_level::info, "tick {}", ++s.log_counter);
    }

    // scenes: things that would need a click
    if (s.scene == "modal" && s.frame == 4) { ui.open_modal("Settings"); }
    if (s.scene == "dialog" && s.frame == 4) { ui.open_modal("Delete file?"); }
    if (s.scene == "toasts" && s.frame == 4) {
        ui.toast("Saved", "profile.json was written (2.4 KB).", toast_kind::success, 30.0f);
        ui.toast("Low disk space", "Only 1.2 GB is left on this drive.", toast_kind::warning, 30.0f);
        ui.toast("Connection lost", "Could not reach the server; retrying in 5 s.", toast_kind::error, 30.0f);
        ui.toast("A short note about something that happened.", toast_kind::info, 30.0f);
        static constexpr std::array<std::string_view, 2> actions = {"Undo", "Details"};
        (void)ui.toast({.title = "Deleted", .text = "3 files were moved to the recycle bin.", .kind = toast_kind::warning, .seconds = 30.0f,
                        .actions = actions});
        (void)ui.toast({.title = "Downloading", .text = "update-1.4.2.zip  62%", .seconds = 0.0f, .progress = 0.62f});
        (void)ui.toast({.title = "Working", .text = "compiling shaders...", .seconds = 0.0f, .progress = toast_busy});
    }
}

void demo2_script(const demo2_state& s, int frame, demo2_sim& sim)
{
    demo3_script(s.d3, frame, sim.pos, sim.down);
    if (s.scene == "menus") { // open File, then hover 'Open recent' and 'More'
        if (frame == 5)  { sim.pos = {29.0f, 14.0f}; }
        if (frame == 7)  { sim.down[0] = true; }
        if (frame == 8)  { sim.down[0] = false; }
        if (frame == 14) { sim.pos = {70.0f, 100.0f}; }
        if (frame == 20) { sim.pos = {175.0f, 128.0f}; }
    } else if (s.scene == "context") { // right-click the box
        if (frame == 5) { sim.pos = s.context_target; }
        if (frame == 8) { sim.down[1] = true; }
        if (frame == 9) { sim.down[1] = false; }
        if (frame == 16) { sim.pos = s.context_target + strata::vec2{40.0f, 66.0f}; }
    } else if (s.scene == "inputs") { // hover a drag field, and a plot
        if (frame == 6) { sim.pos = {300.0f, 214.0f}; }
    } else if (s.scene == "charts") { // hover the first chart
        if (frame == 6) { sim.pos = {330.0f, 190.0f}; }
    } else if (s.scene == "dockdrag") { // pull the "images" tab out of its pane and hold it over the editor: the drop guides show
        if (frame == 6)  { sim.pos = {322.0f, 485.0f}; }
        if (frame == 8)  { sim.down[0] = true; }
        if (frame == 10) { sim.pos = {350.0f, 520.0f}; }
        if (frame == 14) { sim.pos = {560.0f, 300.0f}; }
    } else if (s.scene == "multiselect") { // open the "layers" dropdown and hover its third row
        if (frame == 5)  { sim.pos = {290.0f, 592.0f}; }
        if (frame == 7)  { sim.down[0] = true; }
        if (frame == 8)  { sim.down[0] = false; }
        if (frame == 14) { sim.pos = {200.0f, 682.0f}; }
    }
}

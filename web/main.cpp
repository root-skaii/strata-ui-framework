// Interactive browser sandbox for strata: runs natively on WebGPU (emdawnwebgpu) with an automatic CPU software
// 2D-canvas fallback when a browser has no WebGPU support.

#include <strata/backend/software.hpp>
#include <strata/backend/webgpu.hpp>
#include <strata/context.hpp>
#include <strata/log.hpp>
#include <strata/node_editor.hpp>
#include <strata/svg.hpp>
#include <strata/themes.hpp>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace strata;

struct dom_input {
    vec2            mouse_pos{-10000.0f, -10000.0f};
    std::array<bool, 3> mouse_down{};
    f32             wheel{};
    f32             wheel_x{};
    bool            ctrl{}, shift{}, alt{};
    std::string     typed;
    std::vector<key_event> keys;
    f64             last_time{};
    f32             dpr{1.0f};
    u32             phys_w{1280};
    u32             phys_h{720};
    cursor_kind     last_cursor{cursor_kind::arrow};
    bool            size_dirty{true};
};

struct demo_state {
    std::unique_ptr<context> ui;
    font_id                  mono_font{0};
    bool                     layout_seeded{false};

    // renderer mode: WebGPU primary, CPU software fallback
    bool                    using_webgpu{false};
    WGPUInstance            instance{};
    WGPUSurface             surface{};
    WGPUAdapter             adapter{};
    WGPUDevice              device{};
    WGPUQueue               queue{};
    WGPUTextureFormat       surface_format{WGPUTextureFormat_BGRA8Unorm};
    WGPUCompositeAlphaMode  alpha_mode{WGPUCompositeAlphaMode_Opaque};
    wgpu_renderer           gpu_ren;

    software_renderer sw_ren;
    std::vector<u8>   sw_rgba;

    dom_input in;
    f32       time_acc{};
    f32       fps_smooth{60.0f};
    int       theme_idx{0};
    bool      show_metrics{false};
    bool      show_inspector{false};

    // Controls & inspector
    bool        enabled{true};
    int         seg_mode{1};
    int         combo_idx{0};
    f32         speed{1.45f};
    f32         range_lo{20.0f};
    f32         range_hi{78.0f};
    f32         knob_gain{0.62f};
    f32         knob_pan{0.50f};
    f32         exposure{1.35f};
    f32         roughness{0.42f};
    color       accent_pick{91, 141, 255, 255};
    std::string text_input{"strata + WebGPU"};
    std::array<bool, 6> chip_on{true, false, true, true, false, false};

    // Charts
    std::array<f32, 64> wave_a{};
    std::array<f32, 64> wave_b{};
    std::array<f32, 36> heat_grid{};
    bool                animate_charts{true};

    // Code editor & node graph
    std::string code_src =
        "// Press Ctrl+/ to toggle comments, Ctrl+F to find\n"
        "void build_ui(strata::context& ui) {\n"
        "    if (auto w = ui.window(\"Inspector\", {24, 24}, 320)) {\n"
        "        ui.knob(\"Gain\", gain, 0.0f, 1.0f, 0.5f);\n"
        "        ui.range_slider(\"Clamp\", lo, hi, 0.0f, 100.0f);\n"
        "    }\n"
        "}\n";
    node_editor graph;
    struct demo_node { u64 id; const char* title; vec2 pos; u64 in_p; u64 out_p; };
    std::vector<demo_node> nodes{
        {1, "Signal",    {20, 30},  0, 101},
        {2, "Filter",    {200, 20}, 201, 202},
        {3, "Output",    {390, 65}, 301, 0},
    };
    std::vector<node_link> links{{101, 201}, {202, 301}};

    // Timeline & Bezier curve
    timeline_state tl;
    std::array<timeline_clip, 2> clips{{
        {8.0f, 42.0f, "Camera Pan", {72, 148, 255, 255}},
        {50.0f, 92.0f, "Zoom In",   {62, 198, 148, 255}},
    }};
    std::array<timeline_keyframe, 3> keys{{{14.0f}, {42.0f}, {76.0f}}};
    std::vector<curve_point> curve{
        {.x = 0.0f, .y = 0.05f, .in_tangent = {-0.1f, 0.0f}, .out_tangent = {0.30f, 0.0f}},
        {.x = 0.5f, .y = 0.72f, .in_tangent = {-0.18f, -0.1f}, .out_tangent = {0.18f, 0.1f}},
        {.x = 1.0f, .y = 0.95f, .in_tangent = {-0.25f, 0.0f}, .out_tangent = {0.1f, 0.0f}},
    };

    // Log
    log_buffer log{200};
};

demo_state g;

[[nodiscard]] key map_dom_key(unsigned long kc) noexcept
{
    if (kc >= 'A' && kc <= 'Z') { return static_cast<key>(kc); }
    if (kc >= '0' && kc <= '9') { return static_cast<key>(kc); }
    if (kc >= 112 && kc <= 123) { return static_cast<key>(kc); } // F1..F12
    switch (kc) {
    case 8:   return key::backspace;
    case 9:   return key::tab;
    case 13:  return key::enter;
    case 27:  return key::escape;
    case 32:  return key::space;
    case 33:  return key::page_up;
    case 34:  return key::page_down;
    case 35:  return key::end;
    case 36:  return key::home;
    case 37:  return key::left;
    case 38:  return key::up;
    case 39:  return key::right;
    case 40:  return key::down;
    case 46:  return key::del;
    case 191: return key::slash;
    case 187: return key::equals;
    case 189: return key::minus;
    default:  return key::none;
    }
}

void sync_canvas_size()
{
    double css_w = 1280.0, css_h = 720.0;
    emscripten_get_element_css_size("#canvas", &css_w, &css_h);
    const f32 dpr = std::clamp(static_cast<f32>(emscripten_get_device_pixel_ratio()), 1.0f, 3.0f);
    const u32 pw  = static_cast<u32>(std::max(1.0, std::round(css_w * dpr)));
    const u32 ph  = static_cast<u32>(std::max(1.0, std::round(css_h * dpr)));
    if (pw != g.in.phys_w || ph != g.in.phys_h || std::abs(dpr - g.in.dpr) > 0.01f || g.in.size_dirty) {
        g.in.phys_w     = pw;
        g.in.phys_h     = ph;
        g.in.dpr        = dpr;
        g.in.size_dirty = false;
        emscripten_set_canvas_element_size("#canvas", static_cast<int>(pw), static_cast<int>(ph));
        if (g.using_webgpu && g.surface != nullptr && g.device != nullptr) {
            WGPUSurfaceConfiguration cfg{};
            cfg.device      = g.device;
            cfg.format      = g.surface_format;
            cfg.usage       = WGPUTextureUsage_RenderAttachment;
            cfg.alphaMode   = g.alpha_mode;
            cfg.width       = pw;
            cfg.height      = ph;
            cfg.presentMode = WGPUPresentMode_Fifo;
            wgpuSurfaceConfigure(g.surface, &cfg);
        } else if (!g.using_webgpu && g.sw_ren.valid()) {
            g.sw_ren.resize(pw, ph);
        }
        if (g.ui != nullptr && std::abs(g.ui->scale() - dpr) > 0.01f) {
            if (g.using_webgpu && g.gpu_ren.valid()) {
                if (g.ui->set_scale(dpr)) {
                    (void)g.gpu_ren.update_atlas(g.ui->font());
                    g.ui->release_font_pixels();
                }
            } else if (!g.using_webgpu && g.sw_ren.valid()) {
                if (g.ui->set_scale(dpr)) {
                    (void)g.sw_ren.update_atlas(g.ui->font());
                    g.ui->release_font_pixels();
                }
            }
        }
    }
}

void apply_css_cursor(cursor_kind c)
{
    if (c == g.in.last_cursor) { return; }
    g.in.last_cursor = c;
    int idx = 0;
    switch (c) {
    case cursor_kind::text:        idx = 1; break;
    case cursor_kind::hand:        idx = 2; break;
    case cursor_kind::not_allowed: idx = 3; break;
    case cursor_kind::resize_ew:   idx = 4; break;
    case cursor_kind::resize_ns:   idx = 5; break;
    case cursor_kind::resize_nwse: idx = 6; break;
    case cursor_kind::resize_nesw: idx = 7; break;
    default:                       idx = 0; break;
    }
    EM_ASM({
        const map = ['default','text','pointer','not-allowed','ew-resize','ns-resize','nwse-resize','nesw-resize'];
        const c = document.getElementById('canvas');
        if (c) c.style.cursor = map[$0] || 'default';
    }, idx);
}

// ---- DOM event callbacks -------------------------------------------------------------------------------------------

EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent* e, void*)
{
    g.in.mouse_pos = {static_cast<f32>(e->targetX) * g.in.dpr, static_cast<f32>(e->targetY) * g.in.dpr};
    g.in.ctrl      = e->ctrlKey != 0 || e->metaKey != 0;
    g.in.shift     = e->shiftKey != 0;
    g.in.alt       = e->altKey != 0;
    return EM_FALSE;
}

EM_BOOL on_mouse_down(int, const EmscriptenMouseEvent* e, void*)
{
    g.in.mouse_pos = {static_cast<f32>(e->targetX) * g.in.dpr, static_cast<f32>(e->targetY) * g.in.dpr};
    const int idx  = e->button == 0 ? 0 : (e->button == 2 ? 1 : (e->button == 1 ? 2 : -1));
    if (idx >= 0) { g.in.mouse_down[static_cast<std::size_t>(idx)] = true; }
    EM_ASM({ document.getElementById('canvas').focus(); });
    return EM_TRUE;
}

EM_BOOL on_mouse_up(int, const EmscriptenMouseEvent* e, void*)
{
    const int idx = e->button == 0 ? 0 : (e->button == 2 ? 1 : (e->button == 1 ? 2 : -1));
    if (idx >= 0) { g.in.mouse_down[static_cast<std::size_t>(idx)] = false; }
    return EM_FALSE;
}

EM_BOOL on_wheel(int, const EmscriptenWheelEvent* e, void*)
{
    const f32 scale = e->deltaMode == DOM_DELTA_PIXEL ? (1.0f / 100.0f) : (e->deltaMode == DOM_DELTA_LINE ? (1.0f / 3.0f) : 1.0f);
    g.in.wheel   -= static_cast<f32>(e->deltaY) * scale;
    g.in.wheel_x += static_cast<f32>(e->deltaX) * scale;
    return EM_TRUE;
}

EM_BOOL on_key_down(int, const EmscriptenKeyboardEvent* e, void*)
{
    g.in.ctrl  = e->ctrlKey != 0 || e->metaKey != 0;
    g.in.shift = e->shiftKey != 0;
    g.in.alt   = e->altKey != 0;
    const key k = map_dom_key(e->keyCode);
    if (k != key::none) {
        g.in.keys.push_back(key_event{k, g.in.ctrl, g.in.shift, g.in.alt});
    }
    if (!g.in.ctrl && !g.in.alt) {
        const std::size_t len = std::strlen(e->key);
        // Single UTF-8 codepoint printable key strings (exclude named keys like "Enter", "ArrowLeft")
        if (len >= 1 && len <= 4 && (static_cast<u8>(e->key[0]) >= 0x20)) {
            const bool named = len > 1 && static_cast<u8>(e->key[0]) < 0x80;
            if (!named) { g.in.typed.append(e->key, len); }
        }
    }
    // Prevent browser Tab focus jump / Backspace navigation / Space page scroll
    if (k == key::tab || k == key::backspace || k == key::space || k == key::up || k == key::down ||
        (g.in.ctrl && (k == key::a || k == key::f || k == key::s || k == key::z || k == key::y || k == key::slash))) {
        return EM_TRUE;
    }
    return EM_FALSE;
}

EM_BOOL on_key_up(int, const EmscriptenKeyboardEvent* e, void*)
{
    g.in.ctrl  = e->ctrlKey != 0 || e->metaKey != 0;
    g.in.shift = e->shiftKey != 0;
    g.in.alt   = e->altKey != 0;
    return EM_FALSE;
}

EM_BOOL on_touch(int type, const EmscriptenTouchEvent* e, void*)
{
    if (e->numTouches > 0) {
        g.in.mouse_pos = {static_cast<f32>(e->touches[0].targetX) * g.in.dpr,
                          static_cast<f32>(e->touches[0].targetY) * g.in.dpr};
    }
    if (type == EMSCRIPTEN_EVENT_TOUCHSTART) { g.in.mouse_down[0] = true; }
    if (type == EMSCRIPTEN_EVENT_TOUCHEND || type == EMSCRIPTEN_EVENT_TOUCHCANCEL) { g.in.mouse_down[0] = false; }
    return EM_TRUE;
}

EM_BOOL on_resize(int, const EmscriptenUiEvent*, void*)
{
    g.in.size_dirty = true;
    return EM_FALSE;
}

// ---- UI build ------------------------------------------------------------------------------------------------------

void build_sandbox_ui(context& ui, f32 dt)
{
    g.time_acc += dt;
    if (g.animate_charts) {
        for (std::size_t i = 0; i < g.wave_a.size(); ++i) {
            const f32 x = static_cast<f32>(i) * 0.18f + g.time_acc * g.speed;
            g.wave_a[i] = 52.0f + 28.0f * std::sin(x) + 8.0f * std::cos(x * 2.3f);
            g.wave_b[i] = 36.0f + 18.0f * std::cos(x * 0.8f + 0.7f);
        }
        for (u32 r = 0; r < 6; ++r) {
            for (u32 c = 0; c < 6; ++c) {
                g.heat_grid[r * 6 + c] = 0.5f + 0.5f * std::sin(g.time_acc + static_cast<f32>(r) * 0.6f + static_cast<f32>(c) * 0.5f);
            }
        }
    }

    // Main menu bar
    f32 bar_h = 0.0f;
    if (auto bar = ui.main_menu_bar()) {
        if (auto m = ui.menu("&Themes")) {
            const auto names = themes::names();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (ui.menu_item(names[i], {}, g.theme_idx == static_cast<int>(i))) {
                    g.theme_idx = static_cast<int>(i);
                    (void)themes::by_name(names[i], ui.theme());
                    g.log.addf(log_level::info, "Switched theme to {}", names[i]);
                }
            }
        }
        if (auto m = ui.menu("&Tools")) {
            ui.menu_item("Metrics Window", g.show_metrics);
            ui.menu_item("Widget Inspector", g.show_inspector);
            ui.menu_separator();
            if (ui.menu_item("Trigger Toast")) {
                ui.toast("strata WebGPU", "Running live inside the browser.", toast_kind::success);
            }
        }
        ui.same_line_right(260.0f);
        ui.badge(g.using_webgpu ? "WebGPU" : "CPU Software", g.using_webgpu ? toast_kind::success : toast_kind::warning);
        ui.same_line();
        ui.textf("  {:.0f} fps  |  {} draws", g.fps_smooth, ui.stats().draw_calls);
        bar_h = ui.main_menu_bar_height();
    }

    // Dockspace covering the viewport below the menu bar
    const vec2 ds = ui.display_size();
    rect client{{0.0f, bar_h}, ds};
    client = ui.dock_edge("left",   dock_side::left,   std::min(340.0f, ds.x * 0.30f), client);
    client = ui.dock_edge("bottom", dock_side::bottom, std::min(230.0f, ds.y * 0.32f), client);
    ui.dock_area(client);

    constexpr auto wflags = window_flags::dockable | window_flags::resizable;

    if (auto w = ui.window("Controls & Inspector", {16, 40}, {320, 480}, wflags)) {
        if (ui.button("Notify")) {
            ui.toast("Clicked", "Immediate-mode event handled in WASM.", toast_kind::info);
            g.log.add(log_level::info, "Notify button clicked");
        }
        ui.same_line();
        ui.checkbox("Enabled", g.enabled);
        ui.same_line();
        ui.toggle("Animate", g.animate_charts);

        static constexpr std::string_view modes[] = {"Draft", "Balanced", "Ultra"};
        ui.segmented("quality", modes, g.seg_mode);
        ui.slider("Speed", g.speed, 0.0f, 4.0f);
        ui.range_slider("Range", g.range_lo, g.range_hi, 0.0f, 100.0f);

        ui.knob("Gain", g.knob_gain, 0.0f, 1.0f, 0.62f);
        ui.same_line();
        ui.knob("Pan", g.knob_pan, 0.0f, 1.0f, 0.50f);

        ui.input_text("Label", g.text_input);
        static constexpr std::string_view presets[] = {"Midnight", "Nord", "Dracula", "Glass"};
        if (ui.combo("Quick Theme", g.combo_idx, presets)) {
            static constexpr std::string_view map[] = {"midnight", "nord", "dracula", "glass"};
            (void)themes::by_name(map[static_cast<std::size_t>(g.combo_idx)], ui.theme());
        }
        ui.color_edit("Accent", g.accent_pick);

        ui.separator("Property Grid");
        if (ui.begin_property_grid("demo_props", 0.42f)) {
            if (ui.property_row("Exposure", {.modified = std::abs(g.exposure - 1.0f) > 0.01f}, [&] {
                    ui.drag_float("##exp", g.exposure, 0.02f, 0.1f, 8.0f);
                })) { g.exposure = 1.0f; }
            if (ui.property_row("Roughness", {.modified = std::abs(g.roughness - 0.5f) > 0.01f, .mixed = true}, [&] {
                    ui.slider("##rough", g.roughness, 0.0f, 1.0f);
                })) { g.roughness = 0.5f; }
            ui.end_property_grid();
        }

        ui.separator("Flex Wrap Tags & SVG Icons");
        static constexpr std::string_view tag_names[] = {"WebGPU", "WASM", "SDF", "Docking", "Zero-Heap", "C++23"};
        ui.begin_flex({.gap_x = 6.0f, .gap_y = 6.0f});
        for (std::size_t i = 0; i < 6; ++i) {
            (void)ui.chip(tag_names[i], {.selected = &g.chip_on[i]});
        }
        ui.end_flex();

        // Crisp vector SVG icons drawn directly via draw_svg_path
        const auto strip = ui.custom_item("svg_strip", {0.0f, 28.0f});
        static constexpr std::string_view icons_d[] = {
            svg_icons::play, svg_icons::pause, svg_icons::search, svg_icons::settings,
            svg_icons::folder, svg_icons::eye, svg_icons::lock, svg_icons::check,
        };
        for (std::size_t i = 0; i < 8; ++i) {
            const rect r{{strip.bounds.min.x + static_cast<f32>(i) * 30.0f, strip.bounds.min.y + 3.0f},
                         {strip.bounds.min.x + static_cast<f32>(i) * 30.0f + 22.0f, strip.bounds.min.y + 25.0f}};
            draw_svg_path(ui.draw(), r, icons_d[i], svg_icons::viewbox, ui.theme().accent, 1.8f);
        }
    }

    if (auto w = ui.window("Interactive Charts", {360, 40}, {520, 420}, wflags)) {
        const plot_series series[2] = {
            {.name = "CPU", .values = g.wave_a},
            {.name = "GPU", .values = g.wave_b},
        };
        const plot_ref_line refs[1] = {
            {.value = 65.0f, .vertical = false, .col = {255, 96, 96, 210}, .label = "budget"},
        };
        plot_options po;
        po.size               = {0.0f, 170.0f};
        po.x                  = {"sample", ""};
        po.y                  = {"load", "%", 0.0f, 100.0f};
        po.fill               = true;
        po.zoom_pan           = true;
        po.interactive_legend = true;
        po.ref_lines          = refs;
        ui.plot("Telemetry (click legend to toggle, wheel to zoom)", series, po);

        ui.plot_heatmap("2D Heatmap (hover cells)", g.heat_grid, 6, 6, {0.0f, 130.0f}, 0.0f, 1.0f);
    }

    if (auto w = ui.window("Code & Node Graph", {380, 60}, {520, 420}, wflags)) {
        ui.text_dim("Code editor (Ctrl+/ comment toggle, Ctrl+F find):");
        {
            auto fscope = ui.with_font(g.mono_font);
            ui.input_code("##code", g.code_src, {0.0f, 135.0f});
        }
        ui.separator("Node Editor (drag nodes, wire pins, scroll to zoom)");
        if (g.graph.begin(ui, "web_graph", {0.0f, 190.0f})) {
            for (auto& n : g.nodes) {
                g.graph.begin_node(n.id, n.title, n.pos);
                if (n.in_p != 0)  { g.graph.input_pin(n.in_p, "in"); }
                if (n.out_p != 0) { g.graph.output_pin(n.out_p, "out"); }
                g.graph.end_node();
            }
            for (const auto& l : g.links) { g.graph.link(l.from, l.to); }
            g.graph.end();
        }
        if (node_link made; g.graph.link_created(made)) { g.links.push_back(made); }
        if (node_link del;  g.graph.link_deleted(del)) {
            std::erase_if(g.links, [&](const node_link& l) { return l.from == del.from && l.to == del.to; });
        }
    }

    if (auto w = ui.window("Timeline & Curves", {40, 500}, {680, 220}, wflags)) {
        if (auto r = ui.row({col_weight(1.4f), col_weight(1.0f)}, 10.0f)) {
            if (ui.begin_timeline("Sequencer", g.tl, {0.0f, 145.0f})) {
                (void)ui.timeline_track("Camera", g.clips, g.keys);
                ui.end_timeline();
            }
            ui.row_next();
            ui.curve_editor("Bezier Ease (dbl-click to add)", g.curve, {0.0f, 145.0f});
        }
    }

    if (auto w = ui.window("Log & Rich Text", {40, 520}, {500, 220}, wflags)) {
        ui.rich_text("Built with <c=ffffff>strata</c> - <c=5b8dff>WebGPU</c> + <c=3ec694>WebAssembly</c> (drag tabs to rearrange)");
        ui.log_view("web_log", g.log);
    }

    if (!g.layout_seeded) {
        g.layout_seeded = true;
        ui.dock_window("Controls & Inspector", dock_zone::center, {}, 0.5f, "left");
        ui.dock_window("Interactive Charts",   dock_zone::center, {}, 0.5f, {});
        ui.dock_window("Code & Node Graph",    dock_zone::center, "Interactive Charts", 0.5f, {});
        ui.dock_window("Timeline & Curves",    dock_zone::center, {}, 0.5f, "bottom");
        ui.dock_window("Log & Rich Text",      dock_zone::center, "Timeline & Curves", 0.5f, "bottom");
    }

    ui.debug_metrics_window(g.show_metrics);
    ui.debug_inspector_window(g.show_inspector);
}

// ---- Frame tick ----------------------------------------------------------------------------------------------------

void present_software_to_canvas()
{
    const u32 w = g.sw_ren.width(), h = g.sw_ren.height();
    if (w == 0 || h == 0) { return; }
    g.sw_rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (!g.sw_ren.copy_straight(g.sw_rgba)) { return; }
    EM_ASM({
        const c = document.getElementById('canvas');
        if (!c) return;
        const ctx = c.getContext('2d');
        if (!ctx) return;
        const ptr = $0, w = $1, h = $2;
        const slice = new Uint8ClampedArray(HEAPU8.buffer, ptr, w * h * 4);
        ctx.putImageData(new ImageData(slice, w, h), 0, 0);
    }, g.sw_rgba.data(), static_cast<int>(w), static_cast<int>(h));
}

void frame_tick()
{
    if (g.ui == nullptr) { return; }
    sync_canvas_size();

    const f64 now = emscripten_get_now() * 0.001;
    const f32 dt  = g.in.last_time > 0.0 ? std::clamp(static_cast<f32>(now - g.in.last_time), 0.001f, 0.1f) : (1.0f / 60.0f);
    g.in.last_time = now;
    g.fps_smooth   = g.fps_smooth * 0.92f + (1.0f / std::max(dt, 0.001f)) * 0.08f;

    input_state in;
    in.mouse_pos    = g.in.mouse_pos;
    in.mouse_down   = g.in.mouse_down;
    in.wheel        = std::exchange(g.in.wheel, 0.0f);
    in.wheel_x      = std::exchange(g.in.wheel_x, 0.0f);
    in.delta_time   = dt;
    in.display_size = {static_cast<f32>(g.in.phys_w), static_cast<f32>(g.in.phys_h)};
    in.ctrl         = g.in.ctrl;
    in.shift        = g.in.shift;
    in.alt          = g.in.alt;
    if (!g.in.typed.empty()) {
        const std::size_t n = std::min(g.in.typed.size(), in.typed.size());
        std::memcpy(in.typed.data(), g.in.typed.data(), n);
        in.typed_len = static_cast<u32>(n);
        g.in.typed.clear();
    }
    for (const key_event& ke : g.in.keys) { in.press(ke.k, ke.ctrl, ke.shift, ke.alt); }
    g.in.keys.clear();

    g.ui->begin_frame(std::move(in));
    build_sandbox_ui(*g.ui, dt);
    g.ui->end_frame();
    apply_css_cursor(g.ui->cursor());

    const draw_data dd = g.ui->render_data();
    const color bg{14, 16, 22, 255};

    if (g.using_webgpu) {
        WGPUSurfaceTexture st{};
        wgpuSurfaceGetCurrentTexture(g.surface, &st);
        const bool ok = (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) &&
                        st.texture != nullptr;
        if (ok) {
            WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
            WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
            g.gpu_ren.render(dd, enc, view, nullptr, true, bg);
            WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
            wgpuQueueSubmit(g.queue, 1, &cmd);
            wgpuCommandBufferRelease(cmd);
            wgpuCommandEncoderRelease(enc);
            wgpuTextureViewRelease(view);
            wgpuTextureRelease(st.texture);
            if (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                g.in.size_dirty = true;
            }
        } else {
            if (st.texture != nullptr) { wgpuTextureRelease(st.texture); }
            g.in.size_dirty = true;
        }
    } else {
        g.sw_ren.clear(bg);
        g.sw_ren.render(dd);
        present_software_to_canvas();
    }
}

void start_loop(bool webgpu_ok)
{
    g.using_webgpu = webgpu_ok;
    if (!webgpu_ok) {
        // If a failed WebGPU surface creation already bound a 'webgpu' context to #canvas, replace the element
        // so getContext('2d') succeeds cleanly.
        EM_ASM({
            const c = document.getElementById('canvas');
            if (c && !c.getContext('2d')) {
                const fresh = c.cloneNode(false);
                fresh.addEventListener('contextmenu', e => e.preventDefault());
                c.parentNode.replaceChild(fresh, c);
            }
        });
        sync_canvas_size();
        (void)g.sw_ren.create(g.ui->font());
        g.sw_ren.resize(g.in.phys_w, g.in.phys_h);
        g.ui->release_font_pixels();
        g.log.add(log_level::warn, "WebGPU unavailable in this browser; running CPU software renderer fallback");
    } else {
        g.log.add(log_level::info, "WebGPU initialized (strata::wgpu_renderer)");
    }

    emscripten_set_mousemove_callback("#canvas", nullptr, EM_TRUE, on_mouse_move);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, on_mouse_down);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_mouse_up);
    emscripten_set_wheel_callback("#canvas", nullptr, EM_TRUE, on_wheel);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_key_down);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_key_up);
    emscripten_set_touchstart_callback("#canvas", nullptr, EM_TRUE, on_touch);
    emscripten_set_touchmove_callback("#canvas", nullptr, EM_TRUE, on_touch);
    emscripten_set_touchend_callback("#canvas", nullptr, EM_TRUE, on_touch);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_resize);

    emscripten_set_main_loop(frame_tick, 0, false);
}

void on_device_ready(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView, void*, void*)
{
    if (status != WGPURequestDeviceStatus_Success || device == nullptr) {
        start_loop(false);
        return;
    }
    g.device = device;
    g.queue  = wgpuDeviceGetQueue(device);

    // Create the canvas surface only after adapter + device succeed so a failed WebGPU init leaves #canvas free for 2D.
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector sel{};
    sel.chain.sType     = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    sel.selector.data   = "#canvas";
    sel.selector.length = WGPU_STRLEN;
    WGPUSurfaceDescriptor sdesc{};
    sdesc.nextInChain = &sel.chain;
    g.surface = wgpuInstanceCreateSurface(g.instance, &sdesc);
    if (g.surface == nullptr) {
        start_loop(false);
        return;
    }

    WGPUSurfaceCapabilities caps{};
    wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps);
    g.surface_format = (caps.formatCount > 0 && caps.formats != nullptr) ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    g.alpha_mode     = (caps.alphaModeCount > 0 && caps.alphaModes != nullptr) ? caps.alphaModes[0] : WGPUCompositeAlphaMode_Opaque;
    wgpuSurfaceCapabilitiesFreeMembers(caps);

    sync_canvas_size();
    WGPUSurfaceConfiguration sc{};
    sc.device      = g.device;
    sc.format      = g.surface_format;
    sc.usage       = WGPUTextureUsage_RenderAttachment;
    sc.alphaMode   = g.alpha_mode;
    sc.width       = g.in.phys_w;
    sc.height      = g.in.phys_h;
    sc.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(g.surface, &sc);

    wgpu_init_info info;
    info.device       = g.device;
    info.queue        = g.queue;
    info.color_format = g.surface_format;
    if (!g.gpu_ren.create(info, g.ui->font())) {
        start_loop(false);
        return;
    }
    g.ui->release_font_pixels();
    start_loop(true);
}

void on_adapter_ready(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void*, void*)
{
    if (status != WGPURequestAdapterStatus_Success || adapter == nullptr) {
        start_loop(false);
        return;
    }
    g.adapter = adapter;
    WGPUDeviceDescriptor ddesc{};
    WGPURequestDeviceCallbackInfo cb{};
    cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_device_ready;
    wgpuAdapterRequestDevice(adapter, &ddesc, cb);
}

} // namespace

int main()
{
    context_config cfg;
    cfg.font.file           = "/font.ttf";
    cfg.font.pixel_height   = 14.0f;
    cfg.font.rasterizer     = font_rasterizer::truetype;
    cfg.scale               = std::clamp(static_cast<f32>(emscripten_get_device_pixel_ratio()), 1.0f, 3.0f);
    // Keep WASM linear-memory reservations compact (~2.5 MiB instead of desktop virtual-memory defaults).
    cfg.limits.max_vertices = 1u << 16;
    cfg.limits.max_indices  = 1u << 18;
    cfg.limits.max_commands = 1u << 11;
    cfg.limits.max_shapes   = 1u << 13;

    font_config mono;
    mono.file         = "/mono.ttf";
    mono.pixel_height = 13.0f;
    mono.rasterizer   = font_rasterizer::truetype;
    std::array<font_config, 1> extras{mono};
    if (FILE* f = std::fopen("/mono.ttf", "rb")) {
        std::fclose(f);
        cfg.extra_fonts = extras;
        g.mono_font     = 1;
    }

    auto created = context::create(cfg);
    if (!created) {
        std::fprintf(stderr, "strata_web_demo: failed to initialize font atlas\n");
        return 1;
    }
    g.ui = std::make_unique<context>(std::move(*created));
    g.log.add(log_level::info, "strata browser sandbox initialized");

    const int has_gpu = EM_ASM_INT({ return typeof navigator !== 'undefined' && !!navigator.gpu ? 1 : 0; });
    if (!has_gpu) {
        start_loop(false);
        return 0;
    }

    WGPUInstanceDescriptor idesc{};
    g.instance = wgpuCreateInstance(&idesc);
    if (g.instance == nullptr) {
        start_loop(false);
        return 0;
    }

    WGPURequestAdapterOptions opts{};
    opts.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo cb{};
    cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_adapter_ready;
    wgpuInstanceRequestAdapter(g.instance, &opts, cb);
    return 0;
}

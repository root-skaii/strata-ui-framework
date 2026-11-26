#include "showcase/app.hpp"
#include "showcase/modules/registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>

namespace strata::showcase {
namespace {

constexpr std::array<std::string_view, 7> k_workspaces = {
    "tour",
    "studio",
    "telemetry",
    "authoring",
    "visuals",
    "productivity",
    "gallery",
};

struct chapter_desc {
    std::string_view num;
    std::string_view title;
    std::string_view subtitle;
    std::string_view summary;
};

constexpr std::array<chapter_desc, 6> k_chapters = {{
    {"00", "Overview",       "Meet strata",              "Zero-heap immediate-mode UI with analytic GPU rendering"},
    {"01", "Controls",       "Inputs & live preview",    "Property grids, knobs, sliders, and color pickers driving a live material viewport"},
    {"02", "Visuals & Glass","SDF, blur & typography",   "Single-quad SDF gradients, frosted acrylic backdrop blur, SVG icons, and RTL rich text"},
    {"03", "Data & Plots",   "Charts & 100k-row grid",   "Interactive multi-series charts, heatmaps, and O(visible) virtualized tables"},
    {"04", "Authoring",      "Nodes, code & timeline",   "Shader node graph, syntax-highlighted code editor, sequencer, and Bezier curves"},
    {"05", "Docking Studio", "Multi-panel workspace",    "Edge docking, tabbed panels, drag-to-split layout, and Ctrl+Shift+P Command Palette"},
}};

constexpr std::array<color, 4> k_hero_swatches = {{
    color::from_hex(0x5e8bffff), // indigo
    color::from_hex(0x3ecf8eff), // emerald
    color::from_hex(0xe86ca8ff), // rose
    color::from_hex(0xf0a848ff), // amber
}};

[[nodiscard]] bool icontains(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty()) { return true; }
    if (needle.size() > haystack.size()) { return false; }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const unsigned char a = static_cast<unsigned char>(haystack[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) { return true; }
    }
    return false;
}

[[nodiscard]] bool is_light_surface(const style& s) noexcept
{
    return (static_cast<int>(s.window_bg.r) + s.window_bg.g + s.window_bg.b) > 380;
}

[[nodiscard]] color shift_lum(color c, int delta, u8 alpha = 255) noexcept
{
    const auto clamp_u8 = [](int v) -> u8 { return static_cast<u8>(std::clamp(v, 0, 255)); };
    return {clamp_u8(static_cast<int>(c.r) + delta),
            clamp_u8(static_cast<int>(c.g) + delta),
            clamp_u8(static_cast<int>(c.b) + delta),
            alpha};
}

[[nodiscard]] color with_alpha(color c, u8 a) noexcept
{
    return {c.r, c.g, c.b, a};
}

// Optical vertical alignment using actual capital glyph ink bounds rather than raw line_height.
[[nodiscard]] f32 text_vcenter_y(const font_atlas& fa, f32 cy, font_id f = 0) noexcept
{
    const vec2 cap = fa.cap_bounds(f);
    return cy - (cap.x + cap.y) * 0.5f;
}

// Draws a 2-line vertical text stack optically centered inside `r` (top margin == bottom margin to visible ink).
// Returns the optical vertical midpoint of line 1 so leading dots or step indices align cleanly with it.
f32 draw_centered_stack(draw_list& dl, const font_atlas& fa, const rect& r, f32 x,
                        std::string_view line1, font_id f1, color c1, text_flags st1,
                        std::string_view line2, font_id f2, color c2, f32 gap = 8.0f)
{
    const vec2 cap1    = fa.cap_bounds(f1);
    const vec2 cap2    = fa.cap_bounds(f2);
    const f32  h1      = cap1.y - cap1.x;
    const f32  h2      = cap2.y - cap2.x;
    const f32  ink_top = r.center().y - (h1 + gap + h2) * 0.5f;
    dl.text({x, ink_top - cap1.x}, c1, line1, f1, st1);
    dl.text({x, ink_top + h1 + gap - cap2.x}, c2, line2, f2);
    return ink_top + h1 * 0.5f;
}

// Layered geometric brand mark representing stacked strata surfaces.
void draw_strata_mark(draw_list& dl, vec2 origin, f32 size, color accent, color ink)
{
    const f32 h = size * 0.24f;
    const f32 r = 2.0f;
    dl.rect_filled({{origin.x + size * 0.14f, origin.y + size * 0.68f}, {origin.x + size, origin.y + size * 0.68f + h}},
                   with_alpha(ink, 95), r);
    dl.rect_filled({{origin.x + size * 0.07f, origin.y + size * 0.38f}, {origin.x + size * 0.93f, origin.y + size * 0.38f + h}},
                   with_alpha(ink, 175), r);
    dl.rect_filled({{origin.x, origin.y + size * 0.08f}, {origin.x + size * 0.86f, origin.y + size * 0.08f + h}},
                   accent, r);
}

// Clean hairline-bordered surface card drawn via SDF shape primitive.
void draw_surface_card(draw_list& dl, const rect& r, color fill, color border, f32 radius = 10.0f,
                       color shadow = {0, 0, 0, 0}, f32 shadow_blur = 0.0f)
{
    shape_style st;
    st.radius        = radii(radius);
    st.fill_top      = fill;
    st.fill_bottom   = fill;
    st.border        = border;
    st.border_width  = 1.0f;
    st.shadow        = shadow;
    st.shadow_blur   = shadow_blur;
    st.shadow_offset = {0.0f, shadow_blur * 0.35f};
    dl.shape(r, st);
}

} // namespace

std::span<const std::string_view> showcase_app::workspace_names() noexcept
{
    return k_workspaces;
}

showcase_app::showcase_app()
{
    auto mods = create_builtin_modules();
    modules_.reserve(mods.size());
    for (auto& m : mods) {
        module_slot slot;
        slot.info = m->info();
        slot.mod  = std::move(m);
        modules_.push_back(std::move(slot));
    }
}

void showcase_app::register_keybinds()
{
    auto reg = [&](std::string_view id, std::string_view title, std::string_view chord, std::string_view desc) {
        binds_.add(id, chord, desc);
        binds_.set_title(id, title);
    };
    reg("tour_overview",    "Tour 00: Overview",              "Ctrl+0",         "Guided tour landing page");
    reg("tour_controls",    "Tour 01: Controls & Preview",    "Ctrl+1",         "Inspector controls with live material viewport");
    reg("tour_visuals",     "Tour 02: Visuals & Glass",       "Ctrl+2",         "Analytic SDF gradients, blur, and rich text");
    reg("tour_data",        "Tour 03: Data & Plots",          "Ctrl+3",         "Live charts, heatmap, and 100k-row virtualized table");
    reg("tour_authoring",   "Tour 04: Authoring Tools",       "Ctrl+4",         "Node graph, code editor, and timeline sequencer");
    reg("ws_studio",        "Tour 05: Docking Studio",        "Ctrl+5",         "Focused 3-panel dockable studio workspace");
    reg("ws_gallery",       "Explore: All Components",        "Ctrl+6",         "Searchable catalog of all 12 showcase modules");
    reg("ws_productivity",  "Explore: Workflow & Pickers",    "Ctrl+7",         "Kanban drag-and-drop, pickers, tabs, and keybinds");
    reg("toggle_animate",   "Simulation: Toggle Playback",    "Space",          "Pause or resume live animation");
    reg("theme_obsidian",   "Theme: Obsidian",                "Ctrl+K, Ctrl+1", "Apply Obsidian modern neutral dark theme");
    reg("theme_glass",      "Theme: Frosted Glass",           "Ctrl+K, Ctrl+2", "Apply Frosted Glass acrylic theme");
    reg("theme_light",      "Theme: Light",                   "Ctrl+K, Ctrl+3", "Apply Light theme");
    reg("theme_midnight",   "Theme: Midnight",                "Ctrl+K, Ctrl+4", "Apply Midnight theme");
    reg("theme_dracula",    "Theme: Dracula",                 "Ctrl+K, Ctrl+5", "Apply Dracula theme");
    reg("theme_nord",       "Theme: Nord",                    "Ctrl+K, Ctrl+6", "Apply Nord theme");
    reg("toggle_metrics",   "Tools: Frame Metrics",           "F9",             "Toggle Frame Metrics window");
    reg("toggle_inspector", "Tools: Widget Inspector",        "F10",            "Toggle Widget Inspector window");
    reg("send_toast",       "Notify: Test Toast",             "Ctrl+T",         "Emit a status toast notification");
}

void showcase_app::init(context& ui, const showcase_config& cfg)
{
    env_.font_mono        = cfg.font_mono;
    env_.font_heading     = cfg.font_heading;
    env_.font_icons       = cfg.font_icons;
    env_.backend_name     = cfg.backend_name;
    env_.is_web           = cfg.is_web;
    env_.deterministic    = cfg.deterministic;
    env_.log              = &log_;
    env_.binds            = &binds_;
    env_.switch_workspace = [this, &ui](std::string_view ws) { set_scene(ui, ws); };

    register_keybinds();

    log_.addf(log_level::info, "strata initialized ({})", cfg.backend_name);
    log_.add(log_level::info, "12 modular panels ready (zero-heap steady state)");
    log_.add(log_level::debug, "Press Ctrl+Shift+P to open the Command Palette");

    if (cfg.initial_theme < 0) {
        apply_theme_by_name(ui, "obsidian");
    } else {
        apply_theme(ui, cfg.initial_theme);
    }
    set_scene(ui, cfg.initial_scene.empty() ? "tour" : cfg.initial_scene);
}

void showcase_app::apply_theme(context& ui, int index)
{
    const auto names = themes::names();
    if (names.empty()) { return; }
    theme_idx_ = std::clamp(index, 0, static_cast<int>(names.size()) - 1);
    (void)themes::by_name(names[static_cast<std::size_t>(theme_idx_)], ui.theme());
    ui.theme().anim_speed = 24.0f;
}

void showcase_app::apply_theme_by_name(context& ui, std::string_view name)
{
    const auto names = themes::names();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            apply_theme(ui, static_cast<int>(i));
            return;
        }
    }
    apply_theme(ui, 0);
}

demo_module* showcase_app::find_module(std::string_view id) const noexcept
{
    for (const auto& slot : modules_) {
        if (slot.info.id == id) { return slot.mod.get(); }
    }
    return nullptr;
}

void showcase_app::enter_tour_page(context& ui, tour_page page)
{
    if (page == tour_page::studio) {
        set_scene(ui, "studio");
        return;
    }
    tour_mode_         = true;
    gallery_mode_      = false;
    tour_page_         = page;
    focused_module_.clear();
    open_palette_next_ = false;
    env_.draw_backdrop = (page == tour_page::visuals);

    switch (page) {
    case tour_page::overview:  active_workspace_ = "tour";           break;
    case tour_page::controls:  active_workspace_ = "tour_controls";  break;
    case tour_page::visuals:   active_workspace_ = "tour_visuals";   break;
    case tour_page::data:      active_workspace_ = "tour_data";      break;
    case tour_page::authoring: active_workspace_ = "tour_authoring"; break;
    case tour_page::workflow:  active_workspace_ = "tour_workflow";  break;
    case tour_page::catalog:   active_workspace_ = "tour_catalog";   break;
    case tour_page::studio:    active_workspace_ = "studio";         break;
    }
}

void showcase_app::set_scene(context& ui, std::string_view name)
{
    if (name.empty() || name == "default" || name == "tour" || name == "overview") {
        enter_tour_page(ui, tour_page::overview);
        return;
    }
    if (name == "tour_controls")  { enter_tour_page(ui, tour_page::controls);  return; }
    if (name == "tour_visuals")   { enter_tour_page(ui, tour_page::visuals);   return; }
    if (name == "tour_data")      { enter_tour_page(ui, tour_page::data);      return; }
    if (name == "tour_authoring") { enter_tour_page(ui, tour_page::authoring); return; }
    if (name == "tour_workflow")  { enter_tour_page(ui, tour_page::workflow);  return; }
    if (name == "tour_catalog")   { enter_tour_page(ui, tour_page::catalog);   return; }

    tour_mode_               = false;
    focused_module_.clear();
    gallery_mode_            = false;
    open_palette_next_       = false;
    env_.draw_backdrop       = false;
    studio_viewport_visible_ = false;

    for (auto& slot : modules_) { slot.visible = false; }

    auto show = [&](std::string_view id) {
        for (auto& slot : modules_) {
            if (slot.info.id == id) { slot.visible = true; }
        }
    };

    if (name == "palette") {
        active_workspace_        = "studio";
        open_palette_next_       = true;
        studio_viewport_visible_ = true;
        show("explorer");
        show("nodes");
        show("inspector");
    } else if (name == "studio" || name == "features") {
        // Focused 3-panel creative workspace framing the live Material Viewport (additional panels toggle via Panels menu).
        active_workspace_        = "studio";
        studio_viewport_visible_ = true;
        show("explorer");
        show("nodes");
        show("inspector");
    } else if (name == "telemetry") {
        active_workspace_ = "telemetry";
        show("charts");
        show("inspector");
        show("data");
        show("console");
    } else if (name == "authoring") {
        active_workspace_ = "authoring";
        show("editor");
        show("nodes");
        show("sequencer");
        show("inspector");
    } else if (name == "visuals" || name == "acrylic" || name == "glass") {
        active_workspace_  = "visuals";
        env_.draw_backdrop = true;
        show("visuals");
        show("typography");
    } else if (name == "productivity" || name == "workflow") {
        active_workspace_ = "productivity";
        show("pickers");
        show("overlays");
        show("console");
    } else if (name == "gallery") {
        active_workspace_ = "gallery";
        gallery_mode_     = true;
    } else if (find_module(name) != nullptr) {
        active_workspace_ = "gallery";
        gallery_mode_     = true;
        gallery_selected_ = std::string(name);
        focused_module_   = std::string(name);
    } else {
        enter_tour_page(ui, tour_page::overview);
        return;
    }

    layout_dirty_ = true;
    ui.undock_window("Viewport");
    for (const auto& slot : modules_) {
        ui.undock_window(slot.info.title);
    }
}

void showcase_app::run_action(context& ui, std::string_view action)
{
    if (action == "tour_overview")        { enter_tour_page(ui, tour_page::overview); }
    else if (action == "tour_controls")   { enter_tour_page(ui, tour_page::controls); }
    else if (action == "tour_visuals")    { enter_tour_page(ui, tour_page::visuals); }
    else if (action == "tour_data")       { enter_tour_page(ui, tour_page::data); }
    else if (action == "tour_authoring")  { enter_tour_page(ui, tour_page::authoring); }
    else if (action == "ws_studio")       { set_scene(ui, "studio"); }
    else if (action == "ws_telemetry")    { set_scene(ui, "telemetry"); }
    else if (action == "ws_authoring")    { set_scene(ui, "authoring"); }
    else if (action == "ws_visuals")      { set_scene(ui, "visuals"); }
    else if (action == "ws_productivity") { enter_tour_page(ui, tour_page::workflow); }
    else if (action == "ws_gallery")      { enter_tour_page(ui, tour_page::catalog); }
    else if (action == "toggle_animate")  { env_.animate = !env_.animate; }
    else if (action == "toggle_metrics")  { show_metrics_ = !show_metrics_; }
    else if (action == "toggle_inspector"){ show_inspector_ = !show_inspector_; }
    else if (action == "send_toast") {
        ui.toast("Build Succeeded", "All 12 showcase modules compiled cleanly.", toast_kind::success);
        log_.add(log_level::info, "Toast dispatched via action");
    } else if (action.starts_with("theme_")) {
        const std::string_view tname = action.substr(6);
        apply_theme_by_name(ui, tname);
        log_.addf(log_level::info, "Applied theme '{}'", tname);
    }
}

void showcase_app::draw_tour_header(context& ui, f32 width, f32 height)
{
    const style&      st     = ui.theme();
    const font_atlas& fa     = ui.font();
    const bool        light  = is_light_surface(st);
    const color       bar_bg = light ? shift_lum(st.window_bg, -6, 255) : shift_lum(st.window_bg, -4, 255);
    const color       line   = with_alpha(st.border, 200);
    const font_id     mono   = env_.has_mono() ? env_.font_mono : 0;

    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({
        override_var(style_var::padding, 0.0f),
        override_var(style_var::item_spacing, 0.0f),
    });

    if (auto w = ui.window("##tour_header", {0.0f, 0.0f}, {width, height}, flags)) {
        draw_list& dl = ui.draw();
        dl.rect_filled({{0.0f, 0.0f}, {width, height}}, bar_bg);
        dl.line({0.0f, height - 1.0f}, {width, height - 1.0f}, line, 1.0f);

        const f32 cy = height * 0.5f;

        // Brand mark + wordmark + C++23 pill aligned with the left rail
        draw_strata_mark(dl, {18.0f, cy - 8.0f}, 16.0f, st.accent, st.text);
        dl.text({42.0f, text_vcenter_y(fa, cy, 0)}, st.text, "strata", 0, text_flags::bold);

        const vec2 brand_w = fa.measure(0, "strata");
        const vec2 vw      = fa.measure(0, "C++23");
        const f32  vx0     = 42.0f + brand_w.x + 10.0f;
        const rect ver_r{{vx0, cy - 10.0f}, {vx0 + vw.x + 16.0f, cy + 10.0f}};
        dl.rect_filled(ver_r, light ? color{0, 0, 0, 14} : color{255, 255, 255, 13}, 10.0f);
        dl.text_centered(ver_r, st.text_dim, "C++23", 0);

        // Quiet breadcrumb for the active tour page
        std::string_view crumb_num   = "00";
        std::string_view crumb_title = "Overview";
        const auto idx = static_cast<std::size_t>(tour_page_);
        if (idx < k_chapters.size()) {
            crumb_num   = k_chapters[idx].num;
            crumb_title = k_chapters[idx].title;
        } else if (tour_page_ == tour_page::workflow) {
            crumb_num   = "EX";
            crumb_title = "Workflow & Pickers";
        } else if (tour_page_ == tour_page::catalog) {
            crumb_num   = "EX";
            crumb_title = "All Components";
        }

        const f32 rail_w = 236.0f;
        dl.line({rail_w, 10.0f}, {rail_w, height - 10.0f}, line, 1.0f);
        dl.text({rail_w + 22.0f, text_vcenter_y(fa, cy, mono)}, st.accent, crumb_num, mono);
        dl.text({rail_w + 46.0f, text_vcenter_y(fa, cy, 0)}, st.text_dim, "/");
        dl.text({rail_w + 58.0f, text_vcenter_y(fa, cy, 0)}, st.text, crumb_title);

        // Right-aligned telemetry + dynamic-width theme switcher + Ctrl+K search trigger
        const std::string stats_str = std::format("{:.0f} fps  ·  {:.2f} ms  ·  0 allocs", env_.fps, env_.ui_ms);
        const vec2 stats_sz = fa.measure(mono, stats_str);
        const f32  stats_x  = width - 18.0f - stats_sz.x;
        dl.text({stats_x, text_vcenter_y(fa, cy, mono)}, st.text_dim, stats_str, mono);

        static constexpr std::array<std::string_view, 3> k_quick_ids    = {"obsidian", "glass", "light"};
        static constexpr std::array<std::string_view, 3> k_quick_labels = {"Obsidian", "Glass", "Light"};
        const auto names = themes::names();
        const std::string_view cur_theme = (theme_idx_ >= 0 && static_cast<std::size_t>(theme_idx_) < names.size())
            ? names[static_cast<std::size_t>(theme_idx_)]
            : "obsidian";

        // Size each theme pill from its measured text width + generous 28px horizontal padding
        std::array<f32, 3> cell_w{};
        f32 seg_inner_w = 0.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            cell_w[i] = std::ceil(fa.measure(0, k_quick_labels[i]).x) + 28.0f;
            seg_inner_w += cell_w[i];
        }
        const f32 seg_inset = 3.0f;
        const f32 seg_w     = seg_inner_w + seg_inset * 2.0f;
        const f32 seg_h     = 28.0f;
        const f32 seg_x     = stats_x - 20.0f - seg_w;
        const f32 seg_y     = std::round(cy - seg_h * 0.5f);
        const rect seg_r{{seg_x, seg_y}, {seg_x + seg_w, seg_y + seg_h}};
        dl.rect_filled(seg_r, light ? shift_lum(st.window_bg, -14, 255) : shift_lum(st.window_bg, 4, 255), 7.0f);
        dl.rect_outline(seg_r, line, 7.0f, 1.0f);

        const f32 cmd_w = 160.0f;
        const f32 cmd_x = seg_x - 12.0f - cmd_w;

        // Place interactive header controls on the exact row aligned with `seg_y`
        (void)ui.custom_item("##hdr_pad_y", {1.0f, seg_y});
        (void)ui.custom_item("##hdr_pad_x", {cmd_x, seg_h});
        ui.same_line();
        {
            const auto cmd_it = ui.custom_item("##cmd_trigger", {cmd_w, seg_h});
            const color fill = cmd_it.hovered
                ? (light ? shift_lum(st.window_bg, -18, 255) : shift_lum(st.widget_bg, 8, 255))
                : (light ? shift_lum(st.window_bg, -10, 255) : st.widget_bg);
            dl.rect_filled(cmd_it.bounds, fill, 7.0f);
            dl.rect_outline(cmd_it.bounds, cmd_it.hovered ? st.widget_border : line, 7.0f, 1.0f);

            const f32 bcy = cmd_it.bounds.center().y;
            const rect icon_r{{cmd_it.bounds.min.x + 10.0f, bcy - 6.0f},
                              {cmd_it.bounds.min.x + 22.0f, bcy + 6.0f}};
            draw_svg_path(dl, icon_r, svg_icons::search, svg_icons::viewbox, st.text_dim, 1.6f);
            dl.text({cmd_it.bounds.min.x + 28.0f, text_vcenter_y(fa, bcy, 0)}, st.text_dim, "Search...");

            const vec2 ksz = fa.measure(mono, "Ctrl+K");
            dl.text({cmd_it.bounds.max.x - 10.0f - ksz.x, text_vcenter_y(fa, bcy, mono)},
                    with_alpha(st.text_dim, 185), "Ctrl+K", mono);
            if (cmd_it.pressed) {
                palette_.open();
            }
        }
        ui.same_line();
        (void)ui.custom_item("##hdr_gap", {12.0f + seg_inset, seg_h});

        for (std::size_t i = 0; i < 3; ++i) {
            ui.same_line();
            ui.push_id(static_cast<int>(100 + i));
            const auto it = ui.custom_item("##qt", {cell_w[i], seg_h});
            const bool active = (cur_theme == k_quick_ids[i]);
            const rect inner{{it.bounds.min.x + 1.0f, it.bounds.min.y + seg_inset},
                             {it.bounds.max.x - 1.0f, it.bounds.max.y - seg_inset}};
            if (active) {
                dl.rect_filled(inner, light ? color{255, 255, 255, 255} : st.widget_hover, 5.0f);
                dl.rect_outline(inner, light ? line : st.widget_border, 5.0f, 1.0f);
            } else if (it.hovered) {
                dl.rect_filled(inner, light ? color{0, 0, 0, 12} : color{255, 255, 255, 11}, 5.0f);
            }
            dl.text_centered(inner, active ? st.text : st.text_dim, k_quick_labels[i], 0);
            if (it.pressed) {
                apply_theme_by_name(ui, k_quick_ids[i]);
            }
            ui.pop_id();
        }
    }
}

void showcase_app::draw_tour_rail(context& ui, const rect& r)
{
    const style&      st      = ui.theme();
    const font_atlas& fa      = ui.font();
    const bool        light   = is_light_surface(st);
    const color       rail_bg = light ? shift_lum(st.window_bg, -6, 255) : shift_lum(st.window_bg, -4, 255);
    const color       line    = with_alpha(st.border, 200);
    const font_id     mono    = env_.has_mono() ? env_.font_mono : 0;

    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({
        override_var(style_var::padding, 14.0f),
        override_var(style_var::item_spacing, 4.0f),
    });

    if (auto w = ui.window("##tour_rail", r.min, r.size(), flags)) {
        draw_list& dl = ui.draw();
        dl.rect_filled(r, rail_bg);
        dl.line({r.max.x - 1.0f, r.min.y}, {r.max.x - 1.0f, r.max.y}, line, 1.0f);

        ui.text_dim("GUIDED TOUR");
        ui.spacing();

        const auto draw_nav_item = [&](std::string_view id, std::string_view badge, std::string_view title,
                                       std::string_view sub, bool active) -> bool {
            const f32 h = sub.empty() ? 34.0f : 48.0f;
            const auto it = ui.custom_item(id, {0.0f, h});
            if (active) {
                dl.rect_filled(it.bounds, light ? color{0, 0, 0, 14} : shift_lum(st.window_bg, 10, 255), 7.0f);
                dl.rect_filled({{it.bounds.min.x + 1.0f, it.bounds.min.y + 9.0f},
                                {it.bounds.min.x + 3.5f, it.bounds.max.y - 9.0f}},
                               st.accent, 1.5f);
            } else if (it.hovered) {
                dl.rect_filled(it.bounds, light ? color{0, 0, 0, 8} : color{255, 255, 255, 9}, 7.0f);
            }

            const f32 tx = it.bounds.min.x + (badge.empty() ? 12.0f : 38.0f);
            if (sub.empty()) {
                const f32 cy = it.bounds.center().y;
                if (!badge.empty()) {
                    dl.text({it.bounds.min.x + 12.0f, text_vcenter_y(fa, cy, mono)},
                            active ? st.accent : with_alpha(st.text_dim, 175), badge, mono);
                }
                dl.text({tx, text_vcenter_y(fa, cy, 0)}, active ? st.text : st.text_dim, title);
            } else {
                const f32 l1_cy = draw_centered_stack(
                    dl, fa, it.bounds, tx,
                    title, 0, st.text, active ? text_flags::bold : text_flags::none,
                    sub,   0, with_alpha(st.text_dim, 195), 7.0f);
                if (!badge.empty()) {
                    dl.text({it.bounds.min.x + 12.0f, text_vcenter_y(fa, l1_cy, mono)},
                            active ? st.accent : with_alpha(st.text_dim, 175), badge, mono);
                }
            }
            return it.pressed;
        };

        for (std::size_t i = 0; i < k_chapters.size(); ++i) {
            const auto page = static_cast<tour_page>(i);
            const bool active = (tour_page_ == page);
            if (draw_nav_item(std::format("##ch_{}", i), k_chapters[i].num, k_chapters[i].title,
                              k_chapters[i].subtitle, active)) {
                enter_tour_page(ui, page);
            }
        }

        ui.spacing();
        {
            const auto sep = ui.custom_item("##rail_sep", {0.0f, 10.0f});
            dl.line({sep.bounds.min.x, sep.bounds.center().y}, {sep.bounds.max.x, sep.bounds.center().y}, line, 1.0f);
        }

        ui.text_dim("EXPLORE");
        ui.spacing();
        if (draw_nav_item("##ex_workflow", {}, "Workflow & Pickers", {}, tour_page_ == tour_page::workflow)) {
            enter_tour_page(ui, tour_page::workflow);
        }
        if (draw_nav_item("##ex_catalog", {}, "All Components (12)", {}, tour_page_ == tour_page::catalog)) {
            enter_tour_page(ui, tour_page::catalog);
        }

        // Subtle live runtime card pinned at bottom of rail with optically centered 2-line text
        const rect foot{{r.min.x + 14.0f, r.max.y - 64.0f}, {r.max.x - 14.0f, r.max.y - 14.0f}};
        if (foot.min.y > r.min.y + 440.0f) {
            draw_surface_card(dl, foot, light ? color{0, 0, 0, 8} : color{255, 255, 255, 7}, line, 8.0f);
            const f32 l1_cy = draw_centered_stack(
                dl, fa, foot, foot.min.x + 26.0f,
                env_.backend_name, 0, st.text, text_flags::none,
                "Zero heap allocs", mono, with_alpha(st.text_dim, 195), 7.0f);
            dl.circle_filled({foot.min.x + 15.0f, l1_cy}, 3.5f, st.success);
        }
    }
}

void showcase_app::draw_tour_footer(context& ui, const rect& r)
{
    const style& st     = ui.theme();
    const bool   light  = is_light_surface(st);
    const color  bar_bg = light ? shift_lum(st.window_bg, -4, 255) : shift_lum(st.window_bg, -2, 255);
    const color  line   = with_alpha(st.border, 200);

    const auto page_idx = static_cast<int>(tour_page_);
    const bool in_tour_seq = (page_idx >= 0 && page_idx < 5);

    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({
        override_var(style_var::padding, 0.0f),
        override_var(style_var::item_spacing, 0.0f),
    });

    if (auto w = ui.window("##tour_footer", r.min, r.size(), flags)) {
        draw_list& dl = ui.draw();
        dl.rect_filled(r, bar_bg);
        dl.line({r.min.x, r.min.y}, {r.max.x, r.min.y}, line, 1.0f);

        const f32 btn_h = ui.frame_height();
        const f32 pad_y = std::round((r.height() - btn_h) * 0.5f);
        (void)ui.custom_item("##ft_py", {1.0f, pad_y});
        (void)ui.custom_item("##ft_px", {24.0f, 1.0f});
        ui.same_line();

        if (in_tour_seq && page_idx > 0) {
            const auto prev_title = k_chapters[static_cast<std::size_t>(page_idx - 1)].title;
            if (ui.button(std::format("<-  {}", prev_title))) {
                enter_tour_page(ui, static_cast<tour_page>(page_idx - 1));
            }
        } else if (!in_tour_seq) {
            if (ui.button("<-  Back to Overview")) {
                enter_tour_page(ui, tour_page::overview);
            }
        }

        // Centered progress indicator pills
        const f32 dots_w  = 6.0f * 14.0f + 16.0f;
        const f32 dots_x0 = r.center().x - dots_w * 0.5f;
        const f32 cy      = r.center().y;
        f32 dx = dots_x0;
        for (int i = 0; i < 6; ++i) {
            const bool active = (page_idx == i);
            const f32  pw = active ? 22.0f : 6.0f;
            dl.rect_filled({{dx, cy - 3.0f}, {dx + pw, cy + 3.0f}},
                           active ? st.accent : with_alpha(st.text_dim, 90), 3.0f);
            dx += pw + 8.0f;
        }

        if (in_tour_seq) {
            const auto next_title = k_chapters[static_cast<std::size_t>(page_idx + 1)].title;
            const std::string next_lbl = std::format("Next: {}  ->", next_title);
            const f32 nw = ui.font().measure(0, next_lbl).x + st.frame_padding.x * 2.0f + 4.0f;
            ui.same_line_right(nw + 24.0f);
            const auto accent_btn = ui.style_overrides({
                override_color(style_color::widget_bg, st.accent),
                override_color(style_color::widget_hover, st.accent_hover),
                override_color(style_color::widget_border, st.accent),
                override_color(style_color::text, color{255, 255, 255, 255}),
            });
            if (ui.button(next_lbl)) {
                enter_tour_page(ui, static_cast<tour_page>(page_idx + 1));
            }
        }
    }
}

void showcase_app::draw_tour_overview(context& ui, const rect& r)
{
    const style&      st      = ui.theme();
    const font_atlas& fa      = ui.font();
    const bool        light   = is_light_surface(st);
    const color       card_bg = light ? color{255, 255, 255, 245} : shift_lum(st.window_bg, 6, 255);
    const color       line    = with_alpha(st.border, 200);
    const color       accent  = k_hero_swatches[static_cast<std::size_t>(hero_swatch_)];
    const font_id     mono    = env_.has_mono() ? env_.font_mono : 0;

    // Subtle atmospheric radial light and architectural dot grid behind the Overview hero
    {
        draw_list& bg = ui.draw();
        bg.rect_filled(r, st.window_bg);
        const f32 glow_a = light ? 26.0f : 38.0f;
        bg.rect_gradient_radial({{r.min.x + r.width() * 0.36f, r.min.y - 60.0f},
                                 {r.max.x + 80.0f, r.min.y + r.height() * 0.78f}},
                                with_alpha(accent, static_cast<u8>(glow_a)), with_alpha(accent, 0), 0.0f);
        const color dot_col = light ? color{0, 0, 0, 14} : color{255, 255, 255, 13};
        for (f32 y = r.min.y + 24.0f; y < r.max.y; y += 36.0f) {
            for (f32 x = r.min.x + 24.0f; x < r.max.x; x += 36.0f) {
                bg.circle_filled({x, y}, 1.0f, dot_col);
            }
        }
    }

    const f32 content_w = std::min(r.width() - 72.0f, 1000.0f);
    const f32 ox        = r.min.x + std::max(28.0f, (r.width() - content_w) * 0.5f);
    const f32 oy        = r.min.y + std::clamp((r.height() - 560.0f) * 0.42f, 22.0f, 52.0f);
    const rect stage{{ox, oy}, {ox + content_w, r.max.y - 16.0f}};

    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({
        override_var(style_var::padding, 0.0f),
        override_var(style_var::item_spacing, 12.0f),
    });

    if (auto w = ui.window("##tour_overview", stage.min, stage.size(), flags)) {
        draw_list& dl = ui.draw();

        // Top Hero: 2 balanced columns (Story + CTAs on left, Interactive SDF Surface on right)
        if (auto row = ui.row({col_weight(1.12f), col_weight(0.95f)}, 36.0f)) {
            // Left: Editorial intro
            {
                const auto eyebrow = ui.custom_item("##eyebrow", {0.0f, 26.0f});
                constexpr std::string_view k_eye = "IMMEDIATE-MODE C++ UI  ·  ZERO HEAP";
                const vec2 esz = fa.measure(mono, k_eye);
                const rect pill{{eyebrow.bounds.min.x, eyebrow.bounds.min.y},
                                {eyebrow.bounds.min.x + 28.0f + esz.x + 14.0f, eyebrow.bounds.max.y}};
                dl.rect_filled(pill, with_alpha(accent, light ? 22 : 28), 13.0f);
                dl.rect_outline(pill, with_alpha(accent, 70), 13.0f, 1.0f);
                dl.circle_filled({pill.min.x + 13.0f, pill.center().y}, 3.5f, accent);
                dl.text({pill.min.x + 24.0f, text_vcenter_y(fa, pill.center().y, mono)},
                        light ? st.text : accent, k_eye, mono);

                ui.spacing();
                if (env_.has_heading()) {
                    auto h = ui.with_font(env_.font_heading);
                    ui.text("Modern native interfaces,");
                    ui.text("rendered directly on the GPU.");
                } else {
                    ui.text("Modern native interfaces, rendered directly on the GPU.");
                }

                ui.spacing();
                ui.text_wrapped_colored(
                    st.text_dim,
                    "Analytic SDF shapes, live frosted backdrop blur, docking, and pro authoring widgets - "
                    "running identically across Direct3D 11/12, Vulkan, OpenGL, Metal, WebGPU, and CPU.");

                ui.spacing();
                ui.spacing();
                {
                    const auto primary = ui.style_overrides({
                        override_color(style_color::widget_bg, accent),
                        override_color(style_color::widget_hover, shift_lum(accent, 18)),
                        override_color(style_color::widget_border, accent),
                        override_color(style_color::text, color{255, 255, 255, 255}),
                    });
                    if (ui.button("Start Guided Tour  ->")) {
                        enter_tour_page(ui, tour_page::controls);
                    }
                }
                ui.same_line();
                if (ui.button("Open Docking Studio")) {
                    set_scene(ui, "studio");
                }
                ui.same_line();
                if (ui.button("Command Palette")) {
                    palette_.open();
                }

                ui.spacing();
                ui.spacing();

                // 3 clean metric cards with optical vertical centering (equal top & bottom margins)
                const auto metrics_r = ui.custom_item("##hero_metrics", {0.0f, 74.0f});
                static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> k_stats = {{
                    {"0",            "Steady heap allocs"},
                    {"18 \xc2\xb5s", "4-panel UI layout"},
                    {"7",            "Graphics backends"},
                }};
                const f32 gap = 10.0f;
                const f32 cw  = (metrics_r.bounds.width() - gap * 2.0f) / 3.0f;
                const font_id hf = env_.has_heading() ? env_.font_heading : 0;
                for (std::size_t i = 0; i < 3; ++i) {
                    const f32 cx = metrics_r.bounds.min.x + static_cast<f32>(i) * (cw + gap);
                    const rect cr{{cx, metrics_r.bounds.min.y}, {cx + cw, metrics_r.bounds.max.y}};
                    draw_surface_card(dl, cr, card_bg, line, 9.0f);
                    (void)draw_centered_stack(dl, fa, cr, cr.min.x + 16.0f,
                                              k_stats[i].first,  hf, st.text,     text_flags::none,
                                              k_stats[i].second, 0,  st.text_dim, 10.0f);
                }
            }

            ui.row_next();

            // Right: Live Interactive SDF & Glass Card Preview
            {
                const auto canvas = ui.custom_item("##hero_preview", {0.0f, 252.0f});
                const rect& cr = canvas.bounds;
                draw_surface_card(dl, cr, light ? shift_lum(st.window_bg, -8, 255) : color::from_hex(0x0b0c10ff),
                                  line, 12.0f);

                // Soft glowing gradient orbs behind the live floating SDF card
                dl.push_clip(cr);
                const f32 drift = env_.deterministic ? 0.0f : std::sin(env_.time * 0.7f) * 14.0f;
                dl.rect_gradient_radial({{cr.min.x - 10.0f + drift, cr.min.y - 10.0f},
                                         {cr.min.x + cr.width() * 0.72f + drift, cr.min.y + cr.height() * 0.78f}},
                                        with_alpha(accent, 115), with_alpha(accent, 0), 0.0f);
                dl.rect_gradient_radial({{cr.min.x + cr.width() * 0.35f - drift, cr.min.y + cr.height() * 0.25f},
                                         {cr.max.x + 20.0f - drift, cr.max.y + 20.0f}},
                                        with_alpha(color::from_hex(0x3ecf8eff), 72), color{62, 207, 142, 0}, 0.0f);

                const color pgrid = light ? color{0, 0, 0, 12} : color{255, 255, 255, 11};
                for (f32 x = cr.min.x + 28.0f; x < cr.max.x; x += 28.0f) {
                    dl.line({x, cr.min.y}, {x, cr.max.y}, pgrid, 1.0f);
                }
                for (f32 y = cr.min.y + 28.0f; y < cr.max.y; y += 28.0f) {
                    dl.line({cr.min.x, y}, {cr.max.x, y}, pgrid, 1.0f);
                }

                // Floating live SDF card whose corner radius & glass blur update in real time
                const rect card{{cr.min.x + 26.0f, cr.min.y + 26.0f}, {cr.max.x - 26.0f, cr.max.y - 26.0f}};
                if (hero_glass_) {
                    dl.backdrop(card, 18.0f,
                                light ? color{255, 255, 255, 175} : color{18, 20, 27, 175},
                                radii(hero_radius_), 0.03f, 1.25f, 1.04f);
                }
                shape_style card_st;
                card_st.radius        = radii(hero_radius_);
                card_st.fill_top      = hero_glass_
                    ? (light ? color{255, 255, 255, 65} : color{255, 255, 255, 14})
                    : (light ? color{255, 255, 255, 240} : color{20, 22, 29, 245});
                card_st.fill_bottom   = hero_glass_
                    ? (light ? color{255, 255, 255, 35} : color{255, 255, 255, 6})
                    : (light ? color{246, 248, 252, 240} : color{15, 17, 23, 245});
                card_st.border        = light ? color{0, 0, 0, 26} : color{255, 255, 255, 34};
                card_st.border_width  = 1.0f;
                card_st.shadow        = color{0, 0, 0, 110};
                card_st.shadow_blur   = 22.0f;
                card_st.shadow_offset = {0.0f, 10.0f};
                dl.shape(card, card_st);

                // Card header (optically centered on row y = min.y + 22)
                const f32 hdr_cy = card.min.y + 22.0f;
                dl.circle_filled({card.min.x + 18.0f, hdr_cy}, 4.0f, accent);
                dl.text({card.min.x + 30.0f, text_vcenter_y(fa, hdr_cy, mono)}, st.text, "LIVE SDF SURFACE", mono);
                const std::string rad_lbl = std::format("r={:.0f}px", hero_radius_);
                const vec2 rsz = fa.measure(mono, rad_lbl);
                dl.text({card.max.x - 16.0f - rsz.x, text_vcenter_y(fa, hdr_cy, mono)}, st.text_dim, rad_lbl, mono);

                // Card body: live radial ring + smooth signal sparkline
                const vec2 gc{card.min.x + 56.0f, card.center().y + 4.0f};
                const f32  sa = std::numbers::pi_v<f32> * 0.75f;
                const f32  sw = std::numbers::pi_v<f32> * 1.50f;
                const f32  gv = env_.deterministic ? 0.76f : (0.72f + 0.14f * std::sin(env_.time * 1.1f));
                dl.arc(gc, 30.0f, sa, sa + sw, light ? color{0, 0, 0, 24} : color{255, 255, 255, 24}, 6.0f);
                dl.arc(gc, 30.0f, sa, sa + sw * gv, accent, 6.0f);
                const std::string pct = std::format("{:.0f}%", gv * 100.0f);
                dl.text_centered({{gc.x - 24.0f, gc.y - 12.0f}, {gc.x + 24.0f, gc.y + 12.0f}}, st.text, pct, mono);

                std::array<vec2, 24> wave{};
                const f32 wx0     = card.min.x + 112.0f;
                const f32 wx1     = card.max.x - 18.0f;
                const f32 wy_base = card.max.y - 44.0f;
                const f32 phase   = env_.deterministic ? 0.8f : (0.8f + env_.time * 1.4f);
                for (std::size_t i = 0; i < wave.size(); ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(wave.size() - 1);
                    const f32 v = 0.5f + 0.34f * std::sin(t * 5.2f + phase) + 0.14f * std::cos(t * 11.0f - phase * 0.6f);
                    wave[i] = {wx0 + t * (wx1 - wx0), wy_base - v * 58.0f};
                }
                dl.area_fill(wave, wy_base, with_alpha(accent, 65), with_alpha(accent, 0));
                dl.polyline(wave, accent, 2.0f, false);

                // Quiet capability tags optically centered in the card footer strip
                dl.text({card.min.x + 16.0f, text_vcenter_y(fa, card.max.y - 20.0f, mono)}, st.text_dim,
                        hero_glass_ ? "Gaussian Blur  ·  Single-Quad SDF  ·  AA Vector"
                                    : "Opaque Matte  ·  Single-Quad SDF  ·  AA Vector",
                        mono);
                dl.pop_clip();

                // Interactive micro-controls right beneath the live card
                ui.spacing();
                if (auto crow = ui.row({col_weight(1.25f), col_weight(0.75f)}, 14.0f)) {
                    ui.slider("Corner Radius", hero_radius_, 0.0f, 24.0f);
                    ui.row_next();
                    ui.toggle("Glass Blur", hero_glass_);
                }

                const auto sw_it = ui.custom_item("##hero_swatches", {0.0f, 28.0f});
                dl.text({sw_it.bounds.min.x, text_vcenter_y(fa, sw_it.bounds.center().y, 0)},
                        st.text_dim, "Preview Accent");
                const f32 sx0 = sw_it.bounds.min.x + 118.0f;
                for (int i = 0; i < 4; ++i) {
                    const vec2 sc{sx0 + static_cast<f32>(i) * 30.0f, sw_it.bounds.center().y};
                    if (hero_swatch_ == i) {
                        dl.circle(sc, 10.5f, st.text, 1.5f);
                    }
                    dl.circle_filled(sc, 7.0f, k_hero_swatches[static_cast<std::size_t>(i)]);
                    if (sw_it.pressed) {
                        const vec2 mp = ui.mouse_pos();
                        const f32 dx = mp.x - sc.x;
                        const f32 dy = mp.y - sc.y;
                        if (dx * dx + dy * dy <= 144.0f) {
                            hero_swatch_ = i;
                        }
                    }
                }
            }
        }

        ui.spacing();
        ui.spacing();
        ui.text_dim("EXPLORE BY CHAPTER");

        struct card_meta {
            std::string_view num;
            std::string_view title;
            std::string_view desc;
            std::string_view icon;
            tour_page        target;
        };
        static constexpr std::array<card_meta, 3> k_cards = {{
            {"01", "Controls & Live Preview",
             "Property grids, knobs & live material preview",
             svg_icons::settings, tour_page::controls},
            {"02", "SDF, Glass & Typography",
             "Analytic gradients, acrylic blur & RTL text",
             svg_icons::eye, tour_page::visuals},
            {"03 - 05", "Data, Authoring & Studio",
             "100k-row tables, node graph, code & docking",
             svg_icons::folder, tour_page::data},
        }};

        if (auto crow = ui.row({col_weight(1.0f), col_weight(1.0f), col_weight(1.0f)}, 16.0f)) {
            for (std::size_t i = 0; i < 3; ++i) {
                if (i > 0) { ui.row_next(); }
                const auto it = ui.custom_item(std::format("##ov_card_{}", i), {0.0f, 106.0f});
                const color bg = it.hovered
                    ? (light ? color{255, 255, 255, 255} : shift_lum(st.window_bg, 11, 255))
                    : card_bg;
                const color bd = it.hovered ? with_alpha(accent, 150) : line;
                draw_surface_card(dl, it.bounds, bg, bd, 10.0f);

                const rect icon_box{{it.bounds.min.x + 16.0f, it.bounds.min.y + 16.0f},
                                    {it.bounds.min.x + 42.0f, it.bounds.min.y + 42.0f}};
                dl.rect_filled(icon_box, with_alpha(accent, light ? 22 : 28), 7.0f);
                const rect icon_in{{icon_box.min.x + 5.0f, icon_box.min.y + 5.0f},
                                   {icon_box.max.x - 5.0f, icon_box.max.y - 5.0f}};
                draw_svg_path(dl, icon_in, k_cards[i].icon, svg_icons::viewbox, accent, 1.65f);

                const vec2 nsz = fa.measure(mono, k_cards[i].num);
                dl.text({it.bounds.max.x - 16.0f - nsz.x, text_vcenter_y(fa, icon_box.center().y, mono)},
                        with_alpha(st.text_dim, 180), k_cards[i].num, mono);

                const rect text_area{{it.bounds.min.x + 16.0f, icon_box.max.y + 8.0f},
                                     {it.bounds.max.x - 16.0f, it.bounds.max.y - 12.0f}};
                (void)draw_centered_stack(dl, fa, text_area, text_area.min.x,
                                          k_cards[i].title, 0, st.text, text_flags::bold,
                                          k_cards[i].desc,  0, st.text_dim, 8.0f);

                if (it.pressed) {
                    enter_tour_page(ui, k_cards[i].target);
                }
            }
        }
    }
}

void showcase_app::draw_material_preview(context& ui, const rect& bounds)
{
    const style&      st   = ui.theme();
    const font_atlas& fa   = ui.font();
    const auto&       p    = env_.preview;
    const font_id     mono = env_.has_mono() ? env_.font_mono : 0;
    const f32         mlh  = fa.line_height(mono);
    draw_list&        dl   = ui.draw();

    draw_surface_card(dl, bounds, color::from_hex(0x0b0d12ff), with_alpha(st.border, 200), 10.0f);
    dl.push_clip(bounds);

    dl.rect_gradient_v(bounds, color::from_hex(0x111520ff), color::from_hex(0x090a0eff));

    // Perspective studio floor lines
    const f32 floor_y = bounds.min.y + bounds.height() * 0.72f;
    dl.line({bounds.min.x, floor_y}, {bounds.max.x, floor_y}, color{255, 255, 255, 16}, 1.0f);
    for (int i = 1; i <= 4; ++i) {
        const f32 t = static_cast<f32>(i) / 4.0f;
        const f32 y = floor_y + t * t * (bounds.max.y - floor_y);
        dl.line({bounds.min.x, y}, {bounds.max.x, y}, color{255, 255, 255, 10}, 1.0f);
    }

    const f32 exp_mul  = std::clamp(p.exposure / 1.35f, 0.35f, 2.2f);
    const f32 fov_zoom = std::clamp(68.0f / std::max(p.fov_deg, 24.0f), 0.72f, 1.35f);
    const f32 radius   = std::min(bounds.width(), bounds.height()) * 0.25f * fov_zoom;
    const f32 bob      = env_.deterministic ? 0.0f : std::sin(env_.time * 1.2f) * 5.0f;
    const vec2 center{bounds.center().x, bounds.min.y + bounds.height() * 0.48f + bob};

    const f32 bloom_str = std::clamp(p.bloom * 0.55f * exp_mul, 0.15f, 1.2f);
    const f32 halo_r    = radius * (1.75f + bloom_str * 0.45f);
    dl.rect_gradient_radial({{center.x - halo_r, center.y - halo_r}, {center.x + halo_r, center.y + halo_r}},
                            with_alpha(p.tint, static_cast<u8>(std::clamp(70.0f * bloom_str, 18.0f, 130.0f))),
                            with_alpha(p.tint, 0), 0.0f);

    if (p.shadows) {
        const f32 sx = radius * 1.15f;
        const f32 sy = radius * 0.24f;
        dl.rect_gradient_radial({{center.x - sx, floor_y + 12.0f - sy}, {center.x + sx, floor_y + 12.0f + sy}},
                                color{0, 0, 0, 175}, color{0, 0, 0, 0}, 0.0f);
    }

    const auto scale_rgb = [](color c, f32 k, u8 a = 255) -> color {
        return {
            static_cast<u8>(std::clamp(static_cast<f32>(c.r) * k, 0.0f, 255.0f)),
            static_cast<u8>(std::clamp(static_cast<f32>(c.g) * k, 0.0f, 255.0f)),
            static_cast<u8>(std::clamp(static_cast<f32>(c.b) * k, 0.0f, 255.0f)),
            a,
        };
    };

    const rect sphere_r{{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}};
    const color base_dark = scale_rgb(p.tint, (0.22f - p.metallic * 0.10f) * exp_mul);
    const color base_lit  = scale_rgb(p.tint, (0.78f + p.metallic * 0.22f) * exp_mul);
    dl.rect_gradient_angle(sphere_r, base_lit, base_dark, 132.0f, radius);

    const vec2 light_c{center.x - radius * 0.34f, center.y - radius * 0.36f};
    const f32  diff_r = radius * 1.05f;
    dl.rect_gradient_radial({{light_c.x - diff_r, light_c.y - diff_r}, {light_c.x + diff_r, light_c.y + diff_r}},
                            with_alpha(lerp(p.tint, color{255, 255, 255, 255}, 0.45f),
                                       static_cast<u8>(std::clamp(145.0f * exp_mul, 40.0f, 210.0f))),
                            with_alpha(p.tint, 0), radius);

    const f32 spec_r   = radius * std::lerp(0.24f, 0.85f, p.roughness);
    const f32 spec_a   = std::lerp(240.0f, 65.0f, p.roughness) * std::clamp(exp_mul, 0.5f, 1.3f);
    const color spec_c = lerp(color{255, 255, 255, 255}, p.tint, p.metallic * 0.55f);
    dl.rect_gradient_radial({{light_c.x - spec_r, light_c.y - spec_r}, {light_c.x + spec_r, light_c.y + spec_r}},
                            with_alpha(spec_c, static_cast<u8>(std::clamp(spec_a, 20.0f, 245.0f))),
                            with_alpha(spec_c, 0), spec_r);

    dl.circle(center, radius - 0.5f, with_alpha(lerp(p.tint, color{255, 255, 255, 255}, 0.35f), 70), 1.25f);

    static constexpr std::array<std::string_view, 4> k_shaders = {
        "PBR Standard", "Clearcoat Glass", "Subsurface", "Anisotropic"
    };
    static constexpr std::array<std::string_view, 3> k_qual = {"Draft", "Balanced", "Studio"};
    const std::string_view sh_name = k_shaders[static_cast<std::size_t>(std::clamp(p.shader_preset, 0, 3))];
    const std::string_view q_name  = k_qual[static_cast<std::size_t>(std::clamp(p.quality, 0, 2))];

    dl.text({bounds.min.x + 16.0f, bounds.min.y + 14.0f}, color{235, 238, 245, 230}, "LIVE MATERIAL VIEWPORT", mono);
    dl.text({bounds.min.x + 16.0f, bounds.min.y + 14.0f + mlh + 2.0f}, color{140, 148, 166, 220},
            std::format("{}  ·  {}  ·  {}", env_.selected_entity, sh_name, q_name), mono);

    // Bottom live parameter readout strip (optically centered)
    const rect hud{{bounds.min.x + 14.0f, bounds.max.y - 44.0f}, {bounds.max.x - 14.0f, bounds.max.y - 14.0f}};
    dl.rect_filled(hud, color{15, 17, 23, 210}, 7.0f);
    dl.rect_outline(hud, color{255, 255, 255, 22}, 7.0f, 1.0f);
    const std::string readout = std::format(
        "Exp {:.2f}   Rough {:.2f}   Metal {:.2f}   FOV {:.0f} deg",
        p.exposure, p.roughness, p.metallic, p.fov_deg);
    dl.text({hud.min.x + 14.0f, text_vcenter_y(fa, hud.center().y, mono)}, color{210, 216, 228, 235}, readout, mono);

    dl.pop_clip();
}

void showcase_app::draw_tour_controls(context& ui, const rect& r)
{
    const style& st      = ui.theme();
    const bool   light   = is_light_surface(st);
    const color  card_bg = light ? color{255, 255, 255, 248} : shift_lum(st.window_bg, 5, 255);
    const color  line    = with_alpha(st.border, 200);

    ui.draw().rect_filled(r, st.window_bg);

    const rect inner{{r.min.x + 28.0f, r.min.y + 22.0f}, {r.max.x - 28.0f, r.max.y - 16.0f}};
    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({override_var(style_var::padding, 0.0f)});

    if (auto w = ui.window("##tour_controls", inner.min, inner.size(), flags)) {
        if (env_.has_heading()) {
            auto h = ui.with_font(env_.font_heading);
            ui.text("Controls & Inspector");
        } else {
            ui.text("Controls & Inspector");
        }
        ui.text_wrapped_colored(st.text_dim,
                                "Every widget on the left drives the procedural material viewport on the right in real time.");
        ui.spacing();

        const f32 body_h = std::max(inner.height() - 90.0f, 320.0f);
        if (auto row = ui.row({col_weight(1.05f), col_weight(1.0f)}, 20.0f)) {
            {
                const auto card_ov = ui.style_overrides({
                    override_color(style_color::window_bg, card_bg),
                    override_color(style_color::border, line),
                    override_var(style_var::padding, 16.0f),
                });
                if (auto c = ui.child("ctrl_inspector_card", {0.0f, body_h}, child_flags::frame)) {
                    if (auto* mod = find_module("inspector")) {
                        mod->draw(ui, env_);
                    }
                }
            }

            ui.row_next();

            const auto vp = ui.custom_item("##material_vp", {0.0f, body_h});
            draw_material_preview(ui, vp.bounds);
        }
    }
}

void showcase_app::draw_tour_visuals(context& ui, const rect& r)
{
    const style& st    = ui.theme();
    const bool   light = is_light_surface(st);

    if (auto* vmod = find_module("visuals")) {
        ui.draw().push_clip(r);
        vmod->draw_backdrop(ui, env_);
        ui.draw().pop_clip();
    }

    const f32 pad     = 26.0f;
    const f32 gap     = 20.0f;
    const f32 avail_w = std::max(r.width() - pad * 2.0f - gap, 400.0f);
    const f32 avail_h = std::max(r.height() - pad * 2.0f, 360.0f);
    const f32 left_w  = std::round(avail_w * 0.56f);
    const f32 right_w = avail_w - left_w;

    const auto glass_ov = ui.style_overrides({
        override_color(style_color::window_bg, light ? color{246, 248, 253, 212} : color{18, 20, 28, 204}),
        override_color(style_color::border,    light ? color{0, 0, 0, 28}        : color{255, 255, 255, 30}),
        override_var(style_var::rounding, 11.0f),
        override_var(style_var::padding, 18.0f),
    });

    constexpr auto gflags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                            window_flags::acrylic;

    if (auto w = ui.window("##tour_vis_left", {r.min.x + pad, r.min.y + pad}, {left_w, avail_h}, gflags)) {
        if (env_.has_heading()) {
            auto h = ui.with_font(env_.font_heading);
            ui.text("SDF Shapes & Frosted Glass");
        } else {
            ui.text("SDF Shapes & Frosted Glass");
        }
        ui.spacing();
        if (auto* mod = find_module("visuals")) {
            mod->draw(ui, env_);
        }
    }

    if (auto w = ui.window("##tour_vis_right", {r.min.x + pad + left_w + gap, r.min.y + pad}, {right_w, avail_h}, gflags)) {
        if (auto* mod = find_module("typography")) {
            mod->draw(ui, env_);
        }
    }
}

void showcase_app::draw_tour_data(context& ui, const rect& r)
{
    const style& st      = ui.theme();
    const bool   light   = is_light_surface(st);
    const color  card_bg = light ? color{255, 255, 255, 248} : shift_lum(st.window_bg, 5, 255);
    const color  line    = with_alpha(st.border, 200);

    ui.draw().rect_filled(r, st.window_bg);

    const rect inner{{r.min.x + 28.0f, r.min.y + 22.0f}, {r.max.x - 28.0f, r.max.y - 16.0f}};
    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({override_var(style_var::padding, 0.0f)});

    if (auto w = ui.window("##tour_data", inner.min, inner.size(), flags)) {
        if (auto hrow = ui.row({col_weight(1.4f), col_fixed(340.0f)}, 16.0f)) {
            if (env_.has_heading()) {
                auto h = ui.with_font(env_.font_heading);
                ui.text("Data & Telemetry");
            } else {
                ui.text("Data & Telemetry");
            }
            ui.text_wrapped_colored(st.text_dim,
                                    "Switch between interactive GPU charts and a 100,000-row virtualized profiler table.");
            ui.row_next();
            static constexpr std::string_view k_tabs[] = {"Live Telemetry", "100k-Row Grid"};
            ui.segmented("##data_subtab", k_tabs, data_tab_);
        }
        ui.spacing();

        const f32 body_h = std::max(inner.height() - 90.0f, 340.0f);
        const auto card_ov = ui.style_overrides({
            override_color(style_color::window_bg, card_bg),
            override_color(style_color::border, line),
            override_var(style_var::padding, 16.0f),
        });
        if (auto c = ui.child("data_stage_card", {0.0f, body_h}, child_flags::frame)) {
            if (data_tab_ == 0) {
                if (auto* mod = find_module("charts")) { mod->draw(ui, env_); }
            } else {
                if (auto* mod = find_module("data")) { mod->draw(ui, env_); }
            }
        }
    }
}

void showcase_app::draw_tour_authoring(context& ui, const rect& r)
{
    const style& st      = ui.theme();
    const bool   light   = is_light_surface(st);
    const color  card_bg = light ? color{255, 255, 255, 248} : shift_lum(st.window_bg, 5, 255);
    const color  line    = with_alpha(st.border, 200);

    ui.draw().rect_filled(r, st.window_bg);

    const rect inner{{r.min.x + 28.0f, r.min.y + 22.0f}, {r.max.x - 28.0f, r.max.y - 16.0f}};
    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({override_var(style_var::padding, 0.0f)});

    if (auto w = ui.window("##tour_authoring", inner.min, inner.size(), flags)) {
        if (auto hrow = ui.row({col_weight(1.3f), col_fixed(420.0f)}, 16.0f)) {
            if (env_.has_heading()) {
                auto h = ui.with_font(env_.font_heading);
                ui.text("Authoring Tools");
            } else {
                ui.text("Authoring Tools");
            }
            ui.text_wrapped_colored(st.text_dim,
                                    "Full-size editors with room to work - no cramped panels or clipped labels.");
            ui.row_next();
            static constexpr std::string_view k_tabs[] = {"Node Graph", "Code Editor", "Sequencer & Curves"};
            ui.segmented("##auth_subtab", k_tabs, authoring_tab_);
        }
        ui.spacing();

        const f32 body_h = std::max(inner.height() - 90.0f, 360.0f);
        const auto card_ov = ui.style_overrides({
            override_color(style_color::window_bg, card_bg),
            override_color(style_color::border, line),
            override_var(style_var::padding, 16.0f),
        });
        if (auto c = ui.child("auth_stage_card", {0.0f, body_h}, child_flags::frame)) {
            if (authoring_tab_ == 0) {
                if (auto* mod = find_module("nodes")) { mod->draw(ui, env_); }
            } else if (authoring_tab_ == 1) {
                if (auto* mod = find_module("editor")) { mod->draw(ui, env_); }
            } else {
                if (auto* mod = find_module("sequencer")) { mod->draw(ui, env_); }
            }
        }
    }
}

void showcase_app::draw_tour_workflow(context& ui, const rect& r)
{
    const style& st      = ui.theme();
    const bool   light   = is_light_surface(st);
    const color  card_bg = light ? color{255, 255, 255, 248} : shift_lum(st.window_bg, 5, 255);
    const color  line    = with_alpha(st.border, 200);

    ui.draw().rect_filled(r, st.window_bg);

    const rect inner{{r.min.x + 28.0f, r.min.y + 22.0f}, {r.max.x - 28.0f, r.max.y - 16.0f}};
    constexpr auto flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed |
                           window_flags::no_background;
    const auto pad_ov = ui.style_overrides({override_var(style_var::padding, 0.0f)});

    if (auto w = ui.window("##tour_workflow", inner.min, inner.size(), flags)) {
        if (env_.has_heading()) {
            auto h = ui.with_font(env_.font_heading);
            ui.text("Workflow, Pickers & Overlays");
        } else {
            ui.text("Workflow, Pickers & Overlays");
        }
        ui.text_wrapped_colored(st.text_dim,
                                "Drag-and-drop Kanban boards, date/time pickers, reorderable tabs, modals, toasts, and keybind editing.");
        ui.spacing();

        const f32 body_h = std::max(inner.height() - 92.0f, 340.0f);
        if (auto row = ui.row({col_weight(1.0f), col_weight(1.0f)}, 20.0f)) {
            const auto card_ov = ui.style_overrides({
                override_color(style_color::window_bg, card_bg),
                override_color(style_color::border, line),
                override_var(style_var::padding, 16.0f),
            });
            if (auto c = ui.child("wf_pickers_card", {0.0f, body_h}, child_flags::frame)) {
                if (auto* mod = find_module("pickers")) { mod->draw(ui, env_); }
            }
            ui.row_next();
            if (auto c = ui.child("wf_overlays_card", {0.0f, body_h}, child_flags::frame)) {
                if (auto* mod = find_module("overlays")) { mod->draw(ui, env_); }
            }
        }
    }
}

void showcase_app::draw_menu_bar(context& ui, f32& out_bar_h)
{
    out_bar_h = 0.0f;
    // Pure menu items inside main_menu_bar (no mismatched buttons) so hover/height/mnemonics behave cleanly.
    if (auto bar = ui.main_menu_bar()) {
        if (auto m = ui.menu("&Strata")) {
            if (ui.menu_item("Back to Guided Tour", "Ctrl+0")) {
                enter_tour_page(ui, tour_page::overview);
            }
            if (ui.menu_item("Command Palette...", "Ctrl+Shift+P")) {
                palette_.open();
            }
            ui.menu_separator();
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> k_ws = {{
                {"studio",       "Studio (3-Panel)"},
                {"telemetry",    "Telemetry & Data"},
                {"authoring",    "Authoring & Shaders"},
                {"visuals",      "Visuals & Glass"},
                {"gallery",      "All Components"},
            }};
            for (const auto& [id, label] : k_ws) {
                if (ui.menu_item(label, {}, active_workspace_ == id)) {
                    set_scene(ui, id);
                }
            }
            ui.menu_separator();
            if (ui.menu_item("Reset Layout")) {
                set_scene(ui, active_workspace_);
            }
        }

        if (auto m = ui.menu("&Panels")) {
            if (active_workspace_ == "studio") {
                ui.menu_item("Viewport", studio_viewport_visible_);
                ui.menu_separator();
            }
            for (auto& slot : modules_) {
                if (ui.menu_item(slot.info.title, {}, slot.visible)) {
                    if (gallery_mode_) {
                        gallery_selected_ = std::string(slot.info.id);
                    } else {
                        slot.visible = !slot.visible;
                    }
                }
            }
        }

        if (auto m = ui.menu("&Themes")) {
            const auto names = themes::names();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (ui.menu_item(names[i], {}, theme_idx_ == static_cast<int>(i))) {
                    apply_theme(ui, static_cast<int>(i));
                    log_.addf(log_level::info, "Switched theme to {}", names[i]);
                }
            }
        }

        if (auto m = ui.menu("&Tools")) {
            ui.menu_item("Frame Metrics",    show_metrics_);
            ui.menu_item("Widget Inspector", show_inspector_);
            ui.menu_item("Draw List",        show_draw_list_);
            ui.menu_separator();
            bool reduced = ui.theme().reduce_motion;
            if (ui.menu_item("Reduce Motion", {}, reduced)) {
                ui.theme().reduce_motion = !reduced;
            }
        }

        // Right-aligned quiet runtime readout measured dynamically so it never collides or clips
        const std::string stats = std::format("{}  ·  {:.0f} fps  ·  {} draws  ·  {:.2f} ms",
                                              env_.backend_name, env_.fps, ui.stats().draw_calls, env_.ui_ms);
        const f32 sw = ui.font().measure(0, stats).x;
        ui.same_line_right(sw + 14.0f);
        ui.text_dim(stats);

        out_bar_h = ui.main_menu_bar_height();
    }
}

void showcase_app::apply_workspace_layout(context& ui)
{
    if (active_workspace_ == "studio") {
        // Focused 3-panel creative workspace: Outliner left, Live Viewport + Node Graph center, Inspector right.
        ui.dock_window("Scene Explorer", dock_zone::center, {},         0.5f, "left");
        ui.dock_window("Node Graph",     dock_zone::center, {},         0.5f, {});
        ui.dock_window("Viewport",       dock_zone::center, "Node Graph", 0.5f, {});
        ui.dock_window("Inspector",      dock_zone::center, {},         0.5f, "right");
    } else if (active_workspace_ == "telemetry") {
        ui.dock_window("Telemetry",          dock_zone::center, {},            0.5f, {});
        ui.dock_window("Inspector",          dock_zone::center, {},            0.5f, "right");
        ui.dock_window("Console",            dock_zone::center, {},            0.5f, "bottom");
        ui.dock_window("Data Grid",          dock_zone::center, "Console",     0.5f, "bottom");
    } else if (active_workspace_ == "authoring") {
        ui.dock_window("Code Editor",        dock_zone::center, {},            0.5f, "left");
        ui.dock_window("Node Graph",         dock_zone::center, {},            0.5f, {});
        ui.dock_window("Inspector",          dock_zone::center, {},            0.5f, "right");
        ui.dock_window("Sequencer & Curves", dock_zone::center, {},            0.5f, "bottom");
    } else if (active_workspace_ == "productivity") {
        ui.dock_window("Workflow & Pickers", dock_zone::center, {},            0.5f, "left");
        ui.dock_window("Tabs & Overlays",    dock_zone::center, {},            0.5f, {});
        ui.dock_window("Console",            dock_zone::center, {},            0.5f, "bottom");
    }
}

void showcase_app::draw_gallery(context& ui, const rect& client)
{
    const style& st      = ui.theme();
    const bool   light   = is_light_surface(st);
    const color  nav_bg  = light ? shift_lum(st.window_bg, -4, 255) : shift_lum(st.window_bg, -2, 255);
    const f32 sidebar_w  = std::clamp(client.width() * 0.22f, 210.0f, 265.0f);
    const rect left_r{client.min, {client.min.x + sidebar_w, client.max.y}};
    const rect main_r{{client.min.x + sidebar_w, client.min.y}, client.max};

    constexpr auto fixed_flags = window_flags::no_title_bar | window_flags::no_move | window_flags::placed;

    {
        const auto nav_ov = ui.style_overrides({
            override_color(style_color::window_bg, nav_bg),
            override_var(style_var::rounding, 0.0f),
        });
        if (auto w = ui.window("##gallery_nav", left_r.min, left_r.size(), fixed_flags)) {
            if (env_.has_heading()) {
                auto h = ui.with_font(env_.font_heading);
                ui.text("Components");
            } else {
                ui.text("Components");
            }
            ui.text_dim("12 modular strata panels");
            ui.spacing();
            (void)ui.input_text("##gallery_filter", gallery_filter_, "Filter modules...");
            ui.separator();

            static constexpr std::array<module_category, 6> k_cats = {
                module_category::studio,
                module_category::controls,
                module_category::data,
                module_category::authoring,
                module_category::visuals,
                module_category::system,
            };

            for (module_category cat : k_cats) {
                bool any_in_cat = false;
                for (const auto& slot : modules_) {
                    if (slot.info.category == cat &&
                        (icontains(slot.info.title, gallery_filter_) || icontains(slot.info.summary, gallery_filter_))) {
                        any_in_cat = true;
                        break;
                    }
                }
                if (!any_in_cat) { continue; }

                ui.spacing();
                ui.text_dim(category_label(cat));
                for (const auto& slot : modules_) {
                    if (slot.info.category != cat) { continue; }
                    if (!icontains(slot.info.title, gallery_filter_) && !icontains(slot.info.summary, gallery_filter_)) {
                        continue;
                    }
                    const bool sel = (gallery_selected_ == slot.info.id);
                    if (ui.selectable(slot.info.title, sel)) {
                        gallery_selected_ = std::string(slot.info.id);
                    }
                }
            }
        }
    }

    {
        const auto stage_ov = ui.style_overrides({
            override_var(style_var::rounding, 0.0f),
            override_var(style_var::padding, 22.0f),
        });
        if (auto w = ui.window("##gallery_stage", main_r.min, main_r.size(), fixed_flags)) {
            for (auto& slot : modules_) {
                if (slot.info.id == gallery_selected_) {
                    if (env_.has_heading()) {
                        auto h = ui.with_font(env_.font_heading);
                        ui.text(slot.info.title);
                    } else {
                        ui.text(slot.info.title);
                    }
                    ui.same_line_right(110.0f);
                    ui.text_dim(category_label(slot.info.category));
                    ui.text_wrapped_colored(st.text_dim, slot.info.summary);
                    ui.separator();
                    slot.mod->draw(ui, env_);
                    break;
                }
            }
        }
    }
}

void showcase_app::tick(context& ui, f32 dt, f32 fps, f32 ui_ms)
{
    env_.fps   = fps;
    env_.ui_ms = ui_ms;
    env_.frame += 1;
    if (env_.animate) {
        env_.time += dt * env_.speed;
    }

    if (env_.requested_theme >= 0) {
        apply_theme(ui, env_.requested_theme);
        env_.requested_theme = -1;
    }

    for (auto& slot : modules_) {
        slot.mod->update(env_, dt);
    }

    if (!ui.want_text_input()) {
        for (const auto& a : binds_.actions()) {
            if (binds_.pressed(ui, a.name)) {
                run_action(ui, a.name);
                break;
            }
        }
    }

    const vec2 ds = ui.display_size();

    if (tour_mode_) {
        const f32 header_h = 44.0f;
        const f32 rail_w   = 236.0f;
        const f32 footer_h = 50.0f;

        draw_tour_header(ui, ds.x, header_h);
        draw_tour_rail(ui, {{0.0f, header_h}, {rail_w, ds.y}});

        const rect stage{{rail_w, header_h}, {ds.x, ds.y - footer_h}};
        const rect footer{{rail_w, ds.y - footer_h}, ds};

        switch (tour_page_) {
        case tour_page::overview:  draw_tour_overview(ui, stage);  break;
        case tour_page::controls:  draw_tour_controls(ui, stage);  break;
        case tour_page::visuals:   draw_tour_visuals(ui, stage);   break;
        case tour_page::data:      draw_tour_data(ui, stage);      break;
        case tour_page::authoring: draw_tour_authoring(ui, stage); break;
        case tour_page::workflow:  draw_tour_workflow(ui, stage);  break;
        case tour_page::catalog:   draw_gallery(ui, stage);        break;
        case tour_page::studio:    break;
        }

        draw_tour_footer(ui, footer);
    } else {
        if (env_.draw_backdrop) {
            for (auto& slot : modules_) {
                slot.mod->draw_backdrop(ui, env_);
            }
        }

        f32 bar_h = 0.0f;
        draw_menu_bar(ui, bar_h);

        rect client{{0.0f, bar_h}, ds};

        if (gallery_mode_) {
            draw_gallery(ui, client);
        } else if (active_workspace_ == "visuals") {
            const f32 pad     = 28.0f;
            const f32 gap     = 24.0f;
            const f32 avail_w = std::max(client.width() - pad * 2.0f - gap, 400.0f);
            const f32 avail_h = std::max(client.height() - pad * 2.0f, 360.0f);
            const f32 left_w  = std::round(avail_w * 0.56f);
            const f32 right_w = avail_w - left_w;

            constexpr auto glass_flags = window_flags::resizable | window_flags::acrylic;
            for (auto& slot : modules_) {
                if (!slot.visible) { continue; }
                if (slot.info.id == "visuals") {
                    if (auto w = ui.window(slot.info.title, {client.min.x + pad, client.min.y + pad},
                                           {left_w, avail_h}, glass_flags)) {
                        slot.mod->draw(ui, env_);
                    }
                } else if (slot.info.id == "typography") {
                    if (auto w = ui.window(slot.info.title, {client.min.x + pad + left_w + gap, client.min.y + pad},
                                           {right_w, avail_h}, glass_flags)) {
                        slot.mod->draw(ui, env_);
                    }
                }
            }
        } else {
            if (active_workspace_ == "studio") {
                client = ui.dock_edge("left",  dock_side::left,  std::clamp(ds.x * 0.18f, 215.0f, 260.0f), client);
                client = ui.dock_edge("right", dock_side::right, std::clamp(ds.x * 0.25f, 300.0f, 360.0f), client);
                ui.dock_area(client);
            } else if (active_workspace_ == "telemetry") {
                client = ui.dock_edge("right",  dock_side::right,  std::clamp(ds.x * 0.24f, 270.0f, 340.0f), client);
                client = ui.dock_edge("bottom", dock_side::bottom, std::clamp(ds.y * 0.34f, 220.0f, 300.0f), client);
                ui.dock_area(client);
            } else if (active_workspace_ == "authoring") {
                client = ui.dock_edge("left",   dock_side::left,   std::clamp(ds.x * 0.36f, 360.0f, 520.0f), client);
                client = ui.dock_edge("right",  dock_side::right,  std::clamp(ds.x * 0.22f, 250.0f, 330.0f), client);
                client = ui.dock_edge("bottom", dock_side::bottom, std::clamp(ds.y * 0.28f, 195.0f, 255.0f), client);
                ui.dock_area(client);
            } else if (active_workspace_ == "productivity") {
                client = ui.dock_edge("left",   dock_side::left,   std::clamp(ds.x * 0.48f, 420.0f, 680.0f), client);
                client = ui.dock_edge("bottom", dock_side::bottom, std::clamp(ds.y * 0.26f, 180.0f, 240.0f), client);
                ui.dock_area(client);
            } else {
                ui.dock_area(client);
            }

            constexpr auto wflags = window_flags::dockable | window_flags::resizable;
            if (active_workspace_ == "studio" && studio_viewport_visible_) {
                if (auto w = ui.window("Viewport", {280.0f, bar_h + 24.0f}, {640.0f, 560.0f}, wflags)) {
                    const auto vp = ui.custom_item("##studio_vp", {0.0f, std::max(ds.y - bar_h - 90.0f, 360.0f)});
                    draw_material_preview(ui, vp.bounds);
                }
            }
            for (auto& slot : modules_) {
                if (!slot.visible) { continue; }
                if (auto w = ui.window(slot.info.title, {40.0f, bar_h + 24.0f}, slot.info.default_size, wflags)) {
                    slot.mod->draw(ui, env_);
                }
            }

            if (layout_dirty_) {
                layout_dirty_ = false;
                apply_workspace_layout(ui);
            }
        }
    }

    if (env_.open_palette || (open_palette_next_ && env_.frame >= 2)) {
        env_.open_palette  = false;
        open_palette_next_ = false;
        palette_.open();
    }
    if (const std::string picked = palette_.show(ui, binds_); !picked.empty()) {
        run_action(ui, picked);
    }

    ui.debug_metrics_window(show_metrics_);
    ui.debug_inspector_window(show_inspector_);
    ui.debug_draw_list_window(show_draw_list_);
}

color showcase_app::clear_color(const context& ui) const noexcept
{
    if (env_.draw_backdrop) {
        return color{10, 12, 18, 255};
    }
    const color bg = ui.theme().window_bg;
    return color{
        static_cast<u8>(bg.r > 5 ? bg.r - 5 : 0),
        static_cast<u8>(bg.g > 5 ? bg.g - 5 : 0),
        static_cast<u8>(bg.b > 5 ? bg.b - 5 : 0),
        255,
    };
}

f32 showcase_app::take_requested_scale() noexcept
{
    const f32 s = env_.requested_scale;
    env_.requested_scale = 0.0f;
    return s;
}

} // namespace strata::showcase

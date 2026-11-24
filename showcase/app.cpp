#include "showcase/app.hpp"
#include "showcase/modules/registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace strata::showcase {
namespace {

constexpr std::array<std::string_view, 6> k_workspaces = {
    "studio",
    "telemetry",
    "authoring",
    "visuals",
    "productivity",
    "gallery",
};

constexpr std::array<std::string_view, 6> k_workspace_labels = {
    "Studio",
    "Telemetry",
    "Authoring",
    "Visuals",
    "Workflow",
    "Gallery",
};

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
    reg("ws_studio",        "Workspace: Studio",              "Ctrl+1",         "Full docked studio layout");
    reg("ws_telemetry",     "Workspace: Telemetry & Data",    "Ctrl+2",         "Charts, heatmap, and 100k-row profiler grid");
    reg("ws_authoring",     "Workspace: Authoring & Shaders", "Ctrl+3",         "Code editor, node graph, and sequencer");
    reg("ws_visuals",       "Workspace: Visuals & Glass",     "Ctrl+4",         "SDF gradients, vector primitives, frosted acrylic");
    reg("ws_productivity",  "Workspace: Workflow & Pickers",  "Ctrl+5",         "Kanban drag-and-drop, pickers, tabs, keybinds");
    reg("ws_gallery",       "Workspace: Component Gallery",   "Ctrl+6",         "Searchable catalog of all showcase modules");
    reg("toggle_animate",   "Simulation: Toggle Playback",    "Space",          "Pause or resume live telemetry animation");
    reg("theme_midnight",   "Theme: Midnight",                "Ctrl+K, Ctrl+1", "Apply Midnight dark theme");
    reg("theme_light",      "Theme: Light",                   "Ctrl+K, Ctrl+2", "Apply Light theme");
    reg("theme_dracula",    "Theme: Dracula",                 "Ctrl+K, Ctrl+3", "Apply Dracula theme");
    reg("theme_nord",       "Theme: Nord",                    "Ctrl+K, Ctrl+4", "Apply Nord theme");
    reg("theme_glass",      "Theme: Frosted Glass",           "Ctrl+K, Ctrl+5", "Apply Frosted Glass acrylic theme");
    reg("toggle_metrics",   "Tools: Frame Metrics",           "F9",             "Toggle Frame Metrics window");
    reg("toggle_inspector", "Tools: Widget Inspector",        "F10",            "Toggle Widget Inspector window");
    reg("send_toast",       "Notify: Test Toast",             "Ctrl+T",         "Emit a status toast notification");
}

void showcase_app::init(context& ui, const showcase_config& cfg)
{
    env_.font_mono     = cfg.font_mono;
    env_.font_heading  = cfg.font_heading;
    env_.font_icons    = cfg.font_icons;
    env_.backend_name  = cfg.backend_name;
    env_.is_web        = cfg.is_web;
    env_.deterministic = cfg.deterministic;
    env_.log           = &log_;
    env_.binds         = &binds_;
    env_.switch_workspace = [this, &ui](std::string_view ws) { set_scene(ui, ws); };

    register_keybinds();

    log_.addf(log_level::info, "Strata Studio initialized ({})", cfg.backend_name);
    log_.add(log_level::info, "Loaded 12 modular showcase panels (zero-heap steady state)");
    log_.add(log_level::debug, "Press Ctrl+Shift+P to open the Command Palette");

    apply_theme(ui, cfg.initial_theme);
    set_scene(ui, cfg.initial_scene.empty() ? "studio" : cfg.initial_scene);
}

void showcase_app::apply_theme(context& ui, int index)
{
    const auto names = themes::names();
    if (names.empty()) { return; }
    theme_idx_ = std::clamp(index, 0, static_cast<int>(names.size()) - 1);
    (void)themes::by_name(names[static_cast<std::size_t>(theme_idx_)], ui.theme());
    // Snappy hover/focus transitions so rapid pointer movement across dense trees never leaves lingering trails.
    ui.theme().anim_speed = 24.0f;
}

demo_module* showcase_app::find_module(std::string_view id) const noexcept
{
    for (const auto& slot : modules_) {
        if (slot.info.id == id) { return slot.mod.get(); }
    }
    return nullptr;
}

void showcase_app::set_scene(context& ui, std::string_view name)
{
    if (name.empty() || name == "default") { name = "studio"; }

    focused_module_.clear();
    gallery_mode_      = false;
    open_palette_next_ = false;
    env_.draw_backdrop = false;

    for (auto& slot : modules_) { slot.visible = false; }

    auto show = [&](std::string_view id) {
        for (auto& slot : modules_) {
            if (slot.info.id == id) { slot.visible = true; }
        }
    };

    if (name == "palette") {
        active_workspace_  = "studio";
        open_palette_next_ = true;
        show("explorer");
        show("charts");
        show("nodes");
        show("editor");
        show("inspector");
        show("sequencer");
        show("console");
    } else if (name == "studio" || name == "features") {
        active_workspace_ = "studio";
        show("explorer");
        show("charts");
        show("nodes");
        show("editor");
        show("inspector");
        show("sequencer");
        show("console");
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
        active_workspace_ = "studio";
        show("explorer");
        show("charts");
        show("nodes");
        show("editor");
        show("inspector");
        show("sequencer");
        show("console");
    }

    layout_dirty_ = true;
    for (const auto& slot : modules_) {
        ui.undock_window(slot.info.title);
    }
}

void showcase_app::run_action(context& ui, std::string_view action)
{
    if (action == "ws_studio")            { set_scene(ui, "studio"); }
    else if (action == "ws_telemetry")    { set_scene(ui, "telemetry"); }
    else if (action == "ws_authoring")    { set_scene(ui, "authoring"); }
    else if (action == "ws_visuals")      { set_scene(ui, "visuals"); }
    else if (action == "ws_productivity") { set_scene(ui, "productivity"); }
    else if (action == "ws_gallery")      { set_scene(ui, "gallery"); }
    else if (action == "toggle_animate")  { env_.animate = !env_.animate; }
    else if (action == "toggle_metrics")  { show_metrics_ = !show_metrics_; }
    else if (action == "toggle_inspector"){ show_inspector_ = !show_inspector_; }
    else if (action == "send_toast") {
        ui.toast("Build Succeeded", "All 12 showcase modules compiled cleanly.", toast_kind::success);
        log_.add(log_level::info, "Toast dispatched via action");
    } else if (action.starts_with("theme_")) {
        const std::string_view tname = action.substr(6);
        const auto names = themes::names();
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == tname) {
                apply_theme(ui, static_cast<int>(i));
                log_.addf(log_level::info, "Applied theme '{}'", tname);
                break;
            }
        }
    }
}

void showcase_app::draw_menu_bar(context& ui, f32& out_bar_h)
{
    out_bar_h = 0.0f;
    if (auto bar = ui.main_menu_bar()) {
        if (auto m = ui.menu("&Workspace")) {
            for (std::size_t i = 0; i < k_workspaces.size(); ++i) {
                const bool active = (active_workspace_ == k_workspaces[i]);
                const std::string shortcut = std::format("Ctrl+{}", i + 1);
                if (ui.menu_item(k_workspace_labels[i], shortcut, active)) {
                    set_scene(ui, k_workspaces[i]);
                }
            }
            ui.menu_separator();
            if (ui.menu_item("Command Palette...", "Ctrl+Shift+P")) {
                palette_.open();
            }
        }

        if (auto m = ui.menu("&Modules")) {
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
            if (ui.menu_item("Reset Layout")) {
                set_scene(ui, active_workspace_);
            }
        }

        // Inline workspace quick-switcher pills with a crisp divider separating them from the menus
        ui.same_line();
        {
            const auto sep = ui.custom_item("##ws_div", {22.0f, 18.0f});
            const f32  mx  = sep.bounds.center().x;
            ui.draw().line({mx, sep.bounds.min.y + 2.0f}, {mx, sep.bounds.max.y - 2.0f}, ui.theme().border, 1.0f);
        }
        for (std::size_t i = 0; i < k_workspaces.size(); ++i) {
            ui.same_line();
            const bool active = (active_workspace_ == k_workspaces[i]);
            if (active) {
                const auto accent_pill = ui.style_overrides({
                    override_color(style_color::widget_bg, ui.theme().accent),
                    override_color(style_color::widget_hover, ui.theme().accent_hover),
                    override_color(style_color::text, color{255, 255, 255, 255}),
                });
                (void)ui.button(k_workspace_labels[i]);
            } else {
                if (ui.button(k_workspace_labels[i])) {
                    set_scene(ui, k_workspaces[i]);
                }
            }
        }

        // Right-aligned live status pills with comfortable gap between backend badge and metrics
        ui.same_line_right(335.0f);
        ui.badge(env_.backend_name, toast_kind::info);
        ui.same_line();
        (void)ui.custom_item("##stat_gap", {10.0f, 1.0f});
        ui.same_line();
        ui.textf("{:.0f} fps  |  {} draws  |  {:.2f} ms", env_.fps, ui.stats().draw_calls, env_.ui_ms);

        out_bar_h = ui.main_menu_bar_height();
    }
}

void showcase_app::apply_workspace_layout(context& ui)
{
    if (active_workspace_ == "studio") {
        ui.dock_window("Scene Explorer",     dock_zone::center, {},            0.5f, "left");
        ui.dock_window("Code Editor",        dock_zone::center, {},            0.5f, {});
        ui.dock_window("Node Graph",         dock_zone::center, "Code Editor", 0.5f, {});
        ui.dock_window("Telemetry",          dock_zone::center, "Code Editor", 0.5f, {});
        ui.dock_window("Inspector",          dock_zone::center, {},            0.5f, "right");
        ui.dock_window("Console",            dock_zone::center, {},            0.5f, "bottom");
        ui.dock_window("Sequencer & Curves", dock_zone::center, "Console",     0.5f, "bottom");
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
    const f32 sidebar_w = std::clamp(client.width() * 0.21f, 210.0f, 280.0f);
    const rect left_r{client.min, {client.min.x + sidebar_w, client.max.y}};
    const rect main_r{{client.min.x + sidebar_w + 1.0f, client.min.y}, client.max};

    constexpr auto fixed_flags = window_flags::no_title_bar | window_flags::no_move;

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
                ui.badge(category_label(slot.info.category), toast_kind::info);
                ui.text_dim(slot.info.summary);
                ui.separator();
                slot.mod->draw(ui, env_);
                break;
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

    // Update all modules so telemetry/animations stay smooth even across tab switches.
    for (auto& slot : modules_) {
        slot.mod->update(env_, dt);
    }

    // Check global shortcuts when not typing in a text field.
    if (!ui.want_text_input()) {
        for (const auto& a : binds_.actions()) {
            if (binds_.pressed(ui, a.name)) {
                run_action(ui, a.name);
                break;
            }
        }
    }

    // Draw full-viewport backdrop behind glass/acrylic workspaces before any windows.
    if (env_.draw_backdrop) {
        for (auto& slot : modules_) {
            slot.mod->draw_backdrop(ui, env_);
        }
    }

    f32 bar_h = 0.0f;
    draw_menu_bar(ui, bar_h);

    const vec2 ds = ui.display_size();
    rect client{{0.0f, bar_h}, ds};

    if (gallery_mode_) {
        draw_gallery(ui, client);
    } else if (active_workspace_ == "visuals") {
        // Floating frosted-glass panels framed cleanly over the procedural art backdrop.
        const f32 pad   = 28.0f;
        const f32 gap   = 24.0f;
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
        // Edge-to-edge studio dockspace tailored to the active workspace.
        if (active_workspace_ == "studio") {
            client = ui.dock_edge("left",   dock_side::left,   std::clamp(ds.x * 0.19f, 220.0f, 290.0f), client);
            client = ui.dock_edge("right",  dock_side::right,  std::clamp(ds.x * 0.24f, 280.0f, 360.0f), client);
            client = ui.dock_edge("bottom", dock_side::bottom, std::clamp(ds.y * 0.28f, 200.0f, 260.0f), client);
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
        return color{10, 12, 20, 255};
    }
    // Derive a subtle backdrop slightly darker than the active theme's window background.
    const color bg = ui.theme().window_bg;
    return color{
        static_cast<u8>(bg.r > 8 ? bg.r - 8 : 0),
        static_cast<u8>(bg.g > 8 ? bg.g - 8 : 0),
        static_cast<u8>(bg.b > 8 ? bg.b - 8 : 0),
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

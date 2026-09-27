#pragma once

// newer sandbox demos: visuals (gradients, curves, acrylic), drag / number inputs, plots, text selection, modals,
// menus, toasts, log view, key bindings and config files. state lives in demo2_state.

#include "demo3.hpp"
#include "demo4.hpp"

#include <strata/strata.hpp>

#include <string>
#include <vector>

struct demo2_state {
    demo2_state(); // registers the key bindings

    // set by main: font ids and texture, -1 / 0 when not available
    int                font_mono    = -1;
    int                font_heading = -1;
    int                font_icons   = -1;
    strata::texture_id image_tex    = 0;
    float              time         = 0.0f; // seconds of demo time (advances with the frame delta)
    bool               deterministic = false; // screenshot mode: nothing may depend on the wall clock
    int                frame        = 0;      // frames built so far (scenes use it to trigger things)
    std::string        scene;

    // visuals
    bool  art            = false; // decorative backdrop behind the windows
    bool  show_visuals   = false;
    float gauge          = 0.62f;

    // drag / number inputs and plots
    bool  show_inputs    = false;
    float d_float        = 1.5f;
    int   d_int          = 42;
    float d_pos[3]       = {0.25f, 0.5f, 0.75f};
    float d_size[2]      = {1280.0f, 720.0f};
    float in_float       = 3.14159f;
    int   in_int         = 7;
    bool  layers[6]      = {true, true, false, true, false, false};
    bool  channels[3]    = {true, false, true};
    std::string styled_text = "# Styled contents\nThe field holds *plain text*; ranges of it get their own font, color, _italic_ and underline: `code` is set in the mono font. Edit it: the spans are made again from the text on every change.\nTriple-click selects a line.";
    std::vector<float> history;      // a rolling signal
    unsigned           history_head  = 0;
    std::vector<float> bars;
    bool  plot_paused    = false;

    // textures: mip maps, pixel formats, updates (the ids are made by demo2_textures_create)
    bool  show_textures  = false;
    strata::texture_id tex_checker = 0, tex_checker_mips = 0, tex_mask = 0, tex_grey = 0, tex_rgba = 0, tex_bgra = 0,
                       tex_half = 0, tex_dynamic = 0;

    // right-to-left scripts, joined arabic letters, emoji
    bool  show_scripts   = false;
    std::string rtl_line = "\u05e9\u05dc\u05d5\u05dd \u05e2\u05d5\u05dc\u05dd  -  \u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645";
    std::string mixed_text = "Hello \u05e9\u05dc\u05d5\u05dd 123 (\u05e2\u05d5\u05dc\u05dd)\n\u05e9\u05dc\u05d5\u05dd Hello 2024 world!\n\U0001F680 lift-off \u2764 \u2605";

    // charts: axes, ticks, area fills, zoom / pan
    bool  show_charts    = false;

    // text selection, the log, toasts
    bool  show_textlog   = false;
    strata::log_buffer log{3000};
    bool  log_seeded     = false;
    int   log_counter    = 0;
    bool  auto_log       = false;
    strata::toast_handle t_actions  = 0;   // a toast with buttons, and a progress toast that is fed every frame
    strata::toast_handle t_progress = 0;
    float                t_progress_v = 0.0f;

    // menus and modals
    bool  show_menus     = false;    // the main menu bar and its window
    bool  autosave       = true;
    bool  show_grid      = true;
    bool  show_stats     = false;
    std::string last_action = "-";
    strata::vec2 context_target{-1.0e6f, -1.0e6f}; // where the right-click area is (for scripted input)
    std::string dialog_status = "-";
    std::string name_edit = "untitled";
    float modal_volume   = 0.5f;
    bool  modal_flag     = true;

    // key bindings, settings and theme saved to / loaded from one ini file
    bool               show_config   = false;
    strata::keybinds   binds;
    strata::command_palette palette;
    strata::config_file file;                    // in the temp folder; auto-save / hot reload toggled in the window
    std::string        user_name     = "player one";
    float              volume        = 0.6f;
    bool               muted         = false;
    bool               sidebar       = true;
    bool               ctx_editor    = false;    // keybind contexts
    bool               ctx_viewport  = false;
    strata::f64        last_time     = 0.0;
    std::string        config_text;              // what saving would write (a live preview)
    std::string        config_status = "-";
    std::string        last_bind_action = "-";
    int                config_request = 0;       // 1: save, 2: load (done at the start of the next frame, between widgets)

    demo3_state d3;
    demo4_state d4;

    // requests to the host (main applies them between frames)
    float pending_scale  = 0.0f;  // > 0: set_scale(pending_scale)
    int   scale_combo    = 0;
    bool  dock_anim      = true;
};

// draws the decorative backdrop (call before the windows; it lands behind all of them)
void demo2_art(strata::context& ui, demo2_state& s);
void demo2_visuals(strata::context& ui, demo2_state& s);
void demo2_inputs(strata::context& ui, demo2_state& s);
void demo2_charts(strata::context& ui, demo2_state& s);
void demo2_scripts(strata::context& ui, demo2_state& s);
void demo2_textures(strata::context& ui, demo2_state& s);
// makes the demo textures (and updates the animated one every frame); the host owns the renderer
class gfx_host;
void demo2_textures_create(gfx_host& host, demo2_state& s);
void demo2_textures_update(gfx_host& host, demo2_state& s);
void demo2_textlog(strata::context& ui, demo2_state& s);
// the main menu bar (if enabled) and the window with the modal / context-menu / toast buttons
void demo2_menus(strata::context& ui, demo2_state& s);
// the key binding editor, a few settings and the config file with a live preview of it
void demo2_config(strata::context& ui, demo2_state& s);
// the windows of demo3 (tabs and popups, drag and drop, lists and tables, code editor)
void demo2_more(strata::context& ui, demo2_state& s);
// per-frame housekeeping: advances the rolling data, feeds the log, runs the scene scripts
void demo2_update(strata::context& ui, demo2_state& s, float dt);
// mouse input a scene scripts for its screenshot (frame counts from 0)
struct demo2_sim {
    strata::vec2 pos{-1.0e6f, -1.0e6f};
    bool         down[3]{};
};
void demo2_script(const demo2_state& s, int frame, demo2_sim& sim);

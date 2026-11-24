// end-to-end interactive UI tests: real click, drag, wheel, modifier, keyboard, and rendering checks across
// charts (including sandbox demo charts & extreme/NaN ranges), core controls, windows, menus, tables, tabs, and popups.

#include "selftest_common.hpp"

#include <strata/backend/null.hpp>
#include <strata/backend/software.hpp>
#include <strata/svg.hpp>
#include <strata/themes.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

namespace {

void check_finite_and_valid(const draw_data& d, null_renderer& nr)
{
    nr.render(d);
    CHECK(nr.last().well_formed);
    bool finite = true;
    for (const vertex& v : d.vertices) {
        if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y)) { finite = false; break; }
    }
    CHECK(finite);
}

// clicks the exact sandbox "charts (axes, zoom, area)" flow, exercises every gesture, and tests extreme/NaN data
void test_interactive_charts_and_extremes()
{
    std::fprintf(stderr, "[interactive: charts click-open, zoom/pan/hover, extreme & NaN ranges]\n");
    harness h;
    h.in.display_size = {1280.0f, 800.0f};
    null_renderer nr;
    CHECK(nr.create(h.ui.font()));

    std::array<f32, 240> cpu{};
    std::array<f32, 240> gpu{};
    std::array<f32, 32>  frames{};
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const f32 t = static_cast<f32>(i) * 0.5f;
        cpu[i] = 42.0f + 24.0f * std::sin(t * 0.21f) + 9.0f * std::sin(t * 1.3f);
        gpu[i] = 58.0f + 20.0f * std::sin(t * 0.13f + 1.0f) + 6.0f * std::sin(t * 2.1f);
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        frames[i] = 6.0f + 9.0f * std::abs(std::sin(static_cast<f32>(i) * 0.47f)) + static_cast<f32>(i % 5);
    }

    bool show_charts = false;
    rect toggle_r{}, load_r{}, ft_r{}, area_r{};
    vec2 load_range{};
    bool load_zoomed = false;

    const auto build = [&] {
        if (auto w = h.ui.window("launcher", {20, 20}, {260, 0}, window_flags::resizable)) {
            h.ui.checkbox("charts (axes, zoom, area)", show_charts);
            toggle_r = h.ui.item_rect();
        }
        if (show_charts) {
            if (auto w = h.ui.window("charts", {300, 40}, {620, 0}, window_flags::resizable)) {
                plot_options load;
                load.size     = {0.0f, 220.0f};
                load.x        = {"time", "s"};
                load.y        = {"load", "%", 0.0f, 100.0f};
                load.x_step   = 0.5f;
                load.fill     = true;
                load.zoom_pan = true;
                const std::array<plot_series, 2> series = {{{"cpu", cpu, color{0, 0, 0, 0}}, {"gpu", gpu, color{0, 0, 0, 0}}}};
                h.ui.plot("load over time", series, load);
                load_r      = h.ui.item_rect();
                load_range  = h.ui.plot_x_range("load over time");
                load_zoomed = h.ui.plot_zoomed("load over time");

                plot_options ft;
                ft.kind     = plot_kind::histogram;
                ft.size     = {0.0f, 150.0f};
                ft.x        = {"frame", ""};
                ft.y        = {"frame time", "ms"}; // plot_auto (NaN) y range
                ft.zoom_pan = true;
                const std::array<plot_series, 1> bars = {{{"ms", frames, color{0, 0, 0, 0}}}};
                h.ui.plot("frame times", bars, ft);
                ft_r = h.ui.item_rect();

                plot_options area;
                area.size       = {0.0f, 120.0f};
                area.x          = {"", "s"};
                area.y          = {"gpu", "%"}; // plot_auto (NaN) y range
                area.x_step     = 0.5f;
                area.fill       = true;
                area.fill_alpha = 0.3f;
                const std::array<plot_series, 1> one = {{{"gpu", gpu, color::from_hex(0xf0568fffu)}}};
                h.ui.plot("gpu area", one, area);
                area_r = h.ui.item_rect();
            }
        }
    };

    h.frames(build, 2);
    CHECK(!show_charts);

    // click the checkbox to open charts (previously froze under clang-cl /fp:fast)
    h.click(toggle_r.center(), build);
    h.frames(build, 3);
    CHECK(show_charts);
    CHECK(load_r.height() > 150.0f && ft_r.height() > 100.0f && area_r.height() > 80.0f);
    CHECK(near_eq(load_range.x, 0.0f, 0.1f) && near_eq(load_range.y, 119.5f, 0.1f));
    check_finite_and_valid(h.ui.render_data(), nr);

    // hover sweep across all three charts (triggers crosshair + tooltips)
    for (const rect& r : {load_r, ft_r, area_r}) {
        for (int k = 1; k <= 4; ++k) {
            const f32 frac = static_cast<f32>(k) / 5.0f;
            h.move({r.min.x + r.width() * frac, r.center().y});
            h.frames(build, 2);
        }
    }
    check_finite_and_valid(h.ui.render_data(), nr);

    // wheel zoom in, Ctrl+wheel Y zoom, drag pan, double-click reset
    const vec2 load_mid = load_r.center();
    h.scroll(load_mid, 5.0f, build);
    CHECK(load_zoomed);
    const f32 span_after_zoom = load_range.y - load_range.x;
    CHECK(span_after_zoom < 100.0f);

    h.move(load_mid);
    h.frames(build, 2);
    h.modifiers(true, false, false);
    h.wheel(4.0f);
    h.frame(build);
    h.modifiers(false, false, false);
    h.frames(build, 2);
    CHECK(near_eq(load_range.y - load_range.x, span_after_zoom, 0.2f));

    const f32 x0_before_pan = load_range.x;
    h.move(load_mid);
    h.frames(build, 2);
    h.down();
    h.frame(build);
    h.move({load_mid.x - 80.0f, load_mid.y + 30.0f});
    h.frames(build, 3);
    h.up();
    h.frames(build, 2);
    CHECK(load_range.x > x0_before_pan + 1.0f);

    // double-click resets view (within 0.35 s: advance 1 frame between clicks)
    h.move(load_mid);
    h.frame(build);
    h.down(); h.frame(build); h.up(); h.frame(build);
    h.down(); h.frame(build); h.up(); h.frame(build);
    h.frames(build, 2);
    CHECK(!load_zoomed);

    // extreme & pathological data: NaNs, Infs, huge offsets, tiny spans, extreme zoom must never hang or emit NaN vertices
    const f32 qnan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf  = std::numeric_limits<f32>::infinity();
    const std::array<f32, 8> bad_vals{qnan, inf, -inf, qnan, 1.0f, -1.0f, qnan, inf};
    const std::array<f32, 8> all_nan{qnan, qnan, qnan, qnan, qnan, qnan, qnan, qnan};
    const std::array<f32, 8> huge_neg{-1.0e7f, -1.0e7f + 0.5f, -1.0e7f + 1.0f, -1.0e7f + 0.2f,
                                      -1.0e7f + 0.8f, -1.0e7f + 0.3f, -1.0e7f + 0.9f, -1.0e7f + 0.1f};
    const std::array<plot_series, 1> s_bad{{{"bad", bad_vals, color{0, 0, 0, 0}}}};
    const std::array<plot_series, 1> s_nan{{{"nan", all_nan, color{0, 0, 0, 0}}}};
    const std::array<plot_series, 1> s_huge{{{"huge", huge_neg, color{0, 0, 0, 0}}}};

    plot_options ext_opt;
    ext_opt.size     = {0.0f, 120.0f};
    ext_opt.zoom_pan = true;
    ext_opt.ticks    = true;
    rect huge_r{};
    const auto build_ext = [&] {
        if (auto w = h.ui.window("extremes", {20, 20}, {500, 0}, plain_window)) {
            h.ui.plot("bad", s_bad, ext_opt);
            h.ui.plot("all_nan", s_nan, ext_opt);
            plot_options ho = ext_opt;
            ho.x_start = -1.0e7f;
            ho.x_step  = 0.25f;
            h.ui.plot("huge_neg", s_huge, ho);
            huge_r = h.ui.item_rect();
            h.ui.plot_lines("nan_lines", all_nan, {0.0f, 50.0f});
        }
    };
    h.frames(build_ext, 3);
    h.scroll(huge_r.center(), 100.0f, build_ext);
    h.scroll(huge_r.center(), -100.0f, build_ext);
    check_finite_and_valid(h.ui.render_data(), nr);
    CHECK(h.ui.stats().limits_hit == 0);
}

// exercises clicking, dragging, scrolling, and typing across core input & picker widgets
void test_interactive_controls_click_drag()
{
    std::fprintf(stderr, "[interactive: buttons, toggles, sliders, drags, combos, color, text]\n");
    harness h;
    h.in.display_size = {1280.0f, 900.0f};
    null_renderer nr;
    CHECK(nr.create(h.ui.font()));

    int         btn_clicks = 0;
    bool        flag_cb    = false;
    bool        flag_tog   = false;
    f32         slider_v   = 0.25f;
    f32         drag_v     = 10.0f;
    int         num_i      = 5;
    int         combo_idx  = 0;
    std::string text_buf   = "hi";
    color       col{80, 140, 220, 255};
    rect        r_btn{}, r_cb{}, r_tog{}, r_slider{}, r_drag{}, r_num{}, r_combo{}, r_text{}, r_color{};

    const auto build = [&] {
        if (auto w = h.ui.window("controls", {20, 20}, {420, 0}, window_flags::resizable)) {
            if (h.ui.button("action")) { ++btn_clicks; }
            r_btn = h.ui.item_rect();
            h.ui.checkbox("check", flag_cb);
            r_cb = h.ui.item_rect();
            h.ui.toggle("toggle", flag_tog);
            r_tog = h.ui.item_rect();
            h.ui.slider("gain", slider_v, 0.0f, 1.0f);
            r_slider = h.ui.item_rect();
            h.ui.drag_float("offset", drag_v, 0.5f, -100.0f, 100.0f);
            r_drag = h.ui.item_rect();
            h.ui.input_int("count", num_i, 1);
            r_num = h.ui.item_rect();
            h.ui.combo("quality", combo_idx, {"low", "medium", "high"});
            r_combo = h.ui.item_rect();
            h.ui.input_text("name", text_buf, "hint");
            r_text = h.ui.item_rect();
            h.ui.color_edit("tint", col);
            r_color = h.ui.item_rect();
        }
    };

    h.frames(build, 3);
    // button, checkbox, toggle clicks
    h.click(r_btn.center(), build);
    CHECK(btn_clicks == 1);
    h.click(r_cb.center(), build);
    CHECK(flag_cb);
    h.click(r_tog.center(), build);
    CHECK(flag_tog);

    // slider click + drag right
    const vec2 sl_start{r_slider.min.x + r_slider.width() * 0.25f, r_slider.center().y};
    const vec2 sl_end{r_slider.min.x + r_slider.width() * 0.85f, r_slider.center().y};
    h.move(sl_start); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move(sl_end); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(slider_v > 0.7f);

    // drag_float sideways drag
    const f32 drag_before = drag_v;
    h.move(r_drag.center()); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({r_drag.center().x + 60.0f, r_drag.center().y}); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(drag_v > drag_before + 5.0f);

    // input_int + button click (right side of control)
    const int num_before = num_i;
    h.click({r_num.max.x - 10.0f, r_num.center().y}, build);
    CHECK(num_i == num_before + 1);

    // combo open + select second item (capture box before popup opens)
    const rect combo_box = r_combo;
    h.click(combo_box.center(), build);
    h.frames(build, 2);
    CHECK(h.ui.any_popup_open());
    h.click({combo_box.center().x, combo_box.max.y + 42.0f}, build);
    h.frames(build, 2);
    CHECK(combo_idx != 0);

    // text input click, type, undo
    h.click(r_text.center(), build);
    h.type("!");
    h.frames(build, 2);
    CHECK(text_buf == "hi!");
    h.press(strata::key::z, true);
    h.frames(build, 2);
    CHECK(text_buf == "hi");

    // color_edit popup open + Esc close
    h.click(r_color.center(), build);
    h.frames(build, 2);
    CHECK(h.ui.any_popup_open());
    h.press(strata::key::escape);
    h.frames(build, 2);
    CHECK(!h.ui.any_popup_open());

    check_finite_and_valid(h.ui.render_data(), nr);
}

// exercises window move/resize, menus, context menu, modals, tabs, trees, tables, drag-drop, and software rasterisation
void test_interactive_workspace_session()
{
    std::fprintf(stderr, "[interactive: windows, menus, modals, tabs, tables, drag-drop, software render]\n");
    harness h;
    h.in.display_size = {1280.0f, 800.0f};
    null_renderer nr;
    software_renderer sr;
    CHECK(nr.create(h.ui.font()));
    CHECK(sr.create(h.ui.font()));
    sr.resize(1280, 800);

    std::vector<std::string> tabs{"Main", "Logs", "Notes"};
    int  active_tab = 0;
    int  menu_hits  = 0;
    int  ctx_hits   = 0;
    int  modal_res  = 0;
    int  sort_col   = 0;
    bool sort_asc   = true;
    int  moved_from = -1, moved_to = -1;
    std::array<std::string, 3> items{"Alpha", "Beta", "Gamma"};
    selection_state sel;
    rect r_win{}, r_tab_bar{}, r_ctx_zone{}, r_modal_btn{}, r_drag_src{}, r_drag_dst{}, r_row0{}, r_row2{}, r_hdr0{};

    const auto build = [&] {
        if (auto bar = h.ui.main_menu_bar()) {
            if (auto m = h.ui.menu("&File")) {
                if (h.ui.menu_item("&Save", "Ctrl+S")) { ++menu_hits; }
            }
        }
        if (h.ui.accelerator("Ctrl+S")) { ++menu_hits; }

        if (auto w = h.ui.window("workspace", {40, 50}, {680, 560}, window_flags::resizable)) {
            r_win = h.ui.window_rect("workspace");

            std::vector<tab_desc> descs;
            for (const auto& t : tabs) { descs.emplace_back(std::string_view{t}); }
            const auto ev = h.ui.tab_bar("docs", descs.data(), descs.size(), active_tab,
                                         tab_bar_flags::closable | tab_bar_flags::add_button);
            r_tab_bar = h.ui.item_rect();
            if (ev.add) { tabs.push_back("New"); active_tab = static_cast<int>(tabs.size()) - 1; }
            apply_tab_events(ev, tabs, active_tab);

            if (h.ui.button("open dialog")) { h.ui.open_modal("Confirm?"); }
            r_modal_btn = h.ui.item_rect();

            const auto zone = h.ui.custom_item("ctx_zone", {200.0f, 32.0f});
            r_ctx_zone = zone.bounds;
            if (auto cm = h.ui.context_menu("ctx_menu", r_ctx_zone)) {
                if (h.ui.menu_item("Inspect")) { ++ctx_hits; }
            }

            // sortable table with multi-selection
            if (h.ui.begin_table("items", 2, table_default, 140.0f)) {
                h.ui.table_setup_column("Name");
                h.ui.table_setup_column("Index", 80.0f);
                const int clicked_col = h.ui.table_headers_row(sort_col, sort_asc);
                if (clicked_col >= 0) {
                    if (clicked_col == sort_col) { sort_asc = !sort_asc; }
                    else { sort_col = clicked_col; sort_asc = true; }
                }
                r_hdr0 = h.ui.item_rect();
                for (int i = 0; i < 3; ++i) {
                    if (!h.ui.table_next_row()) { continue; }
                    h.ui.table_next_column();
                    if (h.ui.selectable(items[i], sel.contains(i))) { h.ui.selection_click(sel, i); }
                    if (i == 0) { r_row0 = h.ui.item_rect(); }
                    if (i == 2) { r_row2 = h.ui.item_rect(); }
                    h.ui.table_next_column();
                    h.ui.textf("#{}", i);
                }
                h.ui.end_table();
            }

            // drag-and-drop pair
            (void)h.ui.selectable("Drag Source");
            r_drag_src = h.ui.item_rect();
            if (auto d = h.ui.drag_source("card", 0)) { h.ui.text("card 0"); }
            (void)h.ui.selectable("Drop Target");
            r_drag_dst = h.ui.item_rect();
            if (const drop_result drop = h.ui.drop_target("card")) {
                moved_from = drop.as<int>();
                moved_to   = 1;
            }
        }
        if (const int r = h.ui.dialog("Confirm?", "Proceed with action?", {"Yes", "No"}); r != 0) {
            modal_res = r;
        }
    };

    h.frames(build, 3);
    const vec2 start_pos = r_win.min;

    // 1. keyboard accelerator works with menu closed
    h.press(strata::key::s, true);
    h.frames(build, 2);
    CHECK(menu_hits == 1);

    // 2. drag window by its title bar
    const vec2 title_pt{r_win.min.x + 120.0f, r_win.min.y + 14.0f};
    h.move(title_pt); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({title_pt.x + 50.0f, title_pt.y + 25.0f}); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(r_win.min.x > start_pos.x + 30.0f && r_win.min.y > start_pos.y + 15.0f);

    // 3. click add-tab button at right end of tab bar
    const std::size_t tabs_before = tabs.size();
    h.click({r_tab_bar.max.x - 10.0f, r_tab_bar.center().y}, build);
    CHECK(tabs.size() == tabs_before + 1);

    // 4. click row 0, Shift+click row 2 -> selects all 3 rows
    h.click(r_row0.center(), build);
    CHECK(sel.size() == 1);
    h.modifiers(false, true, false);
    h.click(r_row2.center(), build);
    h.modifiers(false, false, false);
    CHECK(sel.size() == 3);

    // 5. right-click context menu and pick item
    h.right_click(r_ctx_zone.center(), build);
    h.frames(build, 2);
    CHECK(h.ui.menu_open());
    h.click({r_ctx_zone.center().x + 20.0f, r_ctx_zone.center().y + 16.0f}, build);
    h.frames(build, 2);
    CHECK(ctx_hits == 1);

    // 6. drag and drop from source to target
    h.move(r_drag_src.center()); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({r_drag_src.center().x + 12.0f, r_drag_src.center().y + 4.0f}); h.frames(build, 2);
    h.move(r_drag_dst.center()); h.frames(build, 2);
    h.up(); h.frames(build, 2);
    CHECK(moved_from == 0 && moved_to == 1);

    // 7. open modal dialog and dismiss with Esc
    h.click(r_modal_btn.center(), build);
    h.frames(build, 2);
    CHECK(h.ui.modal_open());
    h.press(strata::key::escape);
    h.frames(build, 2);
    CHECK(modal_res == -1 && !h.ui.modal_open());

    // validate draw data and rasterise on CPU software renderer
    const draw_data dd = h.ui.render_data();
    check_finite_and_valid(dd, nr);
    sr.clear({18, 18, 22, 255});
    sr.render(dd);
    std::size_t lit = 0;
    const auto px = sr.pixels();
    for (std::size_t i = 0; i < px.size(); i += 4) {
        lit += (px[i] != 18 || px[i + 1] != 18 || px[i + 2] != 22) ? 1u : 0u;
    }
    CHECK(lit > 50000u);
    CHECK(h.ui.stats().limits_hit == 0);
}

// exercises new widgets (range_slider, knob), chart clickable legend + ref lines, code editor Ctrl+/, and Tab keyboard nav
void test_interactive_new_features()
{
    std::fprintf(stderr, "[interactive: range_slider, knob, chart legend/ref lines, Ctrl+/ comment, Tab focus nav]\n");
    harness h;
    h.in.display_size = {1024.0f, 768.0f};
    null_renderer nr;
    CHECK(nr.create(h.ui.font()));

    float r_lo = 20.0f, r_hi = 80.0f;
    float knob_v = 0.5f;
    int   btn_clicks = 0;
    bool  chk_val = false;
    std::string code = "int a = 1;\nint b = 2;\n";
    rect rc_range{}, rc_knob{}, rc_code{}, rc_chart{};

    static constexpr float s0[] = {10.0f, 20.0f, 15.0f, 30.0f};
    static constexpr float s1[] = {5.0f, 8.0f, 12.0f, 9.0f};
    const plot_series series[2] = {
        {.name = "Alpha", .values = s0, .col = {80, 180, 255, 255}},
        {.name = "Beta",  .values = s1, .col = {255, 160, 80, 255}},
    };
    const plot_ref_line refs[2] = {
        {.value = 18.0f, .vertical = false, .col = {255, 80, 80, 220},  .label = "limit"},
        {.value = 2.0f,  .vertical = true,  .col = {120, 220, 120, 220}, .label = "t=2"},
    };
    plot_options popt;
    popt.size               = {460.0f, 150.0f};
    popt.interactive_legend = true;
    popt.ref_lines          = refs;

    const auto build = [&] {
        if (auto w = h.ui.window("new_features", {20, 20}, {520, 680})) {
            if (h.ui.button("nav_btn")) { ++btn_clicks; }
            h.ui.checkbox("nav_chk", chk_val);
            h.ui.range_slider("bounds", r_lo, r_hi, 0.0f, 100.0f);
            rc_range = h.ui.item_rect();
            h.ui.knob("gain", knob_v, 0.0f, 1.0f, 0.5f);
            rc_knob = h.ui.item_rect();
            h.ui.input_code("src", code, {460.0f, 110.0f});
            rc_code = h.ui.item_rect();
            h.ui.plot("metrics", series, popt);
            rc_chart = h.ui.item_rect();
        }
    };

    h.frames(build, 3);
    CHECK(rc_range.width() > 60.0f && rc_knob.width() > 20.0f && rc_chart.height() > 100.0f);

    // 1. range_slider: drag left handle right, then drag right handle left
    const float track_w = rc_range.width();
    const vec2 lo_pt{rc_range.min.x + track_w * 0.20f, rc_range.center().y};
    h.move(lo_pt); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({rc_range.min.x + track_w * 0.40f, rc_range.center().y}); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(r_lo > 30.0f && r_lo <= r_hi);

    const vec2 hi_pt{rc_range.min.x + track_w * 0.80f, rc_range.center().y};
    h.move(hi_pt); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({rc_range.min.x + track_w * 0.60f, rc_range.center().y}); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(r_hi < 70.0f && r_hi >= r_lo);

    // 2. knob: drag up increases value; Shift fine drag changes less; double-click resets to 0.5
    const vec2 kpt = rc_knob.center();
    h.move(kpt); h.frames(build, 2);
    h.down(); h.frame(build);
    h.move({kpt.x, kpt.y - 60.0f}); h.frames(build, 3);
    h.up(); h.frames(build, 2);
    CHECK(knob_v > 0.7f);
    const float before_fine = knob_v;
    h.modifiers(false, true, false);
    h.move(kpt); h.frames(build, 1);
    h.down(); h.frame(build);
    h.move({kpt.x, kpt.y + 40.0f}); h.frames(build, 2);
    h.up(); h.frames(build, 1);
    h.modifiers(false, false, false);
    CHECK(knob_v < before_fine && (before_fine - knob_v) < 0.1f);
    // double-click reset to default (0.5)
    h.click(kpt, build);
    h.click(kpt, build);
    CHECK(std::fabs(knob_v - 0.5f) < 0.01f);

    // 3. code editor Ctrl+/ comment toggle + Ctrl+Z undo
    h.click({rc_code.min.x + 80.0f, rc_code.min.y + 12.0f}, build);
    h.frames(build, 2);
    h.press(strata::key::slash, true);
    h.frames(build, 2);
    CHECK(code.starts_with("// int a = 1;"));
    h.press(strata::key::slash, true);
    h.frames(build, 2);
    CHECK(code.starts_with("int a = 1;"));
    // select all and comment both lines
    h.press(strata::key::a, true);
    h.frames(build, 1);
    h.press(strata::key::slash, true);
    h.frames(build, 2);
    CHECK(code == "// int a = 1;\n// int b = 2;\n");
    h.press(strata::key::z, true);
    h.frames(build, 2);
    CHECK(code == "int a = 1;\nint b = 2;\n");
    // click away so code editor releases keyboard focus, then click window background to refocus window
    h.click({800.0f, 600.0f}, build);
    h.click({rc_chart.min.x + 8.0f, rc_chart.max.y - 8.0f}, build);
    h.frames(build, 2);

    // 4. chart interactive legend toggle (legend sits at top-right of chart header)
    const std::size_t verts_before = h.ui.render_data().vertices.size();
    h.click({rc_chart.max.x - 20.0f, rc_chart.min.y + 10.0f}, build);
    h.frames(build, 2);
    const std::size_t verts_after = h.ui.render_data().vertices.size();
    CHECK(verts_after != verts_before); // toggling series visibility changes emitted geometry
    // restore series
    h.click({rc_chart.max.x - 20.0f, rc_chart.min.y + 10.0f}, build);
    h.frames(build, 2);

    // 5. Tab / Shift+Tab keyboard navigation + Space activation + Esc clear
    h.ui.set_keyboard_nav(true);
    CHECK(h.ui.keyboard_nav());
    h.press(strata::key::tab);
    h.frames(build, 2);
    const id first_focus = h.ui.keyboard_focus_id();
    CHECK(first_focus != 0);
    h.press(strata::key::space);
    h.frames(build, 2);
    CHECK(btn_clicks == 1);

    h.press(strata::key::tab);
    h.frames(build, 2);
    const id second_focus = h.ui.keyboard_focus_id();
    CHECK(second_focus != 0 && second_focus != first_focus);
    h.press(strata::key::enter);
    h.frames(build, 2);
    CHECK(chk_val);

    h.press(strata::key::tab, false, true); // Shift+Tab returns to first control
    h.frames(build, 2);
    CHECK(h.ui.keyboard_focus_id() == first_focus);

    h.press(strata::key::escape);
    h.frames(build, 2);
    CHECK(h.ui.keyboard_focus_id() == 0);

    check_finite_and_valid(h.ui.render_data(), nr);
}

// exercises SDF fonts, SVG rasteriser, multi-viewport, timeline/curve/property grid, scatter/stacked/log/heatmap charts,
// flex wrap, theme hot-reload, and software renderer partial damage-rect redraw
void test_interactive_universal_suite()
{
    std::fprintf(stderr, "[interactive: SDF font, SVG, multi-viewport, timeline/curves/props/charts, flex, watcher, damage rect]\n");

    // 1. SDF font atlas + negative text_contrast flag + software render
    {
        context_config cfg;
        cfg.font.sdf        = true;
        cfg.font.sdf_spread = 4;
        auto created = context::create(cfg);
        CHECK(created.has_value());
        if (created) {
            context& ui = *created;
            CHECK(ui.font().is_sdf());
            CHECK(ui.font().sdf_spread() == 4);
            input_state in;
            in.display_size = {400.0f, 200.0f};
            ui.begin_frame(in);
            if (auto w = ui.window("sdf", {10, 10}, {240, 120})) { ui.text("Crisp SDF Text"); }
            ui.end_frame();
            const draw_data dd = ui.render_data();
            CHECK(dd.text_contrast < 0.0f);
            software_renderer sr;
            CHECK(sr.create(ui.font()));
            sr.resize(400, 200);
            sr.clear({10, 10, 14, 255});
            sr.render(dd);
            std::size_t lit = 0;
            for (std::size_t i = 0; i < sr.pixels().size(); i += 4) {
                lit += (sr.pixels()[i] != 10 || sr.pixels()[i + 1] != 10 || sr.pixels()[i + 2] != 14) ? 1u : 0u;
            }
            CHECK(lit > 3000u);
        }
    }

    // 2. SVG path parser & anti-aliased rasteriser
    {
        const auto contours = svg_parse_path(svg_icons::play);
        CHECK(contours.size() == 1 && contours[0].closed && contours[0].points.size() >= 3);
        const auto mask = svg_rasterize_alpha(svg_icons::play, svg_icons::viewbox, 32, 32);
        CHECK(mask.size() == 32u * 32u);
        std::size_t non_zero = 0;
        for (u8 a : mask) { non_zero += a > 0 ? 1u : 0u; }
        CHECK(non_zero > 100u && non_zero < 900u);
        const auto rgba = svg_rasterize_rgba(svg_icons::stop, svg_icons::viewbox, 24, 24, {200, 100, 50, 255});
        CHECK(rgba.size() == 24u * 24u * 4u);
        CHECK(rgba[(12 * 24 + 12) * 4 + 3] > 200); // center of stop square is solid
    }

    // 3. Multi-viewport: detached window splits out of primary render_data into viewports()
    {
        harness h;
        h.in.display_size = {800.0f, 600.0f};
        h.ui.set_multi_viewport(true);
        CHECK(h.ui.multi_viewport());
        const auto build_mv = [&] {
            if (auto w1 = h.ui.window("main_win", {40, 40}, {240, 160})) { h.ui.text("primary"); }
            if (auto w2 = h.ui.window("tearoff", {900, 80}, {260, 180}, window_flags::detached)) { h.ui.text("secondary"); }
        };
        h.frames(build_mv, 2);
        const auto vps = h.ui.viewports();
        CHECK(vps.size() == 1);
        if (!vps.empty()) {
            CHECK(vps[0].title == "tearoff");
            const draw_data vp_dd = h.ui.viewport_render_data(vps[0].window_id);
            CHECK(!vp_dd.commands.empty() && vp_dd.origin.x > 800.0f && vp_dd.display_size.x >= 250.0f);
            h.ui.set_viewport_bounds(vps[0].window_id, {950.0f, 120.0f}, {300.0f, 200.0f});
        }
        h.frames(build_mv, 1);
        CHECK(!h.ui.viewports().empty() && h.ui.viewports()[0].bounds.min.x >= 949.0f);
    }

    // 4. Timeline, Bezier curve editor, Property grid, Scatter/Stacked/Log/Heatmap charts, Flex wrap
    {
        harness h;
        h.in.display_size = {1200.0f, 900.0f};
        null_renderer nr;
        CHECK(nr.create(h.ui.font()));

        timeline_state tl;
        tl.start = 0.0f; tl.end = 100.0f; tl.view_start = 0.0f; tl.view_end = 100.0f; tl.time = 10.0f;
        std::array<timeline_clip, 1> clips{{{10.0f, 35.0f, "Intro", {80, 160, 240, 255}}}};
        std::array<timeline_keyframe, 2> keys{{{15.0f}, {60.0f}}};

        std::vector<curve_point> curve{
            {.x = 0.0f, .y = 0.0f, .in_tangent = {-0.1f, 0.0f}, .out_tangent = {0.25f, 0.0f}},
            {.x = 1.0f, .y = 1.0f, .in_tangent = {-0.25f, 0.0f}, .out_tangent = {0.1f, 0.0f}},
        };
        CHECK(std::abs(eval_curve(curve, 0.5f) - 0.5f) < 0.05f);

        float prop_val = 42.0f;
        int   reset_hits = 0;
        rect  rc_prop_row{}, rc_flex_first{}, rc_flex_last{}, rc_curve{};

        static constexpr float sx[] = {1.0f, 2.5f, 4.0f, 7.0f};
        static constexpr float sy0[] = {1.0f, 10.0f, 100.0f, 500.0f};
        static constexpr float sy1[] = {2.0f, 5.0f, 20.0f, 40.0f};
        static constexpr float hm[9] = {0.1f, 0.4f, 0.9f, 0.2f, 0.6f, 0.8f, 0.3f, 0.5f, 1.0f};
        const plot_series sc_series[1] = {{.name = "pts", .values = sy0, .col = {90, 200, 255, 255}, .x_values = sx}};
        const plot_series st_series[2] = {{.name = "A", .values = sy0}, {.name = "B", .values = sy1}};

        const auto build = [&] {
            if (auto w = h.ui.window("tools", {10, 10}, {560, 860})) {
                if (h.ui.begin_timeline("Seq", tl, {520.0f, 110.0f})) {
                    (void)h.ui.timeline_track("Camera", clips, keys);
                    h.ui.end_timeline();
                }
                h.ui.curve_editor("Ease", curve, {520.0f, 110.0f});
                rc_curve = h.ui.item_rect();

                if (h.ui.begin_property_grid("props", 0.4f)) {
                    if (h.ui.property_row("Exposure", {.modified = true, .mixed = true}, [&] {
                            h.ui.slider("##exp", prop_val, 0.0f, 100.0f);
                        })) {
                        ++reset_hits;
                        prop_val = 10.0f;
                    }
                    rc_prop_row = h.ui.item_rect();
                    h.ui.end_property_grid();
                }

                // flex wrap: 5 buttons of 80px inside 260px width must wrap onto >= 2 rows
                h.ui.begin_flex({.gap_x = 6.0f, .gap_y = 6.0f, .max_width = 260.0f});
                for (int i = 0; i < 5; ++i) {
                    h.ui.set_next_item_width(80.0f);
                    h.ui.push_id(i);
                    (void)h.ui.button("Tag");
                    if (i == 0) { rc_flex_first = h.ui.item_rect(); }
                    if (i == 4) { rc_flex_last  = h.ui.item_rect(); }
                    h.ui.pop_id();
                }
                h.ui.end_flex();

                plot_options po_sc; po_sc.kind = plot_kind::scatter; po_sc.size = {250.0f, 90.0f}; po_sc.y.log_scale = true;
                h.ui.plot("ScatterLog", sc_series, po_sc);
                plot_options po_st; po_st.kind = plot_kind::stacked_bars; po_st.size = {250.0f, 90.0f};
                h.ui.plot("Stacked", st_series, po_st);
                h.ui.plot_heatmap("Heat", hm, 3, 3, {250.0f, 90.0f});

                // direct SVG vector drawing
                draw_svg_path(h.ui.draw(), {{460.0f, 700.0f}, {492.0f, 732.0f}}, svg_icons::search, svg_icons::viewbox,
                              {220, 220, 255, 255}, 2.0f);
            }
        };

        h.frames(build, 3);
        CHECK(rc_flex_last.min.y > rc_flex_first.max.y); // wrapped onto second row
        // click reset dot on the left edge of the property row
        h.click({10.0f + h.ui.theme().padding + 6.0f, rc_prop_row.center().y}, build);
        CHECK(reset_hits == 1 && prop_val == 10.0f);
        // double-click middle of curve editor to add a 3rd point
        h.click(rc_curve.center(), build);
        h.click(rc_curve.center(), build);
        CHECK(curve.size() == 3);
        check_finite_and_valid(h.ui.render_data(), nr);
    }

    // 5. theme_watcher live hot-reload
    {
        const auto tmp = std::filesystem::temp_directory_path() / "strata_test_watch.theme";
        const std::string path = tmp.string();
        style s = themes::midnight();
        s.accent = color::from_hex(0x112233ff);
        CHECK(themes::save_file(path, s, "v1"));

        themes::theme_watcher w{path, 0.0f};
        style loaded = themes::light();
        CHECK(w.poll(loaded, 0.1f));
        CHECK(loaded.accent == color::from_hex(0x112233ff));
        CHECK(!w.poll(loaded, 0.1f)); // unchanged mtime

        s.accent = color::from_hex(0xabcdef00 | 0xff);
        CHECK(themes::save_file(path, s, "v2"));
        std::error_code ec;
        std::filesystem::last_write_time(tmp, std::filesystem::last_write_time(tmp, ec) + std::chrono::seconds(2), ec);
        CHECK(w.poll(loaded, 0.1f));
        CHECK(loaded.accent == color::from_hex(0xabcdefff));
        std::filesystem::remove(tmp, ec);
    }

    // 6. software_renderer partial damage-rect clear & render
    {
        harness h;
        h.in.display_size = {200.0f, 200.0f};
        software_renderer sr;
        CHECK(sr.create(h.ui.font()));
        sr.resize(200, 200);
        sr.clear({20, 30, 40, 255});

        h.frame([&] {
            h.ui.draw().rect_filled({{10, 10}, {190, 190}}, {220, 80, 60, 255});
        });
        const rect dmg{{50.0f, 50.0f}, {100.0f, 100.0f}};
        sr.clear({0, 0, 0, 255}, dmg);
        sr.render(h.ui.render_data(), dmg);

        const auto px = sr.pixels();
        const auto idx = [](u32 x, u32 y) { return (static_cast<std::size_t>(y) * 200 + x) * 4; };
        // outside damage rect remains original clear color (20, 30, 40)
        CHECK(px[idx(20, 20) + 0] == 20 && px[idx(20, 20) + 1] == 30 && px[idx(20, 20) + 2] == 40);
        // inside damage rect received the red-orange rect
        CHECK(px[idx(75, 75) + 0] > 200 && px[idx(75, 75) + 1] < 100);
    }
}

} // namespace

void run_interactive_tests()
{
    test_interactive_charts_and_extremes();
    test_interactive_controls_click_drag();
    test_interactive_workspace_session();
    test_interactive_new_features();
    test_interactive_universal_suite();
}

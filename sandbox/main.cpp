#include "demo2.hpp"
#include "gfx_host.hpp"
#include "imgio.hpp"
#include "openvr_main.hpp"
#include "selftest.hpp"
#include "xr_main.hpp"

#include <strata/platform/win32.hpp>
#include <strata/strata.hpp>

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <algorithm>
#include <string_view>
#include <vector>

namespace {

struct options {
    bool           use_dx12   = false;
    bool           use_xr     = false; // --xr: standalone vr loop (xr_main.cpp), skips the windowed path entirely
    bool           use_openvr = false; // --openvr: standalone steamvr overlay loop (openvr_main.cpp)
    bool           vsync      = true;
    int            fps_cap    = -1;    // -1: display refresh when vsync is on, 0: unlimited
    strata::i32    width      = 1280;
    strata::i32    height     = 720;
    strata::i32    max_frames = 0;      // 0 = run until closed
    strata::i32    stress     = 60;
    int            theme      = 0;      // index into strata::themes::names()
    std::string    theme_file;          // a theme file applied on top of the theme
    float          scale      = 0.0f;   // ui scale; 0 = the monitor's dpi scale
    float          rescale    = 0.0f;   // > 0: switch to this scale after a few frames (tests atlas re-upload)
    float          scroll_speed = 1.0f; // how far a wheel notch scrolls, as a multiple of the mouse settings' lines
    std::string    scene;               // start with one scene: default, features, visuals, ...
    int            icon_page  = -1;     // --scene icons: show this 256-code-point page (hex)
    std::string    shot;                // write a screenshot (png) of the last frame and exit
    std::string    golden;              // compare the last frame with this png and exit (0 = same)
    std::string    artifacts;           // where failed comparisons write .actual / .diff (default: temp)
    bool           update_golden = false;
    strata::u32    golden_tolerance = 6;    // per channel, out of 255: gpus round a little differently
    double         golden_allowed   = 0.3;  // percent of pixels allowed beyond that (fonts shift glyphs)
    strata::i32    shot_frames = 24;    // frames rendered (fixed 1/60 s steps) before the screenshot
    const wchar_t* log_path   = nullptr; // redirect stderr (debug layer output, frame report) to a file
    bool           metrics    = false;  // strata's own inspector windows (frame stats, live draw commands)
    bool           idle       = false;  // skip render + present while context::can_idle() says nothing changed

    // fonts: 0 = primary, 1 = mono, 2 = heading
    std::string    font_file;           // primary from a .ttf / .otf / .ttc
    std::string    face       = "Segoe UI";
    float          size       = 14.0f;
    std::string    mono       = "Consolas";
    float          mono_size  = 13.0f;
    std::string    heading    = "Segoe UI";
    float          heading_size = 22.0f;
    bool           extra_fonts = true;
    bool           icons      = true;   // icon font, id after the extras
    std::string    icon_face  = "Segoe MDL2 Assets";
    float          icon_size  = 16.0f;
    bool           cjk        = false;  // bake kana + cjk ideographs into the primary font
    bool           kerning    = true;

    bool           selftest   = false; // run the headless checks of the ui logic and exit
    bool           features   = false; // only the docked feature windows (editor, rich text, images, tables)
    bool           menu_only  = false; // show only the sidebar settings-menu example
    bool           show_help  = false;
    std::wstring   bad_flag;
};

constexpr const wchar_t* usage_text =
    L"strata_sandbox [options]\n\n"
    L"  --dx11 | --dx12          graphics backend (default dx11)\n"
    L"  --xr                     standalone vr loop instead of a window (needs a build with STRATA_BUILD_OPENXR;\n"
    L"                           uses --width/--height as the panel's pixel size, --face/--size/--theme as usual)\n"
    L"  --openvr                 standalone steamvr overlay loop instead of a window (needs STRATA_BUILD_OPENVR);\n"
    L"                           same --width/--height/--face/--size/--theme as --xr\n"
    L"  --vsync | --novsync      present interval (default vsync)\n"
    L"  --fps N                  frame cap; 0 = unlimited (default: the display refresh rate, since some drivers\n"
    L"                           ignore vsync and would otherwise run at thousands of fps)\n"
    L"  --width N --height N     client size (default 1280x720)\n"
    L"  --frames N               exit after N frames (smoke test)\n"
    L"  --stress N               buttons in the stress window (default 200)\n"
    L"  --theme N|NAME           a built-in theme by index or name (midnight, light, ocean, rose, dracula, nord, ...)\n"
    L"  --theme-file FILE        a theme file applied on top\n"
    L"  --scale F                ui scale (default: the monitor's dpi scale)\n"
    L"  --scroll-speed F         how far one wheel notch scrolls: 1 = the mouse settings' lines, 2 = twice as far\n"
    L"  --scene NAME             start with one scene only: default, features, visuals, menus, config, tabs, dnd, lists,\n"
    L"                           editor, icons, bigtree, rows, app, ...\n"
    L"  --icon-page HEX          with --scene icons: show that page of 256 code points instead of the named set\n"
    L"  --shot FILE              render --shot-frames fixed steps, save the last frame as a png and exit\n"
    L"  --golden FILE            same, but compare with the png FILE (exit code 0 = match); --update-golden rewrites it\n"
    L"  --golden-tolerance N     a pixel matches when every channel is within N of the golden (default 6, of 255)\n"
    L"  --golden-allowed PCT     percent of the pixels that may not match (default 0.3)\n"
    L"  --menu                   show only the sidebar settings-menu example\n"
    L"  --selftest               run the headless ui checks (text editing, docking, ...) and exit\n"
    L"  --features               start with the docked feature windows only (multi-line input, rich text, images, tables)\n"
    L"  --log FILE               write stderr (debug layer, frame report) to FILE\n"
    L"  --metrics                show strata's own metrics and draw-list inspector windows\n"
    L"  --idle                   skip rendering and presenting while the ui reports nothing changed, and sleep until\n"
    L"                           input or the ui's next deadline (the frame report says how many frames were skipped)\n\n"
    L"fonts\n"
    L"  --font FILE              primary font from a .ttf/.otf/.ttc\n"
    L"  --face NAME              primary font, installed family (default Segoe UI)\n"
    L"  --size PX                primary pixel height (default 14)\n"
    L"  --mono FILE|NAME         second font, id 1 (default Consolas), --mono-size PX\n"
    L"  --heading FILE|NAME      third font, id 2, drawn bold (default Segoe UI), --heading-size PX\n"
    L"  --no-extra-fonts         load only the primary font\n"
    L"  --icons FILE|NAME        icon font (default Segoe MDL2 Assets, falls back to Segoe Fluent Icons), --icon-size PX\n"
    L"  --no-icons               skip the icon font\n"
    L"  --cjk                    bake kana + CJK ideographs (needs a font that has them)\n"
    L"  --no-kern                ignore the fonts' kerning pairs\n\n"
    L"  --help                   this text\n"
    L"  Esc closes the window.";

[[nodiscard]] std::string to_utf8(const wchar_t* w)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n - 1, 0)), '\0');
    if (n > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    }
    return out;
}

// a path separator or extension means a font file, otherwise an installed family
[[nodiscard]] bool looks_like_path(std::string_view s) noexcept
{
    return s.find_first_of("\\/") != std::string_view::npos || s.find('.') != std::string_view::npos;
}

struct app {
    strata::win32_platform platform;
    gfx_host*              host{};
    bool                   minimized{};
    bool                   wants_text{}; // a text field has focus: Esc belongs to it
    float                  pending_dpi{}; // > 0: the window moved to a monitor with this dpi scale
};

[[nodiscard]] options parse_args()
{
    options opt;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return opt;
    }

    const auto value = [&](int& i) -> const wchar_t* {
        return i + 1 < argc ? argv[++i] : L"";
    };
    const auto integer = [&](int& i) { return static_cast<strata::i32>(std::wcstol(value(i), nullptr, 10)); };
    const auto real    = [&](int& i) { return static_cast<float>(std::wcstod(value(i), nullptr)); };

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--dx12")                { opt.use_dx12 = true; }
        else if (a == L"--dx11")           { opt.use_dx12 = false; }
        else if (a == L"--xr")             { opt.use_xr = true; }
        else if (a == L"--openvr")         { opt.use_openvr = true; }
        else if (a == L"--vsync")          { opt.vsync = true; }
        else if (a == L"--novsync")        { opt.vsync = false; }
        else if (a == L"--fps")            { opt.fps_cap = std::max(integer(i), 0); }
        else if (a == L"--width")          { opt.width = std::max(integer(i), 320); }
        else if (a == L"--height")         { opt.height = std::max(integer(i), 240); }
        else if (a == L"--frames")         { opt.max_frames = integer(i); }
        else if (a == L"--stress")         { opt.stress = std::max(integer(i), 0); }
        else if (a == L"--theme")          {
            const wchar_t* v = value(i);
            if (*v >= L'0' && *v <= L'9') { opt.theme = static_cast<int>(std::wcstol(v, nullptr, 10)); }
            else {
                const std::string name = to_utf8(v);
                const auto names = strata::themes::names();
                for (std::size_t k = 0; k < names.size(); ++k) { if (names[k] == name) { opt.theme = static_cast<int>(k); } }
            }
        }
        else if (a == L"--theme-file")     { opt.theme_file = to_utf8(value(i)); }
        else if (a == L"--scale")          { opt.scale = std::max(real(i), 0.0f); }
        else if (a == L"--rescale")        { opt.rescale = std::max(real(i), 0.0f); }
        else if (a == L"--scroll-speed")   { opt.scroll_speed = std::max(real(i), 0.0f); }
        else if (a == L"--scene")          { opt.scene = to_utf8(value(i)); }
        else if (a == L"--icon-page")      { opt.icon_page = static_cast<int>(std::wcstol(value(i), nullptr, 16)); }
        else if (a == L"--shot")           { opt.shot = to_utf8(value(i)); }
        else if (a == L"--golden")         { opt.golden = to_utf8(value(i)); }
        else if (a == L"--update-golden")  { opt.update_golden = true; }
        else if (a == L"--artifacts")      { opt.artifacts = to_utf8(value(i)); }
        else if (a == L"--golden-tolerance") { opt.golden_tolerance = static_cast<strata::u32>(std::clamp(integer(i), 0, 255)); }
        else if (a == L"--golden-allowed") { opt.golden_allowed = std::clamp(static_cast<double>(real(i)), 0.0, 100.0); }
        else if (a == L"--shot-frames")    { opt.shot_frames = std::max(integer(i), 1); }
        else if (a == L"--menu")           { opt.menu_only = true; }
        else if (a == L"--selftest")       { opt.selftest = true; }
        else if (a == L"--features")       { opt.features = true; }
        else if (a == L"--log")            { opt.log_path = _wcsdup(value(i)); }
        else if (a == L"--metrics")        { opt.metrics = true; }
        else if (a == L"--idle")           { opt.idle = true; }
        else if (a == L"--font")           { opt.font_file = to_utf8(value(i)); }
        else if (a == L"--face")           { opt.face = to_utf8(value(i)); }
        else if (a == L"--size")           { opt.size = std::max(real(i), 6.0f); }
        else if (a == L"--mono")           { opt.mono = to_utf8(value(i)); }
        else if (a == L"--mono-size")      { opt.mono_size = std::max(real(i), 6.0f); }
        else if (a == L"--heading")        { opt.heading = to_utf8(value(i)); }
        else if (a == L"--heading-size")   { opt.heading_size = std::max(real(i), 6.0f); }
        else if (a == L"--no-extra-fonts") { opt.extra_fonts = false; }
        else if (a == L"--icons")          { opt.icon_face = to_utf8(value(i)); }
        else if (a == L"--icon-size")      { opt.icon_size = std::max(real(i), 6.0f); }
        else if (a == L"--no-icons")       { opt.icons = false; }
        else if (a == L"--cjk")            { opt.cjk = true; }
        else if (a == L"--no-kern")        { opt.kerning = false; }
        else if (a == L"--help" || a == L"-h" || a == L"/?") { opt.show_help = true; }
        else                               { opt.bad_flag = a; }
    }
    ::LocalFree(argv);
    return opt;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* self = reinterpret_cast<app*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self != nullptr) {
        (void)self->platform.handle_message(hwnd, msg, wparam, static_cast<std::intptr_t>(lparam));
        if (strata::win32_platform::swallows(msg)) { return 0; } // (the input method's messages: the platform did the work)
    }

    switch (msg) {
    case WM_SIZE:
        if (self != nullptr && self->host != nullptr) {
            self->minimized = wparam == SIZE_MINIMIZED;
            if (!self->minimized) {
                self->host->resize(LOWORD(lparam), HIWORD(lparam));
            }
        }
        return 0;
    case WM_DPICHANGED:
        if (self != nullptr) {
            self->pending_dpi = static_cast<float>(HIWORD(wparam)) / 96.0f;
            const RECT* r = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && (self == nullptr || !self->wants_text)) {
            ::DestroyWindow(hwnd);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && self != nullptr && self->platform.apply_cursor()) {
            return TRUE;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

[[nodiscard]] double seconds_now() noexcept
{
    static const double inv_freq = [] {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * inv_freq;
}

// waits until an absolute time (seconds_now()): high-resolution timer, then a short spin
class frame_limiter {
public:
    frame_limiter() noexcept
        : timer_{::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)}
    {}
    ~frame_limiter()
    {
        if (timer_ != nullptr) { ::CloseHandle(timer_); }
    }
    frame_limiter(const frame_limiter&)            = delete;
    frame_limiter& operator=(const frame_limiter&) = delete;

    void wait_until(double target) const noexcept;

private:
    HANDLE timer_{};
};

void frame_limiter::wait_until(double target) const noexcept
{
    for (;;) {
        const double remaining = target - seconds_now();
        if (remaining <= 0.0) {
            return;
        }
        if (timer_ != nullptr && remaining > 0.002) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((remaining - 0.0015) * 1.0e7); // relative, 100 ns units
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(timer_, INFINITE);
                continue;
            }
        }
        if (remaining > 0.002) {
            ::Sleep(1);
        } else {
            ::YieldProcessor(); // the last stretch is spun for accuracy
        }
    }
}

struct demo_state {
    bool        show_stress = true;
    bool        dim_text    = false;
    bool        flag_a      = true;
    bool        flag_b      = false;
    int         font_mono    = -1;   // font ids, -1 when not loaded
    int         font_heading = -1;
    int         font_icons   = -1;
    float       speed       = 1.0f;
    strata::i32 clicks      = 0;
    float       clear_r     = 14.0f;
    float       clear_g     = 16.0f;
    float       clear_b     = 22.0f;
    strata::i32 stress      = 60;
    float       progress    = 0.0f;
    float       time        = 0.0f;
    double      fps         = 0.0;
    double      ui_ms       = 0.0;
    int         pending_theme = -1; // applied between frames
    float       scroll_speed  = 1.0f; // --scroll-speed

    int         tab         = 0;
    int         theme_combo = 0;
    int         long_combo  = 3;
    std::string name        = "strata";
    strata::secure_string password = "hunter2"; // zeroed whenever it is freed or reallocated
    char        search[64]  = {};
    int         enters      = 0;
    std::string last_action = "-";

    int            data_tab    = 0;
    strata::color  accent_col  = strata::color::from_hex(0x5b8dffff);
    strata::color  glass_col   = strata::color::from_hex(0xf0568fb4);
    strata::color  inline_col  = strata::color::from_hex(0x19c2b4ff);
    std::string    selected_node = "Cube";

    struct file_row {
        std::string name;
        const char* type{};
        int         size{};     // KB
        int         modified{}; // days ago
    };
    std::vector<file_row> rows;
    int  sort_col      = 0;
    bool sort_ascending = true;
    int  selected_row  = -1;

    // feature windows: docking, multi-line input, rich text, images, nested tables
    strata::texture_id image_tex = 0;
    bool        show_features = false;
    bool        features_only = false;
    bool        dock_pending  = false; // dock the feature windows on the next frame
    std::string saved_layout;          // what "save layout" stored (ui.dock_save_layout())
    std::string notes = "Multi-line text input\n\n- Enter starts a new line, Ctrl+Enter submits\n"
                        "- Up / Down / Page Up / Page Down move by line, Home / End by line, Ctrl+Home / End by text\n"
                        "- Ctrl+Z undoes, Ctrl+Y (or Ctrl+Shift+Z) redoes\n"
                        "- with word wrap on, long lines break at word boundaries; otherwise the field scrolls sideways";
    std::string read_only_text = "read-only fields can be selected and copied,\nbut not edited.";
    bool        notes_wrap    = true;
    int         notes_submits = 0;
    int         image_clicks  = 0;
    int         rich_tab      = 0;
    int         rich_combo    = 0;
    bool        rich_flag     = true;
    bool        rich_flag2    = false;
    std::string rich_selected = "one";

    demo2_state x;
    bool        scene_only = false; // a scene that draws nothing but its own windows
    std::string theme_path   = (std::filesystem::temp_directory_path() / "strata_sandbox_theme.ini").string(); // not the working directory: that is the project
    std::string theme_status = "-";

    // --metrics: strata's own inspector windows
    bool show_metrics = true;
    bool show_cmds    = true;

    // settings-menu example
    bool show_menu   = false;
    bool menu_only   = false;
    int  menu_page   = 0;
    bool m_login     = true;
    bool m_updates   = true;
    bool m_reduce    = false;
    bool m_vsync     = true;
    int  m_lang      = 0;
    int  m_quality   = 2;
    int  m_fps       = 144;
    int  m_threads   = 8;
    float m_scale    = 1.0f;
    float m_bright   = 0.55f;
    strata::key key_menu  = strata::key::insert;
    strata::key key_shot  = strata::key::f8;
    strata::key key_quick = strata::key::none;
    std::string m_profile = "default";
    strata::color m_highlight = strata::color::from_hex(0xffb454c8);
};

void init_rows(demo_state& s)
{
    static constexpr const char* types[] = {"texture", "mesh", "shader", "audio", "script"};
    static constexpr const char* stems[] = {"rock", "tree", "water", "metal", "grass", "wall", "sky", "bricks"};
    static constexpr const char* exts[]  = {".png", ".obj", ".hlsl", ".wav", ".lua"};
    for (int i = 0; i < 80; ++i) {
        demo_state::file_row r;
        const int t = (i * 7) % 5;
        r.name     = std::string{stems[(i * 3) % 8]} + "_" + std::to_string(100 + i) + exts[t];
        r.type     = types[t];
        r.size     = (i * 7919) % 5000 + 12;
        r.modified = (i * 31) % 365;
        s.rows.push_back(std::move(r));
    }
}

void apply_theme(strata::context& ui, demo_state& s, int which)
{
    using namespace strata;
    const auto names = themes::names();
    which = std::clamp(which, 0, static_cast<int>(names.size()) - 1);
    s.theme_combo = which;
    switch (which) {
    case 0: ui.theme() = themes::midnight(); s.clear_r = 14;  s.clear_g = 16;  s.clear_b = 22;  break;
    case 1: ui.theme() = themes::light();    s.clear_r = 218; s.clear_g = 223; s.clear_b = 236; break;
    case 2: ui.theme() = themes::ocean();    s.clear_r = 5;   s.clear_g = 13;  s.clear_b = 19;  break;
    case 3: ui.theme() = themes::rose();     s.clear_r = 15;  s.clear_g = 9;   s.clear_b = 14;  break;
    default: {
        (void)themes::by_name(names[static_cast<std::size_t>(which)], ui.theme());
        const color w = ui.theme().window_bg; // the desktop behind the ui follows the window color
        const float luma = 0.299f * w.r + 0.587f * w.g + 0.114f * w.b;
        const float k = luma > 128.0f ? 0.86f : 0.55f;
        s.clear_r = w.r * k; s.clear_g = w.g * k; s.clear_b = w.b * k;
        break;
    }
    }
    ui.theme().scroll_speed = s.scroll_speed; // (--scroll-speed: a theme carries it, so it is set again here)
}

// a widget the library does not have: animated level meter drawn with ui.draw()
void meter(strata::context& ui, float time)
{
    using namespace strata;
    const item_result item = ui.custom_item("meter", {0.0f, 46.0f});
    const f32 glow = ui.animate("meter", item.hovered ? 1.0f : 0.0f);

    draw_list& dl = ui.draw();
    const style& th = ui.theme();

    shape_style backdrop;
    backdrop.radius      = radii(th.rounding * 0.8f);
    backdrop.fill_top    = th.widget_bg;
    backdrop.fill_bottom = th.widget_bg;
    backdrop.border      = lerp(th.widget_border, th.accent, glow);
    backdrop.border_width = th.border_width;
    dl.shape(item.bounds, backdrop);

    constexpr int bars = 30;
    constexpr f32 gap  = 3.0f;
    const rect inner   = item.bounds.expanded(-6.0f);
    const f32  bw      = (inner.width() - gap * (bars - 1)) / static_cast<f32>(bars);
    for (int i = 0; i < bars; ++i) {
        const f32 fi    = static_cast<f32>(i);
        const f32 level = 0.30f + 0.70f * (0.5f + 0.5f * std::sin(time * 2.4f + fi * 0.42f) * std::sin(time * 0.8f + fi * 0.13f));
        const f32 x     = inner.min.x + fi * (bw + gap);
        shape_style bar;
        bar.radius      = radii(bw * 0.5f);
        bar.fill_top    = lerp(th.accent_hover, th.accent, 0.3f + 0.7f * (1.0f - level));
        bar.fill_bottom = lerp(th.accent, th.widget_bg, 0.35f);
        dl.shape({{x, inner.max.y - level * inner.height()}, {x + bw, inner.max.y}}, bar);
    }
}

void inspector(strata::context& ui, demo_state& s)
{
    using namespace strata;
    const bool ic = s.font_icons >= 0;
    const auto icon_id = static_cast<font_id>(std::max(s.font_icons, 0));
    const auto icon_btn = [&](std::string_view glyph, std::string_view label) {
        return ic ? ui.icon_button(icon_id, glyph, label) : ui.button(label);
    };

    const tab_desc tabs[] = {
        {"General", ic ? std::string_view{icons::settings} : std::string_view{}},
        {"Input",   ic ? std::string_view{icons::edit}     : std::string_view{}},
        {"Colors",  ic ? std::string_view{icons::color}    : std::string_view{}},
    };
    (void)ui.tab_bar("inspector_tabs", tabs, std::size(tabs), s.tab, icon_id);
    ui.spacing();

    if (s.tab == 0) {
        const auto theme_names = themes::names();
        if (ui.combo("theme", s.theme_combo, theme_names)) {
            s.pending_theme = s.theme_combo;
        }

        static constexpr std::string_view long_list[] = {
            "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel",
            "India", "Juliett", "Kilo", "Lima", "Mike", "November"};
        if (ui.combo("long list (scrolls)", s.long_combo, long_list)) {
            s.last_action = std::string{"combo -> "} + std::string{long_list[s.long_combo]};
        }
        ui.spacing();

        if (icon_btn(icons::home, "Home"))    { s.last_action = "home"; }
        ui.same_line();
        if (icon_btn(icons::save, "Save"))    { s.last_action = "save"; }
        ui.same_line();
        if (icon_btn(icons::trash, "Delete")) { s.last_action = "delete"; }
        ui.same_line();
        if (icon_btn(icons::refresh, {}))     { s.last_action = "refresh"; }
        ui.textf("last action: {}", s.last_action);
    } else if (s.tab == 1) {
        (void)ui.input_text("name", s.name, "your name");
        (void)ui.input_text("password", s.password, "secret", input_flags::password);
        if (ui.input_text("search", s.search, sizeof(s.search), "type and press enter...")) {
            s.last_action = "search edited";
        }
        if (ui.input_submitted()) { ++s.enters; }
        ui.textf("name = \"{}\"   enter x{}", s.name, s.enters);
        if (ic) {
            ui.icon_label(icon_id, icons::info, "ctrl+a / c / x / v, shift+arrows, double click");
        } else {
            ui.text_dim("ctrl+a / c / x / v, shift+arrows, double click");
        }
    } else {
        ui.slider("red", s.clear_r, 0.0f, 255.0f);
        ui.slider("green", s.clear_g, 0.0f, 255.0f);
        ui.slider("blue", s.clear_b, 0.0f, 255.0f);
    }
}

void data_views(strata::context& ui, demo_state& s)
{
    using namespace strata;
    (void)ui.tab_bar("data_tabs", {"Color", "Tree", "Table"}, s.data_tab);
    ui.spacing();

    if (s.data_tab == 0) {
        if (ui.color_edit("accent color (live theme)", s.accent_col, color_flags::no_alpha)) {
            ui.theme().accent       = s.accent_col;
            ui.theme().accent_hover = lerp(s.accent_col, color{255, 255, 255, 255}, 0.25f);
        }
        (void)ui.color_edit("translucent color", s.glass_col);
        ui.spacing();
        (void)ui.color_picker("inline picker", s.inline_col);
    } else if (s.data_tab == 1) {
        const auto leaf = [&](std::string_view name) {
            if (ui.tree_leaf(name, s.selected_node == name)) { s.selected_node = std::string{name}; }
        };
        if (auto scene = ui.tree("Scene", tree_flags::default_open)) {
            leaf("Camera");
            if (auto lights = ui.tree("Lights", tree_flags::default_open)) {
                leaf("Sun");
                leaf("Lamp 1");
                leaf("Lamp 2");
            }
            if (auto meshes = ui.tree("Meshes")) {
                leaf("Cube");
                leaf("Sphere");
                if (auto suzanne = ui.tree("Suzanne")) {
                    leaf("Material");
                    if (auto mods = ui.tree("Modifiers")) {
                        leaf("Subdivision");
                        leaf("Bevel");
                    }
                }
            }
            if (auto audio = ui.tree("Audio")) {
                leaf("Ambience");
                leaf("Footsteps");
            }
        }
        ui.spacing();
        ui.textf("selected: {}", s.selected_node);
    } else {
        if (ui.begin_table("files", 4, table_default, 250.0f)) {
            ui.table_setup_column("Name", 0.0f, 1.5f);
            ui.table_setup_column("Type", 74.0f);
            ui.table_setup_column("Size", 74.0f);
            ui.table_setup_column("Modified", 0.0f, 0.9f);

            const int clicked = ui.table_headers_row(s.sort_col, s.sort_ascending);
            if (clicked >= 0) {
                s.sort_ascending = clicked == s.sort_col ? !s.sort_ascending : true;
                s.sort_col       = clicked;
                std::string selected_name = s.selected_row >= 0 ? s.rows[static_cast<std::size_t>(s.selected_row)].name : std::string{};
                std::ranges::sort(s.rows, [&](const demo_state::file_row& a, const demo_state::file_row& b) {
                    const auto key = [&](const demo_state::file_row& r) {
                        switch (s.sort_col) {
                        case 1:  return std::pair{std::string{r.type}, 0};
                        case 2:  return std::pair{std::string{}, r.size};
                        case 3:  return std::pair{std::string{}, r.modified};
                        default: return std::pair{r.name, 0};
                        }
                    };
                    return s.sort_ascending ? key(a) < key(b) : key(b) < key(a);
                });
                s.selected_row = -1;
                for (std::size_t i = 0; i < s.rows.size(); ++i) {
                    if (s.rows[i].name == selected_name) { s.selected_row = static_cast<int>(i); }
                }
            }

            for (std::size_t i = 0; i < s.rows.size(); ++i) {
                if (!ui.table_next_row()) { continue; } // scrolled out of view
                const auto& r = s.rows[i];
                (void)ui.table_next_column();
                if (ui.selectable(r.name, s.selected_row == static_cast<int>(i))) { s.selected_row = static_cast<int>(i); }
                (void)ui.table_next_column();
                ui.text_dim(r.type);
                (void)ui.table_next_column();
                ui.textf("{} KB", r.size);
                (void)ui.table_next_column();
                ui.textf("{} d ago", r.modified);
            }
            ui.end_table();
        }
        ui.spacing();
        if (s.selected_row >= 0) {
            ui.textf("selected: {}", s.rows[static_cast<std::size_t>(s.selected_row)].name);
        } else {
            ui.text_dim("click a row; click a header to sort; drag header edges to resize");
        }
    }
}

// sidebar settings menu: borderless draggable window, icon tab strip, a page that fades in on tab change.
// the pages are placeholders.
void menu_demo(strata::context& ui, demo_state& s)
{
    using namespace strata;
    const bool ic = s.font_icons >= 0;
    const auto icon_id = static_cast<font_id>(std::max(s.font_icons, 0));
    const auto ico = [&](const glyph_string& g) { return ic ? std::string_view{g} : std::string_view{}; };
    const bool have_heading = s.font_heading >= 0;
    const bool have_mono    = s.font_mono >= 0;

    constexpr auto flags = window_flags::no_title_bar | window_flags::drag_by_body | window_flags::resizable;
    auto w = ui.window("settings menu", {260, 90}, {740, 480}, flags);
    if (!w) { return; }

    if (have_heading) {
        const auto big = ui.with_font(static_cast<font_id>(s.font_heading));
        ui.text("Settings");
    } else {
        ui.text("Settings");
    }
    ui.same_line();
    ui.rich_text("<c=8a91a6>v0.4  ·  drag anywhere to move, drag the corner to resize</c>");
    ui.same_line(ui.content_width() - 36.0f);
    if (ic ? ui.icon_button(icon_id, icons::cancel) : ui.button("x")) { s.show_menu = false; }
    ui.tooltip("Close");
    ui.separator();

    const tab_desc tabs[] = {
        {"General", ico(icons::settings)}, {"Display", ico(icons::view)}, {"Colors", ico(icons::color)},
        {"Input", ico(icons::edit)},       {"About", ico(icons::info)},
    };
    (void)ui.tab_strip("menu_tabs", tabs, std::size(tabs), s.menu_page, icon_id, 150.0f);
    ui.same_line();

    if (auto page = ui.child("menu_page")) {
        const auto fade = ui.page_transition("menu_fade", s.menu_page);

        if (s.menu_page == 0) {
            if (auto c = ui.card("Startup", ico(icons::home), icon_id)) {
                ui.toggle("Launch at login", s.m_login);
                ui.tooltip("Start the app automatically when you sign in");
                ui.toggle("Check for updates", s.m_updates);
                ui.combo("Language", s.m_lang, {"English", "Polski", "Deutsch", "Español"});
            }
            if (auto c = ui.card("Performance", ico(icons::refresh), icon_id)) {
                ui.slider("Frame limit", s.m_fps, 30, 240);
                ui.tooltip("Upper bound for frames per second");
                ui.slider("Worker threads", s.m_threads, 1, 16);
                ui.checkbox("Reduce motion", s.m_reduce);
            }
        } else if (s.menu_page == 1) {
            if (auto c = ui.card("Graphics", ico(icons::view), icon_id)) {
                ui.combo("Quality", s.m_quality, {"Low", "Medium", "High", "Ultra"});
                ui.slider("Render scale", s.m_scale, 0.5f, 2.0f);
                ui.slider("Brightness", s.m_bright, 0.0f, 1.0f);
                ui.toggle("Vertical sync", s.m_vsync);
                ui.tooltip("Wait for the display's refresh to avoid tearing");
            }
            if (auto c = ui.card("Overlay", ico(icons::color), icon_id)) {
                if (ui.color_edit("Accent", s.accent_col, color_flags::no_alpha)) {
                    ui.theme().accent       = s.accent_col;
                    ui.theme().accent_hover = lerp(s.accent_col, color{255, 255, 255, 255}, 0.25f);
                }
                (void)ui.color_edit("Highlight", s.m_highlight);
            }
        } else if (s.menu_page == 2) {
            if (auto c = ui.card("Theme", ico(icons::color), icon_id)) {
                ui.text_dim("preset");
                if (ui.button("midnight")) { s.pending_theme = 0; }
                ui.same_line();
                if (ui.button("light")) { s.pending_theme = 1; }
                ui.same_line();
                if (ui.button("ocean")) { s.pending_theme = 2; }
                ui.same_line();
                if (ui.button("rose")) { s.pending_theme = 3; }
            }
            if (auto c = ui.card("Custom accent", ico(icons::edit), icon_id)) {
                if (ui.color_picker("accent", s.accent_col, color_flags::no_alpha)) {
                    ui.theme().accent       = s.accent_col;
                    ui.theme().accent_hover = lerp(s.accent_col, color{255, 255, 255, 255}, 0.25f);
                }
            }
        } else if (s.menu_page == 3) {
            if (auto c = ui.card("Key bindings", ico(icons::edit), icon_id)) {
                (void)ui.hotkey("Open menu", s.key_menu);
                ui.tooltip("Click, then press a key. Esc cancels, Backspace clears.");
                (void)ui.hotkey("Screenshot", s.key_shot);
                (void)ui.hotkey("Quick action", s.key_quick);
            }
            if (auto c = ui.card("Profile", ico(icons::contact), icon_id)) {
                (void)ui.input_text("Profile name", s.m_profile, "name");
            }
        } else {
            if (auto c = ui.card("About", ico(icons::info), icon_id)) {
                if (have_heading) { ui.rich_textf("<f={}>Strata</f>  <c=8a91a6>immediate-mode ui for direct3d 11 / 12</c>", s.font_heading); }
                else              { ui.text("Strata"); }
                ui.spacing();
                if (have_mono) { ui.rich_textf("<c=8a91a6>font mixing:</c> plain, <f={0}>mono</f>, <c=ff6b8a>rose</c>, <f={0}><c=5b8dff>blue mono</c></f>, 1 << 2", s.font_mono); }
                ui.text_dim("Icons, tabs, cards, tooltips and key binding all come from the library.");
            }
            if (auto c = ui.card("Build")) {
                ui.textf("backend-independent core, {} fonts in one atlas", ui.font().font_count());
                ui.textf("atlas {}x{}", ui.font().width(), ui.font().height());
            }
        }
    }
}

// procedural test picture: soft-edged disc (transparent corners) with rings, a crosshair and a dot
std::vector<strata::u8> make_demo_image(strata::u32 w, strata::u32 h)
{
    std::vector<strata::u8> px(static_cast<std::size_t>(w) * h * 4);
    const auto byte = [](float v) { return static_cast<strata::u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (strata::u32 y = 0; y < h; ++y) {
        for (strata::u32 x = 0; x < w; ++x) {
            const float u  = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const float v  = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            const float dx = u - 0.5f;
            const float dy = v - 0.5f;
            const float r  = std::sqrt(dx * dx + dy * dy);
            const float t  = (u + v) * 0.5f;
            const float ring = 0.78f + 0.22f * std::sin(r * 46.0f);
            float red   = (0.15f + 0.80f * t) * ring;
            float green = (0.30f + 0.30f * (1.0f - t)) * ring;
            float blue  = (0.95f - 0.55f * t) * ring;
            if (std::abs(dx) < 0.010f || std::abs(dy) < 0.010f) { // crosshair: makes uv crops easy to see
                red = red * 0.4f + 0.6f;  green = green * 0.4f + 0.6f;  blue = blue * 0.4f + 0.6f;
            }
            if (r < 0.09f) { red = green = blue = 1.0f; }
            const float alpha = std::clamp((0.5f - r) * static_cast<float>(w) * 0.35f, 0.0f, 1.0f);
            strata::u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = byte(red);
            p[1] = byte(green);
            p[2] = byte(blue);
            p[3] = byte(alpha);
        }
    }
    return px;
}

// docking, multi-line input with undo, rich text and labels, images, nested tables. the four windows are dockable:
// drag one by its title onto a pane's centre or edge.
void feature_windows(strata::context& ui, demo_state& s)
{
    using namespace strata;
    const auto heading = static_cast<font_id>(std::max(s.font_heading, 0));
    const auto mono    = static_cast<font_id>(std::max(s.font_mono, 0));
    constexpr auto flags = window_flags::resizable | window_flags::dockable;

    if (auto w = ui.window("scene", {40, 90}, {260, 320}, flags)) {
        if (auto root = ui.tree("Scene", tree_flags::default_open)) {
            const auto leaf = [&](std::string_view name) {
                if (ui.tree_leaf(name, s.selected_node == name)) { s.selected_node = std::string{name}; }
            };
            leaf("Camera");
            if (auto lights = ui.tree("Lights", tree_flags::default_open)) { leaf("Sun"); leaf("Lamp 1"); leaf("Lamp 2"); }
            if (auto meshes = ui.tree("Meshes", tree_flags::default_open)) { leaf("Cube"); leaf("Sphere"); leaf("Suzanne"); }
            leaf("Audio");
        }
        ui.spacing();
        ui.text_dim("dock spaces: left / right / bottom edge docks, the main area and the floating \"Tools\" dock");
    }

    if (auto w = ui.window("editor", {60, 70}, {440, 440}, flags)) {
        ui.text_wrapped_colored(ui.theme().text_dim,
                                "Drag any of these windows by its title bar into the dock area: a pane's centre adds a tab, its edges split it. "
                                "Drag a tab away to float it again, drag the splitters to resize.");
        ui.spacing();
        ui.checkbox("word wrap", s.notes_wrap);
        (void)ui.input_multiline("notes", s.notes, {0.0f, 190.0f}, s.notes_wrap ? input_flags::none : input_flags::no_wrap,
                                 "type something...");
        if (ui.input_submitted()) { ++s.notes_submits; }
        ui.textf("{} bytes   {} lines   submitted x{}", s.notes.size(), 1 + std::ranges::count(s.notes, '\n'), s.notes_submits);
        ui.spacing();
        (void)ui.input_text("single line: undo / redo works here too", s.name, "type here");
        ui.spacing();
        (void)ui.input_multiline("read only: select and copy", s.read_only_text, {0.0f, 70.0f}, input_flags::read_only);
    }

    if (auto w = ui.window("rich text", {520, 70}, {430, 440}, flags)) {
        ui.rich_text_wrapped(std::format(
            "<f={0}>Rich text</f>\n<c=8a91a6>Fonts and colors mix inside one paragraph. It wraps at word boundaries, breaks on "
            "new lines, and every run shares the baseline of its line: normal, <f={1}>monospace</f>, <c=ff8800>orange</c>, "
            "<f={1}><c=5b8dff>blue mono</c></f> and <f={0}>heading</f>.</c>\n"
            "Styles nest and combine: <b>bold</b>, <i>italic</i>, <u>underline</u>, <s>strike</s> and <b><i><u>all three</u></i></b>.",
            heading, mono));
        ui.separator();
        ui.text_dim("rich labels: markup in the captions of ordinary widgets");
        {
            const auto rich = ui.rich_labels();
            (void)ui.button(std::format("<f={0}>Save</f> <c=8a91a6>Ctrl+S</c>", heading));
            ui.same_line();
            (void)ui.button(std::format("<c=ff6b8a>Delete</c> <f={0}><c=8a91a6>Del</c></f>", mono));
            (void)ui.checkbox(std::format("enabled <f={0}><c=19c2b4>(mono)</c></f>", mono), s.rich_flag);
            (void)ui.toggle("<c=ffb454>amber</c> toggle", s.rich_flag2);
            const std::array<std::string, 3> items = {"<c=ff6b8a>rose</c>", "<c=19c2b4>teal</c>", std::format("<f={0}>mono</f>", mono)};
            const std::array<std::string_view, 3> views = {items[0], items[1], items[2]};
            (void)ui.combo("<c=8a91a6>colour</c> choice", s.rich_combo, views);
            (void)ui.tab_bar("rich_tabs", {"<c=ff6b8a>one</c>", "<c=19c2b4>two</c>", "three"}, s.rich_tab);
            ui.spacing();
            if (auto t = ui.tree("<c=ffb454>colored</c> tree", tree_flags::default_open)) {
                if (ui.tree_leaf("<c=ff6b8a>one</c>", s.rich_selected == "one")) { s.rich_selected = "one"; }
                if (ui.tree_leaf("<c=19c2b4>two</c>", s.rich_selected == "two")) { s.rich_selected = "two"; }
            }
            (void)ui.button("hover for a rich tooltip");
            ui.tooltip("<c=ffb454>rich</c> tooltip\nwith a second line");
        }
        ui.spacing();
        ui.text_wrapped("plain text_wrapped() breaks a long line at word boundaries, too, without any markup.");
    }

    if (auto w = ui.window("images", {520, 300}, {430, 300}, flags)) {
        if (s.image_tex == 0) {
            ui.text("no texture: the renderer did not accept it");
        } else {
            ui.text_dim("one 160x160 texture, four ways");
            ui.image(s.image_tex, {88, 88});
            ui.same_line();
            ui.image(s.image_tex, {88, 88}, {0.25f, 0.25f}, {0.75f, 0.75f});
            ui.same_line();
            ui.image(s.image_tex, {88, 88}, {0.0f, 0.0f}, {1.0f, 1.0f}, color{255, 170, 120, 255}, 20.0f);
            ui.spacing();
            if (ui.image_button("clickable", s.image_tex, {64, 64})) { ++s.image_clicks; }
            ui.same_line();
            ui.textf("clicked {} times", s.image_clicks);
            ui.text_wrapped_colored(ui.theme().text_dim, "an image is one quad; the shader rounds and tints it");
        }
    }

    if (auto w = ui.window("nested tables", {520, 300}, {430, 300}, flags)) {
        struct group { const char* name; const char* status; int items; };
        static constexpr group groups[] = {{"Textures", "ok", 3}, {"Meshes", "2 warnings", 4}, {"Audio", "ok", 2}};
        if (ui.begin_table("groups", 3)) {
            ui.table_setup_column("Group", 76.0f);
            ui.table_setup_column("Items");
            ui.table_setup_column("Status", 84.0f);
            (void)ui.table_headers_row();
            for (const group& g : groups) {
                (void)ui.table_next_row();
                (void)ui.table_next_column();
                ui.text(g.name);
                (void)ui.table_next_column();
                ui.push_id(g.name);
                if (ui.begin_table("items", 2)) {
                    ui.table_setup_column("Name");
                    ui.table_setup_column("KB", 60.0f);
                    (void)ui.table_headers_row();
                    for (int i = 0; i < g.items; ++i) {
                        (void)ui.table_next_row();
                        (void)ui.table_next_column();
                        ui.textf("{}_{}", g.name, i);
                        (void)ui.table_next_column();
                        ui.textf("{}", 12 * (i + 1) * (i + 2));
                    }
                    ui.end_table();
                }
                ui.pop_id();
                (void)ui.table_next_column();
                ui.text_dim(g.status);
            }
            ui.end_table();
        }
    }
}

void build_ui(strata::context& ui, demo_state& s, const gfx_host& host, strata::draw_data last)
{
    using namespace strata;
    const bool have_mono    = s.font_mono >= 0;
    const bool have_heading = s.font_heading >= 0;

    if (s.menu_only) {
        if (s.show_menu) { menu_demo(ui, s); }
        return;
    }

    s.x.time = s.time;
    s.x.font_mono = s.font_mono;
    s.x.font_heading = s.font_heading;
    s.x.font_icons = s.font_icons;
    s.x.d4.font_icons = s.font_icons;
    s.x.d4.font_mono  = s.font_mono;
    s.x.d4.deterministic = s.x.deterministic;
    s.x.image_tex = s.image_tex;
    demo2_update(ui, s.x, 0.0f);
    if (s.x.art) { demo2_art(ui, s.x); }

    // dockable windows dock into three edge docks (left / right / bottom, below the menu bar; sized only while
    // occupied), the main area, and a movable floating dock.
    const float dock_top = s.x.show_menus ? ui.main_menu_bar_height() : 0.0f;
    rect client{{0.0f, dock_top}, ui.display_size()};
    if (s.show_features) {
        client = ui.dock_edge("explorer", dock_side::left, 280.0f, client);
        client = ui.dock_edge("inspector", dock_side::right, 400.0f, client);
        client = ui.dock_edge("console", dock_side::bottom, 250.0f, client);
    }
    ui.dock_area(client);
    if (s.show_features) {
        (void)ui.floating_dock("Tools", {330.0f, 80.0f}, {360.0f, 330.0f});
        if (s.dock_pending) { // default layout: scene | editor | rich text, images below, tables floating
            s.dock_pending = false;
            (void)ui.dock_window("editor", dock_zone::center);
            (void)ui.dock_window("scene", dock_zone::center, {}, 0.5f, "explorer");
            (void)ui.dock_window("rich text", dock_zone::center, {}, 0.5f, "inspector");
            (void)ui.dock_window("images", dock_zone::center, {}, 0.5f, "console");
            (void)ui.dock_window("nested tables", dock_zone::center, {}, 0.5f, "Tools");
        }
        feature_windows(ui, s);
        if (s.features_only) { return; }
    }
    if (s.x.show_visuals) { demo2_visuals(ui, s.x); }
    if (s.x.show_inputs) { demo2_inputs(ui, s.x); }
    if (s.x.show_charts) { demo2_charts(ui, s.x); }
    if (s.x.show_textures) { demo2_textures(ui, s.x); }
    if (s.x.show_scripts) { demo2_scripts(ui, s.x); }
    if (s.x.show_textlog) { demo2_textlog(ui, s.x); }
    if (s.x.show_menus) { demo2_menus(ui, s.x); }
    if (s.x.show_config) { demo2_config(ui, s.x); }
    if (s.x.d4.show_icons) { demo4_icons(ui, s.x.d4); }
    if (s.x.d4.show_bigtree) { demo4_bigtree(ui, s.x.d4); }
    if (s.x.d4.show_rows) { demo4_rows(ui, s.x.d4); }
    if (s.x.d4.show_app) { demo4_app(ui, s.x.d4); }
    demo2_more(ui, s.x);
    if (s.scene_only) { return; }

    if (auto w = ui.window("strata sandbox", {24, 24}, {340, 0}, window_flags::resizable)) {
        if (have_heading) {
            const auto big = ui.with_font(static_cast<font_id>(s.font_heading));
            ui.text("strata");
        }
        ui.textf("backend  {}", host.name());
        {
            const auto mono = ui.with_font(have_mono ? static_cast<font_id>(s.font_mono) : 0u);
            if (s.x.deterministic) {
                ui.text("- fps   ui - ms");
                ui.text("-v -i -s -d");
            } else {
                ui.textf("{:.0f} fps   ui {:.3f} ms", s.fps, s.ui_ms);
                ui.textf("{}v {}i {}s {}d", last.vertices.size(), last.indices.size(), last.shapes.size(), last.commands.size());
            }
        }
        ui.separator();

        ui.checkbox("dim secondary text", s.dim_text);
        ui.checkbox("show stress window", s.show_stress);
        ui.checkbox("show settings menu example", s.show_menu);
        {
            const bool was_on = s.show_features;
            ui.checkbox("show feature windows (docking)", s.show_features);
            if (s.show_features && !was_on) { s.dock_pending = true; }
        }
        ui.checkbox("background art + visuals window", s.x.show_visuals);
        s.x.art = s.x.show_visuals;
        ui.checkbox("inputs and plots", s.x.show_inputs);
        ui.checkbox("charts (axes, zoom, area)", s.x.show_charts);
        ui.checkbox("textures (mips, formats, updates)", s.x.show_textures);
        ui.checkbox("scripts: hebrew, arabic, emoji", s.x.show_scripts);
        ui.checkbox("text, log and toasts", s.x.show_textlog);
        ui.checkbox("menu bar, menus and modals", s.x.show_menus);
        ui.checkbox("key bindings and config file", s.x.show_config);
        ui.checkbox("tabs, popups, badges, chips", s.x.d3.show_tabs);
        ui.checkbox("drag and drop, dates and times", s.x.d3.show_dnd);
        ui.checkbox("long lists and tables", s.x.d3.show_lists);
        ui.checkbox("code editor, passwords, masks", s.x.d3.show_editor);
        if (ui.checkbox("dock animation", s.x.dock_anim)) { ui.set_dock_animation(s.x.dock_anim); }
        if (s.show_features) { // dock layout as text: save, rearrange, restore
            if (ui.button("save layout")) { s.saved_layout = ui.dock_save_layout(); }
            ui.same_line();
            if (ui.button("restore layout")) { (void)ui.dock_load_layout(s.saved_layout); }
        }
        {
            static constexpr std::string_view scale_names[] = {"100%", "125%", "150%", "200%"};
            static constexpr float scale_values[] = {1.0f, 1.25f, 1.5f, 2.0f};
            if (ui.combo("ui scale", s.x.scale_combo, scale_names)) {
                s.x.pending_scale = scale_values[s.x.scale_combo];
            }
        }
        ui.spacing();

        ui.slider("speed", s.speed, 0.0f, 4.0f);
        ui.slider("stress widgets", s.stress, 0, 4000);
        ui.spacing();

        ui.progress_bar(s.progress);
        ui.spacing();

        if (ui.button("click me")) { ++s.clicks; }
        ui.same_line();
        if (ui.button("reset")) { s.clicks = 0; s.speed = 1.0f; }
        ui.same_line();
        if (s.dim_text) { ui.text_dim("clicks:"); } else { ui.text("clicks:"); }
        ui.same_line();
        ui.textf("{}", s.clicks);

        ui.spacing();
        ui.text_dim("utf-8: zażółć gęślą jaźń · café · ñandú");
        ui.text_dim("Привет · Γειά · 你好 · こんにちは");
        ui.text("AVATAR  To Wa Ty  LT  Yo. 1/2");
        if (have_heading && have_mono) {
            ui.rich_textf("<c=8a91a6>rich:</c> <f={}>bold</f> <f={}>mono</f> <c=ff6b8a>rose</c> <<tag>", s.font_heading, s.font_mono);
        }
        ui.textf("atlas {}x{}  {} glyphs  {} kern pairs", ui.font().width(), ui.font().height(),
                 ui.font().glyph_count(), ui.font().kerning_pair_count());
    }

    if (auto w = ui.window("theme and custom drawing", {390, 24}, {400, 0}, window_flags::resizable)) {
        ui.text_dim("theme presets (more in the inspector's theme list)");
        if (ui.button("midnight")) { s.pending_theme = 0; }
        ui.same_line();
        if (ui.button("light")) { s.pending_theme = 1; }
        ui.same_line();
        if (ui.button("ocean")) { s.pending_theme = 2; }
        ui.same_line();
        if (ui.button("rose")) { s.pending_theme = 3; }
        ui.spacing();
        (void)ui.input_text("theme file", s.theme_path, "path");
        if (ui.button("save")) {
            s.theme_status = strata::themes::save_file(s.theme_path, ui.theme(), strata::themes::names()[static_cast<std::size_t>(s.theme_combo)])
                                 ? "saved" : "cannot write the file";
        }
        ui.same_line();
        if (ui.button("load")) {
            strata::themes::theme_result r;
            if (strata::themes::load_file(s.theme_path, ui.theme(), &r)) {
                s.theme_status = std::format("loaded: {} keys, {} problems", r.applied, r.unknown + r.invalid);
            } else {
                s.theme_status = "cannot read the file";
            }
        }
        ui.same_line();
        ui.text_dim(s.theme_status);
        ui.separator();

        ui.toggle("toggle switch", s.flag_a);
        ui.toggle("another one", s.flag_b);
        ui.spacing();

        ui.text_dim("custom widget built from ui.draw() + custom_item()");
        meter(ui, s.time);
        ui.spacing();

        ui.text_dim("per-widget style overrides");
        {
            const auto danger = ui.style_overrides({
                override_color(style_color::widget_bg, color::from_hex(0x5a1f2bff)),
                override_color(style_color::widget_hover, color::from_hex(0x7a2a3aff)),
                override_color(style_color::widget_border, color::from_hex(0x9a3a4eff)),
                override_color(style_color::accent, color::from_hex(0xff4d6dff)),
                override_color(style_color::text, color::from_hex(0xfff2f4ff)),
            });
            if (ui.button("delete everything")) { ++s.clicks; }
        }
        ui.same_line();
        {
            const auto pill = ui.style_overrides({override_var(style_var::rounding, 24.0f), override_var(style_var::gradient, 0.0f)});
            (void)ui.button("flat pill");
        }
        ui.same_line();
        (void)ui.button("normal");
    }

    if (auto w = ui.window("inspector", {390, 400}, {400, 0}, window_flags::resizable)) {
        inspector(ui, s);
    }

    if (s.show_menu) { menu_demo(ui, s); }

    if (s.show_stress) {
        if (auto w = ui.window("stress", {860, 70}, {380, 250}, window_flags::resizable)) {
            ui.textf("{} widgets", s.stress);
            std::array<char, 16> label;
            for (strata::i32 i = 0; i < s.stress; ++i) {
                const auto r = std::to_chars(label.data(), label.data() + label.size(), i);
                if (i % 10 != 0) { ui.same_line(); }
                (void)ui.button({label.data(), r.ptr});
            }
        }
    }

    if (auto w = ui.window("data views", {820, 24}, {440, 0}, window_flags::resizable)) {
        data_views(ui, s);
    }

    // click either one: the pressed window is raised above the other
    if (auto w = ui.window("overlap A", {820, 500}, {260, 0}, window_flags::resizable)) {
        ui.text("click me to raise me");
        ui.text_dim("windows keep their stacking");
        ui.toggle("some state", s.flag_a);
    }
    if (auto w = ui.window("overlap B", {900, 550}, {260, 0}, window_flags::resizable)) {
        if (have_mono) {
            const auto mono = ui.with_font(static_cast<font_id>(s.font_mono));
            ui.text("mono: 0O1lI {}[]()<>");
        } else {
            ui.text("second window");
        }
        ui.text_dim("drawn after A, so it starts on top");
        ui.toggle("other state", s.flag_b);
    }

}

} // namespace

// scenes for screenshots and quick looks: a fixed set of windows in a known state
void apply_scene(demo_state& s, const std::string& name)
{
    if (name.empty() || name == "default") { return; }
    if (name == "features" || name == "dockdrag") { // (dockdrag: a tab of it is being dragged, see demo2_script)
        s.show_features = s.features_only = s.dock_pending = true;
    } else if (name == "visuals") {
        s.x.art = s.x.show_visuals = true;
        s.scene_only = true;
    } else if (name == "menu") {
        s.menu_only = s.show_menu = true;
    } else if (name == "inputs" || name == "multiselect") {
        s.x.show_inputs = s.scene_only = true;
    } else if (name == "scripts") {
        s.x.show_scripts = s.scene_only = true;
    } else if (name == "textures") {
        s.x.show_textures = s.scene_only = true;
    } else if (name == "charts") {
        s.x.show_charts = s.scene_only = true;
    } else if (name == "textlog") {
        s.x.show_textlog = s.scene_only = true;
    } else if (name == "tabs") {
        s.x.d3.show_tabs = s.scene_only = true;
    } else if (name == "dnd") {
        s.x.d3.show_dnd = s.scene_only = true;
    } else if (name == "lists") {
        s.x.d3.show_lists = s.scene_only = true;
    } else if (name == "editor") {
        s.x.d3.show_editor = s.scene_only = true;
    } else if (name == "config" || name == "palette") {
        s.x.show_config = s.scene_only = true;
    } else if (name == "menus" || name == "context" || name == "modal" || name == "dialog" || name == "toasts") {
        s.x.show_menus = s.scene_only = true;
    } else if (name == "icons") {
        s.x.d4.show_icons = s.scene_only = true;
    } else if (name == "bigtree") {
        s.x.d4.show_bigtree = s.scene_only = true;
    } else if (name == "rows") {
        s.x.d4.show_rows = s.scene_only = true;
    } else if (name == "app") {
        s.x.d4.show_app = s.scene_only = true;
    } else {
        std::fprintf(stderr, "[sandbox] unknown scene '%s'\n", name.c_str());
    }
}

// failed comparison images: <artifacts>/<golden name>.<kind>.png (folder created on demand)
std::string artifact_path(const options& opt, const char* kind)
{
    const std::filesystem::path dir = opt.artifacts.empty() ? std::filesystem::temp_directory_path() / "strata_tests"
                                                            : std::filesystem::path{std::u8string{opt.artifacts.begin(), opt.artifacts.end()}};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path stem = std::filesystem::path{std::u8string{opt.golden.begin(), opt.golden.end()}}.stem();
    return (dir / (stem.string() + "." + kind + ".png")).string();
}

// writes and / or compares the captured frame; returns the process exit code
int finish_shot(gfx_host& host, const options& opt)
{
    std::vector<strata::u8> px;
    strata::u32 w = 0, h = 0;
    if (!host.take_capture(px, w, h)) {
        std::fprintf(stderr, "[shot] the frame could not be read back\n");
        return 2;
    }
    for (std::size_t i = 3; i < px.size(); i += 4) { px[i] = 255; }

    if (!opt.shot.empty()) {
        if (!imgio::write_png(opt.shot, px, w, h)) {
            std::fprintf(stderr, "[shot] cannot write %s\n", opt.shot.c_str());
            return 2;
        }
        std::fprintf(stderr, "[shot] %ux%u -> %s\n", w, h, opt.shot.c_str());
    }
    if (opt.golden.empty()) { return 0; }

    if (opt.update_golden) {
        if (!imgio::write_png(opt.golden, px, w, h)) {
            std::fprintf(stderr, "[shot] cannot write %s\n", opt.golden.c_str());
            return 2;
        }
        std::fprintf(stderr, "[shot] golden updated: %s (%ux%u)\n", opt.golden.c_str(), w, h);
        return 0;
    }
    std::vector<strata::u8> ref;
    strata::u32 rw = 0, rh = 0;
    if (!imgio::read_png(opt.golden, ref, rw, rh)) {
        std::fprintf(stderr, "[shot] no golden image %s - create it with --update-golden\n", opt.golden.c_str());
        return 3;
    }
    if (rw != w || rh != h) {
        std::fprintf(stderr, "[shot] size differs: %ux%u, golden is %ux%u\n", w, h, rw, rh);
        (void)imgio::write_png(artifact_path(opt, "actual"), px, w, h);
        return 1;
    }
    const imgio::compare_result r = imgio::compare(px, ref, w, h, opt.golden_tolerance);
    std::fprintf(stderr, "[shot] %s: %.4f%% of the pixels differ (largest channel difference %u; allowed %.4f%% beyond %u)\n",
                 opt.golden.c_str(), r.fraction() * 100.0, r.max_difference, opt.golden_allowed, opt.golden_tolerance);
    if (r.fraction() * 100.0 > opt.golden_allowed) {
        (void)imgio::write_png(artifact_path(opt, "actual"), px, w, h);
        (void)imgio::write_png(artifact_path(opt, "diff"), imgio::diff_image(px, ref, w, h), w, h);
        std::fprintf(stderr, "[shot] MISMATCH - see %s and the .diff image beside it\n", artifact_path(opt, "actual").c_str());
        return 1;
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const options opt = parse_args();
    if (opt.show_help || !opt.bad_flag.empty()) {
        std::wstring text = usage_text;
        if (!opt.bad_flag.empty()) {
            text = L"unknown option: " + opt.bad_flag + L"\n\n" + text;
        }
        std::fputws(text.c_str(), stderr);
        ::MessageBoxW(nullptr, text.c_str(), L"strata sandbox", opt.bad_flag.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return opt.bad_flag.empty() ? 0 : 2;
    }

    // d3d debug layer output and the frame report go to --log <file>, or to the launching terminal
    FILE* unused{};
    if (opt.log_path != nullptr) {
        (void)_wfreopen_s(&unused, opt.log_path, L"w", stderr);
    } else if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        (void)freopen_s(&unused, "CONOUT$", "w", stderr);
    }

    if (opt.selftest) {
        return run_selftest();
    }

#if STRATA_HAS_OPENXR
    if (opt.use_xr) {
        return run_xr_sandbox(opt.face, opt.size, opt.theme, static_cast<strata::u32>(opt.width),
                              static_cast<strata::u32>(opt.height));
    }
#else
    if (opt.use_xr) {
        ::MessageBoxW(nullptr, L"this build has no openxr backend (configure with -DSTRATA_BUILD_OPENXR=ON)",
                      L"strata sandbox", MB_ICONERROR);
        return 1;
    }
#endif

#if STRATA_HAS_OPENVR
    if (opt.use_openvr) {
        return run_openvr_sandbox(opt.face, opt.size, opt.theme, static_cast<strata::u32>(opt.width),
                                  static_cast<strata::u32>(opt.height));
    }
#else
    if (opt.use_openvr) {
        ::MessageBoxW(nullptr, L"this build has no openvr backend (configure with -DSTRATA_BUILD_OPENVR=ON)",
                      L"strata sandbox", MB_ICONERROR);
        return 1;
    }
#endif

    std::unique_ptr<gfx_host> host;
#if STRATA_HAS_DX12
    if (opt.use_dx12) { host = make_d3d12_host(); }
#endif
#if STRATA_HAS_DX11
    if (host == nullptr) { host = make_d3d11_host(); }
#endif
    if (host == nullptr) {
        ::MessageBoxW(nullptr, L"no graphics backend was compiled in", L"strata sandbox", MB_ICONERROR);
        return 1;
    }

    std::vector<strata::codepoint_range> ranges(strata::glyph_ranges::default_set.begin(),
                                                strata::glyph_ranges::default_set.end());
    // rtl scripts (with Arabic joined forms), symbols and emoji (from the fallback faces below)
    for (const strata::codepoint_range& r : {strata::glyph_ranges::hebrew, strata::glyph_ranges::arabic, strata::glyph_ranges::arabic_forms_a,
                                              strata::glyph_ranges::arabic_forms_b, strata::glyph_ranges::symbols, strata::glyph_ranges::emoji}) {
        ranges.push_back(r);
    }
    static constexpr std::array<std::string_view, 2> symbol_faces = {"Segoe UI Emoji", "Segoe UI Symbol"};
    if (opt.cjk) {
        ranges.push_back(strata::glyph_ranges::cjk_symbols);
        ranges.push_back(strata::glyph_ranges::hiragana);
        ranges.push_back(strata::glyph_ranges::katakana);
        ranges.push_back(strata::glyph_ranges::cjk_unified);
        ranges.push_back(strata::glyph_ranges::fullwidth_forms);
    }
    strata::context_config config;
    config.font.face         = opt.face;
    config.font.file         = opt.font_file;
    config.font.pixel_height = opt.size;
    config.font.ranges       = ranges;
    config.font.kerning      = opt.kerning;
    config.font.fallback_faces = symbol_faces;

    const std::array icon_ranges{strata::glyph_ranges::private_use};

    // fonts: 0 primary, then mono / heading / icons in that order when enabled
    int font_mono = -1;
    int font_heading = -1;
    int font_icons = -1;
    std::vector<strata::font_config> extras;
    const auto build_extras = [&](const std::string& icon_face) {
        extras.clear();
        font_mono = font_heading = font_icons = -1;
        int next = 1;
        if (opt.extra_fonts) {
            strata::font_config mono;
            mono.pixel_height = opt.mono_size;
            (looks_like_path(opt.mono) ? mono.file : mono.face) = opt.mono;
            extras.push_back(mono);
            font_mono = next++;

            strata::font_config heading;
            heading.pixel_height = opt.heading_size;
            heading.bold         = true;
            (looks_like_path(opt.heading) ? heading.file : heading.face) = opt.heading;
            extras.push_back(heading);
            font_heading = next++;
        }
        if (opt.icons && !icon_face.empty()) {
            strata::font_config icons;
            icons.pixel_height = opt.icon_size;
            icons.kerning      = false;
            icons.ranges       = icon_ranges;
            if (looks_like_path(icon_face)) { icons.file = icon_face; }
            else                            { icons.face = icon_face; icons.exact_face = true; }
            extras.push_back(icons);
            font_icons = next++;
        }
        config.extra_fonts = extras;
    };

    // icon font candidates in order; the last (empty) means none.
    // strings must outlive create() (font_config holds views).
    std::vector<std::string> icon_candidates;
    if (opt.icons) {
        icon_candidates.push_back(opt.icon_face);
        if (!looks_like_path(opt.icon_face) && opt.icon_face != "Segoe Fluent Icons") {
            icon_candidates.emplace_back("Segoe Fluent Icons");
        }
    }
    icon_candidates.emplace_back();

    const double font_start = seconds_now();
    std::expected<strata::context, strata::font_error> created = std::unexpected{strata::font_error::no_fonts};
    for (const std::string& candidate : icon_candidates) {
        build_extras(candidate);
        created = strata::context::create(config);
        if (created) { break; }
        if (!candidate.empty()) {
            std::fprintf(stderr, "[sandbox] icon font '%s' unavailable (font_error %d)\n", candidate.c_str(),
                         static_cast<int>(created.error()));
        }
    }
    if (!created) {
        ::MessageBoxW(nullptr, L"failed to build the font atlas (check --font / --face / --mono / --heading)",
                      L"strata sandbox", MB_ICONERROR);
        return 1;
    }
    std::fprintf(stderr, "[sandbox] %zu font(s), atlas %ux%u built in %.0f ms\n", created->font().font_count(),
                 created->font().width(), created->font().height(), (seconds_now() - font_start) * 1000.0);
    strata::context ui = std::move(*created);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = instance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"strata_sandbox";
    ::RegisterClassExW(&wc);

    RECT rc{0, 0, opt.width, opt.height};
    ::AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"strata sandbox", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                  nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }

    app self;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&self));
    self.platform.attach(hwnd);

    // ui scale: --scale, else the monitor dpi (fixed 1.0 for screenshots, which must be machine-independent)
    const bool shot_mode = !opt.shot.empty() || !opt.golden.empty();
    const float start_scale = opt.scale > 0.0f ? opt.scale : (shot_mode ? 1.0f : self.platform.dpi_scale());
    if (start_scale != 1.0f) {
        if (const auto r = ui.set_scale(start_scale); !r) {
            std::fprintf(stderr, "[sandbox] set_scale(%.2f) failed (font_error %d)\n", start_scale, static_cast<int>(r.error()));
        }
    }
    {   // --width / --height are logical: the window is scaled
        RECT want{0, 0, static_cast<LONG>(std::lround(opt.width * ui.scale())), static_cast<LONG>(std::lround(opt.height * ui.scale()))};
        ::AdjustWindowRectEx(&want, WS_OVERLAPPEDWINDOW, FALSE, 0);
        ::SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    ::GetClientRect(hwnd, &client);
    if (!host->init(hwnd, static_cast<strata::u32>(client.right), static_cast<strata::u32>(client.bottom), opt.vsync, ui.font())) {
        ::MessageBoxW(nullptr, L"failed to initialise the graphics backend", L"strata sandbox", MB_ICONERROR);
        return 1;
    }
    self.host = host.get();

    // the gpu has its own copy of the atlas now; wipe the cpu bitmap
    ui.release_font_pixels();

    ::ShowWindow(hwnd, show);
    ::UpdateWindow(hwnd);

    demo_state state;
    state.x.scene         = opt.scene;
    state.x.d4.icon_page  = opt.icon_page;
    state.x.deterministic = shot_mode;
    state.x.scale_combo   = ui.scale() >= 1.75f ? 3 : (ui.scale() >= 1.375f ? 2 : (ui.scale() >= 1.125f ? 1 : 0));
    state.font_mono    = font_mono;
    state.font_heading = font_heading;
    state.font_icons   = font_icons;
    ui.set_clipboard(self.platform.clipboard());
    state.menu_only = opt.menu_only;
    state.show_menu = opt.menu_only;
    {
        const std::vector<strata::u8> picture = make_demo_image(160, 160);
        state.image_tex = host->create_texture(160, 160, picture);
        std::fprintf(stderr, "[sandbox] demo texture id %u\n", state.image_tex);
    }
    demo2_textures_create(*host, state.x);
    state.show_features = opt.features;
    state.features_only = opt.features;
    state.dock_pending  = opt.features;
    apply_scene(state, opt.scene);
    init_rows(state);
    state.scroll_speed = opt.scroll_speed;
    apply_theme(ui, state, opt.theme);
    state.x.dock_anim = state.x.dock_anim && opt.shot.empty() && opt.golden.empty(); // (screenshots are taken of the settled layout)
    ui.set_dock_animation(state.x.dock_anim);
    if (!opt.theme_file.empty()) {
        strata::themes::theme_result r;
        if (!strata::themes::load_file(opt.theme_file, ui.theme(), &r)) {
            std::fprintf(stderr, "[sandbox] cannot read the theme file %s\n", opt.theme_file.c_str());
        } else if (!r.ok()) {
            std::fprintf(stderr, "[sandbox] theme file: %zu unknown / %zu invalid entries (first at line %zu)\n", r.unknown, r.invalid,
                         r.first_problem_line);
        }
    }
    state.stress = opt.stress;
    strata::draw_data last_data{};

    strata::i32 frame = 0;
    strata::i32 frames_idled = 0; // --idle: frames where nothing changed, so nothing was rendered or presented
    strata::u32 worst_collisions = 0; // debug builds: the most duplicate widget ids seen in one frame
    std::string worst_collision_label;
    strata::i32 shot_counter = 0;
    demo2_sim   sim;
    int exit_code     = 0;
    double last_time  = seconds_now();
    const double loop_start = last_time;
    bool quit         = false;

    // frame cap: some drivers ignore vsync, which would run the ui at thousands of frames per second
    double refresh = 60.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
        refresh = static_cast<double>(mode.dmDisplayFrequency);
    }
    const double cap = opt.fps_cap >= 0 ? static_cast<double>(opt.fps_cap) : (opt.vsync ? refresh * 1.05 : 0.0);
    const frame_limiter limiter;
    double next_frame = last_time;

    while (!quit) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (quit) { break; }

        if (self.minimized) {
            ::Sleep(50);
            continue;
        }

        if (opt.rescale > 0.0f && shot_counter == 5) { state.x.pending_scale = opt.rescale; }
        // a change of the dpi scale (the window moved to another monitor) or of the ui scale combo
        {
            float want = 0.0f;
            bool  resize_window = false;
            if (self.pending_dpi > 0.0f) {
                if (opt.scale <= 0.0f) { want = self.pending_dpi; }
                self.pending_dpi = 0.0f;
            }
            if (state.x.pending_scale > 0.0f) {
                // the "ui scale" combo keeps the *logical* client size, so the os window resizes with the
                // scale (a sandbox convenience for stable shots; see --scene app's +/- for in-place scaling)
                want = state.x.pending_scale;
                state.x.pending_scale = 0.0f;
                resize_window = true;
            }
            if (state.x.d4.pending_scale_percent > 0) {
                // in-place scaling: the window keeps its size and the ui grows / shrinks, as a "ui scale" setting should
                want = static_cast<float>(state.x.d4.pending_scale_percent) / 100.0f;
                state.x.d4.pending_scale_percent = 0;
            }
            if (want > 0.0f && want != ui.scale()) {
                const float old_scale = ui.scale();
                if (const auto r = ui.set_scale(want); r) {
                    (void)host->update_atlas(ui.font());
                    ui.release_font_pixels();
                    if (resize_window) { // keep the logical size of the client area
                        RECT cr{};
                        ::GetClientRect(hwnd, &cr);
                        RECT nr{0, 0, static_cast<LONG>(std::lround(cr.right / old_scale * ui.scale())),
                                static_cast<LONG>(std::lround(cr.bottom / old_scale * ui.scale()))};
                        ::AdjustWindowRectEx(&nr, WS_OVERLAPPEDWINDOW, FALSE, 0);
                        ::SetWindowPos(hwnd, nullptr, 0, 0, nr.right - nr.left, nr.bottom - nr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                } else {
                    std::fprintf(stderr, "[sandbox] set_scale(%.2f) failed (font_error %d)\n", want, static_cast<int>(r.error()));
                }
            }
        }

        const double now = seconds_now();
        const double dt  = shot_mode ? 1.0 / 60.0 : now - last_time;
        last_time = now;
        state.fps = shot_mode ? 60.0 : (state.fps == 0.0 ? 1.0 / dt : state.fps * 0.95 + (1.0 / dt) * 0.05);
        state.progress += static_cast<float>(dt) * 0.25f * state.speed;
        if (state.progress > 1.0f) { state.progress -= 1.0f; }

        state.time += static_cast<float>(dt) * state.speed;
        if (state.pending_theme >= 0) {
            apply_theme(ui, state, state.pending_theme);
            state.pending_theme = -1;
        }

        const double ui_start = seconds_now();
        strata::input_state input = self.platform.new_frame();
        if (shot_mode) { // nothing from the real world may leak into a screenshot
            input.delta_time = 1.0f / 60.0f;
            demo2_script(state.x, shot_counter, sim);
            input.mouse_pos  = sim.pos;
            input.mouse_down = {sim.down[0], sim.down[1], sim.down[2]};
            input.key_count  = 0;
            input.typed_len  = 0;
            input.wheel      = 0.0f;
            input.caret_blink_time  = strata::input_state{}.caret_blink_time; // (not the user's system settings)
            input.double_click_time = strata::input_state{}.double_click_time;
        }
        ui.begin_frame(std::move(input));
        build_ui(ui, state, *host, last_data);
        if (opt.metrics) { // strata inspecting itself; last, so it sees every command
            ui.debug_draw_list_window(state.show_cmds);
            ui.debug_metrics_window(state.show_metrics);
        }
        ui.end_frame();
        // debug builds count shared ids; report the worst frame once at the end
        if (ui.stats().id_collisions > worst_collisions) {
            worst_collisions = ui.stats().id_collisions;
            worst_collision_label.assign(ui.id_collision_label());
        }
        demo2_textures_update(*host, state.x); // (before the frame that shows it is rendered)
        self.wants_text = ui.want_text_input() || ui.any_popup_open();
        self.platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height());
        self.platform.set_cursor(ui.cursor());
        const double ui_ms = (seconds_now() - ui_start) * 1000.0;
        state.ui_ms = state.ui_ms == 0.0 ? ui_ms : state.ui_ms * 0.9 + ui_ms * 0.1;

        last_data = ui.render_data();
        const bool shot_frame = shot_mode && ++shot_counter >= opt.shot_frames;
        if (shot_frame) { host->request_capture(); }
        // --idle: an untouched ui repeats its geometry and the window is ours, so skip drawing and presenting (the loop
        // below sleeps until input or ui.next_wake_seconds()). never while capturing a screenshot.
        const bool skip = opt.idle && !shot_mode && !shot_frame && ui.frame_unchanged();
        if (skip) {
            ++frames_idled;
        } else {
            host->render(last_data, {static_cast<strata::u8>(state.clear_r), static_cast<strata::u8>(state.clear_g),
                                     static_cast<strata::u8>(state.clear_b), 255});
        }
        if (shot_frame) {
            exit_code = finish_shot(*host, opt);
            ::DestroyWindow(hwnd);
        }

        if (opt.max_frames > 0 && ++frame >= opt.max_frames) {
            std::fprintf(stderr, "[sandbox] %d frames ok (%s), %.0f fps, ui %.3f ms, ui time %.2f s vs wall %.2f s\n", frame,
                         host->name(), state.fps, state.ui_ms, ui.time(), seconds_now() - loop_start);
            if (opt.idle) {
                const double pct = 100.0 * static_cast<double>(frames_idled) / static_cast<double>(frame > 0 ? frame : 1);
                std::fprintf(stderr, "[sandbox] %d of %d frames idled (%.0f %%): nothing changed, nothing drawn\n",
                             frames_idled, frame, pct);
            }
            if (worst_collisions > 0) { // debug builds only; release never checks
                std::fprintf(stderr, "[sandbox] %u duplicate widget id(s) in one frame, first \"%s\"\n",
                             worst_collisions, worst_collision_label.c_str());
            }
            ::DestroyWindow(hwnd);
        }

        if (opt.idle && !shot_mode) {
            // sleep until a message or the ui's next deadline (caret blink, tooltip, toast). with --frames the wait
            // is at most a frame so the run still ends on time
            double wait = ui.next_wake_seconds();
            if (opt.max_frames > 0) { wait = std::min(wait, 1.0 / refresh); }
            if (wait > 0.0) {
                const DWORD ms = wait >= 3600.0 ? INFINITE : static_cast<DWORD>(std::ceil(wait * 1000.0));
                ::MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                next_frame = seconds_now();
                continue;
            }
        }
        if (cap > 0.0) {
            const double period = 1.0 / cap;
            next_frame += period;
            const double t = seconds_now();
            if (next_frame < t - period) { next_frame = t; } // fell behind: do not try to catch up
            limiter.wait_until(next_frame);
        }
    }

    self.host = nullptr;
    return exit_code;
}

#include "gfx_host.hpp"
#include "imgio.hpp"
#include "openvr_main.hpp"
#include "selftest.hpp"
#include "showcase/app.hpp"
#include "xr_main.hpp"

#include <strata/platform/win32.hpp>
#include <strata/strata.hpp>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct options {
    enum class api_kind { auto_pick, dx11, dx12, opengl, vulkan, software };
    api_kind       api        = api_kind::auto_pick;
    bool           use_xr     = false;
    bool           use_openvr = false;
    bool           vsync      = true;
    int            fps_cap    = -1;
    strata::i32    width      = 1360;
    strata::i32    height     = 820;
    strata::i32    max_frames = 0;
    int            theme      = 0;
    std::string    theme_file;
    float          scale      = 0.0f;
    float          rescale    = 0.0f;
    std::string    scene{"studio"};
    std::string    diff_a, diff_b, diff_out;
    std::string    record;
    std::string    replay;
    std::string    shot;
    std::string    golden;
    std::string    artifacts;
    bool           update_golden    = false;
    strata::u32    golden_tolerance = 6;
    double         golden_allowed   = 0.3;
    strata::i32    shot_frames      = 24;
    const wchar_t* log_path         = nullptr;
    bool           metrics          = false;
    bool           idle             = false;

    std::string    font_file;
    std::string    face         = "Segoe UI";
    float          size         = 14.0f;
    std::string    mono         = "Consolas";
    float          mono_size    = 13.0f;
    std::string    heading      = "Segoe UI";
    float          heading_size = 20.0f;
    bool           extra_fonts  = true;
    bool           cjk          = false;
    bool           kerning      = true;

    bool           selftest     = false;
    bool           show_help    = false;
    std::wstring   bad_flag;
};

constexpr const wchar_t* usage_text =
    L"strata_sandbox [options]\n\n"
    L"  --dx11 | --dx12 | --gl | --vk   graphics backend (default dx11; also --opengl / --vulkan)\n"
    L"  --software                      CPU software rasteriser (no GPU required)\n"
    L"  --xr | --openvr                 standalone VR overlay loop (when built)\n"
    L"  --vsync | --novsync             present interval (default vsync)\n"
    L"  --fps N                         frame cap; 0 = unlimited\n"
    L"  --width N --height N            logical client size (default 1360x820)\n"
    L"  --frames N                      exit after N frames\n"
    L"  --scene NAME                    workspace (studio, telemetry, authoring, visuals, productivity,\n"
    L"                                  gallery, palette) or module id (explorer, inspector, charts,\n"
    L"                                  editor, nodes, sequencer, data, visuals, pickers, typography,\n"
    L"                                  overlays, console)\n"
    L"  --theme N|NAME                  built-in theme by index or name (midnight, light, dracula, nord, glass, ...)\n"
    L"  --theme-file FILE               apply a theme file on top\n"
    L"  --scale F                       UI scale (default: monitor DPI scale)\n"
    L"  --shot FILE                     render --shot-frames fixed steps, save PNG and exit\n"
    L"  --golden FILE [--update-golden] compare against golden PNG (0 = match)\n"
    L"  --diff A.png --against B.png [--diff-out OUT.png]\n"
    L"  --record FILE | --replay FILE   record or replay frame inputs\n"
    L"  --selftest                      run headless UI logic self-test suite and exit\n"
    L"  --log FILE                      redirect stderr to FILE\n"
    L"  --metrics                       show frame metrics and draw-list windows\n"
    L"  --idle                          skip rendering while context::frame_unchanged()\n"
    L"  --help                          this text\n";

[[nodiscard]] std::string to_utf8(const wchar_t* w)
{
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n - 1, 0)), '\0');
    if (n > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    }
    return out;
}

[[nodiscard]] bool looks_like_path(std::string_view s) noexcept
{
    return s.find_first_of("\\/") != std::string_view::npos || s.find('.') != std::string_view::npos;
}

struct host_window {
    strata::win32_platform platform;
    gfx_host*              host{};
    bool                   minimized{};
    bool                   wants_text{};
    float                  pending_dpi{};
};

[[nodiscard]] options parse_args()
{
    options opt;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) { return opt; }

    const auto value   = [&](int& i) -> const wchar_t* { return i + 1 < argc ? argv[++i] : L""; };
    const auto integer = [&](int& i) { return static_cast<strata::i32>(std::wcstol(value(i), nullptr, 10)); };
    const auto real    = [&](int& i) { return static_cast<float>(std::wcstod(value(i), nullptr)); };

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--dx12")                         { opt.api = options::api_kind::dx12; }
        else if (a == L"--dx11")                    { opt.api = options::api_kind::dx11; }
        else if (a == L"--gl" || a == L"--opengl")  { opt.api = options::api_kind::opengl; }
        else if (a == L"--vk" || a == L"--vulkan")  { opt.api = options::api_kind::vulkan; }
        else if (a == L"--software")                { opt.api = options::api_kind::software; }
        else if (a == L"--xr")                      { opt.use_xr = true; }
        else if (a == L"--openvr")                  { opt.use_openvr = true; }
        else if (a == L"--vsync")                   { opt.vsync = true; }
        else if (a == L"--novsync")                 { opt.vsync = false; }
        else if (a == L"--fps")                     { opt.fps_cap = std::max(integer(i), 0); }
        else if (a == L"--width")                   { opt.width = std::max(integer(i), 320); }
        else if (a == L"--height")                  { opt.height = std::max(integer(i), 240); }
        else if (a == L"--frames")                  { opt.max_frames = integer(i); }
        else if (a == L"--theme") {
            const wchar_t* v = value(i);
            if (*v >= L'0' && *v <= L'9') {
                opt.theme = static_cast<int>(std::wcstol(v, nullptr, 10));
            } else {
                const std::string name = to_utf8(v);
                const auto names = strata::themes::names();
                for (std::size_t k = 0; k < names.size(); ++k) {
                    if (names[k] == name) { opt.theme = static_cast<int>(k); }
                }
            }
        }
        else if (a == L"--theme-file")              { opt.theme_file = to_utf8(value(i)); }
        else if (a == L"--scale")                   { opt.scale = std::max(real(i), 0.0f); }
        else if (a == L"--rescale")                 { opt.rescale = std::max(real(i), 0.0f); }
        else if (a == L"--scene")                   { opt.scene = to_utf8(value(i)); }
        else if (a == L"--shot")                    { opt.shot = to_utf8(value(i)); }
        else if (a == L"--record")                  { opt.record = to_utf8(value(i)); }
        else if (a == L"--diff")                    { opt.diff_a = to_utf8(value(i)); }
        else if (a == L"--against")                 { opt.diff_b = to_utf8(value(i)); }
        else if (a == L"--diff-out")                { opt.diff_out = to_utf8(value(i)); }
        else if (a == L"--replay")                  { opt.replay = to_utf8(value(i)); }
        else if (a == L"--golden")                  { opt.golden = to_utf8(value(i)); }
        else if (a == L"--update-golden")           { opt.update_golden = true; }
        else if (a == L"--artifacts")               { opt.artifacts = to_utf8(value(i)); }
        else if (a == L"--golden-tolerance")        { opt.golden_tolerance = static_cast<strata::u32>(std::clamp(integer(i), 0, 255)); }
        else if (a == L"--golden-allowed")          { opt.golden_allowed = std::clamp(static_cast<double>(real(i)), 0.0, 100.0); }
        else if (a == L"--shot-frames")             { opt.shot_frames = std::max(integer(i), 1); }
        else if (a == L"--selftest")                { opt.selftest = true; }
        else if (a == L"--log")                     { opt.log_path = _wcsdup(value(i)); }
        else if (a == L"--metrics")                 { opt.metrics = true; }
        else if (a == L"--idle")                    { opt.idle = true; }
        else if (a == L"--font")                    { opt.font_file = to_utf8(value(i)); }
        else if (a == L"--face")                    { opt.face = to_utf8(value(i)); }
        else if (a == L"--size")                    { opt.size = std::max(real(i), 6.0f); }
        else if (a == L"--mono")                    { opt.mono = to_utf8(value(i)); }
        else if (a == L"--mono-size")               { opt.mono_size = std::max(real(i), 6.0f); }
        else if (a == L"--heading")                 { opt.heading = to_utf8(value(i)); }
        else if (a == L"--heading-size")            { opt.heading_size = std::max(real(i), 6.0f); }
        else if (a == L"--no-extra-fonts")          { opt.extra_fonts = false; }
        else if (a == L"--cjk")                     { opt.cjk = true; }
        else if (a == L"--no-kern")                 { opt.kerning = false; }
        else if (a == L"--help" || a == L"-h" || a == L"/?") { opt.show_help = true; }
        else                                        { opt.bad_flag = a; }
    }
    ::LocalFree(argv);
    return opt;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* self = reinterpret_cast<host_window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        (void)self->platform.handle_message(hwnd, msg, wparam, static_cast<std::intptr_t>(lparam));
        if (strata::win32_platform::swallows(msg)) { return 0; }
    }

    switch (msg) {
    case WM_SIZE:
        if (self != nullptr && self->host != nullptr) {
            self->minimized = wparam == SIZE_MINIMIZED;
            if (!self->minimized) { self->host->resize(LOWORD(lparam), HIWORD(lparam)); }
        }
        return 0;
    case WM_DPICHANGED:
        if (self != nullptr) {
            self->pending_dpi = static_cast<float>(HIWORD(wparam)) / 96.0f;
            const RECT* r = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && (self == nullptr || !self->wants_text)) { ::DestroyWindow(hwnd); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT && self != nullptr && self->platform.apply_cursor()) { return TRUE; }
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

    void wait_until(double target) const noexcept
    {
        for (;;) {
            const double remaining = target - seconds_now();
            if (remaining <= 0.0) { return; }
            if (timer_ != nullptr && remaining > 0.002) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((remaining - 0.0015) * 1.0e7);
                if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    ::WaitForSingleObject(timer_, INFINITE);
                    continue;
                }
            }
            if (remaining > 0.002) { ::Sleep(1); } else { ::YieldProcessor(); }
        }
    }

private:
    HANDLE timer_{};
};

std::string artifact_path(const options& opt, const char* kind)
{
    const std::filesystem::path dir = opt.artifacts.empty()
        ? std::filesystem::temp_directory_path() / "strata_tests"
        : std::filesystem::path{std::u8string{opt.artifacts.begin(), opt.artifacts.end()}};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path stem = std::filesystem::path{std::u8string{opt.golden.begin(), opt.golden.end()}}.stem();
    return (dir / (stem.string() + "." + kind + ".png")).string();
}

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
    std::fprintf(stderr, "[shot] %s: %.4f%% of the pixels differ (max %u; allowed %.4f%% beyond %u)\n",
                 opt.golden.c_str(), r.fraction() * 100.0, r.max_difference, opt.golden_allowed, opt.golden_tolerance);
    if (r.fraction() * 100.0 > opt.golden_allowed) {
        (void)imgio::write_png(artifact_path(opt, "actual"), px, w, h);
        (void)imgio::write_png(artifact_path(opt, "diff"), imgio::diff_image(px, ref, w, h), w, h);
        return 1;
    }
    return 0;
}

int run_diff(const options& opt)
{
    std::vector<strata::u8> a, b;
    strata::u32 aw = 0, ah = 0, bw = 0, bh = 0;
    if (!imgio::read_png(opt.diff_a, a, aw, ah) || !imgio::read_png(opt.diff_b, b, bw, bh)) {
        std::fprintf(stderr, "[diff] cannot read %s or %s\n", opt.diff_a.c_str(), opt.diff_b.c_str());
        return 2;
    }
    if (aw != bw || ah != bh) {
        std::fprintf(stderr, "[diff] sizes differ: %ux%u against %ux%u\n", aw, ah, bw, bh);
        return 1;
    }
    const imgio::compare_result r = imgio::compare(a, b, aw, ah, opt.golden_tolerance);
    std::fprintf(stderr, "[diff] %.3f%% differ beyond %u (max %u, allowed %.3f%%)\n",
                 r.fraction() * 100.0, opt.golden_tolerance, r.max_difference, opt.golden_allowed);
    if (!opt.diff_out.empty() && !imgio::write_png(opt.diff_out, imgio::diff_image(a, b, aw, ah), aw, ah)) {
        std::fprintf(stderr, "[diff] cannot write %s\n", opt.diff_out.c_str());
        return 2;
    }
    return r.fraction() * 100.0 > opt.golden_allowed ? 1 : 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const options opt = parse_args();
    if (opt.show_help || !opt.bad_flag.empty()) {
        std::wstring text = usage_text;
        if (!opt.bad_flag.empty()) { text = L"unknown option: " + opt.bad_flag + L"\n\n" + text; }
        std::fputws(text.c_str(), stderr);
        ::MessageBoxW(nullptr, text.c_str(), L"strata sandbox", opt.bad_flag.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
        return opt.bad_flag.empty() ? 0 : 2;
    }

    FILE* unused{};
    if (opt.log_path != nullptr) {
        (void)_wfreopen_s(&unused, opt.log_path, L"w", stderr);
    } else if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        (void)freopen_s(&unused, "CONOUT$", "w", stderr);
    }

    if (!opt.diff_a.empty()) { return run_diff(opt); }
    if (opt.selftest)        { return run_selftest(); }

#if STRATA_HAS_OPENXR
    if (opt.use_xr) {
        return run_xr_sandbox(opt.face, opt.size, opt.theme, static_cast<strata::u32>(opt.width),
                              static_cast<strata::u32>(opt.height));
    }
#else
    if (opt.use_xr) {
        ::MessageBoxW(nullptr, L"this build has no openxr backend", L"strata sandbox", MB_ICONERROR);
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
        ::MessageBoxW(nullptr, L"this build has no openvr backend", L"strata sandbox", MB_ICONERROR);
        return 1;
    }
#endif

    std::unique_ptr<gfx_host> host;
    const auto fail_backend = [&](const wchar_t* msg) -> int {
        ::MessageBoxW(nullptr, msg, L"strata sandbox", MB_ICONERROR);
        return 1;
    };
    switch (opt.api) {
    case options::api_kind::software: host = make_software_host(); break;
    case options::api_kind::dx12:
#if STRATA_HAS_DX12
        host = make_d3d12_host(); break;
#else
        return fail_backend(L"Direct3D 12 backend not enabled");
#endif
    case options::api_kind::opengl:
#if STRATA_HAS_OPENGL
        host = make_opengl_host(); break;
#else
        return fail_backend(L"OpenGL backend not enabled");
#endif
    case options::api_kind::vulkan:
#if STRATA_HAS_VULKAN
        host = make_vulkan_host(); break;
#else
        return fail_backend(L"Vulkan backend not enabled");
#endif
    case options::api_kind::dx11:
#if STRATA_HAS_DX11
        host = make_d3d11_host(); break;
#else
        return fail_backend(L"Direct3D 11 backend not enabled");
#endif
    case options::api_kind::auto_pick:
#if STRATA_HAS_DX11
        host = make_d3d11_host();
#elif STRATA_HAS_DX12
        host = make_d3d12_host();
#elif STRATA_HAS_VULKAN
        host = make_vulkan_host();
#elif STRATA_HAS_OPENGL
        host = make_opengl_host();
#else
        host = make_software_host();
#endif
        break;
    }
    if (host == nullptr) { return fail_backend(L"no graphics backend compiled in"); }

    std::vector<strata::codepoint_range> ranges(strata::glyph_ranges::default_set.begin(),
                                                strata::glyph_ranges::default_set.end());
    for (const strata::codepoint_range& r : {strata::glyph_ranges::hebrew, strata::glyph_ranges::arabic,
                                             strata::glyph_ranges::arabic_forms_a, strata::glyph_ranges::arabic_forms_b,
                                             strata::glyph_ranges::symbols, strata::glyph_ranges::emoji}) {
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

    strata::context_config cfg;
    cfg.font.face           = opt.face;
    cfg.font.file           = opt.font_file;
    cfg.font.pixel_height   = opt.size;
    cfg.font.ranges         = ranges;
    cfg.font.kerning        = opt.kerning;
    cfg.font.fallback_faces = symbol_faces;

    strata::font_id font_mono    = 0;
    strata::font_id font_heading = 0;
    std::vector<strata::font_config> extras;
    if (opt.extra_fonts) {
        strata::font_config mono;
        mono.pixel_height = opt.mono_size;
        (looks_like_path(opt.mono) ? mono.file : mono.face) = opt.mono;
        extras.push_back(mono);
        font_mono = 1;

        strata::font_config heading;
        heading.pixel_height = opt.heading_size;
        heading.bold         = true;
        (looks_like_path(opt.heading) ? heading.file : heading.face) = opt.heading;
        extras.push_back(heading);
        font_heading = 2;

        cfg.extra_fonts = extras;
    }

    auto created = strata::context::create(cfg);
    if (!created) {
        ::MessageBoxW(nullptr, L"failed to build the font atlas", L"strata sandbox", MB_ICONERROR);
        return 1;
    }
    strata::context ui = std::move(*created);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = instance;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"strata_sandbox";
    ::RegisterClassExW(&wc);

    const auto adjust_for_dpi = [](RECT& r, HWND wnd) {
        const UINT dpi = wnd != nullptr ? ::GetDpiForWindow(wnd) : ::GetDpiForSystem();
        if (!::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
            ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        }
    };

    RECT rc{0, 0, opt.width, opt.height};
    adjust_for_dpi(rc, nullptr);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Strata Studio", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                  nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) { return 1; }

    host_window self;
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&self));
    self.platform.attach(hwnd);

    const bool shot_mode = !opt.shot.empty() || !opt.golden.empty();
    strata::input_recorder recorder;
    strata::input_replayer player;
    const bool recording = !opt.record.empty();
    const bool replaying = !opt.replay.empty();
    bool       replay_over = false;
    if (replaying && !player.load_file(opt.replay)) {
        std::fprintf(stderr, "[sandbox] cannot read recording %s\n", opt.replay.c_str());
        return 2;
    }

    const float start_scale = opt.scale > 0.0f ? opt.scale : (shot_mode ? 1.0f : self.platform.dpi_scale());
    if (start_scale != 1.0f) {
        (void)ui.set_scale(start_scale);
    }
    {
        RECT want{0, 0, static_cast<LONG>(std::lround(opt.width * ui.scale())),
                  static_cast<LONG>(std::lround(opt.height * ui.scale()))};
        adjust_for_dpi(want, hwnd);
        ::SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    RECT client{};
    ::GetClientRect(hwnd, &client);
    if (!host->init(hwnd, static_cast<strata::u32>(client.right), static_cast<strata::u32>(client.bottom),
                    opt.vsync, ui.font())) {
        ::MessageBoxW(nullptr, L"failed to initialize graphics backend", L"strata sandbox", MB_ICONERROR);
        return 1;
    }
    self.host = host.get();
    ui.release_font_pixels();
    ui.set_clipboard(self.platform.clipboard());
    ui.set_dock_animation(!shot_mode);

    if (!shot_mode || opt.api != options::api_kind::software) {
        ::ShowWindow(hwnd, show);
        ::UpdateWindow(hwnd);
    }

    strata::showcase::showcase_app showcase;
    showcase.init(ui, {
        .font_mono     = font_mono,
        .font_heading  = font_heading,
        .backend_name  = host->name(),
        .is_web        = false,
        .deterministic = shot_mode,
        .initial_scene = opt.scene,
        .initial_theme = opt.theme,
    });

    if (!opt.theme_file.empty()) {
        strata::themes::theme_result tr;
        (void)strata::themes::load_file(opt.theme_file, ui.theme(), &tr);
    }

    strata::i32 frame = 0;
    strata::i32 shot_counter = 0;
    strata::i32 frames_idled = 0;
    int    exit_code = 0;
    double last_time = seconds_now();
    double fps_smooth = 60.0;
    double ui_ms_smooth = 0.0;
    bool   quit = false;
    bool   show_metrics_cli = opt.metrics;
    bool   show_cmds_cli    = opt.metrics;

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
        if (self.minimized) { ::Sleep(50); continue; }

        float want_scale = showcase.take_requested_scale();
        if (opt.rescale > 0.0f && shot_counter == 5) { want_scale = opt.rescale; }
        if (self.pending_dpi > 0.0f) {
            if (opt.scale <= 0.0f) { want_scale = self.pending_dpi; }
            self.pending_dpi = 0.0f;
        }
        if (want_scale > 0.0f && std::abs(want_scale - ui.scale()) > 0.01f) {
            if (ui.set_scale(want_scale)) {
                (void)host->update_atlas(ui.font());
                ui.release_font_pixels();
            }
        }

        const double now = seconds_now();
        const double dt  = shot_mode ? (1.0 / 60.0) : std::clamp(now - last_time, 0.0005, 0.1);
        last_time = now;
        fps_smooth = shot_mode ? 60.0 : (fps_smooth * 0.94 + (1.0 / dt) * 0.06);

        const double ui_start = seconds_now();
        strata::input_state input = self.platform.new_frame();
        const auto reset_deterministic = [](strata::input_state& in) {
            const strata::vec2 display = in.display_size;
            in = strata::input_state{};
            in.display_size = display;
            in.delta_time   = 1.0f / 60.0f;
        };
        if (shot_mode && !replaying) { reset_deterministic(input); }
        if (replaying && !player.next(input)) {
            replay_over = true;
            if (shot_mode) { reset_deterministic(input); }
        }
        if (recording) { recorder.record(input); }

        ui.begin_frame(std::move(input));
        showcase.tick(ui, static_cast<strata::f32>(dt), static_cast<strata::f32>(fps_smooth),
                      static_cast<strata::f32>(shot_mode ? 0.35 : ui_ms_smooth));
        if (opt.metrics) {
            ui.debug_draw_list_window(show_cmds_cli);
            ui.debug_metrics_window(show_metrics_cli);
        }
        ui.end_frame();

        self.wants_text = ui.want_text_input() || ui.any_popup_open();
        self.platform.set_ime(ui.ime_wanted(), ui.ime_position(), ui.ime_line_height());
        self.platform.set_cursor(ui.cursor());

        const double ui_ms = (seconds_now() - ui_start) * 1000.0;
        ui_ms_smooth = ui_ms_smooth == 0.0 ? ui_ms : ui_ms_smooth * 0.9 + ui_ms * 0.1;

        const strata::draw_data dd = ui.render_data();
        const bool shot_frame = shot_mode && (replaying ? replay_over : ++shot_counter >= opt.shot_frames);
        if (shot_frame) { host->request_capture(); }

        const bool skip = opt.idle && !shot_mode && !shot_frame && ui.frame_unchanged();
        if (skip) {
            ++frames_idled;
        } else {
            host->render(dd, showcase.clear_color(ui));
        }

        if (shot_frame) {
            exit_code = finish_shot(*host, opt);
            ::DestroyWindow(hwnd);
        }
        if (replay_over && !shot_mode) { ::DestroyWindow(hwnd); }
        if (opt.max_frames > 0 && ++frame >= opt.max_frames) {
            std::fprintf(stderr, "[sandbox] %d frames ok (%s), %.0f fps, ui %.3f ms, idled %d\n",
                         frame, host->name(), fps_smooth, ui_ms_smooth, frames_idled);
            ::DestroyWindow(hwnd);
        }

        if (opt.idle && !shot_mode) {
            double wait = ui.next_wake_seconds();
            if (opt.max_frames > 0) { wait = std::min(wait, 1.0 / refresh); }
            if (wait > 0.0) {
                const DWORD ms = wait >= 3600.0 ? INFINITE : static_cast<DWORD>(std::ceil(wait * 1000.0));
                ::MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                next_frame = seconds_now();
                continue;
            }
        }
        if (cap > 0.0 && !shot_mode) {
            const double period = 1.0 / cap;
            next_frame += period;
            const double t = seconds_now();
            if (next_frame < t - period) { next_frame = t; }
            limiter.wait_until(next_frame);
        }
    }

    if (recording && !recorder.save(opt.record)) {
        std::fprintf(stderr, "[sandbox] cannot write %s\n", opt.record.c_str());
        exit_code = exit_code != 0 ? exit_code : 2;
    }
    self.host = nullptr;
    return exit_code;
}

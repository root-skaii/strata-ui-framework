#include "strata/app.hpp"

#include "strata/config.hpp"
#include "strata/platform/win32.hpp"
#include "strata/themes.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace strata {

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t window_class[] = L"strata_app";

[[nodiscard]] std::wstring to_wide(std::string_view s)
{
    std::wstring w;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n > 0) {
        w.resize(static_cast<std::size_t>(n));
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    }
    return w;
}

} // namespace

struct app::impl {
    app_config                     cfg;
    std::string                    title;      // (cfg's views are not kept: the strings are copied)
    std::string                    state_path;
    app*                           owner{};    // the app object run() was called on (it may have been moved since create)

    HWND                           hwnd{};
    win32_platform                 platform;
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain2>        chain;
    ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE                         latency_wait{}; // the swap chain's frame latency object: a frame starts when it can be shown
    bool                           tearing{};
    UINT                           chain_flags{};
    d3d11_renderer                 renderer;
    std::unique_ptr<context>       ui;

    bool  minimized{};
    bool  quitting{};
    int   exit_code{};
    f32   pending_dpi{};   // WM_DPICHANGED: the new scale, applied between frames
    bool  resized{};
    u64   frames{};
    u64   frames_drawn{};

    ~impl()
    {
        release_device();
        if (hwnd != nullptr) { ::DestroyWindow(hwnd); }
    }

    void release_device() noexcept
    {
        if (ctx != nullptr) { ctx->ClearState(); }
        rtv.Reset();
        renderer.destroy();
        if (latency_wait != nullptr) { ::CloseHandle(latency_wait); latency_wait = nullptr; }
        chain.Reset();
        ctx.Reset();
        device.Reset();
    }

    [[nodiscard]] bool create_target() noexcept
    {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> back;
        return SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) && SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
    }

    // a device (hardware, else WARP), a flip-model swap chain on the window, the renderer
    [[nodiscard]] std::expected<void, app_error> create_device()
    {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx)) &&
            FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
            return std::unexpected{app_error::device};
        }
        ComPtr<IDXGIDevice>   dxgi_device;
        ComPtr<IDXGIAdapter>  adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return std::unexpected{app_error::device};
        }
        // tearing (a variable refresh display without vsync) when the system has it
        ComPtr<IDXGIFactory5> factory5;
        BOOL allow_tearing = FALSE;
        tearing = SUCCEEDED(factory.As(&factory5)) &&
                  SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof allow_tearing)) &&
                  allow_tearing != FALSE;
        chain_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount      = 2;
        sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags            = chain_flags;
        ComPtr<IDXGISwapChain1> chain1;
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &chain1)) || FAILED(chain1.As(&chain))) {
            return std::unexpected{app_error::swap_chain};
        }
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        chain->SetMaximumFrameLatency(1); // (a frame waits until the one before is on its way: the least input lag)
        latency_wait = chain->GetFrameLatencyWaitableObject();
        if (!create_target()) {
            return std::unexpected{app_error::swap_chain};
        }
        // the renderer takes the atlas pixels, which were let go of after the last renderer took them
        if (ui->font().pixels().empty() && !ui->rebuild_font_atlas()) {
            return std::unexpected{app_error::fonts};
        }
        if (!renderer.create(device.Get(), ctx.Get(), ui->font())) {
            return std::unexpected{app_error::renderer};
        }
        renderer.set_state_restore(false); // (the device is ours: nothing to put back)
        ui->release_font_pixels();
        ui->invalidate();
        return {};
    }

    void resize() noexcept
    {
        if (chain == nullptr) { return; }
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        rtv.Reset();
        if (SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, chain_flags))) { (void)create_target(); }
        ui->invalidate(); // (the new buffers hold nothing: the next frame is drawn even if the ui did not change)
    }

    void apply_theme() noexcept
    {
        const win32_platform::appearance_settings a = win32_platform::appearance();
        ui->theme() = themes::for_appearance(a.dark, a.high_contrast, a.accent);
        ui->invalidate();
    }

    void apply_dpi() noexcept
    {
        if (pending_dpi <= 0.0f) { return; }
        const f32 scale = std::exchange(pending_dpi, 0.0f);
        if (scale == ui->scale() || !ui->set_scale(scale)) { return; }
        (void)renderer.update_atlas(ui->font());
        ui->release_font_pixels();
    }

    [[nodiscard]] bool present() noexcept
    {
        const float c[4] = {cfg.clear.r / 255.0f, cfg.clear.g / 255.0f, cfg.clear.b / 255.0f, cfg.clear.a / 255.0f};
        ID3D11RenderTargetView* t = rtv.Get();
        ctx->OMSetRenderTargets(1, &t, nullptr);
        ctx->ClearRenderTargetView(t, c);
        renderer.render(ui->render_data());
        const UINT interval = cfg.vsync ? 1u : 0u;
        const UINT flags    = !cfg.vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        const HRESULT hr = chain->Present(interval, flags);
        ++frames_drawn;
        return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
    }

    bool reset_device()
    {
        release_device();
        if (!create_device()) { return false; }
        if (owner != nullptr && owner->on_device_reset) { owner->on_device_reset(*owner); }
        return true;
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        auto* self = reinterpret_cast<impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self == nullptr) {
            if (msg == WM_NCCREATE) {
                self = static_cast<impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        (void)self->platform.handle_message(hwnd, msg, wparam, static_cast<std::intptr_t>(lparam));
        if (win32_platform::swallows(msg)) { return 0; }
        switch (msg) {
        case WM_SIZE:
            self->minimized = wparam == SIZE_MINIMIZED;
            if (!self->minimized) { self->resized = true; }
            return 0;
        case WM_DPICHANGED: {
            self->pending_dpi = static_cast<f32>(HIWORD(wparam)) / 96.0f;
            const RECT* r = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && self->platform.apply_cursor()) { return TRUE; }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            if (self->owner != nullptr && self->owner->on_close_request && !self->owner->on_close_request(*self->owner)) {
                return 0; // the app said no (it can quit() later itself)
            }
            self->quitting = true;
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
};

app::app(std::unique_ptr<impl> p) noexcept : impl_{std::move(p)} {}
app::app(app&&) noexcept            = default;
app& app::operator=(app&&) noexcept = default;
app::~app()                         = default;

std::expected<app, app_error> app::create(const app_config& cfg)
{
    // per-monitor dpi, so the ui is drawn at the monitor's resolution instead of being stretched by the system. (a
    // process that set its awareness already -- a manifest -- keeps what it has: this fails harmlessly then)
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    auto p   = std::make_unique<impl>();
    p->cfg   = cfg;
    p->title.assign(cfg.title);
    p->state_path.assign(cfg.state_file);
    p->cfg.title      = p->title;
    p->cfg.state_file = p->state_path;

    auto created = context::create(cfg.ui);
    if (!created) {
        return std::unexpected{app_error::fonts};
    }
    p->ui = std::make_unique<context>(std::move(*created));

    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &impl::wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = window_class;
    ::RegisterClassExW(&wc); // (a second app in the process finds it registered: fine)

    // the requested client size is logical: grown by the dpi of the monitor the window opens on
    const POINT origin{CW_USEDEFAULT, CW_USEDEFAULT};
    const UINT  dpi   = ::GetDpiForSystem();
    const f32   scale = static_cast<f32>(dpi) / 96.0f;
    RECT r{0, 0, static_cast<LONG>(std::lround(cfg.width * scale)), static_cast<LONG>(std::lround(cfg.height * scale))};
    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const std::wstring title = to_wide(cfg.title);
    p->hwnd = ::CreateWindowExW(0, window_class, title.c_str(), WS_OVERLAPPEDWINDOW, origin.x, origin.y, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, inst, p.get());
    if (p->hwnd == nullptr) {
        return std::unexpected{app_error::window};
    }
    p->platform.attach(p->hwnd);
    p->ui->set_clipboard(p->platform.clipboard());
    (void)p->ui->set_scale(p->platform.dpi_scale());

    if (const auto d = p->create_device(); !d) {
        return std::unexpected{d.error()};
    }
    if (cfg.follow_system_theme) { p->apply_theme(); }
    if (!p->state_path.empty()) {
        config state;
        if (state.load_file(p->state_path)) { (void)p->ui->load_state(state); }
    }
    return app{std::move(p)};
}

int app::run(const std::function<void(app&, context&)>& frame)
{
    impl& s  = *impl_;
    s.owner  = this;
    ::ShowWindow(s.hwnd, SW_SHOW);
    ::UpdateWindow(s.hwnd);

    while (!s.quitting) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { s.quitting = true; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (s.quitting) { break; }
        if (s.minimized) { // nothing to show: sleep until the window comes back
            ::WaitMessage();
            continue;
        }
        if (s.resized) {
            s.resized = false;
            s.resize();
        }
        s.apply_dpi();
        if (s.cfg.follow_system_theme && s.platform.appearance_changed()) { s.apply_theme(); }

        // a frame starts when the swap chain can take one, which keeps the input the frame reacts to as fresh as it gets
        if (s.latency_wait != nullptr) { ::WaitForSingleObjectEx(s.latency_wait, 100, TRUE); }

        s.ui->begin_frame(s.platform.new_frame());
        frame(*this, *s.ui);
        s.ui->end_frame();
        ++s.frames;
        s.platform.set_ime(s.ui->ime_wanted(), s.ui->ime_position(), s.ui->ime_line_height());
        s.platform.set_cursor(s.ui->cursor());

        // nothing changed: what is on the screen is right. (idling off: every frame is drawn)
        if (!s.cfg.idle || !s.ui->frame_unchanged()) {
            if (!s.present()) { // the device is gone: a new one, and the frame again
                if (!s.reset_device()) { s.exit_code = -1; break; }
                continue;
            }
        }
        if (s.cfg.idle) {
            const f64 wait = s.ui->next_wake_seconds();
            if (wait > 0.0) {
                const DWORD ms = wait >= 3600.0 ? INFINITE : static_cast<DWORD>(std::ceil(wait * 1000.0));
                ::MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }
    }

    if (!s.state_path.empty()) {
        config state;
        (void)state.load_file(s.state_path); // (other sections of the file are kept)
        s.ui->save_state(state);
        (void)state.save_file(s.state_path);
    }
    ::ShowWindow(s.hwnd, SW_HIDE);
    return s.exit_code;
}

void app::quit(int exit_code) noexcept
{
    impl_->exit_code = exit_code;
    impl_->quitting  = true;
}

bool app::reset_device()
{
    impl_->owner = this;
    return impl_->reset_device();
}

context&             app::ui() noexcept { return *impl_->ui; }
d3d11_renderer&      app::renderer() noexcept { return impl_->renderer; }
ID3D11Device*        app::device() const noexcept { return impl_->device.Get(); }
ID3D11DeviceContext* app::device_context() const noexcept { return impl_->ctx.Get(); }
void*                app::window() const noexcept { return impl_->hwnd; }
u64                  app::frames() const noexcept { return impl_->frames; }
u64                  app::frames_drawn() const noexcept { return impl_->frames_drawn; }

} // namespace strata

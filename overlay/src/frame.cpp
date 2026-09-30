#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

// a per-frame back buffer view, never kept: holding the buffer keeps the game's swap chain alive after release,
// so its next CreateSwapChain on the window fails (one flip-model swap chain per window) -- breaking resolution changes
[[nodiscard]] ComPtr<ID3D11RenderTargetView> frame_target(IDXGISwapChain* sc)
{
    ComPtr<ID3D11Texture2D> back;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) { return rtv; }
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    switch (td.Format) { // the ui is drawn in the display's own encoding: no srgb view
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:                              rd.Format = td.Format; break;
    }
    if (FAILED(g.device->CreateRenderTargetView(back.Get(), &rd, &rtv))) {
        (void)g.device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    }
    return rtv;
}

// scale changes from any thread apply here on the render thread: rebuild the atlas and hand it to the renderer
// (skipping that step leaves glyph coordinates mismatched with the texture).
void apply_pending_scale()
{
    const float wanted = g.want_scale.exchange(0.0f);
    if (wanted <= 0.0f || g.ui == nullptr) {
        return;
    }
    const u32 before = g.ui->font_generation();
    if (!g.ui->set_scale(wanted) || g.ui->font_generation() == before) {
        return; // unchanged, or the atlas failed at that size: keep the working one
    }
    const bool ok = g.backend == state::api::d3d12 ? g.renderer12.update_atlas(g.ui->font())
                                                   : g.renderer.update_atlas(g.ui->font());
    if (!ok) {
        log_line("apply_pending_scale: the renderer could not take the new atlas");
    }
    g.ui->release_font_pixels();
}

void draw_frame(IDXGISwapChain* sc)
{
    static bool first = true;
    ComPtr<ID3D11RenderTargetView> rtv; // (released at the end of the frame: see frame_target)
    if (g.backend == state::api::d3d11) {
        rtv = frame_target(sc);
        if (rtv == nullptr) { log_line("draw_frame: no render target"); return; }
    }
    if (g.backend == state::api::d3d12) { // (a resize can change the format: hdr switched on or off in the game)
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)) && d.BufferDesc.Format != g.renderer_format12) {
            if (!ensure_font_pixels() || !create_renderer12(d.BufferDesc.Format, d.BufferCount)) { log_line("draw_frame: no d3d12 renderer"); return; }
            g.ui->release_font_pixels();
            g.targets12_valid = false;
        }
    }
    apply_output(sc);

    apply_pending_scale();

    const bool open = g.visible.load(); // (latched: a toggle in the middle of the frame applies to the next one)
    input_state input;
    {
        const std::lock_guard lock{g.platform_mutex};
        input = g.platform.new_frame();
    }
    {   // a game that stretches a fixed-size back buffer over its window: the ui lives in back buffer pixels, so the
        // mouse is mapped into them (the window's client size is what the platform reports)
        DXGI_SWAP_CHAIN_DESC bd{};
        if (SUCCEEDED(sc->GetDesc(&bd)) && bd.BufferDesc.Width > 0 && bd.BufferDesc.Height > 0 && input.display_size.x > 0.0f && input.display_size.y > 0.0f) {
            const f32 bw = static_cast<f32>(bd.BufferDesc.Width), bh = static_cast<f32>(bd.BufferDesc.Height);
            if (std::fabs(bw - input.display_size.x) > 0.5f || std::fabs(bh - input.display_size.y) > 0.5f) {
                input.mouse_pos.x   *= bw / input.display_size.x;
                input.mouse_pos.y   *= bh / input.display_size.y;
                input.display_size   = {bw, bh};
            }
        }
    }
    if (!open) { // hud only: the ui sees the window's size and the clock, nothing the user does
        input_state idle;
        idle.display_size = input.display_size;
        idle.delta_time   = input.delta_time;
        input = std::move(idle);
    }
    {
        std::vector<std::function<void()>> todo;
        { const std::lock_guard lock{g.post_mutex}; todo.swap(g.posted); }
        for (auto& fn : todo) { fn(); }
    }
    g.ui->begin_frame(std::move(input));
    if (open && g.opt.scale_hotkeys && !g.ui->want_text_input()) {
        //  Ctrl + Plus / Minus (main row or keypad), Ctrl + 0 resets
        const auto stepped = [&](int direction) {
            const float next = std::clamp(g.ui->scale() + 0.1f * static_cast<float>(direction), 0.5f, 4.0f);
            g.want_scale.store(next);
        };
        if (g.ui->key_pressed(VK_OEM_PLUS, true) || g.ui->key_pressed(VK_ADD, true))        { stepped(1); }
        else if (g.ui->key_pressed(VK_OEM_MINUS, true) || g.ui->key_pressed(VK_SUBTRACT, true)) { stepped(-1); }
        else if (g.ui->key_pressed('0', true) || g.ui->key_pressed(VK_NUMPAD0, true))       { g.want_scale.store(g.base_scale); }
    }
    if (g.opt.hud) { g.opt.hud(*g.ui); }
    if (open && g.opt.ui) { g.opt.ui(*g.ui); }
    g.ui->end_frame();
    g.capture_mouse.store(open && g.ui->want_capture_mouse());
    g.capture_keys.store(open && g.ui->want_text_input());
    if (open) {
        {
            const std::lock_guard lock{g.platform_mutex};
            g.platform.set_cursor(g.ui->cursor());
        }
        ::ClipCursor(nullptr); // (games clipping the cursor every frame are overridden: the ui needs the whole desktop)
    }

    const bool want_capture = !g.opt.capture_path.empty() && !g.captured && g.visible_frames + 1 >= g.opt.capture_frame;
    if (g.backend == state::api::d3d12) {
        render12(sc, g.ui->render_data(), want_capture);
        if (want_capture) { g.captured = true; }
    } else {
        // draw into the back buffer and give the game its bindings back
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> old_rtv{};
        ID3D11DepthStencilView* old_dsv = nullptr;
        g.ctx->OMGetRenderTargets(static_cast<UINT>(old_rtv.size()), old_rtv.data(), &old_dsv);
        ID3D11RenderTargetView* target = rtv.Get();
        g.ctx->OMSetRenderTargets(1, &target, nullptr);
        g.renderer.render(g.ui->render_data());
        g.ctx->OMSetRenderTargets(static_cast<UINT>(old_rtv.size()), old_rtv.data(), old_dsv);
        for (ID3D11RenderTargetView* r : old_rtv) { if (r != nullptr) { r->Release(); } }
        if (old_dsv != nullptr) { old_dsv->Release(); }
        if (want_capture) {
            g.captured = true;
            capture_back_buffer(sc);
        }
    }
    if (first) { log_line("drawing the first frame (%s)", g.backend == state::api::d3d12 ? "direct3d 12" : "direct3d 11"); first = false; }
    g.frame_count.fetch_add(1);
    ++g.visible_frames;
}

} // namespace

void on_present(IDXGISwapChain* sc)
{
    static int calls = 0;
    if (calls++ < 3) { log_line("present %d on %p (attached chain %p)", calls, static_cast<void*>(sc), static_cast<void*>(g.chain)); }
    if (g.chain != nullptr && sc != g.chain) {
        // another swap chain presents. on the same window it must replace the attached one (one flip-model swap chain
        // per window): switch now. on another window it may be a second view, so switch only once the attached one has
        // been silent for a while
        DXGI_SWAP_CHAIN_DESC d{};
        const bool described = SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow != nullptr && ::IsWindow(d.OutputWindow);
        if (described && d.OutputWindow == g.hwnd) {
            log_line("the game replaced its swap chain");
            detach_chain();
        } else if (described && ++g.foreign_presents > 180) {
            log_line("the game presents to another window now");
            detach_chain();
        }
    } else if (sc == g.chain) {
        g.foreign_presents = 0;
    }
    if (g.chain == nullptr) {
        if (g.attach_failed) { return; }
        bool fatal = false;
        if (!attach(sc, fatal)) {
            g.attach_failed = fatal;
            return;
        }
        if (g.visible.load() && g.hwnd != nullptr) { ::PostMessageW(g.hwnd, wm_show_changed, 0, 0); }
    }
    if (sc == g.chain && (g.visible.load() || g.draw_hidden.load())) { draw_frame(sc); }
}

} // namespace strata::overlay::detail

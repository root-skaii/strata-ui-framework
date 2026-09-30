#include "internal.hpp"

namespace strata::overlay::detail {

// atlas pixels were released after the first renderer took them: later renderers need a rebuild
bool ensure_font_pixels()
{
    if (!g.ui->font().pixels().empty()) { return true; }
    if (!g.ui->rebuild_font_atlas()) { set_error("could not rebuild the font atlas"); return false; }
    return true;
}

// false: not yet (retry next Present); `fatal`: never (not d3d11 / 12, or the ui cannot be built).
// reruns when the game replaces its swap chain, keeping what still fits (ui, device objects)
bool attach(IDXGISwapChain* sc, bool& fatal)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || d.OutputWindow == nullptr || !::IsWindow(d.OutputWindow)) { return false; }
    ComPtr<ID3D11Device>       device;
    ComPtr<ID3D12Device>       device12;
    ComPtr<ID3D12CommandQueue> queue;
    state::api api = state::api::none;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        api = state::api::d3d11;
    } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device12)))) {
        // d3d12: ui is submitted on the game's direct queue (unknown yet with the fallback: retry next Present)
        queue = game_queue12(device12.Get());
        if (queue == nullptr) { return false; }
        ComPtr<ID3D12Device> queue_device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != device12.Get()) { return false; }
        api = state::api::d3d12;
    } else {
        set_error("the game's swap chain is neither direct3d 11 nor direct3d 12");
        fatal = true;
        return false;
    }

    const bool first_time = g.ui == nullptr;
    if (first_time) {
        context_config cfg;
        if (g.opt.configure_context) { g.opt.configure_context(cfg); }
        auto created = context::create(cfg);
        if (!created) {
            set_error("could not build the font atlas");
            fatal = true;
            return false;
        }
        g.ui = std::make_unique<context>(std::move(*created));
        g.platform.attach(d.OutputWindow);
        g.base_scale = g.opt.ui_scale > 0.0f ? g.opt.ui_scale : g.platform.dpi_scale();
        (void)g.ui->set_scale(g.base_scale);
        g.want_scale.store(0.0f);
    }

    // a new device (the game made one, or switched api): nothing made on the old one can be used
    const bool same_device = api == g.backend && (api == state::api::d3d11 ? device.Get() == g.device.Get()
                                                                           : device12.Get() == g.dev12.Get() && queue.Get() == g.queue12.Get());
    if (!same_device) {
        if (!first_time) { log_line("attach: the game has a new device"); }
        release_device_objects();
        if (!ensure_font_pixels()) { fatal = true; return false; }
        g.backend = api;
        bool renderer_ok = false;
        if (api == state::api::d3d11) {
            g.device = device;
            g.device->GetImmediateContext(&g.ctx);
            renderer_ok = g.renderer.create(g.device.Get(), g.ctx.Get(), g.ui->font());
        } else {
            g.dev12   = device12;
            g.queue12 = queue;
            renderer_ok = create_renderer12(d.BufferDesc.Format, d.BufferCount);
        }
        if (!renderer_ok) {
            set_error("could not create the ui renderer");
            release_device_objects();
            fatal = true;
            return false;
        }
        g.ui->release_font_pixels();
    }

    subclass_window(d.OutputWindow);
    g.chain            = sc;
    g.foreign_presents = 0;
    g.output_dirty.store(true);
    if (first_time && g.opt.on_ready) { g.opt.on_ready(*g.ui); }
    log_line("attached: window %p, subclassed %d, %s", static_cast<void*>(g.hwnd), g.subclassed ? 1 : 0,
             first_time ? "first time" : same_device ? "new swap chain" : "new device");
    return g.subclassed;
}

// the game released the swap chain we drew on: drop per-buffer objects, keep the rest for the next one
void detach_chain()
{
    wait_idle12();
    g.targets12_valid = false;
    g.chain            = nullptr;
    g.foreign_presents = 0;
}

} // namespace strata::overlay::detail

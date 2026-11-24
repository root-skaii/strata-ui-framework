// Interactive browser showcase for strata: runs the shared `strata_showcase` UI natively on WebGPU (emdawnwebgpu)
// with an automatic CPU software 2D-canvas fallback when a browser has no WebGPU support.

#include "showcase/app.hpp"

#include <strata/backend/software.hpp>
#include <strata/backend/webgpu.hpp>
#include <strata/context.hpp>

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

EM_JS(void, js_set_cursor, (int idx), {
    const map = ["default", "text", "pointer", "not-allowed", "ew-resize", "ns-resize", "nwse-resize", "nesw-resize"];
    const c = document.getElementById("canvas");
    if (c) c.style.cursor = map[idx] || "default";
});

EM_JS(void, js_focus_canvas, (), {
    const c = document.getElementById("canvas");
    if (c) c.focus();
});

EM_JS(int, js_has_webgpu, (), {
    return (typeof navigator !== "undefined" && !!navigator.gpu) ? 1 : 0;
});

EM_JS(void, js_ensure_2d_canvas, (), {
    const c = document.getElementById("canvas");
    if (c && !c.getContext("2d")) {
        const fresh = c.cloneNode(false);
        fresh.addEventListener("contextmenu", e => e.preventDefault());
        c.parentNode.replaceChild(fresh, c);
    }
});

EM_JS(void, js_present_2d, (const unsigned char* ptr, int w, int h), {
    const c = document.getElementById("canvas");
    if (!c) return;
    const ctx = c.getContext("2d");
    if (!ctx) return;
    const slice = new Uint8ClampedArray(HEAPU8.buffer, ptr, w * h * 4);
    ctx.putImageData(new ImageData(slice, w, h), 0, 0);
});

namespace {

using namespace strata;

struct dom_input {
    vec2                   mouse_pos{-10000.0f, -10000.0f};
    std::array<bool, 3>    mouse_down{};
    f32                    wheel{};
    f32                    wheel_x{};
    bool                   ctrl{}, shift{}, alt{};
    std::string            typed;
    std::vector<key_event> keys;
    f64                    last_time{};
    f32                    dpr{1.0f};
    u32                    phys_w{1280};
    u32                    phys_h{720};
    cursor_kind            last_cursor{cursor_kind::arrow};
    bool                   size_dirty{true};
};

struct web_host {
    std::unique_ptr<context>       ui;
    showcase::showcase_app         showcase;
    font_id                        mono_font{0};

    bool                           using_webgpu{false};
    WGPUInstance                   instance{};
    WGPUSurface                    surface{};
    WGPUAdapter                    adapter{};
    WGPUDevice                     device{};
    WGPUQueue                      queue{};
    WGPUTextureFormat              surface_format{WGPUTextureFormat_BGRA8Unorm};
    WGPUCompositeAlphaMode         alpha_mode{WGPUCompositeAlphaMode_Opaque};
    wgpu_renderer                  gpu_ren;

    software_renderer              sw_ren;
    std::vector<u8>                sw_rgba;

    dom_input                      in;
    f32                            fps_smooth{60.0f};
    f32                            ui_ms_smooth{0.35f};
};

web_host g;

[[nodiscard]] key map_dom_key(unsigned long kc) noexcept
{
    if (kc >= 'A' && kc <= 'Z') { return static_cast<key>(kc); }
    if (kc >= '0' && kc <= '9') { return static_cast<key>(kc); }
    if (kc >= 112 && kc <= 123) { return static_cast<key>(kc); }
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
            if (g.ui->set_scale(dpr)) {
                if (g.using_webgpu && g.gpu_ren.valid()) {
                    (void)g.gpu_ren.update_atlas(g.ui->font());
                } else if (!g.using_webgpu && g.sw_ren.valid()) {
                    (void)g.sw_ren.update_atlas(g.ui->font());
                }
                g.ui->release_font_pixels();
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
    js_set_cursor(idx);
}

EM_BOOL on_mouse_move(int, const EmscriptenMouseEvent* e, void*)
{
    g.in.mouse_pos = {static_cast<f32>(e->clientX) * g.in.dpr, static_cast<f32>(e->clientY) * g.in.dpr};
    g.in.ctrl      = e->ctrlKey != 0 || e->metaKey != 0;
    g.in.shift     = e->shiftKey != 0;
    g.in.alt       = e->altKey != 0;
    return EM_FALSE;
}

EM_BOOL on_mouse_down(int, const EmscriptenMouseEvent* e, void*)
{
    g.in.mouse_pos = {static_cast<f32>(e->clientX) * g.in.dpr, static_cast<f32>(e->clientY) * g.in.dpr};
    const int idx  = e->button == 0 ? 0 : (e->button == 2 ? 1 : (e->button == 1 ? 2 : -1));
    if (idx >= 0) { g.in.mouse_down[static_cast<std::size_t>(idx)] = true; }
    js_focus_canvas();
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
        if (len >= 1 && len <= 4 && (static_cast<u8>(e->key[0]) >= 0x20)) {
            const bool named = len > 1 && static_cast<u8>(e->key[0]) < 0x80;
            if (!named) { g.in.typed.append(e->key, len); }
        }
    }
    if (k == key::tab || k == key::backspace || k == key::space || k == key::up || k == key::down ||
        (g.in.ctrl && (k == key::a || k == key::f || k == key::s || k == key::z || k == key::y || k == key::slash || k == key::p))) {
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

void present_software_to_canvas()
{
    const u32 w = g.sw_ren.width(), h = g.sw_ren.height();
    if (w == 0 || h == 0) { return; }
    g.sw_rgba.resize(static_cast<std::size_t>(w) * h * 4);
    if (!g.sw_ren.copy_straight(g.sw_rgba)) { return; }
    js_present_2d(g.sw_rgba.data(), static_cast<int>(w), static_cast<int>(h));
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

    const f64 ui_t0 = emscripten_get_now();
    g.ui->begin_frame(std::move(in));
    g.showcase.tick(*g.ui, dt, g.fps_smooth, g.ui_ms_smooth);
    g.ui->end_frame();
    const f32 ui_ms = static_cast<f32>(emscripten_get_now() - ui_t0);
    g.ui_ms_smooth  = g.ui_ms_smooth * 0.90f + ui_ms * 0.10f;
    apply_css_cursor(g.ui->cursor());

    const draw_data dd = g.ui->render_data();
    const color bg = g.showcase.clear_color(*g.ui);

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
        js_ensure_2d_canvas();
        sync_canvas_size();
        (void)g.sw_ren.create(g.ui->font());
        g.sw_ren.resize(g.in.phys_w, g.in.phys_h);
        g.ui->release_font_pixels();
    }

    g.showcase.init(*g.ui, {
        .font_mono     = g.mono_font,
        .backend_name  = webgpu_ok ? "WebGPU" : "CPU Software",
        .is_web        = true,
        .deterministic = false,
        .initial_scene = "studio",
        .initial_theme = 0,
    });

    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, on_mouse_move);
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

    if (!js_has_webgpu()) {
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

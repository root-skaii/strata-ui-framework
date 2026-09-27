// renderer checks on a real d3d11 device (hardware or WARP), offscreen: the ui must draw whatever state is bound,
// and that state must be restored afterwards.
//   strata_render_test.exe            exit code 0 = passed

#include <strata/strata.hpp>
#include <strata/backend/d3d11.hpp>

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "g_test_gs_swallow.h"

namespace {

using Microsoft::WRL::ComPtr;
using namespace strata;

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  FAIL  line %d: %s\n", line, what);
    }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

struct gpu {
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<ID3D11Texture2D>        target;
    ComPtr<ID3D11RenderTargetView> rtv;
    u32                            w{400}, h{300};

    bool create(DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx)) &&
            FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
            return false;
        }
        return make_target(format);
    }
    bool make_target(DXGI_FORMAT format)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = w;
        td.Height           = h;
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = format;
        td.SampleDesc.Count = 1;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        target.Reset();
        rtv.Reset();
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &target)) && SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    }
    void clear(const float c[4])
    {
        ID3D11RenderTargetView* t = rtv.Get();
        ctx->OMSetRenderTargets(1, &t, nullptr);
        ctx->ClearRenderTargetView(t, c);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
    }
    // the target's pixels, 4 bytes each (rgba8 targets only)
    std::vector<u8> read()
    {
        D3D11_TEXTURE2D_DESC td{};
        target->GetDesc(&td);
        td.Usage          = D3D11_USAGE_STAGING;
        td.BindFlags      = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<u8> out;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) { return out; }
        ctx->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { return out; }
        out.resize(static_cast<std::size_t>(w) * h * 4);
        for (u32 y = 0; y < h; ++y) {
            std::memcpy(out.data() + static_cast<std::size_t>(y) * w * 4, static_cast<const u8*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, w * 4);
        }
        ctx->Unmap(staging.Get(), 0);
        return out;
    }
};

// a ui with one window at (20, 20), 200 x 120, over a black target
draw_data build_ui(context& ui, u32 w, u32 h)
{
    input_state in;
    in.display_size = {static_cast<f32>(w), static_cast<f32>(h)};
    in.mouse_pos    = {-100.0f, -100.0f};
    for (int i = 0; i < 2; ++i) {
        ui.begin_frame(in);
        if (auto win = ui.window("render test", {20, 20}, {200, 120}, window_flags::none)) { ui.text("some text"); }
        ui.end_frame();
    }
    return ui.render_data();
}

[[nodiscard]] u32 lit_pixels(const std::vector<u8>& px, u32 w, u32 x0, u32 y0, u32 x1, u32 y1)
{
    u32 n = 0;
    for (u32 y = y0; y < y1; ++y) {
        for (u32 x = x0; x < x1; ++x) {
            const u8* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
            n += (p[0] > 8 || p[1] > 8 || p[2] > 8) ? 1u : 0u;
        }
    }
    return n;
}

void test_hostile_state(gpu& g)
{
    std::fprintf(stderr, "[d3d11: a geometry shader the caller left bound does not swallow the ui, and is bound again after]\n");
    context        ui = context::create().value();
    d3d11_renderer renderer;
    CHECK(renderer.create(g.device.Get(), g.ctx.Get(), ui.font()));
    const draw_data data = build_ui(ui, g.w, g.h);

    ComPtr<ID3D11GeometryShader> gs;
    CHECK(SUCCEEDED(g.device->CreateGeometryShader(g_test_gs_swallow, sizeof(g_test_gs_swallow), nullptr, &gs)));
    const float black[4] = {0, 0, 0, 1};
    g.clear(black);
    g.ctx->GSSetShader(gs.Get(), nullptr, 0); // what a game might have bound when it calls Present
    renderer.render(data);

    ComPtr<ID3D11GeometryShader> after;
    g.ctx->GSGetShader(&after, nullptr, nullptr);
    CHECK(after.Get() == gs.Get()); // given back
    g.ctx->GSSetShader(nullptr, nullptr, 0);

    const std::vector<u8> px = g.read();
    CHECK(!px.empty());
    if (!px.empty()) {
        const u32 window_px = lit_pixels(px, g.w, 20, 20, 220, 140);
        CHECK(window_px > 200 * 120 / 2); // the window's body is drawn
        CHECK(lit_pixels(px, g.w, 300, 200, 400, 300) == 0); // and nothing where there is no window
    }
}

// ---- output encodings: the same opaque colour through each kind of target ---------------------------------------

[[nodiscard]] f32 half_to_float(u16 h)
{
    const u32 sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
    f32 v;
    if (exp == 0)       { v = std::ldexp(static_cast<f32>(man), -24); }
    else if (exp == 31) { v = 65504.0f; }
    else                { v = std::ldexp(static_cast<f32>(man | 0x400u), static_cast<int>(exp) - 25); }
    return sign != 0 ? -v : v;
}

[[nodiscard]] f32 srgb_to_linear(f32 c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] f32 pq_encode(f32 l)
{
    const f32 m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const f32 p = std::pow(std::clamp(l, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

// renders the window with an opaque body into a `format` target encoded as `space`; returns a text-free body texel
[[nodiscard]] std::vector<u8> body_texel(gpu& g, DXGI_FORMAT format, output_space space, f32 nits, u32 bytes_per_pixel)
{
    std::vector<u8> texel;
    if (!g.make_target(format)) { return texel; }
    context_config cfg;
    cfg.theme.window_bg = color{200, 100, 50, 255};
    cfg.theme.shadow_blur = 0.0f;
    context        ui = context::create(cfg).value();
    d3d11_renderer renderer;
    if (!renderer.create(g.device.Get(), g.ctx.Get(), ui.font())) { return texel; }
    renderer.set_output({space, nits});
    const draw_data data = build_ui(ui, g.w, g.h);
    const float black[4] = {0, 0, 0, 1};
    g.clear(black);
    renderer.render(data);

    D3D11_TEXTURE2D_DESC td{};
    g.target->GetDesc(&td);
    td.Usage          = D3D11_USAGE_STAGING;
    td.BindFlags      = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g.device->CreateTexture2D(&td, nullptr, &staging))) { return texel; }
    g.ctx->CopyResource(staging.Get(), g.target.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { return texel; }
    constexpr u32 x = 180, y = 120; // inside the body, below the text
    const u8* p = static_cast<const u8*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch + static_cast<std::size_t>(x) * bytes_per_pixel;
    texel.assign(p, p + bytes_per_pixel);
    g.ctx->Unmap(staging.Get(), 0);
    return texel;
}

void test_output_spaces(gpu& g)
{
    std::fprintf(stderr, "[d3d11: srgb views, scRGB and HDR10 targets get the colour encoded for them]\n");
    const f32 r = 200.0f / 255.0f, gc = 100.0f / 255.0f, b = 50.0f / 255.0f;

    // an 8-bit unorm target: written as it is
    const std::vector<u8> plain = body_texel(g, DXGI_FORMAT_R8G8B8A8_UNORM, output_space::srgb, 200.0f, 4);
    CHECK(plain.size() == 4 && plain[0] == 200 && plain[1] == 100 && plain[2] == 50);

    // an srgb view encodes on write, so the shader writes linear and the bytes round-trip (unconverted, the hardware
    // would encode twice: 200 -> ~228)
    const std::vector<u8> view = body_texel(g, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, output_space::srgb_view, 200.0f, 4);
    CHECK(view.size() == 4 && std::abs(view[0] - 200) <= 1 && std::abs(view[1] - 100) <= 1 && std::abs(view[2] - 50) <= 1);

    // scRGB: linear, 1.0 = 80 nits. at a paper white of 160 nits the ui's white is 2.0
    const std::vector<u8> fp = body_texel(g, DXGI_FORMAT_R16G16B16A16_FLOAT, output_space::scrgb, 160.0f, 8);
    if (fp.size() == 8) {
        u16 h[4];
        std::memcpy(h, fp.data(), sizeof h);
        CHECK(std::abs(half_to_float(h[0]) - srgb_to_linear(r) * 2.0f) < 0.01f);
        CHECK(std::abs(half_to_float(h[1]) - srgb_to_linear(gc) * 2.0f) < 0.01f);
        CHECK(std::abs(half_to_float(h[2]) - srgb_to_linear(b) * 2.0f) < 0.01f);
    } else {
        CHECK(fp.size() == 8);
    }

    // HDR10: BT.709 -> BT.2020 primaries, scaled to nits, PQ
    const std::vector<u8> pq = body_texel(g, DXGI_FORMAT_R10G10B10A2_UNORM, output_space::hdr10, 200.0f, 4);
    if (pq.size() == 4) {
        u32 packed;
        std::memcpy(&packed, pq.data(), sizeof packed);
        const f32 got[3] = {static_cast<f32>(packed & 1023u) / 1023.0f, static_cast<f32>((packed >> 10) & 1023u) / 1023.0f,
                            static_cast<f32>((packed >> 20) & 1023u) / 1023.0f};
        const f32 l[3] = {srgb_to_linear(r), srgb_to_linear(gc), srgb_to_linear(b)};
        const f32 m[3][3] = {{0.6274040f, 0.3292820f, 0.0433136f}, {0.0690970f, 0.9195400f, 0.0113612f}, {0.0163916f, 0.0880132f, 0.8955950f}};
        for (int c = 0; c < 3; ++c) {
            const f32 want = pq_encode((m[c][0] * l[0] + m[c][1] * l[1] + m[c][2] * l[2]) * 200.0f / 10000.0f);
            CHECK(std::abs(got[c] - want) < 2.5f / 1023.0f);
        }
        CHECK(got[0] < 0.75f); // (as plain srgb it would be 200/255 = 0.78: garish on hdr)
    } else {
        CHECK(pq.size() == 4);
    }
    g.make_target(DXGI_FORMAT_R8G8B8A8_UNORM);
}

void test_text_contrast(gpu& g)
{
    std::fprintf(stderr, "[d3d11: style::text_contrast thickens light text]\n");
    const auto text_ink = [&](f32 contrast) {
        context_config cfg;
        cfg.theme.text_contrast = contrast;
        context        ui = context::create(cfg).value();
        d3d11_renderer renderer;
        if (!renderer.create(g.device.Get(), g.ctx.Get(), ui.font())) { return u64{0}; }
        const draw_data data = build_ui(ui, g.w, g.h);
        CHECK(data.text_contrast == contrast);
        const float black[4] = {0, 0, 0, 1};
        g.clear(black);
        renderer.render(data);
        const std::vector<u8> px = g.read();
        u64 sum = 0; // title text (light on dark): its contribution over the bar colour
        if (px.empty()) { return sum; }
        const int bar = px[(static_cast<std::size_t>(30) * g.w + 212) * 4 + 1]; // (right of the title, in the bar)
        for (u32 y = 22; y < 42; ++y) {
            for (u32 x = 40; x < 200; ++x) { sum += static_cast<u64>(std::max(0, px[(static_cast<std::size_t>(y) * g.w + x) * 4 + 1] - bar)); }
        }
        return sum;
    };
    const u64 plain = text_ink(0.0f);
    const u64 thick = text_ink(1.0f);
    CHECK(plain > 0);
    CHECK(thick > plain + plain / 20); // noticeably more ink
}

void test_device_recovery()
{
    std::fprintf(stderr, "[d3d11: after a lost device, the documented recovery gives a working renderer on a new one]\n");
    gpu first;
    if (!first.create()) { CHECK(false); return; }
    context        ui = context::create().value();
    d3d11_renderer renderer;
    CHECK(renderer.create(first.device.Get(), first.ctx.Get(), ui.font()));
    ui.release_font_pixels(); // what every host does once the renderer has the atlas
    CHECK(ui.font().pixels().empty());
    CHECK(!renderer.device_lost());
    const u32 generation = ui.font_generation();

    // the device is gone: a new one, and the steps from device_lost()'s comment
    gpu second;
    if (!second.create()) { CHECK(false); return; }
    CHECK(ui.rebuild_font_atlas().has_value());
    CHECK(!ui.font().pixels().empty());
    CHECK(ui.font_generation() != generation);
    CHECK(renderer.create(second.device.Get(), second.ctx.Get(), ui.font()));
    ui.release_font_pixels();
    ui.invalidate();

    const draw_data data = build_ui(ui, second.w, second.h);
    const float black[4] = {0, 0, 0, 1};
    second.clear(black);
    renderer.render(data);
    const std::vector<u8> px = second.read();
    CHECK(!px.empty() && lit_pixels(px, second.w, 20, 20, 220, 140) > 200 * 120 / 2);
}

} // namespace

int main()
{
    gpu g;
    if (!g.create()) {
        std::fprintf(stderr, "no direct3d 11 device (not even WARP)\n");
        return 2;
    }
    test_hostile_state(g);
    test_output_spaces(g);
    test_text_contrast(g);
    test_device_recovery();
    std::fprintf(stderr, "render test: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

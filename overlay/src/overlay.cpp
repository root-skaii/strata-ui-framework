#include <strata/overlay/overlay.hpp>

#include <strata/backend/d3d11.hpp>
#include <strata/backend/d3d12.hpp>
#include <strata/platform/win32.hpp>

#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace strata::overlay {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT wm_show_changed = WM_APP + 0x5710; // to the game's window: the cursor bookkeeping runs on its thread

using present_fn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using present1_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using resize_fn   = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

using execute_fn  = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

constexpr int vt_queue_execute  = 10; // ID3D12CommandQueue::ExecuteCommandLists
constexpr int vt_present        = 8;
constexpr int vt_resize_buffers = 13;
constexpr int vt_present1       = 22;

struct state {
    options              opt;
    std::atomic<bool>    installed{false};
    std::atomic<bool>    shutting_down{false};
    std::atomic<bool>    visible{false};
    std::atomic<bool>    capture_mouse{false};
    std::atomic<bool>    capture_keys{false};
    std::atomic<u64>     frame_count{0};
    std::atomic<int>     in_hook{0};
    char                 error[256]{};

    void**       vtable{};
    bool         has_sc1{};
    present_fn   orig_present{};
    present1_fn  orig_present1{};
    resize_fn    orig_resize{};
    bool         patched_present{}, patched_present1{}, patched_resize{};

    // direct3d 12: the game's direct queue is found by watching ExecuteCommandLists
    void**                           queue_vtable{};
    execute_fn                       orig_execute{};
    bool                             patched_execute{};
    std::atomic<ID3D12CommandQueue*> last_queue{nullptr};

    enum class api { none, d3d11, d3d12 };
    api                              backend{api::none};

    struct frame12 {
        ComPtr<ID3D12CommandAllocator> alloc;
        UINT64                         fence_value{};
    };
    ComPtr<ID3D12Device>               dev12;
    ComPtr<ID3D12CommandQueue>         queue12;
    ComPtr<ID3D12DescriptorHeap>       rtv_heap12;
    UINT                               rtv_size12{};
    std::vector<frame12>               frames12;
    ComPtr<ID3D12GraphicsCommandList>  list12;
    ComPtr<ID3D12Fence>                fence12;
    HANDLE                             fence_event12{};
    UINT64                             fence_next12{};
    UINT                               buffers12{};
    UINT                               renderer_frames12{};
    DXGI_FORMAT                        format12{};
    UINT                               w12{}, h12{};
    bool                               targets12_valid{};
    d3d12_renderer                     renderer12;

    // the game's swap chain and what is built on it
    IDXGISwapChain*                  chain{};   // identity only: the game owns it
    HWND                             hwnd{};
    WNDPROC                          orig_proc{};
    bool                             subclassed{};
    ComPtr<ID3D11Device>             device;
    ComPtr<ID3D11DeviceContext>      ctx;
    ComPtr<ID3D11RenderTargetView>   rtv;
    UINT                             rtv_w{}, rtv_h{};
    d3d11_renderer                   renderer;
    std::unique_ptr<context>         ui;
    win32_platform                   platform;
    std::recursive_mutex             platform_mutex; // the window thread feeds messages, the render thread reads the frame (recursive: a call inside the platform can dispatch a message to our own window procedure)
    unsigned                         visible_frames{};
    bool                             captured{};
    bool                             attach_failed{};
    int                              cursor_shown{};  // ShowCursor increments made while visible, taken back when hidden
};

// never destroyed: the process may end (or the loader detach the dll) while the game's threads are still inside a hook, and
// tearing d3d objects down during that is a crash at exit. uninstall() frees what matters when the dll is meant to go.
state& g = *new state;

// STRATA_OVERLAY_LOG=<file>: what the hook is doing, for finding out why it does not attach
void log_line(const char* fmt, ...)
{
    char path[512];
    if (::GetEnvironmentVariableA("STRATA_OVERLAY_LOG", path, sizeof(path)) == 0) { return; }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || f == nullptr) { return; }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);
    std::fputc('\n', f);
    std::fclose(f);
}

void set_error(const char* what) noexcept
{
    std::snprintf(g.error, sizeof(g.error), "%s", what);
    log_line("error: %s", what);
}

// ---- png (uncompressed deflate blocks: big, simple, enough for a debug capture) -----------------------------------

[[nodiscard]] u32 crc32(const u8* p, std::size_t n, u32 crc = 0) noexcept
{
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1; }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8); }
    return ~crc;
}

void put_be32(std::vector<u8>& v, u32 x)
{
    v.push_back(static_cast<u8>(x >> 24)); v.push_back(static_cast<u8>(x >> 16)); v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x));
}

void png_chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
{
    put_be32(out, static_cast<u32>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, crc32(out.data() + start, out.size() - start));
}

[[nodiscard]] bool write_png(const char* path, const std::vector<u8>& rgba, u32 w, u32 h)
{
    std::vector<u8> raw;
    raw.reserve((static_cast<std::size_t>(w) * 4 + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4, rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (const u8 c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    for (std::size_t pos = 0; pos < raw.size(); pos += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<u8>(n)); z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n)); z.push_back(static_cast<u8>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    }
    put_be32(z, (b << 16) | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<u8> ihdr;
    put_be32(ihdr, w); put_be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) { return false; }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// copies the back buffer (with the ui on it) out and writes it as a png
void capture_back_buffer(IDXGISwapChain* sc)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) { return; }
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) { set_error("capture: back buffer format not supported"); return; }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &staging))) { return; }
    g.ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { return; }
    std::vector<u8> px(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const u8* src = static_cast<const u8*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        u8* dst = px.data() + static_cast<std::size_t>(y) * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            dst[4 * x]     = bgra ? src[4 * x + 2] : src[4 * x];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = bgra ? src[4 * x] : src[4 * x + 2];
            dst[4 * x + 3] = 255;
        }
    }
    g.ctx->Unmap(staging.Get(), 0);
    if (!write_png(g.opt.capture_path.c_str(), px, desc.Width, desc.Height)) { set_error("capture: cannot write the png"); }
}

// ---- direct3d 12 --------------------------------------------------------------------------------------------------

void wait_fence12(UINT64 value, DWORD timeout_ms = 2000)
{
    if (g.fence12 == nullptr || value == 0 || g.fence12->GetCompletedValue() >= value) { return; }
    if (g.fence12->SetEventOnCompletion(value, g.fence_event12) == S_OK) { ::WaitForSingleObject(g.fence_event12, timeout_ms); }
}

void wait_idle12()
{
    if (g.queue12 == nullptr || g.fence12 == nullptr) { return; }
    wait_fence12(g.fence_next12);
}

void barrier12(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// makes the descriptors (one per buffer of the swap chain), allocators and fence; again when the buffers were recreated
bool ensure_targets12(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) { return false; }
    if (g.targets12_valid && d.BufferCount == g.buffers12 && d.BufferDesc.Width == g.w12 && d.BufferDesc.Height == g.h12) { return true; }

    wait_idle12();
    g.targets12_valid = false;
    g.buffers12 = d.BufferCount;
    g.w12 = d.BufferDesc.Width;
    g.h12 = d.BufferDesc.Height;
    g.format12 = d.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = g.buffers12;
    g.rtv_heap12.Reset();
    if (FAILED(g.dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap12)))) { return false; }
    g.rtv_size12 = g.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtv_heap12->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g.buffers12; ++i) {
        ComPtr<ID3D12Resource> buf;
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&buf)))) { return false; }
        g.dev12->CreateRenderTargetView(buf.Get(), nullptr, h);
        h.ptr += g.rtv_size12;
    }
    if (g.frames12.size() != g.buffers12) {
        g.frames12.clear();
        g.frames12.resize(g.buffers12);
        for (auto& f : g.frames12) {
            if (FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) { return false; }
        }
    }
    g.targets12_valid = true;
    return true;
}

// the back buffer (with the ui on it) as rgba: the copy is part of the frame's command list, the caller waits and reads
struct readback12 {
    ComPtr<ID3D12Resource>             buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT                               width{}, height{};
    DXGI_FORMAT                        format{};
};

bool prepare_readback12(ID3D12Resource* back, readback12& rb)
{
    const D3D12_RESOURCE_DESC rd = back->GetDesc();
    UINT64 total = 0;
    g.dev12->GetCopyableFootprints(&rd, 0, 1, 0, &rb.footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rb.width  = static_cast<UINT>(rd.Width);
    rb.height = rd.Height;
    rb.format = rd.Format;
    return SUCCEEDED(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.buffer)));
}

void write_readback12(const readback12& rb)
{
    const bool bgra = rb.format == DXGI_FORMAT_B8G8R8A8_UNORM || rb.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool rgba = rb.format == DXGI_FORMAT_R8G8B8A8_UNORM || rb.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) { set_error("capture: back buffer format not supported"); return; }
    void* mapped = nullptr;
    if (FAILED(rb.buffer->Map(0, nullptr, &mapped))) { return; }
    std::vector<u8> px(static_cast<std::size_t>(rb.width) * rb.height * 4);
    for (UINT y = 0; y < rb.height; ++y) {
        const u8* src = static_cast<const u8*>(mapped) + rb.footprint.Offset + static_cast<std::size_t>(y) * rb.footprint.Footprint.RowPitch;
        u8* dst = px.data() + static_cast<std::size_t>(y) * rb.width * 4;
        for (UINT x = 0; x < rb.width; ++x) {
            dst[4 * x]     = bgra ? src[4 * x + 2] : src[4 * x];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = bgra ? src[4 * x] : src[4 * x + 2];
            dst[4 * x + 3] = 255;
        }
    }
    rb.buffer->Unmap(0, nullptr);
    if (!write_png(g.opt.capture_path.c_str(), px, rb.width, rb.height)) { set_error("capture: cannot write the png"); }
}

void render12(IDXGISwapChain* sc, const draw_data& data, bool capture)
{
    if (!ensure_targets12(sc)) { log_line("render12: no targets"); return; }
    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) { return; }
    const UINT idx = sc3->GetCurrentBackBufferIndex();
    if (idx >= g.frames12.size()) { return; }
    ID3D12CommandQueue* queue = g.queue12.Get();
    auto& fr = g.frames12[idx];
    wait_fence12(fr.fence_value); // (this frame slot's previous command list and upload buffers have finished)

    ComPtr<ID3D12Resource> back;
    if (FAILED(sc->GetBuffer(idx, IID_PPV_ARGS(&back)))) { return; }
    if (FAILED(fr.alloc->Reset()) || FAILED(g.list12->Reset(fr.alloc.Get(), nullptr))) { return; }

    barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap12->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx) * g.rtv_size12;
    g.list12->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g.renderer12.render(data, g.list12.Get(), idx % g.renderer_frames12, nullptr);

    readback12 rb;
    const bool capturing = capture && prepare_readback12(back.Get(), rb);
    if (capturing) {
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = rb.buffer.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = rb.footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = back.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        g.list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        barrier12(g.list12.Get(), back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (FAILED(g.list12->Close())) { return; }
    ID3D12CommandList* lists[] = {g.list12.Get()};
    queue->ExecuteCommandLists(1, lists);
    fr.fence_value = ++g.fence_next12;
    queue->Signal(g.fence12.Get(), fr.fence_value);
    if (capturing) {
        wait_fence12(fr.fence_value);
        write_readback12(rb);
    }
}

// ---- the window --------------------------------------------------------------------------------------------------

[[nodiscard]] bool is_mouse_message(UINT m) noexcept { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSEHWHEEL || m == WM_XBUTTONDBLCLK; }
[[nodiscard]] bool is_key_message(UINT m) noexcept
{
    return m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR || m == WM_DEADCHAR || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP || m == WM_SYSCHAR ||
           m == WM_UNICHAR;
}

void apply_cursor_state(bool now_visible)
{
    if (now_visible) {
        // a game that hid the cursor: bring it back (the counter is per thread: this runs on the window's thread)
        if (g.cursor_shown == 0) {
            int count = ::ShowCursor(TRUE);
            g.cursor_shown = 1;
            for (int guard = 0; count < 0 && guard < 32; ++guard) { count = ::ShowCursor(TRUE); ++g.cursor_shown; }
        }
        ::ClipCursor(nullptr);
    } else {
        while (g.cursor_shown > 0) { ::ShowCursor(FALSE); --g.cursor_shown; }
    }
}

void toggle_visible()
{
    log_line("toggle: %d -> %d", g.visible.load() ? 1 : 0, g.visible.load() ? 0 : 1);
    show(!g.visible.load());
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const WNDPROC next = g.orig_proc;
    if (g.shutting_down.load()) {
        return ::CallWindowProcW(next, hwnd, msg, wparam, lparam);
    }
    if (msg == wm_show_changed) {
        apply_cursor_state(g.visible.load());
        return 0;
    }
    if (g.opt.toggle_key != 0 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) && wparam == g.opt.toggle_key) {
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (lparam & (1 << 30)) == 0) { toggle_visible(); }
        return 0;
    }
    if (g.visible.load()) {
        {
            const std::lock_guard lock{g.platform_mutex};
            (void)g.platform.handle_message(hwnd, msg, wparam, static_cast<std::intptr_t>(lparam));
        }
        if (win32_platform::swallows(msg)) { return 0; }
        const bool block_all = g.opt.block_game_input;
        if (msg == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
            const std::lock_guard lock{g.platform_mutex};
            if (g.platform.apply_cursor()) { return TRUE; }
        }
        if (is_mouse_message(msg) && (block_all || g.capture_mouse.load())) { return 0; }
        if (is_key_message(msg) && (block_all || g.capture_keys.load())) { return 0; }
        if (msg == WM_INPUT && block_all) { return ::DefWindowProcW(hwnd, msg, wparam, lparam); } // (raw mouse for a camera: cleaned up, not delivered)
    }
    return ::CallWindowProcW(next, hwnd, msg, wparam, lparam);
}

// ---- attaching to the game's swap chain and drawing ---------------------------------------------------------------

// false: not yet (try the next Present); `fatal`: never (this is not a direct3d 11 game / the ui cannot be built)
bool attach(IDXGISwapChain* sc, bool& fatal)
{
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || d.OutputWindow == nullptr || !::IsWindow(d.OutputWindow)) { return false; }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D12Device> device12;
    if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device)))) {
        g.backend = state::api::d3d11;
        g.device = device;
        g.device->GetImmediateContext(&g.ctx);
    } else if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&device12)))) {
        // direct3d 12: the game's direct queue is the one seen executing command lists; until it has been seen, wait
        ID3D12CommandQueue* seen = g.last_queue.load();
        if (seen == nullptr) { return false; }
        ComPtr<ID3D12Device> queue_device;
        if (FAILED(seen->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != device12.Get()) { return false; }
        g.backend = state::api::d3d12;
        g.dev12   = device12;
        g.queue12 = seen;
    } else {
        set_error("the game's swap chain is neither direct3d 11 nor direct3d 12");
        fatal = true;
        return false;
    }
    g.hwnd = d.OutputWindow;

    context_config cfg;
    if (g.opt.configure_context) { g.opt.configure_context(cfg); }
    auto created = context::create(cfg);
    if (!created) {
        set_error("could not build the font atlas");
        fatal = true;
        return false;
    }
    g.ui = std::make_unique<context>(std::move(*created));
    g.platform.attach(g.hwnd);
    g.ui->set_clipboard(g.platform.clipboard());
    (void)g.ui->set_scale(g.platform.dpi_scale());
    bool renderer_ok = false;
    if (g.backend == state::api::d3d11) {
        renderer_ok = g.renderer.create(g.device.Get(), g.ctx.Get(), g.ui->font());
    } else {
        DXGI_SWAP_CHAIN_DESC scd{};
        sc->GetDesc(&scd);
        g.renderer_frames12 = std::max<UINT>(scd.BufferCount, 2);
        d3d12_init_info info;
        info.device           = g.dev12.Get();
        info.queue            = g.queue12.Get();
        info.frames_in_flight = g.renderer_frames12;
        info.rtv_format       = static_cast<u32>(scd.BufferDesc.Format);
        renderer_ok = g.renderer12.create(info, g.ui->font());
        if (renderer_ok) {
            g.fence_event12 = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            ComPtr<ID3D12CommandAllocator> first;
            renderer_ok = g.fence_event12 != nullptr && SUCCEEDED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12))) &&
                          SUCCEEDED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&first))) &&
                          SUCCEEDED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, first.Get(), nullptr, IID_PPV_ARGS(&g.list12)));
            if (renderer_ok) { g.list12->Close(); }
        }
    }
    if (!renderer_ok) {
        set_error("could not create the ui renderer");
        g.ui.reset();
        fatal = true;
        return false;
    }
    g.ui->release_font_pixels();
    if (g.opt.on_ready) { g.opt.on_ready(*g.ui); }

    g.orig_proc  = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc)));
    g.subclassed = g.orig_proc != nullptr;
    if (!g.subclassed) { set_error("could not subclass the game's window"); }
    g.chain = sc;
    log_line("attached: window %p, subclassed %d", static_cast<void*>(g.hwnd), g.subclassed ? 1 : 0);
    return g.subclassed;
}

bool ensure_target(IDXGISwapChain* sc)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) { return false; }
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    if (g.rtv != nullptr && td.Width == g.rtv_w && td.Height == g.rtv_h) { return true; }
    g.rtv.Reset();
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    switch (td.Format) { // the ui is drawn in the display's own encoding: no srgb view
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default:                              rd.Format = td.Format; break;
    }
    if (FAILED(g.device->CreateRenderTargetView(back.Get(), &rd, &g.rtv))) {
        if (FAILED(g.device->CreateRenderTargetView(back.Get(), nullptr, &g.rtv))) { return false; }
    }
    g.rtv_w = td.Width;
    g.rtv_h = td.Height;
    return true;
}

void draw_frame(IDXGISwapChain* sc)
{
    static bool first = true;
    if (g.backend == state::api::d3d11 && !ensure_target(sc)) { log_line("draw_frame: no render target"); return; }

    input_state input;
    {
        const std::lock_guard lock{g.platform_mutex};
        input = g.platform.new_frame();
    }
    g.ui->begin_frame(std::move(input));
    if (g.opt.ui) { g.opt.ui(*g.ui); }
    g.ui->end_frame();
    g.capture_mouse.store(g.ui->want_capture_mouse());
    g.capture_keys.store(g.ui->want_text_input());
    {
        const std::lock_guard lock{g.platform_mutex};
        g.platform.set_cursor(g.ui->cursor());
    }
    ::ClipCursor(nullptr); // (a game that clips the cursor to its window every frame is not asked politely: the ui needs all of the desktop)

    const bool want_capture = !g.opt.capture_path.empty() && !g.captured && g.visible_frames + 1 >= g.opt.capture_frame;
    if (g.backend == state::api::d3d12) {
        render12(sc, g.ui->render_data(), want_capture);
        if (want_capture) { g.captured = true; }
    } else {
        // draw into the back buffer and give the game its bindings back
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> old_rtv{};
        ID3D11DepthStencilView* old_dsv = nullptr;
        g.ctx->OMGetRenderTargets(static_cast<UINT>(old_rtv.size()), old_rtv.data(), &old_dsv);
        ID3D11RenderTargetView* target = g.rtv.Get();
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

void on_present(IDXGISwapChain* sc)
{
    static int calls = 0;
    if (calls++ < 3) { log_line("present %d on %p (attached chain %p)", calls, static_cast<void*>(sc), static_cast<void*>(g.chain)); }
    if (g.chain == nullptr) {
        if (g.attach_failed) { return; }
        bool fatal = false;
        if (!attach(sc, fatal)) {
            g.attach_failed = fatal;
            return;
        }
        if (g.visible.load() && g.hwnd != nullptr) { ::PostMessageW(g.hwnd, wm_show_changed, 0, 0); }
    }
    if (sc == g.chain && g.visible.load()) { draw_frame(sc); }
}

struct hook_scope {
    hook_scope() { g.in_hook.fetch_add(1); }
    ~hook_scope() { g.in_hook.fetch_sub(1); }
};

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const hook_scope scope;
    if (!g.shutting_down.load() && (flags & DXGI_PRESENT_TEST) == 0) { on_present(sc); }
    return g.orig_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    const hook_scope scope;
    if (!g.shutting_down.load() && (flags & DXGI_PRESENT_TEST) == 0) { on_present(sc); }
    return g.orig_present1(sc, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain* sc, UINT buffers, UINT w, UINT h, DXGI_FORMAT format, UINT flags)
{
    const hook_scope scope;
    if (sc == g.chain) {
        if (g.backend == state::api::d3d12) {
            wait_idle12(); // our command lists may still use the old buffers
            g.targets12_valid = false;
        } else {
            g.rtv.Reset(); // the buffers are about to go away: nothing of ours may hold them
            g.rtv_w = g.rtv_h = 0;
        }
    }
    return g.orig_resize(sc, buffers, w, h, format, flags);
}

// the game's direct queue: whichever direct queue executes command lists (a compute / copy queue does not count)
void STDMETHODCALLTYPE hook_execute(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
{
    const hook_scope scope;
    if (!g.shutting_down.load() && q != g.last_queue.load(std::memory_order_relaxed)) {
        if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { g.last_queue.store(q); }
    }
    g.orig_execute(q, count, lists);
}

// ---- the vtable ------------------------------------------------------------------------------------------------------

bool patch_slot(void** vt, int index, void* hook, void** original)
{
    DWORD old = 0;
    if (!::VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) { return false; }
    *original = vt[index];
    vt[index] = hook;
    DWORD ignored = 0;
    ::VirtualProtect(&vt[index], sizeof(void*), old, &ignored);
    return true;
}

void unpatch_slot(void** vt, int index, void* hook, void* original)
{
    if (vt[index] != hook) { return; } // (somebody hooked on top of us: leave the chain intact, we pass through from now on)
    DWORD old = 0;
    if (::VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) {
        vt[index] = original;
        DWORD ignored = 0;
        ::VirtualProtect(&vt[index], sizeof(void*), old, &ignored);
    }
}

// a swap chain of our own, only to read the address of the vtable
void** find_swap_chain_vtable()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"strata_overlay_probe";
    ::RegisterClassExW(&wc);
    HWND probe = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (probe == nullptr) {
        set_error("could not create the probe window");
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 1;
    sd.BufferDesc.Width  = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = probe;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain>      chain;
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    void** vt = nullptr;
    if (SUCCEEDED(::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &chain, &device, &got, &context))) {
        vt = *reinterpret_cast<void***>(chain.Get());
        ComPtr<IDXGISwapChain1> sc1;
        g.has_sc1 = SUCCEEDED(chain.As(&sc1)); // (only then is Present1 in the table)
    } else {
        set_error("could not create the probe swap chain");
    }
    chain.Reset();
    context.Reset();
    device.Reset();
    ::DestroyWindow(probe);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
}

// a direct3d 12 device and queue of our own, only to read the address of the queue's vtable
void** find_queue_vtable()
{
    HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
    if (d3d12 == nullptr) { return nullptr; }
    using create_fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = reinterpret_cast<create_fn>(::GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (create == nullptr) { return nullptr; }
    ComPtr<ID3D12Device> device;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) { return nullptr; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { return nullptr; }
    return *reinterpret_cast<void***>(queue.Get());
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------------------------------

bool install(const options& opt)
{
    if (g.installed.exchange(true)) {
        return true;
    }
    g.opt = opt;
    g.visible.store(opt.start_visible);
    g.vtable = find_swap_chain_vtable();
    if (g.vtable == nullptr) {
        g.installed.store(false);
        return false;
    }
    g.patched_present  = patch_slot(g.vtable, vt_present, reinterpret_cast<void*>(&hook_present), reinterpret_cast<void**>(&g.orig_present));
    g.patched_present1 = g.has_sc1 && patch_slot(g.vtable, vt_present1, reinterpret_cast<void*>(&hook_present1), reinterpret_cast<void**>(&g.orig_present1));
    log_line("vtable %p, present1 %d", static_cast<void*>(g.vtable), g.has_sc1 ? 1 : 0);
    g.patched_resize   = patch_slot(g.vtable, vt_resize_buffers, reinterpret_cast<void*>(&hook_resize), reinterpret_cast<void**>(&g.orig_resize));
    if (!g.patched_present || !g.patched_resize) {
        set_error("could not patch the swap chain's vtable");
        uninstall();
        return false;
    }
    // direct3d 12 games present through the same swap chain class; what is needed on top is their queue
    g.queue_vtable = find_queue_vtable();
    if (g.queue_vtable != nullptr) {
        g.patched_execute = patch_slot(g.queue_vtable, vt_queue_execute, reinterpret_cast<void*>(&hook_execute), reinterpret_cast<void**>(&g.orig_execute));
    }
    log_line("queue vtable %p, patched %d", static_cast<void*>(g.queue_vtable), g.patched_execute ? 1 : 0);
    return true;
}

void uninstall()
{
    log_line("uninstall");
    if (!g.installed.load()) { return; }
    // hidden first: the cursor comes back to the game on its own thread
    if (g.hwnd != nullptr && g.subclassed && ::IsWindow(g.hwnd)) {
        g.visible.store(false);
        DWORD_PTR ignored = 0;
        ::SendMessageTimeoutW(g.hwnd, wm_show_changed, 0, 0, SMTO_ABORTIFHUNG, 500, &ignored);
    }
    g.shutting_down.store(true);
    if (g.vtable != nullptr) {
        if (g.patched_present)  { unpatch_slot(g.vtable, vt_present, reinterpret_cast<void*>(&hook_present), reinterpret_cast<void*>(g.orig_present)); }
        if (g.patched_present1) { unpatch_slot(g.vtable, vt_present1, reinterpret_cast<void*>(&hook_present1), reinterpret_cast<void*>(g.orig_present1)); }
        if (g.patched_resize)   { unpatch_slot(g.vtable, vt_resize_buffers, reinterpret_cast<void*>(&hook_resize), reinterpret_cast<void*>(g.orig_resize)); }
    }
    if (g.queue_vtable != nullptr && g.patched_execute) {
        unpatch_slot(g.queue_vtable, vt_queue_execute, reinterpret_cast<void*>(&hook_execute), reinterpret_cast<void*>(g.orig_execute));
    }
    if (g.subclassed && ::IsWindow(g.hwnd)) {
        if (reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g.hwnd, GWLP_WNDPROC)) == &wnd_proc) {
            ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.orig_proc));
        } else {
            set_error("the window was subclassed on top of the overlay: it stays as a pass-through, do not unload the dll");
            return; // (the state has to outlive the window procedure)
        }
    }
    log_line("uninstall: hooks and window procedure restored");
    for (int i = 0; i < 100 && g.in_hook.load() > 0; ++i) { ::Sleep(20); } // (a Present on another thread may still be inside a hook)
    ::Sleep(50);
    g.rtv.Reset();
    wait_idle12();
    g.renderer.destroy();
    g.renderer12.destroy();
    g.list12.Reset();
    g.frames12.clear();
    g.rtv_heap12.Reset();
    g.fence12.Reset();
    if (g.fence_event12 != nullptr) { ::CloseHandle(g.fence_event12); g.fence_event12 = nullptr; }
    g.queue12.Reset();
    g.dev12.Reset();
    g.targets12_valid = false;
    g.backend = state::api::none;
    g.last_queue.store(nullptr);
    g.ui.reset();
    g.ctx.Reset();
    g.device.Reset();
    g.chain      = nullptr;
    g.hwnd       = nullptr;
    g.subclassed = false;
    g.installed.store(false);
    log_line("uninstall: done");
}

void show(bool v)
{
    g.visible.store(v);
    if (g.hwnd != nullptr && g.subclassed) { ::PostMessageW(g.hwnd, wm_show_changed, 0, 0); }
}

bool visible() noexcept { return g.visible.load(); }
unsigned long long frames() noexcept { return g.frame_count.load(); }
const char* last_error() noexcept { return g.error; }
bool attached() noexcept { return g.chain != nullptr && g.subclassed; }
void* window() noexcept { return g.hwnd; }

} // namespace strata::overlay

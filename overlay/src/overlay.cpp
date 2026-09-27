#include <strata/overlay/overlay.hpp>

#include <strata/backend/d3d11.hpp>
#include <strata/backend/d3d12.hpp>
#include <strata/platform/win32.hpp>

#include <windows.h>
#include <windowsx.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
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
using resize1_fn  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using colorspace_fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

using execute_fn  = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

constexpr int vt_queue_execute  = 10; // ID3D12CommandQueue::ExecuteCommandLists
constexpr int vt_present        = 8;
constexpr int vt_resize_buffers = 13;
constexpr int vt_present1       = 22;
constexpr int vt_set_colorspace1 = 38; // IDXGISwapChain3::SetColorSpace1
constexpr int vt_resize_buffers1 = 39; // IDXGISwapChain3::ResizeBuffers1

struct state {
    options              opt;
    std::atomic<bool>    installed{false};
    std::atomic<bool>    shutting_down{false};
    std::atomic<bool>    visible{false};
    std::atomic<bool>    capture_mouse{false};
    std::atomic<bool>    capture_keys{false};
    std::atomic<u64>     frame_count{0};
    std::atomic<int>     in_hook{0};
    // a ui scale asked for from another thread, taken up by the render thread at the start of the next frame
    // (rebuilding the atlas and handing it to the renderer is not something another thread may do)
    std::atomic<float>   want_scale{0.0f};
    float                base_scale{1.0f}; // what Ctrl+0 goes back to
    char                 error[256]{};

    void**        vtable{};
    bool          has_sc1{};
    bool          has_sc3{};
    present_fn    orig_present{};
    present1_fn   orig_present1{};
    resize_fn     orig_resize{};
    resize1_fn    orig_resize1{};
    colorspace_fn orig_colorspace{};
    bool          patched_present{}, patched_present1{}, patched_resize{}, patched_resize1{}, patched_colorspace{};

    // the colour space a game declared for a swap chain (SetColorSpace1), and whether the output encoding has to be
    // worked out again (a resize can change the format, a game can switch hdr on and off)
    std::atomic<IDXGISwapChain*>     colorspace_chain{nullptr};
    std::atomic<int>                 colorspace{-1};
    std::atomic<bool>                output_dirty{true};
    std::atomic<int>                 output_now{0}; // the output_space in use (for output_space_in_use)
    // presents by a swap chain other than the attached one, since the attached one last presented: the game has
    // replaced its swap chain (or its window) when this keeps growing
    unsigned                         foreign_presents{};
    DXGI_FORMAT                      renderer_format12{};

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

[[nodiscard]] ComPtr<ID3D12CommandQueue> game_queue12(ID3D12Device* device);

// everything made on the game's device: gone when the game replaces its device (or the overlay is uninstalled). the ui
// context stays -- its windows, their places and what the user typed survive a new device
void release_device_objects()
{
    wait_idle12();
    g.renderer.destroy();
    g.renderer12.destroy();
    g.list12.Reset();
    g.frames12.clear();
    g.rtv_heap12.Reset();
    g.fence12.Reset();
    if (g.fence_event12 != nullptr) { ::CloseHandle(g.fence_event12); g.fence_event12 = nullptr; }
    g.fence_next12    = 0;
    g.queue12.Reset();
    g.dev12.Reset();
    g.targets12_valid = false;
    g.renderer_format12 = DXGI_FORMAT_UNKNOWN;
    g.ctx.Reset();
    g.device.Reset();
    g.backend = state::api::none;
}

// the d3d12 renderer is built for one back buffer format (its pipelines have it baked in); made again when the format changes
bool create_renderer12(DXGI_FORMAT format, UINT buffers)
{
    wait_idle12();
    g.renderer12.destroy();
    g.renderer_frames12 = std::max<UINT>(buffers, 2);
    d3d12_init_info info;
    info.device           = g.dev12.Get();
    info.queue            = g.queue12.Get();
    info.frames_in_flight = g.renderer_frames12;
    info.rtv_format       = static_cast<u32>(format);
    if (!g.renderer12.create(info, g.ui->font())) { return false; }
    g.renderer_format12 = format;
    g.output_dirty.store(true);
    if (g.fence12 == nullptr) {
        g.fence_event12 = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<ID3D12CommandAllocator> first;
        if (g.fence_event12 == nullptr || FAILED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12))) ||
            FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&first))) ||
            FAILED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, first.Get(), nullptr, IID_PPV_ARGS(&g.list12)))) {
            return false;
        }
        g.list12->Close();
    }
    return true;
}

// the atlas pixels were released after the first renderer took them: a renderer made later needs them built again
bool ensure_font_pixels()
{
    if (!g.ui->font().pixels().empty()) { return true; }
    if (!g.ui->rebuild_font_atlas()) { set_error("could not rebuild the font atlas"); return false; }
    return true;
}

void subclass_window(HWND hwnd)
{
    if (g.hwnd == hwnd && g.subclassed) { return; }
    // a window of the game's that is going away (or has): give it its procedure back if it still has ours
    if (g.subclassed && g.hwnd != nullptr && ::IsWindow(g.hwnd) &&
        reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(g.hwnd, GWLP_WNDPROC)) == &wnd_proc) {
        ::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.orig_proc));
    }
    g.hwnd = hwnd;
    {
        const std::lock_guard lock{g.platform_mutex};
        g.platform.attach(g.hwnd);
    }
    if (g.ui != nullptr) { g.ui->set_clipboard(g.platform.clipboard()); }
    g.orig_proc  = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wnd_proc)));
    g.subclassed = g.orig_proc != nullptr;
    if (!g.subclassed) { set_error("could not subclass the game's window"); }
}

// false: not yet (try the next Present); `fatal`: never (this is not a direct3d 11 / 12 game, or the ui cannot be built).
// runs again when the game replaces its swap chain: what still fits (the ui, the device's objects) is kept
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
        // direct3d 12: the ui is submitted on the game's direct queue (not known yet with the fallback: try the next Present)
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

// the game let go of the swap chain the ui was drawn on: what was made for its buffers goes, the rest stays for the next one
void detach_chain()
{
    wait_idle12();
    g.targets12_valid = false;
    g.chain            = nullptr;
    g.foreign_presents = 0;
}

// ---- output encoding ------------------------------------------------------------------------------------------------

// is the monitor the swap chain is on in hdr mode (Windows "Use HDR")
[[nodiscard]] bool output_is_hdr(IDXGISwapChain* sc)
{
    ComPtr<IDXGIOutput> out;
    ComPtr<IDXGIOutput6> out6;
    DXGI_OUTPUT_DESC1 od{};
    return SUCCEEDED(sc->GetContainingOutput(&out)) && SUCCEEDED(out.As(&out6)) && SUCCEEDED(out6->GetDesc1(&od)) &&
           od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

// how the ui's colours go into the game's frame: forced by the options, or read off the swap chain -- an FP16 one is scRGB,
// a 10-bit one is HDR10 when the game declared it (SetColorSpace1) or, when that happened before the overlay was there, when
// the monitor is in hdr mode; everything else is srgb
[[nodiscard]] output_desc detect_output(IDXGISwapChain* sc)
{
    output_desc out;
    out.paper_white_nits = g.opt.hdr_paper_white_nits;
    if (g.opt.output.has_value()) {
        out.space = *g.opt.output;
        return out;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d))) { return out; }
    const int declared = g.colorspace_chain.load() == sc ? g.colorspace.load() : -1;
    if (declared == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)        { out.space = output_space::scrgb; }
    else if (declared == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) { out.space = output_space::hdr10; }
    else if (declared >= 0)                                          { out.space = output_space::srgb; }
    else if (d.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)  { out.space = output_space::scrgb; }
    else if (d.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && output_is_hdr(sc)) { out.space = output_space::hdr10; }
    // an scRGB swap chain on a monitor that is not in hdr mode: 1.0 is already the display's white, and a paper white
    // above 80 nits would push the whole ui past it -- blown out to glaring white
    if (out.space == output_space::scrgb && !output_is_hdr(sc)) { out.paper_white_nits = 80.0f; }
    return out;
}

void apply_output(IDXGISwapChain* sc)
{
    if (!g.output_dirty.exchange(false)) { return; }
    const output_desc out = detect_output(sc);
    g.renderer.set_output(out);
    g.renderer12.set_output(out);
    g.output_now.store(static_cast<int>(out.space));
    log_line("output: %s", out.space == output_space::scrgb ? "scRGB" : out.space == output_space::hdr10 ? "HDR10" : "srgb");
}

// a view of the back buffer for this frame only. it is not kept between frames on purpose: a view holds the buffer, and a
// held buffer keeps the game's swap chain alive after the game released it -- so the game's next CreateSwapChain on the same
// window would fail (a window has one flip-model swap chain at a time), which is how an overlay breaks a resolution change
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

// a scale change asked for from anywhere is applied here, on the render thread: the atlas is rebuilt and handed to
// the renderer, which is the step an app driving set_scale() itself has to remember and usually forgets -- text then
// draws from a texture that no longer matches the glyph coordinates.
void apply_pending_scale()
{
    const float wanted = g.want_scale.exchange(0.0f);
    if (wanted <= 0.0f || g.ui == nullptr) {
        return;
    }
    const u32 before = g.ui->font_generation();
    if (!g.ui->set_scale(wanted) || g.ui->font_generation() == before) {
        return; // unchanged, or the atlas could not be built at that size: keep the one that works
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

    input_state input;
    {
        const std::lock_guard lock{g.platform_mutex};
        input = g.platform.new_frame();
    }
    g.ui->begin_frame(std::move(input));
    if (g.opt.scale_hotkeys && !g.ui->want_text_input()) {
        //  Ctrl + Plus / Minus (both the main row and the numeric keypad), Ctrl + 0 back to where it started
        const auto stepped = [&](int direction) {
            const float next = std::clamp(g.ui->scale() + 0.1f * static_cast<float>(direction), 0.5f, 4.0f);
            g.want_scale.store(next);
        };
        if (g.ui->key_pressed(VK_OEM_PLUS, true) || g.ui->key_pressed(VK_ADD, true))        { stepped(1); }
        else if (g.ui->key_pressed(VK_OEM_MINUS, true) || g.ui->key_pressed(VK_SUBTRACT, true)) { stepped(-1); }
        else if (g.ui->key_pressed('0', true) || g.ui->key_pressed(VK_NUMPAD0, true))       { g.want_scale.store(g.base_scale); }
    }
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

void on_present(IDXGISwapChain* sc)
{
    static int calls = 0;
    if (calls++ < 3) { log_line("present %d on %p (attached chain %p)", calls, static_cast<void*>(sc), static_cast<void*>(g.chain)); }
    if (g.chain != nullptr && sc != g.chain) {
        // another swap chain presents. on the same window it can only be the attached one's replacement (a window has one
        // flip-model swap chain at a time): switch now. on another window it may be a second view of the game's, so only
        // once the attached one has stopped presenting for a good while (the game made a new window as well)
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
        if (g.backend == state::api::d3d12) { // (d3d11: the overlay holds no view of the buffers between frames)
            wait_idle12(); // our command lists may still use the old buffers
            g.targets12_valid = false;
        }
    }
    g.output_dirty.store(true);
    return g.orig_resize(sc, buffers, w, h, format, flags);
}

// d3d12 games often resize through this one (it takes a queue per buffer); the overlay's targets go the same way
HRESULT STDMETHODCALLTYPE hook_resize1(IDXGISwapChain3* sc, UINT buffers, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
                                       const UINT* node_masks, IUnknown* const* queues)
{
    const hook_scope scope;
    if (sc == g.chain) {
        if (g.backend == state::api::d3d12) {
            wait_idle12();
            g.targets12_valid = false;
        }
    }
    g.output_dirty.store(true);
    return g.orig_resize1(sc, buffers, w, h, format, flags, node_masks, queues);
}

// the game declares its hdr (or not): remembered, so the ui is encoded to match
HRESULT STDMETHODCALLTYPE hook_colorspace(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE space)
{
    const hook_scope scope;
    const HRESULT hr = g.orig_colorspace(sc, space);
    if (SUCCEEDED(hr)) {
        g.colorspace_chain.store(sc);
        g.colorspace.store(static_cast<int>(space));
        g.output_dirty.store(true);
    }
    return hr;
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

// the swap chain vtable belongs to dxgi and is the same whichever api made the swap chain, so one probe of either api finds
// it. the probe uses the api the game has already loaded: a d3d11 game never gets d3d12 (and its driver) loaded into it by
// the overlay, nor a d3d12 game d3d11. both or neither loaded (injected before the game made its device): d3d11
[[nodiscard]] bool probe_with_d3d12() noexcept
{
    return ::GetModuleHandleW(L"d3d12.dll") != nullptr && ::GetModuleHandleW(L"d3d11.dll") == nullptr;
}

[[nodiscard]] void** probe_d3d11(HWND probe)
{
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
    if (FAILED(::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &chain, &device, &got, &context))) {
        return nullptr;
    }
    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGISwapChain3> sc3;
    g.has_sc1 = SUCCEEDED(chain.As(&sc1)); // (only then is Present1 in the table)
    g.has_sc3 = SUCCEEDED(chain.As(&sc3)); // (... and SetColorSpace1 / ResizeBuffers1)
    return *reinterpret_cast<void***>(chain.Get());
}

[[nodiscard]] void** probe_d3d12(HWND probe)
{
    // d3d12.dll is already in the process (that is why it was picked): no LoadLibrary, nothing to free
    using create_fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    const auto create = reinterpret_cast<create_fn>(::GetProcAddress(::GetModuleHandleW(L"d3d12.dll"), "D3D12CreateDevice"));
    ComPtr<ID3D12Device>       device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory2>      factory;
    D3D12_COMMAND_QUEUE_DESC   qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (create == nullptr || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = 64;
    sd.Height           = 64;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    ComPtr<IDXGISwapChain>  chain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), probe, &sd, nullptr, nullptr, &sc1)) || FAILED(sc1.As(&chain))) {
        return nullptr;
    }
    ComPtr<IDXGISwapChain3> sc3;
    g.has_sc1 = true;
    g.has_sc3 = SUCCEEDED(chain.As(&sc3));
    return *reinterpret_cast<void***>(chain.Get());
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
    const bool use12 = probe_with_d3d12();
    void** vt = use12 ? probe_d3d12(probe) : probe_d3d11(probe); // (the probe's objects are gone again when it returns)
    log_line("probe: %s", use12 ? "direct3d 12" : "direct3d 11");
    if (vt == nullptr) { set_error("could not create the probe swap chain"); }
    ::DestroyWindow(probe);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
}

// the game's direct queue is found by hooking ExecuteCommandLists. the vtable comes from a queue made on the game's own device
// when a d3d12 swap chain first presents: no device of the overlay's, and nothing at all for a game that is not direct3d 12
void hook_queue_vtable(ID3D12Device* device)
{
    if (g.patched_execute) { return; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { return; }
    g.queue_vtable    = *reinterpret_cast<void***>(queue.Get());
    g.patched_execute = patch_slot(g.queue_vtable, vt_queue_execute, reinterpret_cast<void*>(&hook_execute), reinterpret_cast<void**>(&g.orig_execute));
    log_line("queue vtable %p, patched %d", static_cast<void*>(g.queue_vtable), g.patched_execute ? 1 : 0);
}

// the game's direct queue: the one seen executing command lists (IDXGISwapChain::GetDevice does not hand it out). null until
// one has been seen, which takes a frame after the hook goes in
[[nodiscard]] ComPtr<ID3D12CommandQueue> game_queue12(ID3D12Device* device)
{
    hook_queue_vtable(device);
    return g.last_queue.load();
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
    if (g.has_sc3) {
        g.patched_resize1    = patch_slot(g.vtable, vt_resize_buffers1, reinterpret_cast<void*>(&hook_resize1), reinterpret_cast<void**>(&g.orig_resize1));
        g.patched_colorspace = patch_slot(g.vtable, vt_set_colorspace1, reinterpret_cast<void*>(&hook_colorspace), reinterpret_cast<void**>(&g.orig_colorspace));
    }
    if (!g.patched_present || !g.patched_resize) {
        set_error("could not patch the swap chain's vtable");
        uninstall();
        return false;
    }
    // direct3d 12 games present through the same swap chain class; their queue is found when they first present (game_queue12)
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
        if (g.patched_resize1)  { unpatch_slot(g.vtable, vt_resize_buffers1, reinterpret_cast<void*>(&hook_resize1), reinterpret_cast<void*>(g.orig_resize1)); }
        if (g.patched_colorspace) { unpatch_slot(g.vtable, vt_set_colorspace1, reinterpret_cast<void*>(&hook_colorspace), reinterpret_cast<void*>(g.orig_colorspace)); }
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
    release_device_objects();
    g.last_queue.store(nullptr);
    g.ui.reset();
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

void set_ui_scale(float scale) noexcept
{
    g.want_scale.store(std::clamp(scale, 0.5f, 4.0f));
}

float ui_scale() noexcept
{
    const float wanted = g.want_scale.load();
    if (wanted > 0.0f) { return wanted; } // asked for, not applied yet
    return g.ui != nullptr ? g.ui->scale() : g.base_scale;
}
unsigned long long frames() noexcept { return g.frame_count.load(); }
const char* last_error() noexcept { return g.error; }
output_space output_space_in_use() noexcept { return static_cast<output_space>(g.output_now.load()); }
bool attached() noexcept { return g.chain != nullptr && g.subclassed; }
void* window() noexcept { return g.hwnd; }

} // namespace strata::overlay

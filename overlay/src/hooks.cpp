#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

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

// d3d12 games often resize through this one (a queue per buffer); our targets follow
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

// the game's direct queue: whichever direct queue executes command lists (compute / copy do not count)
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

// clears `patched` once the slot holds `original` again. false while another hook sits in the slot (and calls ours):
// the chain is kept intact and ours passes through, so the dll must stay loaded
bool unpatch_slot(void** vt, int index, void* hook, void* original, bool& patched)
{
    if (!patched) { return true; }
    if (vt[index] != hook) { return false; }
    DWORD old = 0;
    if (!::VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) { return false; }
    vt[index] = original;
    DWORD ignored = 0;
    ::VirtualProtect(&vt[index], sizeof(void*), old, &ignored);
    patched = false;
    return true;
}

// the swap chain vtable belongs to dxgi and is shared across apis, so one probe finds it. probe with the api the
// game already loaded so we never load the other one (and its driver) into it; both or neither loaded: d3d11
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
    // d3d12.dll is already loaded (that is why it was picked): no LoadLibrary, nothing to free
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

// the game's direct queue is found by hooking ExecuteCommandLists, with the vtable from a queue made on the game's
// own device when a d3d12 swap chain first presents (nothing for non-d3d12 games)
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

} // namespace

// the game's direct queue as seen executing command lists (GetDevice cannot provide it). null until seen, a frame
// after hooking
[[nodiscard]] ComPtr<ID3D12CommandQueue> game_queue12(ID3D12Device* device)
{
    hook_queue_vtable(device);
    return g.last_queue.load();
}

hook_result install_hooks()
{
    g.vtable = find_swap_chain_vtable();
    if (g.vtable == nullptr) { return hook_result::no_vtable; }
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
        return hook_result::patch_failed;
    }
    // d3d12 games present through the same swap chain class; their queue is found at first present (game_queue12)
    return hook_result::ok;
}

bool remove_hooks()
{
    bool all = true;
    if (g.vtable != nullptr) {
        all &= unpatch_slot(g.vtable, vt_present, reinterpret_cast<void*>(&hook_present), reinterpret_cast<void*>(g.orig_present), g.patched_present);
        all &= unpatch_slot(g.vtable, vt_present1, reinterpret_cast<void*>(&hook_present1), reinterpret_cast<void*>(g.orig_present1), g.patched_present1);
        all &= unpatch_slot(g.vtable, vt_resize_buffers, reinterpret_cast<void*>(&hook_resize), reinterpret_cast<void*>(g.orig_resize), g.patched_resize);
        all &= unpatch_slot(g.vtable, vt_resize_buffers1, reinterpret_cast<void*>(&hook_resize1), reinterpret_cast<void*>(g.orig_resize1), g.patched_resize1);
        all &= unpatch_slot(g.vtable, vt_set_colorspace1, reinterpret_cast<void*>(&hook_colorspace), reinterpret_cast<void*>(g.orig_colorspace), g.patched_colorspace);
    }
    if (g.queue_vtable != nullptr) {
        all &= unpatch_slot(g.queue_vtable, vt_queue_execute, reinterpret_cast<void*>(&hook_execute), reinterpret_cast<void*>(g.orig_execute), g.patched_execute);
    }
    return all;
}

} // namespace strata::overlay::detail

#pragma once

// shared state and entry points of the overlay's implementation files. not public: overlay.hpp is the API.
//   diagnostics  capture  render_d3d12  window  attach  output  frame  hooks  overlay (the public functions)

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
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

namespace strata::overlay::detail {

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
    std::atomic<bool>    draw_hidden{false};
    std::mutex                        post_mutex;
    std::vector<std::function<void()>> posted; // run on the render thread at the start of the next drawn frame
    std::atomic<bool>    capture_mouse{false};
    std::atomic<bool>    capture_keys{false};
    std::atomic<u64>     frame_count{0};
    std::atomic<int>     in_hook{0};
    // ui scale requested from another thread, applied on the render thread next frame (only it may rebuild the atlas)
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

    // colour space the game declared (SetColorSpace1), and whether the output encoding needs re-deriving (a resize
    // can change the format, games can toggle hdr)
    std::atomic<IDXGISwapChain*>     colorspace_chain{nullptr};
    std::atomic<int>                 colorspace{-1};
    std::atomic<bool>                output_dirty{true};
    std::atomic<int>                 output_now{0}; // the output_space in use (for output_space_in_use)
    // presents by other swap chains since the attached one last presented: growing means the game replaced its swap
    // chain (or window)
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
    std::recursive_mutex             platform_mutex; // window thread writes, render thread reads (re-entrant wndproc)
    unsigned                         visible_frames{};
    bool                             captured{};
    bool                             attach_failed{};
    int                              cursor_shown{};  // ShowCursor increments made while visible, taken back when hidden
};

extern state& g;

// diagnostics.cpp
void log_line(const char* fmt, ...);
void set_error(const char* what) noexcept;

// capture.cpp: png files of the back buffer (a debug aid, options::capture_path)
[[nodiscard]] bool write_png(const char* path, const std::vector<u8>& rgba, u32 w, u32 h);
void capture_back_buffer(IDXGISwapChain* sc); // direct3d 11

// render_d3d12.cpp
void wait_idle12();
void render12(IDXGISwapChain* sc, const draw_data& data, bool capture);
bool create_renderer12(DXGI_FORMAT format, UINT buffers);
void release_device_objects(); // everything made on the game's device (both apis)

// window.cpp: the subclassed game window (input, cursor, toggle key)
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void subclass_window(HWND hwnd);

// attach.cpp
bool attach(IDXGISwapChain* sc, bool& fatal);
void detach_chain();
bool ensure_font_pixels();

// output.cpp
void apply_output(IDXGISwapChain* sc);

// frame.cpp
void on_present(IDXGISwapChain* sc); // called from the Present hooks

// hooks.cpp: the vtable patches
enum class hook_result { ok, no_vtable, patch_failed };
[[nodiscard]] hook_result install_hooks();
void remove_hooks();
[[nodiscard]] ComPtr<ID3D12CommandQueue> game_queue12(ID3D12Device* device);

} // namespace strata::overlay::detail

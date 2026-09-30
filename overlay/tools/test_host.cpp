// a stand-in game: a window, a flip-model d3d11 (or --d3d12) swap chain, a changing clear colour and Present.
//   strata_overlay_host.exe [--d3d12] [--fp16] --dll <overlay dll>
// loads the dll like an injector, waits for the hook, then checks: nothing drawn while hidden, F1 shows it, F1 hides
// it, and the overlay follows a swap chain replacement (like a resolution change). --fp16 uses an scRGB swap chain
// the overlay must detect. exit code 0 = passed.

#include <windows.h>
#include <shellapi.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using Microsoft::WRL::ComPtr;

LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_DESTROY: ::PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    default: return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

using frames_fn   = unsigned long long (*)();
using attached_fn = int (*)();
using error_fn    = const char* (*)();
using output_fn   = int (*)();

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    std::wstring dll;
    bool use12 = false;
    bool fp16  = false;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (std::wstring{argv[i]} == L"--dll" && i + 1 < argc) { dll = argv[++i]; }
        else if (std::wstring{argv[i]} == L"--d3d12") { use12 = true; }
        else if (std::wstring{argv[i]} == L"--fp16") { fp16 = true; }
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = host_proc;
    wc.hInstance     = inst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"strata_overlay_host";
    ::RegisterClassExW(&wc);
    RECT rc{0, 0, 960, 540};
    ::AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"overlay test host", WS_OVERLAPPEDWINDOW, 100, 100, rc.right - rc.left, rc.bottom - rc.top,
                                  nullptr, nullptr, inst, nullptr);
    if (hwnd == nullptr) { return 10; }
    ::ShowWindow(hwnd, SW_SHOW);
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); // (nothing may cover it: the test moves the cursor over it)
    ::SetForegroundWindow(hwnd);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12CommandAllocator> alloc12;
    ComPtr<ID3D12GraphicsCommandList> list12;
    ComPtr<ID3D12Fence> fence12;
    UINT rtv_size = 0;
    UINT64 fence_value = 0;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 960; sd.Height = 540;
    sd.Format = fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (!use12) {
        constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device, nullptr, &context))) { return 11; }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { return 12; }
        if (FAILED(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &chain))) { return 13; }
    } else {
        if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { return 12; }
        if (FAILED(::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)))) { return 11; }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12)))) { return 14; }
        if (FAILED(factory->CreateSwapChainForHwnd(queue12.Get(), hwnd, &sd, nullptr, nullptr, &chain))) { return 13; }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        if (FAILED(device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { return 15; }
        rtv_size = device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc12))) ||
            FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12.Get(), nullptr, IID_PPV_ARGS(&list12))) ||
            FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12)))) { return 16; }
        list12->Close();
    }

    // a resolution / mode change: release the swap chain and create a new one on the same window. only works once the
    // old one is really gone, so this also checks the overlay holds nothing of it
    const auto recreate_chain = [&]() -> bool {
        if (use12) {
            queue12->Signal(fence12.Get(), ++fence_value);
            HANDLE e = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence12->SetEventOnCompletion(fence_value, e);
            ::WaitForSingleObject(e, 2000);
            ::CloseHandle(e);
        } else {
            context->ClearState();
            context->Flush();
        }
        chain.Reset();
        IUnknown* owner = use12 ? static_cast<IUnknown*>(queue12.Get()) : static_cast<IUnknown*>(device.Get());
        return SUCCEEDED(factory->CreateSwapChainForHwnd(owner, hwnd, &sd, nullptr, nullptr, &chain));
    };

    HMODULE module = nullptr;
    frames_fn frames = nullptr;
    attached_fn attached = nullptr;
    int failures = 0;
    bool quit = false;

    // one frame of the "game", paced like a 60 fps one (a hidden window may not block in Present)
    int frame_number = 0;
    const auto run_frame = [&] {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        const float t = static_cast<float>(frame_number++) * 0.02f;
        float color[4] = {0.10f + 0.05f * std::sin(t), 0.16f, 0.24f + 0.05f * std::cos(t), 1.0f};
        if (fp16) { // FP16 is linear: write the dark blue linearised or it shows bright
            for (int k = 0; k < 3; ++k) { color[k] = std::pow(color[k], 2.2f); }
        }
        if (!use12) {
            ComPtr<ID3D11Texture2D> back;
            ComPtr<ID3D11RenderTargetView> rtv;
            chain->GetBuffer(0, IID_PPV_ARGS(&back));
            device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
            ID3D11RenderTargetView* target = rtv.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->ClearRenderTargetView(target, color);
        } else {
            ComPtr<IDXGISwapChain3> sc3;
            chain.As(&sc3);
            const UINT idx = sc3->GetCurrentBackBufferIndex();
            ComPtr<ID3D12Resource> back;
            chain->GetBuffer(idx, IID_PPV_ARGS(&back));
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(idx) * rtv_size;
            device12->CreateRenderTargetView(back.Get(), nullptr, h);
            alloc12->Reset();
            list12->Reset(alloc12.Get(), nullptr);
            D3D12_RESOURCE_BARRIER br{};
            br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            br.Transition.pResource = back.Get();
            br.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            br.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list12->ResourceBarrier(1, &br);
            list12->ClearRenderTargetView(h, color, 0, nullptr);
            std::swap(br.Transition.StateBefore, br.Transition.StateAfter);
            list12->ResourceBarrier(1, &br);
            list12->Close();
            ID3D12CommandList* lists[] = {list12.Get()};
            queue12->ExecuteCommandLists(1, lists);
        }
        chain->Present(0, 0); // (with the overlay loaded this goes through its hook)
        if (use12) { // a simple game: wait for the frame
            queue12->Signal(fence12.Get(), ++fence_value);
            if (fence12->GetCompletedValue() < fence_value) {
                HANDLE e = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                fence12->SetEventOnCompletion(fence_value, e);
                ::WaitForSingleObject(e, 2000);
                ::CloseHandle(e);
            }
        }
        ::Sleep(16);
    };
    const auto run_frames = [&](int n) { for (int i = 0; i < n && !quit; ++i) { run_frame(); } };

    // a click on the demo window (positions in a 960 x 540 client area)
    const auto click = [&](int x, int y) {
        // move the real cursor too: otherwise the window gets WM_MOUSELEAVE for the synthetic moves
        POINT screen{x, y};
        ::ClientToScreen(hwnd, &screen);
        ::SetCursorPos(screen.x, screen.y);
        ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        run_frames(4);
        ::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        run_frames(3);
        ::SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        run_frames(6);
    };

    run_frames(20);
    // the api this "game" does not use: the overlay must not load it into the process
    const wchar_t* other_api = use12 ? L"d3d11.dll" : L"d3d12.dll";
    const bool other_loaded_before = ::GetModuleHandleW(other_api) != nullptr;
    if (!dll.empty()) {
        module = ::LoadLibraryW(dll.c_str());
        if (module == nullptr) { std::fprintf(stderr, "cannot load the dll (%lu)\n", ::GetLastError()); return 20; }
        frames   = reinterpret_cast<frames_fn>(::GetProcAddress(module, "strata_overlay_frames"));
        attached = reinterpret_cast<attached_fn>(::GetProcAddress(module, "strata_overlay_attached"));
        if (frames == nullptr || attached == nullptr) { std::fprintf(stderr, "the dll lacks its c api\n"); return 21; }

        // the hook installs on a thread of its own and attaches to the swap chain at the next Present
        for (int i = 0; i < 600 && attached() == 0 && !quit; ++i) { run_frame(); }
        if (attached() == 0) {
            const auto err = reinterpret_cast<error_fn>(::GetProcAddress(module, "strata_overlay_last_error"));
            std::fprintf(stderr, "FAIL: the overlay never attached (%s)\n", err != nullptr ? err() : "?");
            ++failures;
        } else {
            if (!other_loaded_before && ::GetModuleHandleW(other_api) != nullptr) {
                std::fprintf(stderr, "FAIL: the overlay loaded %ls into a game that does not use it\n", other_api);
                ++failures;
            }
            run_frames(30);
            if (frames() != 0) { std::fprintf(stderr, "FAIL: the overlay drew while hidden (%llu frames)\n", frames()); ++failures; }

            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // show
            run_frames(60);
            const unsigned long long shown = frames();
            if (shown < 30) { std::fprintf(stderr, "FAIL: F1 did not show the overlay (%llu frames)\n", shown); ++failures; }

            click(128, 96);   // the Log tab
            click(182, 96);   // the Style tab
            click(77, 96);    // Info again
            click(106, 380);  // "write a log line" (switches to the Log tab)
            click(77, 96);    // Info
            click(64, 279);   // the checkbox
            click(300, 333);  // the slider
            if (frames() < shown) { std::fprintf(stderr, "FAIL: the overlay stopped drawing while clicking\n"); ++failures; }

            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // hide
            run_frames(5);
            const unsigned long long hidden_at = frames();
            run_frames(40);
            if (frames() > hidden_at + 1) { std::fprintf(stderr, "FAIL: the overlay kept drawing after F1 (%llu -> %llu)\n", hidden_at, frames()); ++failures; }
            std::printf("overlay drew %llu frames while it was open\n", shown);

            // the encoding the overlay picked for this swap chain
            const auto output_space = reinterpret_cast<output_fn>(::GetProcAddress(module, "strata_overlay_output_space"));
            const int want_space = fp16 ? 2 : 0; // scRGB / srgb
            if (output_space == nullptr || output_space() != want_space) {
                std::fprintf(stderr, "FAIL: the overlay encodes for output space %d, expected %d\n", output_space != nullptr ? output_space() : -1, want_space);
                ++failures;
            }

            // swap chain replaced while open: the overlay must release the old one (or the new one fails)
            // and draw on the new
            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // show
            run_frames(20);
            if (!recreate_chain()) {
                std::fprintf(stderr, "FAIL: a new swap chain could not be made on the window (the overlay still holds the old one)\n");
                ++failures;
            } else {
                const unsigned long long before = frames();
                run_frames(60);
                if (frames() < before + 30) {
                    std::fprintf(stderr, "FAIL: the overlay did not follow to the new swap chain (%llu -> %llu)\n", before, frames());
                    ++failures;
                }
            }
            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // hide again
            run_frames(5);

            // "unload overlay": the dll takes its hooks out and unloads itself; the game carries on
            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // show
            run_frames(20);
            click(77, 96);
            click(222, 380); // the "unload overlay" button
            bool gone = false;
            // (the dll's worker polls for the request, then waits out hooked calls in flight: time, not frames, bounds it)
            const ULONGLONG give_up = ::GetTickCount64() + 5000;
            while (!gone && !quit && ::GetTickCount64() < give_up) {
                run_frame();
                gone = ::GetModuleHandleW(L"strata_overlay_demo.dll") == nullptr;
            }
            if (!gone) { std::fprintf(stderr, "FAIL: the dll did not unload\n"); ++failures; }
            run_frames(60); // the game keeps running with the hooks gone
            ::SendMessageW(hwnd, WM_KEYDOWN, VK_F1, 0); // (nobody listens any more)
        }
    }
    std::printf("%s\n", failures == 0 ? "overlay host: ok" : "overlay host: FAILED");
    ::DestroyWindow(hwnd);
    return failures == 0 ? 0 : 1;
}

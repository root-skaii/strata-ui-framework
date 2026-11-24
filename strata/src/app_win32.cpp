#include "strata/app.hpp"

#include "strata/config.hpp"
#include "strata/frame.hpp"
#include "strata/platform/win32.hpp"
#include "strata/themes.hpp"

#if STRATA_HAS_VULKAN
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif

#include <windows.h>

#if STRATA_HAS_DX11
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#endif

#if STRATA_HAS_OPENGL
#include <GL/gl.h>
#endif

#if STRATA_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace strata {

#if STRATA_HAS_DX11
using Microsoft::WRL::ComPtr;
#endif

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

[[nodiscard]] app_backend resolve_backend(app_backend req) noexcept
{
    if (req != app_backend::auto_select) { return req; }
#if STRATA_HAS_DX11
    return app_backend::d3d11;
#elif STRATA_HAS_OPENGL
    return app_backend::opengl;
#elif STRATA_HAS_VULKAN
    return app_backend::vulkan;
#else
    return app_backend::auto_select;
#endif
}

#if STRATA_HAS_OPENGL
constexpr int kWGL_DRAW_TO_WINDOW_ARB           = 0x2001;
constexpr int kWGL_SUPPORT_OPENGL_ARB           = 0x2010;
constexpr int kWGL_DOUBLE_BUFFER_ARB            = 0x2011;
constexpr int kWGL_PIXEL_TYPE_ARB               = 0x2013;
constexpr int kWGL_TYPE_RGBA_ARB                = 0x202B;
constexpr int kWGL_COLOR_BITS_ARB               = 0x2014;
constexpr int kWGL_ALPHA_BITS_ARB               = 0x201B;
constexpr int kWGL_CONTEXT_MAJOR_VERSION_ARB    = 0x2091;
constexpr int kWGL_CONTEXT_MINOR_VERSION_ARB    = 0x2092;
constexpr int kWGL_CONTEXT_PROFILE_MASK_ARB     = 0x9126;
constexpr int kWGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;

using wgl_choose_pf_fn     = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using wgl_create_ctx_fn    = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using wgl_swap_interval_fn = BOOL(WINAPI*)(int);
#endif

#if STRATA_HAS_VULKAN
constexpr u32 kVkFrames = 2;

void vk_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout old_l, VkImageLayout new_l,
                VkAccessFlags src_a, VkAccessFlags dst_a, VkPipelineStageFlags src_s, VkPipelineStageFlags dst_s) noexcept
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask               = src_a;
    b.dstAccessMask               = dst_a;
    b.oldLayout                   = old_l;
    b.newLayout                   = new_l;
    b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.image                       = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
}
#endif

} // namespace

struct app::impl {
    app_config               cfg;
    app_backend              backend{app_backend::auto_select};
    std::string              title;      // cfg's views are not kept: strings are copied
    std::string              state_path;
    app*                     owner{};    // the app object run() was called on (it may have been moved since create)

    HWND                     hwnd{};
    win32_platform           platform;
    std::unique_ptr<context> ui;

#if STRATA_HAS_DX11
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain2>        chain;
    ComPtr<ID3D11RenderTargetView> rtv;
    HANDLE                         latency_wait{}; // swap chain frame latency waitable object
    bool                           tearing{};
    UINT                           chain_flags{};
    d3d11_renderer                 renderer;
#endif

#if STRATA_HAS_OPENGL
    HDC         gl_dc{};
    HGLRC       gl_rc{};
    gl_renderer gl_ren;
#endif

#if STRATA_HAS_VULKAN
    VkInstance                        vk_instance{VK_NULL_HANDLE};
    VkSurfaceKHR                      vk_surface{VK_NULL_HANDLE};
    VkPhysicalDevice                  vk_phys{VK_NULL_HANDLE};
    VkDevice                          vk_device{VK_NULL_HANDLE};
    VkQueue                           vk_queue{VK_NULL_HANDLE};
    u32                               vk_qfamily{};
    VkSwapchainKHR                    vk_swapchain{VK_NULL_HANDLE};
    VkFormat                          vk_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D                        vk_extent{};
    std::vector<VkImage>              vk_images;
    std::vector<VkImageView>          vk_views;
    std::vector<VkSemaphore>          vk_render_done;
    std::array<VkCommandPool, kVkFrames>   vk_pools{};
    std::array<VkCommandBuffer, kVkFrames> vk_cmds{};
    std::array<VkSemaphore, kVkFrames>     vk_img_avail{};
    std::array<VkFence, kVkFrames>         vk_fences{};
    u32                               vk_slot{};
    vulkan_renderer                   vk_ren;
#endif

    bool minimized{};
    bool quitting{};
    int  exit_code{};
    f32  pending_dpi{};   // WM_DPICHANGED: new scale, applied between frames
    bool resized{};
    u64  frames{};
    u64  frames_drawn{};

    ~impl()
    {
        release_device();
        if (hwnd != nullptr) { ::DestroyWindow(hwnd); }
    }

    [[nodiscard]] vec2 client_size() const noexcept
    {
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        return {static_cast<f32>(std::max<LONG>(1, rc.right - rc.left)),
                static_cast<f32>(std::max<LONG>(1, rc.bottom - rc.top))};
    }

    void release_device() noexcept
    {
#if STRATA_HAS_DX11
        if (ctx != nullptr) { ctx->ClearState(); }
        rtv.Reset();
        renderer.destroy();
        if (latency_wait != nullptr) { ::CloseHandle(latency_wait); latency_wait = nullptr; }
        chain.Reset();
        ctx.Reset();
        device.Reset();
#endif
#if STRATA_HAS_OPENGL
        if (gl_dc != nullptr && gl_rc != nullptr) {
            ::wglMakeCurrent(gl_dc, gl_rc);
            gl_ren.destroy();
            ::wglMakeCurrent(nullptr, nullptr);
            ::wglDeleteContext(gl_rc);
            gl_rc = nullptr;
        }
        if (hwnd != nullptr && gl_dc != nullptr) {
            ::ReleaseDC(hwnd, gl_dc);
            gl_dc = nullptr;
        }
#endif
#if STRATA_HAS_VULKAN
        if (vk_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk_device);
            vk_ren.destroy();
            vk_destroy_swapchain();
            for (u32 i = 0; i < kVkFrames; ++i) {
                if (vk_img_avail[i] != VK_NULL_HANDLE) { vkDestroySemaphore(vk_device, vk_img_avail[i], nullptr); vk_img_avail[i] = VK_NULL_HANDLE; }
                if (vk_fences[i] != VK_NULL_HANDLE)    { vkDestroyFence(vk_device, vk_fences[i], nullptr); vk_fences[i] = VK_NULL_HANDLE; }
                if (vk_pools[i] != VK_NULL_HANDLE)     { vkDestroyCommandPool(vk_device, vk_pools[i], nullptr); vk_pools[i] = VK_NULL_HANDLE; }
                vk_cmds[i] = VK_NULL_HANDLE;
            }
            vkDestroyDevice(vk_device, nullptr);
            vk_device = VK_NULL_HANDLE;
        }
        if (vk_surface != VK_NULL_HANDLE)  { vkDestroySurfaceKHR(vk_instance, vk_surface, nullptr); vk_surface = VK_NULL_HANDLE; }
        if (vk_instance != VK_NULL_HANDLE) { vkDestroyInstance(vk_instance, nullptr); vk_instance = VK_NULL_HANDLE; }
        vk_phys  = VK_NULL_HANDLE;
        vk_queue = VK_NULL_HANDLE;
#endif
    }

#if STRATA_HAS_DX11
    [[nodiscard]] bool d3d11_create_target() noexcept
    {
        rtv.Reset();
        ComPtr<ID3D11Texture2D> back;
        return SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))) &&
               SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &rtv));
    }

    [[nodiscard]] std::expected<void, app_error> create_d3d11()
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
        chain->SetMaximumFrameLatency(1);
        latency_wait = chain->GetFrameLatencyWaitableObject();
        if (!d3d11_create_target()) { return std::unexpected{app_error::swap_chain}; }
        if (ui->font().pixels().empty() && !ui->rebuild_font_atlas()) { return std::unexpected{app_error::fonts}; }
        if (!renderer.create(device.Get(), ctx.Get(), ui->font())) { return std::unexpected{app_error::renderer}; }
        renderer.set_state_restore(false);
        ui->release_font_pixels();
        ui->invalidate();
        return {};
    }
#endif

#if STRATA_HAS_OPENGL
    [[nodiscard]] std::expected<void, app_error> create_opengl()
    {
        wgl_choose_pf_fn     wgl_choose_pf     = nullptr;
        wgl_create_ctx_fn    wgl_create_ctx    = nullptr;
        wgl_swap_interval_fn wgl_swap_interval = nullptr;
        {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof wc;
            wc.lpfnWndProc   = ::DefWindowProcW;
            wc.hInstance     = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = L"strata_app_gl_boot";
            ::RegisterClassExW(&wc);
            if (HWND dummy = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 1, 1,
                                               nullptr, nullptr, wc.hInstance, nullptr);
                dummy != nullptr) {
                HDC ddc = ::GetDC(dummy);
                PIXELFORMATDESCRIPTOR pfd{};
                pfd.nSize      = sizeof pfd;
                pfd.nVersion   = 1;
                pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cAlphaBits = 8;
                const int pf = ::ChoosePixelFormat(ddc, &pfd);
                if (pf != 0 && ::SetPixelFormat(ddc, pf, &pfd)) {
                    if (HGLRC drc = ::wglCreateContext(ddc); drc != nullptr) {
                        ::wglMakeCurrent(ddc, drc);
                        wgl_choose_pf     = reinterpret_cast<wgl_choose_pf_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglChoosePixelFormatARB")));
                        wgl_create_ctx    = reinterpret_cast<wgl_create_ctx_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglCreateContextAttribsARB")));
                        wgl_swap_interval = reinterpret_cast<wgl_swap_interval_fn>(reinterpret_cast<void*>(::wglGetProcAddress("wglSwapIntervalEXT")));
                        ::wglMakeCurrent(nullptr, nullptr);
                        ::wglDeleteContext(drc);
                    }
                }
                ::ReleaseDC(dummy, ddc);
                ::DestroyWindow(dummy);
            }
            ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        }

        gl_dc = ::GetDC(hwnd);
        if (gl_dc == nullptr) { return std::unexpected{app_error::device}; }

        // A Win32 window's pixel format can only be set once; reuse it on reset_device().
        if (::GetPixelFormat(gl_dc) == 0) {
            int pf = 0;
            PIXELFORMATDESCRIPTOR pfd{};
            pfd.nSize    = sizeof pfd;
            pfd.nVersion = 1;
            if (wgl_choose_pf != nullptr) {
                const int attribs[] = {
                    kWGL_DRAW_TO_WINDOW_ARB, 1,
                    kWGL_SUPPORT_OPENGL_ARB, 1,
                    kWGL_DOUBLE_BUFFER_ARB,  1,
                    kWGL_PIXEL_TYPE_ARB,     kWGL_TYPE_RGBA_ARB,
                    kWGL_COLOR_BITS_ARB,     32,
                    kWGL_ALPHA_BITS_ARB,     8,
                    0,
                };
                UINT count = 0;
                if (wgl_choose_pf(gl_dc, attribs, nullptr, 1, &pf, &count) && count > 0) {
                    ::DescribePixelFormat(gl_dc, pf, sizeof pfd, &pfd);
                } else {
                    pf = 0;
                }
            }
            if (pf == 0) {
                pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cAlphaBits = 8;
                pf = ::ChoosePixelFormat(gl_dc, &pfd);
            }
            if (pf == 0 || !::SetPixelFormat(gl_dc, pf, &pfd)) { return std::unexpected{app_error::device}; }
        }

        if (wgl_create_ctx != nullptr) {
            const int ctx_attribs[] = {
                kWGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                kWGL_CONTEXT_MINOR_VERSION_ARB, 3,
                kWGL_CONTEXT_PROFILE_MASK_ARB,  kWGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0,
            };
            gl_rc = wgl_create_ctx(gl_dc, nullptr, ctx_attribs);
        }
        if (gl_rc == nullptr) { gl_rc = ::wglCreateContext(gl_dc); }
        if (gl_rc == nullptr || !::wglMakeCurrent(gl_dc, gl_rc)) { return std::unexpected{app_error::device}; }
        if (wgl_swap_interval != nullptr) { wgl_swap_interval(cfg.vsync ? 1 : 0); }

        if (ui->font().pixels().empty() && !ui->rebuild_font_atlas()) { return std::unexpected{app_error::fonts}; }
        if (!gl_ren.create(ui->font())) { return std::unexpected{app_error::renderer}; }
        gl_ren.set_state_restore(false);
        ui->release_font_pixels();
        ui->invalidate();
        return {};
    }
#endif

#if STRATA_HAS_VULKAN
    void vk_destroy_swapchain() noexcept
    {
        for (VkSemaphore s : vk_render_done) { if (s != VK_NULL_HANDLE) { vkDestroySemaphore(vk_device, s, nullptr); } }
        vk_render_done.clear();
        for (VkImageView v : vk_views) { if (v != VK_NULL_HANDLE) { vkDestroyImageView(vk_device, v, nullptr); } }
        vk_views.clear();
        vk_images.clear();
        if (vk_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(vk_device, vk_swapchain, nullptr);
            vk_swapchain = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] bool vk_create_swapchain() noexcept
    {
        vk_destroy_swapchain();
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys, vk_surface, &caps) != VK_SUCCESS) { return false; }

        u32 fmt_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &fmt_count, nullptr);
        if (fmt_count == 0) { return false; }
        std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &fmt_count, fmts.data());
        VkSurfaceFormatKHR chosen = fmts[0];
        for (const auto& f : fmts) {
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                break;
            }
        }
        vk_format = chosen.format;

        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!cfg.vsync) {
            u32 mc = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &mc, nullptr);
            std::vector<VkPresentModeKHR> modes(mc);
            vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &mc, modes.data());
            for (VkPresentModeKHR m : modes) {
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR || m == VK_PRESENT_MODE_MAILBOX_KHR) { mode = m; break; }
            }
        }

        const vec2 cs = client_size();
        vk_extent = caps.currentExtent.width != UINT32_MAX
                        ? caps.currentExtent
                        : VkExtent2D{std::clamp(static_cast<u32>(cs.x), caps.minImageExtent.width, caps.maxImageExtent.width),
                                     std::clamp(static_cast<u32>(cs.y), caps.minImageExtent.height, caps.maxImageExtent.height)};
        if (vk_extent.width == 0 || vk_extent.height == 0) { return false; }

        u32 count = std::max(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount > 0 && count > caps.maxImageCount) { count = caps.maxImageCount; }

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface          = vk_surface;
        ci.minImageCount    = count;
        ci.imageFormat      = chosen.format;
        ci.imageColorSpace  = chosen.colorSpace;
        ci.imageExtent      = vk_extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
            ci.imageUsage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform     = caps.currentTransform;
        ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode      = mode;
        ci.clipped          = VK_TRUE;
        if (vkCreateSwapchainKHR(vk_device, &ci, nullptr, &vk_swapchain) != VK_SUCCESS) { return false; }

        u32 img_count = 0;
        vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &img_count, nullptr);
        vk_images.resize(img_count);
        vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &img_count, vk_images.data());
        vk_views.resize(img_count, VK_NULL_HANDLE);
        vk_render_done.resize(img_count, VK_NULL_HANDLE);
        for (u32 i = 0; i < img_count; ++i) {
            VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ivci.image                       = vk_images[i];
            ivci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format                      = vk_format;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.layerCount = 1;
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateImageView(vk_device, &ivci, nullptr, &vk_views[i]) != VK_SUCCESS ||
                vkCreateSemaphore(vk_device, &sci, nullptr, &vk_render_done[i]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::expected<void, app_error> create_vulkan()
    {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "strata_app";
        ai.apiVersion       = VK_API_VERSION_1_0;
        const char* exts[]  = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo        = &ai;
        ici.enabledExtensionCount   = 2;
        ici.ppEnabledExtensionNames = exts;
        if (vkCreateInstance(&ici, nullptr, &vk_instance) != VK_SUCCESS) { return std::unexpected{app_error::device}; }

        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = ::GetModuleHandleW(nullptr);
        sci.hwnd      = hwnd;
        if (vkCreateWin32SurfaceKHR(vk_instance, &sci, nullptr, &vk_surface) != VK_SUCCESS) { return std::unexpected{app_error::swap_chain}; }

        u32 gpu_count = 0;
        vkEnumeratePhysicalDevices(vk_instance, &gpu_count, nullptr);
        if (gpu_count == 0) { return std::unexpected{app_error::device}; }
        std::vector<VkPhysicalDevice> gpus(gpu_count);
        vkEnumeratePhysicalDevices(vk_instance, &gpu_count, gpus.data());

        for (VkPhysicalDevice pd : gpus) {
            u32 qfc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qfp(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfp.data());
            for (u32 q = 0; q < qfc; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, vk_surface, &present);
                if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE) {
                    vk_phys    = pd;
                    vk_qfamily = q;
                    break;
                }
            }
            if (vk_phys != VK_NULL_HANDLE) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(pd, &props);
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { break; }
            }
        }
        if (vk_phys == VK_NULL_HANDLE) { return std::unexpected{app_error::device}; }

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dqci.queueFamilyIndex = vk_qfamily;
        dqci.queueCount       = 1;
        dqci.pQueuePriorities = &prio;
        const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount    = 1;
        dci.pQueueCreateInfos       = &dqci;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        if (vkCreateDevice(vk_phys, &dci, nullptr, &vk_device) != VK_SUCCESS) { return std::unexpected{app_error::device}; }
        vkGetDeviceQueue(vk_device, vk_qfamily, 0, &vk_queue);

        for (u32 i = 0; i < kVkFrames; ++i) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = vk_qfamily;
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            VkSemaphoreCreateInfo smci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateCommandPool(vk_device, &pci, nullptr, &vk_pools[i]) != VK_SUCCESS) { return std::unexpected{app_error::device}; }
            cbai.commandPool = vk_pools[i];
            if (vkAllocateCommandBuffers(vk_device, &cbai, &vk_cmds[i]) != VK_SUCCESS ||
                vkCreateSemaphore(vk_device, &smci, nullptr, &vk_img_avail[i]) != VK_SUCCESS ||
                vkCreateFence(vk_device, &fci, nullptr, &vk_fences[i]) != VK_SUCCESS) {
                return std::unexpected{app_error::device};
            }
        }
        if (!vk_create_swapchain()) { return std::unexpected{app_error::swap_chain}; }
        if (ui->font().pixels().empty() && !ui->rebuild_font_atlas()) { return std::unexpected{app_error::fonts}; }
        vulkan_init_info info;
        info.physical_device  = vk_phys;
        info.device           = vk_device;
        info.queue            = vk_queue;
        info.queue_family     = vk_qfamily;
        info.frames_in_flight = kVkFrames;
        info.color_format     = vk_format;
        if (!vk_ren.create(info, ui->font())) { return std::unexpected{app_error::renderer}; }
        ui->release_font_pixels();
        ui->invalidate();
        return {};
    }
#endif

    [[nodiscard]] std::expected<void, app_error> create_device()
    {
        switch (backend) {
        case app_backend::d3d11:
#if STRATA_HAS_DX11
            return create_d3d11();
#else
            return std::unexpected{app_error::device};
#endif
        case app_backend::opengl:
#if STRATA_HAS_OPENGL
            return create_opengl();
#else
            return std::unexpected{app_error::device};
#endif
        case app_backend::vulkan:
#if STRATA_HAS_VULKAN
            return create_vulkan();
#else
            return std::unexpected{app_error::device};
#endif
        default:
            return std::unexpected{app_error::device};
        }
    }

    void resize() noexcept
    {
#if STRATA_HAS_DX11
        if (backend == app_backend::d3d11 && chain != nullptr) {
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            rtv.Reset();
            if (SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, chain_flags))) { (void)d3d11_create_target(); }
            ui->invalidate();
            return;
        }
#endif
#if STRATA_HAS_OPENGL
        if (backend == app_backend::opengl) {
            ui->invalidate();
            return;
        }
#endif
#if STRATA_HAS_VULKAN
        if (backend == app_backend::vulkan && vk_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk_device);
            vk_ren.clear_target_cache();
            (void)vk_create_swapchain();
            ui->invalidate();
            return;
        }
#endif
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
#if STRATA_HAS_DX11
        if (backend == app_backend::d3d11) { (void)renderer.update_atlas(ui->font()); }
#endif
#if STRATA_HAS_OPENGL
        if (backend == app_backend::opengl && gl_dc != nullptr && gl_rc != nullptr) {
            ::wglMakeCurrent(gl_dc, gl_rc);
            (void)gl_ren.update_atlas(ui->font());
        }
#endif
#if STRATA_HAS_VULKAN
        if (backend == app_backend::vulkan && vk_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vk_device);
            (void)vk_ren.update_atlas(ui->font());
        }
#endif
        ui->release_font_pixels();
    }

    [[nodiscard]] bool present() noexcept
    {
#if STRATA_HAS_DX11
        if (backend == app_backend::d3d11) {
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
#endif
#if STRATA_HAS_OPENGL
        if (backend == app_backend::opengl) {
            if (gl_dc == nullptr || gl_rc == nullptr) { return false; }
            ::wglMakeCurrent(gl_dc, gl_rc);
            const vec2 cs = client_size();
            ::glViewport(0, 0, static_cast<GLsizei>(cs.x), static_cast<GLsizei>(cs.y));
            ::glDisable(GL_SCISSOR_TEST);
            ::glClearColor(cfg.clear.r / 255.0f, cfg.clear.g / 255.0f, cfg.clear.b / 255.0f, cfg.clear.a / 255.0f);
            ::glClear(GL_COLOR_BUFFER_BIT);
            gl_ren.render(ui->render_data());
            ::SwapBuffers(gl_dc);
            ++frames_drawn;
            return !gl_ren.device_lost();
        }
#endif
#if STRATA_HAS_VULKAN
        if (backend == app_backend::vulkan) {
            if (vk_swapchain == VK_NULL_HANDLE || vk_extent.width == 0 || vk_extent.height == 0) { return true; }
            vkWaitForFences(vk_device, 1, &vk_fences[vk_slot], VK_TRUE, UINT64_MAX);
            u32 img_index = 0;
            const VkResult acq = vkAcquireNextImageKHR(vk_device, vk_swapchain, UINT64_MAX, vk_img_avail[vk_slot], VK_NULL_HANDLE, &img_index);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
                resize();
                return true;
            }
            if (acq == VK_ERROR_DEVICE_LOST) { return false; }
            if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { return true; }

            vkResetFences(vk_device, 1, &vk_fences[vk_slot]);
            VkCommandBuffer cmd = vk_cmds[vk_slot];
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { return false; }

            VkImage img = vk_images[img_index];
            vk_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue cc{{cfg.clear.r / 255.0f, cfg.clear.g / 255.0f, cfg.clear.b / 255.0f, cfg.clear.a / 255.0f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
            vk_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            vulkan_target target;
            target.image  = img;
            target.view   = vk_views[img_index];
            target.extent = vk_extent;
            target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vk_ren.render(ui->render_data(), cmd, vk_slot, target);

            vk_barrier(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { return false; }

            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount   = 1;
            si.pWaitSemaphores      = &vk_img_avail[vk_slot];
            si.pWaitDstStageMask    = &wait_stage;
            si.commandBufferCount   = 1;
            si.pCommandBuffers      = &cmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores    = &vk_render_done[img_index];
            if (vkQueueSubmit(vk_queue, 1, &si, vk_fences[vk_slot]) == VK_ERROR_DEVICE_LOST) { return false; }

            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores    = &vk_render_done[img_index];
            pi.swapchainCount     = 1;
            pi.pSwapchains        = &vk_swapchain;
            pi.pImageIndices      = &img_index;
            const VkResult pres = vkQueuePresentKHR(vk_queue, &pi);
            vk_slot = (vk_slot + 1) % kVkFrames;
            ++frames_drawn;
            if (pres == VK_ERROR_DEVICE_LOST) { return false; }
            if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) { resize(); }
            return !vk_ren.device_lost();
        }
#endif
        return false;
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
                return 0;
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
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    auto p          = std::make_unique<impl>();
    p->cfg          = cfg;
    p->backend      = resolve_backend(cfg.backend);
    p->title.assign(cfg.title);
    p->state_path.assign(cfg.state_file);
    p->cfg.title      = p->title;
    p->cfg.state_file = p->state_path;

    auto created = context::create(cfg.ui);
    if (!created) { return std::unexpected{app_error::fonts}; }
    p->ui = std::make_unique<context>(std::move(*created));

    const HINSTANCE inst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof wc;
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = &impl::wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = window_class;
    ::RegisterClassExW(&wc);

    const POINT origin{CW_USEDEFAULT, CW_USEDEFAULT};
    const UINT  dpi   = ::GetDpiForSystem();
    const f32   scale = static_cast<f32>(dpi) / 96.0f;
    RECT r{0, 0, static_cast<LONG>(std::lround(cfg.width * scale)), static_cast<LONG>(std::lround(cfg.height * scale))};
    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const std::wstring title = to_wide(cfg.title);
    p->hwnd = ::CreateWindowExW(0, window_class, title.c_str(), WS_OVERLAPPEDWINDOW, origin.x, origin.y, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, inst, p.get());
    if (p->hwnd == nullptr) { return std::unexpected{app_error::window}; }
    p->platform.attach(p->hwnd);
    p->ui->set_clipboard(p->platform.clipboard());
    (void)p->ui->set_scale(p->platform.dpi_scale());

    if (const auto d = p->create_device(); !d) { return std::unexpected{d.error()}; }
    if (cfg.follow_system_theme) { p->apply_theme(); }
    if (!p->state_path.empty()) {
        config state;
        if (state.load_file(p->state_path)) { (void)p->ui->load_state(state); }
    }
    return app{std::move(p)};
}

int app::run(const std::function<void(app&, context&)>& frame)
{
    impl& s = *impl_;
    s.owner = this;
    ::ShowWindow(s.hwnd, SW_SHOW);
    ::UpdateWindow(s.hwnd);

    bool presented = true;
    while (!s.quitting) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { s.quitting = true; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (s.quitting) { break; }
        if (s.minimized) {
            ::WaitMessage();
            continue;
        }
        if (s.resized) {
            s.resized = false;
            s.resize();
        }
        s.apply_dpi();
        if (s.cfg.follow_system_theme && s.platform.appearance_changed()) { s.apply_theme(); }

#if STRATA_HAS_DX11
        if (s.backend == app_backend::d3d11 && s.latency_wait != nullptr && presented) {
            ::WaitForSingleObjectEx(s.latency_wait, 100, TRUE);
        }
#endif
        (void)presented;

        const bool changed = run_frame(*s.ui, s.platform, [&](context& ui) { frame(*this, ui); }, s.cfg.idle);
        ++s.frames;
        presented = changed;
        if (changed) {
            if (!s.present()) {
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
        (void)state.load_file(s.state_path);
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

context&    app::ui() noexcept { return *impl_->ui; }
app_backend app::active_backend() const noexcept { return impl_->backend; }
#if STRATA_HAS_DX11
d3d11_renderer&      app::renderer() noexcept { return impl_->renderer; }
ID3D11Device*        app::device() const noexcept { return impl_->backend == app_backend::d3d11 ? impl_->device.Get() : nullptr; }
ID3D11DeviceContext* app::device_context() const noexcept { return impl_->backend == app_backend::d3d11 ? impl_->ctx.Get() : nullptr; }
#else
ID3D11Device*        app::device() const noexcept { return nullptr; }
ID3D11DeviceContext* app::device_context() const noexcept { return nullptr; }
#endif
#if STRATA_HAS_OPENGL
gl_renderer* app::gl_renderer_ptr() noexcept { return impl_->backend == app_backend::opengl ? &impl_->gl_ren : nullptr; }
#endif
#if STRATA_HAS_VULKAN
vulkan_renderer* app::vulkan_renderer_ptr() noexcept { return impl_->backend == app_backend::vulkan ? &impl_->vk_ren : nullptr; }
#endif
void* app::window() const noexcept { return impl_->hwnd; }
u64   app::frames() const noexcept { return impl_->frames; }
u64   app::frames_drawn() const noexcept { return impl_->frames_drawn; }

} // namespace strata

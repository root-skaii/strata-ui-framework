// Official Vulkan explicit overlay layer (VK_LAYER_strata_overlay).
// Intercepts vkCreateWin32SurfaceKHR, vkCreateSwapchainKHR, and vkQueuePresentKHR through the Vulkan loader
// dispatch chain with no binary patching. Press F1 to toggle the overlay window.

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <strata/backend/vulkan.hpp>
#include <strata/context.hpp>
#include <strata/platform/win32.hpp>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

[[nodiscard]] void* dispatch_key(void* obj) noexcept
{
    return *reinterpret_cast<void**>(obj);
}

struct inst_data {
    VkInstance                  instance{};
    PFN_vkGetInstanceProcAddr   next_gipa{};
    PFN_vkDestroyInstance       destroy_instance{};
    PFN_vkCreateWin32SurfaceKHR create_win32_surface{};
    PFN_vkDestroySurfaceKHR     destroy_surface{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qfp{};
};

struct dev_data {
    VkDevice                       device{};
    VkPhysicalDevice               phys{};
    PFN_vkGetDeviceProcAddr        next_gdpa{};
    PFN_vkDestroyDevice            destroy_device{};
    PFN_vkGetDeviceQueue           get_device_queue{};
    PFN_vkCreateSwapchainKHR       create_swapchain{};
    PFN_vkDestroySwapchainKHR      destroy_swapchain{};
    PFN_vkGetSwapchainImagesKHR    get_swapchain_images{};
    PFN_vkQueuePresentKHR          queue_present{};
    PFN_vkQueueSubmit              queue_submit{};
    PFN_vkDeviceWaitIdle           device_wait_idle{};
    PFN_vkCreateImageView          create_image_view{};
    PFN_vkDestroyImageView         destroy_image_view{};
    PFN_vkCreateCommandPool        create_cmd_pool{};
    PFN_vkDestroyCommandPool       destroy_cmd_pool{};
    PFN_vkAllocateCommandBuffers   alloc_cmd_bufs{};
    PFN_vkResetCommandBuffer       reset_cmd_buf{};
    PFN_vkBeginCommandBuffer       begin_cmd_buf{};
    PFN_vkEndCommandBuffer         end_cmd_buf{};
    PFN_vkCmdPipelineBarrier       cmd_barrier{};
    PFN_vkCreateSemaphore          create_semaphore{};
    PFN_vkDestroySemaphore         destroy_semaphore{};
    PFN_vkCreateFence              create_fence{};
    PFN_vkDestroyFence             destroy_fence{};
    PFN_vkWaitForFences            wait_fences{};
    PFN_vkResetFences              reset_fences{};
    VkQueue                        graphics_queue{VK_NULL_HANDLE};
    strata::u32                    graphics_family{0};
};

struct swap_data {
    dev_data*                 dev{};
    VkSwapchainKHR            swapchain{};
    HWND                      hwnd{};
    WNDPROC                   orig_proc{};
    VkFormat                  format{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D                extent{};
    std::vector<VkImage>      images;
    std::vector<VkImageView>  views;
    VkCommandPool             cmd_pool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> cmds;
    std::vector<VkSemaphore>  done_sems;
    std::vector<VkFence>      fences;
    strata::u32               slot{};
    strata::vulkan_renderer   renderer;
    std::unique_ptr<strata::context> ui;
    strata::win32_platform    platform;
    std::recursive_mutex      platform_mtx;
    std::atomic<bool>         visible{false};
    strata::u64               frames{};
};

std::mutex g_mtx;
std::unordered_map<void*, std::unique_ptr<inst_data>> g_insts;
std::unordered_map<void*, std::unique_ptr<dev_data>>  g_devs;
std::unordered_map<VkSurfaceKHR, HWND>                g_surfaces;
std::unordered_map<VkSwapchainKHR, std::shared_ptr<swap_data>> g_swaps;
std::unordered_map<HWND, std::shared_ptr<swap_data>>  g_hwnd_map;

LRESULT CALLBACK layer_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    std::shared_ptr<swap_data> sw;
    WNDPROC orig = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto it = g_hwnd_map.find(hwnd);
        if (it != g_hwnd_map.end()) {
            sw   = it->second;
            orig = sw->orig_proc;
        }
    }
    if (sw == nullptr) { return ::DefWindowProcW(hwnd, msg, wp, lp); }

    if (msg == WM_KEYDOWN && wp == VK_F1 && (lp & (1LL << 30)) == 0) {
        sw->visible.store(!sw->visible.load());
        return 0;
    }
    if (sw->visible.load()) {
        std::lock_guard plk{sw->platform_mtx};
        (void)sw->platform.handle_message(hwnd, msg, wp, static_cast<std::intptr_t>(lp));
        if (strata::win32_platform::swallows(msg)) { return 0; }
    }
    return orig != nullptr ? ::CallWindowProcW(orig, hwnd, msg, wp, lp) : ::DefWindowProcW(hwnd, msg, wp, lp);
}

void destroy_swap_resources(swap_data& sw)
{
    if (sw.hwnd != nullptr && sw.orig_proc != nullptr && ::IsWindow(sw.hwnd)) {
        auto it = g_hwnd_map.find(sw.hwnd);
        if (it != g_hwnd_map.end() && it->second.get() == &sw) {
            if (reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(sw.hwnd, GWLP_WNDPROC)) == &layer_wnd_proc) {
                ::SetWindowLongPtrW(sw.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sw.orig_proc));
            }
            g_hwnd_map.erase(it);
        }
        sw.orig_proc = nullptr;
    }
    if (sw.dev == nullptr) { return; }
    dev_data& d = *sw.dev;
    if (d.device_wait_idle != nullptr) { d.device_wait_idle(d.device); }
    sw.renderer.destroy();
    for (VkSemaphore s : sw.done_sems) { if (s != VK_NULL_HANDLE) { d.destroy_semaphore(d.device, s, nullptr); } }
    for (VkFence f : sw.fences)        { if (f != VK_NULL_HANDLE) { d.destroy_fence(d.device, f, nullptr); } }
    for (VkImageView v : sw.views)     { if (v != VK_NULL_HANDLE) { d.destroy_image_view(d.device, v, nullptr); } }
    if (sw.cmd_pool != VK_NULL_HANDLE) { d.destroy_cmd_pool(d.device, sw.cmd_pool, nullptr); }
    sw.done_sems.clear();
    sw.fences.clear();
    sw.views.clear();
    sw.cmds.clear();
    sw.cmd_pool = VK_NULL_HANDLE;
}

void barrier_img(const dev_data& d, VkCommandBuffer cmd, VkImage img, VkImageLayout old_l, VkImageLayout new_l,
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
    d.cmd_barrier(cmd, src_s, dst_s, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VKAPI_ATTR VkResult VKAPI_CALL hook_create_instance(const VkInstanceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkInstance* pInstance)
{
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (link != nullptr && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
    }
    if (link == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto next_create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (next_create == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    const VkResult res = next_create(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) { return res; }

    auto id = std::make_unique<inst_data>();
    id->instance             = *pInstance;
    id->next_gipa            = gipa;
    id->destroy_instance     = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*pInstance, "vkDestroyInstance"));
    id->create_win32_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gipa(*pInstance, "vkCreateWin32SurfaceKHR"));
    id->destroy_surface      = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gipa(*pInstance, "vkDestroySurfaceKHR"));
    id->get_qfp              = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*pInstance, "vkGetPhysicalDeviceQueueFamilyProperties"));

    std::lock_guard lk{g_mtx};
    g_insts[dispatch_key(*pInstance)] = std::move(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_destroy_instance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroyInstance next_fn = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto it = g_insts.find(dispatch_key(instance));
        if (it != g_insts.end()) {
            next_fn = it->second->destroy_instance;
            g_insts.erase(it);
        }
    }
    if (next_fn != nullptr) { next_fn(instance, pAllocator); }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_create_win32_surface(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                         const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    PFN_vkCreateWin32SurfaceKHR next_fn = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto it = g_insts.find(dispatch_key(instance));
        if (it != g_insts.end()) { next_fn = it->second->create_win32_surface; }
    }
    if (next_fn == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    const VkResult res = next_fn(instance, pCreateInfo, pAllocator, pSurface);
    if (res == VK_SUCCESS && pCreateInfo != nullptr) {
        std::lock_guard lk{g_mtx};
        g_surfaces[*pSurface] = pCreateInfo->hwnd;
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL hook_destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroySurfaceKHR next_fn = nullptr;
    {
        std::lock_guard lk{g_mtx};
        g_surfaces.erase(surface);
        auto it = g_insts.find(dispatch_key(instance));
        if (it != g_insts.end()) { next_fn = it->second->destroy_surface; }
    }
    if (next_fn != nullptr) { next_fn(instance, surface, pAllocator); }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    auto* link = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (link != nullptr && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || link->function != VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(link->pNext));
    }
    if (link == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto next_create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (next_create == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    const VkResult res = next_create(phys, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) { return res; }

    auto dd = std::make_unique<dev_data>();
    dd->device               = *pDevice;
    dd->phys                 = phys;
    dd->next_gdpa            = gdpa;
    dd->destroy_device       = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*pDevice, "vkDestroyDevice"));
    dd->get_device_queue     = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*pDevice, "vkGetDeviceQueue"));
    dd->create_swapchain     = reinterpret_cast<PFN_vkCreateSwapchainKHR>(gdpa(*pDevice, "vkCreateSwapchainKHR"));
    dd->destroy_swapchain    = reinterpret_cast<PFN_vkDestroySwapchainKHR>(gdpa(*pDevice, "vkDestroySwapchainKHR"));
    dd->get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(gdpa(*pDevice, "vkGetSwapchainImagesKHR"));
    dd->queue_present        = reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(*pDevice, "vkQueuePresentKHR"));
    dd->queue_submit         = reinterpret_cast<PFN_vkQueueSubmit>(gdpa(*pDevice, "vkQueueSubmit"));
    dd->device_wait_idle     = reinterpret_cast<PFN_vkDeviceWaitIdle>(gdpa(*pDevice, "vkDeviceWaitIdle"));
    dd->create_image_view    = reinterpret_cast<PFN_vkCreateImageView>(gdpa(*pDevice, "vkCreateImageView"));
    dd->destroy_image_view   = reinterpret_cast<PFN_vkDestroyImageView>(gdpa(*pDevice, "vkDestroyImageView"));
    dd->create_cmd_pool      = reinterpret_cast<PFN_vkCreateCommandPool>(gdpa(*pDevice, "vkCreateCommandPool"));
    dd->destroy_cmd_pool     = reinterpret_cast<PFN_vkDestroyCommandPool>(gdpa(*pDevice, "vkDestroyCommandPool"));
    dd->alloc_cmd_bufs       = reinterpret_cast<PFN_vkAllocateCommandBuffers>(gdpa(*pDevice, "vkAllocateCommandBuffers"));
    dd->reset_cmd_buf        = reinterpret_cast<PFN_vkResetCommandBuffer>(gdpa(*pDevice, "vkResetCommandBuffer"));
    dd->begin_cmd_buf        = reinterpret_cast<PFN_vkBeginCommandBuffer>(gdpa(*pDevice, "vkBeginCommandBuffer"));
    dd->end_cmd_buf          = reinterpret_cast<PFN_vkEndCommandBuffer>(gdpa(*pDevice, "vkEndCommandBuffer"));
    dd->cmd_barrier          = reinterpret_cast<PFN_vkCmdPipelineBarrier>(gdpa(*pDevice, "vkCmdPipelineBarrier"));
    dd->create_semaphore     = reinterpret_cast<PFN_vkCreateSemaphore>(gdpa(*pDevice, "vkCreateSemaphore"));
    dd->destroy_semaphore    = reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(*pDevice, "vkDestroySemaphore"));
    dd->create_fence         = reinterpret_cast<PFN_vkCreateFence>(gdpa(*pDevice, "vkCreateFence"));
    dd->destroy_fence        = reinterpret_cast<PFN_vkDestroyFence>(gdpa(*pDevice, "vkDestroyFence"));
    dd->wait_fences          = reinterpret_cast<PFN_vkWaitForFences>(gdpa(*pDevice, "vkWaitForFences"));
    dd->reset_fences         = reinterpret_cast<PFN_vkResetFences>(gdpa(*pDevice, "vkResetFences"));
    if (pCreateInfo->queueCreateInfoCount > 0 && pCreateInfo->pQueueCreateInfos != nullptr) {
        PFN_vkGetPhysicalDeviceQueueFamilyProperties get_qfp = nullptr;
        {
            std::lock_guard lk{g_mtx};
            auto iit = g_insts.find(dispatch_key(phys));
            if (iit != g_insts.end()) { get_qfp = iit->second->get_qfp; }
        }
        std::vector<VkQueueFamilyProperties> qfps;
        if (get_qfp != nullptr) {
            strata::u32 qcount = 0;
            get_qfp(phys, &qcount, nullptr);
            qfps.resize(qcount);
            if (qcount > 0) { get_qfp(phys, &qcount, qfps.data()); }
        }
        strata::u32 picked = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
        for (strata::u32 i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const strata::u32 fam = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
            if (fam < qfps.size() && (qfps[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                picked = fam;
                break;
            }
        }
        dd->graphics_family = picked;
        if (dd->get_device_queue != nullptr) {
            dd->get_device_queue(*pDevice, dd->graphics_family, 0, &dd->graphics_queue);
        }
    }

    std::lock_guard lk{g_mtx};
    g_devs[dispatch_key(*pDevice)] = std::move(dd);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_destroy_device(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroyDevice next_fn = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto it = g_devs.find(dispatch_key(device));
        if (it != g_devs.end()) {
            for (auto sit = g_swaps.begin(); sit != g_swaps.end();) {
                if (sit->second->dev == it->second.get()) {
                    destroy_swap_resources(*sit->second);
                    sit = g_swaps.erase(sit);
                } else {
                    ++sit;
                }
            }
            next_fn = it->second->destroy_device;
            g_devs.erase(it);
        }
    }
    if (next_fn != nullptr) { next_fn(device, pAllocator); }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                     const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    dev_data* dd = nullptr;
    HWND hwnd    = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto it = g_devs.find(dispatch_key(device));
        if (it != g_devs.end()) { dd = it->second.get(); }
        if (pCreateInfo != nullptr) {
            auto sit = g_surfaces.find(pCreateInfo->surface);
            if (sit != g_surfaces.end()) { hwnd = sit->second; }
        }
    }
    if (dd == nullptr || dd->create_swapchain == nullptr) { return VK_ERROR_INITIALIZATION_FAILED; }
    VkSwapchainCreateInfoKHR ci = *pCreateInfo;
    ci.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const VkResult res = dd->create_swapchain(device, &ci, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) { return res; }

    auto sw = std::make_shared<swap_data>();
    sw->dev       = dd;
    sw->swapchain = *pSwapchain;
    sw->hwnd      = hwnd;
    sw->format    = ci.imageFormat;
    sw->extent    = ci.imageExtent;

    strata::u32 img_count = 0;
    dd->get_swapchain_images(device, *pSwapchain, &img_count, nullptr);
    sw->images.resize(img_count);
    dd->get_swapchain_images(device, *pSwapchain, &img_count, sw->images.data());
    sw->views.resize(img_count, VK_NULL_HANDLE);
    sw->cmds.resize(img_count, VK_NULL_HANDLE);
    sw->done_sems.resize(img_count, VK_NULL_HANDLE);
    sw->fences.resize(img_count, VK_NULL_HANDLE);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dd->graphics_family;
    if (dd->create_cmd_pool(device, &pci, nullptr, &sw->cmd_pool) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool        = sw->cmd_pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = img_count;
        dd->alloc_cmd_bufs(device, &cbai, sw->cmds.data());
    }
    for (strata::u32 i = 0; i < img_count; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image                       = sw->images[i];
        ivci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format                      = sw->format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        dd->create_image_view(device, &ivci, nullptr, &sw->views[i]);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        dd->create_semaphore(device, &sci, nullptr, &sw->done_sems[i]);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        dd->create_fence(device, &fci, nullptr, &sw->fences[i]);
    }

    if (auto created = strata::context::create(); created && dd->graphics_queue != VK_NULL_HANDLE) {
        sw->ui = std::make_unique<strata::context>(std::move(*created));
        strata::vulkan_init_info info;
        info.physical_device  = dd->phys;
        info.device           = device;
        info.queue            = dd->graphics_queue;
        info.queue_family     = dd->graphics_family;
        info.frames_in_flight = std::max(img_count, 2u);
        info.color_format     = sw->format;
        if (sw->renderer.create(info, sw->ui->font())) {
            sw->ui->release_font_pixels();
            strata::output_desc od;
            if (sw->format == VK_FORMAT_B8G8R8A8_SRGB || sw->format == VK_FORMAT_R8G8B8A8_SRGB ||
                sw->format == VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
                od.space = strata::output_space::srgb_view;
            } else if (ci.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
                od.space = strata::output_space::hdr10;
            } else if (sw->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                       ci.imageColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                od.space = strata::output_space::scrgb;
            }
            sw->renderer.set_output(od);
        }
    }

    std::lock_guard lk{g_mtx};
    if (hwnd != nullptr && ::IsWindow(hwnd)) {
        sw->platform.attach(hwnd);
        auto it = g_hwnd_map.find(hwnd);
        if (it == g_hwnd_map.end()) {
            sw->orig_proc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&layer_wnd_proc)));
        } else {
            // swapchain resize creates the new swapchain before destroying oldSwapchain: transfer subclass ownership
            sw->orig_proc = it->second->orig_proc;
            sw->visible.store(it->second->visible.load());
            it->second->orig_proc = nullptr;
        }
        g_hwnd_map[hwnd] = sw;
    }
    g_swaps[*pSwapchain] = std::move(sw);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL hook_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    PFN_vkDestroySwapchainKHR next_fn = nullptr;
    {
        std::lock_guard lk{g_mtx};
        auto dit = g_devs.find(dispatch_key(device));
        if (dit != g_devs.end()) { next_fn = dit->second->destroy_swapchain; }
        auto sit = g_swaps.find(swapchain);
        if (sit != g_swaps.end()) {
            destroy_swap_resources(*sit->second);
            g_swaps.erase(sit);
        }
    }
    if (next_fn != nullptr) { next_fn(device, swapchain, pAllocator); }
}

VKAPI_ATTR VkResult VKAPI_CALL hook_queue_present(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    dev_data*  dd = nullptr;
    std::shared_ptr<swap_data> sw;
    strata::u32 img_idx = 0;
    {
        std::lock_guard lk{g_mtx};
        auto dit = g_devs.find(dispatch_key(queue));
        if (dit != g_devs.end()) { dd = dit->second.get(); }
        if (pPresentInfo != nullptr && pPresentInfo->swapchainCount > 0) {
            auto sit = g_swaps.find(pPresentInfo->pSwapchains[0]);
            if (sit != g_swaps.end()) {
                sw      = sit->second;
                img_idx = pPresentInfo->pImageIndices[0];
            }
        }
    }
    if (dd == nullptr || dd->queue_present == nullptr) { return VK_ERROR_DEVICE_LOST; }

    if (sw == nullptr || !sw->visible.load() || !sw->renderer.valid() || sw->ui == nullptr ||
        dd->graphics_queue == VK_NULL_HANDLE || img_idx >= sw->images.size()) {
        return dd->queue_present(queue, pPresentInfo);
    }

    const strata::u32 s = sw->slot % static_cast<strata::u32>(sw->cmds.size());
    sw->slot = (s + 1) % static_cast<strata::u32>(sw->cmds.size());

    dd->wait_fences(dd->device, 1, &sw->fences[s], VK_TRUE, UINT64_MAX);
    dd->reset_fences(dd->device, 1, &sw->fences[s]);

    strata::input_state input;
    {
        std::lock_guard plk{sw->platform_mtx};
        input = sw->platform.new_frame();
    }
    input.display_size = {static_cast<strata::f32>(sw->extent.width), static_cast<strata::f32>(sw->extent.height)};
    sw->ui->begin_frame(std::move(input));
    if (auto w = sw->ui->window("strata vulkan layer (F1)", {32.0f, 32.0f}, {320.0f, 0.0f})) {
        sw->ui->textf("Frame: {}", ++sw->frames);
        sw->ui->textf("Swapchain: {}x{}", sw->extent.width, sw->extent.height);
        sw->ui->text_dim("Press F1 to hide overlay");
    }
    sw->ui->end_frame();

    VkCommandBuffer cmd = sw->cmds[s];
    dd->reset_cmd_buf(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->begin_cmd_buf(cmd, &bi);

    barrier_img(*dd, cmd, sw->images[img_idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    strata::vulkan_target target;
    target.image  = sw->images[img_idx];
    target.view   = sw->views[img_idx];
    target.extent = sw->extent;
    target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sw->renderer.render(sw->ui->render_data(), cmd, s, target);

    barrier_img(*dd, cmd, sw->images[img_idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    dd->end_cmd_buf(cmd);

    std::vector<VkPipelineStageFlags> wait_stages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = pPresentInfo->waitSemaphoreCount;
    si.pWaitSemaphores      = pPresentInfo->pWaitSemaphores;
    si.pWaitDstStageMask    = wait_stages.data();
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &sw->done_sems[img_idx];
    dd->queue_submit(dd->graphics_queue, 1, &si, sw->fences[s]);

    VkPresentInfoKHR pi = *pPresentInfo;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &sw->done_sems[img_idx];
    return dd->queue_present(queue, &pi);
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL strata_vk_get_device_proc_addr(VkDevice device, const char* pName)
{
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)   { return reinterpret_cast<PFN_vkVoidFunction>(&strata_vk_get_device_proc_addr); }
    if (std::strcmp(pName, "vkDestroyDevice") == 0)       { return reinterpret_cast<PFN_vkVoidFunction>(&hook_destroy_device); }
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)  { return reinterpret_cast<PFN_vkVoidFunction>(&hook_create_swapchain); }
    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(&hook_destroy_swapchain); }
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)     { return reinterpret_cast<PFN_vkVoidFunction>(&hook_queue_present); }

    std::lock_guard lk{g_mtx};
    auto it = g_devs.find(dispatch_key(device));
    return (it != g_devs.end() && it->second->next_gdpa != nullptr) ? it->second->next_gdpa(device, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL strata_vk_get_instance_proc_addr(VkInstance instance, const char* pName)
{
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0)   { return reinterpret_cast<PFN_vkVoidFunction>(&strata_vk_get_instance_proc_addr); }
    if (std::strcmp(pName, "vkCreateInstance") == 0)        { return reinterpret_cast<PFN_vkVoidFunction>(&hook_create_instance); }
    if (std::strcmp(pName, "vkDestroyInstance") == 0)       { return reinterpret_cast<PFN_vkVoidFunction>(&hook_destroy_instance); }
    if (std::strcmp(pName, "vkCreateWin32SurfaceKHR") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(&hook_create_win32_surface); }
    if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0)     { return reinterpret_cast<PFN_vkVoidFunction>(&hook_destroy_surface); }
    if (std::strcmp(pName, "vkCreateDevice") == 0)          { return reinterpret_cast<PFN_vkVoidFunction>(&hook_create_device); }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0)     { return reinterpret_cast<PFN_vkVoidFunction>(&strata_vk_get_device_proc_addr); }

    if (instance == VK_NULL_HANDLE) { return nullptr; }
    std::lock_guard lk{g_mtx};
    auto it = g_insts.find(dispatch_key(instance));
    return (it != g_insts.end() && it->second->next_gipa != nullptr) ? it->second->next_gipa(instance, pName) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct)
{
    if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }
    pVersionStruct->pfnGetInstanceProcAddr = &strata_vk_get_instance_proc_addr;
    pVersionStruct->pfnGetDeviceProcAddr   = &strata_vk_get_device_proc_addr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

} // extern "C"

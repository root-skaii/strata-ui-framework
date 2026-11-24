#include "gfx_host.hpp"

#include <strata/backend/vulkan.hpp>

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr strata::u32 frame_count = 2;

[[nodiscard]] strata::u32 find_mem_type(const VkPhysicalDeviceMemoryProperties& props, strata::u32 bits, VkMemoryPropertyFlags req) noexcept
{
    for (strata::u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout old_l, VkImageLayout new_l,
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

class vulkan_host final : public gfx_host {
public:
    ~vulkan_host() override
    {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            ui_.destroy();
            destroy_readback();
            destroy_swapchain();
            for (strata::u32 i = 0; i < frame_count; ++i) {
                if (img_avail_[i] != VK_NULL_HANDLE) { vkDestroySemaphore(device_, img_avail_[i], nullptr); }
                if (fences_[i] != VK_NULL_HANDLE)    { vkDestroyFence(device_, fences_[i], nullptr); }
                if (pools_[i] != VK_NULL_HANDLE)     { vkDestroyCommandPool(device_, pools_[i], nullptr); }
            }
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_ != VK_NULL_HANDLE)  { vkDestroySurfaceKHR(instance_, surface_, nullptr); }
        if (instance_ != VK_NULL_HANDLE) { vkDestroyInstance(instance_, nullptr); }
    }

    bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool vsync, const strata::font_atlas& atlas) override
    {
        hwnd_   = reinterpret_cast<HWND>(hwnd);
        width_  = width;
        height_ = height;
        vsync_  = vsync;

        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "strata_sandbox";
        ai.apiVersion       = VK_API_VERSION_1_0;

        const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        std::vector<const char*> layers;
#ifndef NDEBUG
        {
            strata::u32 lc = 0;
            vkEnumerateInstanceLayerProperties(&lc, nullptr);
            std::vector<VkLayerProperties> avail(lc);
            vkEnumerateInstanceLayerProperties(&lc, avail.data());
            for (const auto& lp : avail) {
                if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    break;
                }
            }
        }
#endif
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo        = &ai;
        ici.enabledExtensionCount   = 2;
        ici.ppEnabledExtensionNames = inst_exts;
        ici.enabledLayerCount       = static_cast<strata::u32>(layers.size());
        ici.ppEnabledLayerNames     = layers.data();
        if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) { return false; }

        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = ::GetModuleHandleW(nullptr);
        sci.hwnd      = hwnd_;
        if (vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_) != VK_SUCCESS) { return false; }

        // pick a physical device with graphics + present support
        strata::u32 gpu_count = 0;
        vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr);
        if (gpu_count == 0) { return false; }
        std::vector<VkPhysicalDevice> gpus(gpu_count);
        vkEnumeratePhysicalDevices(instance_, &gpu_count, gpus.data());

        for (VkPhysicalDevice pd : gpus) {
            strata::u32 qfc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qfp(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfp.data());
            for (strata::u32 q = 0; q < qfc; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
                if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE) {
                    phys_         = pd;
                    queue_family_ = q;
                    break;
                }
            }
            if (phys_ != VK_NULL_HANDLE) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(pd, &props);
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { break; }
            }
        }
        if (phys_ == VK_NULL_HANDLE) { return false; }
        vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props_);

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dqci.queueFamilyIndex = queue_family_;
        dqci.queueCount       = 1;
        dqci.pQueuePriorities = &prio;

        const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount    = 1;
        dci.pQueueCreateInfos       = &dqci;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        if (vkCreateDevice(phys_, &dci, nullptr, &device_) != VK_SUCCESS) { return false; }
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        for (strata::u32 i = 0; i < frame_count; ++i) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = queue_family_;
            VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            VkSemaphoreCreateInfo smci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateCommandPool(device_, &pci, nullptr, &pools_[i]) != VK_SUCCESS) { return false; }
            cbai.commandPool = pools_[i];
            if (vkAllocateCommandBuffers(device_, &cbai, &cmds_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &smci, nullptr, &img_avail_[i]) != VK_SUCCESS ||
                vkCreateFence(device_, &fci, nullptr, &fences_[i]) != VK_SUCCESS) {
                return false;
            }
        }

        if (!create_swapchain(width, height)) { return false; }

        strata::vulkan_init_info info;
        info.physical_device  = phys_;
        info.device           = device_;
        info.queue            = queue_;
        info.queue_family     = queue_family_;
        info.frames_in_flight = frame_count;
        info.color_format     = format_;
        return ui_.create(info, atlas);
    }

    void resize(strata::u32 width, strata::u32 height) override
    {
        if (device_ == VK_NULL_HANDLE || width == 0 || height == 0) { return; }
        width_  = width;
        height_ = height;
        vkDeviceWaitIdle(device_);
        ui_.clear_target_cache();
        (void)create_swapchain(width, height);
    }

    void render(const strata::draw_data& ui, strata::color clear) override
    {
        if (swapchain_ == VK_NULL_HANDLE || extent_.width == 0 || extent_.height == 0) { return; }

        vkWaitForFences(device_, 1, &fences_[slot_], VK_TRUE, UINT64_MAX);

        strata::u32 img_index = 0;
        VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, img_avail_[slot_], VK_NULL_HANDLE, &img_index);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(device_);
            ui_.clear_target_cache();
            (void)create_swapchain(width_, height_);
            return;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { return; }

        vkResetFences(device_, 1, &fences_[slot_]);
        VkCommandBuffer cmd = cmds_[slot_];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) { return; }

        VkImage img = images_[img_index];
        barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cc{{clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, 1.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
        barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        strata::vulkan_target target;
        target.image  = img;
        target.view   = views_[img_index];
        target.extent = extent_;
        target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ui_.render(ui, cmd, slot_, target);

        const bool capturing = capture_requested_ && record_capture(cmd, img);
        capture_requested_ = false;
        if (capturing) {
            barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else {
            barrier(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { return; }

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &img_avail_[slot_];
        si.pWaitDstStageMask    = &wait_stage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &render_done_[img_index];
        vkQueueSubmit(queue_, 1, &si, fences_[slot_]);

        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &render_done_[img_index];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swapchain_;
        pi.pImageIndices      = &img_index;
        const VkResult pres = vkQueuePresentKHR(queue_, &pi);
        slot_ = (slot_ + 1) % frame_count;

        if (capturing) {
            vkDeviceWaitIdle(device_);
            finish_capture();
        }
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(device_);
            ui_.clear_target_cache();
            (void)create_swapchain(width_, height_);
        }
    }

    strata::texture_id create_texture(strata::u32 width, strata::u32 height, std::span<const strata::u8> rgba) override
    {
        return ui_.create_texture(width, height, rgba);
    }
    strata::texture_id create_texture(const strata::texture_desc& desc, std::span<const strata::u8> pixels) override
    {
        return ui_.create_texture(desc, pixels);
    }
    bool update_texture(strata::texture_id id, strata::u32 x, strata::u32 y, strata::u32 width, strata::u32 height,
                        std::span<const strata::u8> pixels) override
    {
        return ui_.update_texture(id, x, y, width, height, pixels);
    }

    const char* name() const noexcept override { return "vulkan"; }

    bool update_atlas(const strata::font_atlas& atlas) override
    {
        vkDeviceWaitIdle(device_);
        return ui_.update_atlas(atlas);
    }

    void request_capture() override { capture_requested_ = true; }
    bool take_capture(std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height) override
    {
        if (captured_.empty()) { return false; }
        rgba   = std::move(captured_);
        width  = captured_w_;
        height = captured_h_;
        captured_.clear();
        return true;
    }

private:
    void destroy_swapchain() noexcept
    {
        for (VkSemaphore s : render_done_) { if (s != VK_NULL_HANDLE) { vkDestroySemaphore(device_, s, nullptr); } }
        render_done_.clear();
        for (VkImageView v : views_) { if (v != VK_NULL_HANDLE) { vkDestroyImageView(device_, v, nullptr); } }
        views_.clear();
        images_.clear();
        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    bool create_swapchain(strata::u32 width, strata::u32 height)
    {
        destroy_swapchain();

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) { return false; }

        strata::u32 fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, nullptr);
        if (fc == 0) { return false; }
        std::vector<VkSurfaceFormatKHR> fmts(fc);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fc, fmts.data());
        VkSurfaceFormatKHR chosen = fmts[0];
        // prefer non-sRGB RGBA8 or BGRA8 so output_space::srgb writes sRGB directly (matching d3d11/d3d12 hosts)
        for (const auto& f : fmts) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = f; }
        }
        format_ = chosen.format;

        VkPresentModeKHR pmode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_) {
            strata::u32 pmc = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmc, nullptr);
            std::vector<VkPresentModeKHR> modes(pmc);
            vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &pmc, modes.data());
            for (VkPresentModeKHR m : modes) {
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR || m == VK_PRESENT_MODE_MAILBOX_KHR) {
                    pmode = m;
                    if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { break; }
                }
            }
        }

        extent_ = caps.currentExtent;
        if (extent_.width == UINT32_MAX) {
            extent_.width  = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (extent_.width == 0 || extent_.height == 0) { return true; }

        strata::u32 img_count = std::max(caps.minImageCount, frame_count);
        if (caps.maxImageCount != 0 && img_count > caps.maxImageCount) { img_count = caps.maxImageCount; }

        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface          = surface_;
        sci.minImageCount    = img_count;
        sci.imageFormat      = chosen.format;
        sci.imageColorSpace  = chosen.colorSpace;
        sci.imageExtent      = extent_;
        sci.imageArrayLayers = 1;
        sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform     = caps.currentTransform;
        sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode      = pmode;
        sci.clipped          = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_) != VK_SUCCESS) { return false; }

        strata::u32 count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        images_.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
        views_.resize(count, VK_NULL_HANDLE);
        render_done_.resize(count, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo smci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (strata::u32 i = 0; i < count; ++i) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image                       = images_[i];
            vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
            vci.format                      = format_;
            vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vci.subresourceRange.levelCount = 1;
            vci.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device_, &vci, nullptr, &views_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &smci, nullptr, &render_done_[i]) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    void destroy_readback() noexcept
    {
        if (readback_buf_ != VK_NULL_HANDLE) { vkDestroyBuffer(device_, readback_buf_, nullptr); readback_buf_ = VK_NULL_HANDLE; }
        if (readback_mem_ != VK_NULL_HANDLE) { vkFreeMemory(device_, readback_mem_, nullptr); readback_mem_ = VK_NULL_HANDLE; }
        readback_bytes_ = 0;
    }

    bool record_capture(VkCommandBuffer cmd, VkImage img)
    {
        const VkDeviceSize needed = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
        if (readback_buf_ == VK_NULL_HANDLE || readback_bytes_ < needed) {
            destroy_readback();
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size        = needed;
            bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device_, &bci, nullptr, &readback_buf_) != VK_SUCCESS) { return false; }
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, readback_buf_, &req);
            const strata::u32 mt = find_mem_type(mem_props_, req.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mt == UINT32_MAX) { destroy_readback(); return false; }
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize  = req.size;
            mai.memoryTypeIndex = mt;
            if (vkAllocateMemory(device_, &mai, nullptr, &readback_mem_) != VK_SUCCESS ||
                vkBindBufferMemory(device_, readback_buf_, readback_mem_, 0) != VK_SUCCESS) {
                destroy_readback();
                return false;
            }
            readback_bytes_ = needed;
        }
        captured_w_ = extent_.width;
        captured_h_ = extent_.height;

        barrier(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bic{};
        bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bic.imageSubresource.layerCount = 1;
        bic.imageExtent                 = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf_, 1, &bic);
        return true;
    }

    void finish_capture()
    {
        void* mapped = nullptr;
        if (readback_mem_ == VK_NULL_HANDLE || vkMapMemory(device_, readback_mem_, 0, readback_bytes_, 0, &mapped) != VK_SUCCESS) {
            return;
        }
        const std::size_t nbytes = static_cast<std::size_t>(captured_w_) * captured_h_ * 4;
        captured_.resize(nbytes);
        std::memcpy(captured_.data(), mapped, nbytes);
        vkUnmapMemory(device_, readback_mem_);
        if (format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB) {
            for (std::size_t i = 0; i < nbytes; i += 4) { std::swap(captured_[i], captured_[i + 2]); }
        }
    }

    HWND                             hwnd_{};
    strata::u32                      width_{}, height_{};
    bool                             vsync_{true};
    VkInstance                       instance_{VK_NULL_HANDLE};
    VkSurfaceKHR                     surface_{VK_NULL_HANDLE};
    VkPhysicalDevice                 phys_{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties mem_props_{};
    strata::u32                      queue_family_{};
    VkDevice                         device_{VK_NULL_HANDLE};
    VkQueue                          queue_{VK_NULL_HANDLE};
    VkSwapchainKHR                   swapchain_{VK_NULL_HANDLE};
    VkFormat                         format_{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D                       extent_{};
    std::vector<VkImage>             images_;
    std::vector<VkImageView>         views_;
    std::vector<VkSemaphore>         render_done_;
    std::array<VkCommandPool, frame_count>   pools_{};
    std::array<VkCommandBuffer, frame_count> cmds_{};
    std::array<VkSemaphore, frame_count>     img_avail_{};
    std::array<VkFence, frame_count>         fences_{};
    strata::u32                      slot_{};
    strata::vulkan_renderer          ui_;
    bool                             capture_requested_{};
    VkBuffer                         readback_buf_{VK_NULL_HANDLE};
    VkDeviceMemory                   readback_mem_{VK_NULL_HANDLE};
    VkDeviceSize                     readback_bytes_{};
    std::vector<strata::u8>          captured_;
    strata::u32                      captured_w_{}, captured_h_{};
};

} // namespace

std::unique_ptr<gfx_host> make_vulkan_host()
{
    return std::make_unique<vulkan_host>();
}

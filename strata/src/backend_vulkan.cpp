#include "strata/backend/vulkan.hpp"
#include "strata/backend/backend.hpp"

#include "blur_plan.hpp"

#include "g_strata_vk_vs.h"
#include "g_strata_vk_ps.h"
#include "g_strata_vk_ps_image.h"
#include "g_strata_vk_ps_backdrop.h"
#include "g_strata_vk_vs_fullscreen.h"
#include "g_strata_vk_ps_blur_down.h"
#include "g_strata_vk_ps_blur_gauss.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace strata {

static_assert(renderer_backend<vulkan_renderer>);

namespace {

[[nodiscard]] constexpr VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) noexcept
{
    return (v + a - 1) & ~(a - 1);
}

[[nodiscard]] VkFormat vk_format_of(texture_layout l) noexcept
{
    switch (l) {
    case texture_layout::bgra8:   return VK_FORMAT_B8G8R8A8_UNORM;
    case texture_layout::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:                      return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

[[nodiscard]] bool wide_vk_format(VkFormat fmt) noexcept
{
    switch (fmt) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return false;
    default:
        return true;
    }
}

[[nodiscard]] u32 find_memory_type(const VkPhysicalDeviceMemoryProperties& mem, u32 type_bits, VkMemoryPropertyFlags props) noexcept
{
    for (u32 i = 0; i < mem.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

[[nodiscard]] VkShaderModule create_shader_module(VkDevice device, std::span<const std::uint32_t> code) noexcept
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size_bytes();
    ci.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

void cmd_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                       VkAccessFlags src_access, VkAccessFlags dst_access,
                       VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                       u32 mip_levels = 1) noexcept
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask               = src_access;
    b.dstAccessMask               = dst_access;
    b.oldLayout                   = old_layout;
    b.newLayout                   = new_layout;
    b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.image                       = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mip_levels;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

struct gpu_buffer {
    VkBuffer       buf{VK_NULL_HANDLE};
    VkDeviceMemory mem{VK_NULL_HANDLE};
    void*          mapped{};
    VkDeviceSize   capacity{};
};

struct gpu_image {
    VkImage        img{VK_NULL_HANDLE};
    VkDeviceMemory mem{VK_NULL_HANDLE};
    VkImageView    view{VK_NULL_HANDLE};
};

struct blur_constants {
    float bp0[4]{};
    float bp1[4]{};
    float bp2[4]{};
    float bp3[4]{};
    float bp4[4]{};
};
static_assert(sizeof(blur_constants) == 80);
static_assert(sizeof(internal::ui_constants) == 48);

} // namespace

struct vulkan_renderer::impl {
    VkPhysicalDevice                 phys{VK_NULL_HANDLE};
    VkDevice                         device{VK_NULL_HANDLE};
    VkQueue                          queue{VK_NULL_HANDLE};
    u32                              queue_family{};
    u32                              frames_in_flight{};
    VkFormat                         color_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkSampleCountFlagBits            samples{VK_SAMPLE_COUNT_1_BIT};
    VkPhysicalDeviceMemoryProperties mem_props{};

    VkDescriptorPool      desc_pool{VK_NULL_HANDLE};
    VkDescriptorSetLayout set0_layout{VK_NULL_HANDLE};
    VkDescriptorSetLayout set1_layout{VK_NULL_HANDLE};
    VkPipelineLayout      pipe_layout_ui{VK_NULL_HANDLE};
    VkPipelineLayout      pipe_layout_blur{VK_NULL_HANDLE};
    VkRenderPass          ui_pass{VK_NULL_HANDLE};
    VkRenderPass          blur_pass_rp{VK_NULL_HANDLE};
    VkPipeline            pipe_main{VK_NULL_HANDLE};
    VkPipeline            pipe_image{VK_NULL_HANDLE};
    VkPipeline            pipe_backdrop{VK_NULL_HANDLE};
    VkPipeline            pipe_blur_down{VK_NULL_HANDLE};
    VkPipeline            pipe_blur_gauss{VK_NULL_HANDLE};
    VkSampler             sampler{VK_NULL_HANDLE};

    gpu_image atlas;
    u32       atlas_w{}, atlas_h{};

    struct frame_slot {
        gpu_buffer      geo;    // vertices + indices
        gpu_buffer      shapes; // shape_record[]
        VkDescriptorSet set0{VK_NULL_HANDLE};
        VkDeviceSize    ib_offset{};
        u64             uploaded_hash{};
    };
    std::vector<frame_slot> frames;

    struct texture_slot {
        gpu_image       img;
        VkDescriptorSet set{VK_NULL_HANDLE};
        texture_image   cpu_image;
        u32             mip_levels{1};
        bool            occupied{};
        bool            owned{};
        bool            updatable{};
        u32             cooldown{};
    };
    std::array<texture_slot, vulkan_renderer::max_textures> textures{};

    struct pending_copy {
        VkImage                            image{VK_NULL_HANDLE};
        gpu_buffer                         staging;
        std::vector<texture_image::region> regions;
        std::vector<VkDeviceSize>          offsets;
        u32                                mip_levels{1};
        VkImageLayout                      before{VK_IMAGE_LAYOUT_UNDEFINED};
        VkImageLayout                      after{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    };
    std::vector<pending_copy> pending;

    // per-frame backdrop blur resources so target resize never updates in-flight descriptor sets
    struct blur_frame {
        gpu_image       snap;
        VkDescriptorSet snap_set{VK_NULL_HANDLE};
        u32             snap_w{}, snap_h{};
        gpu_image       blur_img[2];
        VkFramebuffer   blur_fb[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkDescriptorSet blur_set[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
        u32             blur_w{}, blur_h{};
    };
    VkFormat                blur_rt_format{VK_FORMAT_R8G8B8A8_UNORM};
    std::vector<blur_frame> blur_frames;
    bool                    blur_supported{};

    // cached VkFramebuffers for host swapchain views (retired when unused or on clear_target_cache)
    struct fb_entry {
        VkImageView   view{VK_NULL_HANDLE};
        VkExtent2D    extent{};
        VkFramebuffer fb{VK_NULL_HANDLE};
    };
    std::vector<fb_entry> fb_cache;

    // deferred destruction so in-flight command buffers never reference freed objects
    struct grave {
        VkBuffer       buf{VK_NULL_HANDLE};
        VkImage        img{VK_NULL_HANDLE};
        VkImageView    view{VK_NULL_HANDLE};
        VkFramebuffer  fb{VK_NULL_HANDLE};
        VkDeviceMemory mem{VK_NULL_HANDLE};
        void*          mapped{};
        VkDeviceSize   bytes{};
        u32            renders{};
    };
    std::vector<grave> graveyard;

    output_desc output{};
    bool        lost{};

    void check_lost(VkResult r) noexcept
    {
        if (r == VK_ERROR_DEVICE_LOST) { lost = true; }
    }

    void destroy_grave_now(grave& g) noexcept
    {
        if (g.mapped != nullptr && g.bytes != 0) {
            volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(g.mapped);
            for (VkDeviceSize i = 0; i < g.bytes; ++i) { p[i] = 0; }
            vkUnmapMemory(device, g.mem);
            g.mapped = nullptr;
        }
        if (g.fb != VK_NULL_HANDLE)   { vkDestroyFramebuffer(device, g.fb, nullptr); g.fb = VK_NULL_HANDLE; }
        if (g.view != VK_NULL_HANDLE) { vkDestroyImageView(device, g.view, nullptr); g.view = VK_NULL_HANDLE; }
        if (g.img != VK_NULL_HANDLE)  { vkDestroyImage(device, g.img, nullptr); g.img = VK_NULL_HANDLE; }
        if (g.buf != VK_NULL_HANDLE)  { vkDestroyBuffer(device, g.buf, nullptr); g.buf = VK_NULL_HANDLE; }
        if (g.mem != VK_NULL_HANDLE)  { vkFreeMemory(device, g.mem, nullptr); g.mem = VK_NULL_HANDLE; }
    }

    void retire_buffer(gpu_buffer& b)
    {
        if (b.buf == VK_NULL_HANDLE && b.mem == VK_NULL_HANDLE) { return; }
        graveyard.push_back({b.buf, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, b.mem, b.mapped, b.capacity, frames_in_flight + 1});
        b = {};
    }

    void retire_image(gpu_image& im)
    {
        if (im.img == VK_NULL_HANDLE && im.view == VK_NULL_HANDLE && im.mem == VK_NULL_HANDLE) { return; }
        graveyard.push_back({VK_NULL_HANDLE, im.img, im.view, VK_NULL_HANDLE, im.mem, nullptr, 0, frames_in_flight + 1});
        im = {};
    }

    void retire_framebuffer(VkFramebuffer& fb)
    {
        if (fb == VK_NULL_HANDLE) { return; }
        graveyard.push_back({VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, fb, VK_NULL_HANDLE, nullptr, 0, frames_in_flight + 1});
        fb = VK_NULL_HANDLE;
    }

    void tick_graveyard() noexcept
    {
        for (grave& g : graveyard) {
            if (g.renders > 0) { --g.renders; }
            if (g.renders == 0) { destroy_grave_now(g); }
        }
        std::erase_if(graveyard, [](const grave& g) { return g.renders == 0; });
        for (texture_slot& t : textures) {
            if (t.cooldown > 0) { --t.cooldown; }
        }
    }

    [[nodiscard]] bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                     gpu_buffer& out, bool map = false) noexcept
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = size;
        bci.usage       = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = vkCreateBuffer(device, &bci, nullptr, &out.buf);
        check_lost(r);
        if (r != VK_SUCCESS) { return false; }

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, out.buf, &req);
        const u32 type = find_memory_type(mem_props, req.memoryTypeBits, props);
        if (type == UINT32_MAX) {
            vkDestroyBuffer(device, out.buf, nullptr);
            out.buf = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type;
        r = vkAllocateMemory(device, &mai, nullptr, &out.mem);
        check_lost(r);
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(device, out.buf, nullptr);
            out.buf = VK_NULL_HANDLE;
            return false;
        }
        r = vkBindBufferMemory(device, out.buf, out.mem, 0);
        check_lost(r);
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(device, out.buf, nullptr);
            vkFreeMemory(device, out.mem, nullptr);
            out = {};
            return false;
        }
        if (map) {
            r = vkMapMemory(device, out.mem, 0, size, 0, &out.mapped);
            check_lost(r);
            if (r != VK_SUCCESS) {
                vkDestroyBuffer(device, out.buf, nullptr);
                vkFreeMemory(device, out.mem, nullptr);
                out = {};
                return false;
            }
        }
        out.capacity = size;
        return true;
    }

    [[nodiscard]] bool create_image(u32 w, u32 h, u32 levels, VkFormat fmt, VkImageUsageFlags usage, gpu_image& out) noexcept
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = fmt;
        ici.extent        = {w, h, 1};
        ici.mipLevels     = levels;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = usage;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(device, &ici, nullptr, &out.img);
        check_lost(r);
        if (r != VK_SUCCESS) { return false; }

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, out.img, &req);
        const u32 type = find_memory_type(mem_props, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) {
            vkDestroyImage(device, out.img, nullptr);
            out.img = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type;
        r = vkAllocateMemory(device, &mai, nullptr, &out.mem);
        check_lost(r);
        if (r != VK_SUCCESS) {
            vkDestroyImage(device, out.img, nullptr);
            out.img = VK_NULL_HANDLE;
            return false;
        }
        r = vkBindImageMemory(device, out.img, out.mem, 0);
        check_lost(r);
        if (r != VK_SUCCESS) {
            vkDestroyImage(device, out.img, nullptr);
            vkFreeMemory(device, out.mem, nullptr);
            out = {};
            return false;
        }

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image                       = out.img;
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = levels;
        vci.subresourceRange.layerCount = 1;
        r = vkCreateImageView(device, &vci, nullptr, &out.view);
        check_lost(r);
        if (r != VK_SUCCESS) {
            vkDestroyImage(device, out.img, nullptr);
            vkFreeMemory(device, out.mem, nullptr);
            out = {};
            return false;
        }
        return true;
    }

    void update_set0(frame_slot& f) noexcept
    {
        VkDescriptorBufferInfo bi{};
        bi.buffer = f.shapes.buf;
        bi.offset = 0;
        bi.range  = VK_WHOLE_SIZE;

        VkDescriptorImageInfo ii{};
        ii.sampler     = sampler;
        ii.imageView   = atlas.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet w[2]{};
        w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet          = f.set0;
        w[0].dstBinding      = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo     = &bi;

        w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet          = f.set0;
        w[1].dstBinding      = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo      = &ii;
        vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
    }

    void update_set1(VkDescriptorSet set, VkImageView view, VkSampler samp = VK_NULL_HANDLE) noexcept
    {
        VkDescriptorImageInfo ii{};
        ii.sampler     = samp != VK_NULL_HANDLE ? samp : sampler;
        ii.imageView   = view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet          = set;
        w.dstBinding      = 0;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo      = &ii;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
    }

    // blocking queue upload for font atlas (called at create / update_atlas)
    [[nodiscard]] bool upload_atlas_blocking(const font_atlas& font, gpu_image& out_img) noexcept
    {
        const u32 w = font.width();
        const u32 h = font.height();
        if (w == 0 || h == 0 || font.pixels().empty()) { return false; }

        gpu_image img;
        if (!create_image(w, h, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img)) {
            return false;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h;
        gpu_buffer staging;
        if (!create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, true)) {
            vkDestroyImageView(device, img.view, nullptr);
            vkDestroyImage(device, img.img, nullptr);
            vkFreeMemory(device, img.mem, nullptr);
            return false;
        }
        std::memcpy(staging.mapped, font.pixels().data(), static_cast<std::size_t>(bytes));

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool ok = vkCreateCommandPool(device, &pci, nullptr, &pool) == VK_SUCCESS;
        if (ok) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool        = pool;
            ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            ok = vkAllocateCommandBuffers(device, &ai, &cmd) == VK_SUCCESS;
        }
        if (ok) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            ok = vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS;
        }
        if (ok) {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ok = vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
        }
        if (ok) {
            cmd_image_barrier(cmd, img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent                 = {w, h, 1};
            vkCmdCopyBufferToImage(cmd, staging.buf, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            cmd_image_barrier(cmd, img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
        }
        if (ok) {
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &cmd;
            const VkResult r = vkQueueSubmit(queue, 1, &si, fence);
            check_lost(r);
            ok = r == VK_SUCCESS;
            if (ok) {
                const VkResult wr = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
                check_lost(wr);
                ok = wr == VK_SUCCESS;
            }
        }

        if (fence != VK_NULL_HANDLE) { vkDestroyFence(device, fence, nullptr); }
        if (pool != VK_NULL_HANDLE)  { vkDestroyCommandPool(device, pool, nullptr); }
        // wipe staging before releasing
        {
            volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(staging.mapped);
            for (VkDeviceSize i = 0; i < staging.capacity; ++i) { p[i] = 0; }
            vkUnmapMemory(device, staging.mem);
            vkDestroyBuffer(device, staging.buf, nullptr);
            vkFreeMemory(device, staging.mem, nullptr);
        }
        if (!ok) {
            vkDestroyImageView(device, img.view, nullptr);
            vkDestroyImage(device, img.img, nullptr);
            vkFreeMemory(device, img.mem, nullptr);
            return false;
        }
        out_img = img;
        return true;
    }

    [[nodiscard]] bool stage_image_regions(VkImage image, u32 mip_levels, const texture_image& src,
                                           std::span<const texture_image::region> regions,
                                           VkImageLayout before, VkImageLayout after)
    {
        const u32 bpp = texture_layout_bytes(src.layout());
        std::vector<VkDeviceSize> offsets;
        offsets.reserve(regions.size());
        VkDeviceSize total = 0;
        for (const texture_image::region& r : regions) {
            total = align_up(total, 16);
            offsets.push_back(total);
            total += static_cast<VkDeviceSize>(r.w) * r.h * bpp;
        }
        gpu_buffer staging;
        if (!create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, true)) {
            return false;
        }
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const texture_image::region& r = regions[i];
            const u32 src_pitch = src.pitch(r.level);
            const u32 dst_pitch = r.w * bpp;
            const u8* first = src.pixels(r.level).data() + static_cast<std::size_t>(r.y) * src_pitch + static_cast<std::size_t>(r.x) * bpp;
            for (u32 row = 0; row < r.h; ++row) {
                std::memcpy(static_cast<std::byte*>(staging.mapped) + offsets[i] + static_cast<std::size_t>(row) * dst_pitch,
                            first + static_cast<std::size_t>(row) * src_pitch, dst_pitch);
            }
        }
        pending_copy pc;
        pc.image      = image;
        pc.staging    = staging;
        pc.regions.assign(regions.begin(), regions.end());
        pc.offsets    = std::move(offsets);
        pc.mip_levels = mip_levels;
        pc.before     = before;
        pc.after      = after;
        pending.push_back(std::move(pc));
        return true;
    }

    void record_pending(VkCommandBuffer cmd) noexcept
    {
        for (pending_copy& pc : pending) {
            const VkPipelineStageFlags src_stage = pc.before == VK_IMAGE_LAYOUT_UNDEFINED
                                                       ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                       : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            const VkAccessFlags src_access = pc.before == VK_IMAGE_LAYOUT_UNDEFINED ? 0u : VK_ACCESS_SHADER_READ_BIT;
            cmd_image_barrier(cmd, pc.image, pc.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              src_access, VK_ACCESS_TRANSFER_WRITE_BIT,
                              src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, pc.mip_levels);
            for (std::size_t i = 0; i < pc.regions.size(); ++i) {
                const texture_image::region& r = pc.regions[i];
                VkBufferImageCopy c{};
                c.bufferOffset                = pc.offsets[i];
                c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                c.imageSubresource.mipLevel   = r.level;
                c.imageSubresource.layerCount = 1;
                c.imageOffset                 = {static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), 0};
                c.imageExtent                 = {r.w, r.h, 1};
                vkCmdCopyBufferToImage(cmd, pc.staging.buf, pc.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            }
            cmd_image_barrier(cmd, pc.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pc.after,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, pc.mip_levels);
            retire_buffer(pc.staging);
        }
        pending.clear();
    }

    void drop_pending(VkImage img) noexcept
    {
        for (pending_copy& pc : pending) {
            if (pc.image == img) {
                grave g{pc.staging.buf, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, pc.staging.mem, pc.staging.mapped, pc.staging.capacity, 0};
                destroy_grave_now(g);
                pc.staging = {};
            }
        }
        std::erase_if(pending, [img](const pending_copy& pc) { return pc.image == img; });
    }

    [[nodiscard]] bool ensure_frame_buffers(frame_slot& f, VkDeviceSize geo_bytes, VkDeviceSize shape_bytes) noexcept
    {
        if (f.geo.buf == VK_NULL_HANDLE || f.geo.capacity < geo_bytes) {
            const VkDeviceSize cap = align_up(geo_bytes + geo_bytes / 2 + 65536, 4096);
            gpu_buffer fresh;
            if (!create_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fresh, true)) {
                return false;
            }
            retire_buffer(f.geo);
            f.geo           = fresh;
            f.uploaded_hash = 0;
        }
        if (f.shapes.buf == VK_NULL_HANDLE || f.shapes.capacity < shape_bytes) {
            const VkDeviceSize cap = align_up(shape_bytes + shape_bytes / 2 + 16384, 4096);
            gpu_buffer fresh;
            if (!create_buffer(cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, fresh, true)) {
                return false;
            }
            retire_buffer(f.shapes);
            f.shapes        = fresh;
            f.uploaded_hash = 0;
            update_set0(f);
        }
        return true;
    }

    [[nodiscard]] VkFramebuffer get_framebuffer(const vulkan_target& target) noexcept
    {
        for (const fb_entry& e : fb_cache) {
            if (e.view == target.view && e.extent.width == target.extent.width && e.extent.height == target.extent.height) {
                return e.fb;
            }
        }
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass      = ui_pass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &target.view;
        fci.width           = target.extent.width;
        fci.height          = target.extent.height;
        fci.layers          = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        const VkResult r = vkCreateFramebuffer(device, &fci, nullptr, &fb);
        check_lost(r);
        if (r != VK_SUCCESS) { return VK_NULL_HANDLE; }
        fb_cache.push_back({target.view, target.extent, fb});
        return fb;
    }

    [[nodiscard]] bool ensure_blur(u32 fi, u32 w, u32 h) noexcept
    {
        if (fi >= blur_frames.size()) { return false; }
        blur_frame& bf = blur_frames[fi];
        if (bf.snap.img == VK_NULL_HANDLE || bf.snap_w != w || bf.snap_h != h) {
            retire_image(bf.snap);
            if (!create_image(w, h, 1, color_format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, bf.snap)) {
                return false;
            }
            update_set1(bf.snap_set, bf.snap.view);
            bf.snap_w = w; bf.snap_h = h;
        }
        const u32 lw = (w + 1) / 2;
        const u32 lh = (h + 1) / 2;
        if (bf.blur_img[0].img == VK_NULL_HANDLE || bf.blur_w != lw || bf.blur_h != lh) {
            for (int k = 0; k < 2; ++k) {
                retire_framebuffer(bf.blur_fb[k]);
                retire_image(bf.blur_img[k]);
                if (!create_image(lw, lh, 1, blur_rt_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, bf.blur_img[k])) {
                    return false;
                }
                VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                fci.renderPass      = blur_pass_rp;
                fci.attachmentCount = 1;
                fci.pAttachments    = &bf.blur_img[k].view;
                fci.width           = lw;
                fci.height          = lh;
                fci.layers          = 1;
                const VkResult r = vkCreateFramebuffer(device, &fci, nullptr, &bf.blur_fb[k]);
                check_lost(r);
                if (r != VK_SUCCESS) { return false; }
                update_set1(bf.blur_set[k], bf.blur_img[k].view);
            }
            bf.blur_w = lw; bf.blur_h = lh;
        }
        return true;
    }

    void run_blur_pass(VkCommandBuffer cmd, u32 fi, int dst, u32 vw, u32 vh, VkDescriptorSet src_set,
                       const blur_constants& c, VkPipeline pipe, const internal::blur_plan& plan) noexcept
    {
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass        = blur_pass_rp;
        rbi.framebuffer       = blur_frames[fi].blur_fb[dst];
        rbi.renderArea.offset = {static_cast<int32_t>(plan.lx0), static_cast<int32_t>(plan.ly0)};
        rbi.renderArea.extent = {plan.lx1 - plan.lx0, plan.ly1 - plan.ly0};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

        const VkViewport vp{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
        const VkRect2D   sc{{static_cast<int32_t>(plan.lx0), static_cast<int32_t>(plan.ly0)},
                            {plan.lx1 - plan.lx0, plan.ly1 - plan.ly0}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout_blur, 0, 1, &src_set, 0, nullptr);
        vkCmdPushConstants(cmd, pipe_layout_blur, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(blur_constants), &c);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // must be called outside a render pass; leaves target.image in COLOR_ATTACHMENT_OPTIMAL
    [[nodiscard]] bool run_blur(VkCommandBuffer cmd, u32 fi, const vulkan_target& target, float radius_px, const rect& panel,
                                u32& out_vw, u32& out_vh, rect& out_exact) noexcept
    {
        if (!ensure_blur(fi, target.extent.width, target.extent.height)) { return false; }
        blur_frame& bf = blur_frames[fi];
        const internal::blur_plan plan = internal::plan_blur(radius_px, panel, bf.snap_w, bf.snap_h);
        if (plan.x1 <= plan.x0 || plan.y1 <= plan.y0 || plan.lx1 <= plan.lx0 || plan.ly1 <= plan.ly0) {
            return false;
        }

        cmd_image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        cmd_image_barrier(cmd, bf.snap.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.srcOffset = {static_cast<int32_t>(plan.x0), static_cast<int32_t>(plan.y0), 0};
        copy.dstOffset = copy.srcOffset;
        copy.extent    = {plan.x1 - plan.x0, plan.y1 - plan.y0, 1};
        vkCmdCopyImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       bf.snap.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        cmd_image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        cmd_image_barrier(cmd, bf.snap.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        const u32   ds = plan.ds;
        const u32   vw = std::max(1u, (bf.snap_w + ds - 1) / ds);
        const u32   vh = std::max(1u, (bf.snap_h + ds - 1) / ds);
        const float tw = static_cast<float>(bf.blur_w);
        const float th = static_cast<float>(bf.blur_h);

        blur_constants c;
        c.bp0[0] = static_cast<float>(vw); c.bp0[1] = static_cast<float>(vh); c.bp0[2] = 1.0f; c.bp0[3] = 1.0f;
        c.bp1[0] = 1.0f / static_cast<float>(bf.snap_w); c.bp1[1] = 1.0f / static_cast<float>(bf.snap_h);
        c.bp1[2] = 1.0f - 0.5f * c.bp1[0]; c.bp1[3] = 1.0f - 0.5f * c.bp1[1];
        c.bp2[0] = static_cast<float>(ds) * 0.25f;
        run_blur_pass(cmd, fi, 0, vw, vh, bf.snap_set, c, pipe_blur_down, plan);

        c.bp0[2] = static_cast<float>(vw) / tw; c.bp0[3] = static_cast<float>(vh) / th;
        c.bp1[2] = (static_cast<float>(vw) - 0.5f) / tw; c.bp1[3] = (static_cast<float>(vh) - 0.5f) / th;
        c.bp2[2] = plan.w0;
        c.bp3[0] = plan.w[0]; c.bp3[1] = plan.o[0]; c.bp3[2] = plan.w[1]; c.bp3[3] = plan.o[1];
        c.bp4[0] = plan.w[2]; c.bp4[1] = plan.o[2];
        for (int it = 0; it < plan.iterations; ++it) {
            c.bp1[0] = 1.0f / tw; c.bp1[1] = 0.0f;
            run_blur_pass(cmd, fi, 1, vw, vh, bf.blur_set[0], c, pipe_blur_gauss, plan);
            c.bp1[0] = 0.0f; c.bp1[1] = 1.0f / th;
            run_blur_pass(cmd, fi, 0, vw, vh, bf.blur_set[1], c, pipe_blur_gauss, plan);
        }

        out_vw    = vw;
        out_vh    = vh;
        out_exact = plan.exact;
        return true;
    }

    void release() noexcept
    {
        if (device == VK_NULL_HANDLE) { return; }
        for (pending_copy& pc : pending) {
            grave g{pc.staging.buf, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, pc.staging.mem, pc.staging.mapped, pc.staging.capacity, 0};
            destroy_grave_now(g);
        }
        pending.clear();
        for (grave& g : graveyard) { destroy_grave_now(g); }
        graveyard.clear();
        for (fb_entry& e : fb_cache) {
            if (e.fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, e.fb, nullptr); }
        }
        fb_cache.clear();
        for (texture_slot& t : textures) {
            t.cpu_image.wipe();
            if (t.owned) {
                grave g{VK_NULL_HANDLE, t.img.img, t.img.view, VK_NULL_HANDLE, t.img.mem, nullptr, 0, 0};
                destroy_grave_now(g);
            }
            t = {};
        }
        for (frame_slot& f : frames) {
            grave g1{f.geo.buf, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, f.geo.mem, f.geo.mapped, f.geo.capacity, 0};
            grave g2{f.shapes.buf, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, f.shapes.mem, f.shapes.mapped, f.shapes.capacity, 0};
            destroy_grave_now(g1);
            destroy_grave_now(g2);
        }
        frames.clear();
        {
            grave ga{VK_NULL_HANDLE, atlas.img, atlas.view, VK_NULL_HANDLE, atlas.mem, nullptr, 0, 0};
            destroy_grave_now(ga);
            atlas = {};
            for (blur_frame& bf : blur_frames) {
                grave gs{VK_NULL_HANDLE, bf.snap.img, bf.snap.view, VK_NULL_HANDLE, bf.snap.mem, nullptr, 0, 0};
                destroy_grave_now(gs);
                bf.snap = {};
                for (int k = 0; k < 2; ++k) {
                    grave gb{VK_NULL_HANDLE, bf.blur_img[k].img, bf.blur_img[k].view, bf.blur_fb[k], bf.blur_img[k].mem, nullptr, 0, 0};
                    destroy_grave_now(gb);
                    bf.blur_img[k] = {};
                    bf.blur_fb[k]  = VK_NULL_HANDLE;
                }
            }
            blur_frames.clear();
        }
        if (pipe_main != VK_NULL_HANDLE)       { vkDestroyPipeline(device, pipe_main, nullptr); }
        if (pipe_image != VK_NULL_HANDLE)      { vkDestroyPipeline(device, pipe_image, nullptr); }
        if (pipe_backdrop != VK_NULL_HANDLE)   { vkDestroyPipeline(device, pipe_backdrop, nullptr); }
        if (pipe_blur_down != VK_NULL_HANDLE)  { vkDestroyPipeline(device, pipe_blur_down, nullptr); }
        if (pipe_blur_gauss != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipe_blur_gauss, nullptr); }
        if (pipe_layout_ui != VK_NULL_HANDLE)  { vkDestroyPipelineLayout(device, pipe_layout_ui, nullptr); }
        if (pipe_layout_blur != VK_NULL_HANDLE){ vkDestroyPipelineLayout(device, pipe_layout_blur, nullptr); }
        if (ui_pass != VK_NULL_HANDLE)         { vkDestroyRenderPass(device, ui_pass, nullptr); }
        if (blur_pass_rp != VK_NULL_HANDLE)    { vkDestroyRenderPass(device, blur_pass_rp, nullptr); }
        if (desc_pool != VK_NULL_HANDLE)       { vkDestroyDescriptorPool(device, desc_pool, nullptr); }
        if (set0_layout != VK_NULL_HANDLE)     { vkDestroyDescriptorSetLayout(device, set0_layout, nullptr); }
        if (set1_layout != VK_NULL_HANDLE)     { vkDestroyDescriptorSetLayout(device, set1_layout, nullptr); }
        if (sampler != VK_NULL_HANDLE)         { vkDestroySampler(device, sampler, nullptr); }
        device = VK_NULL_HANDLE;
    }
};

vulkan_renderer::vulkan_renderer() noexcept = default;
vulkan_renderer::~vulkan_renderer() { destroy(); }
vulkan_renderer::vulkan_renderer(vulkan_renderer&&) noexcept            = default;
vulkan_renderer& vulkan_renderer::operator=(vulkan_renderer&&) noexcept = default;

bool vulkan_renderer::create(const vulkan_init_info& info, const font_atlas& font)
{
    destroy();
    if (info.physical_device == VK_NULL_HANDLE || info.device == VK_NULL_HANDLE ||
        info.queue == VK_NULL_HANDLE || info.frames_in_flight == 0) {
        return false;
    }

    auto p = std::make_unique<impl>();
    p->phys             = info.physical_device;
    p->device           = info.device;
    p->queue            = info.queue;
    p->queue_family     = info.queue_family;
    p->frames_in_flight = info.frames_in_flight;
    p->color_format     = info.color_format;
    p->samples          = info.samples;
    p->blur_rt_format   = wide_vk_format(info.color_format) ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    vkGetPhysicalDeviceMemoryProperties(info.physical_device, &p->mem_props);

    const auto fail = [&] { p->release(); return false; };

    // linear-clamp sampler
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter    = VK_FILTER_LINEAR;
    sci.minFilter    = VK_FILTER_LINEAR;
    sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod       = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(p->device, &sci, nullptr, &p->sampler) != VK_SUCCESS) { return fail(); }

    // descriptor set layouts: set 0 = shape SSBO + font atlas, set 1 = image / blur source
    VkDescriptorSetLayoutBinding b0[2]{};
    b0[0].binding         = 0;
    b0[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b0[0].descriptorCount = 1;
    b0[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    b0[1].binding         = 1;
    b0[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0[1].descriptorCount = 1;
    b0[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl0.bindingCount = 2;
    dsl0.pBindings    = b0;
    if (vkCreateDescriptorSetLayout(p->device, &dsl0, nullptr, &p->set0_layout) != VK_SUCCESS) { return fail(); }

    VkDescriptorSetLayoutBinding b1{};
    b1.binding         = 0;
    b1.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1.descriptorCount = 1;
    b1.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl1.bindingCount = 1;
    dsl1.pBindings    = &b1;
    if (vkCreateDescriptorSetLayout(p->device, &dsl1, nullptr, &p->set1_layout) != VK_SUCCESS) { return fail(); }

    // descriptor pool
    const u32 set1_count = max_textures + 3 * info.frames_in_flight; // user textures + per-frame (snap + blur[0..1])
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, info.frames_in_flight};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info.frames_in_flight + set1_count};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets       = info.frames_in_flight + set1_count;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes    = pool_sizes;
    if (vkCreateDescriptorPool(p->device, &dpci, nullptr, &p->desc_pool) != VK_SUCCESS) { return fail(); }

    // pipeline layouts
    VkDescriptorSetLayout ui_sets[2] = {p->set0_layout, p->set1_layout};
    VkPushConstantRange   ui_pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(internal::ui_constants) + sizeof(blur_constants)};
    VkPipelineLayoutCreateInfo pl_ui{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_ui.setLayoutCount         = 2;
    pl_ui.pSetLayouts            = ui_sets;
    pl_ui.pushConstantRangeCount = 1;
    pl_ui.pPushConstantRanges    = &ui_pcr;
    if (vkCreatePipelineLayout(p->device, &pl_ui, nullptr, &p->pipe_layout_ui) != VK_SUCCESS) { return fail(); }

    VkPushConstantRange blur_pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(blur_constants)};
    VkPipelineLayoutCreateInfo pl_blur{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl_blur.setLayoutCount         = 1;
    pl_blur.pSetLayouts            = &p->set1_layout;
    pl_blur.pushConstantRangeCount = 1;
    pl_blur.pPushConstantRanges    = &blur_pcr;
    if (vkCreatePipelineLayout(p->device, &pl_blur, nullptr, &p->pipe_layout_blur) != VK_SUCCESS) { return fail(); }

    // UI render pass (preserves target contents; host clears/draws scene before calling render())
    {
        VkAttachmentDescription att{};
        att.format         = info.color_format;
        att.samples        = info.samples;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription  sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        if (vkCreateRenderPass(p->device, &rpci, nullptr, &p->ui_pass) != VK_SUCCESS) { return fail(); }
    }

    // blur render pass (writes to blur_img[0..1], transitions to SHADER_READ_ONLY_OPTIMAL)
    {
        VkAttachmentDescription att{};
        att.format         = p->blur_rt_format;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription  sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass      = 0;
        deps[0].srcStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        deps[1].srcSubpass      = 0;
        deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
        deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies   = deps;
        if (vkCreateRenderPass(p->device, &rpci, nullptr, &p->blur_pass_rp) != VK_SUCCESS) { return fail(); }
    }

    // shader modules (destroyed after pipeline creation)
    VkShaderModule vs_main   = create_shader_module(p->device, g_strata_vk_vs);
    VkShaderModule ps_main   = create_shader_module(p->device, g_strata_vk_ps);
    VkShaderModule ps_image  = create_shader_module(p->device, g_strata_vk_ps_image);
    VkShaderModule ps_back   = create_shader_module(p->device, g_strata_vk_ps_backdrop);
    VkShaderModule vs_fs     = create_shader_module(p->device, g_strata_vk_vs_fullscreen);
    VkShaderModule ps_down   = create_shader_module(p->device, g_strata_vk_ps_blur_down);
    VkShaderModule ps_gauss  = create_shader_module(p->device, g_strata_vk_ps_blur_gauss);

    const auto free_shaders = [&] {
        for (VkShaderModule m : {vs_main, ps_main, ps_image, ps_back, vs_fs, ps_down, ps_gauss}) {
            if (m != VK_NULL_HANDLE) { vkDestroyShaderModule(p->device, m, nullptr); }
        }
    };
    if (vs_main == VK_NULL_HANDLE || ps_main == VK_NULL_HANDLE || ps_image == VK_NULL_HANDLE) {
        free_shaders();
        return fail();
    }

    // graphics pipelines
    {
        VkVertexInputBindingDescription bind{0, sizeof(vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3]{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vertex, pos)};
        attrs[1] = {1, 0, VK_FORMAT_R16G16_UINT, offsetof(vertex, u)};
        attrs[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(vertex, col)};

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bind;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions    = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = info.samples;

        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable         = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp        = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp        = VK_BLEND_OP_ADD;
        ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments    = &ba;

        const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dyn_states;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_main;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps_main;
        stages[1].pName  = "main";

        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount          = 2;
        gpci.pStages             = stages;
        gpci.pVertexInputState   = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState      = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState   = &ms;
        gpci.pColorBlendState    = &cb;
        gpci.pDynamicState       = &dyn;
        gpci.layout              = p->pipe_layout_ui;
        gpci.renderPass          = p->ui_pass;

        if (vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipe_main) != VK_SUCCESS) {
            free_shaders();
            return fail();
        }
        stages[1].module = ps_image;
        if (vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipe_image) != VK_SUCCESS) {
            free_shaders();
            return fail();
        }

        // optional blur pipelines (only when target is single-sample)
        if (info.samples == VK_SAMPLE_COUNT_1_BIT &&
            ps_back != VK_NULL_HANDLE && vs_fs != VK_NULL_HANDLE && ps_down != VK_NULL_HANDLE && ps_gauss != VK_NULL_HANDLE) {
            stages[1].module = ps_back;
            bool ok = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipe_backdrop) == VK_SUCCESS;

            VkPipelineVertexInputStateCreateInfo empty_vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            ba.blendEnable          = VK_FALSE;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            gpci.pVertexInputState  = &empty_vi;
            gpci.layout             = p->pipe_layout_blur;
            gpci.renderPass         = p->blur_pass_rp;
            stages[0].module        = vs_fs;
            stages[1].module        = ps_down;
            ok = ok && vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipe_blur_down) == VK_SUCCESS;
            stages[1].module = ps_gauss;
            ok = ok && vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &gpci, nullptr, &p->pipe_blur_gauss) == VK_SUCCESS;
            p->blur_supported = ok;
        }
    }
    free_shaders();

    // upload font atlas
    if (!p->upload_atlas_blocking(font, p->atlas)) { return fail(); }
    p->atlas_w = font.width();
    p->atlas_h = font.height();

    // allocate per-frame descriptor sets + initial dummy shape buffers
    p->frames.resize(info.frames_in_flight);
    for (impl::frame_slot& f : p->frames) {
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = p->desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &p->set0_layout;
        if (vkAllocateDescriptorSets(p->device, &dsai, &f.set0) != VK_SUCCESS) { return fail(); }
        if (!p->create_buffer(sizeof(shape_record) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.shapes, true)) {
            return fail();
        }
        std::memset(f.shapes.mapped, 0, sizeof(shape_record));
        p->update_set0(f);
    }

    // preallocate set 1 descriptor sets for textures + blur targets
    {
        std::vector<VkDescriptorSetLayout> layouts(set1_count, p->set1_layout);
        std::vector<VkDescriptorSet>       sets(set1_count, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool     = p->desc_pool;
        dsai.descriptorSetCount = set1_count;
        dsai.pSetLayouts        = layouts.data();
        if (vkAllocateDescriptorSets(p->device, &dsai, sets.data()) != VK_SUCCESS) { return fail(); }
        for (u32 i = 0; i < max_textures; ++i) { p->textures[i].set = sets[i]; }
        p->blur_frames.resize(info.frames_in_flight);
        for (u32 fi = 0; fi < info.frames_in_flight; ++fi) {
            p->blur_frames[fi].snap_set    = sets[max_textures + 3 * fi + 0];
            p->blur_frames[fi].blur_set[0] = sets[max_textures + 3 * fi + 1];
            p->blur_frames[fi].blur_set[1] = sets[max_textures + 3 * fi + 2];
        }
    }

    impl_ = std::move(p);
    return true;
}

void vulkan_renderer::destroy() noexcept
{
    if (impl_ != nullptr) {
        impl_->release();
        impl_.reset();
    }
}

void vulkan_renderer::clear_target_cache() noexcept
{
    if (impl_ == nullptr) { return; }
    for (impl::fb_entry& e : impl_->fb_cache) {
        if (e.fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(impl_->device, e.fb, nullptr); }
    }
    impl_->fb_cache.clear();
}

void vulkan_renderer::set_output(const output_desc& output) noexcept
{
    if (impl_ != nullptr) { impl_->output = output; }
}

output_desc vulkan_renderer::output() const noexcept
{
    return impl_ != nullptr ? impl_->output : output_desc{};
}

bool vulkan_renderer::device_lost() const noexcept
{
    return impl_ != nullptr && impl_->lost;
}

bool vulkan_renderer::update_atlas(const font_atlas& font)
{
    if (impl_ == nullptr || font.pixels().empty()) { return false; }
    impl& s = *impl_;
    gpu_image fresh;
    if (!s.upload_atlas_blocking(font, fresh)) { return false; }
    s.drop_pending(s.atlas.img);
    s.retire_image(s.atlas);
    s.atlas   = fresh;
    s.atlas_w = font.width();
    s.atlas_h = font.height();
    for (impl::frame_slot& f : s.frames) { s.update_set0(f); }
    return true;
}

bool vulkan_renderer::update_atlas_region(const font_atlas& font, atlas_region r)
{
    if (impl_ == nullptr || r.empty() || font.pixels().empty()) { return false; }
    impl& s = *impl_;
    if (s.atlas.img == VK_NULL_HANDLE || font.width() != s.atlas_w || font.height() != s.atlas_h ||
        r.x + r.w > s.atlas_w || r.y + r.h > s.atlas_h) {
        return false;
    }
    const VkDeviceSize total = static_cast<VkDeviceSize>(r.w) * r.h;
    gpu_buffer staging;
    if (!s.create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, true)) {
        return false;
    }
    const u8* src = font.pixels().data() + static_cast<std::size_t>(r.y) * s.atlas_w + r.x;
    for (u32 row = 0; row < r.h; ++row) {
        std::memcpy(static_cast<std::byte*>(staging.mapped) + static_cast<std::size_t>(row) * r.w,
                    src + static_cast<std::size_t>(row) * s.atlas_w, r.w);
    }
    impl::pending_copy pc;
    pc.image      = s.atlas.img;
    pc.staging    = staging;
    pc.regions.push_back({0, r.x, r.y, r.w, r.h});
    pc.offsets.push_back(0);
    pc.mip_levels = 1;
    pc.before     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pc.after      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    s.pending.push_back(std::move(pc));
    return true;
}

texture_id vulkan_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id vulkan_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (impl_ == nullptr) { return 0; }
    impl& s = *impl_;
    u32 slot = 0;
    while (slot < max_textures && (s.textures[slot].occupied || s.textures[slot].cooldown > 0)) { ++slot; }
    if (slot == max_textures) { return 0; }

    texture_image image;
    if (!image.create(desc, pixels)) { return 0; }
    const u32 levels = image.level_count();
    gpu_image gpu;
    if (!s.create_image(image.width(), image.height(), levels, vk_format_of(image.layout()),
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, gpu)) {
        image.wipe();
        return 0;
    }
    std::vector<texture_image::region> all;
    for (u32 k = 0; k < levels; ++k) { all.push_back({k, 0, 0, image.width(k), image.height(k)}); }
    if (!s.stage_image_regions(gpu.img, levels, image, all, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        vkDestroyImageView(s.device, gpu.view, nullptr);
        vkDestroyImage(s.device, gpu.img, nullptr);
        vkFreeMemory(s.device, gpu.mem, nullptr);
        image.wipe();
        return 0;
    }

    impl::texture_slot& ts = s.textures[slot];
    ts.img        = gpu;
    ts.mip_levels = levels;
    ts.occupied   = true;
    ts.owned      = true;
    ts.updatable  = desc.updatable;
    s.update_set1(ts.set, gpu.view);
    if (desc.updatable) { ts.cpu_image = std::move(image); }
    else                { image.wipe(); }
    return slot + 1;
}

bool vulkan_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (impl_ == nullptr || id == 0 || id > max_textures) { return false; }
    impl& s = *impl_;
    impl::texture_slot& ts = s.textures[id - 1];
    if (!ts.occupied || !ts.updatable) { return false; }
    std::vector<texture_image::region> dirty;
    if (!ts.cpu_image.update(x, y, width, height, pixels, dirty)) { return false; }
    return s.stage_image_regions(ts.img.img, ts.mip_levels, ts.cpu_image, dirty,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

texture_id vulkan_renderer::register_texture(VkImageView view, VkSampler samp)
{
    if (impl_ == nullptr || view == VK_NULL_HANDLE) { return 0; }
    impl& s = *impl_;
    u32 slot = 0;
    while (slot < max_textures && (s.textures[slot].occupied || s.textures[slot].cooldown > 0)) { ++slot; }
    if (slot == max_textures) { return 0; }

    impl::texture_slot& ts = s.textures[slot];
    ts.img       = {VK_NULL_HANDLE, VK_NULL_HANDLE, view};
    ts.occupied  = true;
    ts.owned     = false;
    ts.updatable = false;
    s.update_set1(ts.set, view, samp);
    return slot + 1;
}

void vulkan_renderer::destroy_texture(texture_id id) noexcept
{
    if (impl_ == nullptr || id == 0 || id > max_textures) { return; }
    impl& s = *impl_;
    impl::texture_slot& ts = s.textures[id - 1];
    if (!ts.occupied) { return; }
    if (ts.owned) {
        s.drop_pending(ts.img.img);
        s.retire_image(ts.img);
    } else {
        ts.img = {};
    }
    ts.cpu_image.wipe();
    ts.occupied  = false;
    ts.owned     = false;
    ts.updatable = false;
    ts.cooldown  = s.frames_in_flight + 1;
}

void vulkan_renderer::render(const draw_data& data, VkCommandBuffer cmd, u32 frame_index, const vulkan_target& target)
{
    if (impl_ == nullptr || data.commands.empty() || data.display_size.x <= 0.0f || data.display_size.y <= 0.0f ||
        target.view == VK_NULL_HANDLE || target.extent.width == 0 || target.extent.height == 0) {
        return;
    }
    impl& s = *impl_;
    s.tick_graveyard();
    if (frame_index >= s.frames.size()) { return; }

    // staged texture uploads happen outside any render pass
    s.record_pending(cmd);

    impl::frame_slot& fb = s.frames[frame_index];
    const VkDeviceSize vb_bytes = data.vertices.size_bytes();
    const VkDeviceSize ib_bytes = data.indices.size_bytes();
    const VkDeviceSize ib_off   = align_up(vb_bytes, 16);
    const VkDeviceSize geo_tot  = ib_off + ib_bytes;
    const VkDeviceSize sh_bytes = std::max<VkDeviceSize>(data.shapes.size_bytes(), sizeof(shape_record));
    if (!s.ensure_frame_buffers(fb, std::max<VkDeviceSize>(geo_tot, 64), sh_bytes)) { return; }

    if (data.content_hash == 0 || data.content_hash != fb.uploaded_hash) {
        if (vb_bytes != 0) { std::memcpy(fb.geo.mapped, data.vertices.data(), static_cast<std::size_t>(vb_bytes)); }
        if (ib_bytes != 0) { std::memcpy(static_cast<std::byte*>(fb.geo.mapped) + ib_off, data.indices.data(), static_cast<std::size_t>(ib_bytes)); }
        if (!data.shapes.empty()) { std::memcpy(fb.shapes.mapped, data.shapes.data(), data.shapes.size_bytes()); }
        fb.ib_offset     = ib_off;
        fb.uploaded_hash = data.content_hash;
    }

    const VkFramebuffer host_fb = s.get_framebuffer(target);
    if (host_fb == VK_NULL_HANDLE) { return; }

    const bool transition_target = target.image != VK_NULL_HANDLE && target.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (transition_target) {
        cmd_image_barrier(cmd, target.image, target.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    const internal::ui_constants uc = internal::make_ui_constants(data, s.output);
    const VkViewport vp{0.0f, 0.0f, data.display_size.x, data.display_size.y, 0.0f, 1.0f};
    bool pass_open = false;

    const auto begin_ui_pass = [&] {
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass        = s.ui_pass;
        rbi.framebuffer       = host_fb;
        rbi.renderArea.extent = target.extent;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        pass_open = true;

        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_main);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout_ui, 0, 1, &fb.set0, 0, nullptr);
        vkCmdPushConstants(cmd, s.pipe_layout_ui, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uc), &uc);
        const VkDeviceSize vb_off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &fb.geo.buf, &vb_off);
        vkCmdBindIndexBuffer(cmd, fb.geo.buf, fb.ib_offset, VK_INDEX_TYPE_UINT16);
    };

    begin_ui_pass();

    const bool can_blur = s.blur_supported && target.image != VK_NULL_HANDLE &&
                          data.origin.x == 0.0f && data.origin.y == 0.0f;
    enum class ps_mode { main, image, backdrop };
    ps_mode    mode        = ps_mode::main;
    texture_id bound       = 0;
    bool       blur_dirty  = true;
    bool       blur_ready  = false;
    float      blur_radius = -1.0f;
    rect       blur_exact{};
    u32        blur_vw = 0, blur_vh = 0;
    bool       has_scissor = false;
    VkRect2D   bound_sc{};

    for (const draw_cmd& dc : data.commands) {
        if (dc.idx_count == 0 || dc.clip.empty()) { continue; }
        const auto fit = [](f32 v, f32 hi) { return static_cast<int32_t>(std::clamp(std::ceil(v - 0.5f), 0.0f, hi)); };
        const int32_t left   = fit(dc.clip.min.x - data.origin.x, data.display_size.x);
        const int32_t top    = fit(dc.clip.min.y - data.origin.y, data.display_size.y);
        const int32_t right  = fit(dc.clip.max.x - data.origin.x, data.display_size.x);
        const int32_t bottom = fit(dc.clip.max.y - data.origin.y, data.display_size.y);
        if (right <= left || bottom <= top) { continue; }

        ps_mode want = dc.texture != 0 ? ps_mode::image : ps_mode::main;
        if (dc.blur > 0.0f && can_blur) {
            const rect panel = internal::backdrop_bounds(data, dc);
            if (blur_dirty || dc.blur != blur_radius || !internal::rect_inside(panel, blur_exact)) {
                blur_dirty = true;
                if (!panel.empty()) {
                    // end the UI render pass so transfer & blur render passes can run
                    if (pass_open) { vkCmdEndRenderPass(cmd); pass_open = false; }
                    blur_ready = s.run_blur(cmd, frame_index, target, dc.blur, panel, blur_vw, blur_vh, blur_exact);
                    begin_ui_pass();
                    mode        = ps_mode::main;
                    bound       = 0;
                    has_scissor = false;
                    if (blur_ready) {
                        blur_dirty  = false;
                        blur_radius = dc.blur;
                    }
                }
            }
            if (blur_ready && !blur_dirty) { want = ps_mode::backdrop; }
        }

        if (want == ps_mode::image) {
            if (dc.texture > max_textures || !s.textures[dc.texture - 1].occupied) { continue; }
            if (mode != ps_mode::image) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_image); }
            if (mode != ps_mode::image || bound != dc.texture) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout_ui, 1, 1,
                                        &s.textures[dc.texture - 1].set, 0, nullptr);
            }
            bound = dc.texture;
        } else if (want == ps_mode::backdrop) {
            if (mode != ps_mode::backdrop) {
                const impl::blur_frame& bff = s.blur_frames[frame_index];
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_backdrop);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_layout_ui, 1, 1, &bff.blur_set[0], 0, nullptr);
                blur_constants bc;
                bc.bp0[0] = data.display_size.x;
                bc.bp0[1] = data.display_size.y;
                bc.bp0[2] = static_cast<float>(blur_vw) / static_cast<float>(bff.blur_w);
                bc.bp0[3] = static_cast<float>(blur_vh) / static_cast<float>(bff.blur_h);
                bc.bp1[2] = (static_cast<float>(blur_vw) - 0.5f) / static_cast<float>(bff.blur_w);
                bc.bp1[3] = (static_cast<float>(blur_vh) - 0.5f) / static_cast<float>(bff.blur_h);
                vkCmdPushConstants(cmd, s.pipe_layout_ui, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   sizeof(uc), sizeof(bc), &bc);
            }
            bound = 0;
        } else if (mode != ps_mode::main) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe_main);
            bound = 0;
        }
        if (want != ps_mode::backdrop) { blur_dirty = true; }
        mode = want;

        const VkRect2D sc{{left, top}, {static_cast<u32>(right - left), static_cast<u32>(bottom - top)}};
        if (!has_scissor || sc.offset.x != bound_sc.offset.x || sc.offset.y != bound_sc.offset.y ||
            sc.extent.width != bound_sc.extent.width || sc.extent.height != bound_sc.extent.height) {
            vkCmdSetScissor(cmd, 0, 1, &sc);
            bound_sc    = sc;
            has_scissor = true;
        }
        vkCmdDrawIndexed(cmd, dc.idx_count, 1, dc.idx_offset, static_cast<int32_t>(dc.vtx_offset), 0);
    }

    if (pass_open) { vkCmdEndRenderPass(cmd); }

    if (transition_target) {
        cmd_image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, target.layout,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
}

} // namespace strata

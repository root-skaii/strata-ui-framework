// Offscreen headless Vulkan renderer verification: SDF shapes, staged textures, backdrop blur, and output encoding.
// Uses a headless VkInstance/VkDevice and an offscreen RGBA8 VkImage (no window or swapchain required).

#include <strata/backend/vulkan.hpp>
#include <strata/context.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

[[nodiscard]] strata::u32 find_mem_type(const VkPhysicalDeviceMemoryProperties& props, strata::u32 bits, VkMemoryPropertyFlags req) noexcept
{
    for (strata::u32 i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & req) == req) { return i; }
    }
    return UINT32_MAX;
}

void img_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout old_l, VkImageLayout new_l,
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

struct vk_offscreen {
    static constexpr strata::u32 width  = 256;
    static constexpr strata::u32 height = 256;

    VkInstance       instance{VK_NULL_HANDLE};
    VkPhysicalDevice phys{VK_NULL_HANDLE};
    VkDevice         device{VK_NULL_HANDLE};
    VkQueue          queue{VK_NULL_HANDLE};
    strata::u32      qfamily{};
    VkCommandPool    pool{VK_NULL_HANDLE};
    VkCommandBuffer  cmd{VK_NULL_HANDLE};
    VkImage          image{VK_NULL_HANDLE};
    VkDeviceMemory   img_mem{VK_NULL_HANDLE};
    VkImageView      view{VK_NULL_HANDLE};
    VkBuffer         read_buf{VK_NULL_HANDLE};
    VkDeviceMemory   read_mem{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties mem_props{};

    ~vk_offscreen()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (read_buf != VK_NULL_HANDLE) { vkDestroyBuffer(device, read_buf, nullptr); }
            if (read_mem != VK_NULL_HANDLE) { vkFreeMemory(device, read_mem, nullptr); }
            if (view != VK_NULL_HANDLE)     { vkDestroyImageView(device, view, nullptr); }
            if (image != VK_NULL_HANDLE)    { vkDestroyImage(device, image, nullptr); }
            if (img_mem != VK_NULL_HANDLE)  { vkFreeMemory(device, img_mem, nullptr); }
            if (pool != VK_NULL_HANDLE)     { vkDestroyCommandPool(device, pool, nullptr); }
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) { vkDestroyInstance(instance, nullptr); }
    }

    [[nodiscard]] bool init()
    {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "strata_render_test_vk";
        ai.apiVersion       = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { return false; }

        strata::u32 gpu_count = 0;
        vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
        if (gpu_count == 0) { return false; }
        std::vector<VkPhysicalDevice> gpus(gpu_count);
        vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());

        for (VkPhysicalDevice pd : gpus) {
            strata::u32 qfc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
            std::vector<VkQueueFamilyProperties> qfp(qfc);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfp.data());
            for (strata::u32 q = 0; q < qfc; ++q) {
                if ((qfp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                    phys    = pd;
                    qfamily = q;
                    break;
                }
            }
            if (phys != VK_NULL_HANDLE) { break; }
        }
        if (phys == VK_NULL_HANDLE) { return false; }
        vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        dqci.queueFamilyIndex = qfamily;
        dqci.queueCount       = 1;
        dqci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos    = &dqci;
        if (vkCreateDevice(phys, &dci, nullptr, &device) != VK_SUCCESS) { return false; }
        vkGetDeviceQueue(device, qfamily, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qfamily;
        if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) { return false; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool        = pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) { return false; }

        VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imci.imageType     = VK_IMAGE_TYPE_2D;
        imci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imci.extent        = {width, height, 1};
        imci.mipLevels     = 1;
        imci.arrayLayers   = 1;
        imci.samples       = VK_SAMPLE_COUNT_1_BIT;
        imci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &imci, nullptr, &image) != VK_SUCCESS) { return false; }

        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(device, image, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_mem_type(mem_props, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &img_mem) != VK_SUCCESS ||
            vkBindImageMemory(device, image, img_mem, 0) != VK_SUCCESS) {
            return false;
        }

        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image                       = image;
        ivci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &ivci, nullptr, &view) != VK_SUCCESS) { return false; }

        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = static_cast<VkDeviceSize>(width) * height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &bci, nullptr, &read_buf) != VK_SUCCESS) { return false; }
        vkGetBufferMemoryRequirements(device, read_buf, &mr);
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_mem_type(mem_props, mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mai, nullptr, &read_mem) != VK_SUCCESS ||
            vkBindBufferMemory(device, read_buf, read_mem, 0) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    std::vector<strata::u8> render_and_read(strata::vulkan_renderer& ren, const strata::draw_data& dd, strata::u32 slot)
    {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        img_barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue cc{{0.05f, 0.06f, 0.08f, 1.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
        img_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        strata::vulkan_target target;
        target.image  = image;
        target.view   = view;
        target.extent = {width, height};
        target.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ren.render(dd, cmd, slot, target);

        img_barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buf, 1, &region);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);

        std::vector<strata::u8> out(static_cast<std::size_t>(width) * height * 4);
        void* mapped = nullptr;
        if (vkMapMemory(device, read_mem, 0, out.size(), 0, &mapped) == VK_SUCCESS) {
            std::memcpy(out.data(), mapped, out.size());
            vkUnmapMemory(device, read_mem);
        }
        return out;
    }
};

std::array<strata::u8, 4> pixel_at(const std::vector<strata::u8>& px, strata::u32 w, strata::u32 x, strata::u32 y)
{
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

} // namespace

int main()
{
    vk_offscreen host;
    if (!host.init()) {
        std::fprintf(stderr, "render_test_vk: failed to create headless Vulkan device\n");
        return 1;
    }

    auto created = strata::context::create();
    CHECK(created.has_value());
    if (!created) { return 1; }
    strata::context ui = std::move(*created);

    strata::vulkan_init_info info;
    info.physical_device  = host.phys;
    info.device           = host.device;
    info.queue            = host.queue;
    info.queue_family     = host.qfamily;
    info.frames_in_flight = 2;
    info.color_format     = VK_FORMAT_R8G8B8A8_UNORM;

    strata::vulkan_renderer ren;
    CHECK(ren.create(info, ui.font()));
    if (!ren.valid()) { return 1; }

    std::array<strata::u8, 16 * 16 * 4> tex_px{};
    for (std::size_t i = 0; i < tex_px.size(); i += 4) {
        tex_px[i] = 230; tex_px[i + 1] = 70; tex_px[i + 2] = 40; tex_px[i + 3] = 255;
    }
    const strata::texture_id tid = ren.create_texture(16, 16, tex_px);
    CHECK(tid != 0);

    auto run_scene = [&](bool with_backdrop) {
        strata::input_state in;
        in.display_size = {static_cast<float>(vk_offscreen::width), static_cast<float>(vk_offscreen::height)};
        ui.begin_frame(std::move(in));
        auto& dl = ui.draw();
        dl.rect_filled({{16, 16}, {112, 112}}, strata::color{60, 120, 220, 255}, 8.0f);
        dl.image({{140, 16}, {220, 96}}, tid);
        if (with_backdrop) {
            dl.backdrop({{20, 140}, {220, 230}}, 10.0f, strata::color{255, 255, 255, 60});
        }
        ui.end_frame();
    };

    run_scene(true);
    const auto px = host.render_and_read(ren, ui.render_data(), 0);
    const auto bg   = pixel_at(px, vk_offscreen::width, 4, 4);
    const auto rect = pixel_at(px, vk_offscreen::width, 64, 64);
    const auto img  = pixel_at(px, vk_offscreen::width, 180, 56);
    const auto blur = pixel_at(px, vk_offscreen::width, 120, 185);
    CHECK(bg[0] < 25 && bg[1] < 25 && bg[2] < 30);
    CHECK(rect[2] > 190 && rect[0] < 90);
    CHECK(img[0] > 200 && img[1] < 100 && img[2] < 70);
    CHECK(blur[0] > bg[0] + 25);

    ren.set_output({.space = strata::output_space::srgb_view});
    run_scene(false);
    const auto px_lin   = host.render_and_read(ren, ui.render_data(), 1);
    const auto rect_lin = pixel_at(px_lin, vk_offscreen::width, 64, 64);
    CHECK(rect_lin[2] < rect[2] - 20);

    ren.destroy_texture(tid);
    ren.destroy();

    if (g_failures == 0) { std::fprintf(stderr, "render_test_vk: ok\n"); }
    return g_failures == 0 ? 0 : 1;
}

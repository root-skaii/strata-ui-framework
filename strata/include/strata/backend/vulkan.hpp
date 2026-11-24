#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <vulkan/vulkan_core.h>

#include <memory>
#include <span>

namespace strata {

struct vulkan_init_info {
    VkPhysicalDevice      physical_device{};
    VkDevice              device{};
    VkQueue               queue{};            // graphics queue, used once to upload the font atlas
    u32                   queue_family{};
    u32                   frames_in_flight = 2;
    VkFormat              color_format     = VK_FORMAT_B8G8R8A8_UNORM;
    VkSampleCountFlagBits samples          = VK_SAMPLE_COUNT_1_BIT;
};

// target image for render() (recorded outside a render pass).
// `image` is required for backdrop blur (single-sample, created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT) and for
// automatic layout transitions when `layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`.
struct vulkan_target {
    VkImage       image{};
    VkImageView   view{};
    VkExtent2D    extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};

// records UI draw commands into an open command buffer (outside a render pass).
// upload buffers are per frame_index, so the caller must have waited on that frame slot's fence.
class vulkan_renderer {
public:
    vulkan_renderer() noexcept;
    ~vulkan_renderer();

    vulkan_renderer(const vulkan_renderer&)            = delete;
    vulkan_renderer& operator=(const vulkan_renderer&) = delete;
    vulkan_renderer(vulkan_renderer&&) noexcept;
    vulkan_renderer& operator=(vulkan_renderer&&) noexcept;

    [[nodiscard]] bool create(const vulkan_init_info& info, const font_atlas& atlas);
    void destroy() noexcept;

    // records staged texture uploads, UI draws, and any backdrop blur passes; leaves target in target.layout.
    // without target.image, backdrop panels draw as flat tints.
    void render(const draw_data& data, VkCommandBuffer cmd, u32 frame_index, const vulkan_target& target);

    // drops cached VkFramebuffers for swapchain views; call after idling the device on swapchain recreate.
    void clear_target_cache() noexcept;

    // colour encoding of the target (see output_space); applies next render()
    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;

    // true once any Vulkan call returned VK_ERROR_DEVICE_LOST
    [[nodiscard]] bool device_lost() const noexcept;

    // uploads a rebuilt font atlas (blocking queue submit; wait on in-flight frames first). then release_font_pixels().
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);
    // stages a dirty sub-rectangle copy of a dynamic font atlas (same dimensions), recorded on the next render()
    [[nodiscard]] bool update_atlas_region(const font_atlas& atlas, atlas_region region);

    // uploads a straight-alpha rgba8 image (tightly packed) and returns its id; 0 on failure or no free slot. staged,
    // copied at the start of the next render() before the UI render pass.
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // general form: pixel formats, mips, updatable textures (see texture_desc); staged the same way
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // updates a rectangle of an `updatable` texture and its mips, staged; false if not updatable or out of bounds
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // wraps a host VkImageView (kept in SHADER_READ_ONLY_OPTIMAL during render()). caller retains ownership.
    [[nodiscard]] texture_id register_texture(VkImageView view, VkSampler sampler = VK_NULL_HANDLE);
    // safe to call any time: released (slot reused) once frames in flight are done
    void destroy_texture(texture_id id) noexcept;
    static constexpr u32 max_textures = 255;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

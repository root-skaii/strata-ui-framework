#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <memory>
#include <span>

namespace strata {

struct metal_init_info {
    void* device{};            // id<MTLDevice> (bridged)
    u32   pixel_format = 80;   // MTLPixelFormatBGRA8Unorm = 80, RGBA8Unorm = 70, RGBA16Float = 115
    u32   sample_count = 1;
};

// renders draw_data on Apple Metal (macOS / iOS). Pure C++ header using opaque Objective-C handles so callers do not
// require Objective-C++ outside their platform bridge.
class metal_renderer {
public:
    metal_renderer() noexcept;
    ~metal_renderer();

    metal_renderer(const metal_renderer&)            = delete;
    metal_renderer& operator=(const metal_renderer&) = delete;
    metal_renderer(metal_renderer&&) noexcept;
    metal_renderer& operator=(metal_renderer&&) noexcept;

    [[nodiscard]] bool create(const metal_init_info& info, const font_atlas& atlas);
    void destroy() noexcept;

    // records UI draws into an active id<MTLRenderCommandEncoder>
    void render(const draw_data& data, void* render_encoder);

    [[nodiscard]] bool update_atlas(const font_atlas& atlas);
    [[nodiscard]] bool update_atlas_region(const font_atlas& atlas, atlas_region region);

    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;
    [[nodiscard]] bool device_lost() const noexcept { return false; }

    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // wraps a host id<MTLTexture>; caller retains ownership
    [[nodiscard]] texture_id register_texture(void* mtl_texture);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

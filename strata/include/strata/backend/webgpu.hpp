#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <webgpu/webgpu.h>

#include <memory>
#include <span>

namespace strata {

struct wgpu_init_info {
    WGPUDevice        device{};
    WGPUQueue         queue{};
    WGPUTextureFormat color_format = WGPUTextureFormat_BGRA8Unorm;
    u32               sample_count = 1;
};

// renders draw_data on a WebGPU device (Dawn, wgpu-native, or Emscripten/WASM).
// satisfies renderer_backend; embedded WGSL shaders require no runtime shader compiler toolchain.
class wgpu_renderer {
public:
    wgpu_renderer() noexcept;
    ~wgpu_renderer();

    wgpu_renderer(const wgpu_renderer&)            = delete;
    wgpu_renderer& operator=(const wgpu_renderer&) = delete;
    wgpu_renderer(wgpu_renderer&&) noexcept;
    wgpu_renderer& operator=(wgpu_renderer&&) noexcept;

    [[nodiscard]] bool create(const wgpu_init_info& info, const font_atlas& atlas);
    void destroy() noexcept;

    // records UI draw commands into an already-open render pass (backdrop panels draw as flat tints).
    void render(const draw_data& data, WGPURenderPassEncoder pass);

    // records UI draw commands into `encoder` targeting `target_view`. when `target_tex` is supplied (single-sample
    // with CopySrc usage), backdrop blur passes execute before frosted panels.
    void render(const draw_data& data, WGPUCommandEncoder encoder, WGPUTextureView target_view,
                WGPUTexture target_tex = nullptr, bool clear = false, color clear_col = {0, 0, 0, 255});

    [[nodiscard]] bool update_atlas(const font_atlas& atlas);
    [[nodiscard]] bool update_atlas_region(const font_atlas& atlas, atlas_region region);

    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;
    [[nodiscard]] bool device_lost() const noexcept;

    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // wraps a host WGPUTextureView for ui.image(); caller retains ownership
    [[nodiscard]] texture_id register_texture(WGPUTextureView view);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

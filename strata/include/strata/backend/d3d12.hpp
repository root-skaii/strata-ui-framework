#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <cstdint>
#include <memory>
#include <span>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace strata {

struct d3d12_init_info {
    ID3D12Device*       device{};
    ID3D12CommandQueue* queue{};            // used once, to upload the font atlas
    u32                 frames_in_flight = 2;
    u32                 rtv_format       = 0; // DXGI_FORMAT of the render target
};

// the render target the ui draws into; only needed for backdrop blur (acrylic panels). `resource` must be single-sample,
// in the RENDER_TARGET state while render() records, and have the format given as rtv_format. `rtv` is the cpu descriptor
// handle (D3D12_CPU_DESCRIPTOR_HANDLE::ptr), rebound after the blur passes.
struct d3d12_target {
    ID3D12Resource* resource{};
    std::uintptr_t  rtv{};
};

// records draw_data into a command list that is already recording with the render target bound. upload buffers are
// indexed by frame_index, so the host must have waited on that frame's fence first (the usual frames-in-flight contract).
class d3d12_renderer {
public:
    d3d12_renderer() noexcept;
    ~d3d12_renderer();

    d3d12_renderer(const d3d12_renderer&)            = delete;
    d3d12_renderer& operator=(const d3d12_renderer&) = delete;
    d3d12_renderer(d3d12_renderer&&) noexcept;
    d3d12_renderer& operator=(d3d12_renderer&&) noexcept;

    [[nodiscard]] bool create(const d3d12_init_info& info, const font_atlas& atlas);
    void destroy() noexcept;

    // sets its own pso, root signature, descriptor heap, viewport, scissor and ia state.
    // without `target` backdrop panels are drawn as flat tints
    void render(const draw_data& data, ID3D12GraphicsCommandList* list, u32 frame_index, const d3d12_target* target = nullptr);

    // uploads a new font atlas (after context::set_scale rebuilt it). the host must have waited for the gpu to finish
    // every frame that used the old one. call context::release_font_pixels() afterwards.
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);

    // uploads a straight-alpha rgba8 image (rows tightly packed) and returns the id ui.image() takes; 0 on failure or when
    // all max_textures slots are taken. the upload blocks on the queue given to create().
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // the general form: other pixel formats, mip maps and updatable textures (see texture_desc); same blocking upload
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // replaces a rectangle of an `updatable` texture and refreshes its mips, with a blocking copy on the queue (it runs
    // after frames already submitted, so frames in flight are not disturbed). false if the id is not updatable or the
    // rectangle does not fit
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // the host must make sure the gpu no longer uses the texture (wait for the frames in flight) before calling this
    void destroy_texture(texture_id id) noexcept;
    static constexpr u32 max_textures = 255;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

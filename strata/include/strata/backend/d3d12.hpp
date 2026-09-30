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
struct D3D12_SHADER_RESOURCE_VIEW_DESC;

namespace strata {

struct d3d12_init_info {
    ID3D12Device*       device{};
    ID3D12CommandQueue* queue{};            // used once, to upload the font atlas
    u32                 frames_in_flight = 2;
    u32                 rtv_format       = 0; // DXGI_FORMAT of the render target
};

// the render target, needed only for backdrop blur. single-sample, RENDER_TARGET state during render(), format =
// rtv_format. `rtv` = cpu descriptor handle ptr, rebound after the blur passes.
struct d3d12_target {
    ID3D12Resource* resource{};
    std::uintptr_t  rtv{};
};

// records into an open command list with the target bound. upload buffers are per frame_index, so the host must
// have waited on that frame's fence.
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
    // without `target` backdrops are flat tints
    void render(const draw_data& data, ID3D12GraphicsCommandList* list, u32 frame_index, const d3d12_target* target = nullptr);

    // colour encoding of the target (see output_space); wrong values look washed out or garish. applies next render()
    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;

    // device lost (driver update / crash, removal, TDR). recovery is the host's job, in order: new device and swap
    // chain, ui.rebuild_font_atlas(), create() this renderer from ui.font(), ui.release_font_pixels(), recreate
    // textures (old ids are invalid), ui.invalidate()
    [[nodiscard]] bool device_lost() const noexcept;

    // uploads a rebuilt font atlas; the gpu must be done with every frame using the old one. then release_font_pixels().
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);

    // uploads a straight-alpha rgba8 image (tightly packed) and returns its id; 0 on failure or no free slot. staged,
    // copied at the start of the next render() before the draws.
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // general form: pixel formats, mips, updatable textures (see texture_desc); staged the same way
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // updates a rectangle of an `updatable` texture and its mips, staged: cheap per frame; frames in flight keep the old
    // content. false if not updatable or out of bounds
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // shows a resource the host already has (a render target, a video frame ...) through ui.image(), sampled like
    // create_texture's straight-alpha rgba. `view` null: the resource's default view. the host keeps it in a
    // pixel-shader-readable state while render() records. keeps its own reference until destroy_texture()
    [[nodiscard]] texture_id register_texture(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* view = nullptr);
    // any time: released (slot reused) once frames in flight are done
    void destroy_texture(texture_id id) noexcept;
    static constexpr u32 max_textures = 255;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

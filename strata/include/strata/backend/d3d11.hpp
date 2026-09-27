#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <memory>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace strata {

// renders draw_data on an existing d3d11 device; the caller owns device, swap chain and target binding.
// backdrop blur copies the bound target, which must be a plain single-sample texture (otherwise flat tints).
class d3d11_renderer {
public:
    d3d11_renderer() noexcept;
    ~d3d11_renderer();

    d3d11_renderer(const d3d11_renderer&)            = delete;
    d3d11_renderer& operator=(const d3d11_renderer&) = delete;
    d3d11_renderer(d3d11_renderer&&) noexcept;
    d3d11_renderer& operator=(d3d11_renderer&&) noexcept;

    [[nodiscard]] bool create(ID3D11Device* device, ID3D11DeviceContext* context, const font_atlas& atlas);
    void destroy() noexcept;

    // uploads a rebuilt font atlas; then call context::release_font_pixels()
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);

    void render(const draw_data& data);

    // render() saves and restores ~18 pipeline stages (~40 driver calls), which overlays need and owned devices do not.
    // off: strata's state is left bound; set shaders, layout, buffers, blend, scissor and viewport before drawing next.
    void set_state_restore(bool on) noexcept;
    [[nodiscard]] bool state_restore() const noexcept;

    // colour encoding of the target (see output_space); wrong values look washed out or garish. applies next render()
    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;

    // device lost (driver update / crash, removal, TDR). recovery is the host's job, in order: new device and swap
    // chain, ui.rebuild_font_atlas(), create() this renderer from ui.font(), ui.release_font_pixels(), recreate
    // textures (old ids are invalid), ui.invalidate()
    [[nodiscard]] bool device_lost() const noexcept;

    // uploads a straight-alpha rgba8 image (tightly packed); returns the id for ui.image() / draw_list::image(), 0 on
    // failure. lives until destroy_texture() or destroy().
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // general form: bgra8, r8, a8, rgba16f, mips, updatable (see texture_desc). `pixels` in desc.format, tightly packed.
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // updates a rectangle of an `updatable` texture (its format, tightly packed) and its mips; false if not updatable
    // or out of bounds
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

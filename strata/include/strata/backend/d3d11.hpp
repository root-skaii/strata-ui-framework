#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <memory>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace strata {

// renders draw_data with an existing d3d11 device. the caller owns the device, swap chain and render target binding;
// render() saves and restores every pipeline stage it touches.
// backdrop blur (acrylic panels) copies the bound render target to a texture of its own, so the target must be a plain
// single-sample texture; otherwise those panels are drawn as flat tints.
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

    // uploads a new font atlas (after context::set_scale rebuilt it); call context::release_font_pixels() afterwards
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);

    void render(const draw_data& data);

    // render() snapshots and restores roughly eighteen pipeline stages, which an in-game overlay has to do and an
    // application that owns its device does not -- it is about forty driver calls per frame spent putting back state
    // nobody is going to read. turn it off when strata's own state is a fine thing to leave behind; the next thing
    // you draw then has to set what it needs (shaders, input layout, buffers, blend, scissor, viewport).
    void set_state_restore(bool on) noexcept;
    [[nodiscard]] bool state_restore() const noexcept;

    // how colours are written into the target (see output_space): an srgb view, an scRGB or an HDR10 swap chain needs it
    // said, otherwise the ui comes out washed out, too dark or garish. applies from the next render()
    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;

    // the device is gone (a driver update or crash, a gpu that was removed, TDR): Present / Map fail from now on and
    // everything made on it is dead. recovering is the host's job, in this order: make a new device (and swap chain),
    // ui.rebuild_font_atlas(), create() this renderer on the new device from ui.font(), ui.release_font_pixels(), then
    // create the textures again (their old ids mean nothing any more) and ui.invalidate()
    [[nodiscard]] bool device_lost() const noexcept;

    // uploads a straight-alpha rgba8 image (width * height * 4 bytes, rows tightly packed) and returns the id that
    // ui.image() / draw_list::image() take; 0 if it failed. the texture lives until destroy_texture() or destroy().
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // the general form: other pixel formats (bgra8, r8, a8, rgba16f), mip maps and textures that can be updated later
    // (see texture_desc). `pixels` are in desc.format, rows tightly packed.
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // replaces a rectangle of an `updatable` texture (pixels in the texture's format, rows tightly packed) and refreshes
    // its mip levels; false if the id is not an updatable texture or the rectangle does not fit
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

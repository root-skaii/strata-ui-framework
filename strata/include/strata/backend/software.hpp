#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <memory>
#include <span>

namespace strata {

// rasterises draw_data on the cpu into an rgba8 image: no gpu, window or graphics api. for headless tests and
// screenshots (output does not depend on a driver or font rasteriser beyond the baked atlas), servers and CI.
// follows the shader's maths (shapes, gradients, shadows, images, mips, backdrop blur) but is not bit-identical to the
// gpu renderers; compare against goldens made by this renderer. pixels are always plain srgb: output_space is stored
// for the host's bookkeeping and otherwise ignored.
//
//   strata::software_renderer r;
//   r.create(ui.font());
//   r.resize(1280, 720);
//   r.clear({20, 20, 24, 255});
//   r.render(ui.render_data());
//   r.pixels();   // rgba8, width() * height() * 4 bytes
class software_renderer {
public:
    software_renderer() noexcept;
    ~software_renderer();

    software_renderer(const software_renderer&)            = delete;
    software_renderer& operator=(const software_renderer&) = delete;
    software_renderer(software_renderer&&) noexcept;
    software_renderer& operator=(software_renderer&&) noexcept;

    [[nodiscard]] bool create(const font_atlas& atlas);
    void destroy() noexcept;

    // copies a rebuilt font atlas; then call context::release_font_pixels()
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);

    // sizes the target, cleared to transparent. render() does this itself when draw_data::display_size differs
    void resize(u32 width, u32 height);
    // fills the target (sized by resize() or an earlier render()); render() draws over what is there
    void clear(color c = {0, 0, 0, 0});

    void render(const draw_data& data);

    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;
    [[nodiscard]] bool device_lost() const noexcept { return false; }

    // same contract as the gpu renderers; ids are small integers, 0 = failed
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

    [[nodiscard]] u32 width() const noexcept;
    [[nodiscard]] u32 height() const noexcept;
    // the target, premultiplied rgba8, rows tightly packed, top row first. equals straight alpha where alpha is 255
    [[nodiscard]] std::span<const u8> pixels() const noexcept;
    // the target as straight-alpha rgba8; `out` needs width() * height() * 4 bytes. false if too small
    [[nodiscard]] bool copy_straight(std::span<u8> out) const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

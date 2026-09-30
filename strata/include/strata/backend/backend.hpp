#pragma once

// what a backend has to provide, as C++ concepts: checked at compile time, no virtual calls in the frame loop.
// a new backend (vulkan, opengl, sdl, glfw ...) is a class that satisfies one of these; generic code such as
// strata::frame takes `renderer_backend auto&` / `platform_backend auto&` and never names a graphics api.
//
//   platform  feeds the ui: input, dpi, cursor, clipboard, IME        win32_platform
//   renderer  draws draw_data and owns the gpu textures               d3d11_renderer, d3d12_renderer
//
// the one thing renderers do NOT share is setup and the draw call itself, because they take the api's own objects
// (device + context for d3d11; device, queue and a command list for d3d12). everything a host does between those two
// is the same on every renderer, and is what renderer_backend describes.

#include "strata/context.hpp"
#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <concepts>
#include <span>

namespace strata {

template <class R>
concept renderer_backend = requires(R& r, const R& cr, const font_atlas& atlas, const output_desc& out, u32 n, std::span<const u8> px,
                                    const texture_desc& desc, texture_id id) {
    { r.destroy() } noexcept;
    // uploads a rebuilt font atlas (context::rebuild_font_atlas), then the host calls context::release_font_pixels()
    { r.update_atlas(atlas) } -> std::same_as<bool>;
    // colour encoding of the target; applies to the next draw call
    { r.set_output(out) } noexcept;
    { cr.output() } noexcept -> std::same_as<output_desc>;
    // the device was lost: recreate everything (see the device loss notes on the concrete renderer)
    { cr.device_lost() } noexcept -> std::same_as<bool>;
    // images for ui.image(): straight-alpha rgba8, or the general form. 0 = failed
    { r.create_texture(n, n, px) } -> std::same_as<texture_id>;
    { r.create_texture(desc, px) } -> std::same_as<texture_id>;
    { r.update_texture(id, n, n, n, n, px) } -> std::same_as<bool>;
    { r.destroy_texture(id) } noexcept;
    { cr.valid() } noexcept -> std::same_as<bool>;
};

template <class P>
concept platform_backend = requires(P& p, const P& cp, cursor_kind k, vec2 v, f32 f) {
    // the input snapshot for context::begin_frame
    { p.new_frame() } -> std::same_as<input_state>;
    // 1.0 = 96 dpi; feed context::set_scale
    { cp.dpi_scale() } noexcept -> std::same_as<f32>;
    // pass ui.cursor() after end_frame
    { p.set_cursor(k) } noexcept;
    // ui.set_clipboard(platform.clipboard())
    { p.clipboard() } noexcept -> std::same_as<clipboard_hooks>;
    // pass ui.ime_wanted(), ui.ime_position(), ui.ime_line_height() after end_frame
    { p.set_ime(true, v, f) } noexcept;
};

} // namespace strata

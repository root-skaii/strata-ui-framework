#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <memory>
#include <span>

namespace strata {

// renders draw_data on a current OpenGL 3.3+ core context; the caller owns context, window and swap buffers.
// backdrop blur blits the currently bound draw framebuffer (must be single-sample; otherwise flat tints).
class gl_renderer {
public:
    using proc_loader = void* (*)(const char* name);

    gl_renderer() noexcept;
    ~gl_renderer();

    gl_renderer(const gl_renderer&)            = delete;
    gl_renderer& operator=(const gl_renderer&) = delete;
    gl_renderer(gl_renderer&&) noexcept;
    gl_renderer& operator=(gl_renderer&&) noexcept;

    // `loader` resolves GL entry points (e.g. glfwGetProcAddress / SDL_GL_GetProcAddress).
    // null on Windows uses wglGetProcAddress with fallback to opengl32.dll.
    [[nodiscard]] bool create(const font_atlas& atlas, proc_loader loader = nullptr);
    void destroy() noexcept;

    // uploads a rebuilt font atlas; then call context::release_font_pixels()
    [[nodiscard]] bool update_atlas(const font_atlas& atlas);
    // uploads a dirty sub-rectangle of a dynamic font atlas (same dimensions)
    [[nodiscard]] bool update_atlas_region(const font_atlas& atlas, atlas_region region);

    void render(const draw_data& data);

    // render() saves and restores the GL state it modifies (overlays need this; dedicated contexts can disable it).
    void set_state_restore(bool on) noexcept;
    [[nodiscard]] bool state_restore() const noexcept;

    // colour encoding of the target (see output_space); applies next render()
    void set_output(const output_desc& output) noexcept;
    [[nodiscard]] output_desc output() const noexcept;

    // reports GL_ARB_robustness / GL 4.5 context reset when available; false otherwise
    [[nodiscard]] bool device_lost() const noexcept;

    // uploads a straight-alpha rgba8 image (tightly packed); returns the id for ui.image() / draw_list::image(), 0 on failure.
    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba);
    // general form: bgra8, r8, a8, rgba16f, mips, updatable (see texture_desc). `pixels` in desc.format, tightly packed.
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels);
    // updates a rectangle of an `updatable` texture and its mips; false if not updatable or out of bounds
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels);
    // wraps an existing GL_TEXTURE_2D object name for ui.image(); caller retains ownership (not deleted on destroy_texture)
    [[nodiscard]] texture_id register_texture(u32 gl_texture);
    void destroy_texture(texture_id id) noexcept;

    [[nodiscard]] bool valid() const noexcept { return impl_ != nullptr; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace strata

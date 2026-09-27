#pragma once

#include <strata/strata.hpp>

#include <memory>
#include <span>
#include <vector>

struct HWND__;

// minimal device + swap chain owner per api (host code: strata never creates devices).
class gfx_host {
public:
    virtual ~gfx_host() = default;

    [[nodiscard]] virtual bool init(HWND__* hwnd, strata::u32 width, strata::u32 height, bool vsync,
                                    const strata::font_atlas& atlas) = 0;
    virtual void resize(strata::u32 width, strata::u32 height) = 0;
    // clears the backbuffer, draws the ui, presents
    virtual void render(const strata::draw_data& ui, strata::color clear) = 0;
    // uploads a straight-alpha rgba8 image for ui.image(); 0 when it failed
    [[nodiscard]] virtual strata::texture_id create_texture(strata::u32 width, strata::u32 height,
                                                            std::span<const strata::u8> rgba) = 0;
    // any format / mips / updatable (see strata::texture_desc), and rectangle updates
    [[nodiscard]] virtual strata::texture_id create_texture(const strata::texture_desc& desc, std::span<const strata::u8> pixels) = 0;
    virtual bool update_texture(strata::texture_id id, strata::u32 x, strata::u32 y, strata::u32 width, strata::u32 height,
                                std::span<const strata::u8> pixels) = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // uploads a rebuilt font atlas (after ui.set_scale); the host idles the gpu first
    [[nodiscard]] virtual bool update_atlas(const strata::font_atlas& atlas) = 0;
    // screenshots: the next render() also copies the finished frame before present; take_capture() returns it as
    // rgba8 top-down, false if nothing was captured
    virtual void request_capture() = 0;
    [[nodiscard]] virtual bool take_capture(std::vector<strata::u8>& rgba, strata::u32& width, strata::u32& height) = 0;
};

#if STRATA_HAS_DX11
[[nodiscard]] std::unique_ptr<gfx_host> make_d3d11_host();
#endif
#if STRATA_HAS_DX12
[[nodiscard]] std::unique_ptr<gfx_host> make_d3d12_host();
#endif

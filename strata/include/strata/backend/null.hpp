#pragma once

#include "strata/draw_list.hpp"
#include "strata/font.hpp"
#include "strata/texture.hpp"

#include <unordered_map>

namespace strata {

// a renderer that draws nothing. the ui runs a full frame; the renderer checks the draw data is well formed and counts
// it. for tests, fuzzing and hosts that only need the ui's state (hit testing, layout, input), with no gpu.
class null_renderer {
public:
    struct stats {
        u64  frames{};
        u32  commands{};   // of the last render()
        u32  vertices{};
        u32  indices{};
        u32  shapes{};
        u32  textured{};   // commands carrying a user texture
        u32  backdrops{};  // commands with a blur
        bool well_formed{true}; // the last render(): indices, commands and texture ids all in range
    };

    [[nodiscard]] bool create(const font_atlas&) noexcept { valid_ = true; return true; }
    void destroy() noexcept { valid_ = false; textures_.clear(); }
    [[nodiscard]] bool update_atlas(const font_atlas&) noexcept { return valid_; }

    void render(const draw_data& d);

    void set_output(const output_desc& o) noexcept { output_ = o; }
    [[nodiscard]] output_desc output() const noexcept { return output_; }
    [[nodiscard]] bool device_lost() const noexcept { return false; }

    [[nodiscard]] texture_id create_texture(u32 width, u32 height, std::span<const u8> rgba)
    {
        return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
    }
    [[nodiscard]] texture_id create_texture(const texture_desc& desc, std::span<const u8> pixels)
    {
        if (!valid_ || desc.width == 0 || desc.height == 0 ||
            pixels.size() < static_cast<std::size_t>(desc.width) * desc.height * texture_source_bytes(desc.format)) {
            return 0;
        }
        textures_[next_] = {desc.width, desc.height, desc.format, desc.updatable};
        return next_++;
    }
    [[nodiscard]] bool update_texture(texture_id id, u32 x, u32 y, u32 w, u32 h, std::span<const u8> pixels)
    {
        const auto it = textures_.find(id);
        if (it == textures_.end() || !it->second.updatable || w == 0 || h == 0 || x > it->second.w || y > it->second.h ||
            w > it->second.w - x || h > it->second.h - y) {
            return false;
        }
        return pixels.size() >= static_cast<std::size_t>(w) * h * texture_source_bytes(it->second.format);
    }
    void destroy_texture(texture_id id) noexcept { textures_.erase(id); }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::size_t texture_count() const noexcept { return textures_.size(); }
    [[nodiscard]] const stats& last() const noexcept { return stats_; }

private:
    struct tex {
        u32            w{};
        u32            h{};
        texture_format format{};
        bool           updatable{};
    };

    bool                                  valid_{};
    output_desc                           output_{};
    texture_id                            next_{1};
    std::unordered_map<texture_id, tex>   textures_;
    stats                                 stats_;
};

inline void null_renderer::render(const draw_data& d)
{
    stats s;
    s.frames   = stats_.frames + 1;
    s.commands = static_cast<u32>(d.commands.size());
    s.vertices = static_cast<u32>(d.vertices.size());
    s.indices  = static_cast<u32>(d.indices.size());
    s.shapes   = static_cast<u32>(d.shapes.size());

    for (const draw_cmd& c : d.commands) {
        if (c.texture != 0) {
            ++s.textured;
            s.well_formed = s.well_formed && textures_.contains(c.texture);
        }
        if (c.blur > 0.0f) { ++s.backdrops; }
        if (c.idx_count % 3 != 0 || static_cast<std::size_t>(c.idx_offset) + c.idx_count > d.indices.size() ||
            c.vtx_offset > d.vertices.size()) {
            s.well_formed = false;
            continue;
        }
        const u32 span = static_cast<u32>(d.vertices.size()) - c.vtx_offset;
        for (u32 i = 0; i < c.idx_count; ++i) {
            s.well_formed = s.well_formed && d.indices[c.idx_offset + i] < span;
        }
    }
    stats_ = s;
}

} // namespace strata

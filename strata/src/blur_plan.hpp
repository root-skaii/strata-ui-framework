#pragma once

// internal (not installed): what the d3d11 and d3d12 renderers share about a backdrop blur -- how far the frame is scaled
// down, the gaussian's weights, and which part of the frame has to be copied and blurred at all.
//
// the frame is copied into a snapshot, boxed down by `ds` into a low-resolution target and blurred there by a separable
// gaussian, `iterations` times horizontally and vertically. only the panels that show it need it: everything is limited to
// the panel's rectangle grown by how far the passes reach (a full-screen copy and blur per panel used to be the cost of
// every acrylic window, menu and tooltip).

#include "strata/draw_list.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace strata::internal {

struct blur_plan {
    u32 ds{2};            // downsample factor
    int iterations{1};    // horizontal + vertical pass pairs
    // the gaussian of one pass as 7 bilinear taps: the centre texel, then 3 pairs of neighbours (each pair is two texels
    // read by one filtered tap between them), on both sides. weights sum to 1
    f32 w0{1.0f};
    f32 w[3]{};
    f32 o[3]{};
    // the low-resolution texels every pass writes (half-open), and the part of the target that feeds them
    u32 lx0{}, ly0{}, lx1{}, ly1{};
    u32 x0{}, y0{}, x1{}, y1{};
    // where in the target the result is exact: a later panel inside it can reuse this blur
    rect exact;
};

// the rectangle (physical pixels) a backdrop command draws: the bounds of its vertices, inside its clip
[[nodiscard]] inline rect backdrop_bounds(const draw_data& data, const draw_cmd& cmd) noexcept
{
    u32 top = 0;
    for (u32 i = 0; i < cmd.idx_count; ++i) { top = std::max<u32>(top, data.indices[cmd.idx_offset + i]); }
    rect b{{1.0e9f, 1.0e9f}, {-1.0e9f, -1.0e9f}};
    for (u32 v = 0; v <= top && cmd.vtx_offset + v < data.vertices.size(); ++v) {
        const vec2 p = data.vertices[cmd.vtx_offset + v].pos;
        b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y)};
        b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y)};
    }
    return b.intersect(cmd.clip);
}

[[nodiscard]] inline blur_plan plan_blur(f32 radius_px, const rect& panel, u32 target_w, u32 target_h) noexcept
{
    blur_plan p;
    p.ds = radius_px <= 8.0f ? 2u : 4u;
    const f32 sigma_total = std::max(0.6f, radius_px / (2.0f * static_cast<f32>(p.ds)));
    p.iterations          = sigma_total > 3.5f ? (sigma_total > 8.0f ? 3 : 2) : 1;
    const f32 sigma       = sigma_total / std::sqrt(static_cast<f32>(p.iterations));

    // 13 taps (-6 .. 6), as before; the pairs (1, 2), (3, 4), (5, 6) each become one filtered read
    f32 g[7];
    f32 sum = 0.0f;
    for (int k = 0; k <= 6; ++k) {
        g[k] = std::exp(-0.5f * static_cast<f32>(k * k) / std::max(sigma * sigma, 1.0e-4f));
        sum += k == 0 ? g[k] : 2.0f * g[k];
    }
    p.w0 = g[0] / sum;
    for (int i = 0; i < 3; ++i) {
        const int a = 2 * i + 1, b = a + 1;
        const f32 wp = g[a] + g[b];
        p.w[i] = wp / sum;
        p.o[i] = wp > 0.0f ? (static_cast<f32>(a) * g[a] + static_cast<f32>(b) * g[b]) / wp : static_cast<f32>(a);
    }

    // the passes read up to 6 texels on each side, `iterations` times per axis: a margin that wide (and a texel for the
    // backdrop's own filtered read) keeps what the panel samples free of the stale texels outside the region
    const u32  vw     = std::max(1u, (target_w + p.ds - 1) / p.ds);
    const u32  vh     = std::max(1u, (target_h + p.ds - 1) / p.ds);
    const auto margin = static_cast<f32>(6 * p.iterations + 2);
    const f32  inv    = 1.0f / static_cast<f32>(p.ds);
    const auto clamp_to = [](f32 v, u32 hi) { return static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(hi))); };
    p.lx0 = clamp_to(std::floor(panel.min.x * inv - margin), vw);
    p.ly0 = clamp_to(std::floor(panel.min.y * inv - margin), vh);
    p.lx1 = clamp_to(std::ceil(panel.max.x * inv + margin), vw);
    p.ly1 = clamp_to(std::ceil(panel.max.y * inv + margin), vh);
    // the box filter reads a texel's own block and half a target pixel around it
    p.x0 = std::min(target_w, p.lx0 * p.ds > p.ds ? p.lx0 * p.ds - p.ds : 0u);
    p.y0 = std::min(target_h, p.ly0 * p.ds > p.ds ? p.ly0 * p.ds - p.ds : 0u);
    p.x1 = std::min(target_w, p.lx1 * p.ds + p.ds);
    p.y1 = std::min(target_h, p.ly1 * p.ds + p.ds);
    // exact where the margin is whole: inside the region shrunk by it again (at the target's own edges the clamp to the
    // edge is what the full-frame blur did too, so there nothing needs to be shrunk)
    const auto inner = [&](u32 lo, u32 hi, u32 full, f32 m, bool low_side) {
        if (low_side) { return lo == 0 ? 0.0f : (static_cast<f32>(lo) + m) * static_cast<f32>(p.ds); }
        return hi >= full ? 1.0e9f : (static_cast<f32>(hi) - m) * static_cast<f32>(p.ds);
    };
    p.exact = {{inner(p.lx0, p.lx1, vw, margin, true), inner(p.ly0, p.ly1, vh, margin, true)},
               {inner(p.lx0, p.lx1, vw, margin, false), inner(p.ly0, p.ly1, vh, margin, false)}};
    return p;
}

// b0 of the ui shader: the transform to clip space, then the output encoding (see ui.hlsl)
struct ui_constants {
    float transform[4]{};
    float output[4]{};
};

[[nodiscard]] inline ui_constants make_ui_constants(const draw_data& data, const output_desc& out) noexcept
{
    ui_constants c;
    c.transform[0] = 2.0f / data.display_size.x;
    c.transform[1] = -2.0f / data.display_size.y;
    c.transform[2] = -1.0f;
    c.transform[3] = 1.0f;
    const f32 nits = std::clamp(out.paper_white_nits, 1.0f, 10000.0f);
    c.output[0] = static_cast<float>(out.space);
    c.output[1] = out.space == output_space::hdr10 ? nits / 10000.0f : nits / 80.0f;
    c.output[2] = std::max(data.text_contrast, 0.0f);
    return c;
}

// the blur's own low-resolution targets: 8 bits per channel for an 8-bit target, half floats for anything wider (an
// scRGB target goes past 1, and 8 bits of PQ would band)
[[nodiscard]] inline bool wide_format(int dxgi_format) noexcept
{
    switch (dxgi_format) {
    case 27: case 28: case 29:  // DXGI_FORMAT_R8G8B8A8_TYPELESS / _UNORM / _UNORM_SRGB
    case 87: case 88: case 90: case 91: case 92: case 93: // DXGI_FORMAT_B8G8R8A8 / B8G8R8X8 (typeless, unorm, srgb)
        return false;
    default:
        return true;
    }
}

[[nodiscard]] inline bool rect_inside(const rect& inner, const rect& outer) noexcept
{
    return inner.min.x >= outer.min.x && inner.min.y >= outer.min.y && inner.max.x <= outer.max.x && inner.max.y <= outer.max.y;
}

} // namespace strata::internal

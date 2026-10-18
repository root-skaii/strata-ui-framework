// cpu renderer: a port of strata/shaders/ui.hlsl over a triangle rasteriser (see backend/software.hpp)

#include "strata/backend/software.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace strata {

namespace {

constexpr f32 inv255 = 1.0f / 255.0f;

struct vec4 {
    f32 r{}, g{}, b{}, a{};
};

[[nodiscard]] vec4 unpack(color c) noexcept
{
    return {static_cast<f32>(c.r) * inv255, static_cast<f32>(c.g) * inv255, static_cast<f32>(c.b) * inv255,
            static_cast<f32>(c.a) * inv255};
}

[[nodiscard]] f32 saturate(f32 v) noexcept { return std::clamp(v, 0.0f, 1.0f); }
[[nodiscard]] f32 mix(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }
[[nodiscard]] vec4 mix(const vec4& a, const vec4& b, f32 t) noexcept
{
    return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t), mix(a.a, b.a, t)};
}
[[nodiscard]] f32 luma(f32 r, f32 g, f32 b) noexcept { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

[[nodiscard]] f32 smoothstep(f32 lo, f32 hi, f32 x) noexcept
{
    const f32 t = saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}

// signed distance (pixels) to a box with per-corner radii; y down
[[nodiscard]] f32 sd_round_box(vec2 p, vec2 half_size, const std::array<f32, 4>& r) noexcept
{
    const f32 rr = p.x > 0.0f ? (p.y > 0.0f ? r[2] : r[1]) : (p.y > 0.0f ? r[3] : r[0]);
    const f32 qx = std::abs(p.x) - half_size.x + rr;
    const f32 qy = std::abs(p.y) - half_size.y + rr;
    return std::min(std::max(qx, qy), 0.0f) + std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) - rr;
}

[[nodiscard]] vec4 shade_shape(const shape_record& s, vec2 pixel) noexcept
{
    const vec2 p     = pixel - s.center;
    const f32  d     = sd_round_box(p, s.half_size, s.radius);
    const f32  outer = saturate(0.5f - d);
    const f32  inner = saturate(0.5f - (d + s.border_width));

    const f32 extent = std::max(std::abs(s.gradient_dir.x) * 2.0f * s.half_size.x +
                                    std::abs(s.gradient_dir.y) * 2.0f * s.half_size.y, 1.0e-3f);
    const f32 t = s.gradient_kind > 0.5f
                      ? saturate(std::hypot(p.x / std::max(s.half_size.x, 1.0e-3f), p.y / std::max(s.half_size.y, 1.0e-3f)))
                      : saturate(dot(p, s.gradient_dir) / extent + 0.5f);
    const vec4 fill = mix(unpack(s.fill_top), unpack(s.fill_bottom), t);
    const vec4 edge = unpack(s.border);
    const f32  ring = std::max(outer - inner, 0.0f);

    vec4 out{fill.r * fill.a * inner + edge.r * edge.a * ring, fill.g * fill.a * inner + edge.g * edge.a * ring,
             fill.b * fill.a * inner + edge.b * edge.a * ring, fill.a * inner + edge.a * ring};

    const vec4 sh = unpack(s.shadow);
    if (sh.a > 0.0f && s.shadow_blur > 0.0f) {
        const f32 ds = sd_round_box(p - s.shadow_offset, s.half_size, s.radius);
        const f32 k  = 1.0f - smoothstep(-s.shadow_blur, s.shadow_blur, ds);
        const f32 sa = sh.a * k * (1.0f - outer); // never under the shape itself
        const f32 rest = 1.0f - out.a;
        out.r += sh.r * sa * rest;
        out.g += sh.g * sa * rest;
        out.b += sh.b * sa * rest;
        out.a += sa * rest;
    }
    return out;
}

[[nodiscard]] f32 texel_channel(const u8* p, texture_layout l, int c) noexcept
{
    if (l == texture_layout::rgba16f) {
        return std::clamp(half_to_float(reinterpret_cast<const u16*>(p)[c]), 0.0f, 1.0f);
    }
    if (l == texture_layout::bgra8 && c < 3) { c = 2 - c; }
    return static_cast<f32>(p[c]) * inv255;
}

// one mip level, bilinear, clamped
[[nodiscard]] vec4 sample_level(const texture_image& t, u32 level, vec2 uv) noexcept
{
    const u32 w = t.width(level);
    const u32 h = t.height(level);
    const f32 fx = uv.x * static_cast<f32>(w) - 0.5f;
    const f32 fy = uv.y * static_cast<f32>(h) - 0.5f;
    const f32 x0f = std::floor(fx);
    const f32 y0f = std::floor(fy);
    const f32 tx = fx - x0f;
    const f32 ty = fy - y0f;
    const auto cx = [&](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(w - 1))); };
    const auto cy = [&](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, static_cast<f32>(h - 1))); };
    const u32 xs[2] = {cx(x0f), cx(x0f + 1.0f)};
    const u32 ys[2] = {cy(y0f), cy(y0f + 1.0f)};

    const texture_layout l = t.layout();
    const auto  px = t.pixels(level);
    const u32   bpp = texture_layout_bytes(l);
    const auto  fetch = [&](u32 x, u32 y) {
        const u8* p = px.data() + (static_cast<std::size_t>(y) * w + x) * bpp;
        return vec4{texel_channel(p, l, 0), texel_channel(p, l, 1), texel_channel(p, l, 2), texel_channel(p, l, 3)};
    };
    const vec4 top = mix(fetch(xs[0], ys[0]), fetch(xs[1], ys[0]), tx);
    const vec4 bot = mix(fetch(xs[0], ys[1]), fetch(xs[1], ys[1]), tx);
    return mix(top, bot, ty);
}

[[nodiscard]] vec4 sample_image(const texture_image& t, vec2 uv, f32 lod) noexcept
{
    const u32 top = t.level_count() - 1;
    lod = std::clamp(lod, 0.0f, static_cast<f32>(top));
    const u32 l0 = static_cast<u32>(lod);
    const u32 l1 = std::min(l0 + 1, top);
    const f32 f  = lod - static_cast<f32>(l0);
    const vec4 a = sample_level(t, l0, uv);
    return f > 0.0f && l1 != l0 ? mix(a, sample_level(t, l1, uv), f) : a;
}

struct blur_copy {
    u32              x0{}, y0{}, w{}, h{};
    std::vector<f32> rgb; // premultiplied, w * h * 3

    [[nodiscard]] bool empty() const noexcept { return w == 0 || h == 0; }
    [[nodiscard]] const f32* at(i32 x, i32 y) const noexcept
    {
        const u32 cx = static_cast<u32>(std::clamp<i32>(x - static_cast<i32>(x0), 0, static_cast<i32>(w) - 1));
        const u32 cy = static_cast<u32>(std::clamp<i32>(y - static_cast<i32>(y0), 0, static_cast<i32>(h) - 1));
        return rgb.data() + (static_cast<std::size_t>(cy) * w + cx) * 3;
    }
};

// running-sum box blur of one axis with clamped edges
void box_pass(std::vector<f32>& data, std::vector<f32>& tmp, u32 w, u32 h, int r, bool horizontal)
{
    const u32 lines = horizontal ? h : w;
    const u32 len   = horizontal ? w : h;
    const std::size_t step = horizontal ? 3 : static_cast<std::size_t>(w) * 3;
    const std::size_t line_step = horizontal ? static_cast<std::size_t>(w) * 3 : 3;
    const f32 norm = 1.0f / static_cast<f32>(2 * r + 1);
    for (u32 l = 0; l < lines; ++l) {
        const f32* src = data.data() + l * line_step;
        f32* dst = tmp.data() + l * line_step;
        for (int c = 0; c < 3; ++c) {
            const auto at = [&](i32 i) {
                return src[static_cast<std::size_t>(std::clamp<i32>(i, 0, static_cast<i32>(len) - 1)) * step + c];
            };
            f32 sum = 0.0f;
            for (int k = -r; k <= r; ++k) { sum += at(k); }
            for (u32 i = 0; i < len; ++i) {
                dst[static_cast<std::size_t>(i) * step + c] = sum * norm;
                sum += at(static_cast<i32>(i) + r + 1) - at(static_cast<i32>(i) - r);
            }
        }
    }
    data.swap(tmp);
}

} // namespace

struct software_renderer::impl {
    struct gpu_texture {
        texture_image image;
        bool          updatable{};
    };

    u32                       width{};
    u32                       height{};
    std::vector<u8>           target; // premultiplied rgba8
    std::vector<u8>           atlas;  // r8 coverage
    u32                       atlas_w{};
    u32                       atlas_h{};
    std::vector<gpu_texture>  textures; // id - 1; a texture with no levels is a free slot
    output_desc               output;

    void resize(u32 w, u32 h)
    {
        width = w;
        height = h;
        target.assign(static_cast<std::size_t>(w) * h * 4, 0);
    }

    [[nodiscard]] f32 atlas_coverage(vec2 uv) const noexcept
    {
        const f32 fx = uv.x * static_cast<f32>(atlas_w) - 0.5f;
        const f32 fy = uv.y * static_cast<f32>(atlas_h) - 0.5f;
        const f32 x0 = std::floor(fx);
        const f32 y0 = std::floor(fy);
        const f32 tx = fx - x0;
        const f32 ty = fy - y0;
        const auto at = [&](f32 x, f32 y) {
            const u32 cx = static_cast<u32>(std::clamp(x, 0.0f, static_cast<f32>(atlas_w - 1)));
            const u32 cy = static_cast<u32>(std::clamp(y, 0.0f, static_cast<f32>(atlas_h - 1)));
            return static_cast<f32>(atlas[static_cast<std::size_t>(cy) * atlas_w + cx]) * inv255;
        };
        return mix(mix(at(x0, y0), at(x0 + 1.0f, y0), tx), mix(at(x0, y0 + 1.0f), at(x0 + 1.0f, y0 + 1.0f), tx), ty);
    }

    void blend(u32 x, u32 y, const vec4& s) noexcept
    {
        u8* p = target.data() + (static_cast<std::size_t>(y) * width + x) * 4;
        const f32 k = 1.0f - s.a;
        const f32 v[4] = {s.r, s.g, s.b, s.a};
        for (int c = 0; c < 4; ++c) {
            p[c] = static_cast<u8>(std::clamp(v[c] + static_cast<f32>(p[c]) * inv255 * k, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }

    // the frame so far, blurred, over `area` (physical) plus the reach of the filter
    [[nodiscard]] blur_copy make_blur(const rect& area, f32 radius) const
    {
        blur_copy out;
        const int r = std::max(1, static_cast<int>(std::lround(radius * 0.5f)));
        const rect reach = area.expanded(static_cast<f32>(3 * r) + 1.0f);
        const i32 x0 = std::max<i32>(0, static_cast<i32>(std::floor(reach.min.x)));
        const i32 y0 = std::max<i32>(0, static_cast<i32>(std::floor(reach.min.y)));
        const i32 x1 = std::min<i32>(static_cast<i32>(width), static_cast<i32>(std::ceil(reach.max.x)));
        const i32 y1 = std::min<i32>(static_cast<i32>(height), static_cast<i32>(std::ceil(reach.max.y)));
        if (x1 <= x0 || y1 <= y0) { return out; }
        out.x0 = static_cast<u32>(x0);
        out.y0 = static_cast<u32>(y0);
        out.w  = static_cast<u32>(x1 - x0);
        out.h  = static_cast<u32>(y1 - y0);
        out.rgb.resize(static_cast<std::size_t>(out.w) * out.h * 3);
        for (u32 y = 0; y < out.h; ++y) {
            for (u32 x = 0; x < out.w; ++x) {
                const u8* p = target.data() + (static_cast<std::size_t>(out.y0 + y) * width + out.x0 + x) * 4;
                f32* d = out.rgb.data() + (static_cast<std::size_t>(y) * out.w + x) * 3;
                d[0] = static_cast<f32>(p[0]) * inv255;
                d[1] = static_cast<f32>(p[1]) * inv255;
                d[2] = static_cast<f32>(p[2]) * inv255;
            }
        }
        std::vector<f32> tmp(out.rgb.size());
        for (int pass = 0; pass < 3; ++pass) {
            box_pass(out.rgb, tmp, out.w, out.h, r, true);
            box_pass(out.rgb, tmp, out.w, out.h, r, false);
        }
        return out;
    }

    void draw(const draw_data& d, const draw_cmd& cmd);
};

void software_renderer::impl::draw(const draw_data& d, const draw_cmd& cmd)
{
    if (cmd.idx_count < 3 || static_cast<std::size_t>(cmd.idx_offset) + cmd.idx_count > d.indices.size() ||
        cmd.vtx_offset >= d.vertices.size()) {
        return;
    }
    const std::span<const vertex> verts = d.vertices.subspan(cmd.vtx_offset);

    // pixels whose centre lies inside the clip
    const i32 cx0 = std::max<i32>(0, static_cast<i32>(std::ceil(cmd.clip.min.x - 0.5f)));
    const i32 cy0 = std::max<i32>(0, static_cast<i32>(std::ceil(cmd.clip.min.y - 0.5f)));
    const i32 cx1 = std::min<i32>(static_cast<i32>(width), static_cast<i32>(std::ceil(cmd.clip.max.x - 0.5f)));
    const i32 cy1 = std::min<i32>(static_cast<i32>(height), static_cast<i32>(std::ceil(cmd.clip.max.y - 0.5f)));
    if (cx1 <= cx0 || cy1 <= cy0) { return; }

    const bool backdrop = cmd.blur > 0.0f;
    const texture_image* user_tex = nullptr;
    if (cmd.texture != 0) {
        if (cmd.texture > textures.size() || !textures[cmd.texture - 1].image.valid()) { return; }
        user_tex = &textures[cmd.texture - 1].image;
    }

    blur_copy blurred;
    if (backdrop) {
        rect bounds{{1.0e9f, 1.0e9f}, {-1.0e9f, -1.0e9f}};
        for (u32 i = 0; i < cmd.idx_count; ++i) {
            const u32 vi = d.indices[cmd.idx_offset + i];
            if (vi >= verts.size()) { return; }
            bounds.min = {std::min(bounds.min.x, verts[vi].pos.x), std::min(bounds.min.y, verts[vi].pos.y)};
            bounds.max = {std::max(bounds.max.x, verts[vi].pos.x), std::max(bounds.max.y, verts[vi].pos.y)};
        }
        blurred = make_blur(bounds.intersect(cmd.clip), cmd.blur);
        if (blurred.empty()) { return; }
    }

    for (u32 t = 0; t + 2 < cmd.idx_count; t += 3) {
        const u32 i0 = d.indices[cmd.idx_offset + t];
        const u32 i1 = d.indices[cmd.idx_offset + t + 1];
        const u32 i2 = d.indices[cmd.idx_offset + t + 2];
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) { continue; }
        const vertex* a = &verts[i0];
        const vertex* b = &verts[i1];
        const vertex* c = &verts[i2];

        // a shape quad: every vertex carries the same record index, flat-shaded from the first
        const bool is_shape = (a->v & 0x8000u) != 0;
        const shape_record* rec = nullptr;
        if (is_shape) {
            const std::size_t index = static_cast<std::size_t>(a->u) | (static_cast<std::size_t>(a->v & 0x7fffu) << 16);
            if (index >= d.shapes.size()) { continue; }
            rec = &d.shapes[index];
        }

        f32 area = (b->pos.x - a->pos.x) * (c->pos.y - a->pos.y) - (b->pos.y - a->pos.y) * (c->pos.x - a->pos.x);
        if (area == 0.0f) { continue; }
        if (area < 0.0f) { std::swap(b, c); area = -area; }

        const f32 minx = std::min({a->pos.x, b->pos.x, c->pos.x});
        const f32 maxx = std::max({a->pos.x, b->pos.x, c->pos.x});
        const f32 miny = std::min({a->pos.y, b->pos.y, c->pos.y});
        const f32 maxy = std::max({a->pos.y, b->pos.y, c->pos.y});
        const i32 x0 = std::max(cx0, static_cast<i32>(std::ceil(minx - 0.5f)));
        const i32 x1 = std::min(cx1, static_cast<i32>(std::ceil(maxx - 0.5f)) + 1);
        const i32 y0 = std::max(cy0, static_cast<i32>(std::ceil(miny - 0.5f)));
        const i32 y1 = std::min(cy1, static_cast<i32>(std::ceil(maxy - 0.5f)) + 1);

        // top-left fill rule, so pixels on a shared edge are drawn once
        const auto top_left = [](vec2 p, vec2 q) { return (q.y == p.y && q.x > p.x) || q.y < p.y; };
        const bool tl_ab = top_left(a->pos, b->pos);
        const bool tl_bc = top_left(b->pos, c->pos);
        const bool tl_ca = top_left(c->pos, a->pos);
        const auto edge = [](vec2 p, vec2 q, vec2 s) {
            return (q.x - p.x) * (s.y - p.y) - (q.y - p.y) * (s.x - p.x);
        };

        const f32 inv_area = 1.0f / area;
        const vec4 ca = unpack(a->col), cb = unpack(b->col), cc = unpack(c->col);
        const f32 inv_uv = 1.0f / 32767.0f;

        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = x0; x < x1; ++x) {
                const vec2 p{static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f};
                const f32 e_ab = edge(a->pos, b->pos, p);
                const f32 e_bc = edge(b->pos, c->pos, p);
                const f32 e_ca = edge(c->pos, a->pos, p);
                if (!(e_ab > 0.0f || (e_ab == 0.0f && tl_ab)) || !(e_bc > 0.0f || (e_bc == 0.0f && tl_bc)) ||
                    !(e_ca > 0.0f || (e_ca == 0.0f && tl_ca))) {
                    continue;
                }
                const f32 wa = e_bc * inv_area;
                const f32 wb = e_ca * inv_area;
                const f32 wc = e_ab * inv_area;
                const vec4 col{ca.r * wa + cb.r * wb + cc.r * wc, ca.g * wa + cb.g * wb + cc.g * wc,
                               ca.b * wa + cb.b * wb + cc.b * wc, ca.a * wa + cb.a * wb + cc.a * wc};

                vec4 out;
                if (rec != nullptr && backdrop) {
                    const vec2 q   = p - rec->center;
                    const f32  cov = saturate(0.5f - sd_round_box(q, rec->half_size, rec->radius));
                    const f32* bg  = blurred.at(x, y);
                    f32 r = bg[0], g = bg[1], b2 = bg[2];
                    const f32 lum = luma(r, g, b2);
                    r  = mix(lum, r, rec->shadow_offset.x) * rec->shadow_offset.y;
                    g  = mix(lum, g, rec->shadow_offset.x) * rec->shadow_offset.y;
                    b2 = mix(lum, b2, rec->shadow_offset.x) * rec->shadow_offset.y;
                    r = saturate(r); g = saturate(g); b2 = saturate(b2);
                    const vec4 tint = unpack(rec->fill_top);
                    r  = mix(r, tint.r, tint.a);
                    g  = mix(g, tint.g, tint.a);
                    b2 = mix(b2, tint.b, tint.a);
                    f32 grain = std::sin(p.x * 12.9898f + p.y * 78.233f) * 43758.5453f;
                    grain = grain - std::floor(grain) - 0.5f;
                    const f32 n = grain * rec->extra * 0.5f;
                    out = {std::max(r + n, 0.0f) * cov, std::max(g + n, 0.0f) * cov, std::max(b2 + n, 0.0f) * cov, cov};
                } else if (rec != nullptr && user_tex != nullptr) {
                    const vec2 q   = p - rec->center;
                    const f32  cov = saturate(0.5f - sd_round_box(q, rec->half_size, rec->radius));
                    const vec2 st  = {saturate(q.x / (2.0f * rec->half_size.x) + 0.5f),
                                      saturate(q.y / (2.0f * rec->half_size.y) + 0.5f)};
                    const vec2 uv0 = rec->shadow_offset;
                    const vec2 uv1 = {rec->border_width, rec->shadow_blur};
                    const vec2 uv  = {mix(uv0.x, uv1.x, st.x), mix(uv0.y, uv1.y, st.y)};
                    const f32 tpp_x = std::abs(uv1.x - uv0.x) * static_cast<f32>(user_tex->width()) /
                                      std::max(2.0f * rec->half_size.x, 1.0f);
                    const f32 tpp_y = std::abs(uv1.y - uv0.y) * static_cast<f32>(user_tex->height()) /
                                      std::max(2.0f * rec->half_size.y, 1.0f);
                    const f32 lod = std::max(std::log2(std::max(std::max(tpp_x, tpp_y), 1.0e-4f)), 0.0f);
                    const vec4 s  = sample_image(*user_tex, uv, lod);
                    const f32  al = s.a * col.a * cov;
                    out = {s.r * col.r * al, s.g * col.g * al, s.b * col.b * al, al};
                } else if (rec != nullptr) {
                    out = shade_shape(*rec, p);
                } else {
                    f32 cov = atlas_coverage({(static_cast<f32>(a->u) * wa + static_cast<f32>(b->u) * wb + static_cast<f32>(c->u) * wc) * inv_uv,
                                              (static_cast<f32>(a->v) * wa + static_cast<f32>(b->v) * wb + static_cast<f32>(c->v) * wc) * inv_uv});
                    if (d.text_contrast > 0.0f) {
                        // thickens light-on-dark glyph edges in proportion to text lightness
                        const f32 k = d.text_contrast * luma(col.r, col.g, col.b);
                        cov = cov * (1.0f + k) / (1.0f + k * cov);
                    }
                    const f32 al = col.a * cov;
                    out = {col.r * al, col.g * al, col.b * al, al};
                }
                blend(static_cast<u32>(x), static_cast<u32>(y), out);
            }
        }
    }
}

software_renderer::software_renderer() noexcept = default;
software_renderer::~software_renderer()         = default;
software_renderer::software_renderer(software_renderer&&) noexcept            = default;
software_renderer& software_renderer::operator=(software_renderer&&) noexcept = default;

bool software_renderer::create(const font_atlas& atlas)
{
    destroy();
    impl_ = std::make_unique<impl>();
    if (!update_atlas(atlas)) {
        impl_.reset();
        return false;
    }
    return true;
}

void software_renderer::destroy() noexcept { impl_.reset(); }

bool software_renderer::update_atlas(const font_atlas& atlas)
{
    if (!impl_ || atlas.pixels().size() < static_cast<std::size_t>(atlas.width()) * atlas.height() || atlas.width() == 0) {
        return false;
    }
    impl_->atlas.assign(atlas.pixels().begin(), atlas.pixels().end());
    impl_->atlas_w = atlas.width();
    impl_->atlas_h = atlas.height();
    return true;
}

void software_renderer::resize(u32 width, u32 height)
{
    if (impl_) { impl_->resize(width, height); }
}

void software_renderer::clear(color c)
{
    if (!impl_) { return; }
    const f32 a = static_cast<f32>(c.a) * inv255;
    const u8 px[4] = {static_cast<u8>(static_cast<f32>(c.r) * a + 0.5f), static_cast<u8>(static_cast<f32>(c.g) * a + 0.5f),
                      static_cast<u8>(static_cast<f32>(c.b) * a + 0.5f), c.a};
    for (std::size_t i = 0; i + 3 < impl_->target.size(); i += 4) {
        std::copy(px, px + 4, impl_->target.data() + i);
    }
}

void software_renderer::render(const draw_data& data)
{
    if (!impl_) { return; }
    const u32 w = static_cast<u32>(std::max(data.display_size.x, 0.0f) + 0.5f);
    const u32 h = static_cast<u32>(std::max(data.display_size.y, 0.0f) + 0.5f);
    if (w != impl_->width || h != impl_->height) { impl_->resize(w, h); }
    if (w == 0 || h == 0) { return; }
    for (const draw_cmd& cmd : data.commands) { impl_->draw(data, cmd); }
}

void software_renderer::set_output(const output_desc& output) noexcept
{
    if (impl_) { impl_->output = output; }
}

output_desc software_renderer::output() const noexcept { return impl_ ? impl_->output : output_desc{}; }

texture_id software_renderer::create_texture(u32 width, u32 height, std::span<const u8> rgba)
{
    return create_texture(texture_desc{width, height, texture_format::rgba8, 1, false}, rgba);
}

texture_id software_renderer::create_texture(const texture_desc& desc, std::span<const u8> pixels)
{
    if (!impl_) { return 0; }
    impl::gpu_texture tex;
    tex.updatable = desc.updatable;
    if (!tex.image.create(desc, pixels)) { return 0; }
    auto& all = impl_->textures;
    const auto free_slot = std::find_if(all.begin(), all.end(), [](const impl::gpu_texture& t) { return !t.image.valid(); });
    if (free_slot != all.end()) {
        *free_slot = std::move(tex);
        return static_cast<texture_id>(free_slot - all.begin()) + 1;
    }
    all.push_back(std::move(tex));
    return static_cast<texture_id>(all.size());
}

bool software_renderer::update_texture(texture_id id, u32 x, u32 y, u32 width, u32 height, std::span<const u8> pixels)
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return false; }
    impl::gpu_texture& t = impl_->textures[id - 1];
    if (!t.image.valid() || !t.updatable) { return false; }
    std::vector<texture_image::region> dirty;
    return t.image.update(x, y, width, height, pixels, dirty);
}

void software_renderer::destroy_texture(texture_id id) noexcept
{
    if (!impl_ || id == 0 || id > impl_->textures.size()) { return; }
    impl_->textures[id - 1] = {};
}

u32 software_renderer::width() const noexcept { return impl_ ? impl_->width : 0; }
u32 software_renderer::height() const noexcept { return impl_ ? impl_->height : 0; }

std::span<const u8> software_renderer::pixels() const noexcept
{
    return impl_ ? std::span<const u8>{impl_->target} : std::span<const u8>{};
}

bool software_renderer::copy_straight(std::span<u8> out) const noexcept
{
    if (!impl_ || out.size() < impl_->target.size()) { return false; }
    for (std::size_t i = 0; i + 3 < impl_->target.size(); i += 4) {
        const u8* s = impl_->target.data() + i;
        u8* d = out.data() + i;
        const u32 a = s[3];
        for (int c = 0; c < 3; ++c) {
            d[c] = static_cast<u8>(a == 0 ? 0 : std::min<u32>(255, (static_cast<u32>(s[c]) * 255 + a / 2) / a));
        }
        d[3] = s[3];
    }
    return true;
}

} // namespace strata

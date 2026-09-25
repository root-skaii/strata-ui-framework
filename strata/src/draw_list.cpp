#include "strata/draw_list.hpp"

#include "strata/bidi.hpp"

#include <cmath>
#include <numbers>

namespace strata {

namespace {

constexpr u16 shape_flag = 0x8000; // bit 15 of vertex.v marks a shape quad; the rest is the index high bits

[[nodiscard]] vec2 normalized_or(vec2 v, vec2 fallback) noexcept
{
    const f32 l2 = dot(v, v);
    if (l2 < 1.0e-8f) { return fallback; }
    return v * (1.0f / std::sqrt(l2));
}

[[nodiscard]] f32 dist2(vec2 a, vec2 b) noexcept { return dot(a - b, a - b); }

} // namespace

vec2 gradient_direction(f32 degrees) noexcept
{
    const f32 rad = degrees * (std::numbers::pi_v<f32> / 180.0f);
    return {std::cos(rad), std::sin(rad)};
}

draw_list::draw_list(draw_list_limits limits)
    : vertices_{limits.max_vertices}
    , indices_{limits.max_indices}
    , commands_{limits.max_commands}
    , shapes_{limits.max_shapes}
    , reorder_scratch_{limits.max_commands}
{}

void draw_list::begin(vec2 display_size, const font_atlas& atlas, f32 scale)
{
    vertices_.clear();
    indices_.clear();
    commands_.clear();
    shapes_.clear();
    overflow_     = false;
    force_new_command_ = false;
    texture_      = 0;
    blur_         = 0.0f;
    scale_        = scale > 0.0f ? scale : 1.0f;
    display_size_ = display_size;
    atlas_        = &atlas;
    white_u_      = atlas.white_uv()[0];
    white_v_      = atlas.white_uv()[1];
    clip_depth_   = 0;
    alpha_        = 1.0f;
    alpha_depth_  = 0;
    set_clip({{0, 0}, display_size * (1.0f / scale_)});
}

void draw_list::set_clip(const rect& logical) noexcept
{
    clip_ = logical;
    if (scale_ == 1.0f) {
        phys_clip_ = logical;
        return;
    }
    phys_clip_ = {{std::round(logical.min.x * scale_), std::round(logical.min.y * scale_)},
                  {std::round(logical.max.x * scale_), std::round(logical.max.y * scale_)}};
}

void draw_list::push_alpha(f32 a) noexcept
{
    if (alpha_depth_ < alpha_stack_.size()) {
        alpha_stack_[alpha_depth_++] = alpha_;
    }
    alpha_ *= std::clamp(a, 0.0f, 1.0f);
}

void draw_list::pop_alpha() noexcept
{
    if (alpha_depth_ > 0) {
        alpha_ = alpha_stack_[--alpha_depth_];
    }
}

void draw_list::push_clip(const rect& r) noexcept
{
    if (clip_depth_ < max_clip_depth) {
        clip_stack_[clip_depth_++] = clip_;
    }
    set_clip(clip_.intersect(r));
}

void draw_list::push_clip_absolute(const rect& r) noexcept
{
    if (clip_depth_ < max_clip_depth) {
        clip_stack_[clip_depth_++] = clip_;
    }
    set_clip(r);
}

void draw_list::pop_clip() noexcept
{
    if (clip_depth_ > 0) {
        set_clip(clip_stack_[--clip_depth_]);
    }
}

draw_data draw_list::data() const noexcept
{
    return {vertices_.view(), indices_.view(), commands_.view(), shapes_.view(), display_size_};
}

bool draw_list::reserve(u32 vertex_count, u32 index_count, prim& out) noexcept
{
    if (commands_.empty() || force_new_command_ || commands_.back().clip != phys_clip_ ||
        commands_.back().texture != texture_ || commands_.back().blur != blur_) {
        draw_cmd* cmd = commands_.grow(1);
        if (cmd == nullptr) {
            overflow_ = true;
            return false;
        }
        *cmd = {phys_clip_, static_cast<u32>(indices_.size()), 0, texture_, blur_};
        force_new_command_ = false;
    }

    u32* i = indices_.grow(index_count);
    if (i == nullptr) {
        overflow_ = true;
        return false;
    }
    vertex* v = vertices_.grow(vertex_count);
    if (v == nullptr) {
        indices_.shrink(index_count);
        overflow_ = true;
        return false;
    }

    out = {v, i, static_cast<u32>(vertices_.size()) - vertex_count};
    commands_.back().idx_count += index_count;
    return true;
}

void draw_list::add_quad(const rect& r, color tl, color tr, color br, color bl) noexcept
{
    prim p;
    if (!reserve(4, 6, p)) {
        return;
    }
    vec2 a = r.min * scale_;
    vec2 b = r.max * scale_;
    if (scale_ != 1.0f) { // crisp edges at fractional scales
        a = {std::round(a.x), std::round(a.y)};
        b = {std::round(b.x), std::round(b.y)};
        if (b.x <= a.x) { b.x = a.x + 1.0f; }
        if (b.y <= a.y) { b.y = a.y + 1.0f; }
    }
    p.v[0] = {a, white_u_, white_v_, fade(tl)};
    p.v[1] = {{b.x, a.y}, white_u_, white_v_, fade(tr)};
    p.v[2] = {b, white_u_, white_v_, fade(br)};
    p.v[3] = {{a.x, b.y}, white_u_, white_v_, fade(bl)};

    constexpr u32 quad[6] = {0, 1, 2, 0, 2, 3};
    for (u32 k = 0; k < 6; ++k) {
        p.i[k] = p.base + quad[k];
    }
}

// one quad over `bounds` (logical) whose pixels are shaded from shape record `index`
bool draw_list::emit_quad_for(const rect& bounds, u32 index, color vertex_color) noexcept
{
    prim p;
    if (!reserve(4, 6, p)) {
        return false;
    }
    const u16 lo = static_cast<u16>(index & 0xffffu);
    const u16 hi = static_cast<u16>(shape_flag | ((index >> 16) & 0x7fffu));
    const vec2 a = bounds.min * scale_;
    const vec2 b = bounds.max * scale_;
    p.v[0] = {a, lo, hi, vertex_color};
    p.v[1] = {{b.x, a.y}, lo, hi, vertex_color};
    p.v[2] = {b, lo, hi, vertex_color};
    p.v[3] = {{a.x, b.y}, lo, hi, vertex_color};

    constexpr u32 quad[6] = {0, 1, 2, 0, 2, 3};
    for (u32 k = 0; k < 6; ++k) {
        p.i[k] = p.base + quad[k];
    }
    return true;
}

void draw_list::shape(const rect& r, const shape_style& st)
{
    if (r.empty()) {
        return;
    }

    const bool has_fill   = st.fill_top.a != 0 || st.fill_bottom.a != 0;
    const bool has_border = st.border.a != 0 && st.border_width > 0.0f;
    const bool has_shadow = st.shadow.a != 0 && st.shadow_blur > 0.0f;
    if (!has_fill && !has_border && !has_shadow) {
        return;
    }

    // the quad must cover the antialiasing skirt (one physical pixel) and the whole shadow
    const f32 s = scale_;
    f32 margin = 1.0f / s;
    if (has_shadow) {
        margin = std::max(margin, st.shadow_blur + std::max(std::abs(st.shadow_offset.x), std::abs(st.shadow_offset.y)) + 1.0f / s);
    }
    const rect bounds = r.expanded(margin);
    if (!clip_.overlaps(bounds)) {
        return;
    }

    shape_record* rec = shapes_.grow(1);
    if (rec == nullptr) {
        overflow_ = true;
        return;
    }
    const auto index = static_cast<u32>(shapes_.size() - 1);

    const f32 max_radius = std::min(r.width(), r.height()) * 0.5f;
    rec->center        = r.center() * s;
    rec->half_size     = r.size() * (0.5f * s);
    for (u32 k = 0; k < 4; ++k) {
        rec->radius[k] = std::clamp(st.radius[k], 0.0f, max_radius) * s;
    }
    rec->fill_top      = fade(st.fill_top);
    rec->fill_bottom   = fade(st.fill_bottom);
    rec->border        = fade(st.border);
    rec->shadow        = has_shadow ? fade(st.shadow) : color{0, 0, 0, 0};
    rec->border_width  = has_border ? std::max(st.border_width * s, 1.0f) : 0.0f;
    rec->shadow_blur   = has_shadow ? st.shadow_blur * s : 0.0f;
    rec->shadow_offset = st.shadow_offset * s;
    rec->gradient_dir  = normalized_or(st.gradient_dir, {0.0f, 1.0f});
    rec->gradient_kind = st.radial ? 1.0f : 0.0f;
    rec->extra         = 0.0f;

    if (!emit_quad_for(bounds, index, color{255, 255, 255, 255})) {
        shapes_.shrink(1);
    }
}

void draw_list::image(const rect& r, texture_id tex, vec2 uv0, vec2 uv1, color tint, const std::array<f32, 4>& radius)
{
    if (tex == 0 || tint.a == 0 || r.empty()) {
        return;
    }
    const rect bounds = r.expanded(1.0f / scale_); // the antialiasing skirt of the rounded box
    if (!clip_.overlaps(bounds)) {
        return;
    }

    shape_record* rec = shapes_.grow(1);
    if (rec == nullptr) {
        overflow_ = true;
        return;
    }
    const auto index = static_cast<u32>(shapes_.size() - 1);

    const f32 s = scale_;
    const f32 max_radius = std::min(r.width(), r.height()) * 0.5f;
    rec->center    = r.center() * s;
    rec->half_size = r.size() * (0.5f * s);
    for (u32 k = 0; k < 4; ++k) {
        rec->radius[k] = std::clamp(radius[k], 0.0f, max_radius) * s;
    }
    rec->fill_top      = color{0, 0, 0, 0};
    rec->fill_bottom   = color{0, 0, 0, 0};
    rec->border        = color{0, 0, 0, 0};
    rec->shadow        = color{0, 0, 0, 0};
    rec->border_width  = uv1.x; // (not pixels: see the header)
    rec->shadow_blur   = uv1.y;
    rec->shadow_offset = uv0;
    rec->gradient_dir  = {0.0f, 1.0f};
    rec->gradient_kind = 0.0f;
    rec->extra         = 0.0f;

    texture_ = tex;
    const bool ok = emit_quad_for(bounds, index, fade(tint));
    texture_ = 0;
    if (!ok) {
        shapes_.shrink(1);
    }
}

void draw_list::backdrop(const rect& r, f32 blur_radius, color tint, const std::array<f32, 4>& radius, f32 noise, f32 saturation,
                         f32 brightness)
{
    if (r.empty()) {
        return;
    }
    const rect bounds = r.expanded(1.0f / scale_);
    if (!clip_.overlaps(bounds)) {
        return;
    }

    shape_record* rec = shapes_.grow(1);
    if (rec == nullptr) {
        overflow_ = true;
        return;
    }
    const auto index = static_cast<u32>(shapes_.size() - 1);

    const f32 s = scale_;
    const f32 max_radius = std::min(r.width(), r.height()) * 0.5f;
    rec->center    = r.center() * s;
    rec->half_size = r.size() * (0.5f * s);
    for (u32 k = 0; k < 4; ++k) {
        rec->radius[k] = std::clamp(radius[k], 0.0f, max_radius) * s;
    }
    // without backdrop support a renderer draws this record as an ordinary shape: the tint
    rec->fill_top      = fade(tint);
    rec->fill_bottom   = rec->fill_top;
    rec->border        = color{0, 0, 0, 0};
    rec->shadow        = color{0, 0, 0, 0};
    rec->border_width  = 0.0f;
    rec->shadow_blur   = 0.0f;
    rec->shadow_offset = {std::clamp(saturation, 0.0f, 4.0f), std::clamp(brightness, 0.0f, 3.0f)}; // (no shadow: free to carry these)
    rec->gradient_dir  = {0.0f, 1.0f};
    rec->gradient_kind = 0.0f;
    rec->extra         = std::clamp(noise, 0.0f, 1.0f);

    blur_ = std::max(blur_radius * s, 0.5f);
    const bool ok = emit_quad_for(bounds, index, color{255, 255, 255, 255});
    blur_ = 0.0f;
    if (!ok) {
        shapes_.shrink(1);
    }
}

void draw_list::reorder_commands(std::span<const command_range> order)
{
    const std::size_t total = commands_.size();
    std::size_t sum = 0;
    for (const command_range& r : order) {
        if (static_cast<std::size_t>(r.first) + r.count > total) { return; }
        sum += r.count;
    }
    if (sum != total || total == 0) { return; }

    reorder_scratch_.clear();
    draw_cmd* out = reorder_scratch_.grow(total);
    if (out == nullptr) { return; }
    for (const command_range& r : order) {
        std::copy_n(commands_.data() + r.first, r.count, out);
        out += r.count;
    }
    std::copy_n(reorder_scratch_.data(), total, commands_.data());
}

void draw_list::fill_convex_aa(std::span<const vec2> logical_pts, color c) noexcept
{
    const u32 n = static_cast<u32>(logical_pts.size());
    if (n < 3 || n > max_polygon_points) {
        return;
    }
    std::array<vec2, max_polygon_points> pts;
    for (u32 i = 0; i < n; ++i) { pts[i] = logical_pts[i] * scale_; }

    // orientation: outward normal is (dy, -dx) for clockwise-on-screen input
    f32 area = 0;
    for (u32 i = 0; i < n; ++i) {
        const vec2 a = pts[i];
        const vec2 b = pts[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    const f32 sign = area >= 0 ? 1.0f : -1.0f;

    std::array<vec2, max_polygon_points> normals;
    for (u32 i = 0; i < n; ++i) {
        const vec2 d = pts[(i + 1) % n] - pts[i];
        const f32  len = std::sqrt(dot(d, d));
        normals[i] = len > 1e-6f ? vec2{d.y, -d.x} * (sign / len) : vec2{};
    }

    prim p;
    if (!reserve(2 * n, 3 * (n - 2) + 6 * n, p)) {
        return;
    }

    c = fade(c);
    const color fringe = c.scaled_alpha(0.0f);
    for (u32 i = 0; i < n; ++i) {
        const vec2 n_prev = normals[(i + n - 1) % n];
        const vec2 n_next = normals[i];
        vec2 dm = (n_prev + n_next) * 0.5f;
        const f32 len2 = dot(dm, dm);
        if (len2 > 1e-6f) {
            dm = dm * std::min(1.0f / len2, 100.0f);
        }
        dm = dm * 0.5f; // half a pixel each way => one pixel wide fringe
        p.v[2 * i]     = {pts[i] - dm, white_u_, white_v_, c};
        p.v[2 * i + 1] = {pts[i] + dm, white_u_, white_v_, fringe};
    }

    u32 k = 0;
    for (u32 i = 2; i < n; ++i) {
        p.i[k++] = p.base;
        p.i[k++] = p.base + 2 * (i - 1);
        p.i[k++] = p.base + 2 * i;
    }
    for (u32 i = 0; i < n; ++i) {
        const u32 j = (i + 1) % n;
        p.i[k++] = p.base + 2 * i + 1; // outer i
        p.i[k++] = p.base + 2 * j + 1; // outer j
        p.i[k++] = p.base + 2 * j;     // inner j
        p.i[k++] = p.base + 2 * i + 1;
        p.i[k++] = p.base + 2 * j;
        p.i[k++] = p.base + 2 * i;     // inner i
    }
}

void draw_list::rect_filled(const rect& r, color c, f32 rounding, corners which)
{
    if (c.a == 0 || r.empty() || !clip_.overlaps(r)) {
        return;
    }

    rounding = std::clamp(rounding, 0.0f, std::min(r.width(), r.height()) * 0.5f);
    if (rounding * scale_ < 0.5f || which == corners::none) {
        add_quad(r, c, c, c, c); // pixel-crisp fast path, 4 vertices
        return;
    }

    shape_style st;
    st.radius      = radii(rounding, which);
    st.fill_top    = c;
    st.fill_bottom = c;
    shape(r, st);
}

void draw_list::rect_gradient_v(const rect& r, color top, color bottom)
{
    if (r.empty() || !clip_.overlaps(r)) {
        return;
    }
    add_quad(r, top, top, bottom, bottom);
}

void draw_list::rect_gradient(const rect& r, color tl, color tr, color br, color bl)
{
    if (r.empty() || !clip_.overlaps(r)) {
        return;
    }
    add_quad(r, tl, tr, br, bl);
}

void draw_list::rect_gradient_angle(const rect& r, color from, color to, f32 degrees, f32 rounding)
{
    shape_style st;
    st.radius       = radii(rounding);
    st.fill_top     = from;
    st.fill_bottom  = to;
    st.gradient_dir = gradient_direction(degrees);
    shape(r, st);
}

void draw_list::rect_gradient_radial(const rect& r, color inner, color outer, f32 rounding)
{
    shape_style st;
    st.radius      = radii(rounding);
    st.fill_top    = inner;
    st.fill_bottom = outer;
    st.radial      = true;
    shape(r, st);
}

void draw_list::rect_outline(const rect& r, color c, f32 rounding, f32 thickness, corners which)
{
    if (c.a == 0 || r.empty() || !clip_.overlaps(r)) {
        return;
    }

    rounding = std::clamp(rounding, 0.0f, std::min(r.width(), r.height()) * 0.5f);
    if (rounding * scale_ < 0.5f || which == corners::none) {
        const f32 t = thickness;
        add_quad({r.min, {r.max.x, r.min.y + t}}, c, c, c, c);
        add_quad({{r.min.x, r.max.y - t}, r.max}, c, c, c, c);
        add_quad({{r.min.x, r.min.y + t}, {r.min.x + t, r.max.y - t}}, c, c, c, c);
        add_quad({{r.max.x - t, r.min.y + t}, {r.max.x, r.max.y - t}}, c, c, c, c);
        return;
    }

    shape_style st;
    st.radius       = radii(rounding, which);
    st.border       = c;
    st.border_width = thickness;
    shape(r, st);
}

void draw_list::line(vec2 a, vec2 b, color c, f32 thickness)
{
    const vec2 d   = b - a;
    const f32  len = std::sqrt(dot(d, d));
    if (c.a == 0 || len < 1e-6f) {
        return;
    }
    const vec2 n = vec2{-d.y, d.x} * (thickness * 0.5f / len);
    const vec2 quad[4] = {a + n, b + n, b - n, a - n};
    fill_convex_aa(quad, c);
}

void draw_list::triangle_filled(vec2 a, vec2 b, vec2 c, color col)
{
    if (col.a == 0) {
        return;
    }
    const vec2 tri[3] = {a, b, c};
    fill_convex_aa(tri, col);
}

void draw_list::area_fill(std::span<const vec2> top, f32 base_y, color top_col, color base_col)
{
    if (top.size() < 2 || (top_col.a == 0 && base_col.a == 0)) {
        return;
    }
    const u32 n = static_cast<u32>(top.size());
    prim p;
    if (!reserve(2 * n, 6 * (n - 1), p)) {
        return;
    }
    const f32 by = base_y * scale_;
    const color ct = fade(top_col);
    const color cb = fade(base_col);
    for (u32 i = 0; i < n; ++i) {
        const vec2 q = top[i] * scale_;
        p.v[2 * i]     = {q, white_u_, white_v_, ct};
        p.v[2 * i + 1] = {{q.x, by}, white_u_, white_v_, cb};
    }
    for (u32 i = 0; i + 1 < n; ++i) {
        const u32 a = p.base + 2 * i;
        u32* idx = p.i + 6 * i;
        idx[0] = a;     idx[1] = a + 2; idx[2] = a + 1;
        idx[3] = a + 1; idx[4] = a + 2; idx[5] = a + 3;
    }
}

void draw_list::polygon_filled(std::span<const vec2> points, color col)
{
    if (col.a == 0) {
        return;
    }
    fill_convex_aa(points, col);
}

// a strip of four vertices per point: [outer fringe | solid | solid | outer fringe] across the line, mitred where
// segments meet. the fringe is one physical pixel wide and fades to nothing, which is what antialiases the edge.
void draw_list::polyline(std::span<const vec2> points, color col, f32 thickness, bool closed)
{
    if (col.a == 0 || points.size() < 2 || thickness <= 0.0f) {
        return;
    }

    poly_pts_.clear();
    vec2 lo = points[0] * scale_;
    vec2 hi = lo;
    for (const vec2 logical : points) {
        const vec2 p = logical * scale_;
        if (poly_pts_.empty() || dist2(p, poly_pts_.back()) > 1.0e-4f) { // drop repeated points: they have no direction
            poly_pts_.push_back(p);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
        }
    }
    if (closed && poly_pts_.size() > 2 && dist2(poly_pts_.front(), poly_pts_.back()) <= 1.0e-4f) {
        poly_pts_.pop_back();
    }
    const u32 n = static_cast<u32>(poly_pts_.size());
    if (n < 2) {
        return;
    }
    if (n == 2) { closed = false; }

    const f32 half = std::max(thickness * scale_, 1.0f) * 0.5f;
    const f32 pad  = half + 1.0f;
    if (!phys_clip_.overlaps(rect{{lo.x - pad, lo.y - pad}, {hi.x + pad, hi.y + pad}})) {
        return;
    }

    const u32 segs = closed ? n : n - 1;
    poly_nrm_.resize(segs);
    for (u32 s = 0; s < segs; ++s) {
        const vec2 d = poly_pts_[(s + 1) % n] - poly_pts_[s];
        poly_nrm_[s] = normalized_or(vec2{d.y, -d.x}, {0.0f, 1.0f});
    }

    prim p;
    if (!reserve(4 * n, 18 * segs, p)) {
        return;
    }
    const color solid = fade(col);
    const color clear = solid.scaled_alpha(0.0f);
    const f32   inner = std::max(half - 0.5f, 0.0f);
    const f32   outer = half + 0.5f;

    for (u32 i = 0; i < n; ++i) {
        const vec2 n_prev = (i > 0 || closed) ? poly_nrm_[(i + segs - 1) % segs] : poly_nrm_[0];
        const vec2 n_next = i < segs ? poly_nrm_[i] : poly_nrm_[segs - 1];
        vec2 m = normalized_or(n_prev + n_next, n_next); // the mitre direction
        const f32 k = 1.0f / std::max(dot(m, n_next), 0.25f); // its length, limited so sharp corners do not spike
        m = m * k;

        const vec2 pos = poly_pts_[i];
        vertex* v = p.v + 4 * i;
        v[0] = {pos - m * outer, white_u_, white_v_, clear};
        v[1] = {pos - m * inner, white_u_, white_v_, solid};
        v[2] = {pos + m * inner, white_u_, white_v_, solid};
        v[3] = {pos + m * outer, white_u_, white_v_, clear};
    }

    u32 k = 0;
    for (u32 s = 0; s < segs; ++s) {
        const u32 a = p.base + 4 * s;
        const u32 b = p.base + 4 * ((s + 1) % n);
        for (u32 band = 0; band < 3; ++band) {
            p.i[k++] = a + band;
            p.i[k++] = a + band + 1;
            p.i[k++] = b + band + 1;
            p.i[k++] = a + band;
            p.i[k++] = b + band + 1;
            p.i[k++] = b + band;
        }
    }
}

void draw_list::bezier_cubic(vec2 p0, vec2 p1, vec2 p2, vec2 p3, color c, f32 thickness, u32 segments)
{
    constexpr u32 max_segments = 128;
    if (segments == 0) {
        const f32 len = (std::sqrt(dist2(p0, p1)) + std::sqrt(dist2(p1, p2)) + std::sqrt(dist2(p2, p3))) * scale_;
        segments = static_cast<u32>(std::clamp(len / 6.0f + 4.0f, 4.0f, static_cast<f32>(max_segments)));
    }
    segments = std::min(segments, max_segments);

    std::array<vec2, max_segments + 1> pts;
    for (u32 i = 0; i <= segments; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 u = 1.0f - t;
        pts[i] = p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) + p3 * (t * t * t);
    }
    polyline({pts.data(), segments + 1}, c, thickness, false);
}

void draw_list::bezier_quadratic(vec2 p0, vec2 p1, vec2 p2, color c, f32 thickness, u32 segments)
{
    // a quadratic is a cubic with the control points pulled two thirds of the way to the single one
    bezier_cubic(p0, p0 + (p1 - p0) * (2.0f / 3.0f), p2 + (p1 - p2) * (2.0f / 3.0f), p2, c, thickness, segments);
}

void draw_list::arc(vec2 center, f32 radius, f32 a0, f32 a1, color c, f32 thickness, u32 segments)
{
    constexpr u32 max_segments = 256;
    if (radius <= 0.0f) {
        return;
    }
    if (segments == 0) {
        const f32 len = std::abs(a1 - a0) * radius * scale_;
        segments = static_cast<u32>(std::clamp(len / 4.0f + 2.0f, 4.0f, static_cast<f32>(max_segments)));
    }
    segments = std::min(segments, max_segments);

    std::array<vec2, max_segments + 1> pts;
    for (u32 i = 0; i <= segments; ++i) {
        const f32 a = a0 + (a1 - a0) * static_cast<f32>(i) / static_cast<f32>(segments);
        pts[i] = center + vec2{std::cos(a), std::sin(a)} * radius;
    }
    const bool full = std::abs(a1 - a0) >= 2.0f * std::numbers::pi_v<f32> - 1.0e-3f;
    polyline({pts.data(), full ? segments : segments + 1}, c, thickness, full);
}

void draw_list::circle_filled(vec2 center, f32 radius, color c)
{
    if (radius <= 0.0f) {
        return;
    }
    shape_style st;
    st.radius      = radii(radius);
    st.fill_top    = c;
    st.fill_bottom = c;
    shape({{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}}, st);
}

void draw_list::circle(vec2 center, f32 radius, color c, f32 thickness)
{
    if (radius <= 0.0f) {
        return;
    }
    shape_style st;
    st.radius       = radii(radius);
    st.border       = c;
    st.border_width = thickness;
    shape({{center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}}, st);
}

void draw_list::text(vec2 pos, color c, std::string_view s, font_id font, text_flags style)
{
    if (c.a == 0 || s.empty() || atlas_ == nullptr) {
        return;
    }

    std::string visual; // right-to-left text is drawn in visual order, letters joined
    if (has_rtl_text(s)) {
        visual = to_visual(s, atlas_, font);
        s      = visual;
    }
    const color plain = c; // (rect_filled fades by itself)
    c = fade(c);
    const f32 lh = atlas_->line_height_px(font);
    const vec2 p0 = pos * scale_;
    if (p0.y > phys_clip_.max.y || p0.x > phys_clip_.max.x) {
        return;
    }

    const bool bold      = has_text_flag(style, text_flags::bold);
    const bool italic    = has_text_flag(style, text_flags::italic);
    const bool underline = has_text_flag(style, text_flags::underline);
    const bool strike    = has_text_flag(style, text_flags::strike);
    const f32  slant     = italic ? 0.21f : 0.0f;                     // how far the top of a glyph leans right
    const f32  smear     = bold ? std::max(1.0f, std::round(lh * 0.05f)) : 0.0f; // the second strike, in physical pixels
    const f32  asc       = atlas_->ascent_px(font);

    const u32 quads      = bold ? 2u : 1u;
    const u32 max_glyphs = static_cast<u32>(s.size()); // utf-8 bytes >= code points
    prim p;
    if (!reserve(4 * quads * max_glyphs, 6 * quads * max_glyphs, p)) {
        return;
    }

    u32 nv = 0;
    u32 ni = 0;

    // the lines underline / strike-through are drawn along (physical pixels), collected while the vertices are the tail
    struct span_line { f32 x0, x1, base; };
    std::array<span_line, 16> spans{};
    u32 span_count = 0;
    const auto close_line = [&](f32 x0, f32 x1, f32 base) {
        if ((underline || strike) && x1 > x0 && span_count < spans.size()) { spans[span_count++] = {x0, x1, base}; }
    };

    const rect& clip = phys_clip_;
    const f32 origin_x = std::round(p0.x);
    vec2      pen{origin_x, std::round(p0.y)};
    char32_t  prev = 0;

    while (!s.empty()) {
        const char32_t cp = decode_utf8(s);
        if (cp == U'\n') {
            close_line(origin_x, pen.x, pen.y + asc);
            pen.x = origin_x;
            pen.y += lh;
            prev = 0;
            continue;
        }

        if (prev != 0) {
            pen.x += atlas_->kerning_px(font, prev, cp);
        }
        prev = cp;

        const glyph& g = atlas_->find(font, cp);
        if (g.visible) {
            const f32 x0 = pen.x + g.x0;
            const f32 y0 = pen.y + g.y0;
            const f32 x1 = pen.x + g.x1;
            const f32 y1 = pen.y + g.y1;
            const f32 base = pen.y + asc;
            const f32 top_shift    = (base - y0) * slant; // the slant is anchored on the baseline
            const f32 bottom_shift = (base - y1) * slant;

            if (x1 + top_shift + smear >= clip.min.x && x0 + bottom_shift <= clip.max.x && y1 >= clip.min.y && y0 <= clip.max.y) {
                for (u32 pass = 0; pass < quads; ++pass) {
                    const f32 dx = static_cast<f32>(pass) * smear;
                    vertex* v = p.v + nv;
                    v[0] = {{x0 + top_shift + dx, y0}, g.u0, g.v0, c};
                    v[1] = {{x1 + top_shift + dx, y0}, g.u1, g.v0, c};
                    v[2] = {{x1 + bottom_shift + dx, y1}, g.u1, g.v1, c};
                    v[3] = {{x0 + bottom_shift + dx, y1}, g.u0, g.v1, c};

                    const u32 base_index = p.base + nv;
                    u32* idx = p.i + ni;
                    idx[0] = base_index; idx[1] = base_index + 1; idx[2] = base_index + 2;
                    idx[3] = base_index; idx[4] = base_index + 2; idx[5] = base_index + 3;

                    nv += 4;
                    ni += 6;
                }
            }
        }
        pen.x += g.advance;
    }
    close_line(origin_x, pen.x + smear, pen.y + asc);

    // give back what culling / whitespace didn't use (this reservation is the tail)
    vertices_.shrink(4 * quads * max_glyphs - nv);
    indices_.shrink(6 * quads * max_glyphs - ni);
    commands_.back().idx_count -= 6 * quads * max_glyphs - ni;

    const f32 thick = std::max(1.0f, std::round(lh * 0.055f));
    for (u32 k = 0; k < span_count; ++k) {
        const span_line& l = spans[k];
        const auto bar = [&](f32 y) {
            rect_filled({{l.x0 / scale_, y / scale_}, {l.x1 / scale_, (y + thick) / scale_}}, plain);
        };
        if (underline) { bar(l.base + std::max(1.0f, std::round(lh * 0.1f))); }
        if (strike)    { bar(l.base - std::round(asc * 0.32f)); }
    }
}

} // namespace strata

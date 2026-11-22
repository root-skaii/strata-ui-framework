#include "font_ttf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace strata::internal {

namespace {

[[nodiscard]] bool read_u16(std::span<const u8> d, std::size_t off, u16& out) noexcept
{
    if (off + 2 > d.size()) { return false; }
    out = static_cast<u16>((u16{d[off]} << 8) | d[off + 1]);
    return true;
}

[[nodiscard]] bool read_i16(std::span<const u8> d, std::size_t off, i16& out) noexcept
{
    u16 v{};
    if (!read_u16(d, off, v)) { return false; }
    out = static_cast<i16>(v);
    return true;
}

[[nodiscard]] bool read_u32(std::span<const u8> d, std::size_t off, u32& out) noexcept
{
    if (off + 4 > d.size()) { return false; }
    out = (u32{d[off]} << 24) | (u32{d[off + 1]} << 16) | (u32{d[off + 2]} << 8) | d[off + 3];
    return true;
}

[[nodiscard]] constexpr u32 make_tag(char a, char b, char c, char d) noexcept
{
    return (u32{static_cast<u8>(a)} << 24) | (u32{static_cast<u8>(b)} << 16) | (u32{static_cast<u8>(c)} << 8) |
           static_cast<u8>(d);
}

struct point {
    f32  x{}, y{};
    bool on_curve{};
};

void flatten_quad(f32 x0, f32 y0, f32 cx, f32 cy, f32 x1, f32 y1, auto&& emit, int depth = 0)
{
    const f32 mx = 0.5f * (x0 + x1) - cx;
    const f32 my = 0.5f * (y0 + y1) - cy;
    if (depth >= 8 || (mx * mx + my * my) <= 0.12f) {
        emit(x0, y0, x1, y1);
        return;
    }
    const f32 x01 = 0.5f * (x0 + cx), y01 = 0.5f * (y0 + cy);
    const f32 x12 = 0.5f * (cx + x1), y12 = 0.5f * (cy + y1);
    const f32 xm  = 0.5f * (x01 + x12), ym = 0.5f * (y01 + y12);
    flatten_quad(x0, y0, x01, y01, xm, ym, emit, depth + 1);
    flatten_quad(xm, ym, x12, y12, x1, y1, emit, depth + 1);
}

} // namespace

bool ttf_font::init(std::span<const u8> bytes, u32 face_index) noexcept
{
    *this = {};
    data_ = bytes;
    if (bytes.size() < 12) { return false; }

    u32 sfnt = 0;
    u32 tag0{};
    if (!read_u32(bytes, 0, tag0)) { return false; }
    if (tag0 == make_tag('t', 't', 'c', 'f')) {
        u32 num_fonts{};
        if (!read_u32(bytes, 8, num_fonts) || face_index >= num_fonts) { return false; }
        if (!read_u32(bytes, 12 + static_cast<std::size_t>(face_index) * 4, sfnt)) { return false; }
    } else if (tag0 != 0x00010000u && tag0 != make_tag('t', 'r', 'u', 'e')) {
        return false; // CFF/OTTO outlines are not TrueType glyf
    }

    u16 num_tables{};
    if (!read_u16(bytes, static_cast<std::size_t>(sfnt) + 4, num_tables)) { return false; }

    std::size_t head{}, hhea{}, maxp{}, cmap{};
    for (u16 i = 0; i < num_tables; ++i) {
        const std::size_t rec = static_cast<std::size_t>(sfnt) + 12 + static_cast<std::size_t>(i) * 16;
        u32 tag{}, off{}, len{};
        if (!read_u32(bytes, rec, tag) || !read_u32(bytes, rec + 8, off) || !read_u32(bytes, rec + 12, len)) { return false; }
        if (static_cast<std::size_t>(off) + len > bytes.size()) { continue; }
        if      (tag == make_tag('h', 'e', 'a', 'd')) { head = off; }
        else if (tag == make_tag('h', 'h', 'e', 'a')) { hhea = off; }
        else if (tag == make_tag('m', 'a', 'x', 'p')) { maxp = off; }
        else if (tag == make_tag('c', 'm', 'a', 'p')) { cmap = off; }
        else if (tag == make_tag('g', 'l', 'y', 'f')) { glyf_ = off; glyf_len_ = len; }
        else if (tag == make_tag('l', 'o', 'c', 'a')) { loca_ = off; loca_len_ = len; }
        else if (tag == make_tag('h', 'm', 't', 'x')) { hmtx_ = off; hmtx_len_ = len; }
        else if (tag == make_tag('k', 'e', 'r', 'n')) { kern_ = off; kern_len_ = len; }
    }
    if (head == 0 || hhea == 0 || maxp == 0 || cmap == 0 || glyf_ == 0 || loca_ == 0 || hmtx_ == 0) {
        return false;
    }

    i16 loc_fmt{};
    if (!read_u16(bytes, head + 18, units_per_em_) || units_per_em_ == 0 || !read_i16(bytes, head + 50, loc_fmt)) {
        return false;
    }
    loca_long_ = loc_fmt != 0;

    if (!read_i16(bytes, hhea + 4, ascent_) || !read_i16(bytes, hhea + 6, descent_) || !read_i16(bytes, hhea + 8, line_gap_) ||
        !read_u16(bytes, hhea + 34, num_hmetrics_) || num_hmetrics_ == 0) {
        return false;
    }
    if (!read_u16(bytes, maxp + 4, num_glyphs_) || num_glyphs_ == 0) { return false; }

    u16 sub_count{};
    if (!read_u16(bytes, cmap + 2, sub_count)) { return false; }
    for (u16 i = 0; i < sub_count; ++i) {
        const std::size_t rec = cmap + 4 + static_cast<std::size_t>(i) * 8;
        u16 plat{}, enc{}, fmt{};
        u32 sub_off{};
        if (!read_u16(bytes, rec, plat) || !read_u16(bytes, rec + 2, enc) || !read_u32(bytes, rec + 4, sub_off)) { continue; }
        const std::size_t at = cmap + sub_off;
        if (!read_u16(bytes, at, fmt)) { continue; }
        const bool unicode_bmp  = (plat == 3 && enc == 1) || (plat == 0 && (enc == 3 || enc == 4));
        const bool unicode_full = (plat == 3 && enc == 10) || (plat == 0 && (enc == 4 || enc == 6));
        if (fmt == 4 && unicode_bmp && cmap_fmt4_ == 0)   { cmap_fmt4_  = at; }
        if (fmt == 12 && unicode_full && cmap_fmt12_ == 0) { cmap_fmt12_ = at; }
    }
    if (cmap_fmt4_ == 0 && cmap_fmt12_ == 0) { return false; }

    valid_ = true;
    return true;
}

f32 ttf_font::scale_for_pixel_height(f32 pixel_height_px) const noexcept
{
    const f32 span = static_cast<f32>(ascent_ - descent_);
    return span > 0.0f ? pixel_height_px / span : pixel_height_px / static_cast<f32>(units_per_em_);
}

u16 ttf_font::glyph_index(char32_t cp) const noexcept
{
    if (!valid_) { return 0; }
    if (cmap_fmt12_ != 0) {
        u32 n_groups{};
        if (read_u32(data_, cmap_fmt12_ + 12, n_groups) &&
            cmap_fmt12_ + 16 + static_cast<std::size_t>(n_groups) * 12 <= data_.size()) {
            u32 lo = 0, hi = n_groups;
            while (lo < hi) {
                const u32 mid = lo + (hi - lo) / 2;
                const std::size_t g = cmap_fmt12_ + 16 + static_cast<std::size_t>(mid) * 12;
                u32 start{}, end{}, gid{};
                (void)read_u32(data_, g, start);
                (void)read_u32(data_, g + 4, end);
                if (cp < start)    { hi = mid; }
                else if (cp > end) { lo = mid + 1; }
                else {
                    (void)read_u32(data_, g + 8, gid);
                    const u32 out = gid + (cp - start);
                    return out < num_glyphs_ ? static_cast<u16>(out) : u16{0};
                }
            }
        }
    }
    if (cmap_fmt4_ != 0 && cp <= 0xffffu) {
        u16 seg_count_x2{};
        if (!read_u16(data_, cmap_fmt4_ + 6, seg_count_x2) || (seg_count_x2 & 1u) != 0 || seg_count_x2 == 0) { return 0; }
        const std::size_t seg_count = seg_count_x2 / 2;
        const std::size_t end_off   = cmap_fmt4_ + 14;
        const std::size_t start_off = end_off + seg_count * 2 + 2;
        const std::size_t delta_off = start_off + seg_count * 2;
        const std::size_t range_off = delta_off + seg_count * 2;
        if (range_off + seg_count * 2 > data_.size()) { return 0; }

        std::size_t lo = 0, hi = seg_count;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            u16 end_cp{};
            (void)read_u16(data_, end_off + mid * 2, end_cp);
            if (cp > end_cp) { lo = mid + 1; }
            else             { hi = mid; }
        }
        if (lo >= seg_count) { return 0; }
        u16 start_cp{}, end_cp{}, delta{}, range{};
        (void)read_u16(data_, start_off + lo * 2, start_cp);
        (void)read_u16(data_, end_off + lo * 2, end_cp);
        if (cp < start_cp || cp > end_cp) { return 0; }
        (void)read_u16(data_, delta_off + lo * 2, delta);
        (void)read_u16(data_, range_off + lo * 2, range);
        u16 gid = 0;
        if (range == 0) {
            gid = static_cast<u16>(cp + delta);
        } else {
            const std::size_t glyph_id_at = range_off + lo * 2 + range + (cp - start_cp) * 2;
            if (!read_u16(data_, glyph_id_at, gid) || gid == 0) { return 0; }
            gid = static_cast<u16>(gid + delta);
        }
        return gid < num_glyphs_ ? gid : u16{0};
    }
    return 0;
}

f32 ttf_font::advance_units(u16 gid) const noexcept
{
    const u16 idx = std::min<u16>(gid, static_cast<u16>(num_hmetrics_ - 1));
    u16 adv{};
    if (static_cast<std::size_t>(idx) * 4 + 2 > hmtx_len_ || !read_u16(data_, hmtx_ + static_cast<std::size_t>(idx) * 4, adv)) {
        return 0.0f;
    }
    return static_cast<f32>(adv);
}

bool ttf_font::glyph_offset(u16 gid, std::size_t& start, std::size_t& end) const noexcept
{
    if (gid >= num_glyphs_) { return false; }
    if (loca_long_) {
        const std::size_t at = static_cast<std::size_t>(gid) * 4;
        u32 a{}, b{};
        if (at + 8 > loca_len_ || !read_u32(data_, loca_ + at, a) || !read_u32(data_, loca_ + at + 4, b)) { return false; }
        if (a > b || b > glyf_len_) { return false; }
        start = glyf_ + a;
        end   = glyf_ + b;
        return true;
    }
    const std::size_t at = static_cast<std::size_t>(gid) * 2;
    u16 a{}, b{};
    if (at + 4 > loca_len_ || !read_u16(data_, loca_ + at, a) || !read_u16(data_, loca_ + at + 2, b)) { return false; }
    const std::size_t sa = static_cast<std::size_t>(a) * 2;
    const std::size_t sb = static_cast<std::size_t>(b) * 2;
    if (sa > sb || sb > glyf_len_) { return false; }
    start = glyf_ + sa;
    end   = glyf_ + sb;
    return true;
}

bool ttf_font::load_outline(u16 gid, f32 m00, f32 m01, f32 m10, f32 m11, f32 tx, f32 ty,
                            std::vector<edge>& edges, int depth) const
{
    if (depth > 8) { return false; }
    std::size_t start{}, end{};
    if (!glyph_offset(gid, start, end)) { return false; }
    if (start == end) { return true; } // whitespace glyph
    if (end - start < 10) { return false; }

    i16 num_contours{};
    if (!read_i16(data_, start, num_contours)) { return false; }

    const auto xform = [&](f32 x, f32 y) -> std::pair<f32, f32> {
        return {m00 * x + m01 * y + tx, m10 * x + m11 * y + ty};
    };

    if (num_contours > 0) {
        const std::size_t nc = static_cast<std::size_t>(num_contours);
        std::size_t pos = start + 10;
        if (pos + nc * 2 + 2 > end) { return false; }
        std::vector<u16> contour_ends(nc);
        u16 last_pt = 0;
        for (std::size_t i = 0; i < nc; ++i) {
            (void)read_u16(data_, pos + i * 2, contour_ends[i]);
            if (i > 0 && contour_ends[i] < contour_ends[i - 1]) { return false; }
            last_pt = contour_ends[i];
        }
        pos += nc * 2;
        u16 ins_len{};
        (void)read_u16(data_, pos, ins_len);
        pos += 2 + ins_len;
        if (pos > end) { return false; }

        const std::size_t npts = static_cast<std::size_t>(last_pt) + 1;
        if (npts > 16384) { return false; }
        std::vector<u8> flags(npts);
        for (std::size_t i = 0; i < npts; ) {
            if (pos >= end) { return false; }
            const u8 fl = data_[pos++];
            flags[i++] = fl;
            if ((fl & 8u) != 0) {
                if (pos >= end) { return false; }
                const u8 rep = data_[pos++];
                for (u8 r = 0; r < rep && i < npts; ++r) { flags[i++] = fl; }
            }
        }

        std::vector<point> pts(npts);
        i32 cur = 0;
        for (std::size_t i = 0; i < npts; ++i) {
            const u8 fl = flags[i];
            if ((fl & 2u) != 0) {
                if (pos >= end) { return false; }
                const i32 dx = data_[pos++];
                cur += (fl & 16u) != 0 ? dx : -dx;
            } else if ((fl & 16u) == 0) {
                i16 dx{};
                if (pos + 2 > end || !read_i16(data_, pos, dx)) { return false; }
                pos += 2;
                cur += dx;
            }
            pts[i].x        = static_cast<f32>(cur);
            pts[i].on_curve = (fl & 1u) != 0;
        }
        cur = 0;
        for (std::size_t i = 0; i < npts; ++i) {
            const u8 fl = flags[i];
            if ((fl & 4u) != 0) {
                if (pos >= end) { return false; }
                const i32 dy = data_[pos++];
                cur += (fl & 32u) != 0 ? dy : -dy;
            } else if ((fl & 32u) == 0) {
                i16 dy{};
                if (pos + 2 > end || !read_i16(data_, pos, dy)) { return false; }
                pos += 2;
                cur += dy;
            }
            const auto [sx, sy] = xform(pts[i].x, static_cast<f32>(cur));
            pts[i].x = sx;
            pts[i].y = sy;
        }

        const auto emit = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
            if (y0 != y1 && edges.size() < 65536) { edges.push_back({x0, y0, x1, y1}); }
        };

        std::size_t first = 0;
        for (std::size_t c = 0; c < nc; ++c) {
            const std::size_t last = contour_ends[c];
            const std::size_t cnt  = last - first + 1;
            if (cnt >= 2) {
                // find starting on-curve point (or synthetic midpoint between last and first)
                f32 sx{}, sy{};
                std::size_t start_idx = 0;
                if (pts[first].on_curve) {
                    sx = pts[first].x; sy = pts[first].y; start_idx = 1;
                } else if (pts[last].on_curve) {
                    sx = pts[last].x; sy = pts[last].y; start_idx = 0;
                } else {
                    sx = 0.5f * (pts[last].x + pts[first].x);
                    sy = 0.5f * (pts[last].y + pts[first].y);
                    start_idx = 0;
                }
                f32 px = sx, py = sy;
                bool has_ctrl = false;
                f32  cx{}, cy{};
                for (std::size_t k = 0; k < cnt; ++k) {
                    const point& p = pts[first + ((start_idx + k) % cnt)];
                    if (p.on_curve) {
                        if (has_ctrl) {
                            flatten_quad(px, py, cx, cy, p.x, p.y, emit);
                            has_ctrl = false;
                        } else {
                            emit(px, py, p.x, p.y);
                        }
                        px = p.x; py = p.y;
                    } else {
                        if (has_ctrl) {
                            const f32 mx = 0.5f * (cx + p.x);
                            const f32 my = 0.5f * (cy + p.y);
                            flatten_quad(px, py, cx, cy, mx, my, emit);
                            px = mx; py = my;
                        }
                        cx = p.x; cy = p.y; has_ctrl = true;
                    }
                }
                if (has_ctrl) {
                    flatten_quad(px, py, cx, cy, sx, sy, emit);
                } else if (px != sx || py != sy) {
                    emit(px, py, sx, sy);
                }
            }
            first = last + 1;
        }
        return true;
    }

    if (num_contours < 0) { // compound glyph
        std::size_t pos = start + 10;
        u16 flags = 0x20u;
        int parts = 0;
        while ((flags & 0x20u) != 0 && ++parts <= 128 && edges.size() < 65536) {
            u16 comp_gid{};
            if (pos + 4 > end || !read_u16(data_, pos, flags) || !read_u16(data_, pos + 2, comp_gid)) { return false; }
            pos += 4;
            f32 dx = 0.0f, dy = 0.0f;
            if ((flags & 1u) != 0) { // ARG_1_AND_2_ARE_WORDS
                i16 a{}, b{};
                if (pos + 4 > end || !read_i16(data_, pos, a) || !read_i16(data_, pos + 2, b)) { return false; }
                pos += 4;
                if ((flags & 2u) != 0) { dx = static_cast<f32>(a); dy = static_cast<f32>(b); }
            } else {
                if (pos + 2 > end) { return false; }
                const i8 a = static_cast<i8>(data_[pos++]);
                const i8 b = static_cast<i8>(data_[pos++]);
                if ((flags & 2u) != 0) { dx = static_cast<f32>(a); dy = static_cast<f32>(b); }
            }
            f32 c00 = 1.0f, c01 = 0.0f, c10 = 0.0f, c11 = 1.0f;
            if ((flags & 8u) != 0) { // WE_HAVE_A_SCALE
                i16 s{};
                if (pos + 2 > end || !read_i16(data_, pos, s)) { return false; }
                pos += 2;
                c00 = c11 = static_cast<f32>(s) / 16384.0f;
            } else if ((flags & 64u) != 0) { // WE_HAVE_AN_X_AND_Y_SCALE
                i16 sx{}, sy{};
                if (pos + 4 > end || !read_i16(data_, pos, sx) || !read_i16(data_, pos + 2, sy)) { return false; }
                pos += 4;
                c00 = static_cast<f32>(sx) / 16384.0f;
                c11 = static_cast<f32>(sy) / 16384.0f;
            } else if ((flags & 128u) != 0) { // WE_HAVE_A_TWO_BY_TWO
                i16 a{}, b{}, c{}, d{};
                if (pos + 8 > end || !read_i16(data_, pos, a) || !read_i16(data_, pos + 2, b) ||
                    !read_i16(data_, pos + 4, c) || !read_i16(data_, pos + 6, d)) { return false; }
                pos += 8;
                c00 = static_cast<f32>(a) / 16384.0f;
                c10 = static_cast<f32>(b) / 16384.0f;
                c01 = static_cast<f32>(c) / 16384.0f;
                c11 = static_cast<f32>(d) / 16384.0f;
            }
            const f32 n00 = m00 * c00 + m01 * c10;
            const f32 n01 = m00 * c01 + m01 * c11;
            const f32 n10 = m10 * c00 + m11 * c10;
            const f32 n11 = m10 * c01 + m11 * c11;
            const f32 ntx = m00 * dx + m01 * dy + tx;
            const f32 nty = m10 * dx + m11 * dy + ty;
            if (!load_outline(comp_gid, n00, n01, n10, n11, ntx, nty, edges, depth + 1)) { return false; }
        }
        return true;
    }
    return true;
}

bool ttf_font::rasterise(u16 gid, f32 scale, ttf_bitmap& out, bool need_pixels) const
{
    out = {};
    if (!valid_ || gid >= num_glyphs_) { return false; }
    out.advance = std::round(advance_units(gid) * scale);

    std::vector<edge> edges;
    // flip y: font space is +y up, bitmap space is +y down
    if (!load_outline(gid, scale, 0.0f, 0.0f, -scale, 0.0f, 0.0f, edges, 0)) { return false; }
    if (edges.empty()) { return true; }

    f32 min_x = edges[0].x0, max_x = edges[0].x0;
    f32 min_y = edges[0].y0, max_y = edges[0].y0;
    for (const edge& e : edges) {
        min_x = std::min({min_x, e.x0, e.x1});
        max_x = std::max({max_x, e.x0, e.x1});
        min_y = std::min({min_y, e.y0, e.y1});
        max_y = std::max({max_y, e.y0, e.y1});
    }
    if (!std::isfinite(min_x) || !std::isfinite(max_x) || !std::isfinite(min_y) || !std::isfinite(max_y) ||
        (max_x - min_x) > 2048.0f || (max_y - min_y) > 2048.0f ||
        std::abs(min_x) > 1.0e6f || std::abs(min_y) > 1.0e6f) {
        return true;
    }
    const i32 ix0 = static_cast<i32>(std::floor(min_x));
    const i32 iy0 = static_cast<i32>(std::floor(min_y));
    const i32 ix1 = static_cast<i32>(std::ceil(max_x));
    const i32 iy1 = static_cast<i32>(std::ceil(max_y));
    if (ix1 <= ix0 || iy1 <= iy0 || (ix1 - ix0) > 2048 || (iy1 - iy0) > 2048) { return true; }

    const u32 w = static_cast<u32>(ix1 - ix0);
    const u32 h = static_cast<u32>(iy1 - iy0);
    out.w        = static_cast<u16>(w);
    out.h        = static_cast<u16>(h);
    out.origin_x = ix0;
    out.origin_y = -iy0; // top bearing above baseline (+y up)
    if (!need_pixels) { return true; }

    // shift edges into [0, w] x [0, h] and orient top-to-bottom
    struct seg {
        f32 x0{}, y0{}, x1{}, y1{}, inv_dy{}, dir{};
    };
    std::vector<seg> segs;
    segs.reserve(edges.size());
    const f32 ox = static_cast<f32>(ix0);
    const f32 oy = static_cast<f32>(iy0);
    for (const edge& e : edges) {
        f32 ax = e.x0 - ox, ay = e.y0 - oy;
        f32 bx = e.x1 - ox, by = e.y1 - oy;
        f32 dir = 1.0f;
        if (ay > by) {
            std::swap(ax, bx);
            std::swap(ay, by);
            dir = -1.0f;
        }
        const f32 dy = by - ay;
        if (dy > 1.0e-6f) { segs.push_back({ax, ay, bx, by, (bx - ax) / dy, dir}); }
    }

    out.pixels.assign(static_cast<std::size_t>(w) * h, 0);
    std::vector<f32> cover(w + 1);
    std::vector<f32> delta(w + 1);

    const auto add_piece = [&](f32 x_a, f32 x_b, f32 piece_dy, f32 dir) {
        const f32 lo = std::min(x_a, x_b);
        const f32 hi = std::max(x_a, x_b);
        const i32 c0 = static_cast<i32>(std::floor(lo));
        const i32 c1 = static_cast<i32>(std::floor(hi));
        if (c0 == c1) {
            if (c0 < 0) {
                delta[0] += piece_dy * dir;
            } else if (c0 < static_cast<i32>(w)) {
                const f32 xm = 0.5f * (lo + hi);
                cover[static_cast<std::size_t>(c0)] += (static_cast<f32>(c0 + 1) - xm) * piece_dy * dir;
                delta[static_cast<std::size_t>(c0 + 1)] += piece_dy * dir;
            }
            return;
        }
        const f32 dx = hi - lo;
        const f32 inv_dx = 1.0f / dx;
        for (i32 cx = c0; cx <= c1; ++cx) {
            const f32 xa = std::max(lo, static_cast<f32>(cx));
            const f32 xb = std::min(hi, static_cast<f32>(cx + 1));
            const f32 sub_dy = piece_dy * (xb - xa) * inv_dx;
            if (cx < 0) {
                delta[0] += sub_dy * dir;
            } else if (cx < static_cast<i32>(w)) {
                const f32 xm = 0.5f * (xa + xb);
                cover[static_cast<std::size_t>(cx)] += (static_cast<f32>(cx + 1) - xm) * sub_dy * dir;
                delta[static_cast<std::size_t>(cx + 1)] += sub_dy * dir;
            }
        }
    };

    for (u32 row = 0; row < h; ++row) {
        std::fill(cover.begin(), cover.end(), 0.0f);
        std::fill(delta.begin(), delta.end(), 0.0f);
        const f32 y_top = static_cast<f32>(row);
        const f32 y_bot = y_top + 1.0f;

        for (const seg& s : segs) {
            if (s.y1 <= y_top || s.y0 >= y_bot) { continue; }
            const f32 sy0 = std::max(s.y0, y_top);
            const f32 sy1 = std::min(s.y1, y_bot);
            const f32 sx0 = s.x0 + (sy0 - s.y0) * s.inv_dy;
            const f32 sx1 = s.x0 + (sy1 - s.y0) * s.inv_dy;
            add_piece(sx0, sx1, sy1 - sy0, s.dir);
        }

        f32 acc = 0.0f;
        u8* dst = out.pixels.data() + static_cast<std::size_t>(row) * w;
        for (u32 x = 0; x < w; ++x) {
            acc += delta[x];
            const f32 cov = std::min(std::abs(acc + cover[x]), 1.0f);
            dst[x] = static_cast<u8>(cov * 255.0f + 0.5f);
        }
    }
    return true;
}

void ttf_font::collect_kerning(f32 scale, std::vector<ttf_kern_pair>& out) const
{
    if (!valid_ || kern_ == 0 || kern_len_ < 4) { return; }
    u16 version{}, n_tables{};
    if (!read_u16(data_, kern_, version) || !read_u16(data_, kern_ + 2, n_tables) || version != 0) { return; }

    // build reverse map: glyph id -> BMP code point (for ASCII/BMP kerning lookup)
    std::vector<u16> gid_to_cp(num_glyphs_, 0);
    for (char32_t cp = 0x20; cp <= 0x024f; ++cp) {
        const u16 g = glyph_index(cp);
        if (g != 0 && gid_to_cp[g] == 0) { gid_to_cp[g] = static_cast<u16>(cp); }
    }

    std::size_t pos = kern_ + 4;
    const std::size_t end = kern_ + kern_len_;
    for (u16 t = 0; t < n_tables && pos + 6 <= end; ++t) {
        u16 len{}, cov{};
        if (!read_u16(data_, pos + 2, len) || !read_u16(data_, pos + 4, cov) || len < 6 || pos + len > end) { break; }
        const u8 format = static_cast<u8>(cov >> 8);
        const bool horiz = (cov & 1u) != 0 && (cov & 6u) == 0;
        if (format == 0 && horiz && pos + 14 <= end) {
            u16 n_pairs{};
            (void)read_u16(data_, pos + 6, n_pairs);
            std::size_t p = pos + 14;
            for (u16 i = 0; i < n_pairs && p + 6 <= pos + len; ++i, p += 6) {
                u16 left{}, right{};
                i16 value{};
                (void)read_u16(data_, p, left);
                (void)read_u16(data_, p + 2, right);
                (void)read_i16(data_, p + 4, value);
                if (value == 0 || left >= num_glyphs_ || right >= num_glyphs_) { continue; }
                const u16 lcp = gid_to_cp[left];
                const u16 rcp = gid_to_cp[right];
                if (lcp == 0 || rcp == 0) { continue; }
                const f32 px = std::round(static_cast<f32>(value) * scale);
                if (px != 0.0f) { out.push_back({(u32{lcp} << 16) | rcp, px}); }
            }
        }
        pos += len;
    }
}

} // namespace strata::internal

#include "strata/svg.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>

namespace strata {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

void skip_ws_comma(std::string_view& s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r' || s.front() == ',')) {
        s.remove_prefix(1);
    }
}

[[nodiscard]] bool read_float(std::string_view& s, f32& out) noexcept
{
    skip_ws_comma(s);
    if (s.empty()) { return false; }
    // std::from_chars does not accept leading '+'
    if (s.front() == '+') { s.remove_prefix(1); }
    const char* begin = s.data();
    const char* end   = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr == begin) { return false; }
    s.remove_prefix(static_cast<std::size_t>(ptr - begin));
    return true;
}

[[nodiscard]] bool read_flag(std::string_view& s, int& out) noexcept
{
    skip_ws_comma(s);
    if (s.empty() || (s.front() != '0' && s.front() != '1')) { return false; }
    out = s.front() - '0';
    s.remove_prefix(1);
    return true;
}

void flatten_cubic(std::vector<vec2>& pts, vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 flat_sq, int depth = 0)
{
    const vec2 d1 = p1 - p0;
    const vec2 d2 = p2 - p0;
    const vec2 d3 = p3 - p0;
    const f32 len_sq = dot(d3, d3);
    f32 err_sq = 0.0f;
    if (len_sq > 1.0e-6f) {
        const f32 c1 = d1.x * d3.y - d1.y * d3.x;
        const f32 c2 = d2.x * d3.y - d2.y * d3.x;
        err_sq = std::max(c1 * c1, c2 * c2) / len_sq;
    } else {
        err_sq = std::max(dot(d1, d1), dot(d2, d2));
    }
    if (err_sq <= flat_sq || depth >= 8) {
        pts.push_back(p3);
        return;
    }
    const vec2 p01 = (p0 + p1) * 0.5f;
    const vec2 p12 = (p1 + p2) * 0.5f;
    const vec2 p23 = (p2 + p3) * 0.5f;
    const vec2 p012 = (p01 + p12) * 0.5f;
    const vec2 p123 = (p12 + p23) * 0.5f;
    const vec2 mid  = (p012 + p123) * 0.5f;
    flatten_cubic(pts, p0, p01, p012, mid, flat_sq, depth + 1);
    flatten_cubic(pts, mid, p123, p23, p3, flat_sq, depth + 1);
}

void flatten_arc(std::vector<vec2>& pts, vec2 p0, f32 rx, f32 ry, f32 xrot_deg, int large_arc, int sweep, vec2 p1, f32 flatness)
{
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx < 1.0e-4f || ry < 1.0e-4f || (std::abs(p1.x - p0.x) < 1.0e-5f && std::abs(p1.y - p0.y) < 1.0e-5f)) {
        pts.push_back(p1);
        return;
    }
    const f32 phi = xrot_deg * (kPi / 180.0f);
    const f32 cphi = std::cos(phi), sphi = std::sin(phi);
    const vec2 mid = (p0 - p1) * 0.5f;
    const f32 x1p = cphi * mid.x + sphi * mid.y;
    const f32 y1p = -sphi * mid.x + cphi * mid.y;

    const f32 lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0f) {
        const f32 s = std::sqrt(lam);
        rx *= s;
        ry *= s;
    }
    const f32 rx2 = rx * rx, ry2 = ry * ry;
    const f32 num = std::max(0.0f, rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p);
    const f32 den = std::max(rx2 * y1p * y1p + ry2 * x1p * x1p, 1.0e-9f);
    const f32 coef = (large_arc == sweep ? -1.0f : 1.0f) * std::sqrt(num / den);
    const f32 cxp = coef * (rx * y1p / ry);
    const f32 cyp = coef * (-ry * x1p / rx);
    const vec2 c{cphi * cxp - sphi * cyp + (p0.x + p1.x) * 0.5f,
                 sphi * cxp + cphi * cyp + (p0.y + p1.y) * 0.5f};

    const auto angle_between = [](vec2 u, vec2 v) {
        const f32 d = std::clamp(dot(u, v) / std::max(std::hypot(u.x, u.y) * std::hypot(v.x, v.y), 1.0e-9f), -1.0f, 1.0f);
        const f32 a = std::acos(d);
        return (u.x * v.y - u.y * v.x) < 0.0f ? -a : a;
    };
    const vec2 v1{(x1p - cxp) / rx, (y1p - cyp) / ry};
    const vec2 v2{(-x1p - cxp) / rx, (-y1p - cyp) / ry};
    const f32 th0 = angle_between({1.0f, 0.0f}, v1);
    f32 dth = angle_between(v1, v2);
    if (sweep == 0 && dth > 0.0f) { dth -= 2.0f * kPi; }
    if (sweep != 0 && dth < 0.0f) { dth += 2.0f * kPi; }

    const f32 max_r = std::max(rx, ry);
    const f32 step_a = std::acos(std::clamp(1.0f - flatness / std::max(max_r, 1.0f), -1.0f, 1.0f)) * 2.0f;
    const int segs = std::clamp(static_cast<int>(std::ceil(std::abs(dth) / std::max(step_a, 0.05f))), 4, 64);
    for (int i = 1; i <= segs; ++i) {
        const f32 a = th0 + dth * (static_cast<f32>(i) / static_cast<f32>(segs));
        const f32 ex = rx * std::cos(a);
        const f32 ey = ry * std::sin(a);
        pts.push_back({c.x + cphi * ex - sphi * ey, c.y + sphi * ex + cphi * ey});
    }
}

struct edge {
    f32 x0{}, y0{}, x1{}, y1{};
    int dir{};
};

} // namespace

std::vector<svg_contour> svg_parse_path(std::string_view d, f32 flatness)
{
    std::vector<svg_contour> out;
    const f32 flat_sq = std::max(flatness, 0.02f) * std::max(flatness, 0.02f);

    vec2 cur{};
    vec2 start{};
    vec2 last_ctrl{};
    char cmd = 0;
    char prev_cmd = 0;

    auto ensure_contour = [&]() -> svg_contour& {
        if (out.empty() || out.back().closed) {
            out.push_back({});
            out.back().points.push_back(cur);
        }
        return out.back();
    };

    while (true) {
        skip_ws_comma(d);
        if (d.empty()) { break; }
        const char ch = d.front();
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            cmd = ch;
            d.remove_prefix(1);
        } else if (cmd == 0) {
            break;
        }
        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const char u   = rel ? static_cast<char>(cmd - 'a' + 'A') : cmd;

        if (u == 'Z') {
            if (!out.empty()) {
                out.back().closed = true;
                cur = start;
            }
            prev_cmd = u;
            cmd = 0;
            continue;
        }

        if (u == 'M') {
            f32 x{}, y{};
            if (!read_float(d, x) || !read_float(d, y)) { break; }
            cur   = rel ? vec2{cur.x + x, cur.y + y} : vec2{x, y};
            start = cur;
            out.push_back({});
            out.back().points.push_back(cur);
            cmd      = rel ? 'l' : 'L'; // subsequent coordinate pairs are implicit LineTo
            prev_cmd = 'M';
        } else if (u == 'L') {
            f32 x{}, y{};
            if (!read_float(d, x) || !read_float(d, y)) { break; }
            cur = rel ? vec2{cur.x + x, cur.y + y} : vec2{x, y};
            ensure_contour().points.push_back(cur);
            prev_cmd = 'L';
        } else if (u == 'H') {
            f32 x{};
            if (!read_float(d, x)) { break; }
            cur.x = rel ? cur.x + x : x;
            ensure_contour().points.push_back(cur);
            prev_cmd = 'H';
        } else if (u == 'V') {
            f32 y{};
            if (!read_float(d, y)) { break; }
            cur.y = rel ? cur.y + y : y;
            ensure_contour().points.push_back(cur);
            prev_cmd = 'V';
        } else if (u == 'C') {
            f32 x1{}, y1{}, x2{}, y2{}, x{}, y{};
            if (!read_float(d, x1) || !read_float(d, y1) || !read_float(d, x2) || !read_float(d, y2) ||
                !read_float(d, x) || !read_float(d, y)) { break; }
            const vec2 c1 = rel ? cur + vec2{x1, y1} : vec2{x1, y1};
            const vec2 c2 = rel ? cur + vec2{x2, y2} : vec2{x2, y2};
            const vec2 p  = rel ? cur + vec2{x, y}   : vec2{x, y};
            flatten_cubic(ensure_contour().points, cur, c1, c2, p, flat_sq);
            last_ctrl = c2;
            cur       = p;
            prev_cmd  = 'C';
        } else if (u == 'S') {
            f32 x2{}, y2{}, x{}, y{};
            if (!read_float(d, x2) || !read_float(d, y2) || !read_float(d, x) || !read_float(d, y)) { break; }
            const vec2 c1 = (prev_cmd == 'C' || prev_cmd == 'S') ? cur * 2.0f - last_ctrl : cur;
            const vec2 c2 = rel ? cur + vec2{x2, y2} : vec2{x2, y2};
            const vec2 p  = rel ? cur + vec2{x, y}   : vec2{x, y};
            flatten_cubic(ensure_contour().points, cur, c1, c2, p, flat_sq);
            last_ctrl = c2;
            cur       = p;
            prev_cmd  = 'S';
        } else if (u == 'Q') {
            f32 qx{}, qy{}, x{}, y{};
            if (!read_float(d, qx) || !read_float(d, qy) || !read_float(d, x) || !read_float(d, y)) { break; }
            const vec2 q  = rel ? cur + vec2{qx, qy} : vec2{qx, qy};
            const vec2 p  = rel ? cur + vec2{x, y}   : vec2{x, y};
            const vec2 c1 = cur + (q - cur) * (2.0f / 3.0f);
            const vec2 c2 = p + (q - p) * (2.0f / 3.0f);
            flatten_cubic(ensure_contour().points, cur, c1, c2, p, flat_sq);
            last_ctrl = q;
            cur       = p;
            prev_cmd  = 'Q';
        } else if (u == 'T') {
            f32 x{}, y{};
            if (!read_float(d, x) || !read_float(d, y)) { break; }
            const vec2 q  = (prev_cmd == 'Q' || prev_cmd == 'T') ? cur * 2.0f - last_ctrl : cur;
            const vec2 p  = rel ? cur + vec2{x, y} : vec2{x, y};
            const vec2 c1 = cur + (q - cur) * (2.0f / 3.0f);
            const vec2 c2 = p + (q - p) * (2.0f / 3.0f);
            flatten_cubic(ensure_contour().points, cur, c1, c2, p, flat_sq);
            last_ctrl = q;
            cur       = p;
            prev_cmd  = 'T';
        } else if (u == 'A') {
            f32 rx{}, ry{}, rot{}, x{}, y{};
            int large{}, sweep{};
            if (!read_float(d, rx) || !read_float(d, ry) || !read_float(d, rot) ||
                !read_flag(d, large) || !read_flag(d, sweep) || !read_float(d, x) || !read_float(d, y)) { break; }
            const vec2 p = rel ? cur + vec2{x, y} : vec2{x, y};
            flatten_arc(ensure_contour().points, cur, rx, ry, rot, large, sweep, p, flatness);
            cur      = p;
            prev_cmd = 'A';
        } else {
            break;
        }
    }
    return out;
}

std::vector<u8> svg_rasterize_alpha(std::string_view d, vec2 viewbox, u32 width, u32 height, bool even_odd)
{
    std::vector<u8> alpha(static_cast<std::size_t>(width) * height, 0);
    if (width == 0 || height == 0 || viewbox.x <= 0.0f || viewbox.y <= 0.0f) { return alpha; }

    const f32 sx = static_cast<f32>(width) / viewbox.x;
    const f32 sy = static_cast<f32>(height) / viewbox.y;
    const auto contours = svg_parse_path(d, 0.25f / std::max(sx, sy));

    std::vector<edge> edges;
    for (const auto& c : contours) {
        if (c.points.size() < 2) { continue; }
        const std::size_t n = c.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const vec2 a = {c.points[i].x * sx, c.points[i].y * sy};
            const vec2 b = {c.points[(i + 1) % n].x * sx, c.points[(i + 1) % n].y * sy};
            if (std::abs(a.y - b.y) < 1.0e-5f) { continue; }
            if (a.y < b.y) { edges.push_back({a.x, a.y, b.x, b.y, 1}); }
            else           { edges.push_back({b.x, b.y, a.x, a.y, -1}); }
        }
    }
    if (edges.empty()) { return alpha; }

    // 4x vertical supersampling + analytic horizontal coverage
    constexpr int kSub = 4;
    std::vector<std::pair<f32, int>> hits;
    std::vector<f32> row_cov(width);

    for (u32 y = 0; y < height; ++y) {
        std::fill(row_cov.begin(), row_cov.end(), 0.0f);
        for (int s = 0; s < kSub; ++s) {
            const f32 sy_pos = static_cast<f32>(y) + (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kSub);
            hits.clear();
            for (const edge& e : edges) {
                if (sy_pos >= e.y0 && sy_pos < e.y1) {
                    const f32 t = (sy_pos - e.y0) / (e.y1 - e.y0);
                    hits.push_back({e.x0 + (e.x1 - e.x0) * t, e.dir});
                }
            }
            if (hits.empty()) { continue; }
            std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            int winding = 0;
            for (std::size_t i = 0; i + 1 < hits.size(); ++i) {
                winding += hits[i].second;
                const bool inside = even_odd ? ((winding & 1) != 0) : (winding != 0);
                if (!inside) { continue; }
                const f32 x0 = std::clamp(hits[i].first, 0.0f, static_cast<f32>(width));
                const f32 x1 = std::clamp(hits[i + 1].first, 0.0f, static_cast<f32>(width));
                if (x1 <= x0) { continue; }
                const u32 px0 = static_cast<u32>(std::floor(x0));
                const u32 px1 = std::min(width - 1, static_cast<u32>(std::floor(x1)));
                for (u32 px = px0; px <= px1; ++px) {
                    const f32 cover = std::min(x1, static_cast<f32>(px + 1)) - std::max(x0, static_cast<f32>(px));
                    if (cover > 0.0f) { row_cov[px] += cover * (1.0f / static_cast<f32>(kSub)); }
                }
            }
        }
        for (u32 x = 0; x < width; ++x) {
            alpha[static_cast<std::size_t>(y) * width + x] =
                static_cast<u8>(std::clamp(row_cov[x], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return alpha;
}

std::vector<u8> svg_rasterize_rgba(std::string_view d, vec2 viewbox, u32 width, u32 height, color tint, bool even_odd)
{
    const auto mask = svg_rasterize_alpha(d, viewbox, width, height, even_odd);
    std::vector<u8> rgba(mask.size() * 4);
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const u32 a = (static_cast<u32>(mask[i]) * tint.a + 127u) / 255u;
        rgba[i * 4 + 0] = tint.r;
        rgba[i * 4 + 1] = tint.g;
        rgba[i * 4 + 2] = tint.b;
        rgba[i * 4 + 3] = static_cast<u8>(a);
    }
    return rgba;
}

void draw_svg_path(draw_list& dl, const rect& dst, std::string_view d, vec2 viewbox, color col, f32 stroke_width)
{
    if (viewbox.x <= 0.0f || viewbox.y <= 0.0f || dst.width() <= 0.0f || dst.height() <= 0.0f) { return; }
    const f32 sx = dst.width() / viewbox.x;
    const f32 sy = dst.height() / viewbox.y;
    const auto contours = svg_parse_path(d, 0.35f / std::max(sx, sy));
    std::vector<vec2> pts;
    for (const auto& c : contours) {
        if (c.points.size() < 2) { continue; }
        pts.clear();
        pts.reserve(c.points.size());
        for (const vec2& p : c.points) {
            pts.push_back({dst.min.x + p.x * sx, dst.min.y + p.y * sy});
        }
        if (stroke_width > 0.0f || !c.closed || pts.size() < 3) {
            dl.polyline(pts, col, stroke_width > 0.0f ? stroke_width : 1.5f, c.closed);
        } else {
            // drop duplicate closing point so polygon_filled sees clean edges
            while (pts.size() > 2 && std::abs(pts.back().x - pts.front().x) < 1.0e-3f &&
                   std::abs(pts.back().y - pts.front().y) < 1.0e-3f) {
                pts.pop_back();
            }
            if (pts.size() < 3) { continue; }
            // convex contours fit a single polygon_filled() call without internal AA seams
            bool convex = true;
            f32 sign = 0.0f;
            for (std::size_t i = 0; i < pts.size() && convex; ++i) {
                const vec2 a = pts[i];
                const vec2 b = pts[(i + 1) % pts.size()];
                const vec2 e = pts[(i + 2) % pts.size()];
                const f32 cross = (b.x - a.x) * (e.y - b.y) - (b.y - a.y) * (e.x - b.x);
                if (std::abs(cross) > 1.0e-4f) {
                    if (sign == 0.0f) { sign = cross; }
                    else if (cross * sign < 0.0f) { convex = false; }
                }
            }
            if (convex) {
                if (pts.size() > 64) {
                    std::vector<vec2> reduced;
                    reduced.reserve(64);
                    for (std::size_t i = 0; i < 64; ++i) {
                        reduced.push_back(pts[(i * pts.size()) / 64]);
                    }
                    dl.polygon_filled(reduced, col);
                } else {
                    dl.polygon_filled(pts, col);
                }
            } else {
                // ear-clip concave contours (handles non-convex icons like folder)
                std::vector<vec2> rem = pts;
                f32 area = 0.0f;
                for (std::size_t i = 0; i < rem.size(); ++i) {
                    const vec2& a = rem[i];
                    const vec2& b = rem[(i + 1) % rem.size()];
                    area += a.x * b.y - b.x * a.y;
                }
                const f32 orient = area >= 0.0f ? 1.0f : -1.0f;
                auto in_tri = [](vec2 p, vec2 a, vec2 b, vec2 c) {
                    const f32 c1 = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                    const f32 c2 = (c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x);
                    const f32 c3 = (a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x);
                    const bool neg = (c1 < 0.0f) || (c2 < 0.0f) || (c3 < 0.0f);
                    const bool pos = (c1 > 0.0f) || (c2 > 0.0f) || (c3 > 0.0f);
                    return !(neg && pos);
                };
                std::size_t guard = rem.size() * rem.size();
                while (rem.size() > 3 && guard-- > 0) {
                    bool clipped = false;
                    for (std::size_t i = 0; i < rem.size(); ++i) {
                        const vec2& prev = rem[(i + rem.size() - 1) % rem.size()];
                        const vec2& curr = rem[i];
                        const vec2& next = rem[(i + 1) % rem.size()];
                        const f32 cr = ((curr.x - prev.x) * (next.y - curr.y) - (curr.y - prev.y) * (next.x - curr.x)) * orient;
                        if (cr <= 1.0e-5f) { continue; }
                        bool contains = false;
                        for (std::size_t j = 0; j < rem.size(); ++j) {
                            if (j == i || j == (i + rem.size() - 1) % rem.size() || j == (i + 1) % rem.size()) { continue; }
                            if (in_tri(rem[j], prev, curr, next)) { contains = true; break; }
                        }
                        if (!contains) {
                            dl.triangle_filled(prev, curr, next, col);
                            rem.erase(rem.begin() + static_cast<std::ptrdiff_t>(i));
                            clipped = true;
                            break;
                        }
                    }
                    if (!clipped) { break; }
                }
                if (rem.size() == 3) { dl.triangle_filled(rem[0], rem[1], rem[2], col); }
            }
        }
    }
}

} // namespace strata

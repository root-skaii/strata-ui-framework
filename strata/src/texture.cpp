#include "strata/texture.hpp"

#include "strata/vmem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace strata {

f32 half_to_float(u16 h) noexcept
{
    const u32 sign = static_cast<u32>(h & 0x8000u) << 16;
    u32       exp  = (h >> 10) & 0x1fu;
    u32       man  = h & 0x3ffu;
    u32       bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else { // a subnormal half is a normal float
            exp = 127 - 15 + 1;
            while ((man & 0x400u) == 0) { man <<= 1; --exp; }
            man &= 0x3ffu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    return std::bit_cast<f32>(bits);
}

u16 float_to_half(f32 f) noexcept
{
    const u32 bits = std::bit_cast<u32>(f);
    const u32 sign = (bits >> 16) & 0x8000u;
    const u32 abs  = bits & 0x7fffffffu;
    if (abs >= 0x7f800000u) { // inf / nan
        return static_cast<u16>(sign | 0x7c00u | (abs > 0x7f800000u ? 0x200u : 0u));
    }
    if (abs >= 0x477ff000u) { // too large: infinity
        return static_cast<u16>(sign | 0x7c00u);
    }
    if (abs < 0x38800000u) { // subnormal half or zero
        if (abs < 0x33000000u) { return static_cast<u16>(sign); }
        const u32 e    = abs >> 23;
        const u32 man  = (abs & 0x7fffffu) | 0x800000u;
        const u32 shift = 126 - e;
        u32 r = man >> shift;
        const u32 rem = man & ((1u << shift) - 1u);
        const u32 halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (r & 1u))) { ++r; }
        return static_cast<u16>(sign | r);
    }
    u32 r = ((abs - 0x38000000u) >> 13);
    const u32 rem = abs & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u))) { ++r; }
    return static_cast<u16>(sign | r);
}

u32 texture_mip_count(u32 width, u32 height, u32 requested) noexcept
{
    if (width == 0 || height == 0) { return 0; }
    u32 full = 1;
    for (u32 d = std::max(width, height); d > 1; d >>= 1) { ++full; }
    if (requested == 0) { return full; }
    return std::min(requested, full);
}

bool texture_image::create(const texture_desc& desc, std::span<const u8> pixels)
{
    levels_.clear();
    format_ = desc.format;
    if (desc.width == 0 || desc.height == 0) {
        return false;
    }
    const std::size_t src_bpp = texture_source_bytes(desc.format);
    const std::size_t count   = static_cast<std::size_t>(desc.width) * desc.height;
    if (pixels.size() < count * src_bpp) {
        return false;
    }

    const u32 n = texture_mip_count(desc.width, desc.height, desc.mip_levels);
    levels_.resize(n);
    levels_[0].w = desc.width;
    levels_[0].h = desc.height;
    levels_[0].data.resize(count * texture_layout_bytes(layout()));
    u8* dst = levels_[0].data.data();
    switch (desc.format) {
    case texture_format::r8:
        for (std::size_t i = 0; i < count; ++i) { dst[4 * i] = dst[4 * i + 1] = dst[4 * i + 2] = pixels[i]; dst[4 * i + 3] = 255; }
        break;
    case texture_format::a8:
        for (std::size_t i = 0; i < count; ++i) { dst[4 * i] = dst[4 * i + 1] = dst[4 * i + 2] = 255; dst[4 * i + 3] = pixels[i]; }
        break;
    default:
        std::memcpy(dst, pixels.data(), count * src_bpp);
        break;
    }
    for (u32 k = 1; k < n; ++k) {
        levels_[k].w = std::max(1u, levels_[k - 1].w >> 1);
        levels_[k].h = std::max(1u, levels_[k - 1].h >> 1);
        levels_[k].data.resize(static_cast<std::size_t>(levels_[k].w) * levels_[k].h * texture_layout_bytes(layout()));
        build_level(k, 0, 0, levels_[k].w, levels_[k].h);
    }
    return true;
}

// 2x2 box filter, alpha-weighted so transparent texels do not darken shape edges
void texture_image::build_level(u32 k, u32 x0, u32 y0, u32 x1, u32 y1)
{
    const level& src = levels_[k - 1];
    level&       dst = levels_[k];
    const bool   half_float = layout() == texture_layout::rgba16f;
    for (u32 y = y0; y < y1; ++y) {
        const u32 sy0 = std::min(2 * y, src.h - 1);
        const u32 sy1 = std::min(2 * y + 1, src.h - 1);
        for (u32 x = x0; x < x1; ++x) {
            const u32 sx0 = std::min(2 * x, src.w - 1);
            const u32 sx1 = std::min(2 * x + 1, src.w - 1);
            const u32 sxs[4] = {sx0, sx1, sx0, sx1};
            const u32 sys[4] = {sy0, sy0, sy1, sy1};
            u8* out = dst.data.data() + (static_cast<std::size_t>(y) * dst.w + x) * texture_layout_bytes(layout());
            if (!half_float) {
                u32 sa = 0;
                u32 sc[3] = {};
                u32 plain[3] = {};
                for (int i = 0; i < 4; ++i) {
                    const u8* p = src.data.data() + (static_cast<std::size_t>(sys[i]) * src.w + sxs[i]) * 4;
                    sa += p[3];
                    for (int c = 0; c < 3; ++c) { sc[c] += static_cast<u32>(p[c]) * p[3]; plain[c] += p[c]; }
                }
                for (int c = 0; c < 3; ++c) {
                    out[c] = static_cast<u8>(sa > 0 ? (sc[c] + sa / 2) / sa : (plain[c] + 2) / 4);
                }
                out[3] = static_cast<u8>((sa + 2) / 4);
            } else {
                f32 sa = 0.0f;
                f32 sc[3] = {};
                f32 plain[3] = {};
                for (int i = 0; i < 4; ++i) {
                    const u16* p = reinterpret_cast<const u16*>(src.data.data()) + (static_cast<std::size_t>(sys[i]) * src.w + sxs[i]) * 4;
                    const f32 a = half_to_float(p[3]);
                    sa += a;
                    for (int c = 0; c < 3; ++c) { const f32 v = half_to_float(p[c]); sc[c] += v * a; plain[c] += v; }
                }
                u16* o = reinterpret_cast<u16*>(out);
                for (int c = 0; c < 3; ++c) { o[c] = float_to_half(sa > 1.0e-6f ? sc[c] / sa : plain[c] * 0.25f); }
                o[3] = float_to_half(sa * 0.25f);
            }
        }
    }
}

bool texture_image::update(u32 x, u32 y, u32 w, u32 h, std::span<const u8> pixels, std::vector<region>& dirty)
{
    dirty.clear();
    if (levels_.empty() || w == 0 || h == 0 || x >= levels_[0].w || y >= levels_[0].h || w > levels_[0].w - x || h > levels_[0].h - y) {
        return false;
    }
    const std::size_t src_bpp = texture_source_bytes(format_);
    if (pixels.size() < static_cast<std::size_t>(w) * h * src_bpp) {
        return false;
    }
    level& l0 = levels_[0];
    const std::size_t dst_bpp = texture_layout_bytes(layout());
    for (u32 row = 0; row < h; ++row) {
        const u8* in  = pixels.data() + static_cast<std::size_t>(row) * w * src_bpp;
        u8*       out = l0.data.data() + (static_cast<std::size_t>(y + row) * l0.w + x) * dst_bpp;
        switch (format_) {
        case texture_format::r8:
            for (u32 i = 0; i < w; ++i) { out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = in[i]; out[4 * i + 3] = 255; }
            break;
        case texture_format::a8:
            for (u32 i = 0; i < w; ++i) { out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 255; out[4 * i + 3] = in[i]; }
            break;
        default:
            std::memcpy(out, in, static_cast<std::size_t>(w) * dst_bpp);
            break;
        }
    }
    dirty.push_back({0, x, y, w, h});
    u32 x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    for (u32 k = 1; k < levels_.size(); ++k) {
        x0 >>= 1;
        y0 >>= 1;
        x1 = std::min((x1 + 1) >> 1, levels_[k].w);
        y1 = std::min((y1 + 1) >> 1, levels_[k].h);
        x0 = std::min(x0, levels_[k].w - 1); // (a level of an odd size drops the last column / row of the one above)
        y0 = std::min(y0, levels_[k].h - 1);
        x1 = std::max(x1, x0 + 1);
        y1 = std::max(y1, y0 + 1);
        build_level(k, x0, y0, x1, y1);
        dirty.push_back({k, x0, y0, x1 - x0, y1 - y0});
    }
    return true;
}

void texture_image::wipe() noexcept
{
    for (level& l : levels_) {
        if (!l.data.empty()) { detail::secure_wipe(l.data.data(), l.data.size()); }
    }
    levels_.clear();
    levels_.shrink_to_fit();
}

} // namespace strata

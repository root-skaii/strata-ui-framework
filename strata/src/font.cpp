#include "strata/font.hpp"

#include "strata/bidi.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace strata {

namespace {

[[nodiscard]] u16 to_uv15(u32 texel, u32 extent) noexcept
{
    return static_cast<u16>((static_cast<u64>(texel) * 32767u + extent / 2) / extent);
}

[[nodiscard]] std::wstring widen(std::string_view s)
{
    std::wstring out(s.size(), L'\0');
    if (!s.empty()) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                            out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<std::size_t>(std::max(n, 0)));
    }
    return out;
}

[[nodiscard]] std::optional<std::vector<u8>> read_file(std::string_view path)
{
    const std::wstring wide = widen(path);
    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    std::optional<std::vector<u8>> out;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
        std::vector<u8> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()) {
            out = std::move(bytes);
        }
    }
    ::CloseHandle(file);
    return out;
}

// --- minimal sfnt reader: just enough to find the family name gdi will use ---

[[nodiscard]] std::optional<u32> be16(std::span<const u8> d, std::size_t off) noexcept
{
    if (off + 2 > d.size()) { return std::nullopt; }
    return (u32{d[off]} << 8) | d[off + 1];
}

[[nodiscard]] std::optional<u32> be32(std::span<const u8> d, std::size_t off) noexcept
{
    if (off + 4 > d.size()) { return std::nullopt; }
    return (u32{d[off]} << 24) | (u32{d[off + 1]} << 16) | (u32{d[off + 2]} << 8) | d[off + 3];
}

// name id 1 (font family), the name gdi uses to select a face
[[nodiscard]] std::optional<std::wstring> family_name(std::span<const u8> d)
{
    std::size_t sfnt = 0;
    if (const auto tag = be32(d, 0); tag && *tag == 0x74746366u /* 'ttcf' */) {
        const auto first = be32(d, 12);
        if (!first) { return std::nullopt; }
        sfnt = *first;
    }

    const auto table_count = be16(d, sfnt + 4);
    if (!table_count) { return std::nullopt; }

    std::optional<std::size_t> name_table;
    for (u32 i = 0; i < *table_count; ++i) {
        const std::size_t rec = sfnt + 12 + static_cast<std::size_t>(i) * 16;
        const auto tag = be32(d, rec);
        const auto off = be32(d, rec + 8);
        if (tag && off && *tag == 0x6e616d65u /* 'name' */) {
            name_table = *off;
            break;
        }
    }
    if (!name_table) { return std::nullopt; }

    const auto count          = be16(d, *name_table + 2);
    const auto string_storage = be16(d, *name_table + 4);
    if (!count || !string_storage) { return std::nullopt; }

    std::optional<std::wstring> best;
    for (u32 i = 0; i < *count; ++i) {
        const std::size_t rec = *name_table + 6 + static_cast<std::size_t>(i) * 12;
        const auto platform = be16(d, rec);
        const auto lang     = be16(d, rec + 4);
        const auto name_id  = be16(d, rec + 6);
        const auto length   = be16(d, rec + 8);
        const auto offset   = be16(d, rec + 10);
        if (!platform || !lang || !name_id || !length || !offset || *name_id != 1) { continue; }

        const std::size_t at = *name_table + *string_storage + *offset;
        if (at + *length > d.size()) { continue; }

        std::wstring s;
        if (*platform == 3 || *platform == 0) { // utf-16be
            for (u32 k = 0; k + 1 < *length; k += 2) {
                s.push_back(static_cast<wchar_t>((u32{d[at + k]} << 8) | d[at + k + 1]));
            }
        } else if (*platform == 1) {            // mac roman, ascii subset
            for (u32 k = 0; k < *length; ++k) { s.push_back(static_cast<wchar_t>(d[at + k])); }
        } else {
            continue;
        }
        if (s.empty()) { continue; }

        const bool english = *platform == 3 && *lang == 0x0409;
        if (!best || english) { best = std::move(s); }
        if (english) { break; }
    }
    return best;
}

// zeroes a buffer that held font data before it is freed
struct scrub_on_exit {
    std::vector<u8>& bytes;
    ~scrub_on_exit()
    {
        if (!bytes.empty()) { ::SecureZeroMemory(bytes.data(), bytes.size()); }
    }
};

// the cmap of a font, for code points above the BMP (gdi only maps utf-16 code units)
struct cmap12 {
    std::vector<u8> data;
    std::size_t     groups_at{};
    u32             group_count{};

    // glyph id of a supplementary code point, 0xffff if the font has none
    [[nodiscard]] u32 lookup(char32_t cp) const noexcept
    {
        u32 lo = 0, hi = group_count;
        while (lo < hi) {
            const u32 mid = lo + (hi - lo) / 2;
            const std::size_t g = groups_at + static_cast<std::size_t>(mid) * 12;
            const u32 start = *be32(data, g);
            const u32 end   = *be32(data, g + 4);
            if (cp < start)      { hi = mid; }
            else if (cp > end)   { lo = mid + 1; }
            else                 { return *be32(data, g + 8) + (cp - start); }
        }
        return 0xffffu;
    }
    [[nodiscard]] bool empty() const noexcept { return group_count == 0; }
};

[[nodiscard]] cmap12 load_cmap12(HDC dc)
{
    cmap12 out;
    const DWORD tag = 0x70616d63; // 'cmap', as GetFontData wants it (little endian)
    const DWORD size = ::GetFontData(dc, tag, 0, nullptr, 0);
    if (size == GDI_ERROR || size < 4) { return out; }
    std::vector<u8> table(size);
    if (::GetFontData(dc, tag, 0, table.data(), size) != size) { return out; }
    const auto count = be16(table, 2);
    if (!count) { return out; }
    for (u32 i = 0; i < *count; ++i) {
        const std::size_t rec = 4 + static_cast<std::size_t>(i) * 8;
        const auto platform = be16(table, rec);
        const auto encoding = be16(table, rec + 2);
        const auto offset   = be32(table, rec + 4);
        if (!platform || !encoding || !offset) { continue; }
        const bool ucs4 = (*platform == 3 && *encoding == 10) || (*platform == 0 && (*encoding == 4 || *encoding == 6));
        if (!ucs4) { continue; }
        const auto format = be16(table, *offset);
        const auto n      = be32(table, *offset + 12);
        if (!format || *format != 12 || !n || *offset + 16 + static_cast<std::size_t>(*n) * 12 > table.size()) { continue; }
        out.data        = std::move(table);
        out.groups_at   = *offset + 16;
        out.group_count = *n;
        return out;
    }
    return out;
}

struct raw_glyph {
    u8   source{};    // 0: the font itself, n: fallback face n - 1
    u16  index{};
    u16  w{}, h{};
    i32  origin_x{}, origin_y{};
    f32  advance{};
    u32  x{}, y{};
    bool present{};
    bool visible{};
};

// everything one font needs while the shared atlas is being built
struct font_source {
    std::vector<u8>              file_bytes;
    HANDLE                       installed{};
    HDC                          dc{};
    HFONT                        font{};
    HGDIOBJ                      previous{};
    TEXTMETRICW                  tm{};
    std::vector<codepoint_range> merged;
    std::vector<raw_glyph>       raws;
    cmap12                       cmap;          // (only read when a supplementary range was asked for)

    // faces that supply what the main font lacks
    struct fallback {
        HDC    dc{};
        HFONT  font{};
        HGDIOBJ previous{};
        cmap12 cmap;
    };
    std::vector<fallback> fallbacks;

    font_source() = default;
    font_source(const font_source&)            = delete;
    font_source& operator=(const font_source&) = delete;

    ~font_source()
    {
        for (fallback& f : fallbacks) {
            if (f.dc != nullptr) {
                if (f.previous != nullptr) { ::SelectObject(f.dc, f.previous); }
                if (f.font != nullptr)     { ::DeleteObject(f.font); }
                ::DeleteDC(f.dc);
            }
        }
        if (dc != nullptr) {
            if (previous != nullptr) { ::SelectObject(dc, previous); }
            if (font != nullptr)     { ::DeleteObject(font); }
            ::DeleteDC(dc);
        }
        if (installed != nullptr) { ::RemoveFontMemResourceEx(installed); }
        if (!file_bytes.empty())  { ::SecureZeroMemory(file_bytes.data(), file_bytes.size()); }
    }
};

const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
constexpr UINT bitmap_flags = GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX;

} // namespace

std::expected<font_atlas, font_error> font_atlas::build(std::span<const font_config> configs, u32 max_atlas_size, f32 scale)
{
    if (configs.empty()) {
        return std::unexpected{font_error::no_fonts};
    }
    scale = std::clamp(scale, 0.25f, 8.0f);

    std::vector<std::unique_ptr<font_source>> sources;
    sources.reserve(configs.size());

    font_atlas atlas;
    atlas.fonts_.resize(configs.size());
    atlas.scale_     = scale;
    atlas.inv_scale_ = 1.0f / scale;

    // 1. open every font and measure its glyphs --------------------------------
    for (std::size_t fi = 0; fi < configs.size(); ++fi) {
        const font_config& cfg = configs[fi];
        sources.push_back(std::make_unique<font_source>());
        font_source& src = *sources.back();

        std::span<const u8> bytes = cfg.data;
        if (!cfg.file.empty()) {
            auto loaded = read_file(cfg.file);
            if (!loaded) {
                return std::unexpected{font_error::file_unreadable};
            }
            src.file_bytes = std::move(*loaded);
            bytes          = src.file_bytes;
        }

        std::wstring face;
        if (!bytes.empty()) {
            auto family = family_name(bytes);
            if (!family) {
                return std::unexpected{font_error::font_invalid};
            }
            face = std::move(*family);

            DWORD faces = 0;
            src.installed = ::AddFontMemResourceEx(const_cast<u8*>(bytes.data()), static_cast<DWORD>(bytes.size()), nullptr, &faces);
            if (src.installed == nullptr || faces == 0) {
                return std::unexpected{font_error::font_invalid};
            }
        } else {
            face = widen(cfg.face);
        }

        src.dc = ::CreateCompatibleDC(nullptr);
        if (src.dc == nullptr) {
            return std::unexpected{font_error::device_failed};
        }
        src.font = ::CreateFontW(-std::max(1, static_cast<int>(std::lround(cfg.pixel_height * scale))), 0, 0, 0, cfg.bold ? FW_BOLD : FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
        if (src.font == nullptr) {
            return std::unexpected{font_error::font_unavailable};
        }
        src.previous = ::SelectObject(src.dc, src.font);

        if (!bytes.empty() || cfg.exact_face) {
            // gdi silently substitutes a default face when it can't match; for files we know what to expect
            wchar_t actual[LF_FACESIZE]{};
            ::GetTextFaceW(src.dc, LF_FACESIZE, actual);
            if (::_wcsicmp(actual, face.c_str()) != 0) {
                return std::unexpected{font_error::font_unavailable};
            }
        }

        ::GetTextMetricsW(src.dc, &src.tm);

        // faces to fall back on: same height, same weight
        for (const std::string_view fb : cfg.fallback_faces) {
            font_source::fallback f;
            f.dc = ::CreateCompatibleDC(nullptr);
            if (f.dc == nullptr) { continue; }
            const std::wstring fb_face = widen(fb);
            f.font = ::CreateFontW(-std::max(1, static_cast<int>(std::lround(cfg.pixel_height * scale))), 0, 0, 0, cfg.bold ? FW_BOLD : FW_NORMAL,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fb_face.c_str());
            if (f.font != nullptr) {
                f.previous = ::SelectObject(f.dc, f.font);
                wchar_t actual[LF_FACESIZE]{};
                ::GetTextFaceW(f.dc, LF_FACESIZE, actual);
                if (::_wcsicmp(actual, fb_face.c_str()) == 0) { // (not the substitute gdi picks for a missing face)
                    f.cmap = load_cmap12(f.dc);
                    src.fallbacks.push_back(std::move(f));
                    continue;
                }
                ::SelectObject(f.dc, f.previous);
                ::DeleteObject(f.font);
            }
            ::DeleteDC(f.dc);
        }

        // normalise the requested ranges
        for (const codepoint_range& r : cfg.ranges) {
            const char32_t first = std::max<char32_t>(r.first, 0x20);
            const char32_t last  = std::min<char32_t>(r.last, 0x10ffff);
            if (first <= last) { src.merged.push_back({first, last}); }
        }
        if (src.merged.empty()) {
            src.merged.assign(glyph_ranges::default_set.begin(), glyph_ranges::default_set.end());
        }
        std::ranges::sort(src.merged, {}, &codepoint_range::first);
        {
            std::size_t out = 0;
            for (std::size_t i = 1; i < src.merged.size(); ++i) {
                if (src.merged[i].first <= src.merged[out].last + 1) {
                    src.merged[out].last = std::max(src.merged[out].last, src.merged[i].last);
                } else {
                    src.merged[++out] = src.merged[i];
                }
            }
            src.merged.resize(out + 1);
        }

        font_data& fd = atlas.fonts_[fi];
        fd.line_height = static_cast<f32>(src.tm.tmHeight);
        fd.ascent      = static_cast<f32>(src.tm.tmAscent);

        std::size_t total = 0;
        for (const codepoint_range& r : src.merged) {
            fd.ranges.push_back({r.first, r.last, total});
            total += r.last - r.first + 1;
        }
        fd.glyphs.resize(total);
        src.raws.resize(total);

        if (!src.merged.empty() && src.merged.back().last > 0xffff) { src.cmap = load_cmap12(src.dc); }

        // the glyph a face has for a code point (0xffff: none)
        const auto glyph_index = [](HDC dc, const cmap12& map, char32_t cp) -> WORD {
            if (cp > 0xffff) {
                const u32 id = map.lookup(cp);
                return id > 0xfffe ? WORD{0xffff} : static_cast<WORD>(id);
            }
            const wchar_t wc = static_cast<wchar_t>(cp);
            WORD index = 0;
            if (::GetGlyphIndicesW(dc, &wc, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) { return 0xffff; }
            return index;
        };

        // which glyphs exist, and how big are their bitmaps
        for (const range_entry& range : fd.ranges) {
            for (char32_t cp = range.first; cp <= range.last; ++cp) {
                raw_glyph& g = src.raws[range.offset + (cp - range.first)];
                if (cp >= 0x7f && cp <= 0x9f) { continue; } // C1 controls

                HDC   dc     = src.dc;
                WORD  index  = glyph_index(src.dc, src.cmap, cp);
                u8    origin = 0;
                for (std::size_t k = 0; index == 0xffff && k < src.fallbacks.size(); ++k) {
                    index = glyph_index(src.fallbacks[k].dc, src.fallbacks[k].cmap, cp);
                    if (index != 0xffff) { dc = src.fallbacks[k].dc; origin = static_cast<u8>(k + 1); }
                }
                if (index == 0xffff) {
                    continue;
                }

                GLYPHMETRICS gm{};
                const DWORD  size = ::GetGlyphOutlineW(dc, index, bitmap_flags, &gm, 0, nullptr, &identity);
                if (size == GDI_ERROR) { continue; }

                g.source   = origin;
                g.index    = index;
                g.present  = true;
                g.advance  = static_cast<f32>(gm.gmCellIncX);
                g.visible  = size != 0 && gm.gmBlackBoxX != 0 && gm.gmBlackBoxY != 0;
                g.w        = static_cast<u16>(gm.gmBlackBoxX);
                g.h        = static_cast<u16>(gm.gmBlackBoxY);
                g.origin_x = gm.gmptGlyphOrigin.x;
                g.origin_y = gm.gmptGlyphOrigin.y;
            }
        }
    }

    // 2. pick the smallest atlas that fits every font (shelf packing, 1px padding) ---
    const auto try_pack = [&](u32 w, u32 h) {
        u32 cx = 3; // 2x2 white block in the corner
        u32 cy = 0;
        u32 row_h = 0;
        for (auto& src : sources) {
            for (raw_glyph& g : src->raws) {
                if (!g.visible) { continue; }
                if (cx + g.w + 1 > w) {
                    cx = 0;
                    cy += row_h + 1;
                    row_h = 0;
                }
                if (cy + g.h > h || g.w > w) { return false; }
                g.x = cx;
                g.y = cy;
                cx += g.w + 1;
                row_h = std::max<u32>(row_h, g.h);
            }
        }
        return true;
    };

    u32 aw = 0;
    u32 ah = 0;
    for (u32 w = 256, h = 256; w <= max_atlas_size && h <= max_atlas_size; (w <= h ? w : h) *= 2) {
        if (try_pack(w, h)) { aw = w; ah = h; break; }
    }
    if (aw == 0) {
        return std::unexpected{font_error::atlas_full};
    }
    atlas.width_  = aw;
    atlas.height_ = ah;
    atlas.pixels_.assign(static_cast<std::size_t>(aw) * ah, 0);

    const auto put = [&](u32 x, u32 y, u8 v) { atlas.pixels_[static_cast<std::size_t>(y) * aw + x] = v; };
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) { put(x, y, 255); }
    }
    atlas.white_uv_ = {to_uv15(1, aw), to_uv15(1, ah)};

    // 3. rasterise every glyph into the atlas and fill the public tables -------
    std::vector<u8> scratch;
    const scrub_on_exit scrub_scratch{scratch};

    for (std::size_t fi = 0; fi < sources.size(); ++fi) {
        font_source& src = *sources[fi];
        font_data&   fd  = atlas.fonts_[fi];

        for (std::size_t i = 0; i < src.raws.size(); ++i) {
            const raw_glyph& r = src.raws[i];
            glyph&           g = fd.glyphs[i];

            g.present = r.present;
            g.advance = r.advance;
            if (!r.visible) { continue; }

            HDC glyph_dc = r.source == 0 ? src.dc : src.fallbacks[r.source - 1].dc;
            GLYPHMETRICS gm{};
            const DWORD  size = ::GetGlyphOutlineW(glyph_dc, r.index, bitmap_flags, &gm, 0, nullptr, &identity);
            if (size == GDI_ERROR || size == 0) { continue; }
            scratch.resize(size);
            if (::GetGlyphOutlineW(glyph_dc, r.index, bitmap_flags, &gm, size, scratch.data(), &identity) == GDI_ERROR) {
                continue;
            }

            const u32 pitch = (r.w + 3u) & ~3u; // gdi pads rows to dwords
            for (u32 y = 0; y < r.h; ++y) {
                for (u32 x = 0; x < r.w; ++x) {
                    const u32 level = scratch[static_cast<std::size_t>(y) * pitch + x]; // 0..64
                    put(r.x + x, r.y + y, static_cast<u8>(std::min(level * 4u, 255u)));
                }
            }

            g.x0 = static_cast<f32>(r.origin_x);
            g.y0 = static_cast<f32>(src.tm.tmAscent - r.origin_y);
            g.x1 = g.x0 + static_cast<f32>(r.w);
            g.y1 = g.y0 + static_cast<f32>(r.h);
            g.u0 = to_uv15(r.x, aw);
            g.v0 = to_uv15(r.y, ah);
            g.u1 = to_uv15(r.x + r.w, aw);
            g.v1 = to_uv15(r.y + r.h, ah);
            g.visible = true;
        }

        // kerning pairs from the font's kern table, already scaled to pixels by gdi
        if (configs[fi].kerning) {
            const DWORD pair_count = ::GetKerningPairsW(src.dc, 0, nullptr);
            if (pair_count != 0 && pair_count != GDI_ERROR) {
                std::vector<KERNINGPAIR> pairs(pair_count);
                if (::GetKerningPairsW(src.dc, pair_count, pairs.data()) != 0) {
                    fd.kern.reserve(pairs.size());
                    for (const KERNINGPAIR& p : pairs) {
                        if (p.iKernAmount != 0) {
                            fd.kern.push_back({(u32{p.wFirst} << 16) | p.wSecond, static_cast<f32>(p.iKernAmount)});
                        }
                    }
                    std::ranges::sort(fd.kern, {}, &kern_pair::key);
                    if (!fd.kern.empty()) {
                        fd.kern_ascii.assign(128 * 128, 0);
                        for (const kern_pair& k : fd.kern) {
                            const u32 left = k.key >> 16, right = k.key & 0xffffu;
                            if (left >= 128 || right >= 128) { continue; }
                            const bool fits = k.amount == std::round(k.amount) && k.amount > -128.0f && k.amount <= 127.0f;
                            fd.kern_ascii[left * 128 + right] = fits ? static_cast<std::int8_t>(k.amount) : kern_ask;
                        }
                    }
                }
            }
        }

        // fallback glyph: '?', then U+FFFD, then whatever exists first
        const auto index_of = [&](char32_t cp) -> std::optional<std::size_t> {
            for (const range_entry& range : fd.ranges) {
                if (cp >= range.first && cp <= range.last) {
                    const std::size_t i = range.offset + (cp - range.first);
                    if (fd.glyphs[i].present) { return i; }
                }
            }
            return std::nullopt;
        };
        if (const auto q = index_of(U'?'))        { fd.fallback = *q; }
        else if (const auto r = index_of(0xfffd)) { fd.fallback = *r; }
        else {
            const auto it = std::ranges::find_if(fd.glyphs, &glyph::present);
            if (it == fd.glyphs.end()) {
                return std::unexpected{font_error::font_invalid};
            }
            fd.fallback = static_cast<std::size_t>(it - fd.glyphs.begin());
        }
    }

    return atlas;
}

font_atlas::~font_atlas()
{
    discard_pixels();
}

void font_atlas::discard_pixels() noexcept
{
    if (!pixels_.empty()) {
        ::SecureZeroMemory(pixels_.data(), pixels_.size());
    }
    std::vector<u8>().swap(pixels_);
}

std::size_t font_atlas::glyph_count() const noexcept
{
    std::size_t n = 0;
    for (const font_data& f : fonts_) { n += f.glyphs.size(); }
    return n;
}

f32 font_atlas::kerning_slow(const font_data& fd, u32 key) noexcept
{
    const auto it = std::ranges::lower_bound(fd.kern, key, {}, &kern_pair::key);
    return it != fd.kern.end() && it->key == key ? it->amount : 0.0f;
}

// invisible shaping code points: zero-width joiners / spaces, direction marks, variation selectors, skin-tone
// modifiers (single-color glyphs cannot show them)
[[nodiscard]] static bool is_ignorable(char32_t cp) noexcept
{
    return (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
           (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0xfeff || (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0100 && cp <= 0xe01ef) ||
           cp == 0x00ad || cp == 0x061c;
}

const glyph& font_atlas::lookup_slow(const font_data& fd, char32_t cp) noexcept
{
    static constexpr glyph nothing{0, 0, 0, 0, 0, 0, 0, 0, 0, false, true};
    if (is_ignorable(cp)) { return nothing; }
    // last range whose `first` is <= cp
    const auto it = std::ranges::upper_bound(fd.ranges, cp, {}, &range_entry::first);
    if (it != fd.ranges.begin()) {
        const range_entry& r = *(it - 1);
        if (cp <= r.last) {
            const glyph& g = fd.glyphs[r.offset + (cp - r.first)];
            if (g.present) { return g; }
        }
    }
    return fd.glyphs[fd.fallback];
}

bool font_atlas::has_glyph(font_id f, char32_t cp) const noexcept
{
    const font_data& fd = fonts_[f];
    const auto it = std::ranges::upper_bound(fd.ranges, cp, {}, &range_entry::first);
    if (it == fd.ranges.begin()) { return false; }
    const range_entry& r = *(it - 1);
    return cp <= r.last && fd.glyphs[r.offset + (cp - r.first)].present;
}

vec2 font_atlas::measure_px(font_id f, std::string_view text) const noexcept
{
    std::string visual; // rtl text is measured as drawn (joined forms differ in width)
    if (has_rtl_text(text)) {
        visual = to_visual(text, this, f);
        text   = visual;
    }
    f32 line_w = 0;
    f32 max_w  = 0;
    f32 lines  = 1;
    char32_t prev = 0;
    while (!text.empty()) {
        const char32_t cp = decode_utf8(text);
        if (cp == U'\n') {
            max_w  = std::max(max_w, line_w);
            line_w = 0;
            lines += 1;
            prev   = 0;
            continue;
        }
        line_w += find(f, cp).advance + (prev != 0 ? kerning_px(f, prev, cp) : 0.0f);
        prev    = cp;
    }
    return {std::max(max_w, line_w), lines * fonts_[f].line_height};
}

} // namespace strata

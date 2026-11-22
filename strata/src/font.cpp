#include "strata/font.hpp"

#include "font_ttf.hpp"
#include "strata/bidi.hpp"
#include "strata/vmem.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace strata {

namespace {

[[nodiscard]] u16 to_uv15(u32 texel, u32 extent) noexcept
{
    return static_cast<u16>((static_cast<u64>(texel) * 32767u + extent / 2) / extent);
}

[[nodiscard]] u32 from_uv15(u16 uv, u32 extent) noexcept
{
    return static_cast<u32>((static_cast<u64>(uv) * extent + 16383u) / 32767u);
}

#ifdef _WIN32
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
#endif

[[nodiscard]] std::optional<std::vector<u8>> read_file(std::string_view path)
{
    constexpr std::size_t max_bytes = std::size_t{256} << 20;
#ifdef _WIN32
    const std::wstring wide = widen(path);
    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    std::optional<std::vector<u8>> out;
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 && static_cast<std::size_t>(size.QuadPart) < max_bytes) {
        std::vector<u8> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size()) {
            out = std::move(bytes);
        }
    }
    ::CloseHandle(file);
    return out;
#else
    const std::string owned{path};
    FILE* f = std::fopen(owned.c_str(), "rb");
    if (f == nullptr) { return std::nullopt; }
    std::optional<std::vector<u8>> out;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long sz = std::ftell(f);
        if (sz > 0 && static_cast<std::size_t>(sz) < max_bytes && std::fseek(f, 0, SEEK_SET) == 0) {
            std::vector<u8> bytes(static_cast<std::size_t>(sz));
            if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                out = std::move(bytes);
            }
        }
    }
    std::fclose(f);
    return out;
#endif
}

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

// zeroes a buffer that held font data before it is freed
struct scrub_on_exit {
    std::vector<u8>& bytes;
    ~scrub_on_exit()
    {
        if (!bytes.empty()) { detail::secure_wipe(bytes.data(), bytes.size()); }
    }
};

#ifdef _WIN32
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

// the cmap of a font, for code points above the BMP (gdi only maps utf-16 code units)
struct cmap12 {
    std::vector<u8> data;
    std::size_t     groups_at{};
    u32             group_count{};

    [[nodiscard]] u32 lookup(char32_t cp) const noexcept
    {
        u32 lo = 0, hi = group_count;
        while (lo < hi) {
            const u32 mid = lo + (hi - lo) / 2;
            const std::size_t g = groups_at + static_cast<std::size_t>(mid) * 12;
            const u32 start = *be32(data, g);
            const u32 end   = *be32(data, g + 4);
            if (cp < start)    { hi = mid; }
            else if (cp > end) { lo = mid + 1; }
            else               { return *be32(data, g + 8) + (cp - start); }
        }
        return 0xffffu;
    }
    [[nodiscard]] bool empty() const noexcept { return group_count == 0; }
};

[[nodiscard]] cmap12 load_cmap12(HDC dc)
{
    cmap12 out;
    const DWORD tag  = 0x70616d63; // 'cmap', as GetFontData wants it (little endian)
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

const MAT2     gdi_identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
constexpr UINT gdi_bitmap_flags = GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX;

[[nodiscard]] WORD gdi_glyph_index(HDC dc, const cmap12& map, char32_t cp) noexcept
{
    if (cp > 0xffff) {
        const u32 id = map.lookup(cp);
        return id > 0xfffe ? WORD{0xffff} : static_cast<WORD>(id);
    }
    const wchar_t wc = static_cast<wchar_t>(cp);
    WORD index = 0;
    if (::GetGlyphIndicesW(dc, &wc, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) { return 0xffff; }
    return index;
}
#endif

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

// everything one font needs during build() and (when dynamic) for on-demand glyph baking
struct font_source {
    bool                         use_ttf{};
    std::vector<u8>              file_bytes;
    int                          y_shift{}; // font_config::y_offset in physical pixels
    f32                          line_height{};
    f32                          ascent{};
    std::vector<codepoint_range> merged;
    std::vector<raw_glyph>       raws;

    // portable TrueType rasteriser state
    internal::ttf_font           ttf;
    f32                          ttf_scale{};
    struct ttf_fallback {
        std::vector<u8>    bytes;
        internal::ttf_font font;
        f32                scale{};
    };
    std::vector<ttf_fallback>    ttf_fallbacks;

#ifdef _WIN32
    HANDLE                       installed{};
    HDC                          dc{};
    HFONT                        font{};
    HGDIOBJ                      previous{};
    TEXTMETRICW                  tm{};
    cmap12                       cmap;
    bool                         cmap_loaded{};

    struct gdi_fallback {
        HDC     dc{};
        HFONT   font{};
        HGDIOBJ previous{};
        cmap12  cmap;
    };
    std::vector<gdi_fallback>    gdi_fallbacks;
#endif

    font_source() = default;
    font_source(const font_source&)            = delete;
    font_source& operator=(const font_source&) = delete;

    ~font_source()
    {
#ifdef _WIN32
        for (gdi_fallback& f : gdi_fallbacks) {
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
#endif
        for (ttf_fallback& fb : ttf_fallbacks) {
            if (!fb.bytes.empty()) { detail::secure_wipe(fb.bytes.data(), fb.bytes.size()); }
        }
        if (!file_bytes.empty()) { detail::secure_wipe(file_bytes.data(), file_bytes.size()); }
    }

    // measures `cp` (and rasterises into `pixels_out` when `need_pixels` is true)
    void rasterise_cp(char32_t cp, raw_glyph& g, std::vector<u8>* pixels_out) const
    {
        g = {};
        if (cp >= 0x7f && cp <= 0x9f) { return; } // C1 controls

        if (use_ttf) {
            const internal::ttf_font* face = &ttf;
            f32 sc = ttf_scale;
            u16 gid = ttf.glyph_index(cp);
            u8  src = 0;
            for (std::size_t k = 0; gid == 0 && k < ttf_fallbacks.size(); ++k) {
                gid = ttf_fallbacks[k].font.glyph_index(cp);
                if (gid != 0) {
                    face = &ttf_fallbacks[k].font;
                    sc   = ttf_fallbacks[k].scale;
                    src  = static_cast<u8>(k + 1);
                }
            }
            if (gid == 0) { return; }
            internal::ttf_bitmap bm;
            if (!face->rasterise(gid, sc, bm, pixels_out != nullptr)) { return; }
            g.source   = src;
            g.index    = gid;
            g.present  = true;
            g.advance  = bm.advance;
            g.visible  = bm.w != 0 && bm.h != 0;
            g.w        = bm.w;
            g.h        = bm.h;
            g.origin_x = bm.origin_x;
            g.origin_y = bm.origin_y;
            if (pixels_out != nullptr && g.visible) { *pixels_out = std::move(bm.pixels); }
            return;
        }

#ifdef _WIN32
        HDC  hdc    = dc;
        WORD index  = gdi_glyph_index(dc, cmap, cp);
        u8   origin = 0;
        for (std::size_t k = 0; index == 0xffff && k < gdi_fallbacks.size(); ++k) {
            index = gdi_glyph_index(gdi_fallbacks[k].dc, gdi_fallbacks[k].cmap, cp);
            if (index != 0xffff) { hdc = gdi_fallbacks[k].dc; origin = static_cast<u8>(k + 1); }
        }
        if (index == 0xffff) { return; }

        GLYPHMETRICS gm{};
        const DWORD  size = ::GetGlyphOutlineW(hdc, index, gdi_bitmap_flags, &gm, 0, nullptr, &gdi_identity);
        if (size == GDI_ERROR) { return; }

        g.source   = origin;
        g.index    = index;
        g.present  = true;
        g.advance  = static_cast<f32>(gm.gmCellIncX);
        g.visible  = size != 0 && gm.gmBlackBoxX != 0 && gm.gmBlackBoxY != 0;
        g.w        = static_cast<u16>(gm.gmBlackBoxX);
        g.h        = static_cast<u16>(gm.gmBlackBoxY);
        g.origin_x = gm.gmptGlyphOrigin.x;
        g.origin_y = gm.gmptGlyphOrigin.y;

        if (pixels_out != nullptr && g.visible) {
            std::vector<u8> raw(size);
            const scrub_on_exit scrub{raw};
            if (::GetGlyphOutlineW(hdc, index, gdi_bitmap_flags, &gm, size, raw.data(), &gdi_identity) == GDI_ERROR) {
                g.visible = false;
                return;
            }
            const u32 pitch = (g.w + 3u) & ~3u;
            pixels_out->resize(static_cast<std::size_t>(g.w) * g.h);
            for (u32 y = 0; y < g.h; ++y) {
                for (u32 x = 0; x < g.w; ++x) {
                    const u32 level = raw[static_cast<std::size_t>(y) * pitch + x]; // 0..64
                    (*pixels_out)[static_cast<std::size_t>(y) * g.w + x] = static_cast<u8>(std::min(level * 4u, 255u));
                }
            }
        }
#endif
    }

    // rasterises a glyph already measured by rasterise_cp (used in pass 3 of build())
    [[nodiscard]] bool rasterise_measured(const raw_glyph& r, std::vector<u8>& pixels_out) const
    {
        if (!r.visible) { return false; }
        if (use_ttf) {
            const internal::ttf_font& face = r.source == 0 ? ttf : ttf_fallbacks[r.source - 1].font;
            const f32 sc = r.source == 0 ? ttf_scale : ttf_fallbacks[r.source - 1].scale;
            internal::ttf_bitmap bm;
            if (!face.rasterise(r.index, sc, bm, true) || bm.w != r.w || bm.h != r.h) { return false; }
            pixels_out = std::move(bm.pixels);
            return true;
        }
#ifdef _WIN32
        HDC glyph_dc = r.source == 0 ? dc : gdi_fallbacks[r.source - 1].dc;
        GLYPHMETRICS gm{};
        const DWORD  size = ::GetGlyphOutlineW(glyph_dc, r.index, gdi_bitmap_flags, &gm, 0, nullptr, &gdi_identity);
        if (size == GDI_ERROR || size == 0) { return false; }
        std::vector<u8> raw(size);
        const scrub_on_exit scrub{raw};
        if (::GetGlyphOutlineW(glyph_dc, r.index, gdi_bitmap_flags, &gm, size, raw.data(), &gdi_identity) == GDI_ERROR) {
            return false;
        }
        const u32 pitch = (r.w + 3u) & ~3u;
        pixels_out.resize(static_cast<std::size_t>(r.w) * r.h);
        for (u32 y = 0; y < r.h; ++y) {
            for (u32 x = 0; x < r.w; ++x) {
                const u32 level = raw[static_cast<std::size_t>(y) * pitch + x]; // 0..64
                pixels_out[static_cast<std::size_t>(y) * r.w + x] = static_cast<u8>(std::min(level * 4u, 255u));
            }
        }
        return true;
#else
        return false;
#endif
    }
};

} // namespace

struct font_atlas::dynamic_state {
    std::vector<std::unique_ptr<font_source>> sources;
    u32          pack_x{3};
    u32          pack_y{};
    u32          pack_row_h{};
    u32          max_atlas_size{4096};
    atlas_region dirty_rect{};
    bool         is_dirty{};
    bool         is_resized{};

    void mark_dirty(u32 x, u32 y, u32 w, u32 h) noexcept
    {
        if (w == 0 || h == 0) { return; }
        if (!is_dirty || dirty_rect.empty()) {
            dirty_rect = {x, y, w, h};
        } else {
            const u32 x0 = std::min(dirty_rect.x, x);
            const u32 y0 = std::min(dirty_rect.y, y);
            const u32 x1 = std::max(dirty_rect.x + dirty_rect.w, x + w);
            const u32 y1 = std::max(dirty_rect.y + dirty_rect.h, y + h);
            dirty_rect   = {x0, y0, x1 - x0, y1 - y0};
        }
        is_dirty = true;
    }
};

std::expected<font_atlas, font_error> font_atlas::build(std::span<const font_config> configs, u32 max_atlas_size,
                                                        f32 scale, bool dynamic)
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
        } else if (dynamic && !bytes.empty()) {
            // dynamic baking keeps the font alive past build(), so copy caller-borrowed bytes
            src.file_bytes.assign(bytes.begin(), bytes.end());
            bytes = src.file_bytes;
        }

#ifdef _WIN32
        src.use_ttf = cfg.rasterizer == font_rasterizer::truetype;
#else
        src.use_ttf = true;
#endif
        src.y_shift = static_cast<int>(std::lround(cfg.y_offset * scale));

        if (src.use_ttf) {
            if (bytes.empty()) {
                return std::unexpected{font_error::font_unavailable};
            }
            if (!src.ttf.init(bytes)) {
                return std::unexpected{font_error::font_invalid};
            }
            const f32 px_h = std::max(1.0f, std::round(cfg.pixel_height * scale));
            src.ttf_scale   = src.ttf.scale_for_pixel_height(px_h);
            src.ascent      = std::round(src.ttf.ascent_px(src.ttf_scale));
            src.line_height = std::max(1.0f, std::round(src.ttf.line_height_px(src.ttf_scale)));

            for (const std::string_view fb_path : cfg.fallback_faces) {
                auto loaded = read_file(fb_path);
                if (!loaded) { continue; }
                font_source::ttf_fallback fb;
                fb.bytes = std::move(*loaded);
                if (fb.font.init(fb.bytes)) {
                    fb.scale = fb.font.scale_for_pixel_height(px_h);
                    src.ttf_fallbacks.push_back(std::move(fb));
                }
            }
        } else {
#ifdef _WIN32
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
            src.font = ::CreateFontW(-std::max(1, static_cast<int>(std::lround(cfg.pixel_height * scale))), 0, 0, 0,
                                     cfg.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
            if (src.font == nullptr) {
                return std::unexpected{font_error::font_unavailable};
            }
            src.previous = ::SelectObject(src.dc, src.font);

            if (!bytes.empty() || cfg.exact_face) {
                wchar_t actual[LF_FACESIZE]{};
                ::GetTextFaceW(src.dc, LF_FACESIZE, actual);
                if (::_wcsicmp(actual, face.c_str()) != 0) {
                    return std::unexpected{font_error::font_unavailable};
                }
            }

            ::GetTextMetricsW(src.dc, &src.tm);
            src.line_height = static_cast<f32>(src.tm.tmHeight);
            src.ascent      = static_cast<f32>(src.tm.tmAscent);

            for (const std::string_view fb : cfg.fallback_faces) {
                font_source::gdi_fallback f;
                f.dc = ::CreateCompatibleDC(nullptr);
                if (f.dc == nullptr) { continue; }
                const std::wstring fb_face = widen(fb);
                f.font = ::CreateFontW(-std::max(1, static_cast<int>(std::lround(cfg.pixel_height * scale))), 0, 0, 0,
                                       cfg.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fb_face.c_str());
                if (f.font != nullptr) {
                    f.previous = ::SelectObject(f.dc, f.font);
                    wchar_t actual[LF_FACESIZE]{};
                    ::GetTextFaceW(f.dc, LF_FACESIZE, actual);
                    if (::_wcsicmp(actual, fb_face.c_str()) == 0) {
                        f.cmap = load_cmap12(f.dc);
                        src.gdi_fallbacks.push_back(std::move(f));
                        continue;
                    }
                    ::SelectObject(f.dc, f.previous);
                    ::DeleteObject(f.font);
                }
                ::DeleteDC(f.dc);
            }
#endif
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
        fd.line_height = src.line_height;
        fd.ascent      = src.ascent;

        std::size_t total = 0;
        for (const codepoint_range& r : src.merged) {
            fd.ranges.push_back({r.first, r.last, total});
            total += r.last - r.first + 1;
        }
        fd.glyphs.resize(total);
        src.raws.resize(total);

#ifdef _WIN32
        if (!src.use_ttf && (dynamic || (!src.merged.empty() && src.merged.back().last > 0xffff))) {
            src.cmap        = load_cmap12(src.dc);
            src.cmap_loaded = true;
        }
#endif

        // which glyphs exist, and how big are their bitmaps
        for (const range_entry& range : fd.ranges) {
            for (char32_t cp = range.first; cp <= range.last; ++cp) {
                src.rasterise_cp(cp, src.raws[range.offset + (cp - range.first)], nullptr);
            }
        }
    }

    // 2. pick the smallest atlas that fits every font (shelf packing, 1px padding) ---
    u32 end_cx = 3, end_cy = 0, end_row_h = 0;
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
        end_cx    = cx;
        end_cy    = cy;
        end_row_h = row_h;
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
    // dynamic atlases start with headroom when nearly full so early on-demand glyphs avoid a mid-frame resize
    if (dynamic && end_cy + end_row_h > (ah * 3) / 4) {
        const u32 nw = aw <= ah ? aw * 2 : aw;
        const u32 nh = aw <= ah ? ah : ah * 2;
        if (nw <= max_atlas_size && nh <= max_atlas_size && try_pack(nw, nh)) {
            aw = nw;
            ah = nh;
        }
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
            if (!r.visible || !src.rasterise_measured(r, scratch)) { continue; }

            for (u32 y = 0; y < r.h; ++y) {
                std::memcpy(atlas.pixels_.data() + static_cast<std::size_t>(r.y + y) * aw + r.x,
                            scratch.data() + static_cast<std::size_t>(y) * r.w, r.w);
            }

            g.x0 = static_cast<f32>(r.origin_x);
            g.y0 = static_cast<f32>(static_cast<i32>(src.ascent) - r.origin_y + src.y_shift);
            g.x1 = g.x0 + static_cast<f32>(r.w);
            g.y1 = g.y0 + static_cast<f32>(r.h);
            g.u0 = to_uv15(r.x, aw);
            g.v0 = to_uv15(r.y, ah);
            g.u1 = to_uv15(r.x + r.w, aw);
            g.v1 = to_uv15(r.y + r.h, ah);
            g.visible = true;
        }

        // kerning pairs
        if (configs[fi].kerning) {
            if (src.use_ttf) {
                std::vector<internal::ttf_kern_pair> pairs;
                src.ttf.collect_kerning(src.ttf_scale, pairs);
                fd.kern.reserve(pairs.size());
                for (const internal::ttf_kern_pair& p : pairs) { fd.kern.push_back({p.key, p.amount}); }
            } else {
#ifdef _WIN32
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
                    }
                }
#endif
            }
            if (!fd.kern.empty()) {
                std::ranges::sort(fd.kern, {}, &kern_pair::key);
                fd.kern_ascii.assign(128 * 128, 0);
                for (const kern_pair& k : fd.kern) {
                    const u32 left = k.key >> 16, right = k.key & 0xffffu;
                    if (left >= 128 || right >= 128) { continue; }
                    const bool fits = k.amount == std::round(k.amount) && k.amount > -128.0f && k.amount <= 127.0f;
                    fd.kern_ascii[left * 128 + right] = fits ? static_cast<std::int8_t>(k.amount) : kern_ask;
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

        src.raws.clear();
        src.raws.shrink_to_fit();
    }

    if (dynamic) {
        auto d = std::make_shared<dynamic_state>();
        d->sources        = std::move(sources);
        d->pack_x         = end_cx;
        d->pack_y         = end_cy;
        d->pack_row_h     = end_row_h;
        d->max_atlas_size = max_atlas_size;
        atlas.dyn_        = std::move(d);
    }

    return atlas;
}

bool font_atlas::grow_atlas(u32 min_w, u32 min_h) const
{
    if (dyn_ == nullptr || pixels_.empty()) { return false; }
    u32 nw = width_;
    u32 nh = height_;
    while (nw < min_w || nh < min_h) {
        if (nw <= nh) { nw *= 2; } else { nh *= 2; }
        if (nw > dyn_->max_atlas_size || nh > dyn_->max_atlas_size) { return false; }
    }
    if (nw == width_ && nh == height_) { return true; }

    std::vector<u8> grown(static_cast<std::size_t>(nw) * nh, 0);
    for (u32 y = 0; y < height_; ++y) {
        std::memcpy(grown.data() + static_cast<std::size_t>(y) * nw,
                    pixels_.data() + static_cast<std::size_t>(y) * width_, width_);
    }

    const auto remap = [&](glyph& g) {
        if (!g.visible) { return; }
        g.u0 = to_uv15(from_uv15(g.u0, width_), nw);
        g.u1 = to_uv15(from_uv15(g.u1, width_), nw);
        g.v0 = to_uv15(from_uv15(g.v0, height_), nh);
        g.v1 = to_uv15(from_uv15(g.v1, height_), nh);
    };
    for (font_data& fd : fonts_) {
        for (glyph& g : fd.glyphs) { remap(g); }
        for (dyn_entry& e : fd.dyn_glyphs) { remap(e.g); }
    }

    detail::secure_wipe(pixels_.data(), pixels_.size());
    pixels_          = std::move(grown);
    width_           = nw;
    height_          = nh;
    white_uv_        = {to_uv15(1, nw), to_uv15(1, nh)};
    dyn_->is_resized = true;
    dyn_->is_dirty   = true;
    dyn_->dirty_rect = {0, 0, nw, nh};
    return true;
}

bool font_atlas::bake_glyph(font_id f, char32_t cp) const
{
    if (dyn_ == nullptr || f >= fonts_.size() || pixels_.empty() || cp < 0x20 || cp > 0x10ffff) {
        return false;
    }
    font_data& fd = fonts_[f];
    // already in pre-baked ranges?
    {
        const auto it = std::ranges::upper_bound(fd.ranges, cp, {}, &range_entry::first);
        if (it != fd.ranges.begin()) {
            const range_entry& r = *(it - 1);
            if (cp <= r.last && fd.glyphs[r.offset + (cp - r.first)].present) { return false; }
        }
    }
    // already baked (or cached missing) dynamically?
    const auto dit = std::ranges::lower_bound(fd.dyn_glyphs, cp, {}, &dyn_entry::cp);
    if (dit != fd.dyn_glyphs.end() && dit->cp == cp) {
        return false;
    }

    const font_source& src = *dyn_->sources[f];
    raw_glyph          r;
    std::vector<u8>    px;
    const scrub_on_exit scrub{px};
    src.rasterise_cp(cp, r, &px);

    glyph g{};
    g.present = r.present;
    g.advance = r.advance;
    if (r.present && r.visible) {
        if (dyn_->pack_x + r.w + 1 > width_) {
            dyn_->pack_x     = 0;
            dyn_->pack_y    += dyn_->pack_row_h + 1;
            dyn_->pack_row_h = 0;
        }
        if (dyn_->pack_y + r.h > height_ || r.w > width_) {
            if (!grow_atlas(std::max(width_, static_cast<u32>(r.w) + 1), dyn_->pack_y + r.h)) {
                g.present = false;
                fd.dyn_glyphs.insert(dit, {cp, g});
                return false;
            }
        }
        const u32 gx = dyn_->pack_x;
        const u32 gy = dyn_->pack_y;
        for (u32 y = 0; y < r.h; ++y) {
            std::memcpy(pixels_.data() + static_cast<std::size_t>(gy + y) * width_ + gx,
                        px.data() + static_cast<std::size_t>(y) * r.w, r.w);
        }
        dyn_->pack_x    += r.w + 1;
        dyn_->pack_row_h = std::max<u32>(dyn_->pack_row_h, r.h);
        dyn_->mark_dirty(gx, gy, r.w, r.h);

        g.x0      = static_cast<f32>(r.origin_x);
        g.y0      = static_cast<f32>(static_cast<i32>(src.ascent) - r.origin_y + src.y_shift);
        g.x1      = g.x0 + static_cast<f32>(r.w);
        g.y1      = g.y0 + static_cast<f32>(r.h);
        g.u0      = to_uv15(gx, width_);
        g.v0      = to_uv15(gy, height_);
        g.u1      = to_uv15(gx + r.w, width_);
        g.v1      = to_uv15(gy + r.h, height_);
        g.visible = true;
    }

    fd.dyn_glyphs.insert(dit, {cp, g});
    return g.visible;
}

bool font_atlas::bake_text(font_id f, std::string_view utf8) const
{
    bool added = false;
    while (!utf8.empty()) {
        const char32_t cp = decode_utf8(utf8);
        if (bake_glyph(f, cp)) { added = true; }
    }
    return added;
}

bool font_atlas::dirty() const noexcept { return dyn_ != nullptr && dyn_->is_dirty; }
bool font_atlas::resized() const noexcept { return dyn_ != nullptr && dyn_->is_resized; }
atlas_region font_atlas::dirty_region() const noexcept { return dyn_ != nullptr ? dyn_->dirty_rect : atlas_region{}; }
void font_atlas::clear_dirty() const noexcept
{
    if (dyn_ != nullptr) {
        dyn_->is_dirty   = false;
        dyn_->is_resized = false;
        dyn_->dirty_rect = {};
    }
}

font_atlas::~font_atlas()
{
    discard_pixels();
}

namespace {

constexpr u32 atlas_magic = 0x31414653; // "SFA1"

struct byte_writer {
    std::vector<u8>& out;
    template <class T> void pod(const T& v)
    {
        const auto* p = reinterpret_cast<const u8*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    template <class T> void vec(const std::vector<T>& v)
    {
        pod<u64>(v.size());
        const auto* p = reinterpret_cast<const u8*>(v.data());
        out.insert(out.end(), p, p + v.size() * sizeof(T));
    }
};

struct byte_reader {
    std::span<const u8> in;
    std::size_t         pos{};
    bool                ok{true};
    template <class T> T pod()
    {
        T v{};
        if (!ok || in.size() - pos < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    template <class T> void vec(std::vector<T>& v)
    {
        const u64 n = pod<u64>();
        if (!ok || n > (in.size() - pos) / sizeof(T)) { ok = false; return; }
        v.resize(static_cast<std::size_t>(n));
        std::memcpy(static_cast<void*>(v.data()), in.data() + pos, v.size() * sizeof(T));
        pos += v.size() * sizeof(T);
    }
};

} // namespace

std::vector<u8> font_atlas::save() const
{
    std::vector<u8> out;
    byte_writer     w{out};
    w.pod(atlas_magic);
    w.pod<u32>(sizeof(glyph));
    w.pod<u32>(sizeof(range_entry));
    w.pod<u32>(sizeof(kern_pair));
    w.pod(width_);
    w.pod(height_);
    w.pod(scale_);
    w.pod(inv_scale_);
    w.pod(white_uv_);
    w.pod<u64>(fonts_.size());
    for (const font_data& f : fonts_) {
        w.vec(f.ranges);
        w.vec(f.glyphs);
        w.vec(f.kern);
        w.vec(f.kern_ascii);
        w.pod<u64>(f.fallback);
        w.pod(f.line_height);
        w.pod(f.ascent);
    }
    w.vec(pixels_);
    return out;
}

std::optional<font_atlas> font_atlas::load(std::span<const u8> bytes)
{
    byte_reader r{bytes};
    if (r.pod<u32>() != atlas_magic || r.pod<u32>() != sizeof(glyph) || r.pod<u32>() != sizeof(range_entry) || r.pod<u32>() != sizeof(kern_pair)) {
        return std::nullopt;
    }
    font_atlas a;
    a.width_     = r.pod<u32>();
    a.height_    = r.pod<u32>();
    a.scale_     = r.pod<f32>();
    a.inv_scale_ = r.pod<f32>();
    a.white_uv_  = r.pod<std::array<u16, 2>>();
    const u64 count = r.pod<u64>();
    if (!r.ok || count == 0 || count > 64) { return std::nullopt; }
    a.fonts_.resize(static_cast<std::size_t>(count));
    for (font_data& f : a.fonts_) {
        r.vec(f.ranges);
        r.vec(f.glyphs);
        r.vec(f.kern);
        r.vec(f.kern_ascii);
        f.fallback    = static_cast<std::size_t>(r.pod<u64>());
        f.line_height = r.pod<f32>();
        f.ascent      = r.pod<f32>();
        if (!r.ok || f.glyphs.empty() || f.fallback >= f.glyphs.size()) { return std::nullopt; }
        for (const range_entry& re : f.ranges) { // find() indexes the glyph table straight from these
            if (re.last < re.first || re.offset + (re.last - re.first) >= f.glyphs.size()) { return std::nullopt; }
        }
    }
    r.vec(a.pixels_);
    if (!r.ok || r.pos != bytes.size() || a.pixels_.size() != static_cast<std::size_t>(a.width_) * a.height_) { return std::nullopt; }
    return a;
}

void font_atlas::discard_pixels() noexcept
{
    if (!pixels_.empty()) {
        detail::secure_wipe(pixels_.data(), pixels_.size());
    }
    std::vector<u8>().swap(pixels_);
}

std::size_t font_atlas::glyph_count() const noexcept
{
    std::size_t n = 0;
    for (const font_data& f : fonts_) { n += f.glyphs.size() + f.dyn_glyphs.size(); }
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

const glyph& font_atlas::lookup_slow(font_id f, char32_t cp) const noexcept
{
    static constexpr glyph nothing{0, 0, 0, 0, 0, 0, 0, 0, 0, false, true};
    if (is_ignorable(cp)) { return nothing; }
    const font_data& fd = fonts_[f];
    // last range whose `first` is <= cp
    const auto it = std::ranges::upper_bound(fd.ranges, cp, {}, &range_entry::first);
    if (it != fd.ranges.begin()) {
        const range_entry& r = *(it - 1);
        if (cp <= r.last) {
            const glyph& g = fd.glyphs[r.offset + (cp - r.first)];
            if (g.present) { return g; }
        }
    }
    if (!fd.dyn_glyphs.empty()) {
        const auto dit = std::ranges::lower_bound(fd.dyn_glyphs, cp, {}, &dyn_entry::cp);
        if (dit != fd.dyn_glyphs.end() && dit->cp == cp) {
            return dit->g.present ? dit->g : fd.glyphs[fd.fallback];
        }
    }
    if (dyn_ != nullptr) {
        (void)bake_glyph(f, cp);
        const auto dit = std::ranges::lower_bound(fd.dyn_glyphs, cp, {}, &dyn_entry::cp);
        if (dit != fd.dyn_glyphs.end() && dit->cp == cp) {
            return dit->g.present ? dit->g : fd.glyphs[fd.fallback];
        }
    }
    return fd.glyphs[fd.fallback];
}

bool font_atlas::has_glyph(font_id f, char32_t cp) const noexcept
{
    const font_data& fd = fonts_[f];
    const auto it = std::ranges::upper_bound(fd.ranges, cp, {}, &range_entry::first);
    if (it != fd.ranges.begin()) {
        const range_entry& r = *(it - 1);
        if (cp <= r.last && fd.glyphs[r.offset + (cp - r.first)].present) { return true; }
    }
    if (!fd.dyn_glyphs.empty()) {
        const auto dit = std::ranges::lower_bound(fd.dyn_glyphs, cp, {}, &dyn_entry::cp);
        if (dit != fd.dyn_glyphs.end() && dit->cp == cp) { return dit->g.present; }
    }
    return false;
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

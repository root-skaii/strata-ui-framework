#include "internal.hpp"

namespace strata::overlay::detail {

namespace {

// ---- png (stored deflate blocks: big, simple, enough for a debug capture) -----------------------------------

[[nodiscard]] u32 crc32(const u8* p, std::size_t n, u32 crc = 0) noexcept
{
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1; }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8); }
    return ~crc;
}

void put_be32(std::vector<u8>& v, u32 x)
{
    v.push_back(static_cast<u8>(x >> 24)); v.push_back(static_cast<u8>(x >> 16)); v.push_back(static_cast<u8>(x >> 8)); v.push_back(static_cast<u8>(x));
}

void png_chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
{
    put_be32(out, static_cast<u32>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put_be32(out, crc32(out.data() + start, out.size() - start));
}

} // namespace

[[nodiscard]] bool write_png(const char* path, const std::vector<u8>& rgba, u32 w, u32 h)
{
    std::vector<u8> raw;
    raw.reserve((static_cast<std::size_t>(w) * 4 + 1) * h);
    for (u32 y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4, rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4);
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (const u8 c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    for (std::size_t pos = 0; pos < raw.size(); pos += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<u8>(n)); z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n)); z.push_back(static_cast<u8>((~n) >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    }
    put_be32(z, (b << 16) | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<u8> ihdr;
    put_be32(ihdr, w); put_be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) { return false; }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// copies the back buffer (with the ui on it) out and writes it as a png
void capture_back_buffer(IDXGISwapChain* sc)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) { return; }
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!bgra && !rgba) { set_error("capture: back buffer format not supported"); return; }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &staging))) { return; }
    g.ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { return; }
    std::vector<u8> px(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        const u8* src = static_cast<const u8*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        u8* dst = px.data() + static_cast<std::size_t>(y) * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            dst[4 * x]     = bgra ? src[4 * x + 2] : src[4 * x];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = bgra ? src[4 * x] : src[4 * x + 2];
            dst[4 * x + 3] = 255;
        }
    }
    g.ctx->Unmap(staging.Get(), 0);
    if (!write_png(g.opt.capture_path.c_str(), px, desc.Width, desc.Height)) { set_error("capture: cannot write the png"); }
}

} // namespace strata::overlay::detail

#include "imgio.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace imgio {

using strata::u16;
using strata::u32;
using strata::u64;
using strata::u8;

namespace {

[[nodiscard]] std::wstring widen(std::string_view s)
{
    std::wstring out(s.size(), L'\0');
    if (!s.empty()) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), static_cast<int>(out.size()));
        out.resize(static_cast<std::size_t>(n > 0 ? n : 0));
    }
    return out;
}

[[nodiscard]] u32 crc32(const u8* data, std::size_t n, u32 crc = 0) noexcept
{
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1; }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8); }
    return ~crc;
}

[[nodiscard]] u32 adler32(const u8* data, std::size_t n) noexcept
{
    u32 a = 1;
    u32 b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<u8>& v, u32 x)
{
    v.push_back(static_cast<u8>(x >> 24));
    v.push_back(static_cast<u8>(x >> 16));
    v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x));
}

void put_chunk(std::vector<u8>& file, const char (&type)[5], const std::vector<u8>& data)
{
    put_be32(file, static_cast<u32>(data.size()));
    const std::size_t at = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), data.begin(), data.end());
    put_be32(file, crc32(file.data() + at, file.size() - at));
}

// --- deflate: greedy lz77 + the fixed huffman code -----------------------------------------------------------------

struct bit_writer {
    std::vector<u8>& out;
    u32 acc{};
    int nbits{};

    void put(u32 value, int n)
    {
        acc |= value << nbits;
        nbits += n;
        while (nbits >= 8) {
            out.push_back(static_cast<u8>(acc & 0xff));
            acc >>= 8;
            nbits -= 8;
        }
    }
    void code(u32 c, int n) // huffman codes go out most significant bit first
    {
        u32 rev = 0;
        for (int i = 0; i < n; ++i) { rev |= ((c >> i) & 1u) << (n - 1 - i); }
        put(rev, n);
    }
    void flush()
    {
        if (nbits > 0) {
            out.push_back(static_cast<u8>(acc & 0xff));
            acc   = 0;
            nbits = 0;
        }
    }
};

constexpr std::array<u16, 29> length_base  = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<u8, 29>  length_extra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<u16, 30> dist_base    = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<u8, 30>  dist_extra   = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_literal_length(bit_writer& w, u32 sym)
{
    if (sym < 144)      { w.code(0x30 + sym, 8); }
    else if (sym < 256) { w.code(0x190 + (sym - 144), 9); }
    else if (sym < 280) { w.code(sym - 256, 7); }
    else                { w.code(0xc0 + (sym - 280), 8); }
}

[[nodiscard]] std::vector<u8> zlib_compress(const std::vector<u8>& data)
{
    std::vector<u8> out;
    out.reserve(data.size() / 4 + 64);
    out.push_back(0x78);
    out.push_back(0x01);

    bit_writer w{out};
    w.put(1, 1); // last block
    w.put(1, 2); // fixed huffman

    const std::size_t n = data.size();
    std::vector<u32> head(1u << 16, 0);
    std::vector<u32> prev(n, 0);
    const auto hash = [&](std::size_t i) -> u32 {
        return ((u32{data[i]} * 2654435761u) ^ (u32{data[i + 1]} * 40503u) ^ (u32{data[i + 2]} * 9973u)) >> 16 & 0xffffu;
    };
    const auto insert = [&](std::size_t i) {
        if (i + 2 < n) {
            const u32 h = hash(i);
            prev[i] = head[h];
            head[h] = static_cast<u32>(i + 1);
        }
    };

    std::size_t i = 0;
    while (i < n) {
        std::size_t best_len  = 0;
        std::size_t best_dist = 0;
        if (i + 2 < n) {
            u32 cand = head[hash(i)];
            for (int chain = 0; cand != 0 && chain < 48; ++chain) {
                const std::size_t pos  = cand - 1;
                const std::size_t dist = i - pos;
                if (dist > 32768) { break; }
                const std::size_t max_len = std::min<std::size_t>(258, n - i);
                std::size_t len = 0;
                while (len < max_len && data[pos + len] == data[i + len]) { ++len; }
                if (len > best_len) {
                    best_len  = len;
                    best_dist = dist;
                    if (len == max_len) { break; }
                }
                cand = prev[pos];
            }
        }

        if (best_len >= 3) {
            std::size_t lc = 28;
            while (length_base[lc] > best_len) { --lc; }
            put_literal_length(w, static_cast<u32>(257 + lc));
            w.put(static_cast<u32>(best_len - length_base[lc]), length_extra[lc]);
            std::size_t dc = 29;
            while (dist_base[dc] > best_dist) { --dc; }
            w.code(static_cast<u32>(dc), 5);
            w.put(static_cast<u32>(best_dist - dist_base[dc]), dist_extra[dc]);
            for (std::size_t k = 0; k < best_len; ++k) { insert(i + k); }
            i += best_len;
        } else {
            put_literal_length(w, data[i]);
            insert(i);
            ++i;
        }
    }
    put_literal_length(w, 256);
    w.flush();
    put_be32(out, adler32(data.data(), data.size()));
    return out;
}

// --- inflate (after puff.c) ------------------------------------------------------------------------------------------

struct huffman {
    std::array<u16, 16>  count{};
    std::array<u16, 288> symbol{};
};

struct inflater {
    const u8*        in{};
    std::size_t      size{};
    std::size_t      pos{};
    u32              bitbuf{};
    int              bitcnt{};
    std::vector<u8>& out;
    bool             err{};

    int bits(int need)
    {
        u32 val = bitbuf;
        while (bitcnt < need) {
            if (pos >= size) { err = true; return 0; }
            val |= u32{in[pos++]} << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    static bool construct(huffman& h, const u16* length, int n)
    {
        h.count.fill(0);
        for (int s = 0; s < n; ++s) { ++h.count[length[s]]; }
        if (h.count[0] == n) { return true; } // no codes: complete, but nothing to decode
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) { return false; } // over-subscribed
        }
        std::array<u16, 16> offs{};
        for (int len = 1; len < 15; ++len) { offs[len + 1] = static_cast<u16>(offs[len] + h.count[len]); }
        for (int s = 0; s < n; ++s) {
            if (length[s] != 0) { h.symbol[offs[length[s]]++] = static_cast<u16>(s); }
        }
        return true;
    }

    int decode(const huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= bits(1);
            if (err) { return -1; }
            const int count = h.count[len];
            if (code - count < first) { return h.symbol[index + (code - first)]; }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        err = true;
        return -1;
    }

    bool codes(const huffman& lencode, const huffman& distcode)
    {
        for (;;) {
            int sym = decode(lencode);
            if (err || sym < 0) { return false; }
            if (sym < 256) {
                out.push_back(static_cast<u8>(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) { return false; }
                const int len  = length_base[static_cast<std::size_t>(sym)] + bits(length_extra[static_cast<std::size_t>(sym)]);
                const int dsym = decode(distcode);
                if (err || dsym < 0 || dsym >= 30) { return false; }
                const std::size_t dist = dist_base[static_cast<std::size_t>(dsym)] + static_cast<std::size_t>(bits(dist_extra[static_cast<std::size_t>(dsym)]));
                if (err || dist > out.size()) { return false; }
                for (int k = 0; k < len; ++k) { out.push_back(out[out.size() - dist]); }
            }
        }
    }

    bool run()
    {
        int last = 0;
        do {
            last = bits(1);
            const int type = bits(2);
            if (err) { return false; }
            if (type == 0) {
                bitbuf = 0;
                bitcnt = 0;
                if (pos + 4 > size) { return false; }
                const u32 len  = u32{in[pos]} | (u32{in[pos + 1]} << 8);
                const u32 nlen = u32{in[pos + 2]} | (u32{in[pos + 3]} << 8);
                pos += 4;
                if ((len ^ 0xffffu) != nlen || pos + len > size) { return false; }
                out.insert(out.end(), in + pos, in + pos + len);
                pos += len;
            } else if (type == 1) {
                static const std::pair<huffman, huffman> fixed = [] {
                    huffman l, d;
                    std::array<u16, 288> lengths{};
                    for (int s = 0; s < 144; ++s) { lengths[static_cast<std::size_t>(s)] = 8; }
                    for (int s = 144; s < 256; ++s) { lengths[static_cast<std::size_t>(s)] = 9; }
                    for (int s = 256; s < 280; ++s) { lengths[static_cast<std::size_t>(s)] = 7; }
                    for (int s = 280; s < 288; ++s) { lengths[static_cast<std::size_t>(s)] = 8; }
                    (void)construct(l, lengths.data(), 288);
                    std::array<u16, 30> dl{};
                    dl.fill(5);
                    (void)construct(d, dl.data(), 30);
                    return std::pair{l, d};
                }();
                if (!codes(fixed.first, fixed.second)) { return false; }
            } else if (type == 2) {
                const int nlen  = bits(5) + 257;
                const int ndist = bits(5) + 1;
                const int ncode = bits(4) + 4;
                if (err || nlen > 286 || ndist > 30) { return false; }
                static constexpr std::array<u8, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::array<u16, 320> lengths{};
                for (int i = 0; i < ncode; ++i) { lengths[order[static_cast<std::size_t>(i)]] = static_cast<u16>(bits(3)); }
                if (err) { return false; }
                huffman lencode;
                if (!construct(lencode, lengths.data(), 19)) { return false; }
                int idx = 0;
                std::array<u16, 320> ll{};
                while (idx < nlen + ndist) {
                    int sym = decode(lencode);
                    if (err || sym < 0) { return false; }
                    if (sym < 16) {
                        ll[static_cast<std::size_t>(idx++)] = static_cast<u16>(sym);
                    } else {
                        u16 len = 0;
                        int rep = 0;
                        if (sym == 16) {
                            if (idx == 0) { return false; }
                            len = ll[static_cast<std::size_t>(idx - 1)];
                            rep = 3 + bits(2);
                        } else if (sym == 17) {
                            rep = 3 + bits(3);
                        } else {
                            rep = 11 + bits(7);
                        }
                        if (err || idx + rep > nlen + ndist) { return false; }
                        while (rep-- > 0) { ll[static_cast<std::size_t>(idx++)] = len; }
                    }
                }
                huffman l, d;
                if (!construct(l, ll.data(), nlen) || !construct(d, ll.data() + nlen, ndist)) { return false; }
                if (!codes(l, d)) { return false; }
            } else {
                return false;
            }
        } while (last == 0);
        return !err;
    }
};

[[nodiscard]] bool zlib_decompress(const std::vector<u8>& z, std::vector<u8>& out)
{
    if (z.size() < 6 || (z[0] & 0x0f) != 8) { return false; }
    inflater inf{z.data() + 2, z.size() - 2, 0, 0, 0, out, false};
    return inf.run();
}

[[nodiscard]] bool read_all(std::string_view path, std::vector<u8>& out)
{
    const std::wstring wide = widen(path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), L"rb") != 0 || f == nullptr) { return false; }
    std::array<u8, 65536> buf;
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) { break; }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    std::fclose(f);
    return true;
}

} // namespace

bool write_png(std::string_view path, std::span<const u8> rgba, u32 width, u32 height)
{
    if (width == 0 || height == 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4) { return false; }

    // rgb rows with the "sub" filter (difference to the left pixel): flat areas become zeros
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    std::vector<u8> raw((row + 1) * height);
    for (u32 y = 0; y < height; ++y) {
        u8* dst = raw.data() + (row + 1) * y;
        const u8* src = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        dst[0] = 1;
        for (u32 x = 0; x < width; ++x) {
            for (u32 c = 0; c < 3; ++c) {
                const u8 cur  = src[x * 4 + c];
                const u8 left = x > 0 ? src[(x - 1) * 4 + c] : u8{0};
                dst[1 + x * 3 + c] = static_cast<u8>(cur - left);
            }
        }
    }

    std::vector<u8> file = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<u8> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8 bit, rgb, deflate, adaptive filter, no interlace
    put_chunk(file, "IHDR", ihdr);
    put_chunk(file, "IDAT", zlib_compress(raw));
    put_chunk(file, "IEND", {});

    const std::wstring wide = widen(path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wide.c_str(), L"wb") != 0 || f == nullptr) { return false; }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    return std::fclose(f) == 0 && ok;
}

bool read_png(std::string_view path, std::vector<u8>& rgba, u32& width, u32& height)
{
    std::vector<u8> file;
    if (!read_all(path, file) || file.size() < 33 || std::memcmp(file.data(), "\x89PNG\r\n\x1a\n", 8) != 0) { return false; }

    const auto be32 = [&](std::size_t at) {
        return (u32{file[at]} << 24) | (u32{file[at + 1]} << 16) | (u32{file[at + 2]} << 8) | u32{file[at + 3]};
    };
    u32 w = 0, h = 0;
    u8 depth = 0, ctype = 0, interlace = 0;
    std::vector<u8> idat;
    for (std::size_t at = 8; at + 12 <= file.size();) {
        const u32 len = be32(at);
        if (at + 12 + len > file.size()) { return false; }
        const char* type = reinterpret_cast<const char*>(file.data() + at + 4);
        const u8* body = file.data() + at + 8;
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = be32(at + 8);
            h = be32(at + 12);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + len;
    }
    if (w == 0 || h == 0 || depth != 8 || interlace != 0 || (ctype != 2 && ctype != 6)) { return false; }
    const std::size_t bpp = ctype == 6 ? 4 : 3;

    std::vector<u8> raw;
    raw.reserve((static_cast<std::size_t>(w) * bpp + 1) * h);
    if (!zlib_decompress(idat, raw) || raw.size() < (static_cast<std::size_t>(w) * bpp + 1) * h) { return false; }

    const std::size_t row = static_cast<std::size_t>(w) * bpp;
    std::vector<u8> pix(row * h);
    for (u32 y = 0; y < h; ++y) {
        const u8 filter = raw[(row + 1) * y];
        const u8* src = raw.data() + (row + 1) * y + 1;
        u8* dst = pix.data() + row * y;
        const u8* up = y > 0 ? pix.data() + row * (y - 1) : nullptr;
        for (std::size_t i = 0; i < row; ++i) {
            const int a = i >= bpp ? dst[i - bpp] : 0;
            const int b = up != nullptr ? up[i] : 0;
            const int c = (up != nullptr && i >= bpp) ? up[i - bpp] : 0;
            int predictor = 0;
            switch (filter) {
            case 0: predictor = 0; break;
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p  = a + b - c;
                const int pa = std::abs(p - a);
                const int pb = std::abs(p - b);
                const int pc = std::abs(p - c);
                predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: return false;
            }
            dst[i] = static_cast<u8>(src[i] + predictor);
        }
    }

    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        rgba[i * 4 + 0] = pix[i * bpp + 0];
        rgba[i * 4 + 1] = pix[i * bpp + 1];
        rgba[i * 4 + 2] = pix[i * bpp + 2];
        rgba[i * 4 + 3] = bpp == 4 ? pix[i * bpp + 3] : u8{255};
    }
    width  = w;
    height = h;
    return true;
}

compare_result compare(std::span<const u8> a, std::span<const u8> b, u32 width, u32 height, u32 tolerance)
{
    compare_result r;
    const std::size_t n = static_cast<std::size_t>(width) * height;
    r.same_size = a.size() >= n * 4 && b.size() >= n * 4;
    if (!r.same_size) { return r; }
    r.pixels = n;
    for (std::size_t i = 0; i < n; ++i) {
        u32 worst = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const int d = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
            worst = std::max(worst, static_cast<u32>(d));
        }
        r.max_difference = std::max(r.max_difference, worst);
        if (worst > tolerance) { ++r.differing; }
    }
    return r;
}

std::vector<u8> diff_image(std::span<const u8> a, std::span<const u8> b, u32 width, u32 height)
{
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<u8> out(n * 4, 255);
    for (std::size_t i = 0; i < n; ++i) {
        int worst = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c])));
        }
        if (worst == 0) {
            const int gray = (a[i * 4] + a[i * 4 + 1] + a[i * 4 + 2]) / 3 / 4; // unchanged: a dark ghost of the picture
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = static_cast<u8>(gray);
        } else {
            out[i * 4 + 0] = 255;
            out[i * 4 + 1] = static_cast<u8>(std::max(0, 200 - worst * 8));
            out[i * 4 + 2] = 0;
        }
    }
    return out;
}

} // namespace imgio

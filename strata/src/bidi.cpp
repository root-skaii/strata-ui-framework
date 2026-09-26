#include "strata/bidi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace strata {

namespace {

enum class bc : u8 { L, R, AL, EN, ES, ET, AN, CS, NSM, WS, ON };

[[nodiscard]] bool in(char32_t c, char32_t a, char32_t b) noexcept { return c >= a && c <= b; }

// bidi class of a code point (a working approximation of the Unicode database: what text in the common scripts needs)
[[nodiscard]] bc classify(char32_t c) noexcept
{
    if (c < 0x0590) {
        if (c < 0x80) {
            if (c == 0x20 || c == 0x09 || c == 0x0b || c == 0x0c) { return bc::WS; }
            if (in(c, 0x30, 0x39)) { return bc::EN; }
            if (c == '+' || c == '-') { return bc::ES; }
            if (in(c, 0x23, 0x25)) { return bc::ET; }
            if (c == ',' || c == '.' || c == '/' || c == ':') { return bc::CS; }
            if (in(c, 'a', 'z') || in(c, 'A', 'Z')) { return bc::L; }
            return bc::ON;
        }
        if (c == 0xa0) { return bc::CS; }
        if (in(c, 0xa2, 0xa5) || c == 0xb0 || c == 0xb1) { return bc::ET; }
        if (c == 0xb2 || c == 0xb3 || c == 0xb9) { return bc::EN; }
        if (c == 0xaa || c == 0xb5 || c == 0xba || c >= 0xc0) { return c == 0xd7 || c == 0xf7 ? bc::ON : (in(c, 0x300, 0x36f) ? bc::NSM : bc::L); }
        return bc::ON;
    }
    // hebrew
    if (in(c, 0x0590, 0x05ff)) {
        if (in(c, 0x0591, 0x05bd) || c == 0x05bf || c == 0x05c1 || c == 0x05c2 || c == 0x05c4 || c == 0x05c5 || c == 0x05c7) { return bc::NSM; }
        return bc::R;
    }
    // arabic
    if (in(c, 0x0600, 0x07bf)) {
        if (in(c, 0x0660, 0x0669) || c == 0x066b || c == 0x066c || c == 0x0600 || c == 0x0601 || c == 0x0602 || c == 0x0603 || c == 0x0605 || c == 0x06dd) { return bc::AN; }
        if (in(c, 0x06f0, 0x06f9)) { return bc::EN; }
        if (c == 0x066a) { return bc::ET; }
        if (c == 0x060c) { return bc::CS; }
        if (in(c, 0x0610, 0x061a) || in(c, 0x064b, 0x065f) || c == 0x0670 || in(c, 0x06d6, 0x06dc) || in(c, 0x06df, 0x06e4) || c == 0x06e7 ||
            c == 0x06e8 || in(c, 0x06ea, 0x06ed) || c == 0x0711 || in(c, 0x0730, 0x074a) || in(c, 0x07a6, 0x07b0)) { return bc::NSM; }
        if (c == 0x061b || c == 0x061c || in(c, 0x061d, 0x064a) || in(c, 0x066d, 0x066f) || in(c, 0x0671, 0x06d5) || c == 0x06e5 || c == 0x06e6 ||
            c == 0x06ee || c == 0x06ef || in(c, 0x06fa, 0x070d) || c == 0x070f || c == 0x0710 || in(c, 0x0712, 0x072f) || in(c, 0x074d, 0x07a5) ||
            c == 0x07b1) { return bc::AL; }
        return bc::ON;
    }
    if (in(c, 0x07c0, 0x085f)) { return in(c, 0x07eb, 0x07f3) || in(c, 0x0816, 0x082d) ? bc::NSM : bc::R; }        // n'ko, samaritan, mandaic
    if (in(c, 0x0860, 0x08ff)) { return in(c, 0x0898, 0x08e1) || in(c, 0x08e3, 0x08ff) ? bc::NSM : bc::AL; }        // syriac supplement, arabic ext
    if (c == 0x200e) { return bc::L; }
    if (c == 0x200f) { return bc::R; }
    if (in(c, 0x200b, 0x200d) || in(c, 0x202a, 0x202e) || in(c, 0x2060, 0x206f)) { return bc::ON; }
    if (in(c, 0x2000, 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000) { return bc::WS; }
    if (in(c, 0x20a0, 0x20cf)) { return bc::ET; }
    if (in(c, 0x0300, 0x036f) || in(c, 0x20d0, 0x20ff) || in(c, 0xfe20, 0xfe2f)) { return bc::NSM; }
    if (in(c, 0xfb1d, 0xfb4f)) { return in(c, 0xfb1e, 0xfb1e) ? bc::NSM : (c == 0xfb29 ? bc::ES : bc::R); }
    if (in(c, 0xfb50, 0xfdff) || in(c, 0xfe70, 0xfefe)) { return in(c, 0xfd3e, 0xfd3f) ? bc::ON : bc::AL; }
    if (in(c, 0xff10, 0xff19)) { return bc::EN; }
    if (in(c, 0x10800, 0x10fff) || in(c, 0x1e800, 0x1efff)) { return in(c, 0x1ee00, 0x1eeff) ? bc::AL : bc::R; }
    if (in(c, 0x2190, 0x2bff) || in(c, 0x3001, 0x3004) || in(c, 0x3008, 0x3020) || in(c, 0x1f000, 0x1faff)) { return bc::ON; } // arrows, symbols, cjk punctuation, emoji
    return bc::L;
}

[[nodiscard]] bool is_strong_rtl(bc t) noexcept { return t == bc::R || t == bc::AL; }

// ---- arabic joining -------------------------------------------------------------------------------------------------

struct arabic_form {
    char32_t base;
    char32_t iso, fin, init, med; // presentation forms B / A; 0: the letter has no such form
};

constexpr std::array<arabic_form, 43> forms = {{
    {0x0621, 0xfe80, 0, 0, 0},
    {0x0622, 0xfe81, 0xfe82, 0, 0}, {0x0623, 0xfe83, 0xfe84, 0, 0}, {0x0624, 0xfe85, 0xfe86, 0, 0}, {0x0625, 0xfe87, 0xfe88, 0, 0},
    {0x0626, 0xfe89, 0xfe8a, 0xfe8b, 0xfe8c}, {0x0627, 0xfe8d, 0xfe8e, 0, 0},
    {0x0628, 0xfe8f, 0xfe90, 0xfe91, 0xfe92}, {0x0629, 0xfe93, 0xfe94, 0, 0},
    {0x062a, 0xfe95, 0xfe96, 0xfe97, 0xfe98}, {0x062b, 0xfe99, 0xfe9a, 0xfe9b, 0xfe9c}, {0x062c, 0xfe9d, 0xfe9e, 0xfe9f, 0xfea0},
    {0x062d, 0xfea1, 0xfea2, 0xfea3, 0xfea4}, {0x062e, 0xfea5, 0xfea6, 0xfea7, 0xfea8},
    {0x062f, 0xfea9, 0xfeaa, 0, 0}, {0x0630, 0xfeab, 0xfeac, 0, 0}, {0x0631, 0xfead, 0xfeae, 0, 0}, {0x0632, 0xfeaf, 0xfeb0, 0, 0},
    {0x0633, 0xfeb1, 0xfeb2, 0xfeb3, 0xfeb4}, {0x0634, 0xfeb5, 0xfeb6, 0xfeb7, 0xfeb8}, {0x0635, 0xfeb9, 0xfeba, 0xfebb, 0xfebc},
    {0x0636, 0xfebd, 0xfebe, 0xfebf, 0xfec0}, {0x0637, 0xfec1, 0xfec2, 0xfec3, 0xfec4}, {0x0638, 0xfec5, 0xfec6, 0xfec7, 0xfec8},
    {0x0639, 0xfec9, 0xfeca, 0xfecb, 0xfecc}, {0x063a, 0xfecd, 0xfece, 0xfecf, 0xfed0},
    {0x0641, 0xfed1, 0xfed2, 0xfed3, 0xfed4}, {0x0642, 0xfed5, 0xfed6, 0xfed7, 0xfed8}, {0x0643, 0xfed9, 0xfeda, 0xfedb, 0xfedc},
    {0x0644, 0xfedd, 0xfede, 0xfedf, 0xfee0}, {0x0645, 0xfee1, 0xfee2, 0xfee3, 0xfee4}, {0x0646, 0xfee5, 0xfee6, 0xfee7, 0xfee8},
    {0x0647, 0xfee9, 0xfeea, 0xfeeb, 0xfeec}, {0x0648, 0xfeed, 0xfeee, 0, 0}, {0x0649, 0xfeef, 0xfef0, 0, 0},
    {0x064a, 0xfef1, 0xfef2, 0xfef3, 0xfef4},
    // persian / urdu letters
    {0x067e, 0xfb56, 0xfb57, 0xfb58, 0xfb59}, {0x0686, 0xfb7a, 0xfb7b, 0xfb7c, 0xfb7d}, {0x0698, 0xfb8a, 0xfb8b, 0, 0},
    {0x06a9, 0xfb8e, 0xfb8f, 0xfb90, 0xfb91}, {0x06af, 0xfb92, 0xfb93, 0xfb94, 0xfb95}, {0x06cc, 0xfbfc, 0xfbfd, 0xfbfe, 0xfbff},
}};

[[nodiscard]] const arabic_form* find_form(char32_t c) noexcept
{
    for (const arabic_form& f : forms) {
        if (f.base == c) { return &f; }
    }
    return nullptr;
}

[[nodiscard]] bool is_transparent(char32_t c) noexcept // marks sit on the letter: they do not break the joining
{
    return classify(c) == bc::NSM;
}

// lam followed by an alef makes one ligature glyph: lam-alef (FEFB / FEFC), with hamza above (FEF7 / FEF8), below (FEF9 / FEFA), madda (FEF5 / FEF6)
[[nodiscard]] char32_t lam_alef(char32_t alef, bool joined_before) noexcept
{
    switch (alef) {
    case 0x0622: return joined_before ? 0xfef6 : 0xfef5;
    case 0x0623: return joined_before ? 0xfef8 : 0xfef7;
    case 0x0625: return joined_before ? 0xfefa : 0xfef9;
    case 0x0627: return joined_before ? 0xfefc : 0xfefb;
    default:     return 0;
    }
}

[[nodiscard]] bool available(const font_atlas* atlas, font_id f, char32_t cp) noexcept
{
    return atlas == nullptr || atlas->has_glyph(f, cp);
}

// joins `in_cps` into out.shaped / out.origin / out.span (the other fields are left alone)
void shape(const std::vector<char32_t>& in_cps, const font_atlas* atlas, font_id f, visual_scratch& out)
{
    const std::size_t n = in_cps.size();
    out.shaped.clear();
    out.origin.clear();
    out.span.clear();
    out.shaped.reserve(n);
    const auto push = [&](char32_t c, std::size_t at, u8 span) {
        out.shaped.push_back(c);
        out.origin.push_back(static_cast<u32>(at));
        out.span.push_back(span);
    };
    // does the letter at `j` connect towards the following letter / accept a connection from the preceding one
    const auto connects_forward = [&](std::size_t j) {
        if (in_cps[j] == 0x200d) { return true; }
        const arabic_form* fm = find_form(in_cps[j]);
        return fm != nullptr && fm->init != 0;
    };
    const auto accepts_backward = [&](std::size_t j) {
        if (in_cps[j] == 0x200d) { return true; }
        return find_form(in_cps[j]) != nullptr;
    };

    for (std::size_t i = 0; i < n; ++i) {
        const char32_t c = in_cps[i];
        const arabic_form* fm = find_form(c);
        if (fm == nullptr) {
            push(c, i, 1);
            continue;
        }
        bool prev_forward = false;
        for (std::size_t j = i; j-- > 0;) {
            if (is_transparent(in_cps[j])) { continue; }
            prev_forward = connects_forward(j);
            break;
        }
        std::size_t next = n;
        for (std::size_t k = i + 1; k < n; ++k) {
            if (is_transparent(in_cps[k])) { continue; }
            next = k;
            break;
        }
        const bool next_accepts = next < n && accepts_backward(next);

        if (c == 0x0644 && next == i + 1 && next < n) { // lam + alef
            if (const char32_t lig = lam_alef(in_cps[next], prev_forward); lig != 0 && available(atlas, f, lig)) {
                push(lig, i, 2);
                ++i;
                continue;
            }
        }
        char32_t form = fm->iso;
        if (fm->init != 0) { // dual-joining
            if (prev_forward && next_accepts) { form = fm->med; }
            else if (prev_forward)            { form = fm->fin; }
            else if (next_accepts)            { form = fm->init; }
        } else if (prev_forward) {            // right-joining
            form = fm->fin;
        }
        push(form != 0 && available(atlas, f, form) ? form : c, i, 1);
    }
}

[[nodiscard]] char32_t mirrored(char32_t c) noexcept
{
    switch (c) {
    case '(': return ')';       case ')': return '(';
    case '<': return '>';       case '>': return '<';
    case '[': return ']';       case ']': return '[';
    case '{': return '}';       case '}': return '{';
    case 0xab: return 0xbb;     case 0xbb: return 0xab;
    case 0x2039: return 0x203a; case 0x203a: return 0x2039;
    case 0x2264: return 0x2265; case 0x2265: return 0x2264;
    default: return c;
    }
}

// the unicode bidi algorithm for one line: fills `order` (visual position -> index in `cps`) and `level`
void resolve(const std::vector<char32_t>& cps, text_direction dir, std::vector<u32>& order, std::vector<u8>& level)
{
    const std::size_t n = cps.size();
    order.resize(n);
    level.assign(n, 0);
    if (n == 0) { return; }

    std::vector<bc> orig(n), t(n);
    for (std::size_t i = 0; i < n; ++i) { orig[i] = t[i] = classify(cps[i]); }

    // P2 / P3: the paragraph level
    u8 para = 0;
    if (dir == text_direction::rtl) {
        para = 1;
    } else if (dir == text_direction::automatic) {
        for (std::size_t i = 0; i < n; ++i) {
            if (t[i] == bc::L) { break; }
            if (is_strong_rtl(t[i])) { para = 1; break; }
        }
    }
    const bc sos = para != 0 ? bc::R : bc::L;

    // weak types
    for (std::size_t i = 0; i < n; ++i) { // W1
        if (t[i] == bc::NSM) { t[i] = i == 0 ? sos : t[i - 1]; }
    }
    {   // W2, W3
        bc last_strong = sos;
        for (std::size_t i = 0; i < n; ++i) {
            if (t[i] == bc::L || t[i] == bc::R || t[i] == bc::AL) { last_strong = t[i]; }
            else if (t[i] == bc::EN && last_strong == bc::AL) { t[i] = bc::AN; }
        }
        for (bc& x : t) { if (x == bc::AL) { x = bc::R; } }
    }
    for (std::size_t i = 1; i + 1 < n; ++i) { // W4
        if (t[i] == bc::ES && t[i - 1] == bc::EN && t[i + 1] == bc::EN) { t[i] = bc::EN; }
        else if (t[i] == bc::CS && t[i - 1] == bc::EN && t[i + 1] == bc::EN) { t[i] = bc::EN; }
        else if (t[i] == bc::CS && t[i - 1] == bc::AN && t[i + 1] == bc::AN) { t[i] = bc::AN; }
    }
    for (std::size_t i = 0; i < n;) { // W5
        if (t[i] != bc::ET) { ++i; continue; }
        std::size_t j = i;
        while (j < n && t[j] == bc::ET) { ++j; }
        if ((i > 0 && t[i - 1] == bc::EN) || (j < n && t[j] == bc::EN)) {
            for (std::size_t k = i; k < j; ++k) { t[k] = bc::EN; }
        }
        i = j;
    }
    for (bc& x : t) { if (x == bc::ES || x == bc::ET || x == bc::CS) { x = bc::ON; } } // W6
    {   // W7
        bc last_strong = sos;
        for (std::size_t i = 0; i < n; ++i) {
            if (t[i] == bc::L || t[i] == bc::R) { last_strong = t[i]; }
            else if (t[i] == bc::EN && last_strong == bc::L) { t[i] = bc::L; }
        }
    }

    // N0: a pair of brackets takes the direction of what is inside it (or of what is before it when the inside only has the
    // other direction), so "(word)" is closed at the right end of the word
    const auto direction_of = [&](bc x) { return x == bc::L ? bc::L : bc::R; }; // R, EN and AN count as R
    {
        const bc embedding = para != 0 ? bc::R : bc::L;
        const auto closing_of = [](char32_t c) -> char32_t {
            switch (c) {
            case '(': return ')'; case '[': return ']'; case '{': return '}';
            case 0xab: return 0xbb; case 0x2039: return 0x203a;
            default: return 0;
            }
        };
        struct open_bracket { std::size_t at; char32_t closer; };
        std::vector<open_bracket> stack;
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        for (std::size_t i = 0; i < n; ++i) {
            if (t[i] != bc::ON) { continue; }
            if (const char32_t closer = closing_of(cps[i]); closer != 0) {
                if (stack.size() < 63) { stack.push_back({i, closer}); }
            } else if (cps[i] == ')' || cps[i] == ']' || cps[i] == '}' || cps[i] == 0xbb || cps[i] == 0x203a) {
                for (std::size_t k = stack.size(); k-- > 0;) {
                    if (stack[k].closer == cps[i]) {
                        pairs.emplace_back(stack[k].at, i);
                        stack.resize(k);
                        break;
                    }
                }
            }
        }
        std::ranges::sort(pairs);
        for (const auto& [open, close] : pairs) {
            bool has_embedding = false;
            bool has_opposite  = false;
            for (std::size_t k = open + 1; k < close; ++k) {
                if (t[k] == bc::L || t[k] == bc::R || t[k] == bc::EN || t[k] == bc::AN) {
                    (direction_of(t[k]) == embedding ? has_embedding : has_opposite) = true;
                }
            }
            bc resolved = bc::ON;
            if (has_embedding) {
                resolved = embedding;
            } else if (has_opposite) {
                bc before = sos;
                for (std::size_t k = open; k-- > 0;) {
                    if (t[k] == bc::L || t[k] == bc::R || t[k] == bc::EN || t[k] == bc::AN) { before = direction_of(t[k]); break; }
                }
                resolved = before != embedding ? before : embedding;
            }
            if (resolved != bc::ON) { t[open] = t[close] = resolved; }
        }
    }

    // neutrals: between two runs of the same direction they join them, otherwise they follow the paragraph
    for (std::size_t i = 0; i < n;) {
        if (t[i] != bc::ON && t[i] != bc::WS) { ++i; continue; }
        std::size_t j = i;
        while (j < n && (t[j] == bc::ON || t[j] == bc::WS)) { ++j; }
        const bc before = i == 0 ? sos : direction_of(t[i - 1]);
        const bc after  = j == n ? sos : direction_of(t[j]);
        const bc fill   = before == after ? before : (para != 0 ? bc::R : bc::L);
        for (std::size_t k = i; k < j; ++k) { t[k] = fill; }
        i = j;
    }

    // implicit levels
    for (std::size_t i = 0; i < n; ++i) {
        if ((para & 1) == 0) {
            level[i] = t[i] == bc::L ? para : (t[i] == bc::R ? para + 1 : para + 2);
        } else {
            level[i] = t[i] == bc::R ? para : para + 1;
        }
    }
    // L1: white space at the end of the line goes back to the paragraph level
    for (std::size_t i = n; i-- > 0;) {
        if (orig[i] != bc::WS) { break; }
        level[i] = para;
    }

    // L2: reverse runs from the highest level down to the lowest odd one
    for (std::size_t i = 0; i < n; ++i) { order[i] = static_cast<u32>(i); }
    u8 highest = 0;
    u8 lowest_odd = 255;
    for (const u8 l : level) {
        highest = std::max(highest, l);
        if ((l & 1) != 0) { lowest_odd = std::min(lowest_odd, l); }
    }
    for (int lvl = highest; lvl >= lowest_odd && lvl > 0; --lvl) {
        for (std::size_t i = 0; i < n;) {
            if (level[order[i]] < lvl) { ++i; continue; }
            std::size_t j = i;
            while (j < n && level[order[j]] >= lvl) { ++j; }
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i), order.begin() + static_cast<std::ptrdiff_t>(j));
            i = j;
        }
    }
}

void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

void decode_line_into(std::vector<char32_t>& cps, std::string_view s, std::vector<u32>* offsets = nullptr)
{
    cps.clear();
    cps.reserve(s.size());
    const std::size_t total = s.size();
    while (!s.empty()) {
        if (offsets != nullptr) { offsets->push_back(static_cast<u32>(total - s.size())); }
        cps.push_back(decode_utf8(s));
    }
}

} // namespace

bool has_rtl_text(std::string_view s) noexcept
{
    // every right-to-left character is encoded with a lead byte >= 0xd6 (hebrew: d6 d7, arabic / syriac / thaana: d8..de, n'ko
    // and the rest of the bmp block: df e0 ..., presentation forms: ef, supplementary scripts: f0): latin, greek, cyrillic
    // (leads c2..d5) and plain ascii are turned away without decoding
    bool candidate = false;
    for (const char ch : s) {
        if (static_cast<u8>(ch) >= 0xd6) { candidate = true; break; }
    }
    if (!candidate) { return false; }
    while (!s.empty()) {
        if (static_cast<u8>(s.front()) < 0xd6) { s.remove_prefix(1); continue; }
        const bc t = classify(decode_utf8(s));
        if (t == bc::R || t == bc::AL || t == bc::AN) {
            return true;
        }
    }
    return false;
}

void to_visual_into(std::string& out, visual_scratch& sc, std::string_view text, const font_atlas* atlas, font_id font,
                    text_direction direction)
{
    out.clear();
    if (!has_rtl_text(text)) {
        out.assign(text);
        return;
    }
    std::size_t pos = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        decode_line_into(sc.cps, line);
        shape(sc.cps, atlas, font, sc);
        resolve(sc.shaped, direction, sc.order, sc.level);
        for (const u32 idx : sc.order) {
            char32_t c = sc.shaped[idx];
            if ((sc.level[idx] & 1) != 0) { c = mirrored(c); }
            append_utf8(out, c);
        }
        if (nl == std::string_view::npos) { break; }
        out.push_back('\n');
        pos = nl + 1;
    }
}

std::string to_visual(std::string_view text, const font_atlas* atlas, font_id font, text_direction direction)
{
    if (!has_rtl_text(text)) {
        return std::string{text};
    }
    std::string    out;
    visual_scratch sc;
    out.reserve(text.size());
    to_visual_into(out, sc, text, atlas, font, direction);
    return out;
}

void bidi_layout::build(const font_atlas& atlas, font_id font, std::string_view text, text_direction direction)
{
    rtl_ = has_rtl_text(text);
    size_ = text.size();
    byte_of_.clear();
    glyph_of_.clear();
    second_.clear();
    left_.clear();
    advance_.clear();
    level_.clear();
    width_ = 0.0f;
    if (!rtl_) {
        width_ = atlas.measure(font, text).x;
        return;
    }
    // the scratch and byte_of_ are members: a field being edited rebuilds this every frame, so the buffers are
    // grown once and then reused
    visual_scratch& sc = scratch_;
    decode_line_into(sc.cps, text, &byte_of_);
    byte_of_.push_back(static_cast<u32>(text.size()));

    shape(sc.cps, &atlas, font, sc);
    resolve(sc.shaped, direction, sc.order, sc.level);

    const std::size_t m = sc.shaped.size();
    visual_pos_.assign(m, 0);
    for (std::size_t v = 0; v < m; ++v) { visual_pos_[sc.order[v]] = static_cast<u32>(v); }

    left_.resize(m);
    advance_.resize(m);
    level_.resize(m);
    f32 x = 0.0f;
    char32_t prev = 0;
    for (std::size_t v = 0; v < m; ++v) {
        char32_t c = sc.shaped[sc.order[v]];
        if ((sc.level[sc.order[v]] & 1) != 0) { c = mirrored(c); }
        if (prev != 0) { x += atlas.kerning(font, prev, c); }
        left_[v]    = x;
        advance_[v] = atlas.advance(font, c);
        level_[v]   = sc.level[sc.order[v]];
        x += advance_[v];
        prev = c;
    }
    width_ = x;

    glyph_of_.resize(sc.cps.size());
    second_.assign(sc.cps.size(), 0);
    for (std::size_t s = 0; s < m; ++s) {
        for (u32 k = 0; k < sc.span[s]; ++k) {
            glyph_of_[sc.origin[s] + k] = visual_pos_[s];
            second_[sc.origin[s] + k]   = static_cast<u8>(k);
        }
    }
}

f32 bidi_layout::caret_x(std::size_t byte_offset) const noexcept
{
    if (!rtl_ || glyph_of_.empty()) { return 0.0f; }
    // the boundary: the number of code points before the offset
    const std::size_t n = glyph_of_.size();
    const auto it = std::upper_bound(byte_of_.begin(), byte_of_.begin() + static_cast<std::ptrdiff_t>(n), static_cast<u32>(byte_offset));
    const std::size_t b = static_cast<std::size_t>(it - byte_of_.begin()); // code points that start at or before the offset
    // b == 0 cannot happen (offset 0 starts code point 0); the caret sits after code point b - 1 unless the offset is its start
    std::size_t k = b;
    if (b > 0 && byte_of_[b - 1] == byte_offset) { k = b - 1; } // exactly at a boundary: before code point b - 1
    if (k == 0) {
        const u32 v = glyph_of_[0];
        return (level_[v] & 1) != 0 ? left_[v] + advance_[v] : left_[v];
    }
    const u32 v = glyph_of_[k - 1];
    if (k < n && glyph_of_[k] == v) { // inside a ligature: its leading edge
        return (level_[v] & 1) != 0 ? left_[v] + advance_[v] : left_[v];
    }
    return (level_[v] & 1) != 0 ? left_[v] : left_[v] + advance_[v];
}

std::size_t bidi_layout::index_at(f32 x) const noexcept
{
    if (!rtl_ || glyph_of_.empty()) { return 0; }
    const std::size_t n = glyph_of_.size();
    std::size_t best = 0;
    f32 best_d = 1.0e30f;
    for (std::size_t k = 0; k <= n; ++k) {
        const std::size_t off = byte_of_[k];
        const f32 d = std::abs(caret_x(off) - x);
        if (d < best_d) { best_d = d; best = off; }
    }
    return best;
}

} // namespace strata

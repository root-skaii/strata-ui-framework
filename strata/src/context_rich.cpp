// rich text: markup parsing, multi-line word-wrapped layout with mixed fonts and colors, and the label routing that lets
// every widget draw its caption as markup

#include "strata/context.hpp"

#include "text_util.hpp"

#include <algorithm>
#include <charconv>

namespace strata {

using namespace text;

namespace {

constexpr std::size_t max_rich_runs = 4096;

[[nodiscard]] bool parse_color_tag(std::string_view s, color& out) noexcept
{
    if (s.size() != 6 && s.size() != 8) { return false; }
    std::array<u8, 8> nib{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch >= '0' && ch <= '9')      { nib[i] = static_cast<u8>(ch - '0'); }
        else if (ch >= 'a' && ch <= 'f') { nib[i] = static_cast<u8>(ch - 'a' + 10); }
        else if (ch >= 'A' && ch <= 'F') { nib[i] = static_cast<u8>(ch - 'A' + 10); }
        else { return false; }
    }
    const auto byte = [&](std::size_t i) { return static_cast<u8>(nib[i] * 16 + nib[i + 1]); };
    out = color{byte(0), byte(2), byte(4), s.size() == 8 ? byte(6) : u8{255}};
    return true;
}

} // namespace

// splits the markup into runs of one font, color and style. tags: <f=N> </f>, <c=rrggbb[aa]> </c>, and the styles
// <b> bold, <i> italic, <u> underline, <s> strike-through (each closed by </b> ...; they nest and combine);
// "<<" is a literal '<' (the run before it ends with that '<'); anything that is not a known tag stays text.
template <class Run>
static void parse_rich_runs(std::string_view s, font_id base_font, color base_col, std::size_t font_count, std::vector<Run>& runs)
{
    std::array<font_id, 8> fonts{};
    std::array<color, 8>   colors{};
    u32 fd = 0;
    u32 cd = 0;
    fonts[0]  = base_font;
    colors[0] = base_col;
    std::array<u32, 4> style_depth{}; // b, i, u, s
    constexpr std::array<text_flags, 4> style_bits{text_flags::bold, text_flags::italic, text_flags::underline, text_flags::strike};

    std::size_t start = 0;
    const auto flush = [&](std::size_t end) {
        if (end > start && runs.size() < max_rich_runs) {
            text_flags style = text_flags::none;
            for (std::size_t k = 0; k < style_bits.size(); ++k) {
                if (style_depth[k] > 0) { style = style | style_bits[k]; }
            }
            runs.push_back({s.substr(start, end - start), fonts[fd], colors[cd], style});
        }
    };

    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') { ++i; continue; }
        if (i + 1 < s.size() && s[i + 1] == '<') { // "<<" is a literal '<'
            flush(i + 1);
            i += 2;
            start = i;
            continue;
        }
        const std::size_t close = s.find('>', i);
        if (close == std::string_view::npos) { ++i; continue; }

        const std::string_view tag = s.substr(i + 1, close - i - 1);
        bool handled = false;
        if (tag.starts_with("f=")) {
            u32 value = 0;
            const auto res = std::from_chars(tag.data() + 2, tag.data() + tag.size(), value);
            if (res.ec == std::errc{} && res.ptr == tag.data() + tag.size() && value < font_count && fd + 1 < fonts.size()) {
                flush(i);
                fonts[++fd] = value;
                handled = true;
            }
        } else if (tag == "/f") {
            flush(i);
            if (fd > 0) { --fd; }
            handled = true;
        } else if (tag.starts_with("c=")) {
            color c;
            if (parse_color_tag(tag.substr(2), c) && cd + 1 < colors.size()) {
                flush(i);
                colors[++cd] = c;
                handled = true;
            }
        } else if (tag == "/c") {
            flush(i);
            if (cd > 0) { --cd; }
            handled = true;
        } else if (tag.size() >= 1 && tag.size() <= 2) {
            const bool closing = tag[0] == '/';
            const char letter  = tag.size() == 2 ? (closing ? tag[1] : '\0') : (closing ? '\0' : tag[0]);
            const std::size_t k = std::string_view{"bius"}.find(letter);
            if (letter != '\0' && k != std::string_view::npos) {
                flush(i);
                if (!closing) { ++style_depth[k]; } else if (style_depth[k] > 0) { --style_depth[k]; }
                handled = true;
            }
        }
        if (handled) {
            i     = close + 1;
            start = i;
        } else {
            ++i; // not a tag we know: the '<' is ordinary text
        }
    }
    flush(s.size());
}

// lays runs out on lines. with wrap_width > 0 a line ends before the word that would not fit; '\n' always ends one.
// the runs of a line share a baseline (the tallest ascent), and the line is as high as its tallest run.
vec2 context::rich_layout(std::string_view text, font_id base, color base_col, f32 wrap_width, bool markup)
{
    rich_runs_.clear();
    rich_segs_.clear();
    rich_lines_.clear();
    if (markup) {
        parse_rich_runs(text, base, base_col, font_.font_count(), rich_runs_);
    } else if (!text.empty()) {
        rich_runs_.push_back({text, base, base_col, text_flags::none});
    }

    f32 x = 0.0f;                 // pen position on the current line (approximate while wrapping, exact once a line is closed)
    u32 line_first = 0;
    f32 asc = 0.0f;
    f32 desc = 0.0f;
    f32 y = 0.0f;
    f32 max_width = 0.0f;

    const auto note_font = [&](font_id f) {
        const f32 a = font_.ascent(f);
        asc  = std::max(asc, a);
        desc = std::max(desc, font_.line_height(f) - a);
    };
    const auto end_line = [&] {
        const u32 count = static_cast<u32>(rich_segs_.size()) - line_first;
        if (count == 0) { // an empty line is as high as the base font
            note_font(base);
        }
        f32 pen = 0.0f;
        for (u32 k = line_first; k < line_first + count; ++k) {
            rich_seg& seg = rich_segs_[k];
            seg.x = pen;
            pen  += font_.measure(seg.font, seg.text).x; // exact, kerning included
        }
        rich_line line;
        line.first  = line_first;
        line.count  = count;
        line.width  = pen;
        line.asc    = asc;
        line.height = asc + desc;
        line.y      = y;
        rich_lines_.push_back(line);
        y += line.height;
        max_width = std::max(max_width, pen);
        line_first = static_cast<u32>(rich_segs_.size());
        x = 0.0f;
        asc = desc = 0.0f;
    };
    // appends text[a, b) of a run to the line, extending the previous segment when it is the same run
    const auto add = [&](const rich_run& run, std::size_t a, std::size_t b, f32 w) {
        if (b <= a) { return; }
        const std::string_view piece = run.text.substr(a, b - a);
        if (rich_segs_.size() > line_first) {
            rich_seg& last = rich_segs_.back();
            if (last.font == run.font && last.col == run.col && last.style == run.style && last.text.data() + last.text.size() == piece.data()) {
                last.text = std::string_view{last.text.data(), last.text.size() + piece.size()};
                x += w;
                note_font(run.font);
                return;
            }
        }
        rich_segs_.push_back({piece, run.font, run.col, run.style, 0.0f});
        x += w;
        note_font(run.font);
    };

    for (const rich_run& run : rich_runs_) {
        const std::string_view t = run.text;
        std::size_t i = 0;
        while (i < t.size()) {
            if (t[i] == '\n') {
                end_line();
                ++i;
                continue;
            }
            std::size_t we = i; // a word ...
            while (we < t.size() && t[we] != ' ' && t[we] != '\n') { we = next_boundary(t, we); }
            std::size_t te = we; // ... and the spaces after it
            while (te < t.size() && t[te] == ' ') { ++te; }

            const f32 ww = we > i ? font_.measure(run.font, t.substr(i, we - i)).x : 0.0f;
            const f32 sw = te > we ? font_.measure(run.font, t.substr(we, te - we)).x : 0.0f;
            if (wrap_width > 0.0f && x > 0.0f && we > i && x + ww > wrap_width) {
                end_line();
            }
            if (wrap_width > 0.0f && ww > wrap_width) { // a word wider than the line: break it between characters
                std::size_t k = i;
                while (k < we) {
                    const std::size_t nk  = next_boundary(t, k);
                    const f32         adv = font_.measure(run.font, t.substr(k, nk - k)).x;
                    if (x > 0.0f && x + adv > wrap_width) { end_line(); }
                    add(run, k, nk, adv);
                    k = nk;
                }
                add(run, we, te, sw);
            } else {
                add(run, i, te, ww + sw);
            }
            i = te;
        }
    }
    if (!rich_segs_.empty() || !rich_lines_.empty() || !text.empty()) {
        end_line();
    }
    return {max_width, y};
}

void context::rich_draw(vec2 pos)
{
    for (const rich_line& line : rich_lines_) {
        for (u32 k = line.first; k < line.first + line.count; ++k) {
            const rich_seg& seg = rich_segs_[k];
            dl_.text({pos.x + seg.x, pos.y + line.y + line.asc - font_.ascent(seg.font)}, seg.col, seg.text, seg.font, seg.style);
        }
    }
}

vec2 context::label_size(font_id f, std::string_view s)
{
    if (rich_depth_ == 0) {
        return measure_cached(f, s);
    }
    const vec2 size = rich_layout(s, f, style_.text, 0.0f, true);
    return s.empty() ? vec2{0.0f, font_.line_height(f)} : size;
}

void context::label_draw(vec2 pos, color c, std::string_view s, font_id f)
{
    if (rich_depth_ == 0) {
        dl_.text(pos, c, s, f);
        return;
    }
    rich_layout(s, f, c, 0.0f, true);
    rich_draw(pos);
}

void context::rich_text(std::string_view markup)
{
    if (cur_ == nullptr || markup.empty()) {
        return;
    }
    const vec2 size = rich_layout(markup, current_font(), style_.text, 0.0f, true);
    if (rich_lines_.empty()) {
        return;
    }
    const rect r  = layout_place(size);
    const f32  dy = std::max(0.0f, (layout_.line_h - size.y) * 0.5f); // centered on a line shared with taller widgets
    rich_draw({r.min.x, r.min.y + dy});
}

void context::rich_text_wrapped(std::string_view markup)
{
    if (cur_ == nullptr || markup.empty()) {
        return;
    }
    const f32 w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    layout_.next_width = 0.0f;
    const vec2 size = rich_layout(markup, current_font(), style_.text, std::max(w, 1.0f), true);
    if (rich_lines_.empty()) {
        return;
    }
    const rect r = layout_place(size);
    rich_draw(r.min);
}

void context::text_wrapped_colored(color c, std::string_view s)
{
    if (cur_ == nullptr || s.empty()) {
        return;
    }
    if (selectable_depth_ > 0) {
        ml_color_ = c;
        text_selectable(s, s);
        return;
    }
    const f32 w = layout_.next_width > 0.0f ? layout_.next_width : layout_.width;
    layout_.next_width = 0.0f;
    const vec2 size = rich_layout(s, current_font(), c, std::max(w, 1.0f), rich_depth_ > 0);
    if (rich_lines_.empty()) {
        return;
    }
    const rect r = layout_place(size);
    rich_draw(r.min);
}

} // namespace strata

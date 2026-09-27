// text editing engine shared by the text fields (undo / redo, clipboard shortcuts) and the multi-line input

#include "strata/context.hpp"

#include "context_impl.hpp"

#include "strata/bidi.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace strata {

using namespace text;

namespace {

constexpr std::size_t npos = std::string::npos;
constexpr std::size_t max_history_ops   = 256;
constexpr std::size_t max_history_bytes = std::size_t{1} << 20;
constexpr std::size_t max_op_bytes      = std::size_t{4} << 20; // a bigger single change is not worth remembering

// zeroes and drops the tail of a secure string
void wipe_tail(secure_string& s, std::size_t from) noexcept
{
    if (from >= s.size()) { return; }
    detail::secure_wipe(s.data() + from, s.size() - from);
    s.resize(from);
}

// the bracket at the caret (or just before it) and the one it pairs with; only brackets of the same kind count, so a
// bracket inside a string or comment can mislead it. false when there is none (or no partner within a few hundred KB)
[[nodiscard]] bool find_bracket_pair(std::string_view t, std::size_t caret, std::size_t& first, std::size_t& second) noexcept
{
    constexpr std::string_view opens  = "([{";
    constexpr std::string_view closes = ")]}";
    const auto is_bracket = [&](std::size_t i) { return i < t.size() && (opens.find(t[i]) != npos || closes.find(t[i]) != npos); };
    std::size_t at = npos;
    if (is_bracket(caret)) { at = caret; }
    else if (caret > 0 && is_bracket(caret - 1)) { at = caret - 1; }
    if (at == npos) { return false; }

    constexpr std::size_t limit = 400000;
    const char c = t[at];
    const bool forward = opens.find(c) != npos;
    const char partner = forward ? closes[opens.find(c)] : opens[closes.find(c)];
    int depth = 0;
    if (forward) {
        for (std::size_t i = at; i < t.size() && i - at < limit; ++i) {
            if (t[i] == c) { ++depth; }
            else if (t[i] == partner && --depth == 0) { first = at; second = i; return true; }
        }
    } else {
        for (std::size_t i = at + 1; i-- > 0 && at - i < limit;) {
            if (t[i] == c) { ++depth; }
            else if (t[i] == partner && --depth == 0) { first = i; second = at; return true; }
        }
    }
    return false;
}

} // namespace

// undo / redo ---------------------------------------------------------------------------------

void context::edit_history_clear() noexcept
{
    for (edit_history* h : {&m_->undo_, &m_->redo_}) {
        h->text.resize(h->text.capacity()); // also reaches what an earlier, longer text left behind
        detail::secure_wipe(h->text.data(), h->text.size());
        h->text.clear();
        h->ops.clear();
    }
}

void context::edit_history_add(edit_history& h, const edit_op& op, std::string_view removed, std::string_view inserted)
{
    if (removed.size() + inserted.size() > max_op_bytes) {
        edit_history_clear();
        return;
    }
    edit_op entry = op;
    entry.off = h.text.size();
    h.text.append(removed);
    h.text.append(inserted);
    h.ops.push_back(entry);

    // forget the oldest changes once the history gets long
    while (h.ops.size() > 1 && (h.ops.size() > max_history_ops || h.text.size() > max_history_bytes)) {
        const std::size_t drop = h.ops[1].off - h.ops[0].off;
        std::memmove(h.text.data(), h.text.data() + drop, h.text.size() - drop);
        detail::secure_wipe(h.text.data() + h.text.size() - drop, drop);
        h.text.resize(h.text.size() - drop);
        h.ops.erase(h.ops.begin());
        for (edit_op& o : h.ops) { o.off -= drop; }
    }
}

// replaces [pos, pos + len) of the edit buffer with `with`, puts the caret after it and remembers the change.
// consecutive typing / backspacing / deleting within a second merges into one undo step.
bool context::edit_replace(std::size_t pos, std::size_t len, std::string_view with, edit_kind kind)
{
    pos = std::min(pos, m_->edit_buf_.size());
    len = std::min(len, m_->edit_buf_.size() - pos);
    if (len == 0 && with.empty()) {
        return false;
    }

    if (m_->edit_history_on_) {
        bool merged = false;
        if (!m_->undo_.ops.empty() && kind != edit_kind::other) {
            edit_op& last = m_->undo_.ops.back();
            if (last.kind == kind && m_->time_ - last.time < 1.0) {
                const std::string_view removed = std::string_view{m_->edit_buf_}.substr(pos, len);
                if (kind == edit_kind::typing && last.rem_len == 0 && len == 0 && pos == last.pos + last.ins_len) {
                    m_->undo_.text.append(with);
                    last.ins_len     += with.size();
                    last.cursor_after = pos + with.size();
                    merged = true;
                } else if (kind == edit_kind::erase_back && last.ins_len == 0 && with.empty() && pos + len == last.pos) {
                    m_->undo_.text.insert(last.off, removed);
                    last.rem_len     += len;
                    last.pos          = pos;
                    last.cursor_after = pos;
                    merged = true;
                } else if (kind == edit_kind::erase_fwd && last.ins_len == 0 && with.empty() && pos == last.pos) {
                    m_->undo_.text.append(removed);
                    last.rem_len     += len;
                    last.cursor_after = pos;
                    merged = true;
                }
                if (merged) { last.time = m_->time_; }
            }
        }
        if (!merged) {
            edit_op op;
            op.pos           = pos;
            op.rem_len       = len;
            op.ins_len       = with.size();
            op.cursor_before = m_->edit_cursor_;
            op.anchor_before = m_->edit_anchor_;
            op.cursor_after  = pos + with.size();
            op.kind          = kind;
            op.time          = m_->time_;
            edit_history_add(m_->undo_, op, std::string_view{m_->edit_buf_}.substr(pos, len), with);
        }
        // a new change ends the redo chain
        m_->redo_.text.resize(m_->redo_.text.capacity());
        detail::secure_wipe(m_->redo_.text.data(), m_->redo_.text.size());
        m_->redo_.text.clear();
        m_->redo_.ops.clear();
    }

    m_->edit_buf_.replace(pos, len, with);
    m_->edit_cursor_ = m_->edit_anchor_ = pos + with.size();
    ++m_->edit_version_;
    return true;
}

bool context::edit_delete_selection()
{
    if (m_->edit_cursor_ == m_->edit_anchor_) {
        return false;
    }
    const std::size_t lo = std::min(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t hi = std::max(m_->edit_cursor_, m_->edit_anchor_);
    return edit_replace(lo, hi - lo, {}, edit_kind::other);
}

// typed / pasted text replaces the selection; cut at the field's byte limit on a code point boundary
bool context::edit_insert(std::string_view s, bool typed)
{
    if (m_->edit_readonly_) {
        return false;
    }
    const std::size_t lo   = std::min(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t hi   = std::max(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t base = m_->edit_buf_.size() - (hi - lo);
    const std::size_t room = m_->edit_max_bytes_ > base ? m_->edit_max_bytes_ - base : 0;
    if (s.size() > room) {
        std::size_t n = room;
        while (n > 0 && n < s.size() && is_continuation(s[n])) { --n; }
        s = s.substr(0, n);
    }
    if (s.empty() && lo == hi) {
        return false;
    }
    return edit_replace(lo, hi - lo, s, typed && lo == hi ? edit_kind::typing : edit_kind::other);
}

bool context::edit_undo()
{
    if (!m_->edit_history_on_ || m_->undo_.ops.empty()) {
        return false;
    }
    const edit_op op = m_->undo_.ops.back();
    if (op.pos + op.ins_len > m_->edit_buf_.size()) { // the buffer was changed behind our back: the history is useless
        edit_history_clear();
        return false;
    }
    const std::string_view removed{m_->undo_.text.data() + op.off, op.rem_len};
    const std::string_view inserted{m_->undo_.text.data() + op.off + op.rem_len, op.ins_len};
    edit_history_add(m_->redo_, op, removed, inserted);
    m_->edit_buf_.replace(op.pos, op.ins_len, removed);

    wipe_tail(m_->undo_.text, op.off);
    m_->undo_.ops.pop_back();
    m_->edit_cursor_ = std::min(op.cursor_before, m_->edit_buf_.size());
    m_->edit_anchor_ = std::min(op.anchor_before, m_->edit_buf_.size());
    ++m_->edit_version_;
    return true;
}

bool context::edit_redo()
{
    if (!m_->edit_history_on_ || m_->redo_.ops.empty()) {
        return false;
    }
    const edit_op op = m_->redo_.ops.back();
    if (op.pos + op.rem_len > m_->edit_buf_.size()) {
        edit_history_clear();
        return false;
    }
    const std::string_view removed{m_->redo_.text.data() + op.off, op.rem_len};
    const std::string_view inserted{m_->redo_.text.data() + op.off + op.rem_len, op.ins_len};
    edit_history_add(m_->undo_, op, removed, inserted);
    m_->edit_buf_.replace(op.pos, op.rem_len, inserted);

    wipe_tail(m_->redo_.text, op.off);
    m_->redo_.ops.pop_back();
    m_->edit_cursor_ = m_->edit_anchor_ = std::min(op.cursor_after, m_->edit_buf_.size());
    ++m_->edit_version_;
    return true;
}

// ctrl+a / c / x / v / z / y on the focused field. returns true when the text changed.
bool context::edit_shortcut(const key_event& ev, bool password, bool multiline)
{
    const std::string_view t = m_->edit_buf_;
    const std::size_t lo = std::min(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t hi = std::max(m_->edit_cursor_, m_->edit_anchor_);

    switch (ev.k) {
    case key::a:
        m_->edit_anchor_ = 0;
        m_->edit_cursor_ = t.size();
        return false;
    case key::c:
        if (!password && lo != hi && m_->clipboard_.set != nullptr) {
            m_->clipboard_.set(m_->clipboard_.user, t.substr(lo, hi - lo));
        }
        return false;
    case key::x:
        if (!password && !m_->edit_readonly_ && lo != hi) {
            if (m_->clipboard_.set != nullptr) {
                m_->clipboard_.set(m_->clipboard_.user, t.substr(lo, hi - lo));
            }
            return edit_delete_selection();
        }
        return false;
    case key::v: {
        if (m_->edit_readonly_ || m_->clipboard_.get == nullptr) {
            return false;
        }
        std::string pasted;
        bool changed = false;
        if (m_->clipboard_.get(m_->clipboard_.user, pasted)) {
            std::string clean;
            clean.reserve(pasted.size());
            for (std::size_t i = 0; i < pasted.size(); ++i) {
                const char c = pasted[i];
                if (multiline) {
                    if (c == '\r') {
                        if (i + 1 < pasted.size() && pasted[i + 1] == '\n') { ++i; }
                        clean.push_back('\n');
                    } else if (c == '\t') {
                        clean.append("    ");
                    } else {
                        clean.push_back(c);
                    }
                } else {
                    clean.push_back(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
                }
            }
            changed = edit_insert(clean, false);
            detail::secure_wipe(clean.data(), clean.size());
        }
        detail::secure_wipe(pasted.data(), pasted.size());
        return changed;
    }
    case key::z:
        return ev.shift ? edit_redo() : edit_undo();
    case key::y:
        return edit_redo();
    default:
        return false;
    }
}

// multi-line input ----------------------------------------------------------------------------------

std::size_t context::ml_line_of(std::size_t index) const noexcept
{
    // the last line that starts at or before the index (a wrapped line owns its first position)
    const auto it = std::upper_bound(m_->ml_lines_.begin(), m_->ml_lines_.end(), index,
                                     [](std::size_t i, const ml_line& l) { return i < l.start; });
    return it == m_->ml_lines_.begin() ? 0 : static_cast<std::size_t>(it - m_->ml_lines_.begin()) - 1;
}

// splits the text into display lines: at every '\n' and, with `wrap`, at word boundaries once a line is wider than `width`
void context::ml_layout(std::string_view t, f32 width, font_id f, bool wrap)
{
    u64 h = 1469598103934665603ull; // fnv-1a over the text, so an unchanged text keeps its lines
    for (const char c : t) { h = (h ^ static_cast<u8>(c)) * 1099511628211ull; }
    h ^= (static_cast<u64>(std::bit_cast<u32>(width)) << 24) ^ (static_cast<u64>(f) << 56) ^ (wrap ? 0x5bd1e995ull : 0ull) ^
         (static_cast<u64>(t.size()) * 0x9e3779b97f4a7c15ull) ^ (m_->edit_spans_hash_ * 0xff51afd7ed558ccdull);
    if (h == m_->ml_cache_key_ && !m_->ml_lines_.empty()) {
        return;
    }
    m_->ml_cache_key_ = h;
    m_->ml_lines_.clear();

    const auto push = [&](std::size_t a, std::size_t b) {
        m_->ml_lines_.push_back({static_cast<u32>(a), static_cast<u32>(b)});
    };
    const auto break_line = [&](std::size_t ls, std::size_t le) {
        if (!wrap || ls == le || ed_measure(f, t, ls, le) <= width) {
            push(ls, le);
            return;
        }
        std::size_t line_start = ls;
        f32         x          = 0.0f;
        std::size_t i          = ls;
        while (i < le) {
            const std::size_t ws = i; // a word and the spaces after it
            std::size_t we = ws;
            while (we < le && t[we] != ' ') { we = next_boundary(t, we); }
            std::size_t te = we;
            while (te < le && t[te] == ' ') { ++te; }

            const f32 ww = ed_measure(f, t, ws, we);
            const f32 tw = ww + ed_measure(f, t, we, te);
            if (x > 0.0f && x + ww > width && we > ws) {
                push(line_start, ws);
                line_start = ws;
                x          = 0.0f;
            }
            if (ww > width) { // a word wider than the field: break it between characters
                std::size_t k = ws;
                while (k < we) {
                    const std::size_t nk  = next_boundary(t, k);
                    const f32         adv = ed_measure(f, t, k, nk);
                    if (x > 0.0f && x + adv > width) {
                        push(line_start, k);
                        line_start = k;
                        x          = 0.0f;
                    }
                    x += adv;
                    k  = nk;
                }
                x += tw - ww;
            } else {
                x += tw;
            }
            i = te;
        }
        push(line_start, le);
    };

    std::size_t p = 0;
    for (;;) {
        const std::size_t nl = t.find('\n', p);
        const std::size_t le = nl == std::string_view::npos ? t.size() : nl;
        break_line(p, le);
        if (nl == std::string_view::npos) { break; }
        p = nl + 1;
    }
}

void context::text_selectable(std::string_view id_label, std::string_view text)
{
    if (m_->cur_ == nullptr || text.empty()) {
        return;
    }
    (void)input_multiline_core(id_label, text, {}, input_flags::read_only | input_flags::no_frame | input_flags::auto_height, {},
                               std::size_t{1} << 24);
}

bool context::input_multiline(std::string_view label, std::string& value, vec2 size, input_flags flags,
                              std::string_view hint, std::size_t max_bytes)
{
    if (input_multiline_core(label, value, size, flags, hint, max_bytes)) {
        value.assign(m_->edit_buf_.data(), m_->edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_multiline(std::string_view label, secure_string& value, vec2 size, input_flags flags,
                              std::string_view hint, std::size_t max_bytes)
{
    if (input_multiline_core(label, value, size, flags, hint, max_bytes)) {
        value.assign(m_->edit_buf_.data(), m_->edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_multiline_core(std::string_view label, std::string_view current, vec2 size, input_flags flags,
                                   std::string_view hint, std::size_t max_bytes)
{
    if (m_->cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = current_font();
    f32           lh       = m_->font_.line_height(fnt);
    f32           asc      = m_->font_.ascent(fnt);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const bool    wrap     = !has_flag(flags, input_flags::no_wrap);
    const id      key      = widget_id(label);
    ed_prepare_spans(m_->focus_id_ == key ? m_->edit_buf_.size() : current.size(), fnt, lh, asc, false);

    // what input_code asks of this field (it applies to this call only)
    const code_mode code = m_->code_;
    m_->code_ = {};
    const bool code_on = code.on;
    const auto has_code = [&](code_flags f) { return code_on && (static_cast<u8>(code.flags) & static_cast<u8>(f)) != 0; };

    const bool frameless = has_flag(flags, input_flags::no_frame);
    const bool auto_h    = has_flag(flags, input_flags::auto_height);
    const color text_col = m_->ml_color_.a != 0 ? m_->ml_color_ : m_->style_.text;
    m_->ml_color_ = color{0, 0, 0, 0};

    field_layout fl;
    if (frameless) { // selectable text: no caption, no box; the size follows the text
        const f32 w = size.x > 0.0f ? size.x : (m_->layout_.next_width > 0.0f ? m_->layout_.next_width : m_->layout_.width);
        m_->layout_.next_width = 0.0f;
        f32 h = size.y;
        if (auto_h || h <= 0.0f) {
            ml_layout(m_->focus_id_ == key ? std::string_view{m_->edit_buf_} : current, std::max(w, 8.0f), fnt, wrap);
            h = static_cast<f32>(m_->ml_lines_.size()) * lh;
        }
        fl.control = layout_place({w, std::max(h, lh)});
    } else {
        const f32 box_h = size.y > 0.0f ? size.y : lh * 6.0f + m_->style_.frame_padding.y * 2.0f;
        if (size.x > 0.0f) { m_->layout_.next_width = size.x; }
        fl = layout_field(visible_label(label), box_h);
    }
    const rect box = fl.control;

    child_state* st = internal::state_for(m_->children_, key, m_->frame_); // vertical scroll and content height of this field
    constexpr f32 bar_w = 10.0f;

    // the scrollbar column is its own control, so it comes before the field claims the press
    const rect track = {{box.max.x - bar_w - 1.0f, box.min.y + 3.0f}, {box.max.x - 2.0f, box.max.y - 3.0f}};
    const f32  pad_x = frameless ? 0.0f : m_->style_.frame_padding.x;
    const f32  pad_y = frameless ? 0.0f : m_->style_.frame_padding.y;

    // line numbers: a gutter at the left, wide enough for the last one
    f32 gutter_w = 0.0f;
    if (has_code(code_flags::line_numbers)) {
        std::size_t lines = 1;
        for (const char c : (m_->focus_id_ == key ? std::string_view{m_->edit_buf_} : current)) { lines += c == '\n' ? 1u : 0u; }
        int digits = 1;
        for (std::size_t n = lines; n >= 10; n /= 10) { ++digits; }
        gutter_w = m_->font_.measure(fnt, "0").x * static_cast<f32>(std::max(digits, 3)) + 18.0f;
    }
    const rect inner = {{box.min.x + pad_x + gutter_w, box.min.y + pad_y},
                        {box.max.x - (st->overflow ? bar_w + 3.0f : pad_x), box.max.y - pad_y}};
    const f32  view_w = std::max(inner.width(), 8.0f);
    const f32  view_h = std::max(inner.height(), lh);

    bool sb_active = false;
    if (st->overflow && st->content_h > view_h) {
        const f32 max_scroll = st->content_h - view_h;
        const f32 thumb_h    = std::max(20.0f, track.height() * view_h / st->content_h);
        const interaction sb = interact(hash_id("##mlscroll", key), track);
        if (sb.held) {
            const f32 t = std::clamp((m_->mouse_.y - track.min.y - thumb_h * 0.5f) / std::max(track.height() - thumb_h, 1.0f), 0.0f, 1.0f);
            st->scroll  = t * max_scroll;
            sb_active   = true;
        }
    }

    const rect hit = {box.min, {box.max.x - (st->overflow ? bar_w + 4.0f : 0.0f), box.max.y}};
    const interaction in = interact(key, hit);
    if (in.hovered || (in.held && m_->focus_id_ == key)) { m_->cursor_ = cursor_kind::text; }

    bool focused = m_->focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{m_->edit_buf_} : current; };
    ml_layout(text_now(), view_w, fnt, wrap);

    // caret geometry --------------------------------------------------------------------------------
    // a line with right-to-left text puts its caret and hits where the reordered letters are
    bidi_layout line_bidi;
    std::size_t line_bidi_for = ~std::size_t{0};
    u64         line_bidi_version = ~u64{0};
    const auto rtl_line = [&](std::size_t li) -> const bidi_layout* {
        const ml_line& l = m_->ml_lines_[li];
        const std::string_view t = text_now();
        if (l.end <= l.start || !has_rtl_text(t.substr(l.start, l.end - l.start))) { return nullptr; }
        if (line_bidi_for != li || line_bidi_version != m_->edit_version_ + m_->ml_cache_key_) {
            line_bidi.build(m_->font_, fnt, t.substr(l.start, l.end - l.start));
            line_bidi_for     = li;
            line_bidi_version = m_->edit_version_ + m_->ml_cache_key_;
        }
        return &line_bidi;
    };
    const auto line_x = [&](std::size_t li, std::size_t index) -> f32 { // x of a byte index inside a line, unscrolled
        const ml_line& l = m_->ml_lines_[li];
        const std::size_t at = std::clamp<std::size_t>(index, l.start, l.end);
        if (const bidi_layout* b = rtl_line(li)) {
            return b->caret_x(at - l.start);
        }
        return ed_measure(fnt, text_now(), l.start, at);
    };
    // the last position the caret may take on a line: not past the break of a wrapped line
    const auto line_hi = [&](std::size_t li) -> std::size_t {
        const ml_line& l = m_->ml_lines_[li];
        const bool soft = li + 1 < m_->ml_lines_.size() && m_->ml_lines_[li + 1].start == l.end;
        return soft && l.end > l.start ? prev_boundary(text_now(), l.end) : l.end;
    };
    const auto index_in_line = [&](std::size_t li, f32 rel_x) -> std::size_t {
        const std::string_view t = text_now();
        const ml_line&    l  = m_->ml_lines_[li];
        const std::size_t hi = line_hi(li);
        if (const bidi_layout* b = rtl_line(li)) {
            return std::min<std::size_t>(l.start + b->index_at(rel_x), hi);
        }
        std::size_t best   = l.start;
        f32         best_d = std::abs(rel_x);
        f32         x      = 0.0f;
        char32_t    prev   = 0;
        for (std::size_t i = l.start; i < hi;) {
            const std::size_t j = next_boundary(t, i);
            std::string_view  one = t.substr(i, j - i);
            const char32_t    cp  = decode_utf8(one);
            const font_id ff = ed_font_at(fnt, i);
            x += m_->font_.advance(ff, cp) + (prev != 0 ? m_->font_.kerning(ff, prev, cp) : 0.0f);
            prev = cp;
            const f32 d = std::abs(x - rel_x);
            if (d < best_d) { best_d = d; best = j; }
            i = j;
        }
        return best;
    };
    const auto index_at = [&](vec2 p) -> std::size_t {
        const f32 fy = (p.y - inner.min.y + st->scroll) / lh;
        const std::size_t li = fy <= 0.0f ? 0 : std::min<std::size_t>(static_cast<std::size_t>(fy), m_->ml_lines_.size() - 1);
        return index_in_line(li, p.x - inner.min.x + (wrap ? 0.0f : m_->edit_scroll_));
    };

    // focus and mouse --------------------------------------------------------------------------------------
    const bool press_here = m_->mouse_pressed_ && in.held;
    if (press_here) {
        m_->press_claimed_ = true;
        if (m_->focus_id_ != key) {
            m_->focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(); // nothing of the previous field's text may stay behind
            m_->edit_buf_.assign(current);
            m_->edit_scroll_ = 0.0f;
            m_->edit_cursor_ = m_->edit_buf_.size();
            m_->edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : m_->edit_cursor_;
            m_->caret_time_  = m_->time_;
        }
        const std::size_t idx  = index_at(m_->mouse_);
        const u32         clicks = register_click();
        m_->edit_pref_x_     = -1.0f;
        if (clicks == 3) { // triple click: the line between two line breaks, with its break
            const std::size_t at = std::min(idx, m_->edit_buf_.size());
            const std::size_t nl_before = at == 0 ? std::string::npos : std::string_view{m_->edit_buf_}.rfind('\n', at - 1);
            const std::size_t nl_after  = std::string_view{m_->edit_buf_}.find('\n', at);
            m_->edit_anchor_ = nl_before == std::string::npos ? 0 : nl_before + 1;
            m_->edit_cursor_ = nl_after == std::string::npos ? m_->edit_buf_.size() : nl_after + 1;
        } else if (clicks == 2) {
            m_->edit_anchor_ = word_start(m_->edit_buf_, idx);
            m_->edit_cursor_ = word_end(m_->edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || m_->edit_cursor_ != 0) {
            m_->edit_cursor_ = idx;
            m_->edit_anchor_ = idx;
        }
        m_->caret_time_ = m_->time_;
    } else if (focused && in.held) {
        m_->edit_cursor_ = index_at(m_->mouse_); // dragging selects, and pulls the view along past the edges
        m_->caret_time_  = m_->time_;
        if (m_->mouse_.y < inner.min.y)      { st->scroll -= (inner.min.y - m_->mouse_.y) * 8.0f * m_->dt_ + 1.0f; }
        else if (m_->mouse_.y > inner.max.y) { st->scroll += (m_->mouse_.y - inner.max.y) * 8.0f * m_->dt_ + 1.0f; }
    }
    if (focused) {
        m_->focus_seen_ = true;
    }

    // keyboard -----------------------------------------------------------------------------------------------
    bool changed     = false;
    bool caret_moved = press_here;
    if (focused) {
        m_->edit_readonly_   = readonly;
        m_->edit_max_bytes_  = max_bytes;
        m_->edit_history_on_ = !readonly;

        if (m_->typed_len_ != 0) {
            if (has_code(code_flags::auto_indent) && m_->typed_len_ == 1 && m_->typed_[0] == '}' && m_->edit_cursor_ == m_->edit_anchor_) {
                // a } on a line that holds only indentation steps back one level
                const std::string_view t0 = m_->edit_buf_;
                const std::size_t nl = m_->edit_cursor_ == 0 ? npos : t0.rfind('\n', m_->edit_cursor_ - 1);
                const std::size_t ls = nl == npos ? 0 : nl + 1;
                bool blank = m_->edit_cursor_ > ls;
                for (std::size_t i = ls; i < m_->edit_cursor_ && blank; ++i) { blank = t0[i] == ' '; }
                if (blank) {
                    const std::size_t drop = std::min<std::size_t>(m_->edit_cursor_ - ls, static_cast<std::size_t>(std::max(code.tab_size, 1)));
                    changed = edit_replace(m_->edit_cursor_ - drop, drop, {}, edit_kind::other) || changed;
                }
            }
            changed = edit_insert({m_->typed_.data(), m_->typed_len_}, true) || changed;
            m_->typed_len_   = 0;
            m_->caret_time_  = m_->time_;
            m_->edit_pref_x_ = -1.0f;
            caret_moved  = true;
        }

        for (u32 i = 0; i < m_->key_count_ && focused; ++i) {
            const key_event& ev = m_->keys_[i];
            if (ev.alt) { continue; } // Alt + key is a shortcut of the host, not editing
            ml_layout(m_->edit_buf_, view_w, fnt, wrap); // earlier keys of this frame may have changed the lines
            const std::string_view t = m_->edit_buf_;
            const bool has_sel = m_->edit_cursor_ != m_->edit_anchor_;
            const std::size_t li = ml_line_of(m_->edit_cursor_);
            bool vertical = false;
            m_->caret_time_ = m_->time_;
            caret_moved = true;

            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_cursor_ = m_->edit_anchor_ = std::min(m_->edit_cursor_, m_->edit_anchor_);
                } else {
                    m_->edit_cursor_ = ev.ctrl ? prev_word(t, m_->edit_cursor_) : prev_boundary(t, m_->edit_cursor_);
                    if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    m_->edit_cursor_ = m_->edit_anchor_ = std::max(m_->edit_cursor_, m_->edit_anchor_);
                } else {
                    m_->edit_cursor_ = ev.ctrl ? next_word(t, m_->edit_cursor_) : next_boundary(t, m_->edit_cursor_);
                    if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                }
                break;
            case key::up:
            case key::down:
            case key::page_up:
            case key::page_down: {
                vertical = true;
                if (m_->edit_pref_x_ < 0.0f) { m_->edit_pref_x_ = line_x(li, m_->edit_cursor_); }
                const auto page = std::max<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(view_h / lh) - 1);
                std::ptrdiff_t delta = 1;
                if (ev.k == key::up)             { delta = -1; }
                else if (ev.k == key::page_up)   { delta = -page; }
                else if (ev.k == key::page_down) { delta = page; }
                const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(li) + delta;
                if (target < 0) {
                    m_->edit_cursor_ = 0;
                } else if (target >= static_cast<std::ptrdiff_t>(m_->ml_lines_.size())) {
                    m_->edit_cursor_ = t.size();
                } else {
                    m_->edit_cursor_ = index_in_line(static_cast<std::size_t>(target), m_->edit_pref_x_);
                }
                if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                break;
            }
            case key::home:
                m_->edit_cursor_ = ev.ctrl ? 0 : m_->ml_lines_[li].start;
                if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                break;
            case key::end:
                m_->edit_cursor_ = ev.ctrl ? t.size() : line_hi(li);
                if (!ev.shift) { m_->edit_anchor_ = m_->edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, m_->edit_cursor_) : prev_boundary(t, m_->edit_cursor_);
                        if (a < m_->edit_cursor_) {
                            changed = edit_replace(a, m_->edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, m_->edit_cursor_) : next_boundary(t, m_->edit_cursor_);
                        if (b > m_->edit_cursor_) {
                            changed = edit_replace(m_->edit_cursor_, b - m_->edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                if (ev.ctrl) {
                    m_->submitted_ = true;
                } else if (!readonly) {
                    if (has_code(code_flags::auto_indent)) {
                        // the new line starts with the indentation of this one, a level deeper after an opening bracket
                        const std::size_t lo = std::min(m_->edit_cursor_, m_->edit_anchor_);
                        const std::size_t nl = lo == 0 ? npos : t.rfind('\n', lo - 1);
                        const std::size_t ls = nl == npos ? 0 : nl + 1;
                        std::size_t n = 0;
                        while (ls + n < lo && (t[ls + n] == ' ' || t[ls + n] == '\t')) { ++n; }
                        std::string insert = "\n";
                        insert.append(t.substr(ls, n));
                        std::size_t k = lo;
                        while (k > ls && (t[k - 1] == ' ' || t[k - 1] == '\t')) { --k; }
                        if (k > ls && (t[k - 1] == '{' || t[k - 1] == '(' || t[k - 1] == '[')) {
                            insert.append(static_cast<std::size_t>(std::max(code.tab_size, 1)), ' ');
                        }
                        changed = edit_insert(insert, false) || changed;
                    } else {
                        changed = edit_insert("\n", false) || changed;
                    }
                }
                break;
            case key::tab:
                if (!readonly) {
                    if (code_on) {
                        const std::size_t lo = std::min(m_->edit_cursor_, m_->edit_anchor_);
                        const std::size_t hi = std::max(m_->edit_cursor_, m_->edit_anchor_);
                        const bool many_lines = has_sel && t.substr(lo, hi - lo).find('\n') != npos;
                        if (many_lines || ev.shift) {
                            changed = edit_indent_lines(ev.shift, code.tab_size) || changed;
                        } else { // to the next tab stop
                            const std::size_t nl  = lo == 0 ? npos : t.rfind('\n', lo - 1);
                            const std::size_t col = lo - (nl == npos ? 0 : nl + 1);
                            const std::size_t tab = static_cast<std::size_t>(std::max(code.tab_size, 1));
                            changed = edit_insert(std::string(tab - col % tab, ' '), false) || changed;
                        }
                    } else if (!ev.shift) {
                        changed = edit_insert("    ", false) || changed;
                    }
                }
                break;
            case key::escape:
                m_->focus_id_ = 0;
                focused   = false;
                break;
            case key::a:
            case key::c:
            case key::x:
            case key::v:
            case key::z:
            case key::y:
                changed = edit_shortcut(ev, false, true) || changed;
                break;
            default:
                caret_moved = false;
                break;
            }
            if (!vertical) { m_->edit_pref_x_ = -1.0f; }
        }
        m_->key_count_ = 0;
    }
    ml_layout(text_now(), view_w, fnt, wrap);

    // scrolling ---------------------------------------------------------------------------------------------
    const std::size_t line_count = m_->ml_lines_.size();
    const f32 content_h  = static_cast<f32>(line_count) * lh;
    const f32 max_scroll = std::max(0.0f, content_h - view_h);
    st->content_h = content_h;
    st->overflow  = max_scroll > 0.0f;
    if (code_on && code.goto_offset != npos) { // a jump to a match / line (the caret follows, below, when the field has focus)
        const std::size_t gl = ml_line_of(std::min(code.goto_offset, text_now().size()));
        st->scroll = std::max(0.0f, static_cast<f32>(gl) * lh - view_h * 0.35f);
    }
    if (st->overflow && m_->wheel_ != 0.0f && !m_->wheel_consumed_ && pointer_over(box)) {
        st->scroll -= m_->wheel_ * lh * 3.0f;
        m_->wheel_consumed_ = true;
    }
    if (focused && caret_moved && !sb_active) { // keep the caret in view
        const std::size_t cl = ml_line_of(m_->edit_cursor_);
        const f32 top = static_cast<f32>(cl) * lh;
        if (top < st->scroll)                       { st->scroll = top; }
        if (top + lh > st->scroll + view_h)         { st->scroll = top + lh - view_h; }
        if (!wrap) {
            const f32 cx = line_x(cl, m_->edit_cursor_);
            if (cx - m_->edit_scroll_ > view_w - 2.0f) { m_->edit_scroll_ = cx - view_w + 2.0f; }
            if (cx - m_->edit_scroll_ < 0.0f)          { m_->edit_scroll_ = cx; }
        }
    }
    st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);
    if (wrap && focused) { m_->edit_scroll_ = 0.0f; }
    m_->edit_scroll_ = std::max(m_->edit_scroll_, 0.0f);

    // drawing -----------------------------------------------------------------------------------------------
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, focused ? 1.0f : 0.0f);

    if (!frameless) {
        shape_style field;
        field.radius       = radii(m_->style_.rounding * 0.8f);
        field.fill_top     = lerp(m_->style_.widget_bg, color{0, 0, 0, m_->style_.widget_bg.a}, 0.28f);
        field.fill_bottom  = lerp(m_->style_.widget_bg, color{0, 0, 0, m_->style_.widget_bg.a}, 0.12f);
        field.border       = lerp(lerp(m_->style_.widget_border, m_->style_.accent_hover, a.hover * 0.45f), m_->style_.accent, a.toggle);
        field.border_width = 1.0f + a.toggle * 0.5f;
        field.shadow       = m_->style_.accent.scaled_alpha(0.32f * a.toggle);
        field.shadow_blur  = 9.0f * a.toggle;
        m_->dl_.shape(box, field);
    }

    m_->dl_.push_clip({{inner.min.x, box.min.y + (frameless ? 0.0f : 1.0f)}, {inner.max.x, box.max.y - (frameless ? 0.0f : 1.0f)}});
    const std::string_view t = text_now();
    const f32 text_x = inner.min.x - (wrap || !focused ? 0.0f : m_->edit_scroll_);
    const std::size_t first = static_cast<std::size_t>(std::max(0.0f, std::floor(st->scroll / lh)));
    const std::size_t last  = std::min(line_count, first + static_cast<std::size_t>(view_h / lh) + 2);

    const bool        has_selection = focused && m_->edit_cursor_ != m_->edit_anchor_;
    const std::size_t sel_lo        = std::min(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t sel_hi        = std::max(m_->edit_cursor_, m_->edit_anchor_);
    shape_style sel;
    sel.radius      = radii(2.0f);
    sel.fill_top    = m_->style_.accent.scaled_alpha(0.45f);
    sel.fill_bottom = sel.fill_top;

    std::size_t bracket_a = npos, bracket_b = npos;
    if (has_code(code_flags::bracket_match) && focused && !has_selection) {
        (void)find_bracket_pair(t, m_->edit_cursor_, bracket_a, bracket_b);
    }
    const std::size_t caret_line = focused ? ml_line_of(m_->edit_cursor_) : npos;

    for (std::size_t li = first; li < last; ++li) {
        const ml_line& l = m_->ml_lines_[li];
        const f32 y = inner.min.y + static_cast<f32>(li) * lh - st->scroll;
        if (has_code(code_flags::highlight_line) && li == caret_line && !has_selection) {
            m_->dl_.rect_filled({{inner.min.x - 3.0f, y}, {inner.max.x + 2.0f, y + lh}}, m_->style_.accent.scaled_alpha(0.07f));
        }
        if (code_on && !code.marks.empty()) { // find matches: all of them faintly, the current one strongly
            auto it = std::lower_bound(code.marks.begin(), code.marks.end(), l.start,
                                       [](const std::pair<u32, u32>& m, std::size_t v) { return m.first < v; });
            for (; it != code.marks.end() && it->first <= l.end; ++it) {
                const bool current_mark = static_cast<int>(it - code.marks.begin()) == code.mark_active;
                const std::size_t end = std::min<std::size_t>(it->first + it->second, l.end);
                const f32 x0 = text_x + line_x(li, it->first);
                const f32 x1 = text_x + line_x(li, end);
                shape_style mark;
                mark.radius      = radii(2.0f);
                mark.fill_top    = current_mark ? color{255, 176, 64, 150} : color{255, 200, 80, 60};
                mark.fill_bottom = mark.fill_top;
                m_->dl_.shape({{x0, y}, {std::max(x1, x0 + 2.0f), y + lh}}, mark);
            }
        }
        for (const std::size_t p : {bracket_a, bracket_b}) { // the pair of brackets at the caret
            if (p == npos || p < l.start || p >= l.end) { continue; }
            const f32 x0 = text_x + line_x(li, p);
            const f32 x1 = text_x + line_x(li, next_boundary(t, p));
            m_->dl_.rect_filled({{x0, y}, {x1, y + lh}}, m_->style_.accent.scaled_alpha(0.28f), 2.0f);
            m_->dl_.rect_outline({{x0, y}, {x1, y + lh}}, m_->style_.accent_hover, 2.0f, 1.0f);
        }
        if (has_selection) {
            const bool hard = !(li + 1 < line_count && m_->ml_lines_[li + 1].start == l.end);
            const std::size_t sa = std::clamp<std::size_t>(sel_lo, l.start, l.end);
            const std::size_t sb = std::clamp<std::size_t>(sel_hi, l.start, l.end);
            const bool covers_newline = hard && l.end < t.size() && sel_lo <= l.end && sel_hi > l.end;
            if (sa < sb || covers_newline) {
                const f32 x0 = text_x + line_x(li, sa);
                const f32 x1 = text_x + line_x(li, sb) + (covers_newline ? lh * 0.3f : 0.0f);
                m_->dl_.shape({{x0, y}, {x1, y + lh}}, sel);
            }
        }
        if (l.end > l.start) {
            if (m_->edit_spans_.empty()) {
                m_->dl_.text({text_x, y}, text_col, t.substr(l.start, l.end - l.start), fnt);
            } else {
                ed_draw({text_x, y}, asc, text_col, t, l.start, l.end, fnt);
            }
        }
    }
    if (t.empty() && !focused && !hint.empty()) {
        m_->dl_.text({inner.min.x, inner.min.y}, m_->style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    }

    vec2 chip_at{};
    bool want_chip = false;
    if (focused && !readonly) {
        const std::size_t cl = ml_line_of(m_->edit_cursor_);
        const f32 cx = std::round(text_x + line_x(cl, m_->edit_cursor_));
        const f32 cy = inner.min.y + static_cast<f32>(cl) * lh - st->scroll;
        m_->ime_want_   = true;
        m_->ime_pos_    = {cx * m_->scale_, (cy + lh) * m_->scale_};
        m_->ime_line_h_ = lh * m_->scale_;
        if (m_->ime_len_ != 0) {
            want_chip = true;
            chip_at   = {cx, cy + lh};
        } else if (caret_visible()) {
            m_->dl_.rect_filled({{cx, cy}, {cx + 1.5f, cy + lh}}, m_->style_.text);
        }
    }
    m_->dl_.pop_clip();
    if (gutter_w > 0.0f) { // line numbers, right aligned; the caret's line brighter
        const rect g = {{box.min.x + 1.0f, box.min.y + 1.0f}, {inner.min.x - 4.0f, box.max.y - 1.0f}};
        m_->dl_.rect_filled(g, color{0, 0, 0, 44}, m_->style_.rounding * 0.7f, corners::tl | corners::bl);
        m_->dl_.rect_filled({{g.max.x, g.min.y}, {g.max.x + 1.0f, g.max.y}}, m_->style_.border.scaled_alpha(0.7f));
        m_->dl_.push_clip(g);
        for (std::size_t li = first; li < last; ++li) {
            const std::string num = std::to_string(li + 1);
            const f32 y = inner.min.y + static_cast<f32>(li) * lh - st->scroll;
            m_->dl_.text({g.max.x - 8.0f - m_->font_.measure(fnt, num).x, y}, li == caret_line ? m_->style_.text : m_->style_.text_dim.scaled_alpha(0.75f), num, fnt);
        }
        m_->dl_.pop_clip();
    }
    if (want_chip) { draw_ime_chip(chip_at, lh, fnt); }

    if (st->overflow) {
        const f32  thumb_h = std::max(20.0f, track.height() * view_h / content_h);
        const f32  thumb_y = track.min.y + (track.height() - thumb_h) * (st->scroll / max_scroll);
        shape_style bar;
        bar.radius      = radii(2.5f);
        bar.fill_top    = m_->style_.text_dim.scaled_alpha(sb_active ? 0.85f : 0.4f);
        bar.fill_bottom = bar.fill_top;
        m_->dl_.shape({{track.max.x - 6.0f, thumb_y}, {track.max.x - 1.0f, thumb_y + thumb_h}}, bar);
    }
    track_edit(key, changed, m_->focus_id_ == key);
    return changed;
}

// clicks, styled contents, input method -------------------------------------------------------------------------

u32 context::register_click() noexcept
{
    const vec2 moved = m_->mouse_ - m_->last_click_pos_;
    const bool near_ = m_->time_ - m_->last_click_time_ < m_->double_click_ && dot(moved, moved) < 25.0f;
    m_->click_count_     = near_ ? (m_->click_count_ >= 3 ? 1u : m_->click_count_ + 1u) : 1u;
    m_->last_click_time_ = m_->time_;
    m_->last_click_pos_  = m_->mouse_;
    return m_->click_count_;
}

void context::input_spans(std::span<const text_span> spans)
{
    m_->edit_spans_pending_.assign(spans.begin(), spans.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(spans.size(), 4096)));
}

// takes the spans given for this field: sorted, clamped to the text, without overlaps; the line gets as high as the
// tallest font in them
void context::ed_prepare_spans(std::size_t text_size, font_id base, f32& line_h, f32& ascent, bool ignore)
{
    m_->edit_spans_.clear();
    m_->edit_spans_hash_ = 0;
    if (m_->edit_spans_pending_.empty()) {
        return;
    }
    if (!ignore) {
        std::stable_sort(m_->edit_spans_pending_.begin(), m_->edit_spans_pending_.end(),
                         [](const text_span& a, const text_span& b) { return a.start < b.start; });
        u32 reach = 0;
        u64 h = 1469598103934665603ull;
        for (text_span s : m_->edit_spans_pending_) {
            s.end   = static_cast<u32>(std::min<std::size_t>(s.end, text_size));
            s.start = std::max(s.start, reach);
            if (s.end <= s.start) { continue; }
            if (s.font >= m_->font_.font_count()) { s.font = base; }
            reach = s.end;
            m_->edit_spans_.push_back(s);
            line_h = std::max(line_h, m_->font_.line_height(s.font));
            ascent = std::max(ascent, m_->font_.ascent(s.font));
            for (const u32 v : {s.start, s.end, s.font, static_cast<u32>(s.style)}) { h = (h ^ v) * 1099511628211ull; }
        }
        m_->edit_spans_hash_ = h | 1ull;
    }
    m_->edit_spans_pending_.clear();
}

f32 context::ed_measure(font_id base, std::string_view t, std::size_t a, std::size_t b) const
{
    b = std::min(b, t.size());
    a = std::min(a, b);
    if (m_->edit_spans_.empty()) {
        return b > a ? m_->font_.measure(base, t.substr(a, b - a)).x : 0.0f;
    }
    f32 x = 0.0f;
    std::size_t pos = a;
    for (const text_span& s : m_->edit_spans_) {
        if (s.end <= pos) { continue; }
        if (s.start >= b) { break; }
        if (s.start > pos) {
            x += m_->font_.measure(base, t.substr(pos, s.start - pos)).x;
            pos = s.start;
        }
        const std::size_t e = std::min<std::size_t>(s.end, b);
        if (e > pos) { x += m_->font_.measure(s.font, t.substr(pos, e - pos)).x; }
        pos = e;
    }
    if (pos < b) { x += m_->font_.measure(base, t.substr(pos, b - pos)).x; }
    return x;
}

font_id context::ed_font_at(font_id base, std::size_t i) const noexcept
{
    for (const text_span& s : m_->edit_spans_) {
        if (i < s.start) { break; }
        if (i < s.end) { return s.font; }
    }
    return base;
}

// draws t[a, b) run by run, all on the baseline of the line
void context::ed_draw(vec2 pos, f32 line_ascent, color col, std::string_view t, std::size_t a, std::size_t b, font_id base)
{
    b = std::min(b, t.size());
    a = std::min(a, b);
    f32 x = 0.0f;
    std::size_t at = a;
    const auto run = [&](std::size_t from, std::size_t to, font_id f, color c, text_flags style) {
        if (to <= from) { return; }
        const std::string_view piece = t.substr(from, to - from);
        m_->dl_.text({pos.x + x, pos.y + line_ascent - m_->font_.ascent(f)}, c, piece, f, style);
        x += m_->font_.measure(f, piece).x;
    };
    for (const text_span& s : m_->edit_spans_) {
        if (s.end <= at) { continue; }
        if (s.start >= b) { break; }
        if (s.start > at) { run(at, s.start, base, col, text_flags::none); at = s.start; }
        const std::size_t e = std::min<std::size_t>(s.end, b);
        run(at, e, s.font, s.col.a != 0 ? s.col : col, s.style);
        at = e;
    }
    run(at, b, base, col, text_flags::none);
}

// the composition of an input method in a multi-line field: a small box at the caret (the text below is not reflowed)
void context::draw_ime_chip(vec2 caret_bottom, f32 line_h, font_id f)
{
    const std::string_view comp{m_->ime_text_.data(), m_->ime_len_};
    const f32 pad = 6.0f;
    const vec2 ts = m_->font_.measure(f, comp);
    vec2 pos{caret_bottom.x, caret_bottom.y + 2.0f};
    const vec2 size{ts.x + 2.0f * pad, line_h + 4.0f};
    if (pos.x + size.x > m_->display_.x - 4.0f) { pos.x = m_->display_.x - 4.0f - size.x; }
    if (pos.y + size.y > m_->display_.y - 4.0f) { pos.y = caret_bottom.y - line_h - 2.0f - size.y; }
    pos.x = std::max(pos.x, 4.0f);

    const u32 previous_owner = m_->run_owner_;
    switch_run(run_overlay);
    m_->dl_.push_clip_absolute({{0.0f, 0.0f}, m_->display_});
    shape_style body;
    body.radius       = radii(m_->style_.rounding * 0.5f);
    body.fill_top     = color{m_->style_.window_bg.r, m_->style_.window_bg.g, m_->style_.window_bg.b, 255};
    body.fill_bottom  = body.fill_top;
    body.border       = m_->style_.accent;
    body.border_width = 1.0f;
    body.shadow       = m_->style_.shadow;
    body.shadow_blur  = m_->style_.shadow_blur * 0.4f;
    body.shadow_offset = {0.0f, 2.0f};
    m_->dl_.shape(rect::from_size(pos, size), body);
    const vec2 tp{pos.x + pad, pos.y + 2.0f};
    m_->dl_.text(tp, m_->style_.text, comp, f);
    m_->dl_.rect_filled({{tp.x, tp.y + line_h - 1.0f}, {tp.x + ts.x, tp.y + line_h + 0.5f}}, m_->style_.accent_hover);
    const f32 cx = std::round(tp.x + m_->font_.measure(f, comp.substr(0, m_->ime_cursor_)).x);
    m_->dl_.rect_filled({{cx, tp.y}, {cx + 1.5f, tp.y + line_h}}, m_->style_.text);
    m_->dl_.pop_clip();
    switch_run(previous_owner);
}

} // namespace strata

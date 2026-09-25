// text editing engine shared by the text fields (undo / redo, clipboard shortcuts) and the multi-line input

#include "strata/context.hpp"

#include "strata/bidi.hpp"
#include "text_util.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace strata {

using namespace text;

namespace {

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

} // namespace

// undo / redo ---------------------------------------------------------------------------------

void context::edit_history_clear() noexcept
{
    for (edit_history* h : {&undo_, &redo_}) {
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
    pos = std::min(pos, edit_buf_.size());
    len = std::min(len, edit_buf_.size() - pos);
    if (len == 0 && with.empty()) {
        return false;
    }

    if (edit_history_on_) {
        bool merged = false;
        if (!undo_.ops.empty() && kind != edit_kind::other) {
            edit_op& last = undo_.ops.back();
            if (last.kind == kind && time_ - last.time < 1.0) {
                const std::string_view removed = std::string_view{edit_buf_}.substr(pos, len);
                if (kind == edit_kind::typing && last.rem_len == 0 && len == 0 && pos == last.pos + last.ins_len) {
                    undo_.text.append(with);
                    last.ins_len     += with.size();
                    last.cursor_after = pos + with.size();
                    merged = true;
                } else if (kind == edit_kind::erase_back && last.ins_len == 0 && with.empty() && pos + len == last.pos) {
                    undo_.text.insert(last.off, removed);
                    last.rem_len     += len;
                    last.pos          = pos;
                    last.cursor_after = pos;
                    merged = true;
                } else if (kind == edit_kind::erase_fwd && last.ins_len == 0 && with.empty() && pos == last.pos) {
                    undo_.text.append(removed);
                    last.rem_len     += len;
                    last.cursor_after = pos;
                    merged = true;
                }
                if (merged) { last.time = time_; }
            }
        }
        if (!merged) {
            edit_op op;
            op.pos           = pos;
            op.rem_len       = len;
            op.ins_len       = with.size();
            op.cursor_before = edit_cursor_;
            op.anchor_before = edit_anchor_;
            op.cursor_after  = pos + with.size();
            op.kind          = kind;
            op.time          = time_;
            edit_history_add(undo_, op, std::string_view{edit_buf_}.substr(pos, len), with);
        }
        // a new change ends the redo chain
        redo_.text.resize(redo_.text.capacity());
        detail::secure_wipe(redo_.text.data(), redo_.text.size());
        redo_.text.clear();
        redo_.ops.clear();
    }

    edit_buf_.replace(pos, len, with);
    edit_cursor_ = edit_anchor_ = pos + with.size();
    ++edit_version_;
    return true;
}

bool context::edit_delete_selection()
{
    if (edit_cursor_ == edit_anchor_) {
        return false;
    }
    const std::size_t lo = std::min(edit_cursor_, edit_anchor_);
    const std::size_t hi = std::max(edit_cursor_, edit_anchor_);
    return edit_replace(lo, hi - lo, {}, edit_kind::other);
}

// typed / pasted text replaces the selection; cut at the field's byte limit on a code point boundary
bool context::edit_insert(std::string_view s, bool typed)
{
    if (edit_readonly_) {
        return false;
    }
    const std::size_t lo   = std::min(edit_cursor_, edit_anchor_);
    const std::size_t hi   = std::max(edit_cursor_, edit_anchor_);
    const std::size_t base = edit_buf_.size() - (hi - lo);
    const std::size_t room = edit_max_bytes_ > base ? edit_max_bytes_ - base : 0;
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
    if (!edit_history_on_ || undo_.ops.empty()) {
        return false;
    }
    const edit_op op = undo_.ops.back();
    if (op.pos + op.ins_len > edit_buf_.size()) { // the buffer was changed behind our back: the history is useless
        edit_history_clear();
        return false;
    }
    const std::string_view removed{undo_.text.data() + op.off, op.rem_len};
    const std::string_view inserted{undo_.text.data() + op.off + op.rem_len, op.ins_len};
    edit_history_add(redo_, op, removed, inserted);
    edit_buf_.replace(op.pos, op.ins_len, removed);

    wipe_tail(undo_.text, op.off);
    undo_.ops.pop_back();
    edit_cursor_ = std::min(op.cursor_before, edit_buf_.size());
    edit_anchor_ = std::min(op.anchor_before, edit_buf_.size());
    ++edit_version_;
    return true;
}

bool context::edit_redo()
{
    if (!edit_history_on_ || redo_.ops.empty()) {
        return false;
    }
    const edit_op op = redo_.ops.back();
    if (op.pos + op.rem_len > edit_buf_.size()) {
        edit_history_clear();
        return false;
    }
    const std::string_view removed{redo_.text.data() + op.off, op.rem_len};
    const std::string_view inserted{redo_.text.data() + op.off + op.rem_len, op.ins_len};
    edit_history_add(undo_, op, removed, inserted);
    edit_buf_.replace(op.pos, op.rem_len, inserted);

    wipe_tail(redo_.text, op.off);
    redo_.ops.pop_back();
    edit_cursor_ = edit_anchor_ = std::min(op.cursor_after, edit_buf_.size());
    ++edit_version_;
    return true;
}

// ctrl+a / c / x / v / z / y on the focused field. returns true when the text changed.
bool context::edit_shortcut(const key_event& ev, bool password, bool multiline)
{
    const std::string_view t = edit_buf_;
    const std::size_t lo = std::min(edit_cursor_, edit_anchor_);
    const std::size_t hi = std::max(edit_cursor_, edit_anchor_);

    switch (ev.k) {
    case key::a:
        edit_anchor_ = 0;
        edit_cursor_ = t.size();
        return false;
    case key::c:
        if (!password && lo != hi && clipboard_.set != nullptr) {
            clipboard_.set(clipboard_.user, t.substr(lo, hi - lo));
        }
        return false;
    case key::x:
        if (!password && !edit_readonly_ && lo != hi) {
            if (clipboard_.set != nullptr) {
                clipboard_.set(clipboard_.user, t.substr(lo, hi - lo));
            }
            return edit_delete_selection();
        }
        return false;
    case key::v: {
        if (edit_readonly_ || clipboard_.get == nullptr) {
            return false;
        }
        std::string pasted;
        bool changed = false;
        if (clipboard_.get(clipboard_.user, pasted)) {
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
    const auto it = std::upper_bound(ml_lines_.begin(), ml_lines_.end(), index,
                                     [](std::size_t i, const ml_line& l) { return i < l.start; });
    return it == ml_lines_.begin() ? 0 : static_cast<std::size_t>(it - ml_lines_.begin()) - 1;
}

// splits the text into display lines: at every '\n' and, with `wrap`, at word boundaries once a line is wider than `width`
void context::ml_layout(std::string_view t, f32 width, font_id f, bool wrap)
{
    u64 h = 1469598103934665603ull; // fnv-1a over the text, so an unchanged text keeps its lines
    for (const char c : t) { h = (h ^ static_cast<u8>(c)) * 1099511628211ull; }
    h ^= (static_cast<u64>(std::bit_cast<u32>(width)) << 24) ^ (static_cast<u64>(f) << 56) ^ (wrap ? 0x5bd1e995ull : 0ull) ^
         (static_cast<u64>(t.size()) * 0x9e3779b97f4a7c15ull) ^ (edit_spans_hash_ * 0xff51afd7ed558ccdull);
    if (h == ml_cache_key_ && !ml_lines_.empty()) {
        return;
    }
    ml_cache_key_ = h;
    ml_lines_.clear();

    const auto push = [&](std::size_t a, std::size_t b) {
        ml_lines_.push_back({static_cast<u32>(a), static_cast<u32>(b)});
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
    if (cur_ == nullptr || text.empty()) {
        return;
    }
    (void)input_multiline_core(id_label, text, {}, input_flags::read_only | input_flags::no_frame | input_flags::auto_height, {},
                               std::size_t{1} << 24);
}

bool context::input_multiline(std::string_view label, std::string& value, vec2 size, input_flags flags,
                              std::string_view hint, std::size_t max_bytes)
{
    if (input_multiline_core(label, value, size, flags, hint, max_bytes)) {
        value.assign(edit_buf_.data(), edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_multiline(std::string_view label, secure_string& value, vec2 size, input_flags flags,
                              std::string_view hint, std::size_t max_bytes)
{
    if (input_multiline_core(label, value, size, flags, hint, max_bytes)) {
        value.assign(edit_buf_.data(), edit_buf_.size());
        return true;
    }
    return false;
}

bool context::input_multiline_core(std::string_view label, std::string_view current, vec2 size, input_flags flags,
                                   std::string_view hint, std::size_t max_bytes)
{
    if (cur_ == nullptr) {
        return false;
    }

    const font_id fnt      = current_font();
    f32           lh       = font_.line_height(fnt);
    f32           asc      = font_.ascent(fnt);
    const bool    readonly = has_flag(flags, input_flags::read_only);
    const bool    wrap     = !has_flag(flags, input_flags::no_wrap);
    const id      key      = hash_id(label, current_seed());
    ed_prepare_spans(focus_id_ == key ? edit_buf_.size() : current.size(), fnt, lh, asc, false);

    const bool frameless = has_flag(flags, input_flags::no_frame);
    const bool auto_h    = has_flag(flags, input_flags::auto_height);
    const color text_col = ml_color_.a != 0 ? ml_color_ : style_.text;
    ml_color_ = color{0, 0, 0, 0};

    field_layout fl;
    if (frameless) { // selectable text: no caption, no box; the size follows the text
        const f32 w = size.x > 0.0f ? size.x : (layout_.next_width > 0.0f ? layout_.next_width : layout_.width);
        layout_.next_width = 0.0f;
        f32 h = size.y;
        if (auto_h || h <= 0.0f) {
            ml_layout(focus_id_ == key ? std::string_view{edit_buf_} : current, std::max(w, 8.0f), fnt, wrap);
            h = static_cast<f32>(ml_lines_.size()) * lh;
        }
        fl.control = layout_place({w, std::max(h, lh)});
    } else {
        const f32 box_h = size.y > 0.0f ? size.y : lh * 6.0f + style_.frame_padding.y * 2.0f;
        if (size.x > 0.0f) { layout_.next_width = size.x; }
        fl = layout_field(visible_label(label), box_h);
    }
    const rect box = fl.control;

    child_state* st = internal::state_for(children_, key, frame_); // vertical scroll and content height of this field
    constexpr f32 bar_w = 10.0f;

    // the scrollbar column is its own control, so it comes before the field claims the press
    const rect track = {{box.max.x - bar_w - 1.0f, box.min.y + 3.0f}, {box.max.x - 2.0f, box.max.y - 3.0f}};
    const f32  pad_x = frameless ? 0.0f : style_.frame_padding.x;
    const f32  pad_y = frameless ? 0.0f : style_.frame_padding.y;
    const rect inner = {{box.min.x + pad_x, box.min.y + pad_y},
                        {box.max.x - (st->overflow ? bar_w + 3.0f : pad_x), box.max.y - pad_y}};
    const f32  view_w = std::max(inner.width(), 8.0f);
    const f32  view_h = std::max(inner.height(), lh);

    bool sb_active = false;
    if (st->overflow && st->content_h > view_h) {
        const f32 max_scroll = st->content_h - view_h;
        const f32 thumb_h    = std::max(20.0f, track.height() * view_h / st->content_h);
        const interaction sb = interact(hash_id("##mlscroll", key), track);
        if (sb.held) {
            const f32 t = std::clamp((mouse_.y - track.min.y - thumb_h * 0.5f) / std::max(track.height() - thumb_h, 1.0f), 0.0f, 1.0f);
            st->scroll  = t * max_scroll;
            sb_active   = true;
        }
    }

    const rect hit = {box.min, {box.max.x - (st->overflow ? bar_w + 4.0f : 0.0f), box.max.y}};
    const interaction in = interact(key, hit);
    if (in.hovered || (in.held && focus_id_ == key)) { cursor_ = cursor_kind::text; }

    bool focused = focus_id_ == key;
    const auto text_now = [&]() -> std::string_view { return focused ? std::string_view{edit_buf_} : current; };
    ml_layout(text_now(), view_w, fnt, wrap);

    // caret geometry --------------------------------------------------------------------------------
    // a line with right-to-left text puts its caret and hits where the reordered letters are
    bidi_layout line_bidi;
    std::size_t line_bidi_for = ~std::size_t{0};
    u64         line_bidi_version = ~u64{0};
    const auto rtl_line = [&](std::size_t li) -> const bidi_layout* {
        const ml_line& l = ml_lines_[li];
        const std::string_view t = text_now();
        if (l.end <= l.start || !has_rtl_text(t.substr(l.start, l.end - l.start))) { return nullptr; }
        if (line_bidi_for != li || line_bidi_version != edit_version_ + ml_cache_key_) {
            line_bidi.build(font_, fnt, t.substr(l.start, l.end - l.start));
            line_bidi_for     = li;
            line_bidi_version = edit_version_ + ml_cache_key_;
        }
        return &line_bidi;
    };
    const auto line_x = [&](std::size_t li, std::size_t index) -> f32 { // x of a byte index inside a line, unscrolled
        const ml_line& l = ml_lines_[li];
        const std::size_t at = std::clamp<std::size_t>(index, l.start, l.end);
        if (const bidi_layout* b = rtl_line(li)) {
            return b->caret_x(at - l.start);
        }
        return ed_measure(fnt, text_now(), l.start, at);
    };
    // the last position the caret may take on a line: not past the break of a wrapped line
    const auto line_hi = [&](std::size_t li) -> std::size_t {
        const ml_line& l = ml_lines_[li];
        const bool soft = li + 1 < ml_lines_.size() && ml_lines_[li + 1].start == l.end;
        return soft && l.end > l.start ? prev_boundary(text_now(), l.end) : l.end;
    };
    const auto index_in_line = [&](std::size_t li, f32 rel_x) -> std::size_t {
        const std::string_view t = text_now();
        const ml_line&    l  = ml_lines_[li];
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
            x += font_.advance(ff, cp) + (prev != 0 ? font_.kerning(ff, prev, cp) : 0.0f);
            prev = cp;
            const f32 d = std::abs(x - rel_x);
            if (d < best_d) { best_d = d; best = j; }
            i = j;
        }
        return best;
    };
    const auto index_at = [&](vec2 p) -> std::size_t {
        const f32 fy = (p.y - inner.min.y + st->scroll) / lh;
        const std::size_t li = fy <= 0.0f ? 0 : std::min<std::size_t>(static_cast<std::size_t>(fy), ml_lines_.size() - 1);
        return index_in_line(li, p.x - inner.min.x + (wrap ? 0.0f : edit_scroll_));
    };

    // focus and mouse --------------------------------------------------------------------------------------
    const bool press_here = mouse_pressed_ && in.held;
    if (press_here) {
        press_claimed_ = true;
        if (focus_id_ != key) {
            focus_id_    = key;
            focused      = true;
            wipe_edit_buffer(); // nothing of the previous field's text may stay behind
            edit_buf_.assign(current);
            edit_scroll_ = 0.0f;
            edit_cursor_ = edit_buf_.size();
            edit_anchor_ = has_flag(flags, input_flags::select_all_on_focus) ? 0 : edit_cursor_;
            caret_time_  = time_;
        }
        const std::size_t idx  = index_at(mouse_);
        const u32         clicks = register_click();
        edit_pref_x_     = -1.0f;
        if (clicks == 3) { // triple click: the line between two line breaks, with its break
            const std::size_t at = std::min(idx, edit_buf_.size());
            const std::size_t nl_before = at == 0 ? std::string::npos : std::string_view{edit_buf_}.rfind('\n', at - 1);
            const std::size_t nl_after  = std::string_view{edit_buf_}.find('\n', at);
            edit_anchor_ = nl_before == std::string::npos ? 0 : nl_before + 1;
            edit_cursor_ = nl_after == std::string::npos ? edit_buf_.size() : nl_after + 1;
        } else if (clicks == 2) {
            edit_anchor_ = word_start(edit_buf_, idx);
            edit_cursor_ = word_end(edit_buf_, idx);
        } else if (!has_flag(flags, input_flags::select_all_on_focus) || edit_cursor_ != 0) {
            edit_cursor_ = idx;
            edit_anchor_ = idx;
        }
        caret_time_ = time_;
    } else if (focused && in.held) {
        edit_cursor_ = index_at(mouse_); // dragging selects, and pulls the view along past the edges
        caret_time_  = time_;
        if (mouse_.y < inner.min.y)      { st->scroll -= (inner.min.y - mouse_.y) * 8.0f * dt_ + 1.0f; }
        else if (mouse_.y > inner.max.y) { st->scroll += (mouse_.y - inner.max.y) * 8.0f * dt_ + 1.0f; }
    }
    if (focused) {
        focus_seen_ = true;
    }

    // keyboard -----------------------------------------------------------------------------------------------
    bool changed     = false;
    bool caret_moved = press_here;
    if (focused) {
        edit_readonly_   = readonly;
        edit_max_bytes_  = max_bytes;
        edit_history_on_ = !readonly;

        if (typed_len_ != 0) {
            changed = edit_insert({typed_.data(), typed_len_}, true) || changed;
            typed_len_   = 0;
            caret_time_  = time_;
            edit_pref_x_ = -1.0f;
            caret_moved  = true;
        }

        for (u32 i = 0; i < key_count_ && focused; ++i) {
            const key_event& ev = keys_[i];
            ml_layout(edit_buf_, view_w, fnt, wrap); // earlier keys of this frame may have changed the lines
            const std::string_view t = edit_buf_;
            const bool has_sel = edit_cursor_ != edit_anchor_;
            const std::size_t li = ml_line_of(edit_cursor_);
            bool vertical = false;
            caret_time_ = time_;
            caret_moved = true;

            switch (ev.k) {
            case key::left:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    edit_cursor_ = edit_anchor_ = std::min(edit_cursor_, edit_anchor_);
                } else {
                    edit_cursor_ = ev.ctrl ? prev_word(t, edit_cursor_) : prev_boundary(t, edit_cursor_);
                    if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                }
                break;
            case key::right:
                if (!ev.shift && !ev.ctrl && has_sel) {
                    edit_cursor_ = edit_anchor_ = std::max(edit_cursor_, edit_anchor_);
                } else {
                    edit_cursor_ = ev.ctrl ? next_word(t, edit_cursor_) : next_boundary(t, edit_cursor_);
                    if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                }
                break;
            case key::up:
            case key::down:
            case key::page_up:
            case key::page_down: {
                vertical = true;
                if (edit_pref_x_ < 0.0f) { edit_pref_x_ = line_x(li, edit_cursor_); }
                const auto page = std::max<std::ptrdiff_t>(1, static_cast<std::ptrdiff_t>(view_h / lh) - 1);
                std::ptrdiff_t delta = 1;
                if (ev.k == key::up)             { delta = -1; }
                else if (ev.k == key::page_up)   { delta = -page; }
                else if (ev.k == key::page_down) { delta = page; }
                const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(li) + delta;
                if (target < 0) {
                    edit_cursor_ = 0;
                } else if (target >= static_cast<std::ptrdiff_t>(ml_lines_.size())) {
                    edit_cursor_ = t.size();
                } else {
                    edit_cursor_ = index_in_line(static_cast<std::size_t>(target), edit_pref_x_);
                }
                if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                break;
            }
            case key::home:
                edit_cursor_ = ev.ctrl ? 0 : ml_lines_[li].start;
                if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                break;
            case key::end:
                edit_cursor_ = ev.ctrl ? t.size() : line_hi(li);
                if (!ev.shift) { edit_anchor_ = edit_cursor_; }
                break;
            case key::backspace:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t a = ev.ctrl ? prev_word(t, edit_cursor_) : prev_boundary(t, edit_cursor_);
                        if (a < edit_cursor_) {
                            changed = edit_replace(a, edit_cursor_ - a, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_back) || changed;
                        }
                    }
                }
                break;
            case key::del:
                if (!readonly) {
                    if (edit_delete_selection()) {
                        changed = true;
                    } else {
                        const std::size_t b = ev.ctrl ? next_word(t, edit_cursor_) : next_boundary(t, edit_cursor_);
                        if (b > edit_cursor_) {
                            changed = edit_replace(edit_cursor_, b - edit_cursor_, {}, ev.ctrl ? edit_kind::other : edit_kind::erase_fwd) || changed;
                        }
                    }
                }
                break;
            case key::enter:
                if (ev.ctrl) {
                    submitted_ = true;
                } else if (!readonly) {
                    changed = edit_insert("\n", false) || changed;
                }
                break;
            case key::tab:
                if (!ev.shift && !readonly) {
                    changed = edit_insert("    ", false) || changed;
                }
                break;
            case key::escape:
                focus_id_ = 0;
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
            if (!vertical) { edit_pref_x_ = -1.0f; }
        }
        key_count_ = 0;
    }
    ml_layout(text_now(), view_w, fnt, wrap);

    // scrolling ---------------------------------------------------------------------------------------------
    const std::size_t line_count = ml_lines_.size();
    const f32 content_h  = static_cast<f32>(line_count) * lh;
    const f32 max_scroll = std::max(0.0f, content_h - view_h);
    st->content_h = content_h;
    st->overflow  = max_scroll > 0.0f;
    if (st->overflow && wheel_ != 0.0f && !wheel_consumed_ && pointer_over(box)) {
        st->scroll -= wheel_ * lh * 3.0f;
        wheel_consumed_ = true;
    }
    if (focused && caret_moved && !sb_active) { // keep the caret in view
        const std::size_t cl = ml_line_of(edit_cursor_);
        const f32 top = static_cast<f32>(cl) * lh;
        if (top < st->scroll)                       { st->scroll = top; }
        if (top + lh > st->scroll + view_h)         { st->scroll = top + lh - view_h; }
        if (!wrap) {
            const f32 cx = line_x(cl, edit_cursor_);
            if (cx - edit_scroll_ > view_w - 2.0f) { edit_scroll_ = cx - view_w + 2.0f; }
            if (cx - edit_scroll_ < 0.0f)          { edit_scroll_ = cx; }
        }
    }
    st->scroll = std::clamp(st->scroll, 0.0f, max_scroll);
    if (wrap && focused) { edit_scroll_ = 0.0f; }
    edit_scroll_ = std::max(edit_scroll_, 0.0f);

    // drawing -----------------------------------------------------------------------------------------------
    anim_slot& a = anim_for(key);
    a.hover  = approach(a.hover, in.hovered ? 1.0f : 0.0f);
    a.toggle = approach(a.toggle, focused ? 1.0f : 0.0f);

    if (!frameless) {
        shape_style field;
        field.radius       = radii(style_.rounding * 0.8f);
        field.fill_top     = lerp(style_.widget_bg, color{0, 0, 0, style_.widget_bg.a}, 0.28f);
        field.fill_bottom  = lerp(style_.widget_bg, color{0, 0, 0, style_.widget_bg.a}, 0.12f);
        field.border       = lerp(lerp(style_.widget_border, style_.accent_hover, a.hover * 0.45f), style_.accent, a.toggle);
        field.border_width = 1.0f + a.toggle * 0.5f;
        field.shadow       = style_.accent.scaled_alpha(0.32f * a.toggle);
        field.shadow_blur  = 9.0f * a.toggle;
        dl_.shape(box, field);
    }

    dl_.push_clip({{inner.min.x, box.min.y + (frameless ? 0.0f : 1.0f)}, {inner.max.x, box.max.y - (frameless ? 0.0f : 1.0f)}});
    const std::string_view t = text_now();
    const f32 text_x = inner.min.x - (wrap || !focused ? 0.0f : edit_scroll_);
    const std::size_t first = static_cast<std::size_t>(std::max(0.0f, std::floor(st->scroll / lh)));
    const std::size_t last  = std::min(line_count, first + static_cast<std::size_t>(view_h / lh) + 2);

    const bool        has_selection = focused && edit_cursor_ != edit_anchor_;
    const std::size_t sel_lo        = std::min(edit_cursor_, edit_anchor_);
    const std::size_t sel_hi        = std::max(edit_cursor_, edit_anchor_);
    shape_style sel;
    sel.radius      = radii(2.0f);
    sel.fill_top    = style_.accent.scaled_alpha(0.45f);
    sel.fill_bottom = sel.fill_top;

    for (std::size_t li = first; li < last; ++li) {
        const ml_line& l = ml_lines_[li];
        const f32 y = inner.min.y + static_cast<f32>(li) * lh - st->scroll;
        if (has_selection) {
            const bool hard = !(li + 1 < line_count && ml_lines_[li + 1].start == l.end);
            const std::size_t sa = std::clamp<std::size_t>(sel_lo, l.start, l.end);
            const std::size_t sb = std::clamp<std::size_t>(sel_hi, l.start, l.end);
            const bool covers_newline = hard && l.end < t.size() && sel_lo <= l.end && sel_hi > l.end;
            if (sa < sb || covers_newline) {
                const f32 x0 = text_x + line_x(li, sa);
                const f32 x1 = text_x + line_x(li, sb) + (covers_newline ? lh * 0.3f : 0.0f);
                dl_.shape({{x0, y}, {x1, y + lh}}, sel);
            }
        }
        if (l.end > l.start) {
            if (edit_spans_.empty()) {
                dl_.text({text_x, y}, text_col, t.substr(l.start, l.end - l.start), fnt);
            } else {
                ed_draw({text_x, y}, asc, text_col, t, l.start, l.end, fnt);
            }
        }
    }
    if (t.empty() && !focused && !hint.empty()) {
        dl_.text({inner.min.x, inner.min.y}, style_.text_dim.scaled_alpha(0.7f), hint, fnt);
    }

    vec2 chip_at{};
    bool want_chip = false;
    if (focused && !readonly) {
        const std::size_t cl = ml_line_of(edit_cursor_);
        const f32 cx = std::round(text_x + line_x(cl, edit_cursor_));
        const f32 cy = inner.min.y + static_cast<f32>(cl) * lh - st->scroll;
        ime_want_   = true;
        ime_pos_    = {cx * scale_, (cy + lh) * scale_};
        ime_line_h_ = lh * scale_;
        if (ime_len_ != 0) {
            want_chip = true;
            chip_at   = {cx, cy + lh};
        } else if (std::fmod(time_ - caret_time_, 1.06) < 0.53) { // the usual 530 ms on / 530 ms off
            dl_.rect_filled({{cx, cy}, {cx + 1.5f, cy + lh}}, style_.text);
        }
    }
    dl_.pop_clip();
    if (want_chip) { draw_ime_chip(chip_at, lh, fnt); }

    if (st->overflow) {
        const f32  thumb_h = std::max(20.0f, track.height() * view_h / content_h);
        const f32  thumb_y = track.min.y + (track.height() - thumb_h) * (st->scroll / max_scroll);
        shape_style bar;
        bar.radius      = radii(2.5f);
        bar.fill_top    = style_.text_dim.scaled_alpha(sb_active ? 0.85f : 0.4f);
        bar.fill_bottom = bar.fill_top;
        dl_.shape({{track.max.x - 6.0f, thumb_y}, {track.max.x - 1.0f, thumb_y + thumb_h}}, bar);
    }
    return changed;
}

// clicks, styled contents, input method -------------------------------------------------------------------------

u32 context::register_click() noexcept
{
    const vec2 moved = mouse_ - last_click_pos_;
    const bool near_ = time_ - last_click_time_ < 0.35 && dot(moved, moved) < 25.0f;
    click_count_     = near_ ? (click_count_ >= 3 ? 1u : click_count_ + 1u) : 1u;
    last_click_time_ = time_;
    last_click_pos_  = mouse_;
    return click_count_;
}

void context::input_spans(std::span<const text_span> spans)
{
    edit_spans_pending_.assign(spans.begin(), spans.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(spans.size(), 4096)));
}

// takes the spans given for this field: sorted, clamped to the text, without overlaps; the line gets as high as the
// tallest font in them
void context::ed_prepare_spans(std::size_t text_size, font_id base, f32& line_h, f32& ascent, bool ignore)
{
    edit_spans_.clear();
    edit_spans_hash_ = 0;
    if (edit_spans_pending_.empty()) {
        return;
    }
    if (!ignore) {
        std::stable_sort(edit_spans_pending_.begin(), edit_spans_pending_.end(),
                         [](const text_span& a, const text_span& b) { return a.start < b.start; });
        u32 reach = 0;
        u64 h = 1469598103934665603ull;
        for (text_span s : edit_spans_pending_) {
            s.end   = static_cast<u32>(std::min<std::size_t>(s.end, text_size));
            s.start = std::max(s.start, reach);
            if (s.end <= s.start) { continue; }
            if (s.font >= font_.font_count()) { s.font = base; }
            reach = s.end;
            edit_spans_.push_back(s);
            line_h = std::max(line_h, font_.line_height(s.font));
            ascent = std::max(ascent, font_.ascent(s.font));
            for (const u32 v : {s.start, s.end, s.font, static_cast<u32>(s.style)}) { h = (h ^ v) * 1099511628211ull; }
        }
        edit_spans_hash_ = h | 1ull;
    }
    edit_spans_pending_.clear();
}

f32 context::ed_measure(font_id base, std::string_view t, std::size_t a, std::size_t b) const
{
    b = std::min(b, t.size());
    a = std::min(a, b);
    if (edit_spans_.empty()) {
        return b > a ? font_.measure(base, t.substr(a, b - a)).x : 0.0f;
    }
    f32 x = 0.0f;
    std::size_t pos = a;
    for (const text_span& s : edit_spans_) {
        if (s.end <= pos) { continue; }
        if (s.start >= b) { break; }
        if (s.start > pos) {
            x += font_.measure(base, t.substr(pos, s.start - pos)).x;
            pos = s.start;
        }
        const std::size_t e = std::min<std::size_t>(s.end, b);
        if (e > pos) { x += font_.measure(s.font, t.substr(pos, e - pos)).x; }
        pos = e;
    }
    if (pos < b) { x += font_.measure(base, t.substr(pos, b - pos)).x; }
    return x;
}

font_id context::ed_font_at(font_id base, std::size_t i) const noexcept
{
    for (const text_span& s : edit_spans_) {
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
        dl_.text({pos.x + x, pos.y + line_ascent - font_.ascent(f)}, c, piece, f, style);
        x += font_.measure(f, piece).x;
    };
    for (const text_span& s : edit_spans_) {
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
    const std::string_view comp{ime_text_.data(), ime_len_};
    const f32 pad = 6.0f;
    const vec2 ts = font_.measure(f, comp);
    vec2 pos{caret_bottom.x, caret_bottom.y + 2.0f};
    const vec2 size{ts.x + 2.0f * pad, line_h + 4.0f};
    if (pos.x + size.x > display_.x - 4.0f) { pos.x = display_.x - 4.0f - size.x; }
    if (pos.y + size.y > display_.y - 4.0f) { pos.y = caret_bottom.y - line_h - 2.0f - size.y; }
    pos.x = std::max(pos.x, 4.0f);

    const u32 previous_owner = run_owner_;
    switch_run(run_overlay);
    dl_.push_clip_absolute({{0.0f, 0.0f}, display_});
    shape_style body;
    body.radius       = radii(style_.rounding * 0.5f);
    body.fill_top     = color{style_.window_bg.r, style_.window_bg.g, style_.window_bg.b, 255};
    body.fill_bottom  = body.fill_top;
    body.border       = style_.accent;
    body.border_width = 1.0f;
    body.shadow       = style_.shadow;
    body.shadow_blur  = style_.shadow_blur * 0.4f;
    body.shadow_offset = {0.0f, 2.0f};
    dl_.shape(rect::from_size(pos, size), body);
    const vec2 tp{pos.x + pad, pos.y + 2.0f};
    dl_.text(tp, style_.text, comp, f);
    dl_.rect_filled({{tp.x, tp.y + line_h - 1.0f}, {tp.x + ts.x, tp.y + line_h + 0.5f}}, style_.accent_hover);
    const f32 cx = std::round(tp.x + font_.measure(f, comp.substr(0, ime_cursor_)).x);
    dl_.rect_filled({{cx, tp.y}, {cx + 1.5f, tp.y + line_h}}, style_.text);
    dl_.pop_clip();
    switch_run(previous_owner);
}

} // namespace strata

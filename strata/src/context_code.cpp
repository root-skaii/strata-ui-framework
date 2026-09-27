// masked input, and the code editor (find bar, jumps, indenting) around the multi-line field

#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace strata {

namespace {

constexpr std::size_t npos = std::string::npos;

// input masks -----------------------------------------------------------------------------------

enum class slot_kind : u8 { literal, digit, letter, upper, lower, alnum, any };

struct mask_slot {
    slot_kind kind{};
    char      literal{};
};

[[nodiscard]] std::vector<mask_slot> parse_mask(std::string_view mask)
{
    std::vector<mask_slot> out;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        const char c = mask[i];
        if (c == '\\' && i + 1 < mask.size()) {
            out.push_back({slot_kind::literal, mask[++i]});
            continue;
        }
        switch (c) {
        case '#': out.push_back({slot_kind::digit, 0}); break;
        case 'A': out.push_back({slot_kind::letter, 0}); break;
        case 'U': out.push_back({slot_kind::upper, 0}); break;
        case 'L': out.push_back({slot_kind::lower, 0}); break;
        case 'X': out.push_back({slot_kind::alnum, 0}); break;
        case '?': out.push_back({slot_kind::any, 0}); break;
        default:  out.push_back({slot_kind::literal, c}); break;
        }
    }
    return out;
}

[[nodiscard]] bool accepts(slot_kind k, char c) noexcept
{
    const unsigned char u = static_cast<unsigned char>(c);
    switch (k) {
    case slot_kind::digit:  return u >= '0' && u <= '9';
    case slot_kind::letter:
    case slot_kind::upper:
    case slot_kind::lower:  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
    case slot_kind::alnum:  return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
    case slot_kind::any:    return u >= 32 && u < 127;
    default:                return false;
    }
}

[[nodiscard]] char convert(slot_kind k, char c) noexcept
{
    if (k == slot_kind::upper) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    if (k == slot_kind::lower) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return c;
}

// the characters of `text` that fit the mask, in order (the fixed characters of the mask are skipped, anything that does
// not fit is dropped), and how many of them lie before byte `caret`
[[nodiscard]] std::string mask_extract(const std::vector<mask_slot>& slots, std::string_view text, std::size_t caret,
                                       std::size_t& raw_before_caret)
{
    std::string raw;
    std::size_t mp = 0; // the next slot of the mask
    raw_before_caret = 0;
    for (std::size_t i = 0;; ++i) {
        if (i == caret) { raw_before_caret = raw.size(); }
        if (i >= text.size()) { break; }
        const char c = text[i];
        std::size_t q = mp;
        while (q < slots.size() && slots[q].kind == slot_kind::literal && slots[q].literal != c) { ++q; }
        if (q < slots.size() && slots[q].kind == slot_kind::literal) { // one of the fixed characters: already there
            mp = q + 1;
            continue;
        }
        if (q < slots.size() && accepts(slots[q].kind, c)) {
            raw.push_back(convert(slots[q].kind, c));
            mp = q + 1;
        }
    }
    return raw;
}

// the characters laid into the mask: fixed characters appear once something follows them, so a half-typed value
// has no dangling ") " and can be erased down to nothing. `caret` gets the position after `raw_before_caret` of them
[[nodiscard]] std::string mask_format(const std::vector<mask_slot>& slots, std::string_view raw, std::size_t raw_before_caret,
                                      std::size_t& caret)
{
    std::string out;
    std::size_t used = 0;
    caret = 0;
    for (const mask_slot& s : slots) {
        if (s.kind == slot_kind::literal) {
            if (used < raw.size()) { out.push_back(s.literal); }
            continue;
        }
        if (used >= raw.size()) { break; }
        out.push_back(raw[used++]);
        if (used == raw_before_caret) { caret = out.size(); }
    }
    if (raw_before_caret >= raw.size()) { caret = out.size(); }
    return out;
}

// find and replace -----------------------------------------------------------------------------

void find_all(std::string_view text, std::string_view needle, bool match_case, std::vector<std::pair<u32, u32>>& out)
{
    out.clear();
    if (needle.empty()) { return; }
    const auto low = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i + needle.size() <= text.size() && out.size() < 20000;) {
        bool same = true;
        for (std::size_t j = 0; j < needle.size() && same; ++j) {
            same = match_case ? text[i + j] == needle[j] : low(text[i + j]) == low(needle[j]);
        }
        if (same) {
            out.emplace_back(static_cast<u32>(i), static_cast<u32>(needle.size()));
            i += needle.size();
        } else {
            ++i;
        }
    }
}

} // namespace

// input masks -----------------------------------------------------------------------------------

void context::apply_input_mask()
{
    const std::vector<mask_slot> slots = parse_mask(m_->edit_mask_);
    std::size_t before = 0;
    const std::string raw = mask_extract(slots, m_->edit_buf_, m_->edit_cursor_, before);
    std::size_t caret = 0;
    const std::string out = mask_format(slots, raw, before, caret);
    m_->edit_buf_.assign(out.data(), out.size());
    m_->edit_cursor_ = m_->edit_anchor_ = caret;
    ++m_->edit_version_;
}

bool context::input_masked(std::string_view label, std::string& value, std::string_view mask, std::string_view hint,
                           input_flags flags)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    {   // the stored text is kept in the shape of the mask
        const std::vector<mask_slot> slots = parse_mask(mask);
        std::size_t before = 0, caret = 0;
        const std::string shaped = mask_format(slots, mask_extract(slots, value, value.size(), before), before, caret);
        if (shaped != value) { value = shaped; }
    }
    m_->edit_mask_ = mask;
    bool changed = input_core(label, value, hint, flags, mask.size() + 8);
    m_->edit_mask_ = {};
    if (changed) { // characters the mask refuses are dropped again, which can leave the text as it was
        changed = std::string_view{m_->edit_buf_} != std::string_view{value};
        if (changed) { value.assign(m_->edit_buf_.data(), m_->edit_buf_.size()); }
    }
    return changed;
}

// the code editor ------------------------------------------------------------------------------

context::code_state& context::code_state_for(id key)
{
    for (code_state& s : m_->code_states_) {
        if (s.key == key) {
            s.last_frame = m_->frame_;
            return s;
        }
    }
    if (m_->code_states_.size() >= 16) { // forget fields that are gone
        std::erase_if(m_->code_states_, [&](const code_state& s) { return s.last_frame + 600 < m_->frame_; });
    }
    m_->code_states_.emplace_back();
    m_->code_states_.back().key        = key;
    m_->code_states_.back().last_frame = m_->frame_;
    return m_->code_states_.back();
}

void context::code_goto_line(std::string_view label, int line)
{
    code_state_for(widget_id(label)).want_line = std::max(line, 1);
}

void context::code_find(std::string_view label, std::string_view text, bool with_replace)
{
    code_state& cs = code_state_for(widget_id(label));
    cs.find_open    = true;
    cs.replace_open = with_replace;
    cs.open_request = true;
    if (!text.empty()) { cs.find.assign(text); }
}

// Tab / Shift+Tab over the lines a selection touches (or the caret's line)
bool context::edit_indent_lines(bool unindent, int tab_size)
{
    const std::string_view t = m_->edit_buf_;
    const std::size_t tab = static_cast<std::size_t>(std::max(tab_size, 1));
    const std::size_t lo  = std::min(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t hi  = std::max(m_->edit_cursor_, m_->edit_anchor_);
    const std::size_t nl  = lo == 0 ? npos : t.rfind('\n', lo - 1);
    const std::size_t s   = nl == npos ? 0 : nl + 1;
    std::size_t e = hi;
    if (hi > lo && t[hi - 1] == '\n') { // the selection ends at the start of a line: that line is not part of it
        e = hi - 1;
    } else {
        const std::size_t f = t.find('\n', hi);
        e = f == npos ? t.size() : f;
    }

    std::string out;
    std::ptrdiff_t first_delta = 0;
    bool first = true;
    for (std::size_t pos = s;;) {
        std::size_t le = t.find('\n', pos);
        if (le == npos || le > e) { le = e; }
        std::string_view line = t.substr(pos, le - pos);
        std::ptrdiff_t delta = 0;
        if (!unindent) {
            if (!line.empty()) {
                out.append(tab, ' ');
                delta = static_cast<std::ptrdiff_t>(tab);
            }
        } else {
            std::size_t n = 0;
            if (!line.empty() && line[0] == '\t') { n = 1; }
            else { while (n < tab && n < line.size() && line[n] == ' ') { ++n; } }
            line.remove_prefix(n);
            delta = -static_cast<std::ptrdiff_t>(n);
        }
        out.append(line);
        if (first) { first_delta = delta; first = false; }
        if (le >= e) { break; }
        out.push_back('\n');
        pos = le + 1;
    }
    if (std::string_view{out} == t.substr(s, e - s)) {
        return false;
    }
    const bool changed = edit_replace(s, e - s, out, edit_kind::other);
    if (lo == hi) {
        const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(lo) + first_delta;
        m_->edit_cursor_ = m_->edit_anchor_ = std::clamp<std::size_t>(static_cast<std::size_t>(std::max<std::ptrdiff_t>(at, 0)), s, s + out.size());
    } else {
        m_->edit_anchor_ = s;
        m_->edit_cursor_ = s + out.size();
    }
    return changed;
}

bool context::input_code(std::string_view label, std::string& value, vec2 size, code_flags flags, std::string_view hint,
                         std::size_t max_bytes, int tab_size)
{
    if (m_->cur_ == nullptr) {
        return false;
    }
    const auto has = [&](code_flags f) { return (static_cast<u8>(flags) & static_cast<u8>(f)) != 0; };
    const id   key           = widget_id(label);
    const bool focused_here  = m_->focus_id_ == key;
    const bool find_shortcut = has(code_flags::find_replace) && focused_here;
    const std::size_t caret  = focused_here ? std::min(m_->edit_cursor_, m_->edit_anchor_) : 0;
    code_state& cs = code_state_for(key);

    bool changed = false;
    std::size_t goto_offset = npos;

    // input_spans() is for the code field: the find bar's own text fields must not use it up
    std::vector<text_span> spans = std::move(m_->edit_spans_pending_);
    m_->edit_spans_pending_.clear();

    if (has(code_flags::find_replace)) {
        push_id(label);
        const id find_key = widget_id("##find");
        bool fresh_search = false; // code_find(): start at the first match after the caret
        if (cs.open_request) {
            cs.open_request = false;
            m_->focus_request_  = find_key;
            fresh_search    = true;
            cs.active       = -1;
        }
        if (find_shortcut && chord_pressed({'F', true, false, false})) {
            cs.find_open    = true;
            cs.replace_open = false;
            m_->focus_request_  = find_key;
        }
        if (find_shortcut && chord_pressed({'H', true, false, false})) {
            cs.find_open    = true;
            cs.replace_open = true;
            m_->focus_request_  = find_key;
        }

        if (cs.find_open) {
            const f32 w = m_->layout_.width;
            const auto go = [&](int index) {
                const int n = static_cast<int>(m_->code_marks_.size());
                if (n == 0) { cs.active = -1; return; }
                cs.active   = (index % n + n) % n;
                goto_offset = m_->code_marks_[static_cast<std::size_t>(cs.active)].first;
            };

            set_next_item_width(std::max(w - 130.0f, 80.0f));
            const bool find_changed = input_text("##find", cs.find, "Find", input_flags::none, 256);
            find_all(value, cs.find, cs.match_case, m_->code_marks_);
            if (find_changed || (fresh_search && !cs.find.empty())) { // the first match at or after the caret
                cs.active = -1;
                int first = 0;
                for (std::size_t i = 0; i < m_->code_marks_.size(); ++i) {
                    if (m_->code_marks_[i].first >= caret) { first = static_cast<int>(i); break; }
                }
                go(first);
            }
            const bool enter_in_find = input_submitted() && m_->focus_id_ == find_key;
            same_line();
            if (button("<")) { go(cs.active - 1); }
            same_line();
            if (button(">") || (enter_in_find && !shift_down())) { go(cs.active + 1); }
            if (enter_in_find && shift_down()) { go(cs.active - 1); }
            same_line();
            if (button("x")) { cs.find_open = cs.replace_open = false; }

            const int n = static_cast<int>(m_->code_marks_.size());
            if (cs.active >= n) { cs.active = n - 1; }
            if (cs.find.empty())    { text_dim("type to search"); }
            else if (n == 0)        { text_dim("no matches"); }
            else                    { text_dim(std::format("{} of {}", cs.active + 1, n)); }
            same_line();
            if (checkbox("match case", cs.match_case)) { find_all(value, cs.find, cs.match_case, m_->code_marks_); cs.active = -1; }
            same_line();
            (void)checkbox("replace", cs.replace_open);

            if (cs.replace_open) {
                set_next_item_width(std::max(w - 150.0f, 80.0f));
                (void)input_text("##replace", cs.replace, "Replace with", input_flags::none, 1024);
                same_line();
                if (button("Replace") && !m_->code_marks_.empty()) {
                    const auto m = m_->code_marks_[static_cast<std::size_t>(std::max(cs.active, 0))];
                    value.replace(m.first, m.second, cs.replace);
                    changed = true;
                    find_all(value, cs.find, cs.match_case, m_->code_marks_);
                    if (!m_->code_marks_.empty()) { go(std::min<int>(std::max(cs.active, 0), static_cast<int>(m_->code_marks_.size()) - 1)); }
                }
                same_line();
                if (button("All") && !m_->code_marks_.empty()) {
                    for (std::size_t i = m_->code_marks_.size(); i-- > 0;) { value.replace(m_->code_marks_[i].first, m_->code_marks_[i].second, cs.replace); }
                    changed = true;
                    find_all(value, cs.find, cs.match_case, m_->code_marks_);
                    cs.active = -1;
                }
            }
        } else {
            m_->code_marks_.clear();
        }
        pop_id();
    }

    if (cs.want_line > 0) { // code_goto_line
        std::size_t off = 0;
        for (int l = 1; l < cs.want_line && off != npos; ++l) {
            off = value.find('\n', off);
            if (off != npos) { ++off; }
        }
        goto_offset = off == npos ? value.size() : off;
        if (focused_here) { m_->edit_cursor_ = m_->edit_anchor_ = std::min(goto_offset, m_->edit_buf_.size()); }
        cs.want_line = 0;
    }

    m_->edit_spans_pending_ = std::move(spans);

    m_->code_ = {};
    m_->code_.on          = true;
    m_->code_.flags       = flags;
    m_->code_.tab_size    = std::max(tab_size, 1);
    m_->code_.goto_offset = goto_offset;
    if (cs.find_open && has(code_flags::find_replace)) {
        m_->code_.marks       = m_->code_marks_;
        m_->code_.mark_active = cs.active;
    }
    vec2 field = size;
    if (field.y <= 0.0f) { field.y = m_->font_.line_height(current_font()) * 12.0f + m_->style_.frame_padding.y * 2.0f; }
    if (input_multiline_core(label, value, field, input_flags::no_wrap, hint, max_bytes)) {
        value.assign(m_->edit_buf_.data(), m_->edit_buf_.size());
        changed = true;
    }
    return changed;
}

} // namespace strata

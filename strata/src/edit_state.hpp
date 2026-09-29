#pragma once

// internal: the focused text field's live state -- undo/redo history, the edit buffer, cursor/selection,
// multi-line layout cache, styled spans (input_spans), and the host-facing IME request -- behind
// context::impl::edit_. context::edit_kind/edit_op/edit_history/ml_line (declared in context.hpp) alias the
// types below, so context.cpp's, context_text.cpp's, and context_code.cpp's unqualified uses need no change.
// context_text.cpp owns the logic; context_number.cpp and context_code.cpp use the same field editor core.
//
// focus_id_/focus_seen_ and press_claimed_ stay on context::impl directly, NOT here: they gate focus/press
// handling for every widget kind (nav, menus, modals, rich-text links), not just text fields.

#include "strata/context.hpp"

#include <vector>

namespace strata::internal {

enum class edit_kind : u8 { other, typing, erase_back, erase_fwd };

struct edit_op {
    std::size_t pos{};      // where the change happened
    std::size_t off{};      // in history text: removed bytes, then inserted
    std::size_t rem_len{};
    std::size_t ins_len{};
    std::size_t cursor_before{};
    std::size_t anchor_before{};
    std::size_t cursor_after{};
    edit_kind   kind{};
    f64         time{};
};

struct edit_history {
    std::vector<edit_op> ops;
    secure_string        text; // typed text lives here so it is zeroed like the edit buffer
};

struct ml_line {
    u32 start{}; // byte range of the line, without its newline
    u32 end{};
};

struct edit_state {
    edit_history  undo_;
    edit_history  redo_;
    bool          edit_history_on_{};
    bool          edit_readonly_{};
    std::size_t   edit_max_bytes_{};
    u64           edit_version_{};     // bumped on every change of the edit buffer
    f32           edit_pref_x_{-1.0f}; // multi-line: column kept while moving up / down
    std::vector<ml_line> ml_lines_;
    u64           ml_cache_key_{};
    bool          submitted_{};
    secure_string edit_buf_; // live text of the focused field; zeroed on blur
    std::size_t edit_cursor_{};
    std::size_t edit_anchor_{};
    f32         edit_scroll_{};
    f64         caret_time_{};
    f64         last_click_time_{-10.0};
    vec2        last_click_pos_{};
    u32         click_count_{};

    // styled contents (input_spans) for the field being built
    std::vector<text_span> edit_spans_pending_;
    std::vector<text_span> edit_spans_;
    u64         edit_spans_hash_{};

    // IME: what the focused field reports to the host (composition itself is m_->input_.ime_*)
    bool        ime_want_{};
    vec2        ime_pos_{};
    f32         ime_line_h_{};
};

} // namespace strata::internal

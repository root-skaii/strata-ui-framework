#pragma once

// internal: the code editor's find/replace UI (code_mode) and per-field state across frames (code_state).
// context::code_mode/code_state (declared in context.hpp) alias the types below, so context_code.cpp's and
// context_text.cpp's unqualified uses need no change. context_code.cpp owns the logic; m_->code_/code_states_/
// code_marks_ stay separate members of context::impl (not consolidated into one), since that's all this is.

#include "strata/context.hpp"

#include <span>
#include <string>
#include <vector>

namespace strata::internal {

struct code_mode {
    bool                                          on{};
    code_flags                                    flags{};
    int                                           tab_size{4};
    std::span<const std::pair<u32, u32>>          marks{}; // find matches (start, length), sorted
    int                                           mark_active{-1};
    std::size_t                                   goto_offset{~std::size_t{0}};
};

struct code_state { // per code field, across frames
    id          key{};
    u64         last_frame{};
    bool        find_open{};
    bool        replace_open{};
    bool        match_case{};
    bool        open_request{}; // code_find(): give the find field the keyboard
    int         active{-1};
    int         want_line{};
    std::string find;
    std::string replace;
};

} // namespace strata::internal

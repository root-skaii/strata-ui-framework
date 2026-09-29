#pragma once

// internal: ask_confirm()/confirm()'s modal state, behind context::impl::confirm_. context_modal.cpp owns the
// logic -- the only file that touches it (context_access.cpp just reads a couple of fields for accessors).

#include "strata/context.hpp"

#include <string>

namespace strata::internal {

struct confirm_state {
    id          confirm_key_{};
    u64         confirm_data_{};
    std::string confirm_message_;
    int         confirm_answer_{};   // set when a button is pressed, read once by confirm()
    id          confirm_answer_key_{};
    id          confirm_open_{};     // the confirm() whose modal is up
    bool        confirm_pending_{};  // ask_confirm() ran; the next confirm() with this id opens
    bool        confirm_remember_{}; // "don't ask again" checkbox state while open
};

} // namespace strata::internal

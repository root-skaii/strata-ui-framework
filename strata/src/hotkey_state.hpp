#pragma once

// internal: hotkey capture (hotkey_field/hotkey_sequence, context_extra.cpp) and chord/sequence matching
// (chord_pressed/sequence_pressed, context_menu.cpp) state, behind context::impl::hotkey_. a plain struct with
// public fields, like dock_state.hpp -- the two files above are its main owners, but context.cpp's begin_frame
// resets it and a few other files (context_access.cpp, context_modal.cpp, context_nav.cpp) read hotkey_capture_
// as a simple "something else has the keyboard" check.

#include "strata/context.hpp"

#include <array>

namespace strata::internal {

struct hotkey_state {
    id   hotkey_capture_{};
    bool hotkey_seen_{};
    // hotkey_sequence(): steps captured so far by the capturing field (hotkey_capture_ keeps it to one)
    std::array<key_chord, key_sequence::max_steps> seq_edit_capture_{};
    u8   seq_edit_count_{};
    f64  seq_edit_deadline_{};
    // sequence_pressed(): prefix matched so far across the sequences asked about each frame. mutable so it can
    // stay const like chord_pressed
    mutable std::array<key_chord, key_sequence::max_steps - 1> seq_pending_{};
    mutable u8   seq_pending_count_{};
    mutable f64  seq_pending_time_{};
    mutable bool seq_pending_touched_{}; // a call advanced / completed the prefix this frame (see begin_frame)
};

} // namespace strata::internal

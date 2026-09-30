#pragma once

// internal: this frame's raw input -- mouse, wheel, modifiers, keyboard events/typed text, the dequeued hotkey
// press (+ its queue), and IME composition. begin_frame() (context.cpp) fills this from the host's input_state
// each frame; everything else just reads it (m_->input_.mouse_, m_->input_.keys_, ...). a plain struct with
// public fields, like dock_state.hpp -- there is no single owning .cpp for input the way context_popup.cpp
// owns popups, so encapsulating behind methods would only add indirection at ~19 call sites for no real
// boundary. ime_want_/ime_pos_/ime_line_h_ are NOT here: those are the focused field's request to the host,
// set by context_text.cpp, not part of what the host handed in.

#include "strata/context.hpp"

#include <array>

namespace strata::internal {

struct input_frame {
    vec2 mouse_{};
    vec2 mouse_delta_{};
    bool have_mouse_{};
    bool mouse_down_{};
    bool mouse_pressed_{};
    bool mouse_released_{};
    bool mouse_right_down_{};
    bool mouse_right_pressed_{};
    bool mouse_right_released_{};
    bool mouse_middle_down_{};
    bool mouse_middle_pressed_{};
    bool mouse_middle_released_{};
    bool mod_ctrl_{};
    bool mod_shift_{};
    bool mod_alt_{};

    f32  wheel_{};           // this frame's smoothed vertical notches
    f32  wheel_x_{};         // ... horizontal (see input_state::wheel_x)
    f32  wheel_pending_{};   // smoothing backlog still to let out, persists across frames
    f32  wheel_x_pending_{};
    bool wheel_moving_{};    // backlog not yet empty (animations_settling)
    f32  wheel_lines_{3.0f}; // input_state::wheel_lines

    std::array<key_event, max_key_events> keys_{};
    u32                                   key_count_{};
    std::array<char, max_typed_bytes>     typed_{};
    u32                                   typed_len_{};
    std::array<u8, 32>                    keys_held_{}; // bitset: key_down() / hotkeys

    // the queued press this frame is handling now, and modifiers; more presses wait in the queue
    key                        pressed_key_{};
    bool                       press_ctrl_{};
    bool                       press_shift_{};
    bool                       press_alt_{};
    std::array<key_event, 64> press_queue_{};
    u32                       press_queued_{};

    // IME composition, as reported by the host
    std::array<char, 256> ime_text_{};
    u32                    ime_len_{};
    u32                    ime_cursor_{};
};

} // namespace strata::internal

namespace strata { using internal::input_frame; }

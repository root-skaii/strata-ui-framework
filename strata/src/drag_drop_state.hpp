#pragma once

// internal: drag and drop (drag_source/drop_target), behind context::impl::dnd_. context.cpp owns the logic
// built on this; context_popup.cpp draws the dragged preview as overlay content (like a tooltip).

#include "strata/context.hpp"

#include "core/layout_state.hpp"

#include <string>
#include <vector>

namespace strata::internal {

struct drag_drop_state {
    bool            dd_active_{};     // a payload is being dragged
    bool            dd_cancelled_{};  // Esc: ignored until the button is let go
    id              dd_source_{};     // the widget it was picked up from
    id              dd_candidate_{};  // pressed widget that becomes a drag source once moved
    vec2            dd_press_pos_{};
    std::string     dd_type_;
    std::vector<u8> dd_data_;
    vec2            dd_size_{};       // the preview panel, measured last frame
    bool            dd_hidden_{};
    bool            dd_saved_overlay_{};
    u32             dd_prev_owner_{};
    layout_state    dd_saved_layout_{};
};

} // namespace strata::internal

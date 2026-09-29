#pragma once

// internal: the layout cursor snapshot -- context::impl::layout_ is the one being built; menu_frame/child_frame/
// card_frame/table_frame/popup_frame each save and restore one of these around nested content. context::
// layout_state (declared in context.hpp) aliases internal::layout_state below, so the structs and call sites
// that write "layout_state saved{};" / "layout_state& l = m_->layout_;" unqualified need no change.

#include "strata/types.hpp"

namespace strata::internal {

struct layout_state {
    vec2 origin{};
    f32  width{};
    f32  cursor_x{};
    f32  line_top{};
    f32  line_h{};
    f32  line_h_seed{};  // expected height for the line about to start (e.g. a table row's known height),
                          // so its first item centers against siblings that haven't been placed yet this frame
    f32  bottom{};
    f32  next_width{};
    f32  bound_bottom{}; // bottom limit for fill-height children / strips (0 = none)
    f32  right{};        // the furthest right edge of anything placed
    f32  gutter{};       // taken off `width` by push_right_gutter (same_line_right adds it back)
    bool same_line{};
    bool first{true};
};

} // namespace strata::internal

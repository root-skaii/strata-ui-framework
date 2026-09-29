#pragma once

// internal: toast notifications, behind context::impl::toast_. context::toast_entry (declared in context.hpp)
// aliases internal::toast_entry below, so context.cpp's "for (const toast_entry& t : m_->toast_.toasts_)" needs
// no change. context_toast.cpp owns the logic built on this.

#include "strata/context.hpp"

#include <string>
#include <vector>

namespace strata::internal {

struct toast_entry {
    std::string title;
    std::string text;
    toast_kind  kind{};
    f32         duration{};
    f32         age{};
    f32         anim{};   // slide / fade in, 0..1
    bool        dismissed{};
    u64         seq{};
    std::vector<std::string> actions;
    f32         progress{-1.0f};
    bool        sticky{};       // no timer until it completes / is closed
    f32         busy_phase{};
    bool        paused{};       // the pointer is on it: its timer stands still
};

struct toast_state {
    std::vector<toast_entry> toasts_;
    screen_corner toast_corner_{screen_corner::bottom_right};
    bool          toast_hover_cur_{};
    bool          toast_hover_prev_{};
    u64           toast_seq_{};
};

} // namespace strata::internal

#pragma once

// internal: the modal stack (ask_confirm-style blocking windows), behind context::impl::modal_. context_modal.cpp
// owns the logic built on this (m_->modal_.*).

#include "strata/context.hpp"

#include <array>

namespace strata::internal {

struct modal_frame {
    bool alpha_pushed{};
};

struct modal_state {
    static constexpr u32 max_modals = 4; // (context::max_modals is the same)

    std::array<id, max_modals> modal_stack_{};
    u32 modal_count_{};
    u32 next_window_modal_level_{};
    std::array<modal_frame, max_modals> modal_frames_{};
    u32 modal_depth_{};       // modals being built right now
    id  modal_top_prev_{};    // modal with the input (as of last frame)
};

} // namespace strata::internal

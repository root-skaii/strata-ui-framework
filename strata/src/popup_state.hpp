#pragma once

// internal: the popup stack (open_popup, combo lists, pickers), behind context::impl::popup_. one opened while
// another is being drawn goes on top of it as its child; one opened from outside every popup replaces the whole
// stack. levels 1.. draw in layers of their own above the overlay (popup_run), so a child covers the rest of
// its parent's content. context_popup.cpp owns the logic built on this (m_->popup_.*); a few call sites
// elsewhere (context.cpp, context_access.cpp, context_data.cpp, context_modal.cpp, context_combo.cpp,
// context_datetime.cpp) read it too, since a popup can be a combo list, a date picker, or a plain popup alike.

#include "strata/context.hpp"

#include "core/layout_state.hpp"

#include <algorithm>
#include <array>

namespace strata::internal {

struct popup_level {
    id   key{};
    bool open_cur{};     // drawn this frame
    bool open_prev{};
    rect rect_cur{};
    rect rect_prev{};
    rect anchor_cur{};   // the widget that owns it: a press there is left to that widget
    rect anchor_prev{};
    rect opener{};       // open_popup: the item it opens under
    vec2 size{};         // open_popup: last frame's content size ...
    id   measured{};     // ... and the popup it was measured for
};

// a popup being drawn: what its end restores
struct popup_frame {
    u32          level{};
    u32          prev_owner{0xffffffffu}; // (context::run_base is the same)
    bool         saved_overlay{};
    bool         outer_hidden{};  // begin_popup: gpopup_hidden_ of the popup around this one
    layout_state saved_layout{};
};

class popup_stack {
public:
    static constexpr u32 max_popup_levels = 4;
    static constexpr u32 run_popup        = 0xfffffff8u; // + level (1..): the draw layer of a nested popup
    static constexpr u32 no_popup         = 0xffffffffu;
    static constexpr u32 run_overlay      = 0xfffffffeu; // (context::run_overlay is the same)
    static constexpr u32 run_base         = 0xffffffffu; // (context::run_base is the same)

    std::array<popup_level, max_popup_levels> popups_{};
    u32  popup_count_{};       // open levels
    std::array<popup_frame, max_popup_levels> popup_frames_{};
    u32  popup_depth_{};       // popups being drawn right now
    bool popup_esc_used_{};    // Esc closed a level this frame: one level per press
    f32  popup_scroll_{};      // the open combo list (lists hold no widgets, so there is only ever one)
    int  popup_hover_{-1};

    [[nodiscard]] u32 popup_level_of(id key) const noexcept
    {
        for (u32 i = 0; key != 0 && i < popup_count_; ++i) {
            if (popups_[i].key == key) { return i; }
        }
        return no_popup;
    }
    [[nodiscard]] bool popup_has(id key) const noexcept { return popup_level_of(key) != no_popup; }
    [[nodiscard]] id   popup_top() const noexcept { return popup_count_ > 0 ? popups_[popup_count_ - 1].key : id{}; }
    // the innermost popup being drawn (popup_depth_ > 0). a popup drawn inside itself (an id collision) would nest
    // deeper than the levels: the slot is clamped so that stays in bounds
    [[nodiscard]] popup_frame&       popup_frame_top() noexcept { return popup_frames_[std::min(popup_depth_, max_popup_levels) - 1]; }
    [[nodiscard]] const popup_frame& popup_frame_top() const noexcept { return popup_frames_[std::min(popup_depth_, max_popup_levels) - 1]; }
    // the level of the popup being drawn, no_popup outside every popup
    [[nodiscard]] u32  popup_drawing() const noexcept { return popup_depth_ > 0 ? popup_frame_top().level : no_popup; }
    [[nodiscard]] id   popup_drawing_key() const noexcept
    {
        const u32 l = popup_drawing();
        return l < popup_count_ ? popups_[l].key : id{};
    }
    // closes `level` and everything above it
    void popup_close_from(u32 level) noexcept
    {
        for (u32 i = level; i < popup_count_; ++i) { popups_[i] = {}; }
        popup_count_ = std::min(popup_count_, level);
    }
    void popup_close(id key) noexcept
    {
        const u32 l = popup_level_of(key);
        if (l != no_popup) { popup_close_from(l); }
    }
    [[nodiscard]] static u32 popup_run(u32 level) noexcept
    {
        return level == 0 ? run_overlay : run_popup + level;
    }
    // where overlay drawing (tooltips, menus, drag previews) goes: the layer of the popup being drawn, if any
    [[nodiscard]] u32 overlay_run() const noexcept
    {
        const u32 l = popup_drawing();
        return l == no_popup ? run_overlay : popup_run(l);
    }
    // any level was drawn last frame
    [[nodiscard]] bool popup_any_prev() const noexcept
    {
        for (u32 i = 0; i < popup_count_; ++i) {
            if (popups_[i].open_prev) { return true; }
        }
        return false;
    }
    // `p` is over an open popup (last frame's rects) above what is being built: above the popup being drawn, or any
    // level from outside the popups
    [[nodiscard]] bool popup_covers(vec2 p) const noexcept
    {
        const u32 l = popup_drawing();
        for (u32 i = l == no_popup ? 0u : l + 1; i < popup_count_; ++i) {
            if (popups_[i].open_prev && popups_[i].rect_prev.contains(p)) { return true; }
        }
        return false;
    }
};

} // namespace strata::internal

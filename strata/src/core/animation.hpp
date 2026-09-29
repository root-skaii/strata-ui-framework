#pragma once

// internal: per-id animation state, the slot table behind anim_for/anim_find/button_anim. included via
// context_impl.hpp; context::anim_slot (declared in context.hpp) aliases internal::anim_slot below, so the
// widget files that write "anim_slot& a = anim_for(key);" need no change. context::anim_for/anim_find/
// button_anim/approach/animate in context.cpp stay put and forward into this for the slot lookup; approach()
// itself is unrelated (it depends on m_->style_.anim_speed / m_->dt_, not the slot table).

#include "strata/types.hpp"

#include <vector>

namespace strata::internal {

struct anim_slot {
    id   key{};
    u64  last_frame{};
    f32  hover{};
    f32  active{};
    f32  toggle{};
    f32  custom{};
    bool custom_init{};
};

// open-addressed by id (linear probing), power-of-two size, grows on demand
class animation {
public:
    animation() : slots_(1024) {}

    // finds an existing slot; nullptr if none. does not create or touch last_frame.
    [[nodiscard]] anim_slot* find(id key) noexcept;
    // finds or creates a slot for `key`, stamping last_frame = frame (a stale slot is reused if the table is full)
    [[nodiscard]] anim_slot& for_key(id key, u64 frame) noexcept;

    // stale slots keep the table as big as its peak (e.g. a fully expanded tree), costing a cache miss per lookup:
    // call periodically (every ~256 frames is what end_frame does) to shrink back down once most slots are idle
    void maybe_compact(u64 frame) noexcept;

    [[nodiscard]] u32 used() const noexcept { return used_; }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(slots_.size()); }

private:
    void rehash(u64 frame) noexcept;

    std::vector<anim_slot> slots_;
    u32                    used_{};
};

} // namespace strata::internal

namespace strata { using internal::animation; }

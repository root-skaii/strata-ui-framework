#include "animation.hpp"

namespace strata::internal {

anim_slot* animation::find(id key) noexcept
{
    const u32 mask = static_cast<u32>(slots_.size()) - 1;
    for (u32 i = 0, at = key & mask; i <= mask; ++i, at = (at + 1) & mask) {
        anim_slot& s = slots_[at];
        if (s.key == key) { return &s; }
        if (s.key == 0)   { return nullptr; }
    }
    return nullptr;
}

anim_slot& animation::for_key(id key, u64 frame) noexcept
{
    if (static_cast<std::size_t>(used_) * 4 >= slots_.size() * 3) {
        rehash(frame);
    }

    const u32  mask  = static_cast<u32>(slots_.size()) - 1;
    anim_slot* stale = nullptr;
    u32        at    = key & mask;
    for (;;) {
        anim_slot& s = slots_[at];
        if (s.key == key) {
            s.last_frame = frame;
            return s;
        }
        if (s.key == 0) {
            anim_slot& target = stale != nullptr ? *stale : s;
            if (stale == nullptr) { ++used_; }
            target            = {};
            target.key        = key;
            target.last_frame = frame;
            return target;
        }
        if (stale == nullptr && s.last_frame + 2 < frame) { stale = &s; }
        at = (at + 1) & mask;
    }
}

void animation::rehash(u64 frame) noexcept
{
    std::size_t live = 0;
    for (const anim_slot& s : slots_) {
        live += s.key != 0 && s.last_frame + 2 >= frame;
    }

    std::size_t size = 1024;
    while (size < live * 4 + 1) { size *= 2; }

    std::vector<anim_slot> old = std::move(slots_);
    slots_.assign(size, anim_slot{});
    used_ = 0;

    const u32 mask = static_cast<u32>(size) - 1;
    for (const anim_slot& s : old) {
        if (s.key == 0 || s.last_frame + 2 < frame) { continue; }
        u32 at = s.key & mask;
        while (slots_[at].key != 0) { at = (at + 1) & mask; }
        slots_[at] = s;
        ++used_;
    }
}

void animation::maybe_compact(u64 frame) noexcept
{
    if (slots_.size() <= 1024 || (frame & 0xff) != 0) { return; }
    std::size_t live = 0;
    for (const anim_slot& s : slots_) {
        live += s.key != 0 && s.last_frame + 2 >= frame;
    }
    if (live * 16 < slots_.size()) {
        rehash(frame);
    }
}

} // namespace strata::internal

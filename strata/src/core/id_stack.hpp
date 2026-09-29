#pragma once

// internal: the id stack (push_id / pop_id / widget_id) and its debug-only duplicate-id bookkeeping.
// included via context_impl.hpp; context::push_id/pop_id/widget_id/check_id/current_seed in context.cpp
// and context_access.cpp are thin forwards to this.

#include "strata/types.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace strata::internal {

class id_stack {
public:
    static constexpr u32 max_depth = 16; // (context::max_id_depth is the same)

    // hashes `s` / `value` / `p` against current() and pushes it; false (nothing pushed) past max_depth
    bool push(std::string_view s) noexcept;
    bool push(u64 value) noexcept;
    bool push(const void* p) noexcept { return push(static_cast<u64>(reinterpret_cast<std::uintptr_t>(p))); }
    // pushes an already-hashed id verbatim (push_id_value: no re-hash)
    bool push_raw(id key) noexcept;
    void pop() noexcept { if (depth_ > 0) { --depth_; } }

    [[nodiscard]] id   current() const noexcept { return stack_[depth_]; }
    [[nodiscard]] id   root() const noexcept { return stack_[0]; } // window ids hash against this
    [[nodiscard]] u32  depth() const noexcept { return depth_; }
    [[nodiscard]] bool contains(id seed) const noexcept;

    void begin_frame() noexcept; // depth() -> 0, root() -> 0; clears this frame's duplicate table and collision report

    // widget id from a label in the current scope; debug builds also record the label for collision reports
    [[nodiscard]] id widget_id(std::string_view label) noexcept;

    struct duplicate_result {
        bool             is_duplicate{};
        std::string_view label; // known label of `key`, if any (debug builds only)
    };
    // debug only: records `key` for this frame, counting repeats. the frame's first duplicate is remembered
    // for collision() / collision_label()
    [[nodiscard]] duplicate_result check_duplicate(id key) noexcept;

    [[nodiscard]] id               collision() const noexcept { return collision_id_; }
    [[nodiscard]] std::string_view collision_label() const noexcept { return {collision_label_.data(), collision_label_len_}; }

private:
    std::array<id, max_depth + 1> stack_{};
    u32                           depth_{};

    // label_table/seen_table: direct-mapped (power of two), grown lazily so release builds never pay for them
    struct label_slot { id key{}; std::array<char, 48> text{}; u8 len{}; };
    static constexpr u32 label_table_size = 1024;
    static constexpr u32 seen_table_size  = 2048;
    std::vector<label_slot> labels_; // sticky: id -> label, for collision messages
    std::vector<id>         seen_;   // per-frame: ids already submitted

    id                    collision_id_{};
    std::array<char, 64>  collision_label_{};
    u32                   collision_label_len_{};
};

} // namespace strata::internal

namespace strata { using internal::id_stack; }

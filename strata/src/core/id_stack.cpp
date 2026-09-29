#include "id_stack.hpp"

#include <algorithm>

namespace strata::internal {

bool id_stack::push(std::string_view s) noexcept
{
    if (depth_ >= max_depth) { return false; }
    const id next = hash_id(s, current());
    stack_[++depth_] = next;
    return true;
}

bool id_stack::push(u64 value) noexcept
{
    if (depth_ >= max_depth) { return false; }
    std::array<char, sizeof(u64)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
    const id next = hash_id({bytes.data(), bytes.size()}, current());
    stack_[++depth_] = next;
    return true;
}

bool id_stack::push_raw(id key) noexcept
{
    if (depth_ >= max_depth) { return false; }
    stack_[++depth_] = key;
    return true;
}

bool id_stack::contains(id seed) const noexcept
{
    for (u32 i = 0; i <= depth_; ++i) {
        if (stack_[i] == seed) { return true; }
    }
    return false;
}

void id_stack::begin_frame() noexcept
{
    depth_    = 0;
    stack_[0] = 0;
    collision_id_        = 0;
    collision_label_len_ = 0;
#ifndef NDEBUG
    // the ids submitted last frame are not the ones submitted this frame
    if (!seen_.empty()) {
        std::ranges::fill(seen_, id{});
    }
#endif
}

id id_stack::widget_id([[maybe_unused]] std::string_view label) noexcept
{
    const id key = hash_id(label, current());
#ifndef NDEBUG
    // record the label behind this id for collision reports. keep the raw string: "a##b" and "a##c" are distinct
    // ids that would both show as "a".
    if (labels_.size() != label_table_size) {
        labels_.assign(label_table_size, label_slot{});
    }
    label_slot& slot = labels_[key & (label_table_size - 1)];
    if (slot.key != key) {
        slot.key = key;
        const std::size_t n = std::min(label.size(), slot.text.size());
        std::copy_n(label.data(), n, slot.text.data());
        slot.len = static_cast<u8>(n);
    }
#endif
    return key;
}

id_stack::duplicate_result id_stack::check_duplicate([[maybe_unused]] id key) noexcept
{
#ifndef NDEBUG
    if (key == 0) { return {}; }
    if (seen_.size() != seen_table_size) {
        seen_.assign(seen_table_size, id{});
    }
    id& slot = seen_[key & (seen_table_size - 1)];
    if (slot == key) {
        std::string_view label;
        if (labels_.size() == label_table_size) {
            const label_slot& ls = labels_[key & (label_table_size - 1)];
            if (ls.key == key) { label = {ls.text.data(), ls.len}; }
        }
        if (collision_id_ == 0) { // the first one of the frame is the one worth naming
            collision_id_ = key;
            collision_label_len_ = static_cast<u32>(std::min(label.size(), collision_label_.size()));
            std::copy_n(label.data(), collision_label_len_, collision_label_.data());
        }
        return {true, label};
    }
    slot = key;
#endif
    return {};
}

} // namespace strata::internal

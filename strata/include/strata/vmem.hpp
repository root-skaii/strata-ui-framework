#pragma once

#include "strata/types.hpp"

#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace strata {

namespace detail {
inline constexpr std::size_t vmem_chunk = 64 * 1024;

[[nodiscard]] void* vmem_reserve(std::size_t bytes) noexcept;
[[nodiscard]] bool  vmem_commit(void* address, std::size_t bytes) noexcept;
void                vmem_release(void* base) noexcept;
// overwrites memory with zeros in a way the optimiser may not drop
void                secure_wipe(void* p, std::size_t bytes) noexcept;
} // namespace detail

// allocator that zeroes memory before handing it back, so text that lived in a std::basic_string never
// lingers in freed heap (including the copies left behind when a string grows).
template <class T>
struct wiping_allocator {
    using value_type = T;

    wiping_allocator() noexcept = default;
    template <class U>
    constexpr wiping_allocator(const wiping_allocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept
    {
        detail::secure_wipe(p, n * sizeof(T));
        std::allocator<T>{}.deallocate(p, n);
    }

    template <class U>
    [[nodiscard]] friend constexpr bool operator==(const wiping_allocator&, const wiping_allocator<U>&) noexcept { return true; }
};

using secure_string = std::basic_string<char, std::char_traits<char>, wiping_allocator<char>>;

// a contiguous array backed by reserved address space. memory is committed in
// 64 KiB chunks as the array grows, so the base pointer never moves, growth
// never copies and steady state (clear() + refill) never touches the heap.
template <class T>
    requires std::is_trivially_copyable_v<T>
class vmem_array {
public:
    vmem_array() noexcept = default;

    explicit vmem_array(std::size_t max_count) noexcept
        : reserved_{(max_count * sizeof(T) + detail::vmem_chunk - 1) & ~(detail::vmem_chunk - 1)}
    {
        base_ = static_cast<T*>(detail::vmem_reserve(reserved_));
    }

    ~vmem_array() { release(); }

    vmem_array(const vmem_array&)            = delete;
    vmem_array& operator=(const vmem_array&) = delete;

    vmem_array(vmem_array&& o) noexcept
        : base_{std::exchange(o.base_, nullptr)}
        , size_{std::exchange(o.size_, 0)}
        , committed_{std::exchange(o.committed_, 0)}
        , reserved_{std::exchange(o.reserved_, 0)}
    {}

    vmem_array& operator=(vmem_array&& o) noexcept
    {
        if (this != &o) {
            release();
            base_      = std::exchange(o.base_, nullptr);
            size_      = std::exchange(o.size_, 0);
            committed_ = std::exchange(o.committed_, 0);
            reserved_  = std::exchange(o.reserved_, 0);
        }
        return *this;
    }

    // appends n uninitialised elements; nullptr when the reservation is exhausted
    [[nodiscard]] T* grow(std::size_t n) noexcept
    {
        const std::size_t need = (size_ + n) * sizeof(T);
        if (need > committed_) {
            if (base_ == nullptr || need > reserved_) {
                return nullptr;
            }
            const std::size_t target =
                std::min((need + detail::vmem_chunk - 1) & ~(detail::vmem_chunk - 1), reserved_);
            auto* commit_at = reinterpret_cast<std::byte*>(base_) + committed_;
            if (!detail::vmem_commit(commit_at, target - committed_)) {
                return nullptr;
            }
            committed_ = target;
        }
        T* out = base_ + size_;
        size_ += n;
        return out;
    }

    void shrink(std::size_t n) noexcept { size_ -= std::min(n, size_); }
    void clear() noexcept { size_ = 0; }

    [[nodiscard]] T*          data() noexcept { return base_; }
    [[nodiscard]] const T*    data() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }
    [[nodiscard]] T&          back() noexcept { return base_[size_ - 1]; }
    [[nodiscard]] const T&    back() const noexcept { return base_[size_ - 1]; }

    [[nodiscard]] std::span<const T> view() const noexcept { return {base_, size_}; }

private:
    // the committed pages are zeroed before they go back to the os
    void release() noexcept
    {
        if (base_ != nullptr) {
            detail::secure_wipe(base_, committed_);
            detail::vmem_release(base_);
        }
    }

    T*          base_{};
    std::size_t size_{};
    std::size_t committed_{};
    std::size_t reserved_{};
};

} // namespace strata

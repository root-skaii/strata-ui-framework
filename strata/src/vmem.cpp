#include "strata/vmem.hpp"

#include <windows.h>

namespace strata::detail {

void* vmem_reserve(std::size_t bytes) noexcept
{
    return ::VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
}

bool vmem_commit(void* address, std::size_t bytes) noexcept
{
    return ::VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void secure_wipe(void* p, std::size_t bytes) noexcept
{
    if (p != nullptr && bytes != 0) {
        ::SecureZeroMemory(p, bytes);
    }
}

void vmem_release(void* base) noexcept
{
    if (base != nullptr) {
        ::VirtualFree(base, 0, MEM_RELEASE);
    }
}

} // namespace strata::detail

#include "strata/vmem.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace strata::detail {

void* vmem_reserve(std::size_t bytes) noexcept
{
#ifdef _WIN32
    return ::VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
#else
    if (bytes == 0) { return nullptr; }
    void* p = ::mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

bool vmem_commit(void* address, std::size_t bytes) noexcept
{
#ifdef _WIN32
    return ::VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return address != nullptr && ::mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

void secure_wipe(void* p, std::size_t bytes) noexcept
{
    if (p == nullptr || bytes == 0) { return; }
#ifdef _WIN32
    ::SecureZeroMemory(p, bytes);
#else
    volatile u8* dst = static_cast<volatile u8*>(p);
    for (std::size_t i = 0; i < bytes; ++i) { dst[i] = 0; }
#endif
}

void vmem_release(void* base, std::size_t bytes) noexcept
{
    if (base == nullptr) { return; }
#ifdef _WIN32
    (void)bytes;
    ::VirtualFree(base, 0, MEM_RELEASE);
#else
    ::munmap(base, bytes);
#endif
}

} // namespace strata::detail

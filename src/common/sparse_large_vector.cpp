// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.cpp */
// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <windows.h>
#include <mutex>
#else
#include <sys/mman.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"
#include "common/sparse_large_vector.h"

#if defined(_WIN32) && defined(YUZU_UWP_APPCONTAINER)
#include "common/dynamic_library.h"
#endif

namespace Common {

#ifdef _WIN32
#ifdef YUZU_UWP_APPCONTAINER
// Xbox/UWP AppContainer: plain VirtualAlloc/VirtualProtect are not available to the app, and the
// errorhandling api-set that exports AddVectoredExceptionHandler is missing from the Xbox loader, so
// importing it makes the package fail to activate. Use the *FromApp variants and resolve the handler
// API by name (same approach as host_memory.cpp).
namespace {
using PFN_AddVectoredExceptionHandler = PVOID(WINAPI*)(ULONG First,
                                                       PVECTORED_EXCEPTION_HANDLER Handler);
PVOID AddExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
    static DynamicLibrary kernelbase("Kernelbase");
    PFN_AddVectoredExceptionHandler add_veh{};
    if (!kernelbase.IsOpen() || !kernelbase.GetSymbol("AddVectoredExceptionHandler", &add_veh)) {
        return nullptr;
    }
    return add_veh(first, handler);
}
} // namespace
#define LARGE_VECTOR_VIRTUAL_ALLOC(address, size, type, protection)                                \
    VirtualAllocFromApp(address, size, type, protection)
#define LARGE_VECTOR_VIRTUAL_PROTECT(address, size, protection, old)                               \
    VirtualProtectFromApp(address, size, protection, old)
#define LARGE_VECTOR_ADD_EXCEPTION_HANDLER(first, handler) AddExceptionHandler(first, handler)
#else
#define LARGE_VECTOR_VIRTUAL_ALLOC(address, size, type, protection)                                \
    VirtualAlloc(address, size, type, protection)
#define LARGE_VECTOR_VIRTUAL_PROTECT(address, size, protection, old)                               \
    VirtualProtect(address, size, protection, old)
#define LARGE_VECTOR_ADD_EXCEPTION_HANDLER(first, handler) AddVectoredExceptionHandler(first, handler)
#endif
#endif

#ifdef _WIN32
static std::vector<std::pair<u64, u64>> vector_regions {};
static std::mutex vector_regions_mutex {};

// Dynarmic reads the raw sparse JIT table, including reserved pages whose entries are all zero.
static LONG WINAPI FakePageFaultHandler(PEXCEPTION_POINTERS info) {
    const auto& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 ||
        (record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) || record.ExceptionInformation[0] != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 exception_addr = record.ExceptionInformation[1];
    const u64 page = exception_addr >> HostPageBits;
    // FreeMemoryPages takes this lock too: keep ownership stable until the page is committed.
    std::lock_guard lock(vector_regions_mutex);
    const bool owned = std::ranges::any_of(vector_regions, [page](const auto& region) {
        return region.first <= page && page < region.second;
    });
    if (!owned) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // A protection fault on a committed page is not a sparse-table first read. In particular,
    // do not change permissions of live writable metadata or unrelated reused memory.
    MEMORY_BASIC_INFORMATION state{};
    if (!VirtualQuery(reinterpret_cast<void*>(exception_addr), &state, sizeof(state))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (state.State == MEM_COMMIT) {
        // Another reader/writer may have committed the page while this fault was pending.
        // Retry readable pages without downgrading a concurrent writer's permissions.
        constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (state.Protect & readable) && !(state.Protect & PAGE_GUARD)
                   ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    if (state.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    // Commit only the reported fault page. A crossing access can fault on its next page;
    // guessing a page at fault+64 could commit outside the vector.
    return CommitVectorPage(page << HostPageBits, false) ? EXCEPTION_CONTINUE_EXECUTION
                                                       : EXCEPTION_CONTINUE_SEARCH;
}

bool CommitVectorPage(uintptr_t addr, bool write) noexcept {
    MEMORY_BASIC_INFORMATION info {};
    auto res = VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info));
    if (res == 0) {
        LOG_CRITICAL(HW_Memory, "Failed to query large buffer region at {:#x} with error {}, will try committing anyway", addr, GetLastError());
    } else if (info.State == MEM_COMMIT) {
        if (!write) {
            // Preserve permissions if a concurrent mapping committed this page first.
            constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (info.Protect & readable) && !(info.Protect & PAGE_GUARD);
        }
        ULONG old_protect {};
        auto perm = write ? PAGE_READWRITE : PAGE_READONLY;
        if (!LARGE_VECTOR_VIRTUAL_PROTECT(reinterpret_cast<void*>(addr), HostPageSize, perm, &old_protect)) {
            LOG_ERROR(HW_Memory, "Failed to change permissions of large buffer region at {:#x}, error {}", addr, GetLastError());
            return false;
        }
        return true;
    } else if (info.State != MEM_RESERVE) {
        LOG_ERROR(HW_Memory, "Tried to commit an unreserved large buffer region at {:#x} that is not mapped (state {:#x})", addr, info.State);
        return false;
    }

    auto perm = write ? PAGE_READWRITE : PAGE_READONLY;
    void* res2 = LARGE_VECTOR_VIRTUAL_ALLOC(reinterpret_cast<LPVOID>(addr), HostPageSize, MEM_COMMIT, perm);
    if (res2 == nullptr) {
        LOG_ERROR(HW_Memory, "Failed to commit large buffer region at {:#x}, error {}", addr, GetLastError());
        return false;
    }

    return true;
}
#endif

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif
#ifndef MADV_FREE
#define MADV_FREE MADV_DONTNEED
#endif

void DecommitVectorPage(uintptr_t base) noexcept {
#if defined(_WIN32)
    VirtualFree(reinterpret_cast<LPVOID>(base), HostPageSize, MEM_DECOMMIT);
#elif defined(__linux__)
    // Linux's MADV_DONTNEED zeros out pages for us
    madvise(reinterpret_cast<void*>(base), HostPageSize, MADV_DONTNEED);
#else
    madvise(reinterpret_cast<void*>(base), HostPageSize, MADV_FREE);
    std::memset(reinterpret_cast<void*>(base), 0, HostPageSize);
#endif
}

void* AllocateMemoryPages(std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        LOG_WARNING(HW_Memory, "Allocating unaligned large vector with size {:#x}; aligning to {} page size", size, page);
        size = AlignUp(size, page);
    }

#ifdef _WIN32
    // We will never use this memory entirely so instead of committing it up front let's just reserve it and commit each page individually
    void* base = LARGE_VECTOR_VIRTUAL_ALLOC(nullptr, size, MEM_RESERVE, PAGE_READWRITE);

    if (base != nullptr) {
        std::lock_guard lock(vector_regions_mutex);
        vector_regions.emplace_back(reinterpret_cast<u64>(base) >> HostPageBits, (reinterpret_cast<u64>(base) + size) >> HostPageBits);

        static std::once_flag flag;
        std::call_once(flag, []() { LARGE_VECTOR_ADD_EXCEPTION_HANDLER(1, FakePageFaultHandler); });
    } else {
        // Try committing everything instead??
        LOG_WARNING(HW_Memory, "Failed to reserve large vector region with error {}, trying to commit instead..", GetLastError());
        base = LARGE_VECTOR_VIRTUAL_ALLOC(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
    }
    ASSERT_MSG(base, "Failed to reserve {:#x} sized region with error {}", size, GetLastError());
#else
    void* base = mmap(nullptr, size, PROT_READ, MAP_ANON | MAP_PRIVATE | MAP_NOCORE, -1, 0);
    if (base == MAP_FAILED)
        base = nullptr;
    ASSERT_MSG(base, "Failed to allocate {:#x} sized region with error {}", size, strerror(errno));
#endif
    return base;
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        size = AlignUp(size, page);
    }
    if (!base)
        return;
#ifdef _WIN32
    std::lock_guard lock(vector_regions_mutex);
    const u64 first_page = reinterpret_cast<u64>(base) >> HostPageBits;
    std::erase_if(vector_regions, [first_page](const auto& r) { return r.first == first_page; });
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common

// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>

// Exercise the real Windows handler and its registry, including fault records that would be
// unsafe to raise against the old handler (execute faults and foreign reserved pages).
#include "common/sparse_large_vector.cpp"

void AssertFailSoftImpl() { std::abort(); }
void AssertFailedAt(const char*) { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
[[noreturn]] void UnreachableAt(const char*) { std::abort(); }
void Common::Log::FmtLogMessageImpl(Class, Level level, const char*, unsigned, const char*,
                                  fmt::string_view, const fmt::format_args&) {
    if (level >= Level::Error) std::abort();
}

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
MEMORY_BASIC_INFORMATION Query(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    Check(VirtualQuery(address, &info, sizeof(info)) != 0, "query allocation");
    return info;
}
LONG Fault(void* address, ULONG_PTR access = 0, DWORD flags = 0, DWORD parameters = 2) {
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionFlags = flags;
    record.NumberParameters = parameters;
    record.ExceptionInformation[0] = access;
    record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(address);
    EXCEPTION_POINTERS info{&record, nullptr};
    return Common::FakePageFaultHandler(&info);
}
} // namespace

int main() {
    constexpr auto page = Common::HostPageSize;
    const auto initial_regions = Common::vector_regions.size();
    auto* base = static_cast<unsigned char*>(Common::AllocateMemoryPages(page));
    Check(Common::vector_regions.size() == initial_regions + 1, "registered allocation");
    Check(Query(base).State == MEM_RESERVE, "initially reserved");
    Check(Fault(base, 1) == EXCEPTION_CONTINUE_SEARCH, "write fault belongs to caller");
    Check(Fault(base, 8) == EXCEPTION_CONTINUE_SEARCH, "execute fault belongs to caller");
    Check(Fault(base, 0, 0, 0) == EXCEPTION_CONTINUE_SEARCH, "short record ignored");
    Check(Fault(base, 0, EXCEPTION_NONCONTINUABLE) == EXCEPTION_CONTINUE_SEARCH,
          "noncontinuable fault ignored");
    Check(Query(base).State == MEM_RESERVE, "rejected faults do not commit");

    // The next page can be free or owned by a neighbor; neither belongs to this vector.
    const auto neighbor = Query(base + page);
    Check(Fault(base + page) == EXCEPTION_CONTINUE_SEARCH, "end is exclusive");
    Check(Fault(base + page - 8) == EXCEPTION_CONTINUE_EXECUTION, "last entry first read");
    Check(Query(base).Protect == PAGE_READONLY, "read fault commits read-only zero page");
    Check(Query(base + page).State == neighbor.State, "no speculative commit beyond vector");
    Check(*reinterpret_cast<volatile std::uint64_t*>(base) == 0, "committed bytes are zero");
    Check(Common::CommitVectorPage(reinterpret_cast<uintptr_t>(base), true), "make writable");
    Check(Fault(base) == EXCEPTION_CONTINUE_EXECUTION && Query(base).Protect == PAGE_READWRITE,
          "pending read preserves concurrent writer permissions");
    Check(Common::CommitVectorPage(reinterpret_cast<uintptr_t>(base), false) &&
              Query(base).Protect == PAGE_READWRITE, "read commit does not downgrade writer");
    DWORD previous{};
    Check(VirtualProtect(base, page, PAGE_NOACCESS, &previous), "protect committed page");
    Check(Fault(base) == EXCEPTION_CONTINUE_SEARCH && Query(base).Protect == PAGE_NOACCESS,
          "no-access fault is not repaired as a sparse read");
    Common::FreeMemoryPages(base, page);
    Check(Common::vector_regions.size() == initial_regions, "freed range unregistered");

    auto* reused = VirtualAlloc(base, page, MEM_RESERVE, PAGE_READWRITE);
    Check(reused == base, "reuse released address as foreign reservation");
    Check(Fault(reused) == EXCEPTION_CONTINUE_SEARCH && Query(reused).State == MEM_RESERVE,
          "handler never commits a released range reused by another owner");
    Check(VirtualFree(reused, 0, MEM_RELEASE), "release foreign allocation");

    // Exercise the installed VEH with a real hardware fault, not only synthetic records.
    base = static_cast<unsigned char*>(Common::AllocateMemoryPages(page));
    Check(*reinterpret_cast<volatile std::uint64_t*>(base) == 0, "real sparse JIT read fault");
    Common::FreeMemoryPages(base, page);
    for (unsigned i = 0; i < 1000; ++i) {
        void* allocation = Common::AllocateMemoryPages(page * (1 + i % 5));
        Common::FreeMemoryPages(allocation, page * (1 + i % 5));
    }
    Check(Common::vector_regions.size() == initial_regions, "no registry growth across games");
    std::puts("PASS: Windows sparse registry, fault ownership, bounds, permissions, real VEH, reuse");
}

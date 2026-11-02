// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercise the production sparse metadata and the separate table consumed directly by Dynarmic.
// A process table reinitialized at the same address-space width must not retain host pointers.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/page_table.h"

// Isolated gate: assertions and unexpected memory errors fail instead of requiring the frontend.
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

void CheckEmpty(const Common::PageTable& table) {
    for (std::size_t i = 0; i < table.entries.size(); ++i) {
        Check(table.entries[i].Type() == Common::PageType::Unmapped,
              "reset metadata page type");
        Check(table.entries[i].Pointer(true) == 0, "reset metadata host pointer");
        Check(table.jit_entries[i].Load() == 0, "reset JIT host pointer");
    }
}
} // namespace

int main() {
    Common::PageTable table;
    table.Resize(24, 12);
    const auto count = table.entries.size();
    Check(count == 4096, "configured page count");
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
        for (const auto index : std::array<std::size_t, 6>{0, 1, 511, 512, 1023, count - 1}) {
            table.entries.GetAndFault(index).Store(false, Common::PageType::Memory, 7,
                                                   0x12345000);
            table.jit_entries.GetAndFault(index).Store(0x12345000);
            Check(table.entries[index].Pointer() == 0x12345000, "metadata written");
            Check(table.jit_entries[index].Load() == 0x12345000, "JIT pointer written");
        }
        table.Resize(24, 12);
        CheckEmpty(table);
    }
    table.Resize(25, 12);
    CheckEmpty(table);
    table.Resize(23, 12);
    CheckEmpty(table);

    // The last allocation page need not be full; reset must clear its valid tail too.
    Common::SparseLargeVector<std::uint64_t> partial{777};
    partial.Set(0, 12);
    partial.Set(776, 34);
    partial.ResizeAndClear(777);
    for (std::size_t i = 0; i < partial.size(); ++i) Check(partial[i] == 0, "partial page reset");
    partial.Set(776, 56);
    Check(partial[776] == 56, "partial page writable after reset");
    std::puts("PASS: production page-table/JIT reset, same-size reuse, grow/shrink, partial pages");
}

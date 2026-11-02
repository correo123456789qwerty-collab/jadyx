// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace Common {

// Keep the first sixteen faults, then a logarithmic progress report. The tracker separately
// counts every fault and retains address samples. Successful accesses never touch this counter.
class FaultLogLimiter {
public:
    std::uint64_t Next() noexcept {
        return hits.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    static constexpr bool ShouldLog(std::uint64_t occurrence) noexcept {
        return occurrence <= 16 || std::has_single_bit(occurrence);
    }

private:
    std::atomic<std::uint64_t> hits{};
};

} // namespace Common

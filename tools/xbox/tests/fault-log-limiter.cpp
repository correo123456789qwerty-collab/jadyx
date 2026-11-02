// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "common/fault_log_limiter.h"

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    Common::FaultLogLimiter limiter;
    std::uint64_t logged{};
    for (std::uint64_t i = 1; i <= 26630352; ++i) {
        const auto occurrence = limiter.Next();
        Check(occurrence == i, "every occurrence counted");
        if (Common::FaultLogLimiter::ShouldLog(occurrence)) {
            if (logged < 16) Check(occurrence == logged + 1, "first sixteen preserved");
            ++logged;
        }
    }
    Check(logged == 36, "26,630,352 faults produce 36 progress lines");

    Common::FaultLogLimiter concurrent;
    std::atomic<std::uint64_t> total{}, messages{};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (unsigned j = 0; j < 10000; ++j) {
            const auto occurrence = concurrent.Next();
            total.fetch_add(occurrence, std::memory_order_relaxed);
            if (Common::FaultLogLimiter::ShouldLog(occurrence)) ++messages;
        }
    });
    for (auto& thread : threads) thread.join();
    Check(concurrent.Next() == 40001, "cross-core count exact");
    Check(total == 40000ULL * 40001 / 2 && messages == 27, "concurrent sequence and bound");
    Common::FaultLogLimiter next_game;
    Check(next_game.Next() == 1, "new memory instance starts with its first report");
    std::puts("PASS: 26,630,352 faults -> 36 log lines; samples, counters and cross-core accounting");
}

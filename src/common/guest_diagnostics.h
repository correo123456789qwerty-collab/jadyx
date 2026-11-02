// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include "common/bug_tracker.h"

namespace Common::GuestDiagnostics {

// Fatal guest reports must survive a disabled/unavailable text logger. The tracker stores only
// 480 bytes per message: split backtraces into its 16 detail slots, without growing its queue or
// adding work to successful memory accesses. Repeated chunks are deduplicated by the tracker.
inline void RecordBacktrace(std::string_view text) {
    constexpr std::size_t chunk_size = 400;
    std::size_t offset{};
    for (std::size_t chunk = 0; chunk < BugTracker::Site::DETAIL_SLOTS && offset < text.size();
         ++chunk) {
        auto count = (std::min)(chunk_size, text.size() - offset);
        // Module/symbol names may be UTF-8: each JSON detail must remain a complete string.
        while (count > 0 && offset + count < text.size() &&
               (static_cast<unsigned char>(text[offset + count]) & 0xc0) == 0x80) {
            --count;
        }
        if (count == 0) break;
        BUG_TRACK_KEY(Other, chunk, "guest backtrace +{}: {}", offset,
                      text.substr(offset, count));
        offset += count;
    }
    if (offset < text.size()) {
        BUG_TRACK(Other, "guest backtrace truncated: retained {} of {} bytes", offset,
                  text.size());
    }
}

inline void RecordBreakCode(std::uint64_t address, std::uint32_t code) {
    BUG_TRACK_KEY(Other, address, "guest break code={:#010x} buffer={:#x}", code, address);
}

} // namespace Common::GuestDiagnostics

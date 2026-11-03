// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace D3D12 {

/// Remove only PSO inputs never read by shader preparation or MakeRuntimeInfo. Preserve every
/// shader input, including dual-source blending, vertex fetch layout, topology, XFB and alpha
/// test. This is a separate key: the actual PSO always uses the full, original pipeline key.
template <class Key>
std::vector<u32> MakeGraphicsShaderKey(Key key) {
    static_assert(std::is_trivially_copyable_v<Key>);
    static_assert(std::has_unique_object_representations_v<Key>);
    static_assert(sizeof(Key) % sizeof(u32) == 0);
    key.depth_bias = 0;
    key.depth_bias_clamp = 0;
    key.slope_scaled_depth_bias = 0;
    key.stencil_read_mask = 0;
    key.stencil_write_mask = 0;
    key.strip_cut = 0;
    key.state.dynamic_state = {};
    key.state.attachments = {};
    key.state.color_formats = {};
    key.state.depth_enabled.Assign(0);
    key.state.depth_format.Assign(0);
    // FixedPipelineState::Size() excludes this tail when XFB is off. The shader key stores
    // complete bytes, so normalize the inactive tail rather than retaining stale registers.
    if (key.state.xfb_enabled == 0) {
        key.state.xfb_state = {};
        key.state.depth_bounds_min = 0;
        key.state.depth_bounds_max = 0;
        key.state.line_stipple_factor = 0;
        key.state.line_stipple_pattern = 0;
    }
    std::vector<u32> words(sizeof(Key) / sizeof(u32));
    std::memcpy(words.data(), &key, sizeof(Key));
    return words;
}

} // namespace D3D12

// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cassert>
#include <bit>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_graphics_shader_key.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"

using namespace D3D12;

#include "graphics-frontend-fixture.h"

int main() try {
    Common::Log::Initialize();
    CheckEnvironmentRecord();
    GraphicsPipelineCacheKey key{};
    key.unique_hashes[1] = 11;
    key.unique_hashes[5] = 55;
    key.state.topology.Assign(Maxwell::PrimitiveTopology::Triangles);
    key.state.alpha_test_func.Assign(Vulkan::FixedPipelineState::PackComparisonOp(
        Maxwell::ComparisonOp::Always_GL));
    key.state.point_size = std::bit_cast<u32>(1.0f);
    const auto base_key = MakeGraphicsShaderKey(key);
    // Changing a real shader input must split the preparation key.
    for (u32 field = 0; field < 12; ++field) {
        auto changed = key;
        switch (field) {
        case 0: ++changed.unique_hashes[5]; break;
        case 1: changed.state.topology.Assign(Maxwell::PrimitiveTopology::Points); break;
        case 2: changed.state.ndc_minus_one_to_one.Assign(1); break;
        case 3: ++changed.state.point_size; break;
        case 4: changed.state.alpha_test_func.Assign(1); break;
        case 5: ++changed.state.alpha_test_ref; break;
        case 6: changed.state.attachment0_dual_source_blend.Assign(1); break;
        case 7: changed.state.attributes[0].enabled.Assign(1); break;
        case 8: ++changed.state.vertex_strides[0]; break;
        case 9: changed.state.y_negate.Assign(1); break;
        case 10: changed.state.early_z.Assign(1); break;
        case 11: changed.state.xfb_enabled.Assign(1); break;
        }
        assert(MakeGraphicsShaderKey(changed) != base_key);
    }
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    Shader::HostTranslateInfo host{};
    host.ApplyDescriptorLimitPolicy();
    TestEnvironment vs, ps(Shader::Stage::Fragment);
    std::array<Shader::Environment*, 2> envs{&vs, &ps};
    ShaderPools pools;
    using Cache = LinkedShaderCache<PreparedGraphicsShaders>;
    Cache cache;
    u32 translations{};
    auto build = [&](const auto& variant, bool record = true) {
        ++translations;
        pools.ReleaseContents();
        return PrepareGraphicsShaders(pools, variant, envs, profile, host, false, record);
    };
    auto measure = [](const PreparedGraphicsShaders& value) { return value.SizeBytes(); };
    const auto cached = cache.Get(base_key, [&] { return build(key); }, measure);
    assert(!cached.value->spirv[0].empty() && !cached.value->spirv[4].empty());
    assert(cached.value->Matches(envs));
    double uncached_us{}, reused_us{};
    for (u32 i = 1; i <= 128; ++i) {
        auto variant = key;
        variant.depth_bias = i;
        variant.depth_bias_clamp = i + 1;
        variant.slope_scaled_depth_bias = i + 2;
        variant.stencil_read_mask = static_cast<u8>(i);
        variant.stencil_write_mask = static_cast<u8>(i + 1);
        variant.strip_cut = static_cast<u8>(i % 3);
        variant.state.dynamic_state.raw1 = i;
        variant.state.dynamic_state.raw2 = i << 16;
        variant.state.attachments[0].raw = i * 17;
        variant.state.color_formats[0] = static_cast<u8>(i);
        variant.state.depth_enabled.Assign(i & 1);
        variant.state.depth_format.Assign(i & 31);
        assert(MakeGraphicsShaderKey(variant) == base_key);
        assert(variant != key); // PSO identity is still distinct.
        auto start = std::chrono::steady_clock::now();
        auto reference = build(variant, false);
        uncached_us += std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        start = std::chrono::steady_clock::now();
        const auto hit = cache.Get(MakeGraphicsShaderKey(variant), [&] { return build(variant); }, measure);
        assert(hit.outcome == Cache::Outcome::Hit && hit.value->Matches(envs));
        reused_us += std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        assert(hit.value->spirv == reference.spirv);
        assert(hit.value->lower_noperspective_mask == reference.lower_noperspective_mask);
        for (size_t stage : hit.value->stage_indices) {
            assert(*InternStage(*hit.value->infos[stage]) == *InternStage(*reference.infos[stage]));
            assert(hit.value->infos[stage]->constant_buffer_mask ==
                   reference.infos[stage]->constant_buffer_mask);
            assert(hit.value->infos[stage]->constant_buffer_used_sizes ==
                   reference.infos[stage]->constant_buffer_used_sizes);
        }
    }
    assert(translations == 129);
    assert(cache.RetainedBytes() <= Cache::DEFAULT_BUDGET);
    cache.SetBudget(0);
    assert(cache.RetainedBytes() == 0 && cached.value->Matches(envs));
    std::cout << "PASS 128 real Maxwell frontend variants, exact SPIR-V/bindings, "
                 "15 specialization mutations, replayed inputs and pressure trim\n"
              << "Synthetic frontend mean us: " << uncached_us / 128 << " -> " << reused_us / 128
              << " (not game FPS); entry bytes " << cached.value->SizeBytes() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}

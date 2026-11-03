// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cassert>
#include "video_core/renderer_d3d12/d3d12_shader_environment_record.h"
using D3D12::ShaderEnvironmentRecord;

class TestEnvironment final : public Shader::Environment {
public:
    explicit TestEnvironment(Shader::Stage shader_stage = Shader::Stage::VertexB) {
        stage = shader_stage;
        code.assign(1056, 0x50b0000000070f00ULL); // Maxwell NOP, always predicate.
        // LDC.32 R0, c[0][0]: a real uniform read kept live by the stage's output.
        code[0] = 0xef9400000007ff00ULL;
        if (stage == Shader::Stage::VertexB) {
            // AST Position.x, R0, with direct addressing and vertex/index RZ.
            code[1] = 0xeff000000007ff00ULL | (u64{255} << 39) | (u64{112} << 20);
            sph.vtg.omap_systemb.position_x.Assign(1);
        } else if (stage == Shader::Stage::Fragment) {
            sph.ps.omap.target = 1; // EXIT exports R0 as RT0.x.
        }
        code.back() = 0xe30000000007000fULL; // EXIT; control words are skipped by Location.
        // The last word must not be a scheduler control word.
        assert(((code.size() - 1) * 8 + sizeof(sph)) % 32 != 0);
    }
    std::vector<u64> code;
    u32 specialization{17};
    mutable u32 visited{};
    bool missing{};
    void ChangeHeader() { sph.common1.shader_local_memory_low_size.Assign(32); }
    u64 ReadInstruction(u32 address) override {
        visited |= 1;
        const auto i = (address - sizeof(sph)) / sizeof(u64);
        if (i >= code.size()) throw Shader::LogicError("fixture instruction out of bounds");
        return code[i];
    }
    u32 ReadCbufValue(u32 a, u32 b) override {
        visited |= 2;
        if (missing) throw Shader::LogicError("missing fixture specialization");
        return specialization + a + b;
    }
    Shader::TextureType ReadTextureType(u32) override { visited |= 4; return texture_type; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        visited |= 8; return texture_format;
    }
    bool IsTexturePixelFormatInteger(u32) override { visited |= 16; return integer; }
    u32 ReadViewportTransformState() override { visited |= 32; return viewport; }
    u32 TextureBoundBuffer() const override { visited |= 64; return bound; }
    u32 LocalMemorySize() const override { visited |= 128; return local; }
    u32 SharedMemorySize() const override { visited |= 256; return shared; }
    std::array<u32, 3> WorkgroupSize() const override { visited |= 512; return workgroup; }
    bool HasHLEMacroState() const override { visited |= 1024; return hle; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        visited |= 2048; return replacement;
    }
    void Dump(u64, u64) override {}
    Shader::TextureType texture_type{Shader::TextureType::Color2D};
    Shader::TexturePixelFormat texture_format{Shader::TexturePixelFormat::R8_UNORM};
    bool integer{};
    u32 viewport{1}, bound{2}, local{}, shared{};
    std::array<u32, 3> workgroup{1, 2, 3};
    bool hle{};
    std::optional<Shader::ReplaceConstant> replacement;
};

static void RecordQueries(Shader::Environment& env) {
    (void)env.ReadInstruction(80);
    (void)env.ReadCbufValue(1, 8);
    (void)env.ReadTextureType(12);
    (void)env.ReadTexturePixelFormat(13);
    (void)env.IsTexturePixelFormatInteger(14);
    (void)env.ReadViewportTransformState();
    (void)env.TextureBoundBuffer();
    (void)env.LocalMemorySize();
    (void)env.SharedMemorySize();
    (void)env.WorkgroupSize();
    (void)env.HasHLEMacroState();
    (void)env.GetReplaceConstBuffer(0, 4);
}

static void CheckEnvironmentRecord() {
    TestEnvironment source;
    ShaderEnvironmentRecord record(source);
    RecordQueries(record);
    record.Detach();
    TestEnvironment same;
    assert(record.Matches(same) && same.visited == 4095);
    for (u32 field = 0; field < 15; ++field) {
        TestEnvironment changed;
        switch (field) {
        case 0: changed.code[0] ^= 1; break;
        case 1: ++changed.specialization; break;
        case 2: changed.texture_type = Shader::TextureType::ColorCube; break;
        case 3: changed.texture_format = Shader::TexturePixelFormat::R32_UINT; break;
        case 4: changed.integer = true; break;
        case 5: ++changed.viewport; break;
        case 6: ++changed.bound; break;
        case 7: ++changed.local; break;
        case 8: ++changed.shared; break;
        case 9: ++changed.workgroup[0]; break;
        case 10: ++changed.workgroup[1]; break;
        case 11: ++changed.workgroup[2]; break;
        case 12: changed.hle = true; break;
        case 13: changed.replacement = Shader::ReplaceConstant::BaseVertex; break;
        case 14: changed.ChangeHeader(); break;
        }
        assert(!record.Matches(changed));
    }
    same.missing = true;
    assert(!record.Matches(same));
    TestEnvironment replaced;
    replaced.replacement = Shader::ReplaceConstant::BaseInstance; // enum zero != nullopt
    ShaderEnvironmentRecord replacement_record(replaced);
    assert(replacement_record.GetReplaceConstBuffer(0, 4) == replaced.replacement);
    replacement_record.Detach();
    assert(replacement_record.Matches(replaced));
    replaced.replacement.reset();
    assert(!replacement_record.Matches(replaced));
}


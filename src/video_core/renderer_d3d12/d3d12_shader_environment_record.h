// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>
#include <vector>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"

namespace D3D12 {

/// All inputs actually consulted by a graphics shader translation. Replaying them both checks
/// specialization (including code, not just its hash) and populates a fresh GenericEnvironment's
/// serialization tables. A cache hit must never serialize an environment with missing queries.
class ShaderEnvironmentRecord final : public Shader::Environment {
public:
    explicit ShaderEnvironmentRecord(Shader::Environment& source_) : source{&source_} {
        sph = source->SPH();
        gp_passthrough_mask = source->GpPassthroughMask();
        stage = source->ShaderStage();
        start_address = source->StartAddress();
        is_proprietary_driver = source->IsProprietaryDriver();
    }

    void Detach() {
        source = nullptr;
        decltype(cbuf_word_cache){}.swap(cbuf_word_cache);
        decltype(handle_cache){}.swap(handle_cache);
        decltype(track_cache){}.swap(track_cache);
    }

    bool Matches(Shader::Environment& other) const {
        if (std::memcmp(&sph, &other.SPH(), sizeof(sph)) != 0 ||
            gp_passthrough_mask != other.GpPassthroughMask() || stage != other.ShaderStage() ||
            start_address != other.StartAddress() ||
            is_proprietary_driver != other.IsProprietaryDriver()) {
            return false;
        }
        try {
            for (const auto& query : queries) {
                if (Read(other, query.kind, query.a, query.b) != query.value) {
                    return false;
                }
            }
        } catch (const Shader::Exception&) {
            // A disk environment may not contain a query made by a different specialization.
            return false;
        }
        return true;
    }

    size_t SizeBytes() const {
        return sizeof(*this) + queries.capacity() * sizeof(Query);
    }

    u64 ReadInstruction(u32 address) override { return Record(Kind::Instruction, address); }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        return static_cast<u32>(Record(Kind::Cbuf, bank, offset));
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        return static_cast<Shader::TextureType>(Record(Kind::TextureType, handle));
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        return static_cast<Shader::TexturePixelFormat>(Record(Kind::TextureFormat, handle));
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return Record(Kind::IntegerFormat, handle) != 0;
    }
    u32 ReadViewportTransformState() override { return static_cast<u32>(Record(Kind::Viewport)); }
    u32 TextureBoundBuffer() const override { return static_cast<u32>(Record(Kind::BoundBuffer)); }
    u32 LocalMemorySize() const override { return static_cast<u32>(Record(Kind::LocalMemory)); }
    u32 SharedMemorySize() const override { return static_cast<u32>(Record(Kind::SharedMemory)); }
    std::array<u32, 3> WorkgroupSize() const override {
        return {static_cast<u32>(Record(Kind::Workgroup, 0)),
                static_cast<u32>(Record(Kind::Workgroup, 1)),
                static_cast<u32>(Record(Kind::Workgroup, 2))};
    }
    bool HasHLEMacroState() const override { return Record(Kind::Hle) != 0; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        const auto value = Record(Kind::Replacement, bank, offset);
        return value ? std::optional{static_cast<Shader::ReplaceConstant>(value - 1)} : std::nullopt;
    }
    void Dump(u64 pipeline, u64 shader) override { source->Dump(pipeline, shader); }
    std::string DescribeCode() const override { return source->DescribeCode(); }

private:
    enum class Kind : u32 {
        Instruction, Cbuf, TextureType, TextureFormat, IntegerFormat, Viewport, BoundBuffer,
        LocalMemory, SharedMemory, Workgroup, Hle, Replacement,
    };
    struct Query {
        Kind kind;
        u32 a;
        u32 b;
        u64 value;
    };
    static u64 Read(Shader::Environment& env, Kind kind, u32 a, u32 b) {
        switch (kind) {
        case Kind::Instruction: return env.ReadInstruction(a);
        case Kind::Cbuf: return env.ReadCbufValue(a, b);
        case Kind::TextureType: return static_cast<u64>(env.ReadTextureType(a));
        case Kind::TextureFormat: return static_cast<u64>(env.ReadTexturePixelFormat(a));
        case Kind::IntegerFormat: return env.IsTexturePixelFormatInteger(a);
        case Kind::Viewport: return env.ReadViewportTransformState();
        case Kind::BoundBuffer: return env.TextureBoundBuffer();
        case Kind::LocalMemory: return env.LocalMemorySize();
        case Kind::SharedMemory: return env.SharedMemorySize();
        case Kind::Workgroup: return env.WorkgroupSize()[a];
        case Kind::Hle: return env.HasHLEMacroState();
        case Kind::Replacement: {
            const auto replacement = env.GetReplaceConstBuffer(a, b);
            return replacement ? static_cast<u64>(*replacement) + 1 : 0;
        }
        }
        return 0;
    }
    u64 Record(Kind kind, u32 a = 0, u32 b = 0) const {
        const auto value = Read(*source, kind, a, b);
        queries.push_back({kind, a, b, value});
        return value;
    }

    // Consulted only during translation. Matches() uses the supplied fresh environment instead.
    Shader::Environment* source;
    mutable std::vector<Query> queries;
};

} // namespace D3D12

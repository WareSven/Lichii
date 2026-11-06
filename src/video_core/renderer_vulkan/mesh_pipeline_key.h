// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "shader_recompiler/stage.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace Vulkan {

inline bool HasMeshShaderSlots(const std::array<u64, 6>& hashes) {
    return hashes[1] != 0 && hashes[0] == 0 && hashes[2] == 0 && hashes[4] == 0;
}

inline bool HasOnlyMeshStages(std::span<const Shader::Stage> stages) {
    bool has_mesh = false;
    for (const auto stage : stages) {
        if (stage == Shader::Stage::Mesh) {
            has_mesh = true;
        } else if (stage != Shader::Stage::Task && stage != Shader::Stage::Fragment) {
            return false;
        }
    }
    return has_mesh;
}

inline void NormalizeMeshVertexInput(FixedPipelineState& state, bool is_mesh) {
    if (is_mesh && state.dynamic_vertex_input) {
        state.attribute_types = 0;
    }
}

// Refresh retains vertex input between dirty updates, including across mesh draws.
class ScopedMeshVertexInput {
public:
    ScopedMeshVertexInput(FixedPipelineState& state_, bool is_mesh)
        : state{is_mesh && state_.dynamic_vertex_input ? &state_ : nullptr},
          saved_types{state ? state->attribute_types : 0} {
        if (state) {
            state->attribute_types = 0;
        }
    }

    ~ScopedMeshVertexInput() {
        if (state) {
            state->attribute_types = saved_types;
        }
    }

    ScopedMeshVertexInput(const ScopedMeshVertexInput&) = delete;
    ScopedMeshVertexInput& operator=(const ScopedMeshVertexInput&) = delete;

private:
    FixedPipelineState* state;
    u64 saved_types;
};

inline void MigrateUsedPipelineHash(std::unordered_map<u64, u8>& used, u64 old_hash, u64 hash) {
    if (old_hash == hash) {
        return;
    }
    auto old = used.extract(old_hash);
    if (old.empty()) {
        return;
    }
    const auto [it, inserted] = used.try_emplace(hash, old.mapped());
    if (!inserted) {
        it->second = std::min(it->second, old.mapped());
    }
}

enum class GraphicsPipelineLoadAction { Duplicate, Defer, Preload, Promote };

class GraphicsPipelineLoadPlan {
public:
    GraphicsPipelineLoadAction Add(std::unordered_set<u64>& disk_pipelines, u64 hash,
                                   bool preload) {
        if (disk_pipelines.insert(hash).second) {
            if (preload) {
                return GraphicsPipelineLoadAction::Preload;
            }
            deferred.insert(hash);
            return GraphicsPipelineLoadAction::Defer;
        }
        // A later legacy alias can be in the used list even when the first alias was not.
        if (preload && deferred.erase(hash) != 0) {
            return GraphicsPipelineLoadAction::Promote;
        }
        return GraphicsPipelineLoadAction::Duplicate;
    }

private:
    std::unordered_set<u64> deferred;
};

} // namespace Vulkan

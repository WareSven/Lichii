// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>

#include "shader_recompiler/shader_info.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Native task and mesh programs occupy vertex and tessellation slots.
inline VkShaderStageFlagBits GraphicsStageFlag(const Shader::Info& info,
                                               VkShaderStageFlagBits slot_stage) {
    if (info.is_task) {
        return VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (info.is_mesh) {
        return VK_SHADER_STAGE_MESH_BIT_EXT;
    }
    return slot_stage;
}

inline bool HasTessellationStages(bool control_present, const Shader::Info& control,
                                  bool evaluation_present, const Shader::Info& evaluation) {
    return (control_present && GraphicsStageFlag(control, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) ==
                                   VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT) ||
           (evaluation_present &&
            GraphicsStageFlag(evaluation, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) ==
                VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
}

inline bool IsVertexInputDynamicState(VkDynamicState state) {
    return state == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT ||
           state == VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT ||
           state == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT ||
           state == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT ||
           state == VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT;
}

inline std::span<const VkPipeline> LinkedGraphicsLibraries(
    const std::array<VkPipeline, 4>& libraries, bool mesh_pipeline) {
    return std::span{libraries}.subspan(mesh_pipeline ? 1 : 0);
}

} // namespace Vulkan

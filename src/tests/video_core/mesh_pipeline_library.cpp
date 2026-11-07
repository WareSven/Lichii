// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/graphics_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_trace.h"

using namespace Vulkan;

TEST_CASE("Task and mesh slots do not acquire tessellation state",
          "[video_core][mesh_pipeline_library]") {
    const Shader::Info ordinary{};
    Shader::Info task{};
    task.is_task = true;
    Shader::Info mesh{};
    mesh.is_mesh = true;

    CHECK(GraphicsStageFlag(task, VK_SHADER_STAGE_VERTEX_BIT) == VK_SHADER_STAGE_TASK_BIT_EXT);
    CHECK(GraphicsStageFlag(mesh, VK_SHADER_STAGE_VERTEX_BIT) == VK_SHADER_STAGE_MESH_BIT_EXT);
    CHECK(GraphicsStageFlag(mesh, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) ==
          VK_SHADER_STAGE_MESH_BIT_EXT);
    CHECK_FALSE(HasTessellationStages(false, ordinary, true, mesh));
    CHECK_FALSE(HasTessellationStages(false, ordinary, false, ordinary));
    CHECK(HasTessellationStages(true, ordinary, false, ordinary));
    CHECK(HasTessellationStages(false, ordinary, true, ordinary));
    CHECK(HasTessellationStages(true, ordinary, true, ordinary));
}

TEST_CASE("Mesh library linking omits the absent vertex input subset",
          "[video_core][mesh_pipeline_library]") {
    // Applies to both mesh-only and task/mesh, including optimized relinking.
    const std::array<VkPipeline, 4> libraries{};
    const auto mesh = LinkedGraphicsLibraries(libraries, true);
    REQUIRE(mesh.size() == 3);
    CHECK(mesh.data() == libraries.data() + 1);
    const auto vertex = LinkedGraphicsLibraries(libraries, false);
    REQUIRE(vertex.size() == 4);
    CHECK(vertex.data() == libraries.data());
}

TEST_CASE("Mesh removes only vertex input and tessellation dynamic states",
          "[video_core][mesh_pipeline_library]") {
    const std::array removed{
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT,
        VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT,
        VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT,
    };
    const std::array retained{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
    };
    std::vector<VkDynamicState> states{retained.begin(), retained.end()};
    states.insert(states.end(), removed.begin(), removed.end());
    std::erase_if(states, IsVertexInputDynamicState);
    CHECK(std::ranges::equal(states, retained));
}

TEST_CASE("Mesh shader libraries and fallback share the complete graphics push range",
          "[video_core][mesh_pipeline_library]") {
    const auto vertex = DescriptorLayoutBuilder::MakePushConstantRange(false, false);
    const auto mesh = DescriptorLayoutBuilder::MakePushConstantRange(false, true);
    CHECK(vertex.stageFlags == VK_SHADER_STAGE_ALL_GRAPHICS);
    CHECK(mesh.stageFlags == (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_TASK_BIT_EXT |
                             VK_SHADER_STAGE_MESH_BIT_EXT));
    CHECK(mesh.offset == vertex.offset);
    CHECK(mesh.size == vertex.size);
    CHECK(mesh.size > 0);
    const auto compute = DescriptorLayoutBuilder::MakePushConstantRange(true, false);
    CHECK(compute.stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
    CHECK(compute.offset == 0);
    CHECK(compute.size < mesh.size);
}

TEST_CASE("Pipeline trace hashes require the complete hexadecimal key",
          "[video_core][mesh_pipeline_library]") {
    CHECK(ParsePipelineTraceHash("20acc41b19b908cf") == 0x20acc41b19b908cfULL);
    CHECK(ParsePipelineTraceHash("20ACC41B19B908CF") == 0x20acc41b19b908cfULL);
    CHECK(ParsePipelineTraceHash("0000000000000000") == 0);
    CHECK(ParsePipelineTraceHash("ffffffffffffffff") == ~u64{});
    for (const auto invalid : {"", "20acc41b", "020acc41b19b908cf0", "0xacc41b19b908cf",
                               " 0acc41b19b908cf", "+0acc41b19b908cf", "-0acc41b19b908cf",
                               "20acc41b19b908cg", "20acc41b19b908c "}) {
        CHECK_FALSE(ParsePipelineTraceHash(invalid));
    }
}

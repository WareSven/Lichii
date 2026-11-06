// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <future>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/mesh_pipeline_key.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"

namespace {
using namespace Vulkan;

GraphicsPipelineCacheKey Key(u64 types) {
    GraphicsPipelineCacheKey key{};
    key.unique_hashes = {0, 11, 0, 22, 0, 33};
    key.state.dynamic_vertex_input.Assign(1);
    key.state.attribute_types = types;
    return key;
}
} // namespace

TEST_CASE("Mesh keys omit only dynamic vertex types", "[video_core][mesh_pipeline_key]") {
    auto first = Key(0x155);
    auto second = Key(0x105);
    REQUIRE(first != second);
    {
        const ScopedMeshVertexInput first_mesh{first.state, true};
        const ScopedMeshVertexInput second_mesh{second.state, true};
        CHECK(first == second);
        CHECK(first.Hash() == second.Hash());
        for (const auto change : {0, 1, 2, 3}) {
            auto distinct = first;
            switch (change) {
            case 0:
                distinct.state.color_formats[0] = 1;
                break;
            case 1:
                distinct.state.depth_enabled.Assign(1);
                break;
            case 2:
                distinct.state.attachments[0].enable.Assign(1);
                break;
            case 3:
                distinct.unique_hashes[5] ^= 1;
                break;
            }
            CHECK(distinct != first);
        }
    }
    CHECK(first.state.attribute_types == 0x155);
    CHECK(second.state.attribute_types == 0x105);
    CHECK(first != second);
}

TEST_CASE("Mesh lookup restores retained vertex input on every exit",
          "[video_core][mesh_pipeline_key]") {
    auto key = Key(0x155);
    const auto vertex_before = key;
    const auto lookup = [&](bool mesh, bool fail) {
        const ScopedMeshVertexInput projection{key.state, mesh};
        if (fail) {
            throw std::runtime_error("pipeline build failed");
        }
        return key; // Pipeline objects and asynchronous serialization retain this copy.
    };
    CHECK(lookup(false, false) == vertex_before);
    const auto mesh_copy = lookup(true, false);
    CHECK(mesh_copy.state.attribute_types == 0);
    // A clean VertexInput draw keeps the previous Refresh result instead of rebuilding it.
    CHECK(std::memcmp(&key, &vertex_before, sizeof(key)) == 0);
    CHECK(lookup(false, false) == vertex_before);
    CHECK_THROWS_AS(lookup(true, true), std::runtime_error);
    CHECK(key == vertex_before);

    // A dirty refresh during mesh rendering must survive the later clean vertex draw too.
    key.state.attribute_types = 0x305;
    const auto vertex_after_dirty = key;
    CHECK(lookup(true, false).state.attribute_types == 0);
    CHECK(lookup(false, false) == vertex_after_dirty);
    CHECK(mesh_copy.state.attribute_types == 0);
}

TEST_CASE("Vertex pipelines and static divisors retain their key state",
          "[video_core][mesh_pipeline_key]") {
    auto vertex = Key(0x255);
    const auto before = vertex;
    {
        const ScopedMeshVertexInput ordinary{vertex.state, false};
        CHECK(vertex == before);
    }
    vertex.state.dynamic_vertex_input.Assign(0);
    vertex.state.enabled_divisors = 0x1234;
    const auto static_before = vertex;
    {
        const ScopedMeshVertexInput mesh{vertex.state, true};
        CHECK(std::memcmp(&vertex, &static_before, sizeof(vertex)) == 0);
    }
    NormalizeMeshVertexInput(vertex.state, true);
    CHECK(std::memcmp(&vertex, &static_before, sizeof(vertex)) == 0);
}

TEST_CASE("Mesh classification uses actual stages and compatible slots",
          "[video_core][mesh_pipeline_key]") {
    using S = Shader::Stage;
    CHECK(HasOnlyMeshStages(std::array{S::Task, S::Mesh, S::Fragment}));
    CHECK(HasOnlyMeshStages(std::array{S::Mesh}));
    CHECK_FALSE(HasOnlyMeshStages(std::array{S::Task, S::Fragment}));
    for (const auto stage : {S::VertexB, S::VertexA, S::TessellationControl, S::TessellationEval,
                             S::Geometry, S::Compute}) {
        CHECK_FALSE(HasOnlyMeshStages(std::array{S::Mesh, stage}));
    }
    CHECK(HasMeshShaderSlots(Key(1).unique_hashes));
    for (const size_t slot : {0, 2, 4}) {
        auto key = Key(1);
        key.unique_hashes[slot] = 99;
        CHECK_FALSE(HasMeshShaderSlots(key.unique_hashes));
    }
}

TEST_CASE("Legacy mesh aliases preserve used ages and promote deferred work once",
          "[video_core][mesh_pipeline_key]") {
    auto first = Key(0x155);
    auto recent = Key(0x105);
    const auto first_hash = first.Hash();
    const auto recent_hash = recent.Hash();
    NormalizeMeshVertexInput(first.state, true);
    NormalizeMeshVertexInput(recent.state, true);
    const auto canonical = first.Hash();
    REQUIRE(canonical == recent.Hash());
    REQUIRE(canonical != first_hash);
    REQUIRE(canonical != recent_hash);

    for (const bool recent_first : {false, true}) {
        std::unordered_map<u64, u8> used{{recent_hash, 2}};
        std::unordered_set<u64> disk;
        GraphicsPipelineLoadPlan plan;
        PipelinePrebuild<GraphicsPipelineCacheKey, int> background{4, 1024, false};
        int preloads = 0;
        int background_builds = 0;
        const std::array order = recent_first ? std::array{recent_hash, first_hash}
                                              : std::array{first_hash, recent_hash};
        for (const auto original : order) {
            MigrateUsedPipelineHash(used, original, canonical);
            switch (plan.Add(disk, canonical, used.contains(canonical))) {
            case GraphicsPipelineLoadAction::Defer:
                REQUIRE(background.Add(first, 16, [&] {
                    ++background_builds;
                    return std::make_unique<int>(1);
                }));
                break;
            case GraphicsPipelineLoadAction::Promote:
                CHECK_FALSE(background.Take(first));
                [[fallthrough]];
            case GraphicsPipelineLoadAction::Preload:
                ++preloads;
                break;
            case GraphicsPipelineLoadAction::Duplicate:
                break;
            }
        }
        std::promise<void> reached;
        auto completed = reached.get_future();
        auto marker = first;
        ++marker.unique_hashes[5];
        REQUIRE(background.Add(marker, 16, [&] {
            reached.set_value();
            return std::make_unique<int>(2);
        }));
        background.Start(1);
        REQUIRE(completed.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        background.Stop();
        CHECK(background_builds == 0);
        CHECK(preloads == 1);
        CHECK(disk.size() == 1);
        CHECK(used.size() == 1);
        CHECK(used.at(canonical) == 2);
        CHECK(plan.Add(disk, canonical, true) == GraphicsPipelineLoadAction::Duplicate);
        used.emplace(first_hash, 1);
        MigrateUsedPipelineHash(used, first_hash, canonical);
        CHECK(used.at(canonical) == 1);
        used.emplace(recent_hash, 8);
        MigrateUsedPipelineHash(used, recent_hash, canonical);
        CHECK(used.at(canonical) == 1);
    }
}

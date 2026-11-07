// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/used_pipeline_cache.h"

namespace {
using namespace std::chrono_literals;
using Vulkan::UsedPipelineAges;
using Vulkan::UsedPipelineCheckpoint;

struct TempDir {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("eden-used-pipelines-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
} // namespace

TEST_CASE("Used pipelines checkpoint the final burst without another new key",
          "[video_core][used_pipeline_cache]") {
    const UsedPipelineCheckpoint::Clock::time_point start{};
    UsedPipelineCheckpoint checkpoint{start};
    CHECK_FALSE(checkpoint.IsDue(start + 60s, 64));
    checkpoint.MarkDirty();
    CHECK_FALSE(checkpoint.IsDue(start + 14s, 64));
    REQUIRE(checkpoint.IsDue(start + 15s, 64));
    checkpoint.CompleteAttempt(start + 15s, true);
    CHECK_FALSE(checkpoint.IsDue(start + 60s, 64));

    checkpoint.MarkDirty(); // A final burst after the previous checkpoint.
    CHECK_FALSE(checkpoint.IsDue(start + 29s, 128));
    CHECK(checkpoint.IsDue(start + 30s, 128));
    CHECK(checkpoint.CanSave(128)); // Shutdown can flush before the timer expires.
}

TEST_CASE("Used pipeline checkpoints preserve short sessions and retry failed saves",
          "[video_core][used_pipeline_cache]") {
    const UsedPipelineCheckpoint::Clock::time_point start{};
    UsedPipelineCheckpoint checkpoint{start};
    checkpoint.MarkDirty();
    CHECK_FALSE(checkpoint.CanSave(63));
    CHECK_FALSE(checkpoint.IsDue(start + 60s, 63));
    REQUIRE(checkpoint.IsDue(start + 60s, 64));
    checkpoint.CompleteAttempt(start + 60s, false);
    CHECK(checkpoint.CanSave(64));
    CHECK_FALSE(checkpoint.IsDue(start + 74s, 64));
    CHECK(checkpoint.IsDue(start + 75s, 64));
    checkpoint.CompleteAttempt(start + 75s, true);
    CHECK_FALSE(checkpoint.CanSave(64));
}

TEST_CASE("Repeated checkpoints age old pipelines once per session and keep evicted uses",
          "[video_core][used_pipeline_cache]") {
    const UsedPipelineAges boot{{1, 0}, {2, 14}, {3, 15}, {4, 7}};
    const std::array<u64, 3> used{3, 5, 5};
    const auto first = Vulkan::MakeUsedPipelineSnapshot(boot, used, 16);
    const UsedPipelineAges expected{{1, 1}, {2, 15}, {3, 0}, {4, 8}, {5, 0}};
    CHECK(first == expected);
    CHECK(Vulkan::MakeUsedPipelineSnapshot(boot, used, 16) == first);
    const std::array<u64, 4> with_evicted{3, 5, 6, 9};
    const auto later = Vulkan::MakeUsedPipelineSnapshot(boot, with_evicted, 16);
    CHECK(later.at(1) == 1);
    CHECK(later.at(6) == 0);
    CHECK(later.at(9) == 0);
    const auto molten = Vulkan::MakeUsedPipelineSnapshot(boot, used, 8);
    CHECK_FALSE(molten.contains(2));
    CHECK_FALSE(molten.contains(4));
    CHECK(molten.at(3) == 0);
}

TEST_CASE("Used pipeline files replace existing snapshots and reject incomplete input",
          "[video_core][used_pipeline_cache]") {
    TempDir temp;
    const auto path = temp.path / "used.bin";
    const UsedPipelineAges old{{1, 2}, {3, 4}};
    const UsedPipelineAges next{{5, 0}, {7, 15}};
    REQUIRE(Vulkan::SaveUsedPipelineSnapshot(path, old));
    CHECK(Vulkan::LoadUsedPipelines(path, 16) == old);
    REQUIRE(Vulkan::SaveUsedPipelineSnapshot(path, next));
    CHECK(Vulkan::LoadUsedPipelines(path, 16) == next);
    const UsedPipelineAges young{{5, 0}};
    CHECK(Vulkan::LoadUsedPipelines(path, 8) == young);
    const std::string complete = ReadAll(path);
    for (size_t length = 0; length < complete.size(); ++length) {
        std::ofstream file{path, std::ios::binary | std::ios::trunc};
        file.write(complete.data(), static_cast<std::streamsize>(length));
        file.close();
        CHECK(Vulkan::LoadUsedPipelines(path, 16).empty());
    }
}

TEST_CASE("Failed used pipeline writes and replacements preserve the previous file",
          "[video_core][used_pipeline_cache]") {
    TempDir temp;
    const auto path = temp.path / "used.bin";
    REQUIRE(Vulkan::SaveUsedPipelineSnapshot(path, {{1, 0}, {2, 3}}));
    const std::string original = ReadAll(path);
    SECTION("partial write failure") {
        CHECK_FALSE(Vulkan::WritePipelineFileAtomically(path, [](std::ostream& file) {
            file.write("partial", 7);
            file.setstate(std::ios::badbit);
            return false;
        }));
    }
    SECTION("replacement failure") {
        CHECK_FALSE(Vulkan::WritePipelineFileAtomically(
            path,
            [](std::ostream& file) { return Vulkan::WriteUsedPipelineSnapshot(file, {{9, 0}}); },
            [](const std::filesystem::path&, const std::filesystem::path&) { return false; }));
    }
    SECTION("stream failure detected after writer returns") {
        CHECK_FALSE(Vulkan::WritePipelineFileAtomically(path, [](std::ostream& file) {
            file.write("partial", 7);
            file.setstate(std::ios::badbit);
            return true;
        }));
    }
    SECTION("opening a temporary file fails") {
        bool wrote = false;
        CHECK_FALSE(Vulkan::WritePipelineFileAtomically(path / "child", [&](std::ostream&) {
            wrote = true;
            return true;
        }));
        CHECK_FALSE(wrote);
    }
    CHECK(ReadAll(path) == original);
    CHECK(std::distance(std::filesystem::directory_iterator{temp.path},
                        std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Used pipeline checkpoints persist on a frame tick after the last insertion",
          "[video_core][used_pipeline_cache]") {
    TempDir temp;
    const auto path = temp.path / "used.bin";
    const UsedPipelineCheckpoint::Clock::time_point start{};
    UsedPipelineCheckpoint checkpoint{start};
    std::array<u64, 64> used{};
    for (size_t i = 0; i < used.size(); ++i) {
        used[i] = i;
        checkpoint.MarkDirty();
    }
    const auto tick = [&](auto now) {
        if (checkpoint.IsDue(now, used.size())) {
            const bool success = Vulkan::SaveUsedPipelineSnapshot(
                path, Vulkan::MakeUsedPipelineSnapshot({}, used, 16));
            checkpoint.CompleteAttempt(now, success);
        }
    };
    tick(start + 10s);
    CHECK_FALSE(std::filesystem::exists(path));
    tick(start + 20s);
    CHECK(Vulkan::LoadUsedPipelines(path, 16).size() == 64);
    CHECK_FALSE(checkpoint.IsDue(start + 60s, used.size()));
}

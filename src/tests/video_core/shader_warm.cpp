// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>

#include <catch2/catch_test_macros.hpp>

#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/warm_records.h"
#include "video_core/shader_warm/warmup_wait.h"

namespace {
using namespace VideoCommon::ShaderWarm;

template <typename T>
void Store(std::span<u8> data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

std::vector<u8> ComputeArchive() {
    std::vector<u8> bytes(128);
    std::memcpy(bytes.data(), "CGSB\x33\x22\x11\0", 8);
    Store<u16>(bytes, 8, 1);
    Store<u16>(bytes, 10, 5);
    Store<u64>(bytes, 16, 24);
    std::memcpy(bytes.data() + 24, "NVN \1\0\0\0", 8);
    Store<u64>(bytes, 40, 104);
    Store<u64>(bytes, 48, 16);
    Store<u64>(bytes, 56, 120);
    Store<u64>(bytes, 64, 8);
    bytes[104] = 0x42;
    bytes[127] = 0x37;
    return bytes;
}
} // namespace

TEST_CASE("Shader archive validates table and payload extents", "[video_core][shader_warm]") {
    auto bytes = ComputeArchive();
    size_t records = 0;
    const auto record = [&](const ShaderRecord& item) {
        ++records;
        CHECK(item.stage == ProgramStage::Compute);
        REQUIRE(item.code.size() == 16);
        REQUIRE(item.control.size() == 8);
        CHECK(item.code.front() == 0x42);
        CHECK(item.control.back() == 0x37);
    };
    const auto member_error = [](const std::string&) { FAIL("Unexpected member error"); };
    WalkArchive(bytes, "fixture.cgsb", nullptr, record, member_error);
    REQUIRE(records == 1);
    records = 0;
    SECTION("truncated table") {
        bytes.resize(103);
    }
    SECTION("truncated control") {
        bytes.resize(127);
    }
    SECTION("overflowing table offset") {
        Store<u64>(bytes, 16, std::numeric_limits<u64>::max() - 1);
    }
    SECTION("overflowing code size") {
        Store<u64>(bytes, 48, std::numeric_limits<u64>::max());
    }
    SECTION("invalid record count") {
        Store<u16>(bytes, 8, 17);
    }
    SECTION("unsupported stage") {
        Store<u16>(bytes, 10, 8);
    }
    REQUIRE_THROWS_AS(WalkArchive(bytes, "fixture.cgsb", nullptr, record, member_error),
                      ArchiveError);
    CHECK(records == 0);
}

TEST_CASE("Warm graphics key contains exactly its declared stage", "[video_core][shader_warm]") {
    GraphicsKey source;
    source.fill(0xA5);
    for (const u32 stage : {0U, 4U}) {
        const auto key = GraphicsEntryKey(stage, source, 0x12345678, 0x5555);
        std::array<u64, 6> hashes;
        std::memcpy(hashes.data(), key.data(), sizeof(hashes));
        CHECK(ValidGraphicsWarmStages(stage, hashes));
        CHECK_FALSE(ValidGraphicsWarmStages(stage == 0 ? 4 : 0, hashes));
        CHECK(key[200] == source[200]);
        u64 attributes;
        std::memcpy(&attributes, key.data() + AttributeTypesOffset, sizeof(attributes));
        CHECK(attributes == (stage == 0 ? 0x5555ULL : 0xA5A5A5A5A5A5A5A5ULL));
        hashes[0] = 1;
        CHECK_FALSE(ValidGraphicsWarmStages(stage, hashes));
    }
    CHECK_THROWS_AS(GraphicsEntryKey(5, source, 1, 0), std::invalid_argument);
    CHECK_THROWS_AS(GraphicsEntryKey(0, source, 0, 0), std::invalid_argument);
    CHECK_FALSE(ValidGraphicsWarmStages(0, std::array<u64, 6>{}));
    CHECK_FALSE(ValidGraphicsWarmStages(4, std::array<u64, 5>{}));
}

TEST_CASE("Warm serialization rejects incomplete code and headers", "[video_core][shader_warm]") {
    LoweredCode lowered;
    ComputeKey key{};
    CHECK_THROWS_AS(ComputeEntry(lowered, {}, key), std::invalid_argument);
    lowered.code.resize(9);
    CHECK_THROWS_AS(ComputeEntry(lowered, {}, key), std::invalid_argument);
    lowered.code.resize(8);
    std::array<u8, 80> header{};
    CHECK_THROWS_AS(GraphicsEnv(0, lowered, std::span{header}.first(79)), std::invalid_argument);
    CHECK_THROWS_AS(GraphicsEnv(5, lowered, header), std::invalid_argument);
    const auto compute = ComputeEntry(lowered, {{2, 3, 4}, 256, 16}, key);
    REQUIRE(compute.size() == 4 + 76 + 8 + 16 + 24);
    u64 hash;
    std::memcpy(&hash, key.data(), sizeof(hash));
    CHECK(hash == StageHash(lowered.code));
    CHECK(GraphicsEnv(0, lowered, header).size() == 76 + 8 + 20 * 12 + 80);
}

TEST_CASE("Warm completion marker requires every requested build", "[video_core][shader_warm]") {
    CHECK(WarmBuildComplete(true, false, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, false, 3, 2, 1));
    CHECK_FALSE(WarmBuildComplete(true, false, 3, 2, 0));
    CHECK_FALSE(WarmBuildComplete(false, false, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, true, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, false, 0, 0, 0));
}

TEST_CASE("Waiting for warmup does not cancel successful work", "[video_core][shader_warm]") {
    std::promise<void> compute_done;
    auto ready = compute_done.get_future().share();
    std::atomic_bool cancelled = false;
    std::vector<std::jthread> compute, graphics;
    compute.emplace_back([&](std::stop_token stop) {
        if (stop.stop_requested()) {
            cancelled = true;
        }
        compute_done.set_value();
    });
    graphics.emplace_back([&](std::stop_token stop) {
        ready.wait();
        if (stop.stop_requested()) {
            cancelled = true;
        }
    });
    WaitForWarmup(compute, graphics, {});
    CHECK_FALSE(cancelled.load());
    CHECK_FALSE(compute.front().joinable());
    CHECK_FALSE(graphics.front().joinable());
}

TEST_CASE("Warmup cancellation reaches both groups before joining", "[video_core][shader_warm]") {
    std::stop_source loading;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::promise<void> compute_entered, graphics_entered;
    std::promise<void> graphics_cancelled;
    auto graphics_stopped = graphics_cancelled.get_future();
    auto compute_ready = compute_entered.get_future();
    auto graphics_ready = graphics_entered.get_future();
    std::atomic_int cancelled = 0;
    std::vector<std::jthread> compute, graphics;
    const auto work = [&](std::stop_token stop, std::promise<void>& entered) {
        std::unique_lock lock{mutex};
        entered.set_value();
        cv.wait(lock, stop, [] { return false; });
        if (stop.stop_requested()) {
            ++cancelled;
        }
    };
    compute.emplace_back([&](std::stop_token stop) {
        work(stop, compute_entered);
        graphics_stopped.wait();
    });
    graphics.emplace_back([&](std::stop_token stop) {
        work(stop, graphics_entered);
        graphics_cancelled.set_value();
    });
    compute_ready.wait();
    graphics_ready.wait();
    SECTION("already cancelled") {
        loading.request_stop();
    }
    SECTION("cancel while joining") {
        CHECK_FALSE(loading.stop_requested());
    }
    auto waiter = std::async(std::launch::async,
                             [&] { WaitForWarmup(compute, graphics, loading.get_token()); });
    loading.request_stop();
    const bool completed = waiter.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    if (!completed) {
        compute.front().request_stop();
        graphics.front().request_stop();
    }
    waiter.get();
    CHECK(completed);
    CHECK(cancelled.load() == 2);
    CHECK_FALSE(compute.front().joinable());
    CHECK_FALSE(graphics.front().joinable());
}

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
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

std::vector<u8> GraphicsAllocation() {
    std::vector<u8> bytes(512);
    Store<u32>(bytes, 0, 1U << 10);
    Store<u32>(bytes, 128, 0x0000794dU);
    Store<u32>(bytes, 136, 0x03800000U);
    Store<u32>(bytes, 256, 0x12345678U);
    return bytes;
}

std::vector<u8> GraphicsControl() {
    std::vector<u8> control(108);
    Store<u32>(control, 0, 0x98761234);
    Store<u32>(control, 4, 0xc9);
    Store<u32>(control, 56, 160);
    Store<u32>(control, 64, 256);
    Store<u32>(control, 68, 512);
    return control;
}

std::vector<u8> GraphicsArchive(bool bahs) {
    constexpr size_t CodeOffset = 768;
    constexpr size_t ControlOffset = 408;
    std::vector<u8> bytes(CodeOffset + 512);
    const auto allocation = GraphicsAllocation();
    const auto control = GraphicsControl();
    std::memcpy(bytes.data() + CodeOffset, allocation.data(), allocation.size());
    std::memcpy(bytes.data() + ControlOffset, control.data(), control.size());
    if (bahs) {
        std::memcpy(bytes.data(), "BAHS", 4);
        Store<u32>(bytes, 4, 11);
        Store<u32>(bytes, 8, static_cast<u32>(bytes.size()));
        Store<u32>(bytes, 12, 5);
        Store<u32>(bytes, 20, 32);
        Store<u32>(bytes, 24, 512);
        Store<u32>(bytes, 28, CodeOffset);
        Store<u32>(bytes, 368, 148);
        Store<u32>(bytes, 372, 1);
        Store<u32>(bytes, 376, 140);
        Store<u32>(bytes, 388, 124);
        Store<u64>(bytes, 392, 0);
        Store<u32>(bytes, 400, 108);
        Store<s32>(bytes, 404, 16);
    } else {
        std::memcpy(bytes.data(), "BNSH", 4);
        Store<u32>(bytes, 8, 0x00020400);
        Store<u16>(bytes, 0xc, 0xfeff);
        Store<u32>(bytes, 0x1c, static_cast<u32>(bytes.size()));
        std::memcpy(bytes.data() + 0x60, "grsc", 4);
        Store<u16>(bytes, 0x70, 4);
        Store<u16>(bytes, 0x72, 200);
        Store<u32>(bytes, 0x7c, 1);
        Store<u64>(bytes, 0x80, 256);
        Store<u64>(bytes, 272, 320);
        Store<u64>(bytes, 328, 352);
        Store<u64>(bytes, 360, ControlOffset);
        Store<u64>(bytes, 368, CodeOffset);
        Store<u32>(bytes, 376, 512);
        Store<u32>(bytes, 380, 108);
    }
    return bytes;
}

LoweredCode IndirectTableProgram(u32 bank = 1, u32 offset = 0, u32 entries = 1) {
    std::vector<u64> words(10);
    words.push_back(0x3820000000000000ULL | (u64{entries - 1} << 20)); // IMNMX R0
    words.push_back(0x3848000000200000ULL);                            // SHL R0, R0, 2
    words.push_back(0);                                                // Scheduling word
    words.push_back(0xef94000000000001ULL | (u64{bank} << 36) |
                    (u64{offset & 0xffff} << 20));                  // LDC R1, c[bank][R0]
    words.push_back(0xe250000000000100ULL | (u64{0xffff88} << 20)); // BRX R1, -120
    words.push_back(0xe30000000000000fULL);                         // EXIT
    words.push_back(0);
    for (const u64 address : {80U, 88U, 104U, 112U, 120U}) {
        words.push_back(address);
    }
    words.push_back(5);
    words.push_back(0x50414d4e4544454eULL);
    LoweredCode result;
    result.requires_captured_indirect_table = true;
    result.code.resize(words.size() * sizeof(u64));
    std::memcpy(result.code.data(), words.data(), result.code.size());
    return result;
}
} // namespace

TEST_CASE("BNSH and BAHS preserve the control-delimited constant segment",
          "[video_core][shader_warm]") {
    for (const bool bahs : {false, true}) {
        const auto bytes = GraphicsArchive(bahs);
        size_t records = 0;
        WalkArchive(
            bytes, bahs ? "fixture.sharcb" : "fixture.bnsh", nullptr,
            [&](const ShaderRecord& record) {
                ++records;
                REQUIRE(record.code.size() == 512);
                const auto data = ReadProgramData(record.code, record.control, true);
                REQUIRE(data);
                CHECK(data->instructions.size() == 160);
                REQUIRE(data->constant_buffer1.size() == 256);
                CHECK(data->constant_buffer1.front() == 0x78);
                LoweredCode lowered;
                CHECK_FALSE(LowerProgram(data->instructions, true, lowered));
            },
            [](const std::string&) { FAIL("Unexpected member error"); });
        CHECK(records == 1);
    }
    auto bytes = GraphicsArchive(true);
    Store<u64>(bytes, 392, std::numeric_limits<u64>::max() - 32);
    CHECK_THROWS_AS(WalkArchive(
                        bytes, "overflow.sharcb", nullptr, [](const ShaderRecord&) {},
                        [](const std::string&) {}),
                    ArchiveError);
}

TEST_CASE("Native constant metadata refuses invalid extents", "[video_core][shader_warm]") {
    auto allocation = GraphicsAllocation();
    auto control = GraphicsControl();
    REQUIRE(ReadProgramData(allocation, control, true));
    SECTION("short control") {
        control.resize(71);
    }
    SECTION("unknown control version") {
        Store<u32>(control, 4, 0xca);
    }
    SECTION("invalid magic") {
        Store<u32>(control, 0, 0);
    }
    SECTION("instructions overlap constants") {
        Store<u32>(control, 64, 144);
    }
    SECTION("reversed constant extent") {
        Store<u32>(control, 68, 252);
    }
    SECTION("constant extent exceeds allocation") {
        Store<u32>(control, 68, 516);
    }
    SECTION("overflowing constant offset") {
        Store<u32>(control, 64, 0xfffffff0);
    }
    SECTION("partial instruction") {
        Store<u32>(control, 56, 159);
    }
    SECTION("partial constant word") {
        Store<u32>(control, 68, 511);
    }
    CHECK_FALSE(ReadProgramData(allocation, control, true));
}

TEST_CASE("BNSH variants retain constants independently of their native code hash",
          "[video_core][shader_warm]") {
    auto bytes = GraphicsArchive(false);
    bytes.resize(2048);
    std::fill(bytes.begin() + 256, bytes.end(), 0);
    Store<u32>(bytes, 0x1c, static_cast<u32>(bytes.size()));
    Store<u32>(bytes, 0x7c, 2);
    const auto control = GraphicsControl();
    for (size_t index = 0; index < 2; ++index) {
        const size_t program = 384 + index * 64;
        const size_t record = 512 + index * 64;
        const size_t metadata = 640 + index * 128;
        const size_t allocation = 1024 + index * 512;
        Store<u64>(bytes, 272 + index * 64, program);
        Store<u64>(bytes, program + 8, record);
        Store<u64>(bytes, record + 8, metadata);
        Store<u64>(bytes, record + 16, allocation);
        Store<u32>(bytes, record + 24, 512);
        Store<u32>(bytes, record + 28, static_cast<u32>(control.size()));
        std::memcpy(bytes.data() + metadata, control.data(), control.size());
        auto code = GraphicsAllocation();
        Store<u32>(code, 256, static_cast<u32>(index + 1));
        std::memcpy(bytes.data() + allocation, code.data(), code.size());
    }
    std::vector<u64> hashes;
    std::vector<std::vector<u8>> environments;
    WalkArchive(
        bytes, "variants.bnsh", nullptr,
        [&](const ShaderRecord& record) {
            const auto data = ReadProgramData(record.code, record.control, true);
            REQUIRE(data);
            LoweredCode lowered;
            REQUIRE_FALSE(LowerProgram(data->instructions, true, lowered));
            hashes.push_back(StageHash(lowered.code));
            environments.push_back(
                GraphicsEnv(0, lowered, data->instructions, data->constant_buffer1));
        },
        [](const std::string&) { FAIL("Unexpected member error"); });
    REQUIRE(hashes.size() == 2);
    CHECK(hashes[0] == hashes[1]);
    CHECK(environments[0] != environments[1]);
}

TEST_CASE("Constant data cannot complete an unterminated instruction capture",
          "[video_core][shader_warm]") {
    auto allocation = GraphicsAllocation();
    const auto control = GraphicsControl();
    Store<u32>(allocation, 128, 0x00007918U);
    Store<u32>(allocation, 136, 0U);
    Store<u32>(allocation, 144, 0x00007918U);
    Store<u32>(allocation, 256, 0x0000794dU);
    Store<u32>(allocation, 264, 0x03800000U);
    const auto data = ReadProgramData(allocation, control, true);
    REQUIRE(data);
    LoweredCode lowered;
    CHECK(LowerProgram(data->instructions, true, lowered));
}

TEST_CASE("Indirect tables require every value from authenticated CB1",
          "[video_core][shader_warm]") {
    const std::array<u8, 4> constants{};
    const auto valid = IndirectTableProgram();
    CHECK_FALSE(ValidateIndirectBranchTables(valid, constants));
    auto absolute = valid;
    Store<u64>(absolute.code, 112, 0xe200000000000100ULL | (u64{0xfffff8} << 20));
    CHECK_FALSE(ValidateIndirectBranchTables(absolute, constants));
    CHECK(ValidateIndirectBranchTables(valid, {}));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(0), constants));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(2), constants));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(1, 1), constants));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(1, 4), constants));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(1, 0, 2), constants));
    CHECK(ValidateIndirectBranchTables(IndirectTableProgram(1, 0xfffc), constants));
    std::array<u8, 4> invalid_target{1};
    CHECK(ValidateIndirectBranchTables(valid, invalid_target));
    auto untracked = valid;
    Store<u64>(untracked.code, 104, 0xe30000000000000fULL);
    CHECK(ValidateIndirectBranchTables(untracked, constants));
    auto second_branch = valid;
    Store<u64>(second_branch.code, 120, 0xe250000000000200ULL); // R2 has no table
    CHECK(ValidateIndirectBranchTables(second_branch, constants));

    ComputeKey first_key{}, second_key{};
    const auto first = ComputeEntry(valid, {{1, 1, 1}, 0, 0}, first_key, constants);
    std::array<u8, 4> other_target{16};
    const auto second = ComputeEntry(valid, {{1, 1, 1}, 0, 0}, second_key, other_target);
    CHECK(first_key == second_key);
    CHECK(first != second); // Equal native code can have distinct constant environments.
}

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
    CHECK(GraphicsEnv(0, lowered, header).size() == 76 + 8 + 80);
}

TEST_CASE("Warm completion marker requires every requested build", "[video_core][shader_warm]") {
    CHECK(WarmBuildComplete(true, false, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, false, 3, 2, 1));
    CHECK_FALSE(WarmBuildComplete(true, false, 3, 2, 0));
    CHECK_FALSE(WarmBuildComplete(false, false, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, true, 3, 3, 0));
    CHECK_FALSE(WarmBuildComplete(true, false, 0, 0, 0));
}

TEST_CASE("Warm generation requires captured tables for indirect branches",
          "[video_core][shader_warm]") {
    for (const bool uniform : {false, true}) {
        std::vector<u8> program(128 + 32);
        Store<u32>(program, 0, 1U << 10); // Vertex header
        Store<u32>(program, 128, uniform ? 0x00007958U : 0x00007949U);
        Store<u32>(program, 132, 0xfffffff0U);
        Store<u32>(program, 136, uniform ? 0x0b83ffffU : 0x0383ffffU);
        Store<u32>(program, 140, 0x000fc000U);
        Store<u32>(program, 144, 0x00007947U); // Capture-ending branch to itself
        Store<u32>(program, 148, 0xfffffff0U);
        Store<u32>(program, 152, 0x0383ffffU);
        Store<u32>(program, 156, 0x000fc000U);

        LoweredCode lowered;
        REQUIRE_FALSE(LowerProgram(program, true, lowered));
        REQUIRE(lowered.requires_captured_indirect_table);
        ComputeKey key{};
        CHECK_THROWS_AS(ComputeEntry(lowered, {{1, 1, 1}, 0, 0}, key), std::invalid_argument);
        CHECK_THROWS_AS(GraphicsEnv(0, lowered, program), std::invalid_argument);

        // A BRX-looking word after the capture's EXIT is allocation padding, not program code.
        Store<u32>(program, 128, 0x0000794dU);
        Store<u32>(program, 132, 0U);
        Store<u32>(program, 136, 0x03800000U);
        Store<u32>(program, 144, 0x00007949U);
        REQUIRE_FALSE(LowerProgram(program, true, lowered));
        CHECK_FALSE(lowered.requires_captured_indirect_table);
        CHECK_NOTHROW(GraphicsEnv(0, lowered, program));
    }
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

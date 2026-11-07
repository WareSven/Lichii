// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/mesh_pipeline_key.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

namespace {

constexpr std::array<char, 8> Magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr std::array<char, 8> OtherMagic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-pipeline-cache-files-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteCache(const std::filesystem::path& file, const std::array<char, 8>& magic, u32 version,
                std::string_view body) {
    std::ofstream stream(file, std::ios::binary);
    stream.write(magic.data(), magic.size());
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class SerializedEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit SerializedEnvironment(Shader::Stage shader_stage) {
        stage = shader_stage;
        code = {0x123456789abcdef0, 0xfedcba9876543210};
        cached_lowest = 0;
        cached_highest = sizeof(u64);
        mesh_registers = {0x12345678, 0x87654321, 0xabcdef01};
        std::memset(&mesh_primitive_header, 0x5a, sizeof(mesh_primitive_header));
        texture_bound = 2;
        cbuf_values.emplace((u64{1} << 32) | 16, 0x1357);
        texture_types.emplace(3, Shader::TextureType::Color2D);
    }

    void MarkNative() {
        Shader::Ampere::AppendNativeAddressMap(code, std::array<u32, 1>{0});
        cached_highest = static_cast<u32>((code.size() - 1) * sizeof(u64));
    }

    void RecordPointerClassification(u32 offset, bool is_pointer) {
        storage_pointer_types.emplace(offset, static_cast<u8>(is_pointer));
    }

    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

} // Anonymous namespace

TEST_CASE("Sparse graphics replay recaptures and persists the latest normalized environment",
          "[video_core][pipeline_cache_files][mesh_pipeline_key][storage_buffer]") {
    namespace IR = Shader::IR;
    const auto track = [](Shader::Environment& env) {
        Shader::ObjectPool<IR::Inst> pool{256};
        IR::Block entry{pool};
        IR::Program program;
        program.stage = env.ShaderStage();
        program.blocks = {&entry};
        program.post_order_blocks = {&entry};
        IR::IREmitter ir{entry};
        const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
        const IR::U32 lane{ir.LocalInvocationIdX()};
        const IR::U32 row{ir.IAdd(ir.ShiftLeftLogical(lane, ir.Imm32(3)), word(0x1c0))};
        const IR::U32 low{ir.IAdd(ir.IAdd(row, word(0x178)),
                                ir.ShiftLeftLogical(lane, ir.Imm32(4)))};
        ir.WriteGlobal32(ir.PackUint2x32(ir.CompositeConstruct(low, word(0x17c))), lane);
        Shader::HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        Shader::Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
        std::vector<u32> offsets;
        for (const auto& descriptor : program.info.storage_buffers_descriptors) {
            offsets.push_back(descriptor.cbuf_offset);
        }
        return offsets;
    };
    struct Record {
        Vulkan::GraphicsPipelineCacheKey key;
        std::vector<VideoCommon::FileEnvironment> envs;
    };
    TempDir temp;
    const auto path = temp.path / "sparse-graphics-v53.bin";
    const auto load_latest = [&] {
        Vulkan::LatestPipelineRecords<Vulkan::GraphicsPipelineCacheKey, Record> records;
        VideoCommon::LoadPipelines(
            {}, path, VideoCommon::VulkanPipelineCacheVersion,
            [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
            [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
                Vulkan::GraphicsPipelineCacheKey key;
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
                REQUIRE(envs.size() == 1);
                REQUIRE(envs.front().ShaderStage() == Shader::Stage::Mesh);
                REQUIRE(envs.front().IsNativeProgram());
                Vulkan::NormalizeMeshVertexInput(key.state,
                    Vulkan::HasMeshShaderSlots(key.unique_hashes) &&
                    Vulkan::HasOnlyMeshStages(std::array{envs.front().ShaderStage()}));
                records.Add(key, Record{key, std::move(envs)});
            });
        return records.Take();
    };
    SerializedEnvironment sparse{Shader::Stage::Mesh};
    sparse.MarkNative();
    sparse.RecordPointerClassification(0x178, true);
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[1] = 11;
    key.state.dynamic_vertex_input.Assign(1);
    key.state.attribute_types = 0x155;
    VideoCommon::SerializePipeline(key, std::array<const VideoCommon::GenericEnvironment*, 1>{&sparse},
                                   path, VideoCommon::VulkanPipelineCacheVersion);
    const auto old_file = ReadAll(path);
    auto first = load_latest();
    REQUIRE(first.size() == 1);
    const u64 hash = first.front().key.Hash();
    std::unordered_set<u64> disk;
    Vulkan::GraphicsPipelineLoadPlan plan;
    REQUIRE(plan.Add(disk, hash, true) == Vulkan::GraphicsPipelineLoadAction::Preload);
    // The old paired-pointer path never asked whether this scalar was mapped memory.
    REQUIRE_THROWS_WITH(track(first.front().envs.front()),
                        "Uncached storage pointer classification c0[0x1c0]");
    plan.ForgetFailed(disk, hash);
    REQUIRE_FALSE(disk.contains(hash));
    CHECK(ReadAll(path) == old_file);

    SerializedEnvironment recaptured{Shader::Stage::Mesh};
    recaptured.MarkNative();
    recaptured.RecordPointerClassification(0x178, true);
    recaptured.RecordPointerClassification(0x1c0, false);
    REQUIRE(track(recaptured) == std::vector<u32>{0x178});
    key.state.attribute_types = 0x105; // A different legacy alias, same normalized pipeline.
    if (disk.insert(hash).second) {
        VideoCommon::SerializePipeline(
            key, std::array<const VideoCommon::GenericEnvironment*, 1>{&recaptured}, path,
            VideoCommon::VulkanPipelineCacheVersion);
    }
    const auto repaired_file = ReadAll(path);
    REQUIRE(repaired_file.size() > old_file.size());
    CHECK(repaired_file.starts_with(old_file));
    auto second = load_latest();
    REQUIRE(second.size() == 1);
    CHECK(second.front().key.Hash() == hash);
    CHECK(track(second.front().envs.front()) == std::vector<u32>{0x178});
    CHECK_FALSE(second.front().envs.front().IsCbufStorageBufferPointer(0, 0x1c0));
    REQUIRE_THROWS_AS(second.front().envs.front().IsCbufStorageBufferPointer(0, 0x200),
                      Shader::LogicError);
    CHECK(ReadAll(path) == repaired_file);
}

TEST_CASE("Compute replay keeps the last serialized record per full key without merging answers",
          "[video_core][pipeline_cache_files][storage_buffer]") {
    struct Record {
        Vulkan::ComputePipelineCacheKey key;
        VideoCommon::FileEnvironment env;
    };
    TempDir temp;
    const auto path = temp.path / "compute-recapture-v53.bin";
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0x1234, .shared_memory_size = 1024, .workgroup_size = {32, 1, 1}};
    auto different_group = key;
    different_group.workgroup_size[0] = 64;
    auto different_shared = key;
    different_shared.shared_memory_size = 2048;
    SerializedEnvironment sparse{Shader::Stage::Compute};
    sparse.MarkNative();
    sparse.RecordPointerClassification(0x178, true);
    sparse.RecordPointerClassification(0x200, true);
    const std::array<const VideoCommon::GenericEnvironment*, 1> old_env{&sparse};
    for (const auto& distinct : {key, different_group, different_shared}) {
        VideoCommon::SerializePipeline(distinct, old_env, path,
                                       VideoCommon::VulkanPipelineCacheVersion);
    }
    const auto old_file = ReadAll(path);
    SerializedEnvironment recaptured{Shader::Stage::Compute};
    recaptured.MarkNative();
    recaptured.RecordPointerClassification(0x178, true);
    recaptured.RecordPointerClassification(0x1c0, false);
    VideoCommon::SerializePipeline(
        key, std::array<const VideoCommon::GenericEnvironment*, 1>{&recaptured}, path,
        VideoCommon::VulkanPipelineCacheVersion);
    const auto appended_file = ReadAll(path);
    REQUIRE(appended_file.starts_with(old_file));
    REQUIRE(appended_file.size() > old_file.size());
    Vulkan::LatestPipelineRecords<Vulkan::ComputePipelineCacheKey, Record> latest;
    size_t read{};
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey restored;
            file.read(reinterpret_cast<char*>(&restored), sizeof(restored));
            if (++read <= 3) {
                REQUIRE_THROWS_AS(env.IsCbufStorageBufferPointer(0, 0x1c0), Shader::LogicError);
            }
            latest.Add(restored, Record{restored, std::move(env)});
        },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Unexpected graphics record");
        });
    CHECK(read == 4);
    auto records = latest.Take();
    REQUIRE(records.size() == 3);
    size_t refreshed{};
    for (auto& record : records) {
        CHECK(record.env.IsCbufStorageBufferPointer(0, 0x178));
        if (record.key == key) {
            ++refreshed;
            CHECK_FALSE(record.env.IsCbufStorageBufferPointer(0, 0x1c0));
            REQUIRE_THROWS_AS(record.env.IsCbufStorageBufferPointer(0, 0x200), Shader::LogicError);
        } else {
            CHECK(record.env.IsCbufStorageBufferPointer(0, 0x200));
            REQUIRE_THROWS_AS(record.env.IsCbufStorageBufferPointer(0, 0x1c0), Shader::LogicError);
        }
    }
    CHECK(refreshed == 1);
    CHECK(latest.Take().empty());
    CHECK(ReadAll(path) == appended_file);
}

TEST_CASE("Task and mesh disk environments roundtrip all translation state and record boundaries",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "mesh.bin";
    constexpr u64 key = 0xabcdef;
    for (const auto stage : {Shader::Stage::Task, Shader::Stage::Mesh, Shader::Stage::Fragment}) {
        SerializedEnvironment source{stage};
        const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
        VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    }
    size_t read = 0;
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            const std::array stages{Shader::Stage::Task, Shader::Stage::Mesh,
                                    Shader::Stage::Fragment};
            REQUIRE(read < stages.size());
            REQUIRE(envs.size() == 1);
            auto& restored = envs.front();
            SerializedEnvironment source{stages[read++]};
            REQUIRE(restored.ShaderStage() == source.ShaderStage());
            REQUIRE(restored.ReadInstruction(0) == 0x123456789abcdef0);
            REQUIRE(restored.ReadInstruction(8) == 0xfedcba9876543210);
            REQUIRE(restored.ReadCbufValue(1, 16) == 0x1357);
            REQUIRE(restored.ReadTextureType(3) == Shader::TextureType::Color2D);
            REQUIRE(restored.TextureBoundBuffer() == 2);
            REQUIRE(restored.MemorySize() > sizeof(restored) + 2 * sizeof(u64));
            if (source.ShaderStage() != Shader::Stage::Fragment) {
                REQUIRE(restored.MeshRegisters() == source.MeshRegisters());
                REQUIRE(std::memcmp(&restored.MeshPrimitiveHeader(), &source.MeshPrimitiveHeader(),
                                    sizeof(Shader::ProgramHeader)) == 0);
            }
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
        });
    REQUIRE(read == 3);
}

TEST_CASE("Compatible v52 pipelines migrate without changing or discarding the old warm cache",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    SerializedEnvironment source{Shader::Stage::Fragment};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, old, 52);
    const auto before = ReadAll(old);
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    bool loaded = false;
    VideoCommon::LoadPipelines(
        {}, current, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> restored) {
            REQUIRE(restored.size() == 1);
            REQUIRE(restored.front().ShaderStage() == Shader::Stage::Fragment);
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
            loaded = true;
        });
    REQUIRE(loaded);
    const auto other = temp.path / "wrong.bin";
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(old, other, Magic, 51, 53));
    REQUIRE_FALSE(std::filesystem::exists(other));
}

TEST_CASE("Compatible pipeline cache migration retries after an interrupted temporary copy",
          "[pipeline_cache_files][mesh_serialization][migration_retry]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    auto temporary = current;
    temporary += ".migrating";
    WriteCache(old, Magic, 52, "complete pipeline records");
    const auto before = ReadAll(old);
    {
        std::ofstream stream(temporary, std::ios::binary);
        stream.write(before.data(), 4);
    }
    REQUIRE(ReadAll(temporary) == before.substr(0, 4));
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(
        current, Magic, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    const auto migrated = ReadAll(current);
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current) == migrated);
    REQUIRE(ReadAll(old) == before);
}

TEST_CASE("Legacy v53 mesh keys normalize in memory without rewriting their cache",
          "[video_core][pipeline_cache_files][mesh_pipeline_key]") {
    TempDir temp;
    const auto path = temp.path / "mesh-v53.bin";
    SerializedEnvironment task{Shader::Stage::Task};
    SerializedEnvironment mesh{Shader::Stage::Mesh};
    SerializedEnvironment fragment{Shader::Stage::Fragment};
    const std::array<const VideoCommon::GenericEnvironment*, 3> envs{&task, &mesh, &fragment};
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes = {0, 11, 0, 22, 0, 33};
    key.state.dynamic_vertex_input.Assign(1);
    key.state.attribute_types = 0x155;
    const auto old_hash = key.Hash();
    VideoCommon::SerializePipeline(key, envs, path, 53);
    const auto before = ReadAll(path);
    std::unordered_map<u64, u8> used{{old_hash, 3}};
    size_t loaded = 0;
    VideoCommon::LoadPipelines(
        {}, path, 53,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> restored) {
            Vulkan::GraphicsPipelineCacheKey cached;
            file.read(reinterpret_cast<char*>(&cached), sizeof(cached));
            REQUIRE(cached.Hash() == old_hash);
            REQUIRE(restored.size() == 3);
            const std::array stages{restored[0].ShaderStage(), restored[1].ShaderStage(),
                                    restored[2].ShaderStage()};
            Vulkan::NormalizeMeshVertexInput(cached.state,
                                             Vulkan::HasMeshShaderSlots(cached.unique_hashes) &&
                                                 Vulkan::HasOnlyMeshStages(stages));
            Vulkan::MigrateUsedPipelineHash(used, old_hash, cached.Hash());
            CHECK(cached.state.attribute_types == 0);
            CHECK(used.at(cached.Hash()) == 3);
            CHECK_FALSE(used.contains(old_hash));
            ++loaded;
        });
    CHECK(loaded == 1);
    CHECK(ReadAll(path) == before);
}

TEST_CASE("Truncated mesh state is rejected before a pipeline can be queued",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "truncated-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - sizeof(key) - 1);
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Incomplete mesh state reached pipeline creation");
        });
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("Cancelled mesh loading preserves its disk cache and queues no work",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "cancelled-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion);
    const auto before = ReadAll(path);
    std::stop_source stop;
    stop.request_stop();
    VideoCommon::LoadPipelines(
        stop.get_token(), path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Cancelled load reached pipeline creation");
        });
    REQUIRE(ReadAll(path) == before);
}

TEST_CASE("Pipeline cache names are version-suffixed per build", "[pipeline_cache_files]") {
    const std::filesystem::path dir{"shader"};
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39).filename() ==
          "vulkan.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan_pipelines", 39).filename() ==
          "vulkan_pipelines.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 18) !=
          VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39));
}

TEST_CASE("A legacy cache of this version is adopted with its contents",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "pipelines");
    const std::string before = ReadAll(legacy);
    REQUIRE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(ReadAll(versioned) == before);
    // Idempotent: nothing left to adopt.
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
}

TEST_CASE("Another build's legacy cache is never touched", "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 18, "official");
    const std::string before = ReadAll(legacy);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(legacy) == before);
    CHECK_FALSE(std::filesystem::exists(versioned));

    WriteCache(legacy, OtherMagic, 39, "wrong magic");
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));

    // Truncated header.
    {
        std::ofstream stream(legacy, std::ios::binary | std::ios::trunc);
        stream.write(Magic.data(), 4);
    }
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));
}

TEST_CASE("An existing versioned cache is never overwritten by a legacy one",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "older");
    WriteCache(versioned, Magic, 39, "current");
    const std::string current = ReadAll(versioned);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(versioned) == current);
    CHECK(std::filesystem::exists(legacy));
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(temp.path / "missing.bin", versioned,
                                                      Magic, 39));
}

TEST_CASE("Cached compute failures retain their cause and the current dispatch address",
          "[pipeline_cache][diagnostics]") {
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0x1234,
        .shared_memory_size = 1024,
        .workgroup_size = {32, 1, 1},
    };
    std::unordered_map<Vulkan::ComputePipelineCacheKey, Vulkan::ComputePipelineCacheEntry> cache;
    const std::string cause =
        "SPIR-V emission failed at IR opcode UndefU8: SPIR-V Instruction is not implemented";
    cache[key].failure = cause;
    for (const GPUVAddr address : {0x211ddd9000ULL, 0x21ebdfa000ULL}) {
        const auto [entry, inserted] = cache.try_emplace(key);
        REQUIRE_FALSE(inserted);
        CHECK(entry->second.Get(false, address, key) == nullptr);
        try {
            static_cast<void>(entry->second.Get(true, address, key));
            FAIL("Native compute failure was not propagated");
        } catch (const Shader::RuntimeError& exception) {
            const std::string message{exception.what()};
            CHECK(message.find(cause) != std::string::npos);
            CHECK(message.find(fmt::format("address={:#x}", address)) != std::string::npos);
            CHECK(message.find(fmt::format("shader={:#016x}", key.unique_hash)) != std::string::npos);
            CHECK(message.find(fmt::format("pipeline={:#016x}", key.Hash())) != std::string::npos);
        }
        CHECK(entry->second.failure == cause);
        CHECK_FALSE(entry->second.pipeline);
    }
    auto other = key;
    other.workgroup_size[0] = 64;
    const auto [entry, inserted] = cache.try_emplace(other);
    REQUIRE(inserted);
    CHECK(entry->second.failure.empty());
    CHECK(entry->second.Get(false, 0x2120633c00, other) == nullptr);
    REQUIRE_THROWS_AS(entry->second.Get(true, 0x2120633c00, other), Shader::RuntimeError);
}

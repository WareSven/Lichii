// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "video_core/renderer_vulkan/used_pipeline_cache.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Vulkan {
namespace {
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"

std::filesystem::path TemporaryPath(const std::filesystem::path& path) {
    static std::atomic<u64> sequence{};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    auto temporary = path;
    temporary += ".tmp." + std::to_string(pid) + "." +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

bool ReplacePipelineFile(const std::filesystem::path& temporary,
                         const std::filesystem::path& path) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}
} // namespace

UsedPipelineAges MakeUsedPipelineSnapshot(const UsedPipelineAges& boot, std::span<const u64> used,
                                          u8 max_age) {
    UsedPipelineAges snapshot;
    snapshot.reserve(boot.size() + used.size());
    for (const u64 hash : used) {
        snapshot.emplace(hash, u8{0});
    }
    // Always age the boot snapshot, not the last checkpoint in this session.
    for (const auto& [hash, age] : boot) {
        if (age + 1 < max_age) {
            snapshot.try_emplace(hash, static_cast<u8>(age + 1));
        }
    }
    return snapshot;
}

UsedPipelineAges LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    const auto size = file.tellg();
    file.seekg(0);
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count)) ||
        size != static_cast<std::streamoff>(sizeof(magic) + sizeof(count) +
                                            u64{count} * (sizeof(u64) + sizeof(u8)))) {
        return {};
    }
    UsedPipelineAges used;
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            return {};
        }
        if (age < max_age) {
            used.emplace(hash, age);
        }
    }
    return used;
}

bool WriteUsedPipelineSnapshot(std::ostream& stream, const UsedPipelineAges& snapshot) {
    if (snapshot.size() > (std::numeric_limits<u32>::max)()) {
        return false;
    }
    const u32 count = static_cast<u32>(snapshot.size());
    stream.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
    stream.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [hash, age] : snapshot) {
        stream.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        stream.write(reinterpret_cast<const char*>(&age), sizeof(age));
    }
    return stream.good();
}

bool WritePipelineFileAtomically(const std::filesystem::path& path,
                                 const std::function<bool(std::ostream&)>& write,
                                 PipelineFileReplace replace) {
    const auto temporary = TemporaryPath(path);
    std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
    bool success = file.is_open() && write(file);
    file.close();
    success = success && !file.fail();
    if (success) {
        success = (replace ? replace : ReplacePipelineFile)(temporary, path);
    }
    if (!success) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    }
    return success;
}

bool SaveUsedPipelineSnapshot(const std::filesystem::path& path, const UsedPipelineAges& snapshot) {
    return WritePipelineFileAtomically(
        path, [&](std::ostream& stream) { return WriteUsedPipelineSnapshot(stream, snapshot); });
}

} // namespace Vulkan
